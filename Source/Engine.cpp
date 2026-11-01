/*
    NovaAudioEngine — le moteur (voir Engine.h).
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
#include "Engine.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace nova::audio
{
    using json::Value;

    namespace
    {
        int64_t qpcNow() { LARGE_INTEGER c; QueryPerformanceCounter (&c); return c.QuadPart; }
        double qpcFreq() { static const double f = [] { LARGE_INTEGER x; QueryPerformanceFrequency (&x); return (double) x.QuadPart; }(); return f; }
        constexpr int kCaptureChunk = 4096;
        constexpr int kWriterChunk = 8192;
#if NOVA_AUDIO_PRO && ! NOVA_AUDIO_WITH_ASIO
        constexpr const char* kDriverLossKind = "carte";
#else
        constexpr const char* kDriverLossKind = "asio";   // nom historique, relu tel quel par le pont
#endif
        std::atomic<int> gShmCounter { 0 };

        void atomicMax (std::atomic<uint64_t>& a, uint64_t v)
        {
            uint64_t cur = a.load (std::memory_order_relaxed);
            while (v > cur && ! a.compare_exchange_weak (cur, v, std::memory_order_relaxed)) {}
        }
    }

    Engine::Engine()
    {
        captureEvent = CreateEventW (nullptr, FALSE, FALSE, nullptr);
        writerEvent = CreateEventW (nullptr, FALSE, FALSE, nullptr);
    }

    Engine::~Engine()
    {
        stop();
        retired.clear();
        chainOwned.reset();
        if (captureEvent) CloseHandle (captureEvent);
        if (writerEvent) CloseHandle (writerEvent);
    }

    //==========================================================================
    bool Engine::allocShared (std::string& err)
    {
        const size_t headerBytes = shm::align64 (sizeof (shm::Header));
        const uint32_t prevCap = nextPow2 ((uint64_t) projRate * 2);
        const uint32_t prevCh = (uint32_t) std::max (1, nIn);
        const uint32_t playCap = nextPow2 ((uint64_t) projRate);
        const uint32_t playCh = (uint32_t) std::max (1, nOut);
        const size_t prevOff = headerBytes;
        const size_t playOff = shm::align64 (prevOff + sizeof (float) * (size_t) prevCap * prevCh);
        const size_t total = shm::align64 (playOff + sizeof (float) * (size_t) playCap * playCh);

        uint8_t* base = nullptr;
        if (cfg.shared)
        {
            const int n = ++gShmCounter;
            shmName = "Local\\NovaAudio-" + std::to_string (GetCurrentProcessId()) + "-" + std::to_string (n);
            mapping = CreateFileMappingW (INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, (DWORD) ((uint64_t) total >> 32),
                                          (DWORD) (total & 0xFFFFFFFFu), widen (shmName).c_str());
            if (mapping == nullptr) { err = "mémoire partagée non créée"; return false; }
            view = (uint8_t*) MapViewOfFile (mapping, FILE_MAP_ALL_ACCESS, 0, 0, total);
            if (view == nullptr) { err = "mémoire partagée non projetée"; CloseHandle (mapping); mapping = nullptr; return false; }
            base = view;
        }
        else
        {
            localZone.assign (total, 0);
            base = localZone.data();
        }
        std::memset (base, 0, total);
        hdr = new (base) shm::Header();
        hdr->magic = shm::kMagic;
        hdr->version = shm::kVersion;
        hdr->headerSize = (uint32_t) sizeof (shm::Header);
        hdr->totalSize = total;
        hdr->preview.capacity = prevCap;
        hdr->preview.channels = prevCh;
        hdr->preview.dataOffset = prevOff;
        hdr->play.capacity = playCap;
        hdr->play.channels = playCh;
        hdr->play.dataOffset = playOff;
        preview.attach (&hdr->preview, (float*) (base + prevOff));
        play.attach (&hdr->play, (float*) (base + playOff));
        return true;
    }

    void Engine::lockRealtimeMemory()
    {
        // Tout ce que touchent le fil audio et le fil de capture reste en mémoire physique : un défaut
        // de page « dur » (page renvoyée sur le disque quand la mémoire manque) bloquerait le fil
        // temps réel plusieurs millisecondes. Ensemble de travail minimal relevé d'autant, puis
        // VirtualLock (sans effet si Windows refuse : le moteur marche quand même).
        struct Zone { const void* p; size_t n; };
        const Zone zones[] = {
            { view != nullptr ? (const void*) view : (const void*) localZone.data(), hdr ? (size_t) hdr->totalSize : 0 },
            { devData.data(), devData.size() * sizeof (float) },
            { projData.data(), projData.size() * sizeof (float) },
            { capInter.data(), capInter.size() * sizeof (float) },
            { capPlanarIn.data(), capPlanarIn.size() * sizeof (float) },
            { capPlanarOut.data(), capPlanarOut.size() * sizeof (float) },
            { capOutInter.data(), capOutInter.size() * sizeof (float) },
            { playInter.data(), playInter.size() * sizeof (float) },
            { playPlanarIn.data(), playPlanarIn.size() * sizeof (float) },
            { playPlanarOut.data(), playPlanarOut.size() * sizeof (float) },
        };
        size_t total = 0;
        for (auto& z : zones) total += z.n;
        SIZE_T mn = 0, mx = 0;
        GetProcessWorkingSetSize (GetCurrentProcess(), &mn, &mx);
        const SIZE_T want = mn + total + (64u << 20);
        SetProcessWorkingSetSize (GetCurrentProcess(), want, std::max (mx, want + (64u << 20)));
        lockedBytes = 0;
        for (auto& z : zones)
            if (z.p != nullptr && z.n > 0 && VirtualLock (const_cast<void*> (z.p), z.n)) lockedBytes += z.n;
    }

    void Engine::freeShared()
    {
        if (hdr) hdr->running = 0;
        hdr = nullptr;
        if (view) UnmapViewOfFile (view);
        if (mapping) CloseHandle (mapping);
        view = nullptr;
        mapping = nullptr;
        localZone.clear();
        shmName.clear();
    }

    Value Engine::layout() const
    {
        using H = shm::Header;
        auto o = Value::object();
        auto f = [&o] (const char* k, size_t v) { o.set (k, (double) v); };
        f ("header", sizeof (H));
        f ("magic", offsetof (H, magic));
        f ("version", offsetof (H, version));
        f ("totalSize", offsetof (H, totalSize));
        f ("running", offsetof (H, running));
        f ("projectRate", offsetof (H, projectRate));
        f ("deviceRate", offsetof (H, deviceRate));
        f ("blockFrames", offsetof (H, blockFrames));
        f ("inChannels", offsetof (H, inChannels));
        f ("outChannels", offsetof (H, outChannels));
        f ("inputLatency", offsetof (H, inputLatency));
        f ("outputLatency", offsetof (H, outputLatency));
        f ("playLookahead", offsetof (H, playLookahead));
        f ("clockSeq", offsetof (H, clockSeq));
        f ("clockFrame", offsetof (H, clockFrame));
        f ("clockTime", offsetof (H, clockTime));
        f ("callbacks", offsetof (H, callbacks));
        f ("deviceFrames", offsetof (H, deviceFrames));
        f ("lateCallbacks", offsetof (H, lateCallbacks));
        f ("deviceXruns", offsetof (H, deviceXruns));
        f ("deviceLostFrames", offsetof (H, deviceLostFrames));
        f ("captureLostFrames", offsetof (H, captureLostFrames));
        f ("playUnderruns", offsetof (H, playUnderruns));
        f ("playDroppedFrames", offsetof (H, playDroppedFrames));
        f ("playFrames", offsetof (H, playFrames));
        f ("cbLastNs", offsetof (H, cbLastNs));
        f ("cbMaxNs", offsetof (H, cbMaxNs));
        f ("cbTotalNs", offsetof (H, cbTotalNs));
        f ("playTargetBlocks", offsetof (H, playTargetBlocks));
        f ("playMinBlocks", offsetof (H, playMinBlocks));
        f ("monitorLatency", offsetof (H, monitorLatency));
        f ("inPeak", offsetof (H, inPeak));
        f ("outPeak", offsetof (H, outPeak));
        f ("preview", offsetof (H, preview));
        f ("play", offsetof (H, play));
        f ("ringWrite", offsetof (RingState, write));
        f ("ringClaim", offsetof (RingState, claim));
        f ("ringRead", offsetof (RingState, read));
        f ("ringCapacity", offsetof (RingState, capacity));
        f ("ringChannels", offsetof (RingState, channels));
        f ("ringDataOffset", offsetof (RingState, dataOffset));
        if (hdr) f ("total", hdr->totalSize);
        o.set ("magic_value", (double) shm::kMagic);
        o.set ("version_value", (double) shm::kVersion);
        o.set ("max_channels", shm::kMaxChannels);
        return o;
    }

    //==========================================================================
    bool Engine::start (const EngineConfig& c, Value& result, std::string& err)
    {
        stop();
        cfg = c;
        dev = createDevice (cfg.device, err);
        if (! dev) return false;
        DeviceRequest req;
        req.sampleRate = cfg.deviceRate > 0 ? cfg.deviceRate : cfg.projectRate;
        req.bufferFrames = cfg.buffer;
        req.inChannels = cfg.inChannels;
        req.outChannels = cfg.outChannels;
        if (! dev->open (req, err)) { dev.reset(); return false; }
        const auto& inf = dev->info();
        devRate = (int) std::lround (inf.sampleRate);
        projRate = cfg.projectRate > 0 ? cfg.projectRate : devRate;
        block = inf.bufferFrames;
        nIn = std::min (shm::kMaxChannels, inf.inChannels);
        nOut = std::min (shm::kMaxChannels, inf.outChannels);
        resampling = devRate != projRate;

        if (! allocShared (err)) { dev->close(); dev.reset(); return false; }

        // Anneau « carte » : 2 s ; anneau « projet » : captureSeconds (128 Mo au plus, 4 s au moins).
        const uint32_t inCh = (uint32_t) std::max (1, nIn);
        devState.~RingState();
        new (&devState) RingState();
        devState.capacity = nextPow2 ((uint64_t) devRate * 2);
        devState.channels = inCh;
        devData.assign ((size_t) devState.capacity * inCh, 0.0f);
        devRing.attach (&devState, devData.data());
        uint64_t projFrames = (uint64_t) (projRate * std::max (4.0, cfg.captureSeconds));
        projFrames = std::min<uint64_t> (projFrames, (128ull << 20) / (4ull * inCh));
        projState.~RingState();
        new (&projState) RingState();
        projState.capacity = nextPow2 (projFrames);
        projState.channels = inCh;
        projData.assign ((size_t) projState.capacity * inCh, 0.0f);
        projRing.attach (&projState, projData.data());

        // Rééchantillonneurs (aucune allocation ensuite).
        capRs.setup (devRate, projRate, (int) inCh, kCaptureChunk);
        const int capOutMax = (int) std::ceil ((double) kCaptureChunk * projRate / devRate) + 64;
        capInter.assign ((size_t) kCaptureChunk * inCh, 0.0f);
        capPlanarIn.assign ((size_t) kCaptureChunk * inCh, 0.0f);
        capPlanarOut.assign ((size_t) capOutMax * inCh, 0.0f);
        capOutInter.assign ((size_t) capOutMax * inCh, 0.0f);
        capInPtr.resize (inCh);
        capOutPtr.resize (inCh);
        for (uint32_t ch = 0; ch < inCh; ++ch)
        {
            capInPtr[ch] = &capPlanarIn[(size_t) ch * kCaptureChunk];
            capOutPtr[ch] = &capPlanarOut[(size_t) ch * capOutMax];
        }
        const uint32_t outCh = (uint32_t) std::max (1, nOut);
        playMaxNeed = (int) std::ceil ((double) block * projRate / devRate) + 8;
        playRs.setup (projRate, devRate, (int) outCh, playMaxNeed);
        playInter.assign ((size_t) playMaxNeed * outCh, 0.0f);
        playPlanarIn.assign ((size_t) playMaxNeed * outCh, 0.0f);
        playPlanarOut.assign ((size_t) block * outCh, 0.0f);
        playInPtr.resize (outCh);
        playOutPtr.resize (outCh);
        for (uint32_t ch = 0; ch < outCh; ++ch)
        {
            playInPtr[ch] = &playPlanarIn[(size_t) ch * playMaxNeed];
            playOutPtr[ch] = &playPlanarOut[(size_t) ch * block];
        }
        playRead = 0;
        playPrimed = false;
        playTarget = (uint32_t) std::max (1, cfg.playMinBlocks);
        // Marge maximale de la lecture : 24 blocs, et au moins 150 ms (petits tampons).
        playMaxBlocks = (uint32_t) std::max (24, (int) std::ceil (0.15 * devRate / block));
        devReadPos = 0;
        {
            std::lock_guard<std::mutex> l (lossLock);
            driverLosses.clear();
        }
        writerInter.assign ((size_t) kWriterChunk * inCh, 0.0f);
        probe.state = 0;

        hdr->projectRate = (uint32_t) projRate;
        hdr->deviceRate = (uint32_t) devRate;
        hdr->blockFrames = (uint32_t) block;
        hdr->inChannels = (uint32_t) nIn;
        hdr->outChannels = (uint32_t) nOut;
        repOut = inf.outputLatency;
        repIn = inf.inputLatency;
        if (cfg.measuredRoundtrip > 0)
        {
            // Compensation exacte : l'aller-retour mesuré remplace l'annoncé ; l'écart va à l'entrée
            // (c'est elle qui décale les prises), la sortie reste celle du pilote.
            repIn = std::max (0, cfg.measuredRoundtrip - repOut);
            if (repIn == 0) { repIn = cfg.measuredRoundtrip / 2; repOut = cfg.measuredRoundtrip - repIn; }
        }
        hdr->inputLatency = repIn;
        hdr->outputLatency = repOut;
        hdr->playLookahead = resampling ? playRs.lookahead() : 0;
        hdr->playTargetBlocks = playTarget;
        hdr->playMinBlocks = playTarget;
        lockRealtimeMemory();

        // Chaîne du retour : reconstruite pour ce tampon / cette fréquence si elle existait.
        if (chainSpec.isObject())
        {
            Value rep;
            std::string e2;
            auto patches = chainPatches;           // réglages Nova Tune reçus depuis : rejoués
            setMonitorChain (chainSpec, rep, e2);
            for (auto& m : patches) setMonitorParams (m, rep, e2);
        }

        quitThreads = false;
        captureThread = std::thread ([this] { captureLoop(); });
        writerThread = std::thread ([this] { writerLoop(); });
        running = true;
        hdr->running = 1;
        if (! dev->start (this, err))
        {
            stop();
            return false;
        }

        result = Value::object();
        result.set ("device", inf.toJson());
        result.set ("shm", shmName);
        result.set ("layout", layout());
        result.set ("project_rate", projRate);
        result.set ("device_rate", devRate);
        result.set ("buffer", block);
        result.set ("in_channels", nIn);
        result.set ("out_channels", nOut);
        result.set ("resampling", resampling);
        if (resampling)
        {
            auto r = Value::object();
            r.set ("capture_taps", capRs.tapsPerPhase());
            r.set ("capture_phases", capRs.upFactor());
            r.set ("play_taps", playRs.tapsPerPhase());
            r.set ("play_lookahead", playRs.lookahead());
            r.set ("passband_hz", capRs.passbandHz());
            result.set ("resampler", r);
        }
        result.set ("input_latency", repIn);
        result.set ("output_latency", repOut);
        result.set ("roundtrip_ms", 1000.0 * (repIn + repOut) / (double) devRate);
        result.set ("announced_roundtrip_ms", 1000.0 * (inf.inputLatency + inf.outputLatency) / (double) devRate);
        result.set ("latency_source", cfg.measuredRoundtrip > 0 ? "measured" : "announced");
        return true;
    }

    void Engine::stop()
    {
        recovering = false;         // arrêt demandé (ou réouverture) : plus de reprise en attente
        if (dev)
        {
            dev->stop();
            dev->close();
        }
        running = false;
        quitThreads = true;
        if (captureEvent) SetEvent (captureEvent);
        if (writerEvent) SetEvent (writerEvent);
        if (captureThread.joinable()) captureThread.join();
        if (writerThread.joinable()) writerThread.join();
        {
            std::lock_guard<std::mutex> l (takesLock);
            for (auto& t : takes)
                if (! t->done)
                {
                    if (t->error.empty()) t->error = "flux arrêté pendant la prise";
                    closeTake (*t);
                }
        }
        takesCv.notify_all();
        if (probe.state.load() != 0)
        {
            auto cb = std::move (probe.done);
            probe.state = 0;
            if (cb) { auto o = Value::object(); o.set ("error", "flux arrêté"); cb (o); }
        }
        // Chaîne : gardée (spécification) mais l'instance est libérée avec le flux.
        auto* c = chain.exchange (nullptr);
        (void) c;
        chainOwned.reset();
        retired.clear();
        auto* r = routes.exchange (nullptr);
        (void) r;
        freeShared();
        dev.reset();
    }

    //==========================================================================
    void Engine::setRoutes (const std::vector<Route>& rs, bool enabled)
    {
        auto set = std::make_unique<RouteSet>();
        for (auto& r : rs)
            if (set->n < 256 && r.in >= 0 && r.out >= 0 && r.gain > 0) set->r[set->n++] = r;
        auto* old = routes.exchange (set.get(), std::memory_order_acq_rel);
        routesEnabled = enabled;
        Retired ret;
        ret.afterCallback = hdr ? (int64_t) hdr->callbacks.load() + 2 : 0;
        if (old != nullptr && routesOwned.get() == old) ret.routes = std::move (routesOwned);
        routesOwned = std::move (set);
        if (ret.routes) retired.push_back (std::move (ret));
    }

    bool Engine::setMonitorChain (const Value& spec, Value& report, std::string& err)
    {
        if (! running && ! dev) { chainSpec = spec; chainPatches.clear(); report = Value::object(); report.set ("pending", true); return true; }
        auto c = MonitorChain::build (spec, (double) devRate, block, report, err);
        if (! c) return false;
        chainSpec = spec;
        chainPatches.clear();
        auto* old = chain.exchange (c.get(), std::memory_order_acq_rel);
        if (hdr) hdr->monitorLatency = (uint32_t) c->latency();
        Retired ret;
        ret.afterCallback = hdr ? (int64_t) hdr->callbacks.load() + 2 : 0;
        if (old != nullptr && chainOwned.get() == old) ret.chain = std::move (chainOwned);
        chainOwned = std::move (c);
        if (ret.chain) retired.push_back (std::move (ret));
        return true;
    }

    void Engine::clearMonitorChain()
    {
        chainSpec = Value();
        chainPatches.clear();
        auto* old = chain.exchange (nullptr, std::memory_order_acq_rel);
        if (hdr) hdr->monitorLatency = 0;
        if (old != nullptr && chainOwned.get() == old)
        {
            Retired ret;
            ret.afterCallback = hdr ? (int64_t) hdr->callbacks.load() + 2 : 0;
            ret.chain = std::move (chainOwned);
            retired.push_back (std::move (ret));
        }
    }

    bool Engine::setMonitorParams (const Value& m, Value& report, std::string& err)
    {
        const auto strip = m["strip"].asString();
        const auto& insert = m.has ("insert") ? m["insert"] : Value ("novatune");
        if (! chainOwned)
        {
            if (! chainSpec.isObject()) { err = "aucune chaîne de retour"; return false; }
            chainPatches.push_back (m);          // chaîne en attente du démarrage : appliqué à sa construction
            report = Value::object();
            report.set ("pending", true);
            return true;
        }
        if (! chainOwned->setInsertParams (strip, insert, m["params"], report, err)) return false;
        // Mémorisé sous sa forme complète (réglages effectifs) : un seul par insert, la liste reste courte.
        auto full = Value::object();
        full.set ("strip", strip);
        full.set ("insert", report["id"]);
        full.set ("params", report["params"]);
        chainPatches.erase (std::remove_if (chainPatches.begin(), chainPatches.end(), [&] (const Value& v)
            { return v["strip"].asString() == strip && v["insert"].asString() == report["id"].asString(); }), chainPatches.end());
        chainPatches.push_back (full);
        if (hdr) hdr->monitorLatency = (uint32_t) chainOwned->latency();
        return true;
    }

    int Engine::monitorLatency() const
    {
        auto* c = chain.load();
        return c ? c->latency() : 0;
    }

    Value Engine::housekeeping()
    {
        const int64_t cbs = hdr ? (int64_t) hdr->callbacks.load() : 0;
        const bool flowing = running && dev && cbs > 0;
        retired.erase (std::remove_if (retired.begin(), retired.end(), [&] (const Retired& r) { return ! flowing || cbs >= r.afterCallback; }), retired.end());
        if (recovering)
        {
            // Carte partie (débranchée, veille…) : on réessaie toutes les 0,5 s (puis 2 s), sans
            // redémarrer Nova ; dès qu'elle revient, le flux repart avec les mêmes réglages.
            if (nowMs() < nextRetryMs) return Value();
            Value res;
            std::string err;
            const auto c = cfg;
            ++retries;
            if (start (c, res, err))
            {
                recovering = false;
                ++recoveries;
                auto o = Value::object();
                o.set ("recovered", true);
                o.set ("attempts", retries);
                o.set ("restarted", res);
                retries = 0;
                return o;
            }
            cfg = c;
            recovering = true;
            nextRetryMs = nowMs() + (retries < 20 ? 500.0 : 2000.0);
            return Value();
        }
        if (! dev) return Value();
        const uint32_t ev = dev->events.exchange (0);
        if (ev == 0) return Value();
        if (ev & kEvLost)
        {
            auto o = Value::object();
            o.set ("lost", true);
            o.set ("recovering", true);
            const auto c = cfg;
            stop();                 // prises en cours fermées proprement (fichiers lisibles)
            cfg = c;
            recovering = true;
            retries = 0;
            nextRetryMs = nowMs() + 300.0;
            return o;
        }
        auto o = Value::object();
        if (ev & kEvEnded) o.set ("ended", true);
        if (ev & kEvReset) o.set ("reset", true);
        if (ev & kEvResync) o.set ("resync", true);
        if (ev & kEvLatency) o.set ("latency_changed", true);
        if (ev & kEvOverload) o.set ("overload", true);
        if (ev & kEvError) o.set ("error", true);
        if (ev & kEvDefaultChanged) o.set ("default_changed", true);
        if (ev & (kEvReset | kEvLatency | kEvDefaultChanged))
        {
            // Le pilote demande une réinitialisation (tampon ou fréquence changés dans son panneau,
            // horloge…) ou annonce d'autres latences : comme tout hôte ASIO, on rouvre le flux avec
            // les mêmes réglages. Les prises en cours sont fermées proprement (fichiers lisibles) ;
            // le pont reçoit la nouvelle zone partagée dans l'événement.
            Value res;
            std::string err;
            const auto c = cfg;
            if (start (c, res, err)) o.set ("restarted", res);
            else
            {
                o.set ("restart_error", err);
                cfg = c;
                recovering = true;          // on réessaiera tout seul
                retries = 0;
                nextRetryMs = nowMs() + 500.0;
                o.set ("recovering", true);
            }
        }
        return o;
    }

    //==========================================================================
    bool Engine::measureLatency (const std::vector<int>& outs, double seconds, float amp, std::function<void (Value)> done, std::string& err)
    {
        if (! running) { err = "flux arrêté"; return false; }
        int expected = 0;
        if (! probe.state.compare_exchange_strong (expected, 9)) { err = "une mesure est déjà en cours"; return false; }
        probe.nOuts = 0;
        for (int o : outs)
            if (probe.nOuts < 8 && o >= 0 && o < nOut) probe.outs[probe.nOuts++] = o;
        probe.amp = amp;
        probe.window = (int64_t) (std::max (0.05, seconds) * devRate);
        probe.peak.assign ((size_t) std::max (1, nIn), 0.0f);
        probe.peakAt.assign ((size_t) std::max (1, nIn), -1);
        probe.analysed = 0;
        probe.done = std::move (done);
        probe.emitFrame = (int64_t) devRing.written() + 4 * block;
        probe.state.store (1, std::memory_order_release);
        return true;
    }

    void Engine::analyseProbe (const float* inter, int64_t first, int frames)
    {
        const int st = probe.state.load (std::memory_order_acquire);
        if (st != 1 && st != 2) return;
        const int ch = (int) devRing.channels();
        const int64_t a = std::max (first, probe.emitFrame);
        const int64_t b = std::min (first + frames, probe.emitFrame + probe.window);
        for (int64_t f = a; f < b; ++f)
        {
            const float* fr = inter + (size_t) (f - first) * ch;
            for (int c = 0; c < nIn; ++c)
            {
                const float v = std::fabs (fr[c]);
                if (v > probe.peak[(size_t) c]) { probe.peak[(size_t) c] = v; probe.peakAt[(size_t) c] = f; }
            }
        }
        if (first + frames >= probe.emitFrame + probe.window)
        {
            auto o = Value::object();
            auto delays = Value::array();
            auto peaks = Value::array();
            for (int c = 0; c < nIn; ++c)
            {
                const bool ok = probe.peak[(size_t) c] >= 0.1f * probe.amp && probe.peakAt[(size_t) c] >= probe.emitFrame;
                if (ok) delays.push ((double) (probe.peakAt[(size_t) c] - probe.emitFrame));
                else delays.push (Value());
                peaks.push ((double) probe.peak[(size_t) c]);
            }
            o.set ("delays", delays);
            o.set ("peaks", peaks);
            o.set ("sample_rate", devRate);
            o.set ("emit_frame", (double) probe.emitFrame);
            o.set ("emitted", st == 2);
            auto cb = std::move (probe.done);
            probe.state.store (0, std::memory_order_release);
            if (cb) cb (o);
        }
    }

    //==========================================================================
    // FIL AUDIO
    void Engine::audioBlock (const float* const* in, int numIn, float* const* out, int numOut, int frames, const BlockTime& t) noexcept
    {
        const int64_t t0 = qpcNow();
        numIn = std::min (numIn, nIn);
        numOut = std::min (numOut, nOut);

        // Pertes de la carte : silence à leur place (borné), la suite reste calée.
        if (t.lostBefore > 0)
        {
            int64_t left = std::min<int64_t> (t.lostBefore, devRing.capacity() / 2);
            const int64_t at = (int64_t) devRing.written();
            while (left > 0)
            {
                const int n = (int) std::min<int64_t> (left, 8192);
                devRing.writeSilence (n);
                left -= n;
            }
            lossQueue.push ({ at, std::min<int64_t> (t.lostBefore, devRing.capacity() / 2), kDriverLossKind });
            hdr->deviceLostFrames.fetch_add ((uint64_t) t.lostBefore, std::memory_order_relaxed);
        }

        const int64_t blockStart = (int64_t) devRing.written();
        devRing.writePlanar (in, numIn, frames);
        lastBlockFrame.store (blockStart, std::memory_order_relaxed);
        lastBlockMs.store (t.hostMs, std::memory_order_relaxed);

        renderPlayback (out, numOut, frames);

        if (routesEnabled.load (std::memory_order_relaxed))
            if (const auto* rs = routes.load (std::memory_order_acquire))
                for (int k = 0; k < rs->n; ++k)
                {
                    const auto& r = rs->r[k];
                    if (r.in < numIn && r.out < numOut && in[r.in] != nullptr)
                        for (int i = 0; i < frames; ++i) out[r.out][i] += in[r.in][i] * r.gain;
                }

        if (auto* c = chain.load (std::memory_order_acquire))
            c->process (in, numIn, out, numOut, frames);

        if (cfg.stress > 0)
        {
            // Charge de calcul simulée (banc, assistant du mode pro) : comme une séance chargée.
            const int64_t until = t0 + (int64_t) (cfg.stress * frames / devRate * qpcFreq());
            volatile float acc = 0;
            while (qpcNow() < until)
                for (int i = 0; i < 64; ++i) acc = acc * 0.999f + (float) i;
        }

        // Impulsion de mesure.
        if (probe.state.load (std::memory_order_acquire) == 1)
        {
            const int64_t e = probe.emitFrame - blockStart;
            if (e >= 0 && e < frames)
            {
                for (int k = 0; k < probe.nOuts; ++k)
                    if (probe.outs[k] < numOut) out[probe.outs[k]][e] += probe.amp;
                probe.state.store (2, std::memory_order_release);
            }
            else if (e < 0)
                probe.emitFrame = blockStart + frames;   // armée trop tard : au bloc suivant
        }

        for (int c = 0; c < numIn; ++c)
        {
            float m = 0;
            for (int i = 0; i < frames; ++i) m = std::max (m, std::fabs (in[c][i]));
            if (m > hdr->inPeak[c]) hdr->inPeak[c] = m;
        }
        for (int c = 0; c < numOut; ++c)
        {
            float m = 0;
            for (int i = 0; i < frames; ++i) m = std::max (m, std::fabs (out[c][i]));
            if (m > hdr->outPeak[c]) hdr->outPeak[c] = m;
        }

        hdr->callbacks.fetch_add (1, std::memory_order_relaxed);
        hdr->deviceFrames.fetch_add ((uint64_t) frames, std::memory_order_relaxed);
        const uint64_t ns = (uint64_t) ((double) (qpcNow() - t0) * 1e9 / qpcFreq());
        hdr->cbLastNs.store (ns, std::memory_order_relaxed);
        hdr->cbTotalNs.fetch_add (ns, std::memory_order_relaxed);
        atomicMax (hdr->cbMaxNs, ns);
        if ((double) ns > 1e9 * frames / devRate) hdr->lateCallbacks.fetch_add (1, std::memory_order_relaxed);
        hdr->deviceXruns.store (dev ? dev->xruns.load (std::memory_order_relaxed) : 0, std::memory_order_relaxed);
        SetEvent (captureEvent);
    }

    void Engine::renderPlayback (float* const* out, int numOut, int frames) noexcept
    {
        const uint64_t w = play.written();
        uint64_t r = playRead;
        const uint64_t avail = w - r;
        const int need = resampling ? playRs.inputNeeded (frames) : frames;
        const uint64_t perBlock = (uint64_t) std::max (1, (int) std::lround ((double) frames * projRate / devRate));
        const uint64_t target = (uint64_t) playTarget * perBlock;
        if (! playPrimed)
        {
            if (avail >= std::max<uint64_t> (target, (uint64_t) need)) playPrimed = true;
            else return;
        }
        if (avail < (uint64_t) need)
        {
            // File vidée en pleine lecture : le pont est en retard. Marge agrandie (comme le pont Python).
            playPrimed = false;
            playTarget = std::min<uint32_t> (playMaxBlocks, playTarget + 2);
            hdr->playTargetBlocks.store (playTarget, std::memory_order_relaxed);
            hdr->playUnderruns.fetch_add (1, std::memory_order_relaxed);
            return;
        }
        if (! resampling)
            play.peekPlanar (r, out, numOut, frames, 0);
        else if (need <= playMaxNeed)
        {
            const int ch = (int) play.channels();
            play.peekInterleaved (r, playInter.data(), need);
            for (int c = 0; c < ch; ++c)
                for (int i = 0; i < need; ++i) playInPtr[(size_t) c][i] = playInter[(size_t) i * ch + c];
            const int got = playRs.process (playInPtr.data(), need, playOutPtr.data(), frames);
            for (int c = 0; c < std::min (ch, numOut); ++c)
                for (int i = 0; i < got; ++i) out[c][i] += playOutPtr[(size_t) c][i];
        }
        r += (uint64_t) need;
        hdr->playFrames.fetch_add ((uint64_t) need, std::memory_order_relaxed);
        const uint64_t left = avail - (uint64_t) need;
        if (left > target + 4 * perBlock)
        {
            if (playTarget < playMaxBlocks)
            {
                // Le pont envoie par paquets : la marge grandit d'abord (aucune image jetée), comme
                // après un trou ; au-delà de la marge maximale (150 ms) seulement, on jette.
                playTarget = std::min<uint32_t> (playMaxBlocks, playTarget + 2);
                hdr->playTargetBlocks.store (playTarget, std::memory_order_relaxed);
            }
            else
            {
                const uint64_t drop = left - target;
                r += drop;
                hdr->playDroppedFrames.fetch_add (drop, std::memory_order_relaxed);
            }
        }
        playRead = r;
        play.state()->read.store (r, std::memory_order_release);
    }

    //==========================================================================
    // FIL DE CAPTURE
    int64_t Engine::projectFrameOf (int64_t d) const
    {
        return resampling ? (int64_t) std::llround ((double) d * projRate / devRate) : d;
    }

    void Engine::publishClock (int64_t projFrame, double timeSec)
    {
        hdr->clockSeq.fetch_add (1, std::memory_order_acq_rel);
        hdr->clockFrame = (double) projFrame;
        hdr->clockTime = timeSec;
        hdr->clockSeq.fetch_add (1, std::memory_order_acq_rel);
    }

    void Engine::captureLoop()
    {
        SetThreadPriority (GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        const int ch = (int) devRing.channels();
        std::vector<float> zeros ((size_t) kCaptureChunk * ch, 0.0f);
        auto emit = [&] (const float* inter, int n)
        {
            if (! resampling)
            {
                projRing.writeInterleaved (inter, n);
                preview.writeInterleaved (inter, n);
                return;
            }
            for (int c = 0; c < ch; ++c)
                for (int i = 0; i < n; ++i) capInPtr[(size_t) c][i] = inter[(size_t) i * ch + c];
            const int capOutMax = (int) (capPlanarOut.size() / (size_t) ch);
            const int got = capRs.process (capInPtr.data(), n, capOutPtr.data(), capOutMax);
            for (int i = 0; i < got; ++i)
                for (int c = 0; c < ch; ++c) capOutInter[(size_t) i * ch + c] = capOutPtr[(size_t) c][i];
            projRing.writeInterleaved (capOutInter.data(), got);
            preview.writeInterleaved (capOutInter.data(), got);
        };

        while (! quitThreads.load())
        {
            WaitForSingleObject (captureEvent, 10);
            for (;;)
            {
                uint64_t skipped = 0;
                const uint64_t before = devReadPos;
                const int n = devRing.readOverwrite (devReadPos, capInter.data(), kCaptureChunk, skipped);
                if (skipped > 0)
                {
                    // Le fil de capture a pris trop de retard (anneau de 2 s réécrit) : silence à la place.
                    hdr->captureLostFrames.fetch_add (skipped, std::memory_order_relaxed);
                    {
                        std::lock_guard<std::mutex> l (lossLock);
                        driverLosses.push_back ({ projectFrameOf ((int64_t) before), projectFrameOf ((int64_t) skipped), "tampon" });
                    }
                    uint64_t left = skipped;
                    while (left > 0)
                    {
                        const int k = (int) std::min<uint64_t> (left, kCaptureChunk);
                        emit (zeros.data(), k);
                        left -= (uint64_t) k;
                    }
                }
                Loss l;
                while (lossQueue.pop (l))
                {
                    std::lock_guard<std::mutex> lk (lossLock);
                    driverLosses.push_back ({ projectFrameOf (l.frame), projectFrameOf (l.frames), l.kind });
                }
                if (n <= 0) break;
                analyseProbe (capInter.data(), (int64_t) devReadPos - n, n);
                emit (capInter.data(), n);
            }
            const double ms = lastBlockMs.load (std::memory_order_relaxed);
            if (ms > 0 && dev)
                publishClock (projectFrameOf (lastBlockFrame.load (std::memory_order_relaxed)),
                              ms / 1000.0 - (double) dev->info().inputLatency / devRate);
            SetEvent (writerEvent);
        }
    }

    //==========================================================================
    // PRISES
    bool Engine::recStart (const Value& a, Value& result, std::string& err)
    {
        if (! running) { err = "flux arrêté"; return false; }
        auto t = std::make_unique<Take>();
        t->id = a["take_id"].asString();
        if (t->id.empty()) { err = "take_id manquant"; return false; }
        const auto& files = a["files"];
        if (files.size() == 0) { err = "aucune piste à enregistrer"; return false; }
        for (size_t i = 0; i < files.size(); ++i)
        {
            const auto& f = files.at (i);
            auto tf = std::make_unique<TakeFile>();
            tf->path = widen (f["path"].asString());
            tf->trackId = f["track_id"].asString (std::to_string (i));
            for (size_t k = 0; k < f["inputs"].size(); ++k) tf->inputs.push_back (f["inputs"].at (k).asInt (0));
            const auto mode = f["mode"].asString (tf->inputs.size() == 2 ? "stereo" : "mono");
            tf->mode = mode == "stereo" ? 1 : (mode == "sum" ? 2 : 0);
            if (tf->inputs.empty() || (tf->mode == 1 && tf->inputs.size() != 2)) { err = "entrées invalides"; return false; }
            for (int c : tf->inputs) if (c < 0 || c >= shm::kMaxChannels) { err = "entrée hors de la carte"; return false; }
            if (! tf->wav.open (tf->path, tf->mode == 1 ? 2 : 1, projRate, err)) return false;
            tf->buf.assign ((size_t) kWriterChunk * (tf->mode == 1 ? 2 : 1), 0.0f);
            t->files.push_back (std::move (tf));
        }
        const uint64_t w = projRing.written();
        const uint64_t margin = (uint64_t) projRate;    // 1 s de marge avant l'écrasement
        const uint64_t keep = projRing.capacity() > margin ? projRing.capacity() - margin : projRing.capacity() / 2;
        const int64_t oldest = (int64_t) (w > keep ? w - keep : 0);
        const double preroll = std::max (0.0, std::min (3.0, a["preroll"].asDouble (1.0)));
        int64_t start = a.has ("start_frame") ? a["start_frame"].asInt64 (-1) : -1;
        if (start < 0) start = (int64_t) w - (int64_t) (preroll * projRate);
        start = std::max (start, oldest);
        t->start = t->next = start;
        t->lastHeader = t->lastSync = nowMs();
        result = Value::object();
        result.set ("take_id", t->id);
        result.set ("start_frame", (double) start);
        result.set ("now_frame", (double) w);
        result.set ("sample_rate", projRate);
        result.set ("device_rate", devRate);
        {
            std::lock_guard<std::mutex> l (takesLock);
            for (auto& o : takes)
                if (o->id == t->id) { err = "prise déjà commencée"; return false; }
            t->driverLossSeen = 0;
            takes.push_back (std::move (t));
        }
        SetEvent (writerEvent);
        return true;
    }

    void Engine::writeTake (Take& t)
    {
        const uint64_t w = projRing.written();
        const int64_t limit = t.stopAt >= 0 ? std::min<int64_t> ((int64_t) w, t.stopAt) : (int64_t) w;
        const int ch = (int) projRing.channels();
        auto writeFrames = [&] (const float* inter, int n)
        {
            for (auto& f : t.files)
            {
                const int width = f->mode == 1 ? 2 : 1;
                float* o = f->buf.data();
                for (int i = 0; i < n; ++i)
                {
                    const float* fr = inter + (size_t) i * ch;
                    auto col = [&] (int c) { return c < ch ? fr[c] : 0.0f; };
                    if (f->mode == 1) { o[2 * i] = col (f->inputs[0]); o[2 * i + 1] = col (f->inputs[1]); }
                    else if (f->mode == 2) { float acc = 0; for (int c : f->inputs) acc += col (c); o[i] = acc; }
                    else o[i] = col (f->inputs[0]);
                }
                if (! f->wav.write (o, n) && t.error.empty()) t.error = f->wav.error();
                (void) width;
            }
        };
        while (t.next < limit)
        {
            const int n = (int) std::min<int64_t> (limit - t.next, kWriterChunk);
            uint64_t pos = (uint64_t) t.next, skipped = 0;
            const int got = projRing.readOverwrite (pos, writerInter.data(), n, skipped);
            if (skipped > 0)
            {
                // Fil d'écriture trop en retard (disque) : silence à la place, la suite reste calée.
                t.losses.push_back ({ t.next, (int64_t) skipped, "tampon" });
                std::fill (writerInter.begin(), writerInter.end(), 0.0f);
                uint64_t left = skipped;
                while (left > 0)
                {
                    const int k = (int) std::min<uint64_t> (left, kWriterChunk);
                    writeFrames (writerInter.data(), k);
                    left -= (uint64_t) k;
                }
                t.next += (int64_t) skipped;
                continue;
            }
            if (got <= 0) break;
            writeFrames (writerInter.data(), got);
            t.next += got;
        }
        // Pertes de la carte (ou du fil de capture) tombées dans la prise.
        std::lock_guard<std::mutex> ll (lossLock);
        for (; t.driverLossSeen < driverLosses.size(); ++t.driverLossSeen)
        {
            const auto& l = driverLosses[t.driverLossSeen];
            if (l.frame + l.frames > t.start && (t.stopAt < 0 || l.frame < t.stopAt))
                t.losses.push_back (l);
        }
        const double now = nowMs();
        if (now - t.lastHeader >= 500)
        {
            const bool sync = now - t.lastSync >= 2000;
            t.lastHeader = now;
            if (sync) t.lastSync = now;
            for (auto& f : t.files) f->wav.updateHeader (sync);
        }
    }

    void Engine::closeTake (Take& t)
    {
        // Le fil d'écriture a pu aller plus loin que la fin demandée (elle arrive après coup) :
        // chaque fichier est coupé exactement à [start, stopAt).
        if (t.stopAt >= 0 && t.next > t.stopAt)
        {
            for (auto& f : t.files)
                if (! f->wav.truncateFrames ((uint64_t) (t.stopAt - t.start)) && t.error.empty()) t.error = f->wav.error();
            t.next = t.stopAt;
        }
        for (auto& f : t.files)
            if (! f->wav.close() && t.error.empty()) t.error = f->wav.error();
        t.done = true;
    }

    void Engine::writerLoop()
    {
        SetThreadPriority (GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        while (! quitThreads.load())
        {
            WaitForSingleObject (writerEvent, 20);
            bool finished = false;
            std::lock_guard<std::mutex> l (takesLock);
            for (auto& t : takes)
            {
                if (t->done) continue;
                writeTake (*t);
                if (t->stopAt >= 0 && t->next >= t->stopAt) { closeTake (*t); finished = true; }
            }
            if (finished) takesCv.notify_all();
        }
    }

    bool Engine::recStop (const std::string& id, int64_t endFrame, Value& result, std::string& err, int timeoutMs)
    {
        std::unique_lock<std::mutex> l (takesLock);
        Take* t = nullptr;
        for (auto& o : takes) if (o->id == id) t = o.get();
        if (t == nullptr) { err = "prise inconnue du moteur"; return false; }
        if (! t->done && t->stopAt < 0)
        {
            const int64_t now = (int64_t) projRing.written();
            t->stopAt = std::max (t->start, endFrame >= 0 ? endFrame : now);
        }
        SetEvent (writerEvent);
        takesCv.wait_for (l, std::chrono::milliseconds (timeoutMs), [t] { return t->done; });
        if (! t->done) { err = "écriture de la prise pas finie à temps"; return false; }
        result = Value::object();
        result.set ("take_id", t->id);
        result.set ("start_frame", (double) t->start);
        result.set ("end_frame", (double) t->next);
        result.set ("sample_rate", projRate);
        auto files = Value::array();
        for (auto& f : t->files)
        {
            auto o = Value::object();
            o.set ("track_id", f->trackId);
            o.set ("path", narrow (f->path));
            o.set ("frames", (double) f->wav.frames());
            o.set ("channels", f->mode == 1 ? 2 : 1);
            files.push (o);
        }
        result.set ("files", files);
        auto losses = Value::array();
        int64_t lost = 0;
        for (auto& x : t->losses)
        {
            auto o = Value::object();
            o.set ("frame", (double) x.frame);
            o.set ("frames", (double) x.frames);
            o.set ("kind", x.kind);
            losses.push (o);
            lost += x.frames;
        }
        result.set ("losses", losses);
        result.set ("lost_frames", (double) lost);
        if (! t->error.empty()) result.set ("error", t->error);
        takes.erase (std::remove_if (takes.begin(), takes.end(), [t] (const std::unique_ptr<Take>& p) { return p.get() == t; }), takes.end());
        return true;
    }

    Value Engine::recStatus() const
    {
        auto arr = Value::array();
        std::lock_guard<std::mutex> l (takesLock);
        for (auto& t : takes)
        {
            auto o = Value::object();
            o.set ("take_id", t->id);
            o.set ("start_frame", (double) t->start);
            o.set ("written_to", (double) t->next);
            o.set ("done", t->done);
            o.set ("losses", (double) t->losses.size());
            arr.push (o);
        }
        return arr;
    }

    //==========================================================================
    Value Engine::stats() const
    {
        auto o = Value::object();
        o.set ("running", running.load());
        o.set ("recovering", recovering);
        o.set ("recoveries", (double) recoveries);
        if (! hdr || ! dev) return o;
        const auto& inf = dev->info();
        const double cbs = (double) hdr->callbacks.load();
        o.set ("device", inf.name);
        o.set ("device_type", inf.type);
        o.set ("project_rate", projRate);
        o.set ("device_rate", devRate);
        o.set ("buffer", block);
        o.set ("in_channels", nIn);
        o.set ("out_channels", nOut);
        o.set ("resampling", resampling);
        o.set ("input_latency", repIn);
        o.set ("output_latency", repOut);
        o.set ("latency_source", cfg.measuredRoundtrip > 0 ? "measured" : "announced");
        o.set ("callbacks", cbs);
        o.set ("device_frames", (double) hdr->deviceFrames.load());
        o.set ("late_callbacks", (double) hdr->lateCallbacks.load());
        o.set ("device_xruns", (double) dev->xruns.load());
        o.set ("device_lost_frames", (double) hdr->deviceLostFrames.load());
        o.set ("capture_lost_frames", (double) hdr->captureLostFrames.load());
        o.set ("play_underruns", (double) hdr->playUnderruns.load());
        o.set ("play_dropped_frames", (double) hdr->playDroppedFrames.load());
        o.set ("play_target_blocks", (double) hdr->playTargetBlocks.load());
        o.set ("callback_last_us", (double) hdr->cbLastNs.load() / 1000.0);
        o.set ("callback_max_us", (double) hdr->cbMaxNs.load() / 1000.0);
        o.set ("callback_avg_us", cbs > 0 ? (double) hdr->cbTotalNs.load() / cbs / 1000.0 : 0.0);
        o.set ("block_us", 1e6 * block / devRate);
        o.set ("monitor_latency", monitorLatency());
        o.set ("project_frames", (double) projRing.written());
        if (auto* c = chain.load()) o.set ("inserts", c->stats());
        o.set ("device_stats", dev->extraStats());
        o.set ("loss_queue_dropped", (double) lossQueue.dropped.load());
        o.set ("locked_bytes", (double) lockedBytes);
        return o;
    }
}
