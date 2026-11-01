/*
    NovaAudioEngine — moteur audio natif de Nova Studio (lot L1, phase 1). SANS JUCE.

    Programme AUTONOME (serveur audio) : il ne connaît ni Nova Studio ni ses projets. Un client le
    lance caché et le pilote par le protocole générique décrit dans native-audio/PROTOCOLE.md (dans
    Nova Studio, le client est bridge-python/native_audio.py, utilisé seulement quand l'interrupteur
    « Moteur audio natif » est allumé ; sinon le pont garde son chemin habituel). Lignes JSON :
       stdin  ← {"id": 1, "cmd": "...", ...}
       stdout → {"id": 1, "ok": true, ...} | {"id": 1, "ok": false, "error": "..."} | {"event": "...", ...}
    Audio : mémoire partagée (SharedEngine.h) ; prises : écrites sur le disque par le moteur.

    Commandes
      ping                                   → version, protocol, asio (compilé ?)
      devices     wasapi?                    → asio:[{name}], wasapi?:[…], simulated
      start       device, project_rate, device_rate?, buffer, in_channels?, out_channels?,
                  capture_seconds?, play_min_blocks?
                                             → device{…}, shm, layout{…}, latences, rééchantillonnage
      stop | stats | rec_status
      routes      enabled, routes:[{in, out, gain}]           retour direct (sans effet)
      monitor_chain  strips:[…] | clear:true                  retour avec effets (MonitorChain.h)
      monitor_params strip, insert?, params:{…}               réglages d'un Nova Tune natif (NovaTune.h)
      measure_latency  outs:[…], seconds?, amp?               → delays:[par entrée], sample_rate
      rec_start   take_id, files:[{path, track_id, inputs, mode}], preroll?, start_frame?
                                             → start_frame, sample_rate
      rec_stop    take_id, end_frame?        → files:[{path, frames}], losses, end_frame
      control_panel                          (ASIO) panneau du pilote
      bench_ipc   seconds?, frames?, rate?   → aller-retour entre processus (µs) : justifie le choix
                                               « VST3 dans le moteur » pour le retour casque
      quit
    Événements : ready {version, asio, pid}, device {reset|ended|overload…}, log

    Mode --ipc-echo <nom> : processus écho du banc bench_ipc (interne).

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

#include "Common.h"
#include "Engine.h"
#include "Guard.h"

#include <avrt.h>
#if NOVA_AUDIO_PRO
 #include "pro/ProCalibrate.h"
#endif

#include <algorithm>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

using nova::json::Value;

namespace nova::audio
{
#if NOVA_AUDIO_WITH_ASIO
    extern HWND gAsioOwnerWindow;
    bool asioControlPanel (AudioDevice* d);
#endif
}

namespace
{
    using namespace nova;
    using namespace nova::audio;

    const char* kVersion = "0.1.0";
    // Version du protocole (native-audio/PROTOCOLE.md) : augmentée à chaque changement incompatible.
    constexpr int kProtocol = 1;
    // Ajouts compatibles du protocole (un client plus ancien les ignore) : « novatune » = insert Nova Tune
    // natif dans monitor_chain et commande monitor_params (30/10/2026).
    Value features()
    {
        auto f = Value::array();
        f.push ("monitor_chain");
#if NOVA_AUDIO_WITH_NOVATUNE
        f.push ("novatune");
        f.push ("monitor_params");
#endif
#if NOVA_AUDIO_PRO
        // Mode audio pro Nova (30/10/2026) : périphérique « pro », assistant, reprise automatique,
        // agrégation de deux cartes, latence mesurée.
        f.push ("pro_audio");
        f.push ("pro_calibrate");
        f.push ("device_recovery");
        f.push ("aggregate");
        f.push ("measured_latency");
#endif
        return f;
    }

    // Édition du moteur : « pro » (Nova Pro Audio, propriétaire, sans pilote tiers), « asio » (GPLv3,
    // avec le SDK de Steinberg, sans Nova Tune), « libre » (GPLv3 sans ASIO, essais).
    const char* edition()
    {
#if NOVA_AUDIO_PRO
        return "pro";
#elif NOVA_AUDIO_WITH_ASIO
        return "asio";
#else
        return "libre";
#endif
    }

    void setDriverFlag (Value& o)
    {
        o.set ("edition", edition());
#if NOVA_AUDIO_WITH_ASIO
        o.set ("asio", true);
#elif ! NOVA_AUDIO_PRO
        o.set ("asio", false);
#else
        (void) o;
#endif
    }

    HWND msgWindow = nullptr;
    std::mutex queueLock;
    std::deque<std::function<void()>> queue;
    constexpr UINT WM_NOVA_TASK = WM_APP + 1;
    constexpr UINT_PTR kTimerId = 1;
    std::unique_ptr<Engine> engine;

    void post (std::function<void()> fn)
    {
        {
            std::lock_guard<std::mutex> l (queueLock);
            queue.push_back (std::move (fn));
        }
        PostMessageW (msgWindow, WM_NOVA_TASK, 0, 0);
    }

    void runQueue()
    {
        for (;;)
        {
            std::function<void()> fn;
            {
                std::lock_guard<std::mutex> l (queueLock);
                if (queue.empty()) return;
                fn = std::move (queue.front());
                queue.pop_front();
            }
            fn();
        }
    }

    void reply (const Value& id, Value o)
    {
        o.set ("id", id);
        o.set ("ok", true);
        io::writeLine (o);
    }

    void replyErr (const Value& id, const std::string& msg)
    {
        auto o = Value::object();
        o.set ("id", id);
        o.set ("ok", false);
        o.set ("error", msg);
        io::writeLine (o);
    }

    //==========================================================================
    // Banc « aller-retour entre processus » : ce que coûterait de passer chaque bloc du retour
    // casque par un hôte VST séparé (NovaVSTHost : mémoire partagée + 2 événements).
    namespace ipc
    {
        struct Zone
        {
            std::atomic<uint32_t> seq;
            std::atomic<uint32_t> quit;
            uint32_t frames;
            uint32_t channels;
            float in[8 * 4096];
            float out[8 * 4096];
        };

        int echo (const std::wstring& name)
        {
            HANDLE map = OpenFileMappingW (FILE_MAP_ALL_ACCESS, FALSE, (name + L"-zone").c_str());
            HANDLE req = OpenEventW (EVENT_ALL_ACCESS, FALSE, (name + L"-req").c_str());
            HANDLE done = OpenEventW (EVENT_ALL_ACCESS, FALSE, (name + L"-done").c_str());
            if (! map || ! req || ! done) return 2;
            auto* z = (Zone*) MapViewOfFile (map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof (Zone));
            if (! z) return 3;
            enterProAudioThread();
            while (! z->quit.load())
            {
                if (WaitForSingleObject (req, 500) != WAIT_OBJECT_0) continue;
                const size_t n = (size_t) z->frames * z->channels;
                for (size_t i = 0; i < n; ++i) z->out[i] = z->in[i];   // « plugin » neutre
                SetEvent (done);
            }
            return 0;
        }

        Value run (double seconds, int frames, double rate)
        {
            auto res = Value::object();
            const std::wstring name = L"Local\\NovaAudioIpc-" + std::to_wstring (GetCurrentProcessId()) + L"-" + std::to_wstring (GetTickCount64());
            HANDLE map = CreateFileMappingW (INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, (DWORD) sizeof (Zone), (name + L"-zone").c_str());
            HANDLE req = CreateEventW (nullptr, FALSE, FALSE, (name + L"-req").c_str());
            HANDLE done = CreateEventW (nullptr, FALSE, FALSE, (name + L"-done").c_str());
            auto* z = map ? (Zone*) MapViewOfFile (map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof (Zone)) : nullptr;
            if (! z || ! req || ! done) { res.set ("error", "zone non créée"); return res; }
            z->frames = (uint32_t) std::min (frames, 4096);
            z->channels = 2;
            wchar_t exe[MAX_PATH];
            GetModuleFileNameW (nullptr, exe, MAX_PATH);
            std::wstring cmd = L"\"" + std::wstring (exe) + L"\" --ipc-echo " + name;
            STARTUPINFOW si { sizeof (si) };
            PROCESS_INFORMATION pi {};
            if (! CreateProcessW (nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
            { res.set ("error", "processus écho non lancé"); return res; }
            Sleep (200);
            enterProAudioThread();
            PreciseWaiter waiter;
            const double f = PreciseWaiter::freq();
            const double period = (double) frames / rate;
            const int64_t t0 = PreciseWaiter::now();
            std::vector<double> rtt;
            rtt.reserve ((size_t) (seconds / period) + 16);
            int timeouts = 0;
            for (int64_t k = 0;; ++k)
            {
                const int64_t due = t0 + (int64_t) ((double) k * period * f);
                if ((double) (due - t0) / f > seconds) break;
                waiter.waitUntil (due);
                for (int i = 0; i < frames * 2; ++i) z->in[i] = (float) i;
                const int64_t a = PreciseWaiter::now();
                SetEvent (req);
                if (WaitForSingleObject (done, 100) != WAIT_OBJECT_0) { ++timeouts; continue; }
                rtt.push_back ((double) (PreciseWaiter::now() - a) * 1e6 / f);
            }
            leaveProAudioThread();
            z->quit = 1;
            SetEvent (req);
            WaitForSingleObject (pi.hProcess, 2000);
            CloseHandle (pi.hProcess);
            CloseHandle (pi.hThread);
            UnmapViewOfFile (z);
            CloseHandle (map); CloseHandle (req); CloseHandle (done);
            std::sort (rtt.begin(), rtt.end());
            auto pct = [&] (double p) { return rtt.empty() ? 0.0 : rtt[(size_t) std::min<double> ((double) rtt.size() - 1, p * (double) rtt.size())]; };
            double sum = 0;
            for (double v : rtt) sum += v;
            int over250 = 0, over500 = 0, over1000 = 0;
            for (double v : rtt) { over250 += v > 250; over500 += v > 500; over1000 += v > 1000; }
            res.set ("iterations", (double) rtt.size());
            res.set ("timeouts", timeouts);
            res.set ("period_us", period * 1e6);
            res.set ("avg_us", rtt.empty() ? 0.0 : sum / (double) rtt.size());
            res.set ("p50_us", pct (0.5));
            res.set ("p99_us", pct (0.99));
            res.set ("p999_us", pct (0.999));
            res.set ("max_us", rtt.empty() ? 0.0 : rtt.back());
            res.set ("over_250us", over250);
            res.set ("over_500us", over500);
            res.set ("over_1000us", over1000);
            return res;
        }
    }

    //==========================================================================
    EngineConfig configFrom (const Value& m)
    {
        EngineConfig c;
        c.device = m["device"].asString ("null");
        c.projectRate = m["project_rate"].asInt (48000);
        c.deviceRate = m["device_rate"].asInt (0);
        c.buffer = m["buffer"].asInt (128);
        c.inChannels = m["in_channels"].asInt (0);
        c.outChannels = m["out_channels"].asInt (0);
        c.captureSeconds = m["capture_seconds"].asDouble (12.0);
        c.playMinBlocks = m["play_min_blocks"].asInt (2);
        c.measuredRoundtrip = std::max (0, m["measured_roundtrip"].asInt (0));
        c.stress = std::max (0.0, std::min (0.9, m["stress"].asDouble (0.0)));
        return c;
    }

    void handle (const std::string& line)
    {
        std::string perr;
        auto m = Value::parse (line, &perr);
        if (! m.isObject()) { io::log ("commande illisible : " + perr); return; }
        const auto id = m["id"];
        const auto cmd = m["cmd"].asString();
        try
        {
            if (cmd == "ping")
            {
                auto o = Value::object();
                o.set ("version", kVersion);
                o.set ("protocol", kProtocol);
                o.set ("features", features());
                setDriverFlag (o);
                reply (id, o);
            }
            else if (cmd == "devices") reply (id, listDevices (m["wasapi"].asBool (false)));
            else if (cmd == "start")
            {
                Value res;
                std::string err;
                if (engine->start (configFrom (m), res, err)) reply (id, res);
                else replyErr (id, err);
            }
            else if (cmd == "stop") { engine->stop(); reply (id, Value::object()); }
            else if (cmd == "stats") reply (id, engine->stats());
            else if (cmd == "rec_status") { auto o = Value::object(); o.set ("takes", engine->recStatus()); reply (id, o); }
            else if (cmd == "routes")
            {
                std::vector<Route> rs;
                const auto& arr = m["routes"];
                for (size_t i = 0; i < arr.size(); ++i)
                    rs.push_back ({ arr.at (i)["in"].asInt (0), arr.at (i)["out"].asInt (0), (float) arr.at (i)["gain"].asDouble (1.0) });
                engine->setRoutes (rs, m["enabled"].asBool (true));
                auto o = Value::object();
                o.set ("routes", (double) rs.size());
                reply (id, o);
            }
            else if (cmd == "monitor_chain")
            {
                if (m["clear"].asBool (false)) { engine->clearMonitorChain(); reply (id, Value::object()); return; }
                Value rep;
                std::string err;
                if (engine->setMonitorChain (m, rep, err)) reply (id, rep);
                else replyErr (id, err);
            }
            else if (cmd == "monitor_params")
            {
                // Réglages d'un Nova Tune natif de la chaîne, sans la reconstruire (PROTOCOLE.md § 2.3).
                Value rep;
                std::string err;
                if (engine->setMonitorParams (m, rep, err)) reply (id, rep);
                else replyErr (id, err);
            }
            else if (cmd == "measure_latency")
            {
                std::vector<int> outs;
                for (size_t i = 0; i < m["outs"].size(); ++i) outs.push_back (m["outs"].at (i).asInt (0));
                if (outs.empty()) outs = { 0, 1 };
                std::string err;
                const bool ok = engine->measureLatency (outs, m["seconds"].asDouble (0.5), (float) m["amp"].asDouble (0.5),
                    [id] (Value r) { if (r.has ("error")) replyErr (id, r["error"].asString()); else reply (id, r); }, err);
                if (! ok) replyErr (id, err);
            }
            else if (cmd == "rec_start")
            {
                Value res;
                std::string err;
                if (engine->recStart (m, res, err)) reply (id, res);
                else replyErr (id, err);
            }
            else if (cmd == "rec_stop")
            {
                const auto take = m["take_id"].asString();
                const int64_t end = m.has ("end_frame") && ! m["end_frame"].isNull() ? m["end_frame"].asInt64 (-1) : -1;
                // Attente de la fin d'écriture hors du fil principal.
                std::thread ([id, take, end] {
                    Value res;
                    std::string err;
                    if (engine->recStop (take, end, res, err)) reply (id, res);
                    else replyErr (id, err);
                }).detach();
            }
            else if (cmd == "control_panel")
            {
#if NOVA_AUDIO_WITH_ASIO
                if (engine->device() && asioControlPanel (engine->device())) reply (id, Value::object());
                else replyErr (id, "panneau du pilote indisponible");
#elif NOVA_AUDIO_PRO
                replyErr (id, "panneau du pilote indisponible");
#else
                replyErr (id, "ASIO non compilé");
#endif
            }
#if NOVA_AUDIO_PRO
            else if (cmd == "pro_calibrate")
            {
                // Assistant du mode pro : tampon le plus petit sans coupure + marge, latence mesurée.
                // Le flux en cours est arrêté pendant l'essai, puis rouvert avec le réglage choisi.
                if (engine->recStatus().size() > 0) { replyErr (id, "prise en cours : assistant refusé"); return; }
                EngineConfig base = engine->isRunning() ? engine->config() : configFrom (m);
                if (m.has ("device")) base.device = m["device"].asString (base.device);
                if (m.has ("project_rate")) base.projectRate = m["project_rate"].asInt (base.projectRate);
                const bool apply = m["apply"].asBool (true);
                engine->stop();
                std::thread ([id, m, base, apply] {
                    auto res = pro::calibrate (m, base);
                    post ([id, res, base, apply] () mutable {
                        if (apply && res.has ("chosen"))
                        {
                            EngineConfig c = base;
                            c.buffer = res["chosen"].asInt (base.buffer);
                            c.measuredRoundtrip = res["measured_roundtrip"].isNumber() ? res["measured_roundtrip"].asInt (0) : 0;
                            c.stress = 0;
                            Value r;
                            std::string err;
                            if (engine->start (c, r, err)) res.set ("restarted", r);
                            else res.set ("restart_error", err);
                        }
                        reply (id, res);
                    });
                }).detach();
            }
#endif
            else if (cmd == "bench_ipc")
            {
                const double s = m["seconds"].asDouble (10.0);
                const int fr = m["frames"].asInt (128);
                const double rate = m["rate"].asDouble (48000.0);
                std::thread ([id, s, fr, rate] { reply (id, ipc::run (s, fr, rate)); }).detach();
            }
            else if (cmd == "quit")
            {
                reply (id, Value::object());
                engine->stop();
                PostQuitMessage (0);
            }
            else replyErr (id, "commande inconnue : " + cmd);
        }
        catch (const std::exception& e)
        {
            replyErr (id, e.what());
        }
    }

    void readLoop()
    {
        auto h = GetStdHandle (STD_INPUT_HANDLE);
        char buf[65536];
        std::string pending;
        for (;;)
        {
            DWORD got = 0;
            if (! ReadFile (h, buf, sizeof (buf), &got, nullptr) || got == 0) break;
            pending.append (buf, got);
            size_t nl;
            while ((nl = pending.find ('\n')) != std::string::npos)
            {
                auto one = pending.substr (0, nl);
                pending.erase (0, nl + 1);
                if (! one.empty() && one.back() == '\r') one.pop_back();
                if (! one.empty()) post ([line = one] { handle (line); });
            }
        }
        // Le pont s'est arrêté : on arrête le flux (prises fermées proprement) et on quitte.
        post ([] { engine->stop(); PostQuitMessage (0); });
        std::thread ([] { Sleep (6000); TerminateProcess (GetCurrentProcess(), 0); }).detach();
    }

    LRESULT CALLBACK msgProc (HWND h, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (msg == WM_NOVA_TASK) { runQueue(); return 0; }
        if (msg == WM_TIMER && wp == kTimerId)
        {
            auto ev = engine->housekeeping();
            if (ev.isObject()) io::event ("device", ev);
            return 0;
        }
        return DefWindowProcW (h, msg, wp, lp);
    }
}

int WINAPI wWinMain (HINSTANCE, HINSTANCE, PWSTR cmdLine, int)
{
    SetErrorMode (SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    {
        // Jamais « mode efficacité » (EcoQoS) pour un moteur audio.
        PROCESS_POWER_THROTTLING_STATE pt {};
        pt.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        pt.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
        pt.StateMask = 0;
        SetProcessInformation (GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof (pt));
    }
    const std::wstring args = cmdLine != nullptr ? cmdLine : L"";
    const auto echoAt = args.find (L"--ipc-echo ");
    if (echoAt != std::wstring::npos) return ipc::echo (args.substr (echoAt + 11));

    OleInitialize (nullptr);   // STA : les pilotes ASIO sont appelés depuis ce fil
    WNDCLASSEXW wc { sizeof (wc) };
    wc.lpfnWndProc = &msgProc;
    wc.hInstance = GetModuleHandleW (nullptr);
    wc.lpszClassName = L"NovaAudioEngineMessages";
    RegisterClassExW (&wc);
    msgWindow = CreateWindowExW (0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
#if NOVA_AUDIO_WITH_ASIO
    gAsioOwnerWindow = msgWindow;
#endif
    engine = std::make_unique<Engine>();
    MonitorChain::setMainThreadPoster ([] (std::function<void()> fn) { post (std::move (fn)); });
    SetTimer (msgWindow, kTimerId, 100, nullptr);

    std::thread (readLoop).detach();
    auto o = Value::object();
    o.set ("version", kVersion);
    o.set ("protocol", kProtocol);
    o.set ("features", features());
    setDriverFlag (o);
    o.set ("pid", (double) GetCurrentProcessId());
    io::event ("ready", o);

    MSG msg;
    while (GetMessageW (&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage (&msg);
        DispatchMessageW (&msg);
    }
    engine->stop();
    io::event ("bye");
    // Sortie franche : certains plugins gardent des fils en vie.
    TerminateProcess (GetCurrentProcess(), 0);
    return 0;
}
