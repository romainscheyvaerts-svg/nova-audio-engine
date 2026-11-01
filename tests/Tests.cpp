/*
    NovaAudioEngine — tests unitaires (console). Aucun son sur les haut-parleurs : périphériques
    « nul » et « fichier » seulement (le pilote ASIO factice est testé par le pont, voir
    bridge-python/tests/test_native_audio.py).

      NovaAudioTests.exe                 tous les tests
      NovaAudioTests.exe resampler       mesures du rééchantillonneur (JSON)
      NovaAudioTests.exe --json <f>      résultats détaillés dans un fichier

    Variable NOVA_TEST_VST3=<chemin .vst3> : ajoute le test « retour casque avec un vrai VST3 ».

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
#include "Resampler.h"
#include "WavFile.h"
#if NOVA_AUDIO_WITH_NOVATUNE
 #include "NovaTune.h"   // édition pro seulement (propriétaire)
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace nova;
using namespace nova::audio;
using nova::json::Value;

namespace
{
    int failures = 0, passes = 0;
    Value report = Value::object();

    void check (bool ok, const std::string& what)
    {
        if (ok) ++passes;
        else { ++failures; std::printf ("ÉCHEC : %s\n", what.c_str()); }
    }

    std::wstring tempPath (const std::wstring& name)
    {
        wchar_t dir[MAX_PATH];
        GetTempPathW (MAX_PATH, dir);
        std::wstring d = std::wstring (dir) + L"NovaAudioTests";
        CreateDirectoryW (d.c_str(), nullptr);
        return d + L"\\" + name;
    }

    //==========================================================================
    void testRings()
    {
        RingState st;
        st.capacity = 64;
        st.channels = 2;
        std::vector<float> data (128, 0.0f);
        FrameRing r;
        r.attach (&st, data.data());
        float blk[2][16];
        const float* pl[2] = { blk[0], blk[1] };
        int v = 0;
        for (int b = 0; b < 3; ++b)
        {
            for (int i = 0; i < 16; ++i) { blk[0][i] = (float) v; blk[1][i] = (float) -v; ++v; }
            r.writePlanar (pl, 2, 16);
        }
        uint64_t pos = 0, skipped = 0;
        float out[2 * 64];
        int n = r.readOverwrite (pos, out, 64, skipped);
        check (n == 48 && skipped == 0 && out[0] == 0 && out[1] == 0 && out[2 * 47] == 47 && out[2 * 47 + 1] == -47, "anneau : lecture simple");
        for (int b = 0; b < 6; ++b)
        {
            for (int i = 0; i < 16; ++i) { blk[0][i] = (float) v; blk[1][i] = (float) -v; ++v; }
            r.writePlanar (pl, 2, 16);
        }
        // 144 images écrites, capacité 64 : le lecteur resté à 48 a perdu 32 images.
        n = r.readOverwrite (pos, out, 64, skipped);
        check (skipped == 32 && n == 64 && out[0] == 80.0f && pos == 144, "anneau : écrasement détecté (pertes comptées, position absolue)");
        // Mode borné
        RingState bs; bs.capacity = 32; bs.channels = 1;
        std::vector<float> bd (32);
        FrameRing br; br.attach (&bs, bd.data());
        float src[40];
        for (int i = 0; i < 40; ++i) src[i] = (float) i;
        check (br.writeBounded (src, 40) == 32, "anneau borné : écrit seulement la place libre");
        bs.read.store (10);
        check (br.writeBounded (src, 40) == 10, "anneau borné : place rendue par le lecteur");
    }

    //==========================================================================
    struct Fit { double amp, residual; };

    // Ajustement aux moindres carrés d'une sinusoïde de fréquence connue (+ continu).
    Fit fitSine (const std::vector<float>& y, size_t from, size_t to, double f, double rate)
    {
        double ss = 0, cc = 0, sc = 0, ys = 0, yc = 0, s1 = 0, c1 = 0, y1 = 0, nn = 0;
        for (size_t i = from; i < to; ++i)
        {
            const double w = 2 * 3.14159265358979323846 * f * (double) i / rate;
            const double s = std::sin (w), c = std::cos (w), v = y[i];
            ss += s * s; cc += c * c; sc += s * c; ys += v * s; yc += v * c; s1 += s; c1 += c; y1 += v; nn += 1;
        }
        // Système 3×3 (a·sin + b·cos + d)
        double A[3][4] = { { ss, sc, s1, ys }, { sc, cc, c1, yc }, { s1, c1, nn, y1 } };
        for (int i = 0; i < 3; ++i)
        {
            int p = i;
            for (int k = i + 1; k < 3; ++k) if (std::fabs (A[k][i]) > std::fabs (A[p][i])) p = k;
            for (int k = 0; k < 4; ++k) std::swap (A[i][k], A[p][k]);
            for (int k = 0; k < 3; ++k)
            {
                if (k == i || A[i][i] == 0) continue;
                const double m = A[k][i] / A[i][i];
                for (int j = 0; j < 4; ++j) A[k][j] -= m * A[i][j];
            }
        }
        const double a = A[0][3] / A[0][0], b = A[1][3] / A[1][1], d = A[2][3] / A[2][2];
        double res = 0;
        for (size_t i = from; i < to; ++i)
        {
            const double w = 2 * 3.14159265358979323846 * f * (double) i / rate;
            const double e = y[i] - (a * std::sin (w) + b * std::cos (w) + d);
            res += e * e;
        }
        return { std::sqrt (a * a + b * b), std::sqrt (res / nn) * std::sqrt (2.0) };
    }

    std::vector<float> runResampler (int inRate, int outRate, double f, double amp, double seconds)
    {
        Resampler rs;
        rs.setup (inRate, outRate, 1, 4096);
        const size_t nIn = (size_t) (seconds * inRate);
        std::vector<float> x (nIn), y;
        for (size_t i = 0; i < nIn; ++i) x[i] = (float) (amp * std::sin (2 * 3.14159265358979323846 * f * (double) i / inRate));
        std::vector<float> out (8192 * 3);
        for (size_t p = 0; p < nIn; p += 1000)   // blocs irréguliers : le flux doit rester continu
        {
            const int n = (int) std::min<size_t> (1000, nIn - p);
            const float* in[1] = { x.data() + p };
            float* o[1] = { out.data() };
            const int got = rs.process (in, n, o, (int) out.size());
            y.insert (y.end(), out.begin(), out.begin() + got);
        }
        return y;
    }

    Value testResampler()
    {
        const int pairs[][2] = { { 44100, 48000 }, { 48000, 44100 }, { 48000, 96000 }, { 96000, 48000 }, { 44100, 96000 }, { 96000, 44100 }, { 44100, 88200 }, { 88200, 48000 } };
        auto all = Value::array();
        double worstFlat = 0, worstResidual = -300, worstAlias = -300;
        for (auto& pr : pairs)
        {
            const int inR = pr[0], outR = pr[1];
            Resampler probe;
            probe.setup (inR, outR, 1, 4096);
            double maxDev = 0, maxRes = -300, maxAlias = -300;
            // Bande passante : 20 Hz … 20 kHz (pas logarithmique + bord à 20 kHz).
            std::vector<double> freqs;
            for (double f = 20; f < 20000; f *= 1.25) freqs.push_back (f);
            for (double f : { 15000.0, 18000.0, 19000.0, 19500.0, 19900.0, 20000.0 }) freqs.push_back (f);
            const double amp = 0.5;
            for (double f : freqs)
            {
                if (f > probe.passbandHz() + 1) continue;
                auto y = runResampler (inR, outR, f, amp, 0.5);
                const size_t skip = (size_t) probe.tapsPerPhase() * 2 + 64;
                if (y.size() < skip * 2 + 1000) continue;
                const auto fit = fitSine (y, skip, y.size() - skip, f, outR);
                const double dev = 20 * std::log10 (fit.amp / amp);
                maxDev = std::max (maxDev, std::fabs (dev));
                maxRes = std::max (maxRes, 20 * std::log10 (std::max (1e-15, fit.residual / amp)));
            }
            // Repliement : sinusoïdes entre le Nyquist de sortie et celui d'entrée (descente seulement).
            if (outR < inR)
            {
                for (double f = outR / 2.0 + 50; f < inR / 2.0 - 50; f += (inR / 2.0 - outR / 2.0) / 7.0)
                {
                    auto y = runResampler (inR, outR, f, amp, 0.5);
                    const size_t skip = (size_t) probe.tapsPerPhase() * 2 + 64;
                    double e = 0;
                    size_t n = 0;
                    for (size_t i = skip; i + skip < y.size(); ++i, ++n) e += (double) y[i] * y[i];
                    const double rms = std::sqrt (e / std::max<size_t> (1, n)) * std::sqrt (2.0);
                    maxAlias = std::max (maxAlias, 20 * std::log10 (std::max (1e-15, rms / amp)));
                }
            }
            auto o = Value::object();
            o.set ("in", inR);
            o.set ("out", outR);
            o.set ("taps_per_phase", probe.tapsPerPhase());
            o.set ("phases", probe.upFactor());
            o.set ("passband_hz", probe.passbandHz());
            o.set ("flatness_db", maxDev);
            o.set ("residual_db", maxRes);           // images + repliement + bruit de calcul, sinus dans la bande
            if (outR < inR) o.set ("alias_db", maxAlias);
            all.push (o);
            worstFlat = std::max (worstFlat, maxDev);
            worstResidual = std::max (worstResidual, maxRes);
            worstAlias = std::max (worstAlias, maxAlias);
            check (maxDev <= 0.1, "rééchantillonneur " + std::to_string (inR) + "→" + std::to_string (outR) + " : plat à ±0,1 dB jusqu'à 20 kHz");
            check (maxRes < -90, "rééchantillonneur " + std::to_string (inR) + "→" + std::to_string (outR) + " : résidu < -90 dB");
            if (outR < inR) check (maxAlias < -90, "rééchantillonneur " + std::to_string (inR) + "→" + std::to_string (outR) + " : repliement < -90 dB");
        }
        // Calage : sortie j <-> instant j·M/L de l'entrée (aucun retard).
        {
            Resampler rs;
            rs.setup (48000, 44100, 1, 4096);
            std::vector<float> x (48000, 0.0f), out (60000);
            x[4800] = 1.0f;   // impulsion à 100 ms
            int got = 0;
            for (int p = 0; p < 48000; p += 4000)
            {
                const float* in[1] = { x.data() + p };
                float* o[1] = { out.data() + got };
                got += rs.process (in, 4000, o, 60000 - got);
            }
            int k = 0;
            for (int i = 1; i < got; ++i) if (std::fabs (out[(size_t) i]) > std::fabs (out[(size_t) k])) k = i;
            check (k == 4410, "rééchantillonneur : impulsion à 100 ms retrouvée à 100 ms (sans retard) : " + std::to_string (k) + " sur " + std::to_string (got));
        }
        auto r = Value::object();
        r.set ("pairs", all);
        r.set ("worst_flatness_db", worstFlat);
        r.set ("worst_residual_db", worstResidual);
        r.set ("worst_alias_db", worstAlias);
        return r;
    }

    //==========================================================================
    void testWav()
    {
        const auto p = tempPath (L"wav_roundtrip.wav");
        std::string err;
        WavWriter w;
        check (w.open (p, 2, 48000, err), "WAV : création");
        std::vector<float> x (2 * 100000);
        for (size_t i = 0; i < x.size(); ++i) x[i] = testNoise (7, (uint32_t) (i & 1), i / 2);
        w.write (x.data(), 30000);
        w.updateHeader (false);
        w.write (x.data() + 60000, 70000);
        w.close();
        WavReader r;
        check (r.open (p, err) && r.channels() == 2 && r.totalFrames() == 100000, "WAV : relu (en-tête à jour)");
        std::vector<float> y (x.size());
        r.read (y.data(), 100000);
        check (std::memcmp (x.data(), y.data(), sizeof (float) * x.size()) == 0, "WAV : 32 bits flottants au bit près");
        r.close();
        // En-tête identique à celui du pont Python (58 octets, fact).
        HANDLE h = CreateFileW (p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        uint8_t hd[58] {};
        DWORD got = 0;
        ReadFile (h, hd, 58, &got, nullptr);
        CloseHandle (h);
        check (std::memcmp (hd + 38, "fact", 4) == 0 && std::memcmp (hd + 50, "data", 4) == 0 && hd[20] == 3, "WAV : en-tête 58 octets (format 3, fact) comme le pont");
    }

    //==========================================================================
    Value runEngine (const std::string& device, int projectRate, int buffer, double seconds, Engine& e, std::string& err)
    {
        EngineConfig c;
        c.device = device;
        c.projectRate = projectRate;
        c.buffer = buffer;
        c.inChannels = 2;
        c.outChannels = 2;
        c.shared = false;
        Value res;
        if (! e.start (c, res, err)) return Value();
        (void) seconds;
        return res;
    }

    void pumpUntil (Engine& e, uint64_t projFrames, double timeoutS)
    {
        const double t0 = nowMs();
        while (e.projectFramesWritten() < projFrames && nowMs() - t0 < timeoutS * 1000) { e.housekeeping(); Sleep (5); }
    }

    Value testEngineTake()
    {
        // Prise écrite par le moteur : bruit déterministe, nul cadencé 8× : fichier = entrée au bit près.
        Engine e;
        std::string err;
        auto res = runEngine ("null?input=noise&seed=11&speed=8", 48000, 128, 0, e, err);
        check (res.isObject(), "moteur (nul) : démarrage " + err);
        if (! res.isObject()) return Value();
        pumpUntil (e, 48000, 5);
        Value a = Value::object(), files = Value::array(), f1 = Value::object(), f2 = Value::object(), ins1 = Value::array(), ins2 = Value::array();
        f1.set ("path", narrow (tempPath (L"take_mono.wav")));
        ins1.push (1);
        f1.set ("inputs", ins1);
        f2.set ("path", narrow (tempPath (L"take_stereo.wav")));
        ins2.push (0); ins2.push (1);
        f2.set ("inputs", ins2);
        f2.set ("mode", "stereo");
        files.push (f1); files.push (f2);
        a.set ("take_id", "t1");
        a.set ("files", files);
        a.set ("preroll", 0.5);
        Value r;
        check (e.recStart (a, r, err), "moteur : rec_start " + err);
        const int64_t start = r["start_frame"].asInt64();
        pumpUntil (e, (uint64_t) start + 48000 * 20, 10);   // 20 s de son (2,5 s réelles)
        Value s;
        check (e.recStop ("t1", start + 48000 * 20, s, err), "moteur : rec_stop " + err);
        auto stats = e.stats();
        e.stop();
        WavReader rd;
        bool same = rd.open (tempPath (L"take_stereo.wav"), err) && rd.totalFrames() == 48000ull * 20;
        std::vector<float> y (2 * 4096);
        uint64_t f = 0, bad = 0;
        while (same)
        {
            const int n = rd.read (y.data(), 4096);
            if (n <= 0) break;
            for (int i = 0; i < n; ++i, ++f)
                for (uint32_t c = 0; c < 2; ++c)
                    if (y[(size_t) i * 2 + c] != testNoise (11, c, (uint64_t) start + f)) ++bad;
        }
        check (same && bad == 0 && f == 48000ull * 20, "moteur : prise stéréo identique à l'entrée au bit près (20 s)");
        auto o = Value::object();
        o.set ("start_frame", (double) start);
        o.set ("frames", (double) f);
        o.set ("mismatches", (double) bad);
        o.set ("late_callbacks", stats["late_callbacks"]);
        o.set ("callback_max_us", stats["callback_max_us"]);
        o.set ("callback_avg_us", stats["callback_avg_us"]);
        o.set ("losses", s["losses"]);
        check (s["losses"].size() == 0, "moteur : aucune perte dans la prise");
        return o;
    }

    Value testEngineResampledTake()
    {
        // Carte à 44,1 kHz, projet à 48 kHz : sinus 1 kHz, prise à 48 kHz, amplitude et fréquence gardées.
        Engine e;
        std::string err;
        auto res = runEngine ("null?input=sine&rate=44100&speed=8", 48000, 128, 0, e, err);
        check (res.isObject() && res["resampling"].asBool(), "moteur : rééchantillonnage 44,1 -> 48 kHz actif " + err);
        if (! res.isObject()) return Value();
        pumpUntil (e, 48000, 5);
        Value a = Value::object(), files = Value::array(), f1 = Value::object(), ins = Value::array();
        f1.set ("path", narrow (tempPath (L"take_rs.wav")));
        ins.push (0);
        f1.set ("inputs", ins);
        files.push (f1);
        a.set ("take_id", "rs");
        a.set ("files", files);
        a.set ("preroll", 0.0);
        Value r, s;
        e.recStart (a, r, err);
        const int64_t start = r["start_frame"].asInt64();
        pumpUntil (e, (uint64_t) start + 48000 * 5, 10);
        e.recStop ("rs", start + 48000 * 5, s, err);
        e.stop();
        WavReader rd;
        rd.open (tempPath (L"take_rs.wav"), err);
        std::vector<float> y ((size_t) rd.totalFrames());
        rd.read (y.data(), (int) y.size());
        const auto fit = fitSine (y, 1000, y.size() - 1000, 1000.0, 48000.0);
        // Phase : la prise commence à l'image projet start = instant start/48000 de l'entrée.
        double num = 0, den = 0;
        for (size_t i = 1000; i < y.size() - 1000; ++i)
        {
            const double t = (double) (start + (int64_t) i) / 48000.0;
            const double ref = 0.25 * std::sin (2 * 3.14159265358979 * 1000.0 * t);
            num += (y[i] - ref) * (y[i] - ref);
            den += ref * ref;
        }
        const double errDb = 10 * std::log10 (std::max (1e-30, num / den));
        check (rd.totalFrames() == 48000ull * 5, "moteur : prise rééchantillonnée de la bonne longueur");
        check (std::fabs (20 * std::log10 (fit.amp / 0.25)) < 0.01, "moteur : amplitude gardée (44,1 -> 48 kHz)");
        check (errDb < -80, "moteur : prise rééchantillonnée calée à l'échantillon (écart à la référence < -80 dB)");
        auto o = Value::object();
        o.set ("frames", (double) rd.totalFrames());
        o.set ("amp_db", 20 * std::log10 (fit.amp / 0.25));
        o.set ("error_vs_reference_db", errDb);
        return o;
    }

    Value testLatencyLoopback()
    {
        // Câble simulé sortie k -> entrée k (2 tampons + 48 images de convertisseurs). Retour direct
        // entrée 1 -> sortie 2 : la chaîne de retour ajoute 0 bloc (latence = aller-retour du pilote).
        Engine e;
        std::string err;
        auto res = runEngine ("null?loopback=1&extra=48&speed=1", 48000, 128, 0, e, err);
        check (res.isObject(), "moteur (boucle) : démarrage " + err);
        if (! res.isObject()) return Value();
        e.setRoutes ({ { 0, 1, 1.0f } }, true);
        Sleep (100);
        Value got;
        HANDLE ev = CreateEventW (nullptr, TRUE, FALSE, nullptr);
        e.measureLatency ({ 0 }, 0.3, 0.5f, [&] (Value v) { got = v; SetEvent (ev); }, err);
        WaitForSingleObject (ev, 3000);
        CloseHandle (ev);
        auto st = e.stats();
        e.stop();
        const int rt = 2 * 128 + 48;
        const int d0 = got["delays"].at (0).asInt (-1), d1 = got["delays"].at (1).asInt (-1);
        check (d0 == rt, "latence : aller-retour mesuré = 2 tampons + convertisseurs (" + std::to_string (d0) + ")");
        check (d1 - d0 == rt, "latence : retour casque = aller-retour du pilote, aucun bloc ajouté (" + std::to_string (d1 - d0) + ")");
        auto o = Value::object();
        o.set ("roundtrip_frames", d0);
        o.set ("monitor_frames", d1 - d0);
        o.set ("monitor_ms", 1000.0 * (d1 - d0) / 48000.0);
        o.set ("late_callbacks", st["late_callbacks"]);
        return o;
    }

    Value testFileMonitorChain()
    {
        // Fichier -> chaîne (gain -6 dB) -> fichier : sortie = entrée × 0,5 dans le MÊME bloc.
        const auto in = tempPath (L"chain_in.wav"), out = tempPath (L"chain_out.wav");
        std::string err;
        {
            WavWriter w;
            w.open (in, 1, 48000, err);
            std::vector<float> x (48000);
            for (size_t i = 0; i < x.size(); ++i) x[i] = testNoise (3, 0, i);
            w.write (x.data(), (int) x.size());
            w.close();
        }
        Engine e;
        EngineConfig c;
        c.device = "file?in=" + narrow (in) + "&out=" + narrow (out) + "&speed=0";
        c.projectRate = 48000;
        c.buffer = 128;
        c.inChannels = 1;
        c.outChannels = 2;
        c.shared = false;
        Value spec = Value::parse (R"({"strips":[{"id":"voix","inputs":[0],"gain_db":0,"inserts":[{"type":"gain","db":-6.0205999132796}],"sends":[{"outs":[0,1],"gain":1}]}]})");
        Value rep;
        e.setMonitorChain (spec, rep, err);
        Value res;
        check (e.start (c, res, err), "moteur (fichier) : démarrage " + err);
        const double t0 = nowMs();
        while (nowMs() - t0 < 5000)
        {
            auto ev = e.housekeeping();
            if (ev.isObject() && ev["ended"].asBool()) break;
            Sleep (5);
        }
        e.stop();
        WavReader r;
        r.open (out, err);
        std::vector<float> y ((size_t) r.totalFrames() * 2);
        r.read (y.data(), (int) r.totalFrames());
        uint64_t bad = 0;
        for (size_t i = 0; i < 48000 && i < r.totalFrames(); ++i)
        {
            const float ref = testNoise (3, 0, i) * (float) std::pow (10.0, -6.0205999132796 / 20.0);
            if (std::fabs (y[2 * i] - ref) > 1e-7f || std::fabs (y[2 * i + 1] - ref) > 1e-7f) ++bad;
        }
        check (r.totalFrames() >= 48000 && bad == 0, "chaîne du retour (gain) : sortie = entrée × gain, même bloc, mono -> 2 sorties");
        auto o = Value::object();
        o.set ("frames", (double) r.totalFrames());
        o.set ("mismatches", (double) bad);
        return o;
    }

    Value testVst3Chain (const std::string& path)
    {
        // Vrai VST3 dans la chaîne du retour, sur la boucle simulée : latence et coût par bloc.
        Engine e;
        std::string err;
        EngineConfig c;
        c.device = "null?loopback=1&extra=48&speed=1";
        c.projectRate = 48000;
        c.buffer = 128;
        c.inChannels = 2;
        c.outChannels = 2;
        c.shared = false;
        Value spec = Value::object(), strips = Value::array(), st = Value::object(), ins = Value::array(), i0 = Value::object(),
              inputs = Value::array(), sends = Value::array(), s0 = Value::object(), outs = Value::array();
        inputs.push (0);
        st.set ("id", "voix");
        st.set ("inputs", inputs);
        i0.set ("type", "vst3");
        i0.set ("path", path);
        char pv[512] {};
        if (GetEnvironmentVariableA ("NOVA_TEST_VST3_PARAMS", pv, sizeof (pv)) > 0)
        {
            // « Nom=valeur;Nom2=valeur2 » (valeurs normalisées 0-1)
            auto params = Value::array();
            std::string all = pv;
            size_t i = 0;
            while (i < all.size())
            {
                auto sc = all.find (';', i);
                if (sc == std::string::npos) sc = all.size();
                const auto kv = all.substr (i, sc - i);
                const auto eq = kv.find ('=');
                if (eq != std::string::npos)
                {
                    auto one = Value::object();
                    one.set ("name", kv.substr (0, eq));
                    one.set ("value", std::atof (kv.substr (eq + 1).c_str()));
                    params.push (one);
                }
                i = sc + 1;
            }
            i0.set ("params", params);
        }
        ins.push (i0);
        st.set ("inserts", ins);
        outs.push (1);
        s0.set ("outs", outs);
        s0.set ("gain", 1.0);
        sends.push (s0);
        st.set ("sends", sends);
        strips.push (st);
        spec.set ("strips", strips);
        Value rep;
        Value res;
        if (! e.start (c, res, err)) { check (false, "VST3 : démarrage " + err); return Value(); }
        const double tl = nowMs();
        const bool built = e.setMonitorChain (spec, rep, err);
        const double loadMs = nowMs() - tl;
        check (built, "VST3 : chargé dans la chaîne du retour " + err);
        const int lat = e.monitorLatency();
        Sleep (3000);
        Value got;
        HANDLE ev = CreateEventW (nullptr, TRUE, FALSE, nullptr);
        e.measureLatency ({ 0 }, 0.5, 0.5f, [&] (Value v) { got = v; SetEvent (ev); }, err);
        WaitForSingleObject (ev, 3000);
        CloseHandle (ev);
        Sleep (2000);
        auto stats = e.stats();
        e.stop();
        const int d0 = got["delays"].at (0).asInt (-1), d1 = got["delays"].at (1).asInt (-1);
        auto o = Value::object();
        o.set ("plugin", path);
        o.set ("chain", rep);
        o.set ("reported_latency", lat);
        o.set ("roundtrip_frames", d0);
        o.set ("monitor_frames", d1 >= 0 ? d1 - d0 : -1);
        o.set ("monitor_ms", d1 >= 0 ? 1000.0 * (d1 - d0) / 48000.0 : -1.0);
        o.set ("inserts", stats["inserts"]);
        o.set ("late_callbacks", stats["late_callbacks"]);
        o.set ("callback_max_us", stats["callback_max_us"]);
        o.set ("callback_avg_us", stats["callback_avg_us"]);
        o.set ("load_ms", loadMs);
        if (built)
        {
            // Un plugin peut retarder un peu plus ou moins que ce qu'il annonce (forme de l'impulsion) :
            // on vérifie qu'aucun BLOC n'est ajouté par le moteur (écart < 1 tampon).
            o.set ("plugin_delay_vs_reported", (d1 - d0) - (2 * 128 + 48 + lat));
            // (Auto-Tune Pro en « Classic Mode » retarde ~40 images mais annonce encore 2670 : on borne par le haut.)
            check (d1 >= 0 && (d1 - d0) - (2 * 128 + 48 + lat) < 128, "VST3 : retour casque ≤ aller-retour du pilote + latence du plugin (aucun bloc ajouté)");
            check (stats["late_callbacks"].asInt (1) == 0, "VST3 : aucun callback en retard");
        }
        return o;
    }

#if NOVA_AUDIO_WITH_NOVATUNE
    //==========================================================================
    // Nova Tune natif (insert « novatune », NovaTune.h). La comparaison avec la version web est
    // faite par le banc tests/novaTuneNatif.test.ts (NovaTuneBench.exe) ; ici, les garanties propres
    // au moteur : retard exact, bypass au bit près, tranches de 128, latence au casque, réglage en direct.
    std::vector<float> runTune (const std::vector<float>& x, int block, const char* params, double sr = 48000)
    {
        NovaTune t (sr);
        t.setParams (Value::parse (params));
        std::vector<float> y (x.size());
        for (size_t s0 = 0; s0 < x.size(); s0 += (size_t) block)
        {
            const int n = (int) std::min ((size_t) block, x.size() - s0);
            const float* ip[1] = { x.data() + s0 };
            float* op[1] = { y.data() + s0 };
            t.process (ip, op, 1, n);
        }
        return y;
    }

    /** Fréquence moyenne (Hz) par passages à zéro interpolés sur [a, b[. */
    double zcFreq (const std::vector<float>& y, size_t a, size_t b, double sr)
    {
        double first = -1, last = -1;
        int n = 0;
        for (size_t i = a + 1; i < b; ++i)
            if (y[i - 1] < 0 && y[i] >= 0)
            {
                const double t = (double) (i - 1) + y[i - 1] / (double) (y[i - 1] - y[i]);
                if (first < 0) first = t;
                last = t;
                ++n;
            }
        return n > 1 ? (n - 1) * sr / (last - first) : 0;
    }

    Value testNovaTune()
    {
        const double sr = 48000;
        std::vector<float> x (96000);
        // Voix tenue simplifiée : 445 Hz (≈ +19,6 cents au-dessus du La 440), 3 harmoniques.
        for (size_t i = 0; i < x.size(); ++i)
        {
            const double ph = 2 * 3.141592653589793 * 445.0 * (double) i / sr;
            x[i] = (float) (0.25 * std::sin (ph) + 0.08 * std::sin (2 * ph) + 0.04 * std::sin (3 * ph));
        }
        auto o = Value::object();
        const int dmin = (int) std::floor (0.0005 * sr + 0.5) + 4;

        // 1. Dosage 0, mode direct : sortie = entrée retardée de llDmin (0,5 ms + 4 images), au bit près.
        auto y0 = runTune (x, 128, R"({"mix":0,"speed":0,"lowLatency":true})");
        size_t bad = 0;
        for (size_t i = (size_t) dmin; i < x.size(); ++i) if (y0[i] != x[i - (size_t) dmin]) ++bad;
        check (bad == 0, "Nova Tune : dosage 0 en mode direct = entrée retardée de " + std::to_string (dmin) + " images, au bit près (" + std::to_string (bad) + " écarts)");
        o.set ("direct_retard_images", dmin);

        // 2. Bypass : sortie = entrée au bit près.
        auto yb = runTune (x, 128, R"({"isEnabled":false})");
        check (yb == x, "Nova Tune : désactivé = entrée au bit près");

        // 3. Correction : 445 Hz ramené à 440 Hz (robot, chromatique), dans les deux modes.
        for (const bool ll : { true, false })
        {
            auto y = runTune (x, 128, ll ? R"({"speed":0,"humanize":0,"lowLatency":true})" : R"({"speed":0,"humanize":0,"lowLatency":false})");
            const double f = zcFreq (y, 48000, 96000, sr);
            const double cents = 1200 * std::log2 (f / 440.0);
            o.set (ll ? "direct_cents_vs_la440" : "qualite_cents_vs_la440", cents);
            check (std::fabs (cents) < 3, std::string ("Nova Tune : 445 Hz corrigé à 440 Hz ±3 cents (") + (ll ? "direct" : "qualité") + ") : " + std::to_string (cents));
        }

        // 4. Tranches de 128 alignées : un tampon de 256 ou 512 donne le MÊME son qu'un tampon de 128 (au bit près).
        //    Avec 64 ou 96, les tranches sont plus courtes (sans retard ajouté) : la hauteur visée arrive à
        //    quelques dizaines d'images près, les raccords tombent ailleurs ; même hauteur à ±0,5 cent.
        const char* robot = R"({"speed":0,"humanize":0,"lowLatency":true,"scale":"MINOR","rootKey":9})";
        auto r128 = runTune (x, 128, robot), r256 = runTune (x, 256, robot), r512 = runTune (x, 512, robot);
        check (r256 == r128 && r512 == r128, "Nova Tune : tampon 256 / 512 = tampon 128 au bit près (tranches alignées)");
        const double f128 = zcFreq (r128, 48000, 96000, sr);
        for (const int blk : { 32, 64, 96 })
        {
            auto r = runTune (x, blk, robot);
            const double dc = 1200 * std::log2 (zcFreq (r, 48000, 96000, sr) / f128);
            o.set ("tampon" + std::to_string (blk) + "_vs_128_cents", dc);
            check (std::fabs (dc) < 0.5, "Nova Tune : tampon " + std::to_string (blk) + " = même hauteur que 128 (±0,5 cent) : " + std::to_string (dc));
        }
        return o;
    }

    Value testNovaTuneChain()
    {
        // Boucle simulée (comme testLatencyLoopback) : Nova Tune en mode direct dans la chaîne du casque.
        Engine e;
        std::string err;
        auto res = runEngine ("null?loopback=1&extra=48&speed=1", 48000, 128, 0, e, err);
        check (res.isObject(), "Nova Tune natif : démarrage " + err);
        if (! res.isObject()) return Value();
        Value rep;
        const bool built = e.setMonitorChain (Value::parse (R"({"strips":[{"id":"voix","inputs":[0],"gain_db":0,
            "inserts":[{"type":"novatune","id":"novatune","params":{"speed":0,"scale":"MINOR","rootKey":9,"lowLatency":true}}],
            "sends":[{"outs":[1],"gain":1}]}]})"), rep, err);
        check (built, "Nova Tune natif : inséré dans la chaîne du retour " + err);
        const int nominal = NovaTune::lowLatencyNominal (48000);
        check (rep["latency"].asInt (-1) == nominal, "Nova Tune natif : latence annoncée du mode direct = " + std::to_string (nominal) + " images");
        Sleep (200);
        Value got;
        HANDLE ev = CreateEventW (nullptr, TRUE, FALSE, nullptr);
        e.measureLatency ({ 0 }, 0.3, 0.5f, [&] (Value v) { got = v; SetEvent (ev); }, err);
        WaitForSingleObject (ev, 3000);
        CloseHandle (ev);
        const int rt = 2 * 128 + 48;
        const int d0 = got["delays"].at (0).asInt (-1), d1 = got["delays"].at (1).asInt (-1);
        const int dmin = (int) std::floor (0.0005 * 48000 + 0.5) + 4;
        check (d0 == rt && d1 - d0 == rt + dmin, "Nova Tune natif : retour casque = aller-retour du pilote + " + std::to_string (dmin)
               + " images (0,58 ms), aucun bloc ajouté (" + std::to_string (d1 - d0) + ")");
        // Réglage en direct (monitor_params) : mode qualité, latence relue.
        Value pr;
        const bool ok = e.setMonitorParams (Value::parse (R"({"strip":"voix","insert":"novatune","params":{"lowLatency":false,"speed":0.5}})"), pr, err);
        check (ok && pr["chain_latency"].asInt (-1) == NovaTune::qualityLatency (48000) && e.monitorLatency() == NovaTune::qualityLatency (48000),
               "Nova Tune natif : monitor_params bascule en mode qualité, latence relue (" + std::to_string (e.monitorLatency()) + ") " + err);
        check (std::fabs (pr["params"]["speed"].asDouble (-1) - 0.5) < 1e-6 && pr["params"]["scale"].asString() == "MINOR",
               "Nova Tune natif : réglage modifié, les autres gardés");
        Value bad;
        std::string err2;
        check (! e.setMonitorParams (Value::parse (R"({"strip":"autre","params":{}})"), bad, err2), "Nova Tune natif : tranche inconnue refusée");
        Sleep (300);
        auto st = e.stats();
        e.stop();
        auto o = Value::object();
        o.set ("chain", rep);
        o.set ("monitor_frames", d1 - d0);
        o.set ("monitor_ms", 1000.0 * (d1 - d0) / 48000.0);
        o.set ("added_by_novatune_ms", 1000.0 * (d1 - d0 - rt) / 48000.0);
        o.set ("inserts", st["inserts"]);
        o.set ("late_callbacks", st["late_callbacks"]);
        check (st["late_callbacks"].asInt (1) == 0, "Nova Tune natif : aucun callback en retard");
        return o;
    }
#endif
}

#if NOVA_AUDIO_WITH_ASIO
namespace
{
    // Banc de stress du chemin ASIO existant, avec le pilote ASIO FACTICE (NovaFakeAsio.dll, même
    // modèle physique que le faux WASAPI du mode pro : 2 périodes + 24 images de convertisseurs).
    //   NovaAudioTests.exe asiobench   (NOVA_BENCH_SECONDS=600, NOVA_BENCH_BUFFER=64, NOVA_BENCH_STRESS=0.5,
    //                                   NOVA_BENCH_OUT=<fichier.json>)
    Value asioBench()
    {
        char buf[MAX_PATH] {};
        auto env = [&] (const char* k, double def) { return GetEnvironmentVariableA (k, buf, MAX_PATH) > 0 ? std::atof (buf) : def; };
        const double seconds = env ("NOVA_BENCH_SECONDS", 600);
        const int buffer = (int) env ("NOVA_BENCH_BUFFER", 64);
        const double stress = env ("NOVA_BENCH_STRESS", 0.5);
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW (nullptr, exe, MAX_PATH);
        std::wstring dir (exe);
        dir = dir.substr (0, dir.find_last_of (L"\\/"));
        const auto rep = tempPath (L"fakeasio_bench.json");
        const std::string fake = "inputs=2;outputs=2;buffer=" + std::to_string (buffer) + ";extra_in=12;extra_out=12;noise=none;speed=1;report=" + narrow (rep);
        SetEnvironmentVariableA ("NOVA_FAKEASIO", fake.c_str());
        EngineConfig c;
        c.device = "asio-dll:" + narrow (dir) + "\\NovaFakeAsio.dll?clsid={A6B0C3E2-5D1F-4C7A-9E31-0F4E2B7D9C11}";
        c.projectRate = 48000;
        c.buffer = buffer;
        c.shared = false;
        c.captureSeconds = 4;
        c.stress = stress;
        Engine e;
        Value res;
        std::string err;
        auto o = Value::object();
        if (! e.start (c, res, err)) { o.set ("error", err); return o; }
        Sleep (500);
        int rt = -1;
        {
            HANDLE done = CreateEventW (nullptr, TRUE, FALSE, nullptr);
            Value got;
            std::string e2;
            if (e.measureLatency ({ 0 }, 0.3, 0.5, [&] (Value v) { got = v; SetEvent (done); }, e2) && WaitForSingleObject (done, 5000) == WAIT_OBJECT_0)
                rt = got["delays"].at (0).isNumber() ? got["delays"].at (0).asInt (-1) : -3;
            CloseHandle (done);
        }
        auto cpuSec = [] { FILETIME a, b, k, u; GetProcessTimes (GetCurrentProcess(), &a, &b, &k, &u);
                           auto s = [] (const FILETIME& f) { return (double) (((uint64_t) f.dwHighDateTime << 32) | f.dwLowDateTime) * 1e-7; };
                           return s (k) + s (u); };
        const double cpu0 = cpuSec(), t0 = nowMs();
        const auto s0 = e.stats();
        Sleep ((DWORD) (seconds * 1000));
        const double cpu = 100.0 * (cpuSec() - cpu0) / ((nowMs() - t0) / 1000.0);
        const auto st = e.stats();
        e.stop();
        o.set ("path", "asio (pilote ASIO factice)");
        o.set ("seconds", seconds);
        o.set ("buffer", buffer);
        o.set ("stress", stress);
        o.set ("announced_roundtrip_ms", res["roundtrip_ms"]);
        o.set ("measured_roundtrip_frames", rt);
        o.set ("measured_roundtrip_ms", 1000.0 * rt / 48000.0);
        o.set ("xruns", st["device_xruns"].asDouble (0) - s0["device_xruns"].asDouble (0) + st["late_callbacks"].asDouble (0) - s0["late_callbacks"].asDouble (0));
        o.set ("callback_avg_us", st["callback_avg_us"]);
        o.set ("callback_max_us", st["callback_max_us"]);
        o.set ("cpu_percent_one_core", cpu);
        o.set ("path_overhead_avg_us", st["callback_avg_us"].asDouble (0) - stress * buffer * 1e6 / 48000.0);
        if (FILE* f = _wfopen (rep.c_str(), L"rb"))
        {
            std::string t;
            char b2[4096];
            size_t n;
            while ((n = std::fread (b2, 1, sizeof (b2), f)) > 0) t.append (b2, n);
            std::fclose (f);
            o.set ("fake_driver_report", Value::parse (t));
        }
        return o;
    }
}
#endif

#if NOVA_AUDIO_PRO
// Tests du mode audio pro (tests/pro/ProTests.cpp, faux WASAPI : aucun vrai périphérique).
void runProTests (const std::string& only, nova::json::Value& report, int& passes, int& failures);
#endif

int main (int argc, char** argv)
{
    SetConsoleOutputCP (CP_UTF8);
    OleInitialize (nullptr);
    std::string only = argc > 1 && argv[1][0] != '-' ? argv[1] : "";
    std::string jsonOut;
    for (int i = 1; i + 1 < argc; ++i) if (std::string (argv[i]) == "--json") jsonOut = argv[i + 1];

    if (only.empty() || only == "rings") testRings();
    if (only.empty() || only == "wav") testWav();
    if (only.empty() || only == "resampler") report.set ("resampler", testResampler());
    if (only.empty() || only == "take") report.set ("take", testEngineTake());
    if (only.empty() || only == "take_rs") report.set ("take_resampled", testEngineResampledTake());
    if (only.empty() || only == "latency") report.set ("latency_loopback", testLatencyLoopback());
    if (only.empty() || only == "chain") report.set ("chain_file", testFileMonitorChain());
#if NOVA_AUDIO_WITH_NOVATUNE
    if (only.empty() || only == "novatune") report.set ("novatune", testNovaTune());
    if (only.empty() || only == "novatune") report.set ("novatune_chain", testNovaTuneChain());
#endif
#if NOVA_AUDIO_PRO
    runProTests (only, report, passes, failures);
#endif
#if NOVA_AUDIO_WITH_ASIO
    if (only == "asiobench")
    {
        const auto r = asioBench();
        report.set ("asiobench", r);
        char path[MAX_PATH] {};
        if (GetEnvironmentVariableA ("NOVA_BENCH_OUT", path, MAX_PATH) > 0)
            if (FILE* f = std::fopen (path, "wb")) { const auto t = r.dump(); std::fwrite (t.data(), 1, t.size(), f); std::fclose (f); }
    }
#endif
    char vst[MAX_PATH] {};
    if ((only.empty() || only == "vst3") && GetEnvironmentVariableA ("NOVA_TEST_VST3", vst, MAX_PATH) > 0)
        report.set ("vst3", testVst3Chain (vst));
    report.set ("passed", passes);
    report.set ("failed", failures);
    const auto text = report.dump();
    if (! jsonOut.empty())
    {
        if (FILE* f = std::fopen (jsonOut.c_str(), "wb")) { std::fwrite (text.data(), 1, text.size(), f); std::fclose (f); }
    }
    std::printf ("%s\n", text.c_str());
    std::printf ("%d réussis, %d échecs\n", passes, failures);
    std::fflush (stdout);
    TerminateProcess (GetCurrentProcess(), failures ? 1 : 0);
    return failures ? 1 : 0;
}
