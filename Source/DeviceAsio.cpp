/*
    NovaAudioEngine — périphérique ASIO (SDK ASIO 2.3.4 de Steinberg).

    LICENCE : ce fichier inclut les en-têtes du SDK ASIO (asio.h, iasiodrv.h, asiosys.h), utilisés
    tels quels. Tant que l'accord de licence propriétaire de Steinberg n'est pas contresigné, il
    sert aux tests INTERNES (SDK aussi publié sous GPLv3) : rien de ce qui en est issu n'est
    distribué (option CMake NOVA_AUDIO_WITH_ASIO, exclu de l'installateur sans
    NOVA_SHIP_NATIVE_AUDIO=1). Voir native-audio/LICENCES.md.

    - Pilote installé : CLSID lu dans HKLM\SOFTWARE\ASIO\<nom> (lecture seule), CoCreateInstance.
    - Pilote factice des tests : DLL chargée directement (DllGetClassObject), SANS inscription COM.
    - Tous les appels au pilote (init, createBuffers, start, stop, disposeBuffers) se font sur le fil
      principal du moteur (STA, comme les hôtes ASIO habituels) ; seul bufferSwitch tourne sur le fil
      du pilote, dont la priorité (MMCSS « Pro Audio ») appartient au pilote : l'hôte n'y touche pas
      (règle du SDK ASIO 2.3, § « MMCSS »).
    - Pertes : saut de samplePosition (horloge de la carte) -> images perdues, signalées au moteur
      (qui insère du silence dans la prise, la suite reste calée).

    Copyright (C) 2026 Make Music.
    SPDX-License-Identifier: GPL-3.0-only
    Ce fichier fait partie de NovaAudioEngine, logiciel libre : vous pouvez le redistribuer et/ou le
    modifier selon les termes de la GNU General Public License version 3, publiée par la Free Software
    Foundation, avec la permission supplémentaire (article 7) décrite dans native-audio/LICENCES.md
    (pilotes audio et modules d'effets tiers chargés à l'exécution). Il est distribué SANS AUCUNE
    GARANTIE, sans même la garantie implicite de qualité marchande ou d'adéquation à un usage
    particulier. Texte de la licence : native-audio/LICENSE (https://www.gnu.org/licenses/gpl-3.0.html).
*/
#include "Device.h"

#include <objbase.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "asiosys.h"
#include "asio.h"
#include "iasiodrv.h"

namespace nova::audio
{
    using json::Value;

    namespace
    {
        constexpr int kMaxCh = 32;

        bool readAsioClsid (const std::string& name, CLSID& clsid)
        {
            HKEY k = nullptr;
            const auto path = L"SOFTWARE\\ASIO\\" + widen (name);
            if (RegOpenKeyExW (HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
            wchar_t buf[128] {};
            DWORD sz = sizeof (buf);
            DWORD type = 0;
            const bool ok = RegQueryValueExW (k, L"CLSID", nullptr, &type, (LPBYTE) buf, &sz) == ERROR_SUCCESS && type == REG_SZ;
            RegCloseKey (k);
            return ok && SUCCEEDED (CLSIDFromString (buf, &clsid));
        }

        int64_t samplesToInt (const ASIOSamples& s) { return (int64_t) (((uint64_t) s.hi << 32) | (uint64_t) s.lo); }

        int bytesFor (ASIOSampleType t)
        {
            switch (t)
            {
                case ASIOSTInt16LSB: case ASIOSTInt16MSB: return 2;
                case ASIOSTInt24LSB: case ASIOSTInt24MSB: return 3;
                case ASIOSTFloat64LSB: case ASIOSTFloat64MSB: return 8;
                default: return 4;
            }
        }

        bool isMsb (ASIOSampleType t) { return t < ASIOSTInt16LSB; }

        const char* typeName (ASIOSampleType t)
        {
            switch (t)
            {
                case ASIOSTInt16LSB: return "int16";
                case ASIOSTInt24LSB: return "int24";
                case ASIOSTInt32LSB: return "int32";
                case ASIOSTFloat32LSB: return "float32";
                case ASIOSTFloat64LSB: return "float64";
                case ASIOSTInt32LSB16: return "int32(16)";
                case ASIOSTInt32LSB18: return "int32(18)";
                case ASIOSTInt32LSB20: return "int32(20)";
                case ASIOSTInt32LSB24: return "int32(24)";
                default: return isMsb (t) ? "big-endian" : "autre";
            }
        }

        // Conversion carte -> float (fil du pilote : sans allocation).
        void toFloat (const void* src, ASIOSampleType t, float* dst, int n)
        {
            const uint8_t* s = (const uint8_t*) src;
            const bool msb = isMsb (t);
            const int b = bytesFor (t);
            uint8_t tmp[8];
            for (int i = 0; i < n; ++i, s += b)
            {
                const uint8_t* p = s;
                if (msb) { for (int k = 0; k < b; ++k) tmp[k] = s[b - 1 - k]; p = tmp; }
                const int base = msb ? (int) t + 16 : (int) t;
                switch (base)
                {
                    case ASIOSTInt16LSB: { int16_t v; std::memcpy (&v, p, 2); dst[i] = (float) v * (1.0f / 32768.0f); break; }
                    case ASIOSTInt24LSB: { const int32_t v = (int32_t) ((uint32_t) p[0] << 8 | (uint32_t) p[1] << 16 | (uint32_t) p[2] << 24) >> 8; dst[i] = (float) v * (1.0f / 8388608.0f); break; }
                    case ASIOSTFloat32LSB: std::memcpy (&dst[i], p, 4); break;
                    case ASIOSTFloat64LSB: { double v; std::memcpy (&v, p, 8); dst[i] = (float) v; break; }
                    case ASIOSTInt32LSB16: { int32_t v; std::memcpy (&v, p, 4); dst[i] = (float) v * (1.0f / 32768.0f); break; }
                    case ASIOSTInt32LSB18: { int32_t v; std::memcpy (&v, p, 4); dst[i] = (float) v * (1.0f / 131072.0f); break; }
                    case ASIOSTInt32LSB20: { int32_t v; std::memcpy (&v, p, 4); dst[i] = (float) v * (1.0f / 524288.0f); break; }
                    case ASIOSTInt32LSB24: { int32_t v; std::memcpy (&v, p, 4); dst[i] = (float) v * (1.0f / 8388608.0f); break; }
                    default: { int32_t v; std::memcpy (&v, p, 4); dst[i] = (float) ((double) v * (1.0 / 2147483648.0)); break; }
                }
            }
        }

        void fromFloat (const float* src, ASIOSampleType t, void* dstv, int n)
        {
            uint8_t* d = (uint8_t*) dstv;
            const bool msb = isMsb (t);
            const int b = bytesFor (t);
            const int base = msb ? (int) t + 16 : (int) t;
            uint8_t tmp[8];
            for (int i = 0; i < n; ++i, d += b)
            {
                const double x = std::max (-1.0, std::min (1.0, (double) src[i]));
                uint8_t* p = msb ? tmp : d;
                switch (base)
                {
                    case ASIOSTInt16LSB: { const int16_t v = (int16_t) std::lrint (std::min (32767.0, x * 32768.0)); std::memcpy (p, &v, 2); break; }
                    case ASIOSTInt24LSB: { const int32_t v = (int32_t) std::lrint (std::min (8388607.0, x * 8388608.0)); p[0] = (uint8_t) v; p[1] = (uint8_t) (v >> 8); p[2] = (uint8_t) (v >> 16); break; }
                    case ASIOSTFloat32LSB: std::memcpy (p, &src[i], 4); break;
                    case ASIOSTFloat64LSB: { const double v = src[i]; std::memcpy (p, &v, 8); break; }
                    case ASIOSTInt32LSB16: { const int32_t v = (int32_t) std::lrint (std::min (32767.0, x * 32768.0)); std::memcpy (p, &v, 4); break; }
                    case ASIOSTInt32LSB18: { const int32_t v = (int32_t) std::lrint (std::min (131071.0, x * 131072.0)); std::memcpy (p, &v, 4); break; }
                    case ASIOSTInt32LSB20: { const int32_t v = (int32_t) std::lrint (std::min (524287.0, x * 524288.0)); std::memcpy (p, &v, 4); break; }
                    case ASIOSTInt32LSB24: { const int32_t v = (int32_t) std::lrint (std::min (8388607.0, x * 8388608.0)); std::memcpy (p, &v, 4); break; }
                    default: { const int32_t v = (int32_t) std::llrint (std::min (2147483647.0, x * 2147483648.0)); std::memcpy (p, &v, 4); break; }
                }
                if (msb) for (int k = 0; k < b; ++k) d[k] = tmp[b - 1 - k];
            }
        }
    }

    class AsioDevice;
    static AsioDevice* gAsio = nullptr;   // un seul pilote ASIO par processus (rappels sans contexte)
    HWND gAsioOwnerWindow = nullptr;      // posé par Main (fenêtre de messages du moteur)

    class AsioDevice : public AudioDevice
    {
    public:
        explicit AsioDevice (const DeviceSpec& s) : spec (s) {}
        ~AsioDevice() override { close(); }

        const DeviceInfo& info() const override { return inf; }

        bool open (const DeviceRequest& r, std::string& err) override
        {
            if (gAsio != nullptr && gAsio != this) { err = "un pilote ASIO est déjà ouvert"; return false; }
            if (! loadDriver (err)) return false;
            if (! drv->init (gAsioOwnerWindow))
            {
                char msg[128] {};
                drv->getErrorMessage (msg);
                err = std::string ("le pilote ASIO refuse de démarrer : ") + msg;
                unload();
                return false;
            }
            char nm[64] {};
            drv->getDriverName (nm);
            inf.type = "asio";
            inf.name = nm;
            long nIn = 0, nOut = 0;
            if (drv->getChannels (&nIn, &nOut) != ASE_OK) { err = "canaux ASIO illisibles"; unload(); return false; }
            long mn = 0, mx = 0, pref = 0, gran = 0;
            drv->getBufferSize (&mn, &mx, &pref, &gran);
            inf.minBuffer = (int) mn; inf.maxBuffer = (int) mx; inf.preferredBuffer = (int) pref; inf.granularity = (int) gran;
            for (double rate : { 44100.0, 48000.0, 88200.0, 96000.0 })
                if (drv->canSampleRate (rate) == ASE_OK) inf.rates.push_back (rate);

            // Fréquence : celle demandée si la carte la prend, sinon la sienne (le moteur rééchantillonne).
            ASIOSampleRate cur = 0;
            drv->getSampleRate (&cur);
            if (r.sampleRate > 0 && cur != r.sampleRate && drv->canSampleRate (r.sampleRate) == ASE_OK)
                drv->setSampleRate (r.sampleRate);
            drv->getSampleRate (&cur);
            inf.sampleRate = cur > 0 ? cur : r.sampleRate;

            // Taille de tampon : la demande, ramenée dans [min, max] et au pas du pilote.
            long buf = r.bufferFrames > 0 ? r.bufferFrames : pref;
            buf = std::max (mn, std::min (mx, buf));
            if (gran == -1)
            {
                long p = 1;
                while (p < buf) p <<= 1;
                buf = std::max (mn, std::min (mx, p));
            }
            else if (gran > 0 && mn != mx) buf = mn + ((buf - mn + gran - 1) / gran) * gran;
            if (mn == mx) buf = mn;
            inf.bufferFrames = (int) buf;

            numIn = std::min<int> (kMaxCh, r.inChannels > 0 ? std::min<long> (r.inChannels, nIn) : nIn);
            numOut = std::min<int> (kMaxCh, r.outChannels > 0 ? std::min<long> (r.outChannels, nOut) : nOut);
            inf.inChannels = numIn;
            inf.outChannels = numOut;

            for (int i = 0; i < numIn; ++i) { infos[i].isInput = ASIOTrue; infos[i].channelNum = i; infos[i].buffers[0] = infos[i].buffers[1] = nullptr; }
            for (int i = 0; i < numOut; ++i) { infos[numIn + i].isInput = ASIOFalse; infos[numIn + i].channelNum = i; infos[numIn + i].buffers[0] = infos[numIn + i].buffers[1] = nullptr; }
            callbacks.bufferSwitch = &AsioDevice::cbBufferSwitch;
            callbacks.sampleRateDidChange = &AsioDevice::cbRateChanged;
            callbacks.asioMessage = &AsioDevice::cbMessage;
            callbacks.bufferSwitchTimeInfo = &AsioDevice::cbBufferSwitchTimeInfo;
            gAsio = this;
            if (drv->createBuffers (infos, numIn + numOut, buf, &callbacks) != ASE_OK)
            {
                gAsio = nullptr;
                err = "le pilote ASIO refuse ce tampon (" + std::to_string (buf) + " images)";
                unload();
                return false;
            }
            buffersCreated = true;
            for (int i = 0; i < numIn + numOut; ++i)
            {
                ASIOChannelInfo ci {};
                ci.channel = infos[i].channelNum;
                ci.isInput = infos[i].isInput;
                drv->getChannelInfo (&ci);
                types[i] = ci.type;
                (i < numIn ? inf.inNames : inf.outNames).push_back (ci.name);
            }
            inf.format = numOut > 0 ? typeName (types[numIn]) : (numIn > 0 ? typeName (types[0]) : "");
            long li = 0, lo = 0;
            drv->getLatencies (&li, &lo);
            inf.inputLatency = (int) li;
            inf.outputLatency = (int) lo;
            inf.outputReady = drv->outputReady() == ASE_OK;
            useOutputReady = inf.outputReady;

            inData.assign ((size_t) std::max (1, numIn) * (size_t) buf, 0.0f);
            outData.assign ((size_t) std::max (1, numOut) * (size_t) buf, 0.0f);
            for (int c = 0; c < numIn; ++c) inPtr[c] = &inData[(size_t) c * buf];
            for (int c = 0; c < numOut; ++c) outPtr[c] = &outData[(size_t) c * buf];
            return true;
        }

        bool start (AudioCallback* c, std::string& err) override
        {
            cb = c;
            frame = 0;
            lastPos = INT64_MIN;
            if (drv == nullptr || drv->start() != ASE_OK) { err = "le pilote ASIO refuse de démarrer le flux"; return false; }
            started = true;
            return true;
        }

        void stop() override
        {
            if (drv != nullptr && started) drv->stop();
            started = false;
        }

        void close() override
        {
            stop();
            if (drv != nullptr && buffersCreated) drv->disposeBuffers();
            buffersCreated = false;
            if (gAsio == this) gAsio = nullptr;
            unload();
        }

        Value extraStats() const override
        {
            auto o = Value::object();
            o.set ("driver", inf.name);
            o.set ("overloads", (double) overloads.load());
            o.set ("resets_requested", (double) resets.load());
            return o;
        }

        bool openControlPanel() { return drv != nullptr && drv->controlPanel() == ASE_OK; }

    private:
        bool loadDriver (std::string& err)
        {
            CLSID clsid {};
            if (spec.type == "asio-dll")
            {
                // Pilote factice des tests : DLL chargée directement, sans inscription système.
                const auto cl = spec.opt ("clsid");
                if (cl.empty() || FAILED (CLSIDFromString (widen (cl).c_str(), &clsid))) { err = "clsid manquant pour asio-dll"; return false; }
                dll = LoadLibraryW (widen (spec.name).c_str());
                if (dll == nullptr) { err = "DLL du pilote introuvable : " + spec.name; return false; }
                using GetCO = HRESULT (STDAPICALLTYPE*) (REFCLSID, REFIID, LPVOID*);
                auto fn = (GetCO) GetProcAddress (dll, "DllGetClassObject");
                IClassFactory* cf = nullptr;
                if (fn == nullptr || FAILED (fn (clsid, IID_IClassFactory, (void**) &cf)) || cf == nullptr) { err = "DllGetClassObject a échoué"; unload(); return false; }
                const HRESULT hr = cf->CreateInstance (nullptr, clsid, (void**) &drv);
                cf->Release();
                if (FAILED (hr) || drv == nullptr) { err = "pilote non créé"; unload(); return false; }
                return true;
            }
            if (! readAsioClsid (spec.name, clsid)) { err = "pilote ASIO introuvable : " + spec.name; return false; }
            // ASIO : l'IID demandé est le CLSID du pilote (convention du SDK).
            if (FAILED (CoCreateInstance (clsid, nullptr, CLSCTX_INPROC_SERVER, clsid, (void**) &drv)) || drv == nullptr)
            { err = "pilote ASIO non chargé : " + spec.name; return false; }
            return true;
        }

        void unload()
        {
            if (drv != nullptr) { drv->Release(); drv = nullptr; }
            // La DLL factice reste chargée (comme COM le ferait) : pas de FreeLibrary pendant un rappel.
        }

        //---------------------------------------------------------------- rappels du pilote
        static void cbBufferSwitch (long index, ASIOBool processNow)
        {
            if (gAsio == nullptr) return;
            ASIOTime t {};
            ASIOTimeStamp ts {};
            if (gAsio->drv->getSamplePosition (&t.timeInfo.samplePosition, &ts) == ASE_OK)
                t.timeInfo.flags = kSamplePositionValid | kSystemTimeValid;
            gAsio->process (index, &t);
            (void) processNow;
        }

        static ASIOTime* cbBufferSwitchTimeInfo (ASIOTime* params, long index, ASIOBool)
        {
            if (gAsio != nullptr) gAsio->process (index, params);
            return nullptr;
        }

        static void cbRateChanged (ASIOSampleRate) { if (gAsio) gAsio->events.fetch_or (kEvReset); }

        static long cbMessage (long selector, long value, void*, double*)
        {
            switch (selector)
            {
                case kAsioSelectorSupported:
                    return (value == kAsioResetRequest || value == kAsioEngineVersion || value == kAsioResyncRequest
                            || value == kAsioLatenciesChanged || value == kAsioSupportsTimeInfo || value == kAsioOverload
                            || value == kAsioBufferSizeChange) ? 1L : 0L;
                case kAsioEngineVersion: return 2L;
                case kAsioResetRequest: if (gAsio) { gAsio->resets.fetch_add (1); gAsio->events.fetch_or (kEvReset); } return 1L;
                case kAsioBufferSizeChange: if (gAsio) gAsio->events.fetch_or (kEvReset | kEvBufferSize); return 1L;
                case kAsioResyncRequest: if (gAsio) gAsio->events.fetch_or (kEvResync); return 1L;
                case kAsioLatenciesChanged: if (gAsio) gAsio->events.fetch_or (kEvLatency); return 1L;
                case kAsioSupportsTimeInfo: return 1L;
                case kAsioSupportsTimeCode: return 0L;
                case kAsioOverload: if (gAsio) { gAsio->overloads.fetch_add (1); gAsio->xruns.fetch_add (1); gAsio->events.fetch_or (kEvOverload); } return 1L;
                default: return 0L;
            }
        }

        // Fil du pilote : conversion, moteur, conversion. Aucune allocation, aucun verrou.
        void process (long index, const ASIOTime* t)
        {
            const int n = inf.bufferFrames;
            BlockTime bt;
            bt.hostMs = nowMs();
            if (t != nullptr && (t->timeInfo.flags & kSamplePositionValid))
            {
                const int64_t pos = samplesToInt (t->timeInfo.samplePosition);
                if (lastPos != INT64_MIN)
                {
                    const int64_t gap = pos - (lastPos + n);
                    // Saut d'au moins un demi-bloc : la carte a perdu des images (rappel manqué).
                    if (gap >= n / 2 && gap < (int64_t) inf.sampleRate * 10)
                    {
                        bt.lostBefore = gap;
                        lostFrames.fetch_add ((uint64_t) gap);
                        xruns.fetch_add (1);
                        frame += gap;
                    }
                }
                lastPos = pos;
            }
            bt.deviceFrame = frame;
            for (int c = 0; c < numIn; ++c) toFloat (infos[c].buffers[index], types[c], inPtr[c], n);
            std::memset (outData.data(), 0, sizeof (float) * outData.size());
            cb->audioBlock (inPtr, numIn, outPtr, numOut, n, bt);
            for (int c = 0; c < numOut; ++c) fromFloat (outPtr[c], types[numIn + c], infos[numIn + c].buffers[index], n);
            if (useOutputReady) drv->outputReady();
            frame += n;
        }

        DeviceSpec spec;
        DeviceInfo inf;
        IASIO* drv = nullptr;
        HMODULE dll = nullptr;
        ASIOBufferInfo infos[2 * kMaxCh] {};
        ASIOSampleType types[2 * kMaxCh] {};
        ASIOCallbacks callbacks {};
        int numIn = 0, numOut = 0;
        bool buffersCreated = false, started = false, useOutputReady = false;
        AudioCallback* cb = nullptr;
        std::vector<float> inData, outData;
        float* inPtr[kMaxCh] {};
        float* outPtr[kMaxCh] {};
        int64_t frame = 0, lastPos = INT64_MIN;
        std::atomic<uint64_t> overloads { 0 }, resets { 0 };
    };

    std::unique_ptr<AudioDevice> createAsioDevice (const DeviceSpec& s) { return std::make_unique<AsioDevice> (s); }

    bool asioControlPanel (AudioDevice* d)
    {
        auto* a = dynamic_cast<AsioDevice*> (d);
        return a != nullptr && a->openControlPanel();
    }

    Value listAsioDrivers()
    {
        // Lecture seule du registre : HKLM\SOFTWARE\ASIO\<pilote>\CLSID. Rien n'est chargé.
        auto out = Value::array();
        HKEY k = nullptr;
        if (RegOpenKeyExW (HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ, &k) != ERROR_SUCCESS) return out;
        for (DWORD i = 0;; ++i)
        {
            wchar_t name[256];
            DWORD len = 256;
            if (RegEnumKeyExW (k, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            auto o = Value::object();
            o.set ("name", narrow (name));
            CLSID c;
            o.set ("registered", readAsioClsid (narrow (name), c));
            out.push (o);
        }
        RegCloseKey (k);
        return out;
    }
}
