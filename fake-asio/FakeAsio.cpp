/*
    NovaFakeAsio — pilote ASIO FACTICE pour les tests du moteur natif (jamais livré, jamais inscrit
    dans le registre). Le moteur le charge directement : device = "asio-dll:<chemin>?clsid={…}".

    Il se comporte comme une carte : un fil (MMCSS « Pro Audio », priorité critique, comme le SDK
    le demande aux pilotes) appelle bufferSwitch toutes les « buffer / rate » secondes (ou N fois
    plus vite), avec double tampon, samplePosition et heure système.

    Modèle physique (même que les tests C++ « null?loopback ») : l'échantillon s de la sortie
    remplie au rappel k sort du convertisseur à (k+1)·B + s + extra_out ; l'entrée livrée au rappel
    k a été captée pendant la période k-1. Avec un câble sortie c -> entrée c, l'aller-retour vaut
    donc exactement 2·B + extra_in + extra_out images, et getLatencies annonce B + extra_in /
    B + extra_out.

    Réglages : variable d'environnement NOVA_FAKEASIO (lue à init), « clé=valeur;… » :
      inputs=8  outputs=8  rate=48000  buffer=128  extra_in=24  extra_out=24
      format=int32|float32|int24|int16      (format des échantillons)
      noise=0,1 | all | none                 (entrées qui reçoivent le bruit déterministe des tests,
                                             les autres reçoivent la boucle de la sortie de même n°)
      seed=1  speed=1 (0 = au plus vite)  drop_every=0 (s : un rappel sauté de temps en temps)
      report=<fichier.json>                  (bilan écrit à l'arrêt)

    Copyright (C) 2026 Make Music.
    SPDX-License-Identifier: GPL-3.0-only
    Ce fichier fait partie de NovaAudioEngine, logiciel libre : vous pouvez le redistribuer et/ou le
    modifier selon les termes de la GNU General Public License version 3, publiée par la Free Software
    Foundation, avec la permission supplémentaire (article 7) décrite dans native-audio/LICENCES.md
    (pilotes audio et modules d'effets tiers chargés à l'exécution). Il est distribué SANS AUCUNE
    GARANTIE, sans même la garantie implicite de qualité marchande ou d'adéquation à un usage
    particulier. Texte de la licence : native-audio/LICENSE (https://www.gnu.org/licenses/gpl-3.0.html).
    Tests internes uniquement.
*/
#ifndef NOMINMAX
 #define NOMINMAX 1
#endif
#include <windows.h>
#include <objbase.h>
#include <avrt.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "asiosys.h"
#include "asio.h"
#include "iasiodrv.h"

// {A6B0C3E2-5D1F-4C7A-9E31-0F4E2B7D9C11}
static const CLSID CLSID_NovaFakeAsio = { 0xa6b0c3e2, 0x5d1f, 0x4c7a, { 0x9e, 0x31, 0x0f, 0x4e, 0x2b, 0x7d, 0x9c, 0x11 } };
static std::atomic<long> gLocks { 0 }, gObjects { 0 };

namespace
{
    float testNoise (uint64_t seed, uint32_t channel, uint64_t frame)
    {
        uint64_t z = frame * 0x9E3779B97F4A7C15ull + (uint64_t) channel * 0xD1B54A32D192ED03ull + seed;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z = z ^ (z >> 31);
        return (float) ((double) (z >> 40) / 16777216.0 - 0.5);
    }

    std::map<std::string, std::string> readConfig()
    {
        std::map<std::string, std::string> m;
        char buf[4096] {};
        if (GetEnvironmentVariableA ("NOVA_FAKEASIO", buf, sizeof (buf)) == 0) return m;
        std::string s = buf;
        size_t i = 0;
        while (i < s.size())
        {
            auto sc = s.find (';', i);
            if (sc == std::string::npos) sc = s.size();
            auto kv = s.substr (i, sc - i);
            auto eq = kv.find ('=');
            if (eq != std::string::npos) m[kv.substr (0, eq)] = kv.substr (eq + 1);
            i = sc + 1;
        }
        return m;
    }

    int64_t qpc() { LARGE_INTEGER c; QueryPerformanceCounter (&c); return c.QuadPart; }
    double qpcFreq() { static const double f = [] { LARGE_INTEGER x; QueryPerformanceFrequency (&x); return (double) x.QuadPart; }(); return f; }
}

class FakeAsio : public IASIO
{
public:
    FakeAsio() { ++gObjects; }
    virtual ~FakeAsio() { stop(); disposeBuffers(); --gObjects; }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface (REFIID riid, void** ppv) override
    {
        if (riid == IID_IUnknown || riid == CLSID_NovaFakeAsio) { *ppv = this; AddRef(); return S_OK; }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG) ++refs; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const long r = --refs;
        if (r == 0) delete this;
        return (ULONG) r;
    }

    // IASIO
    ASIOBool init (void*) override
    {
        cfg = readConfig();
        auto num = [this] (const char* k, double d) { auto it = cfg.find (k); return it == cfg.end() ? d : std::atof (it->second.c_str()); };
        nIn = (long) num ("inputs", 8);
        nOut = (long) num ("outputs", 8);
        rate = num ("rate", 48000);
        prefBuffer = (long) num ("buffer", 128);
        extraIn = (long) num ("extra_in", 24);
        extraOut = (long) num ("extra_out", 24);
        seed = (uint64_t) num ("seed", 1);
        speed = num ("speed", 1.0);
        dropEvery = num ("drop_every", 0);
        resetAfter = num ("reset_after", 0);
        const auto f = cfg.count ("format") ? cfg["format"] : std::string ("int32");
        type = f == "float32" ? ASIOSTFloat32LSB : f == "int24" ? ASIOSTInt24LSB : f == "int16" ? ASIOSTInt16LSB : ASIOSTInt32LSB;
        noiseMask.assign ((size_t) nIn, false);
        const auto nz = cfg.count ("noise") ? cfg["noise"] : std::string ("none");
        if (nz == "all") std::fill (noiseMask.begin(), noiseMask.end(), true);
        else if (nz != "none")
        {
            size_t i = 0;
            while (i < nz.size())
            {
                auto c = nz.find (',', i);
                if (c == std::string::npos) c = nz.size();
                const int ch = std::atoi (nz.substr (i, c - i).c_str());
                if (ch >= 0 && ch < nIn) noiseMask[(size_t) ch] = true;
                i = c + 1;
            }
        }
        reportPath = cfg.count ("report") ? cfg["report"] : std::string();
        return ASIOTrue;
    }
    void getDriverName (char* name) override { std::strcpy (name, "Nova Fake ASIO"); }
    long getDriverVersion() override { return 1; }
    void getErrorMessage (char* s) override { std::strcpy (s, lastError.c_str()); }

    ASIOError start() override
    {
        if (! callbacks || buffers.empty()) return ASE_NotPresent;
        if (running) return ASE_OK;
        quit = false;
        running = true;
        th = std::thread ([this] { run(); });
        return ASE_OK;
    }
    ASIOError stop() override
    {
        quit = true;
        if (th.joinable()) th.join();
        if (running) writeReport();
        running = false;
        return ASE_OK;
    }
    ASIOError getChannels (long* in, long* out) override { *in = nIn; *out = nOut; return ASE_OK; }
    ASIOError getLatencies (long* in, long* out) override { *in = buffer + extraIn; *out = buffer + extraOut; return ASE_OK; }
    ASIOError getBufferSize (long* mn, long* mx, long* pref, long* gran) override { *mn = 32; *mx = 2048; *pref = prefBuffer; *gran = -1; return ASE_OK; }
    ASIOError canSampleRate (ASIOSampleRate r) override { return (r == 44100 || r == 48000 || r == 88200 || r == 96000) && ! cfg.count ("fixed_rate") ? ASE_OK : (r == rate ? ASE_OK : ASE_NoClock); }
    ASIOError getSampleRate (ASIOSampleRate* r) override { *r = rate; return ASE_OK; }
    ASIOError setSampleRate (ASIOSampleRate r) override
    {
        if (canSampleRate (r) != ASE_OK) return ASE_NoClock;
        rate = r;
        return ASE_OK;
    }
    ASIOError getClockSources (ASIOClockSource* c, long* n) override
    {
        c[0].index = 0; c[0].associatedChannel = -1; c[0].associatedGroup = -1; c[0].isCurrentSource = ASIOTrue;
        std::strcpy (c[0].name, "Interne");
        *n = 1;
        return ASE_OK;
    }
    ASIOError setClockSource (long) override { return ASE_OK; }
    ASIOError getSamplePosition (ASIOSamples* s, ASIOTimeStamp* t) override
    {
        const int64_t p = samplePos.load();
        s->hi = (unsigned long) ((uint64_t) p >> 32);
        s->lo = (unsigned long) ((uint64_t) p & 0xFFFFFFFFu);
        const uint64_t ns = (uint64_t) ((double) qpc() * 1e9 / qpcFreq());
        t->hi = (unsigned long) (ns >> 32);
        t->lo = (unsigned long) (ns & 0xFFFFFFFFu);
        return ASE_OK;
    }
    ASIOError getChannelInfo (ASIOChannelInfo* info) override
    {
        const long n = info->isInput ? nIn : nOut;
        if (info->channel < 0 || info->channel >= n) return ASE_InvalidParameter;
        info->isActive = ASIOTrue;
        info->channelGroup = 0;
        info->type = type;
        std::snprintf (info->name, sizeof (info->name), "%s %ld", info->isInput ? "Entree" : "Sortie", info->channel + 1);
        return ASE_OK;
    }
    ASIOError createBuffers (ASIOBufferInfo* infos, long num, long size, ASIOCallbacks* cb) override
    {
        disposeBuffers();
        if (size < 32 || size > 2048) return ASE_InvalidMode;
        buffer = size;
        callbacks = cb;
        bytes = type == ASIOSTInt16LSB ? 2 : (type == ASIOSTInt24LSB ? 3 : 4);
        for (long i = 0; i < num; ++i)
        {
            Chan c;
            c.input = infos[i].isInput == ASIOTrue;
            c.index = infos[i].channelNum;
            c.mem.assign ((size_t) bytes * (size_t) size * 2, 0);
            infos[i].buffers[0] = c.mem.data();
            infos[i].buffers[1] = c.mem.data() + (size_t) bytes * size;
            buffers.push_back (std::move (c));
        }
        timeInfo = callbacks->asioMessage && callbacks->asioMessage (kAsioSupportsTimeInfo, 0, nullptr, nullptr) == 1;
        loopDelay = 2 * buffer + extraIn + extraOut;
        loopSize = 1;
        while (loopSize < (uint64_t) (loopDelay + 4 * buffer)) loopSize <<= 1;
        loop.assign ((size_t) loopSize * (size_t) std::max<long> (1, nOut), 0.0f);
        tmp.assign ((size_t) buffer, 0.0f);
        // Comme un vrai pilote (tampons en mémoire non paginée) : rien de ce que touche le fil n'est paginé.
        SIZE_T mn = 0, mx = 0;
        GetProcessWorkingSetSize (GetCurrentProcess(), &mn, &mx);
        size_t total = loop.size() * sizeof (float) + tmp.size() * sizeof (float);
        for (auto& c : buffers) total += c.mem.size();
        SetProcessWorkingSetSize (GetCurrentProcess(), mn + total + (16u << 20), std::max (mx, mn + total + (32u << 20)));
        VirtualLock (loop.data(), loop.size() * sizeof (float));
        VirtualLock (tmp.data(), tmp.size() * sizeof (float));
        for (auto& c : buffers) VirtualLock (c.mem.data(), c.mem.size());
        return ASE_OK;
    }
    ASIOError disposeBuffers() override
    {
        stop();
        buffers.clear();
        callbacks = nullptr;
        return ASE_OK;
    }
    ASIOError controlPanel() override { return ASE_NotPresent; }
    ASIOError future (long selector, void*) override
    {
        switch (selector)
        {
            case kAsioCanTimeInfo: return ASE_SUCCESS;
            case kAsioCanReportOverload: return ASE_SUCCESS;
            default: return ASE_InvalidParameter;
        }
    }
    ASIOError outputReady() override { ++outputReadyCalls; return ASE_OK; }

private:
    struct Chan { bool input = false; long index = 0; std::vector<uint8_t> mem; };

    void toBytes (const float* src, uint8_t* dst)
    {
        for (long i = 0; i < buffer; ++i, dst += bytes)
        {
            const double x = std::max (-1.0, std::min (1.0, (double) src[i]));
            if (type == ASIOSTFloat32LSB) std::memcpy (dst, &src[i], 4);
            else if (type == ASIOSTInt16LSB) { const int16_t v = (int16_t) std::lrint (std::min (32767.0, x * 32768.0)); std::memcpy (dst, &v, 2); }
            else if (type == ASIOSTInt24LSB) { const int32_t v = (int32_t) std::lrint (std::min (8388607.0, x * 8388608.0)); dst[0] = (uint8_t) v; dst[1] = (uint8_t) (v >> 8); dst[2] = (uint8_t) (v >> 16); }
            else { const int32_t v = (int32_t) std::llrint (std::min (2147483647.0, x * 2147483648.0)); std::memcpy (dst, &v, 4); }
        }
    }
    void fromBytes (const uint8_t* src, float* dst)
    {
        for (long i = 0; i < buffer; ++i, src += bytes)
        {
            if (type == ASIOSTFloat32LSB) std::memcpy (&dst[i], src, 4);
            else if (type == ASIOSTInt16LSB) { int16_t v; std::memcpy (&v, src, 2); dst[i] = (float) v / 32768.0f; }
            else if (type == ASIOSTInt24LSB) { const int32_t v = (int32_t) ((uint32_t) src[0] << 8 | (uint32_t) src[1] << 16 | (uint32_t) src[2] << 24) >> 8; dst[i] = (float) v / 8388608.0f; }
            else { int32_t v; std::memcpy (&v, src, 4); dst[i] = (float) ((double) v / 2147483648.0); }
        }
    }

    void run()
    {
        // Règle du SDK : le pilote met lui-même son fil en MMCSS « Pro Audio », priorité critique.
        DWORD task = 0;
        HANDLE av = AvSetMmThreadCharacteristicsW (L"Pro Audio", &task);
        if (av) AvSetMmThreadPriority (av, AVRT_PRIORITY_CRITICAL);
        HANDLE timer = CreateWaitableTimerExW (nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        const double f = qpcFreq();
        const double periodTicks = speed > 0 ? (double) buffer / rate * f / speed : 0.0;
        const int64_t t0 = qpc();
        const int64_t dropPeriod = dropEvery > 0 ? (int64_t) (dropEvery * rate / buffer) : 0;
        int64_t k = 0;
        int64_t pos = 0;   // n° de l'image de la carte (horloge matérielle)
        while (! quit.load())
        {
            if (speed > 0)
            {
                const int64_t due = t0 + (int64_t) ((double) k * periodTicks);
                // Comme une vraie carte : le fil dort jusqu'à « l'interruption » (minuterie haute
                // résolution), sans tourner à vide (MMCSS bride un fil critique qui consomme tout).
                const double us = (double) (due - qpc()) * 1e6 / f;
                if (us > 50 && timer)
                {
                    LARGE_INTEGER d;
                    d.QuadPart = -(LONGLONG) (us * 10);
                    SetWaitableTimerEx (timer, &d, 0, nullptr, nullptr, nullptr, 0);
                    WaitForSingleObject (timer, INFINITE);
                }
            }
            const long idx = (long) (k & 1);
            if (resetAfter > 0 && ! resetSent && (double) pos >= resetAfter * rate)
            {
                // Comme un pilote dont on change le tampon dans son panneau : demande de réinitialisation.
                resetSent = true;
                callbacks->asioMessage (kAsioResetRequest, 0, nullptr, nullptr);
            }
            if (dropPeriod > 0 && k > 0 && k % dropPeriod == 0)
            {
                // Rappel sauté (simulation d'un décrochage du pilote) : la carte avance quand même.
                for (long c = 0; c < nOut; ++c)
                    for (long i = 0; i < buffer; ++i) loop[(size_t) c * loopSize + ((uint64_t) (pos + i) & (loopSize - 1))] = 0.0f;
                ++dropped;
                pos += buffer;
                samplePos = pos;
                ++k;
                continue;
            }
            // Entrées : bruit déterministe ou boucle (sortie de même n°, retardée de 2·B + extra).
            for (auto& c : buffers)
            {
                if (! c.input) continue;
                float* t = tmp.data();
                if (noiseMask[(size_t) c.index])
                    for (long i = 0; i < buffer; ++i) t[i] = testNoise (seed, (uint32_t) c.index, (uint64_t) (pos + i));
                else
                    for (long i = 0; i < buffer; ++i)
                    {
                        const int64_t src = pos + i - loopDelay;
                        t[i] = (src >= 0 && c.index < nOut) ? loop[(size_t) c.index * loopSize + ((uint64_t) src & (loopSize - 1))] : 0.0f;
                    }
                toBytes (t, c.mem.data() + (size_t) idx * bytes * buffer);
            }
            samplePos = pos;
            const int64_t a = qpc();
            // Réveil du fil du pilote : en retard de combien sur l'échéance de ce bloc ?
            const double wakeLateUs = speed > 0 ? (double) (a - (t0 + (int64_t) ((double) k * periodTicks))) * 1e6 / f : 0.0;
            maxWakeLateUs = std::max (maxWakeLateUs, wakeLateUs);
            if (timeInfo)
            {
                ASIOTime tm {};
                tm.timeInfo.samplePosition.hi = (unsigned long) ((uint64_t) pos >> 32);
                tm.timeInfo.samplePosition.lo = (unsigned long) ((uint64_t) pos & 0xFFFFFFFFu);
                const uint64_t ns = (uint64_t) ((double) a * 1e9 / f);
                tm.timeInfo.systemTime.hi = (unsigned long) (ns >> 32);
                tm.timeInfo.systemTime.lo = (unsigned long) (ns & 0xFFFFFFFFu);
                tm.timeInfo.sampleRate = rate;
                tm.timeInfo.flags = kSystemTimeValid | kSamplePositionValid | kSampleRateValid;
                callbacks->bufferSwitchTimeInfo (&tm, idx, ASIOTrue);
            }
            else callbacks->bufferSwitch (idx, ASIOTrue);
            const int64_t b = qpc();
            const double cbUs = (double) (b - a) * 1e6 / f;
            totalUs += cbUs;
            maxUs = std::max (maxUs, cbUs);
            ++callbacksDone;
            if (speed > 0 && b > t0 + (int64_t) ((double) (k + 1) * periodTicks))
            {
                // La moitié suivante est partie sans être prête. Cause : réveil tardif du fil du
                // pilote factice (minuterie de Windows) ou rappel de l'hôte trop long.
                ++xruns;
                if (wakeLateUs > cbUs) ++xrunsWake; else ++xrunsHost;
                if (xrunTimes.size() < 200) xrunTimes.push_back ({ (double) pos / rate, wakeLateUs, cbUs });
            }
            // Sorties -> câble simulé.
            for (auto& c : buffers)
            {
                if (c.input) continue;
                fromBytes (c.mem.data() + (size_t) idx * bytes * buffer, tmp.data());
                for (long i = 0; i < buffer; ++i) loop[(size_t) c.index * loopSize + ((uint64_t) (pos + i) & (loopSize - 1))] = tmp[(size_t) i];
            }
            pos += buffer;
            ++k;
        }
        if (timer) CloseHandle (timer);
        if (av) AvRevertMmThreadCharacteristics (av);
    }

    void writeReport()
    {
        if (reportPath.empty()) return;
        if (FILE* f = std::fopen (reportPath.c_str(), "wb"))
        {
            std::fprintf (f, "{\"callbacks\":%lld,\"xruns\":%lld,\"dropped\":%lld,\"max_callback_us\":%.3f,\"avg_callback_us\":%.3f,"
                             "\"buffer\":%ld,\"rate\":%.0f,\"output_ready_calls\":%lld,\"time_info\":%s,\"roundtrip_frames\":%ld,"
                             "\"xruns_driver_wake\":%lld,\"xruns_host_callback\":%lld,\"max_wake_late_us\":%.1f,\"xrun_list\":[",
                          (long long) callbacksDone, (long long) xruns, (long long) dropped, maxUs,
                          callbacksDone ? totalUs / (double) callbacksDone : 0.0, buffer, rate, (long long) outputReadyCalls.load(),
                          timeInfo ? "true" : "false", loopDelay, (long long) xrunsWake, (long long) xrunsHost, maxWakeLateUs);
            for (size_t i = 0; i < xrunTimes.size(); ++i)
                std::fprintf (f, "%s{\"t\":%.3f,\"wake_late_us\":%.1f,\"callback_us\":%.1f}", i ? "," : "", xrunTimes[i].t, xrunTimes[i].wake, xrunTimes[i].cb);
            std::fprintf (f, "]}");
            std::fclose (f);
        }
    }

    std::atomic<long> refs { 1 };
    std::map<std::string, std::string> cfg;
    long nIn = 8, nOut = 8, prefBuffer = 128, buffer = 128, extraIn = 24, extraOut = 24, bytes = 4, loopDelay = 0;
    double rate = 48000, speed = 1, dropEvery = 0, resetAfter = 0;
    bool resetSent = false;
    uint64_t seed = 1, loopSize = 0;
    ASIOSampleType type = ASIOSTInt32LSB;
    std::vector<bool> noiseMask;
    std::vector<Chan> buffers;
    std::vector<float> loop, tmp;
    ASIOCallbacks* callbacks = nullptr;
    bool timeInfo = false;
    std::thread th;
    std::atomic<bool> quit { false };
    bool running = false;
    std::atomic<int64_t> samplePos { 0 };
    int64_t callbacksDone = 0, xruns = 0, dropped = 0, xrunsWake = 0, xrunsHost = 0;
    double maxWakeLateUs = 0;
    struct XrunAt { double t, wake, cb; };
    std::vector<XrunAt> xrunTimes;
    std::atomic<int64_t> outputReadyCalls { 0 };
    double totalUs = 0, maxUs = 0;
    std::string reportPath, lastError;
};

class Factory : public IClassFactory
{
public:
    HRESULT STDMETHODCALLTYPE QueryInterface (REFIID riid, void** ppv) override
    {
        if (riid == IID_IUnknown || riid == IID_IClassFactory) { *ppv = this; AddRef(); return S_OK; }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE CreateInstance (IUnknown* outer, REFIID riid, void** ppv) override
    {
        if (outer) return CLASS_E_NOAGGREGATION;
        auto* d = new FakeAsio();
        const HRESULT hr = d->QueryInterface (riid, ppv);
        d->Release();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE LockServer (BOOL lock) override { lock ? ++gLocks : --gLocks; return S_OK; }
};

static Factory gFactory;

extern "C" HRESULT __stdcall DllGetClassObject (REFCLSID rclsid, REFIID riid, void** ppv)
{
    if (rclsid != CLSID_NovaFakeAsio) return CLASS_E_CLASSNOTAVAILABLE;
    return gFactory.QueryInterface (riid, ppv);
}

extern "C" HRESULT __stdcall DllCanUnloadNow() { return (gLocks == 0 && gObjects == 0) ? S_OK : S_FALSE; }
