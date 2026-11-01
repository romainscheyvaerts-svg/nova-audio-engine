/*
    NovaAudioEngine — périphériques « nul » et « fichier », fabrique, outils temps réel.
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
#include "WavFile.h"

#include <avrt.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>

namespace nova::audio
{
    using json::Value;

    //==========================================================================
    Value DeviceInfo::toJson() const
    {
        auto o = Value::object();
        o.set ("type", type);
        o.set ("name", name);
        o.set ("format", format);
        o.set ("sample_rate", sampleRate);
        o.set ("buffer", bufferFrames);
        o.set ("in_channels", inChannels);
        o.set ("out_channels", outChannels);
        o.set ("input_latency", inputLatency);
        o.set ("output_latency", outputLatency);
        o.set ("min_buffer", minBuffer);
        o.set ("max_buffer", maxBuffer);
        o.set ("preferred_buffer", preferredBuffer);
        o.set ("granularity", granularity);
        o.set ("output_ready", outputReady);
        auto names = [] (const std::vector<std::string>& v) { auto a = Value::array(); for (auto& s : v) a.push (s); return a; };
        o.set ("in_names", names (inNames));
        o.set ("out_names", names (outNames));
        auto r = Value::array();
        for (double x : rates) r.push (x);
        o.set ("rates", r);
        return o;
    }

    double DeviceSpec::optNum (const std::string& k, double def) const
    {
        auto it = opts.find (k);
        if (it == opts.end() || it->second.empty()) return def;
        char* end = nullptr;
        const double d = std::strtod (it->second.c_str(), &end);
        return end != it->second.c_str() ? d : def;
    }

    DeviceSpec parseSpec (const std::string& spec)
    {
        DeviceSpec s;
        std::string head = spec, query;
        // Les chemins Windows ne contiennent pas « ? » : la requête commence au premier « ? ».
        const auto q = spec.find ('?');
        if (q != std::string::npos) { head = spec.substr (0, q); query = spec.substr (q + 1); }
        const auto colon = head.find (':');
        // « asio-dll:C:\x.dll » : le type s'arrête au premier « : » ; un nom de pilote seul = ASIO.
        if (colon != std::string::npos && colon > 1)
        {
            s.type = head.substr (0, colon);
            s.name = head.substr (colon + 1);
        }
        else if (head == "null" || head == "file" || head == "wasapi" || head == "pro")
            s.type = head;
        else
        {
#if NOVA_AUDIO_PRO && ! NOVA_AUDIO_WITH_ASIO
            s.type = "?";              // édition pro : pas de pilote tiers par son seul nom
#else
            s.type = "asio";
#endif
            s.name = head;
        }
        size_t i = 0;
        while (i <= query.size() && ! query.empty())
        {
            auto amp = query.find ('&', i);
            if (amp == std::string::npos) amp = query.size();
            const auto kv = query.substr (i, amp - i);
            const auto eq = kv.find ('=');
            if (! kv.empty()) s.opts[kv.substr (0, eq)] = eq == std::string::npos ? "1" : kv.substr (eq + 1);
            i = amp + 1;
            if (amp == query.size()) break;
        }
        return s;
    }

    //==========================================================================
    namespace
    {
        thread_local HANDLE tlsAvrt = nullptr;
        thread_local bool tlsEntered = false;
    }

    void enterProAudioThread()
    {
        if (tlsEntered) return;
        tlsEntered = true;
        DWORD task = 0;
        tlsAvrt = AvSetMmThreadCharacteristicsW (L"Pro Audio", &task);
        if (tlsAvrt != nullptr) AvSetMmThreadPriority (tlsAvrt, AVRT_PRIORITY_HIGH);
        else SetThreadPriority (GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    }

    void leaveProAudioThread()
    {
        if (tlsAvrt != nullptr) AvRevertMmThreadCharacteristics (tlsAvrt);
        tlsAvrt = nullptr;
        tlsEntered = false;
    }

    PreciseWaiter::PreciseWaiter()
    {
        timer = CreateWaitableTimerExW (nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (timer == nullptr) timer = CreateWaitableTimerW (nullptr, FALSE, nullptr);
    }
    PreciseWaiter::~PreciseWaiter() { if (timer) CloseHandle (timer); }

    double PreciseWaiter::freq()
    {
        static const double f = [] { LARGE_INTEGER x; QueryPerformanceFrequency (&x); return (double) x.QuadPart; }();
        return f;
    }
    int64_t PreciseWaiter::now()
    {
        LARGE_INTEGER c;
        QueryPerformanceCounter (&c);
        return c.QuadPart;
    }

    void PreciseWaiter::waitUntil (int64_t target)
    {
        const double f = freq();
        for (;;)
        {
            const int64_t t = now();
            const double remainUs = (double) (target - t) * 1e6 / f;
            if (remainUs <= 0) return;
            if (remainUs > 50 && timer != nullptr)
            {
                // Minuterie haute résolution jusqu'à l'échéance : jamais de boucle à vide (un fil
                // MMCSS qui consomme tout son quantum est bridé par Windows).
                LARGE_INTEGER due;
                due.QuadPart = -(LONGLONG) (remainUs * 10.0);   // 100 ns, relatif
                if (SetWaitableTimerEx (timer, &due, 0, nullptr, nullptr, nullptr, 0))
                    WaitForSingleObject (timer, INFINITE);
                return;
            }
            SwitchToThread();
        }
    }

    //==========================================================================
    // Périphériques simulés : fil cadencé commun.
    class ClockedDevice : public AudioDevice
    {
    public:
        explicit ClockedDevice (const DeviceSpec& s) : spec (s) {}
        ~ClockedDevice() override { stop(); }

        const DeviceInfo& info() const override { return inf; }

        bool start (AudioCallback* c, std::string& err) override
        {
            if (running) return true;
            if (! prepareStart (err)) return false;
            cb = c;
            quit = false;
            running = true;
            th = std::thread ([this] { run(); });
            return true;
        }

        void stop() override
        {
            quit = true;
            if (th.joinable()) th.join();
            running = false;
        }

        void close() override { stop(); }

        Value extraStats() const override
        {
            auto o = Value::object();
            o.set ("callbacks", (double) blocks.load());
            o.set ("late_blocks", (double) xruns.load());
            o.set ("max_lateness_us", maxLatenessUs.load());
            return o;
        }

    protected:
        virtual bool prepareStart (std::string&) { return true; }
        virtual bool fillInput (int64_t frame, int frames) = 0;     // faux : fin
        virtual void consumeOutput (int frames) { (void) frames; }

        void allocate()
        {
            const int n = inf.bufferFrames;
            inData.assign ((size_t) std::max (1, inf.inChannels) * (size_t) n, 0.0f);
            outData.assign ((size_t) std::max (1, inf.outChannels) * (size_t) n, 0.0f);
            inPtr.resize ((size_t) std::max (1, inf.inChannels));
            outPtr.resize ((size_t) std::max (1, inf.outChannels));
            for (int c = 0; c < inf.inChannels; ++c) inPtr[(size_t) c] = &inData[(size_t) c * n];
            for (int c = 0; c < inf.outChannels; ++c) outPtr[(size_t) c] = &outData[(size_t) c * n];
        }

        void run()
        {
            enterProAudioThread();
            PreciseWaiter waiter;
            const double speed = std::max (0.0, spec.optNum ("speed", defaultSpeed));
            const double f = PreciseWaiter::freq();
            const double periodTicks = speed > 0 ? (double) inf.bufferFrames / inf.sampleRate * f / speed : 0.0;
            const int64_t t0 = PreciseWaiter::now();
            int64_t k = 0, frame = 0;
            while (! quit)
            {
                if (speed > 0)
                {
                    const int64_t due = t0 + (int64_t) ((double) k * periodTicks);
                    waiter.waitUntil (due);
                }
                if (! fillInput (frame, inf.bufferFrames)) { events.fetch_or (kEvEnded); break; }
                for (auto& o : outData) o = 0.0f;
                BlockTime bt;
                bt.deviceFrame = frame;
                bt.hostMs = nowMs();
                cb->audioBlock (inPtr.data(), inf.inChannels, outPtr.data(), inf.outChannels, inf.bufferFrames, bt);
                consumeOutput (inf.bufferFrames);
                if (speed > 0)
                {
                    // Échéance : le bloc suivant doit pouvoir partir à l'heure (sinon une vraie carte craquerait).
                    const int64_t deadline = t0 + (int64_t) ((double) (k + 1) * periodTicks);
                    const int64_t late = PreciseWaiter::now() - deadline;
                    if (late > 0)
                    {
                        xruns.fetch_add (1);
                        const double us = (double) late * 1e6 / f;
                        if (us > maxLatenessUs.load()) maxLatenessUs.store (us);
                    }
                }
                ++k;
                frame += inf.bufferFrames;
                blocks.store ((uint64_t) k);
            }
            leaveProAudioThread();
        }

        DeviceSpec spec;
        DeviceInfo inf;
        double defaultSpeed = 1.0;
        AudioCallback* cb = nullptr;
        std::thread th;
        std::atomic<bool> quit { false };
        bool running = false;
        std::vector<float> inData, outData;
        std::vector<float*> inPtr, outPtr;
        std::atomic<uint64_t> blocks { 0 };
        std::atomic<double> maxLatenessUs { 0 };
    };

    //==========================================================================
    class NullDevice : public ClockedDevice
    {
    public:
        explicit NullDevice (const DeviceSpec& s) : ClockedDevice (s) {}

        bool open (const DeviceRequest& r, std::string&) override
        {
            inf.type = "null";
            inf.name = "Périphérique nul";
            inf.format = "float32";
            inf.sampleRate = spec.optNum ("rate", r.sampleRate);
            inf.bufferFrames = std::max (16, r.bufferFrames);
            inf.inChannels = r.inChannels > 0 ? r.inChannels : (int) spec.optNum ("inputs", 2);
            inf.outChannels = r.outChannels > 0 ? r.outChannels : (int) spec.optNum ("outputs", 2);
            inf.minBuffer = 16; inf.maxBuffer = 8192; inf.preferredBuffer = inf.bufferFrames; inf.granularity = 1;
            inf.rates = { 44100, 48000, 88200, 96000 };
            for (int c = 0; c < inf.inChannels; ++c) inf.inNames.push_back ("Entrée " + std::to_string (c + 1));
            for (int c = 0; c < inf.outChannels; ++c) inf.outNames.push_back ("Sortie " + std::to_string (c + 1));
            mode = spec.opt ("input", "silence");
            seed = (uint64_t) spec.optNum ("seed", 1);
            // Boucle sortie k -> entrée k (câble simulé) : aller-retour = 2 tampons + « extra ».
            if (spec.opt ("loopback") == "1")
            {
                loopDelay = 2 * inf.bufferFrames + (int) spec.optNum ("extra", 0);
                inf.inputLatency = inf.bufferFrames + loopDelay / 2 - inf.bufferFrames;
                inf.outputLatency = loopDelay - inf.inputLatency;
                loopSize = nextPow2Local ((uint64_t) loopDelay + 4 * (uint64_t) inf.bufferFrames);
                loop.assign ((size_t) loopSize * (size_t) std::max (1, inf.outChannels), 0.0f);
            }
            allocate();
            return true;
        }

    protected:
        static uint32_t nextPow2Local (uint64_t v) { uint32_t p = 1; while (p < v) p <<= 1; return p; }

        void consumeOutput (int frames) override
        {
            if (loopDelay <= 0) return;
            for (int c = 0; c < inf.outChannels; ++c)
                for (int i = 0; i < frames; ++i)
                    loop[(size_t) c * loopSize + ((uint64_t) (written + i) & (loopSize - 1))] = outPtr[(size_t) c][i];
            written += frames;
        }

        bool fillInput (int64_t frame, int frames) override
        {
            if (loopDelay > 0)
            {
                for (int c = 0; c < inf.inChannels; ++c)
                    for (int i = 0; i < frames; ++i)
                    {
                        const int64_t src = frame + i - loopDelay;
                        inPtr[(size_t) c][i] = (src >= 0 && c < inf.outChannels) ? loop[(size_t) c * loopSize + ((uint64_t) src & (loopSize - 1))] : 0.0f;
                    }
                return true;
            }
            if (mode == "noise")
                for (int c = 0; c < inf.inChannels; ++c)
                    for (int i = 0; i < frames; ++i) inPtr[(size_t) c][i] = testNoise (seed, (uint32_t) c, (uint64_t) (frame + i));
            else if (mode == "sine")
                for (int c = 0; c < inf.inChannels; ++c)
                    for (int i = 0; i < frames; ++i) inPtr[(size_t) c][i] = 0.25f * (float) std::sin (2 * 3.14159265358979 * 1000.0 * (double) (frame + i) / inf.sampleRate);
            else
                std::fill (inData.begin(), inData.end(), 0.0f);
            return true;
        }

    private:
        std::string mode;
        uint64_t seed = 1;
        int loopDelay = 0;
        uint32_t loopSize = 0;
        int64_t written = 0;
        std::vector<float> loop;
    };

    //==========================================================================
    class FileDevice : public ClockedDevice
    {
    public:
        explicit FileDevice (const DeviceSpec& s) : ClockedDevice (s) { defaultSpeed = 0.0; }

        bool open (const DeviceRequest& r, std::string& err) override
        {
            inf.type = "file";
            inf.format = "float32";
            inf.bufferFrames = std::max (16, r.bufferFrames);
            const auto inPath = spec.opt ("in");
            if (! inPath.empty())
            {
                if (! reader.open (widen (inPath), err)) return false;
                inf.sampleRate = reader.sampleRate();
                inf.inChannels = reader.channels();
            }
            else
            {
                inf.sampleRate = r.sampleRate;
                inf.inChannels = std::max (1, r.inChannels);
            }
            inf.name = inPath.empty() ? "Fichier" : inPath;
            inf.outChannels = r.outChannels > 0 ? r.outChannels : (int) spec.optNum ("outputs", 2);
            inf.minBuffer = 16; inf.maxBuffer = 8192; inf.preferredBuffer = inf.bufferFrames; inf.granularity = 1;
            inf.rates = { inf.sampleRate };
            for (int c = 0; c < inf.inChannels; ++c) inf.inNames.push_back ("Entrée " + std::to_string (c + 1));
            for (int c = 0; c < inf.outChannels; ++c) inf.outNames.push_back ("Sortie " + std::to_string (c + 1));
            outPath = spec.opt ("out");
            loop = spec.opt ("loop") == "1";
            allocate();
            inter.assign ((size_t) inf.inChannels * (size_t) inf.bufferFrames, 0.0f);
            outInter.assign ((size_t) inf.outChannels * (size_t) inf.bufferFrames, 0.0f);
            return true;
        }

        void close() override
        {
            ClockedDevice::close();
            writer.close();
            reader.close();
        }

    protected:
        bool prepareStart (std::string& err) override
        {
            if (! outPath.empty() && ! writer.isOpen())
                return writer.open (widen (outPath), inf.outChannels, (int) inf.sampleRate, err);
            return true;
        }

        bool fillInput (int64_t, int frames) override
        {
            if (! reader.isOpenFile()) { std::fill (inData.begin(), inData.end(), 0.0f); return true; }
            int got = reader.read (inter.data(), frames);
            if (got < frames && loop) { reader.seekFrame (0); got += reader.read (inter.data() + (size_t) got * inf.inChannels, frames - got); }
            if (got <= 0) return false;
            for (int i = got; i < frames; ++i)
                for (int c = 0; c < inf.inChannels; ++c) inter[(size_t) i * inf.inChannels + c] = 0.0f;
            for (int c = 0; c < inf.inChannels; ++c)
                for (int i = 0; i < frames; ++i) inPtr[(size_t) c][i] = inter[(size_t) i * inf.inChannels + c];
            return true;
        }

        void consumeOutput (int frames) override
        {
            if (! writer.isOpen()) return;
            for (int i = 0; i < frames; ++i)
                for (int c = 0; c < inf.outChannels; ++c) outInter[(size_t) i * inf.outChannels + c] = outPtr[(size_t) c][i];
            writer.write (outInter.data(), frames);
        }

    private:
        struct Reader : WavReader { bool isOpenFile() const { return channels() > 0; } };
        Reader reader;
        WavWriter writer;
        std::string outPath;
        bool loop = false;
        std::vector<float> inter, outInter;
    };

    std::unique_ptr<AudioDevice> createNullDevice (const DeviceSpec& s) { return std::make_unique<NullDevice> (s); }
    std::unique_ptr<AudioDevice> createFileDevice (const DeviceSpec& s) { return std::make_unique<FileDevice> (s); }

    std::unique_ptr<AudioDevice> createDevice (const std::string& specText, std::string& err)
    {
        const auto s = parseSpec (specText);
        if (s.type == "null") return createNullDevice (s);
        if (s.type == "file") return createFileDevice (s);
        if (s.type == "wasapi") return createWasapiDevice (s);
#if NOVA_AUDIO_PRO
        if (s.type == "pro") return createProDevice (s);
#endif
#if NOVA_AUDIO_WITH_ASIO
        if (s.type == "asio" || s.type == "asio-dll") return createAsioDevice (s);
#elif ! NOVA_AUDIO_PRO
        if (s.type == "asio" || s.type == "asio-dll")
        {
            err = "ASIO non compilé dans ce moteur (build.bat asio)";
            return nullptr;
        }
#endif
        err = "périphérique inconnu : " + specText;
        return nullptr;
    }

    Value listDevices (bool wasapi)
    {
        auto o = Value::object();
#if NOVA_AUDIO_WITH_ASIO
        o.set ("asio", listAsioDrivers());
        o.set ("asio_available", true);
#elif ! NOVA_AUDIO_PRO
        o.set ("asio", Value::array());
        o.set ("asio_available", false);
#endif
        if (wasapi) o.set ("wasapi", listWasapiEndpoints());
#if NOVA_AUDIO_PRO
        // Mode audio pro Nova : sorties / entrées, noms des canaux, plus petite période.
        if (wasapi) o.set ("pro", listProEndpoints());
        o.set ("pro_available", true);
#endif
        auto sim = Value::array();
        sim.push ("null");
        sim.push ("file");
        o.set ("simulated", sim);
        return o;
    }
}
