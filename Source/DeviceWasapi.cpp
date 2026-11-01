/*
    NovaAudioEngine — périphérique WASAPI (partagé ou exclusif), piloté par l'événement de rendu.

    - Sortie : client de rendu en mode événement ; chaque événement = un bloc (période du moteur).
    - Entrée : client de capture lu dans le même fil ; ses paquets passent par une petite file
      (les deux horloges d'un même périphérique sont les mêmes ; pour deux périphériques différents,
      la file absorbe la dérive : trop pleine on jette le plus vieux, vide on donne du silence, et
      c'est compté dans xruns).
    - Partagé : format du mélangeur de Windows (fréquence imposée : le moteur rééchantillonne).
      Exclusif : float 32, puis entier 32 / 24 / 16 bits à la fréquence demandée.

    NON testé sur du matériel dans la phase 1 (aucun son sur les haut-parleurs du PC de dev) : à
    valider au studio. ASIO reste le chemin visé pour la cabine.

    Copyright (C) 2026 Make Music.
    SPDX-License-Identifier: GPL-3.0-only OR LicenseRef-MakeMusic-NovaPro
    (Make Music, seul auteur, publie ce fichier sous GPLv3 dans les éditions asio / libre du moteur et
    sous sa propre licence dans l'édition « Nova Pro Audio » : native-audio/pro/LICENCE.md.)
    Ce fichier fait partie de NovaAudioEngine, logiciel libre : vous pouvez le redistribuer et/ou le
    modifier selon les termes de la GNU General Public License version 3, publiée par la Free Software
    Foundation, avec la permission supplémentaire (article 7) décrite dans native-audio/LICENCES.md
    (pilotes audio et modules d'effets tiers chargés à l'exécution). Il est distribué SANS AUCUNE
    GARANTIE, sans même la garantie implicite de qualité marchande ou d'adéquation à un usage
    particulier. Texte de la licence : native-audio/LICENSE (https://www.gnu.org/licenses/gpl-3.0.html).
*/
#include "Device.h"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>

namespace nova::audio
{
    using json::Value;

    namespace
    {
        template <typename T> void release (T*& p) { if (p) { p->Release(); p = nullptr; } }

        struct Fmt
        {
            bool isFloat = false;
            int bits = 32, container = 32, channels = 2, rate = 48000;
        };

        Fmt describe (const WAVEFORMATEX* wf)
        {
            Fmt f;
            f.channels = wf->nChannels;
            f.rate = (int) wf->nSamplesPerSec;
            f.container = wf->wBitsPerSample;
            f.bits = wf->wBitsPerSample;
            f.isFloat = wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
            if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
            {
                auto* x = (const WAVEFORMATEXTENSIBLE*) wf;
                f.isFloat = x->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
                f.bits = x->Samples.wValidBitsPerSample ? x->Samples.wValidBitsPerSample : wf->wBitsPerSample;
            }
            return f;
        }

        WAVEFORMATEXTENSIBLE makeFormat (int channels, int rate, bool isFloat, int container, int valid)
        {
            WAVEFORMATEXTENSIBLE x {};
            x.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
            x.Format.nChannels = (WORD) channels;
            x.Format.nSamplesPerSec = (DWORD) rate;
            x.Format.wBitsPerSample = (WORD) container;
            x.Format.nBlockAlign = (WORD) (channels * container / 8);
            x.Format.nAvgBytesPerSec = (DWORD) rate * x.Format.nBlockAlign;
            x.Format.cbSize = 22;
            x.Samples.wValidBitsPerSample = (WORD) valid;
            x.dwChannelMask = channels == 2 ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT) : (channels == 1 ? SPEAKER_FRONT_CENTER : 0);
            x.SubFormat = isFloat ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
            return x;
        }

        void toFloat (const uint8_t* src, const Fmt& f, float* dst, size_t n)
        {
            const int b = f.container / 8;
            for (size_t i = 0; i < n; ++i, src += b)
            {
                if (f.isFloat) std::memcpy (&dst[i], src, 4);
                else if (f.container == 16) { int16_t v; std::memcpy (&v, src, 2); dst[i] = (float) v / 32768.0f; }
                else if (f.container == 24) { const int32_t v = (int32_t) ((uint32_t) src[0] << 8 | (uint32_t) src[1] << 16 | (uint32_t) src[2] << 24) >> 8; dst[i] = (float) v / 8388608.0f; }
                else { int32_t v; std::memcpy (&v, src, 4); dst[i] = (float) ((double) v / 2147483648.0); }
            }
        }

        void fromFloat (const float* src, const Fmt& f, uint8_t* dst, size_t n)
        {
            const int b = f.container / 8;
            for (size_t i = 0; i < n; ++i, dst += b)
            {
                const float x = std::max (-1.0f, std::min (1.0f, src[i]));
                if (f.isFloat) std::memcpy (dst, &src[i], 4);
                else if (f.container == 16) { const int16_t v = (int16_t) std::lrint (std::min (32767.0f, x * 32768.0f)); std::memcpy (dst, &v, 2); }
                else if (f.container == 24) { const int32_t v = (int32_t) std::lrint (std::min (8388607.0f, x * 8388608.0f)); dst[0] = (uint8_t) v; dst[1] = (uint8_t) (v >> 8); dst[2] = (uint8_t) (v >> 16); }
                else { const int32_t v = (int32_t) std::llrint (std::min (2147483647.0, (double) x * 2147483648.0)); std::memcpy (dst, &v, 4); }
            }
        }

        std::string endpointName (IMMDevice* d)
        {
            IPropertyStore* ps = nullptr;
            std::string name;
            if (SUCCEEDED (d->OpenPropertyStore (STGM_READ, &ps)))
            {
                PROPVARIANT v;
                PropVariantInit (&v);
                if (SUCCEEDED (ps->GetValue (PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) name = narrow (v.pwszVal);
                PropVariantClear (&v);
                ps->Release();
            }
            return name;
        }
    }

    class WasapiDevice : public AudioDevice
    {
    public:
        explicit WasapiDevice (const DeviceSpec& s) : spec (s) {}
        ~WasapiDevice() override { close(); }

        const DeviceInfo& info() const override { return inf; }

        bool open (const DeviceRequest& r, std::string& err) override
        {
            CoInitializeEx (nullptr, COINIT_MULTITHREADED);
            exclusive = spec.opt ("exclusive") == "1";
            if (FAILED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof (IMMDeviceEnumerator), (void**) &enumr)))
            { err = "WASAPI indisponible"; return false; }
            const auto id = spec.name.empty() ? spec.opt ("id") : spec.name;
            HRESULT hr = id.empty() ? enumr->GetDefaultAudioEndpoint (eRender, eConsole, &renderDev) : enumr->GetDevice (widen (id).c_str(), &renderDev);
            if (FAILED (hr)) { err = "sortie WASAPI introuvable"; return false; }
            const auto inId = spec.opt ("in");
            if (r.inChannels != 0 || ! inId.empty())
            {
                if (inId.empty()) enumr->GetDefaultAudioEndpoint (eCapture, eConsole, &captureDev);
                else enumr->GetDevice (widen (inId).c_str(), &captureDev);
            }
            inf.type = "wasapi";
            inf.name = endpointName (renderDev) + (exclusive ? " (exclusif)" : " (partagé)");
            if (! initClient (renderDev, false, r, renderClient, renderFmt, renderFrames, err)) return false;
            if (FAILED (renderClient->GetService (__uuidof (IAudioRenderClient), (void**) &render))) { err = "rendu WASAPI impossible"; return false; }
            if (captureDev != nullptr)
            {
                std::string e2;
                if (initClient (captureDev, true, r, captureClient, captureFmt, captureFrames, e2))
                    captureClient->GetService (__uuidof (IAudioCaptureClient), (void**) &capture);
            }
            inf.sampleRate = renderFmt.rate;
            inf.outChannels = r.outChannels > 0 ? std::min (r.outChannels, renderFmt.channels) : renderFmt.channels;
            inf.inChannels = capture ? (r.inChannels > 0 ? std::min (r.inChannels, captureFmt.channels) : captureFmt.channels) : 0;
            inf.bufferFrames = (int) period;
            inf.format = std::string (renderFmt.isFloat ? "float" : "int") + std::to_string (renderFmt.bits);
            REFERENCE_TIME lat = 0;
            renderClient->GetStreamLatency (&lat);
            inf.outputLatency = (int) period + (int) std::lround ((double) lat * 1e-7 * inf.sampleRate);
            inf.inputLatency = (int) period;
            inf.minBuffer = inf.maxBuffer = inf.preferredBuffer = (int) period;
            inf.rates = { (double) renderFmt.rate };
            for (int c = 0; c < inf.inChannels; ++c) inf.inNames.push_back ("Entrée " + std::to_string (c + 1));
            for (int c = 0; c < inf.outChannels; ++c) inf.outNames.push_back ("Sortie " + std::to_string (c + 1));
            const int n = inf.bufferFrames;
            inData.assign ((size_t) std::max (1, inf.inChannels) * n, 0.0f);
            outData.assign ((size_t) std::max (1, inf.outChannels) * n, 0.0f);
            inPtr.resize ((size_t) std::max (1, inf.inChannels));
            outPtr.resize ((size_t) std::max (1, inf.outChannels));
            for (int c = 0; c < inf.inChannels; ++c) inPtr[(size_t) c] = &inData[(size_t) c * n];
            for (int c = 0; c < inf.outChannels; ++c) outPtr[(size_t) c] = &outData[(size_t) c * n];
            fifo.assign ((size_t) std::max (1, captureFmt.channels) * (size_t) n * 16, 0.0f);
            scratch.assign ((size_t) std::max (renderFmt.channels, captureFmt.channels) * (size_t) n * 8, 0.0f);
            return true;
        }

        bool start (AudioCallback* c, std::string& err) override
        {
            cb = c;
            quit = false;
            if (FAILED (renderClient->SetEventHandle (event))) { err = "événement WASAPI refusé"; return false; }
            th = std::thread ([this] { run(); });
            return true;
        }

        void stop() override
        {
            quit = true;
            if (event) SetEvent (event);
            if (th.joinable()) th.join();
        }

        void close() override
        {
            stop();
            release (render); release (capture);
            release (renderClient); release (captureClient);
            release (renderDev); release (captureDev); release (enumr);
            if (event) { CloseHandle (event); event = nullptr; }
        }

    private:
        bool initClient (IMMDevice* dev, bool isCapture, const DeviceRequest& r, IAudioClient*& client, Fmt& fmt, UINT32& frames, std::string& err)
        {
            if (FAILED (dev->Activate (__uuidof (IAudioClient), CLSCTX_ALL, nullptr, (void**) &client))) { err = "client WASAPI impossible"; return false; }
            WAVEFORMATEX* mix = nullptr;
            client->GetMixFormat (&mix);
            const int ch = mix ? mix->nChannels : 2;
            HRESULT hr = E_FAIL;
            const double rate = r.sampleRate > 0 ? r.sampleRate : (mix ? mix->nSamplesPerSec : 48000);
            if (exclusive)
            {
                const struct { bool f; int c, v; } tries[] = { { true, 32, 32 }, { false, 32, 32 }, { false, 32, 24 }, { false, 24, 24 }, { false, 16, 16 } };
                for (auto t : tries)
                {
                    auto x = makeFormat (ch, (int) rate, t.f, t.c, t.v);
                    if (client->IsFormatSupported (AUDCLNT_SHAREMODE_EXCLUSIVE, &x.Format, nullptr) != S_OK) continue;
                    const REFERENCE_TIME per = (REFERENCE_TIME) std::llround ((double) r.bufferFrames / rate * 1e7);
                    hr = client->Initialize (AUDCLNT_SHAREMODE_EXCLUSIVE, isCapture ? 0 : AUDCLNT_STREAMFLAGS_EVENTCALLBACK, per, per, &x.Format, nullptr);
                    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED)
                    {
                        UINT32 aligned = 0;
                        client->GetBufferSize (&aligned);
                        client->Release();
                        client = nullptr;
                        dev->Activate (__uuidof (IAudioClient), CLSCTX_ALL, nullptr, (void**) &client);
                        const REFERENCE_TIME per2 = (REFERENCE_TIME) std::llround ((double) aligned / rate * 1e7);
                        hr = client->Initialize (AUDCLNT_SHAREMODE_EXCLUSIVE, isCapture ? 0 : AUDCLNT_STREAMFLAGS_EVENTCALLBACK, per2, per2, &x.Format, nullptr);
                    }
                    if (SUCCEEDED (hr)) { fmt = describe (&x.Format); break; }
                }
            }
            else if (mix != nullptr)
            {
                hr = client->Initialize (AUDCLNT_SHAREMODE_SHARED, isCapture ? 0 : AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, mix, nullptr);
                if (SUCCEEDED (hr)) fmt = describe (mix);
            }
            if (mix) CoTaskMemFree (mix);
            if (FAILED (hr)) { err = "format WASAPI refusé"; return false; }
            client->GetBufferSize (&frames);
            if (! isCapture)
            {
                REFERENCE_TIME def = 0, minp = 0;
                client->GetDevicePeriod (&def, &minp);
                period = exclusive ? frames : (UINT32) std::lround ((double) def * 1e-7 * fmt.rate);
                period = std::max<UINT32> (16, std::min (period, frames));
                if (event == nullptr) event = CreateEventW (nullptr, FALSE, FALSE, nullptr);
            }
            return true;
        }

        void run()
        {
            CoInitializeEx (nullptr, COINIT_MULTITHREADED);
            enterProAudioThread();
            // Pré-remplissage en silence (évite un craquement au départ).
            BYTE* p = nullptr;
            if (SUCCEEDED (render->GetBuffer (renderFrames, &p))) render->ReleaseBuffer (renderFrames, AUDCLNT_BUFFERFLAGS_SILENT);
            renderClient->Start();
            if (captureClient) captureClient->Start();
            int64_t frame = 0;
            while (! quit)
            {
                if (WaitForSingleObject (event, 2000) != WAIT_OBJECT_0) { events.fetch_or (kEvError); break; }
                if (quit) break;
                pullCapture();
                UINT32 pad = 0;
                if (! exclusive) renderClient->GetCurrentPadding (&pad);
                const UINT32 room = exclusive ? renderFrames : renderFrames - pad;
                UINT32 n = std::min (room, period);
                if (n == 0) continue;
                takeInput ((int) n);
                std::fill (outData.begin(), outData.end(), 0.0f);
                BlockTime bt;
                bt.deviceFrame = frame;
                bt.hostMs = nowMs();
                cb->audioBlock (inPtr.data(), inf.inChannels, outPtr.data(), inf.outChannels, (int) n, bt);
                if (SUCCEEDED (render->GetBuffer (n, &p)))
                {
                    float* tmp = scratch.data();
                    for (UINT32 i = 0; i < n; ++i)
                        for (int c = 0; c < renderFmt.channels; ++c)
                            tmp[(size_t) i * renderFmt.channels + c] = c < inf.outChannels ? outPtr[(size_t) c][i] : 0.0f;
                    fromFloat (tmp, renderFmt, p, (size_t) n * renderFmt.channels);
                    render->ReleaseBuffer (n, 0);
                }
                frame += n;
            }
            renderClient->Stop();
            if (captureClient) captureClient->Stop();
            leaveProAudioThread();
            CoUninitialize();
        }

        void pullCapture()
        {
            if (! capture) return;
            const int ch = captureFmt.channels;
            const size_t capFrames = fifo.size() / (size_t) ch;
            for (;;)
            {
                UINT32 next = 0;
                if (FAILED (capture->GetNextPacketSize (&next)) || next == 0) break;
                BYTE* data = nullptr;
                UINT32 got = 0;
                DWORD flags = 0;
                if (FAILED (capture->GetBuffer (&data, &got, &flags, nullptr, nullptr))) break;
                if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) xruns.fetch_add (1);
                for (UINT32 i = 0; i < got; ++i)
                {
                    if (fifoCount >= capFrames) { fifoRead = (fifoRead + 1) % capFrames; --fifoCount; xruns.fetch_add (1); }
                    float* dst = &fifo[((fifoRead + fifoCount) % capFrames) * (size_t) ch];
                    if (flags & AUDCLNT_BUFFERFLAGS_SILENT) std::fill (dst, dst + ch, 0.0f);
                    else toFloat (data + (size_t) i * ch * (captureFmt.container / 8), captureFmt, dst, (size_t) ch);
                    ++fifoCount;
                }
                capture->ReleaseBuffer (got);
            }
        }

        void takeInput (int n)
        {
            if (inf.inChannels == 0) return;
            const int ch = captureFmt.channels;
            const size_t capFrames = fifo.size() / (size_t) ch;
            if (fifoCount < (size_t) n) xruns.fetch_add (1);
            for (int i = 0; i < n; ++i)
            {
                if (fifoCount == 0) { for (int c = 0; c < inf.inChannels; ++c) inPtr[(size_t) c][i] = 0.0f; continue; }
                const float* src = &fifo[fifoRead * (size_t) ch];
                for (int c = 0; c < inf.inChannels; ++c) inPtr[(size_t) c][i] = src[c];
                fifoRead = (fifoRead + 1) % capFrames;
                --fifoCount;
            }
        }

        DeviceSpec spec;
        DeviceInfo inf;
        bool exclusive = false;
        IMMDeviceEnumerator* enumr = nullptr;
        IMMDevice* renderDev = nullptr;
        IMMDevice* captureDev = nullptr;
        IAudioClient* renderClient = nullptr;
        IAudioClient* captureClient = nullptr;
        IAudioRenderClient* render = nullptr;
        IAudioCaptureClient* capture = nullptr;
        Fmt renderFmt, captureFmt;
        UINT32 renderFrames = 0, captureFrames = 0, period = 0;
        HANDLE event = nullptr;
        AudioCallback* cb = nullptr;
        std::thread th;
        std::atomic<bool> quit { false };
        std::vector<float> inData, outData, fifo, scratch;
        std::vector<float*> inPtr, outPtr;
        size_t fifoRead = 0, fifoCount = 0;
    };

    std::unique_ptr<AudioDevice> createWasapiDevice (const DeviceSpec& s) { return std::make_unique<WasapiDevice> (s); }

    Value listWasapiEndpoints()
    {
        // Inventaire seulement (aucun flux ouvert, aucun son).
        auto out = Value::array();
        CoInitializeEx (nullptr, COINIT_MULTITHREADED);
        IMMDeviceEnumerator* e = nullptr;
        if (FAILED (CoCreateInstance (__uuidof (MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof (IMMDeviceEnumerator), (void**) &e))) return out;
        for (EDataFlow flow : { eRender, eCapture })
        {
            IMMDeviceCollection* col = nullptr;
            if (FAILED (e->EnumAudioEndpoints (flow, DEVICE_STATE_ACTIVE, &col))) continue;
            UINT n = 0;
            col->GetCount (&n);
            for (UINT i = 0; i < n; ++i)
            {
                IMMDevice* d = nullptr;
                if (FAILED (col->Item (i, &d))) continue;
                LPWSTR id = nullptr;
                d->GetId (&id);
                auto o = Value::object();
                o.set ("id", id ? narrow (id) : std::string());
                o.set ("name", endpointName (d));
                o.set ("flow", flow == eRender ? "sortie" : "entrée");
                out.push (o);
                if (id) CoTaskMemFree (id);
                d->Release();
            }
            col->Release();
        }
        e->Release();
        return out;
    }
}
