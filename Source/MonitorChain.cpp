/*
    NovaAudioEngine — chaîne du retour casque (voir MonitorChain.h).
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
#include "MonitorChain.h"
#if NOVA_AUDIO_WITH_NOVATUNE
 #include "NovaTune.h"
#else
namespace nova::audio
{
    // Nova Tune natif n'est livré que dans l'édition « Nova Pro Audio » (licence propriétaire de Make
    // Music, native-audio/pro/). Ici : coquille jamais créée (l'insert « novatune » est refusé).
    class NovaTune
    {
    public:
        std::atomic<float> detectedHz { 0 }, targetHz { 0 }, correctionCents { 0 };
        int latencySamples() const { return 0; }
        explicit NovaTune (double) {}
        void process (float* const*, int, int) noexcept {}
        void setParams (const json::Value&) {}
        json::Value params() const { return json::Value::object(); }
    };
}
#endif
#include "SharedAudio.h"
#include "VstInstance.h"
#include "Guard.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstring>
#include <functional>

namespace nova::audio
{
    using json::Value;

    namespace
    {
        int64_t qpcNs()
        {
            static const double f = [] { LARGE_INTEGER x; QueryPerformanceFrequency (&x); return (double) x.QuadPart; }();
            LARGE_INTEGER c;
            QueryPerformanceCounter (&c);
            return (int64_t) ((double) c.QuadPart * 1e9 / f);
        }
        float dbToGain (double db) { return (float) std::pow (10.0, db / 20.0); }
    }

    namespace
    {
        std::function<void (std::function<void()>)> gPoster;
        std::string lower (std::string x) { for (auto& ch : x) ch = (char) std::tolower ((unsigned char) ch); return x; }
    }

    void MonitorChain::setMainThreadPoster (std::function<void (std::function<void()>)> p) { gPoster = std::move (p); }
    void MonitorChain::postToMainThread (std::function<void()> fn)
    {
        if (gPoster) gPoster (std::move (fn));
        else fn();
    }

    MonitorChain::~MonitorChain()
    {
        // Plugins détruits sur le fil principal (la chaîne n'est plus vue par le fil audio).
        for (auto& s : strips)
            for (auto& ins : s.inserts)
                if (ins.vst) { auto* p = ins.vst.release(); guard::call ([p] { delete p; }); }
    }

    std::unique_ptr<MonitorChain> MonitorChain::build (const Value& spec, double sampleRate, int maxBlk, Value& report, std::string& err)
    {
        auto chain = std::unique_ptr<MonitorChain> (new MonitorChain());
        chain->maxBlock = std::max (16, maxBlk);
        report = Value::object();
        auto rStrips = Value::array();
        const auto& list = spec["strips"];
        for (size_t si = 0; si < list.size(); ++si)
        {
            const auto& js = list.at (si);
            Strip s;
            s.id = js["id"].asString (std::to_string (si));
            const auto& ins = js["inputs"];
            s.width = ins.size() >= 2 ? 2 : 1;
            s.inputs[0] = ins.size() > 0 ? ins.at (0).asInt (0) : 0;
            s.inputs[1] = s.width == 2 ? ins.at (1).asInt (1) : -1;
            s.gain = dbToGain (js["gain_db"].asDouble (0.0));
            s.a.assign ((size_t) 2 * chain->maxBlock, 0.0f);
            s.t.assign ((size_t) 2 * chain->maxBlock, 0.0f);
            auto rIns = Value::array();
            int width = s.width;
            const auto& jins = js["inserts"];
            for (size_t k = 0; k < jins.size(); ++k)
            {
                const auto& ji = jins.at (k);
                Insert in;
                in.st = std::make_unique<InsertStats>();
                const auto type = ji["type"].asString ("vst3");
                auto ri = Value::object();
                if (type == "gain")
                {
                    in.name = "gain";
                    in.gain = dbToGain (ji["db"].asDouble (0.0));
                }
                else if (type == "novatune")
                {
#if ! NOVA_AUDIO_WITH_NOVATUNE
                    err = "Nova Tune natif : seulement dans le mode audio pro Nova";
                    return nullptr;
#endif
                    // Nova Tune natif (choix C2) : aucun fichier à charger, réglages de la version web.
                    in.name = "novatune";
                    in.id = ji["id"].asString ("novatune");
                    in.tune = std::make_unique<NovaTune> (sampleRate);
                    in.tune->setParams (ji["params"]);
                    in.latency = in.tune->latencySamples();
                    ri.set ("id", in.id);
                    ri.set ("params", in.tune->params());
                }
                else
                {
                    const auto path = ji["path"].asString();
                    in.vst = std::make_unique<VstInstance>();
                    in.vst->emit = [] (const std::string&, Value) {};
                    in.vst->postToMain = [] (std::function<void()> fn) { postToMainThread (std::move (fn)); };
                    try
                    {
                        const bool ok = guard::call ([&] { in.vst->load (path, ji["class"].asString(), ji["cid"].asString()); });
                        if (! ok) throw std::runtime_error ("le plugin a planté au chargement");
                        const auto& state = ji["state"];
                        if (state.isObject())
                        {
                            std::vector<uint8_t> comp, ctrl;
                            const bool hasC = state["component"].isString() && base64::decode (state["component"].asString(), comp);
                            const bool hasK = state["controller"].isString() && base64::decode (state["controller"].asString(), ctrl);
                            guard::call ([&] { in.vst->setState (hasC ? &comp : nullptr, hasK ? &ctrl : nullptr); });
                        }
                        // Réglages par nom (ex. « Low Latency » d'un autotune pour la cabine) ou par id.
                        auto applied = Value::array();
                        const auto& jp = ji["params"];
                        for (size_t q = 0; q < jp.size(); ++q)
                        {
                            const auto& one = jp.at (q);
                            const auto want = lower (one["name"].asString());
                            int idx = -1;
                            for (size_t pi = 0; pi < in.vst->params.size() && idx < 0; ++pi)
                            {
                                const auto& pinfo = in.vst->params[pi];
                                if ((one.has ("id") && (int64_t) pinfo.id == one["id"].asInt64 (-1)) || (! want.empty() && lower (pinfo.title) == want))
                                    idx = (int) pi;
                            }
                            for (size_t pi = 0; pi < in.vst->params.size() && idx < 0 && ! want.empty(); ++pi)
                                if (lower (in.vst->params[pi].title).find (want) != std::string::npos) idx = (int) pi;
                            if (idx < 0)
                            {
                                // Réglage introuvable : la liste des réglages du plugin aide à le nommer.
                                auto titles = Value::array();
                                for (size_t pi = 0; pi < in.vst->params.size() && pi < 400; ++pi) titles.push (in.vst->params[pi].title);
                                ri.set ("missing", one["name"].asString());
                                ri.set ("param_titles", titles);
                                continue;
                            }
                            const float v = (float) one["value"].asDouble (0.0);
                            guard::call ([&] { in.vst->setValue (idx, v); });
                            auto a = Value::object();
                            a.set ("name", in.vst->params[(size_t) idx].title);
                            a.set ("value", v);
                            applied.push (a);
                        }
                        ri.set ("params", applied);
                        Value prep;
                        // Largeur courante de la tranche ; un plugin qui refuse le mono est préparé en
                        // stéréo (voix mono -> réverbe stéréo : la tranche devient stéréo ensuite).
                        bool okp = guard::call ([&] { prep = in.vst->prepare (sampleRate, chain->maxBlock, false, width, false); });
                        in.channels = width;
                        if ((! okp || ! in.vst->isPrepared()) && width == 1)
                        {
                            okp = guard::call ([&] { prep = in.vst->prepare (sampleRate, chain->maxBlock, false, 2, false); });
                            in.channels = 2;
                        }
                        if (! okp || ! in.vst->isPrepared()) throw std::runtime_error ("le plugin refuse la préparation");
                        if (applied.size() > 0)
                        {
                            // Réglages remis au processeur, puis préparation refaite : la latence relue
                            // est celle du mode choisi (un autotune en « faible latence » change la sienne).
                            const int chans = in.channels;
                            guard::call ([&] { in.vst->flushParams(); in.vst->release(); prep = in.vst->prepare (sampleRate, chain->maxBlock, false, chans, false); });
                            if (! in.vst->isPrepared()) throw std::runtime_error ("le plugin refuse la préparation après ses réglages");
                        }
                        in.channels = std::min (2, std::max (1, in.vst->mainOutChannels()));
                        in.latency = in.vst->latency();
                        in.name = in.vst->name;
                    }
                    catch (const std::exception& e)
                    {
                        err = "insert « " + path + " » : " + e.what();
                        return nullptr;
                    }
                }
                if (in.vst) width = in.channels;
                else in.channels = width;
                if (in.id.empty()) in.id = std::to_string (k);
                s.latency += in.latency;
                ri.set ("name", in.name);
                ri.set ("channels", in.channels);
                ri.set ("latency", in.latency);
                rIns.push (ri);
                s.inserts.push_back (std::move (in));
            }
            s.outWidth = width;
            const auto& jsends = js["sends"];
            for (size_t k = 0; k < jsends.size(); ++k)
            {
                Send sd;
                const auto& outs = jsends.at (k)["outs"];
                for (size_t o = 0; o < outs.size(); ++o) sd.outs.push_back (outs.at (o).asInt (0));
                sd.gain = (float) jsends.at (k)["gain"].asDouble (1.0);
                if (! sd.outs.empty()) s.sends.push_back (sd);
            }
            chain->maxLatency = std::max (chain->maxLatency, s.latency);
            auto rs = Value::object();
            rs.set ("id", s.id);
            rs.set ("latency", s.latency);
            rs.set ("inserts", rIns);
            rStrips.push (rs);
            chain->strips.push_back (std::move (s));
        }
        report.set ("strips", rStrips);
        report.set ("latency", chain->maxLatency);
        return chain;
    }

    void MonitorChain::process (const float* const* in, int numIn, float* const* out, int numOut, int frames) noexcept
    {
        frames = std::min (frames, maxBlock);
        for (auto& s : strips)
        {
            float* a[2] = { s.a.data(), s.a.data() + maxBlock };
            for (int c = 0; c < s.width; ++c)
            {
                const int ch = s.inputs[c];
                if (ch >= 0 && ch < numIn && in[ch] != nullptr)
                    for (int i = 0; i < frames; ++i) a[c][i] = in[ch][i] * s.gain;
                else
                    std::memset (a[c], 0, sizeof (float) * (size_t) frames);
            }
            int w = s.width;
            for (auto& ins : s.inserts)
            {
                if (ins.tune)
                {
                    // Nova Tune natif : en place, somme mono -> les w canaux de la tranche.
                    const int64_t t0 = qpcNs();
                    ins.tune->process (a, w, frames);
                    const int64_t dt = qpcNs() - t0;
                    ins.st->calls.fetch_add (1, std::memory_order_relaxed);
                    ins.st->totalNs.fetch_add ((uint64_t) dt, std::memory_order_relaxed);
                    ins.st->lastNs.store ((uint64_t) dt, std::memory_order_relaxed);
                    if ((uint64_t) dt > ins.st->maxNs.load (std::memory_order_relaxed)) ins.st->maxNs.store ((uint64_t) dt, std::memory_order_relaxed);
                    continue;
                }
                if (! ins.vst)
                {
                    for (int c = 0; c < w; ++c)
                        for (int i = 0; i < frames; ++i) a[c][i] *= ins.gain;
                    continue;
                }
                const int oc = ins.channels;
                if (ins.st->failed.load (std::memory_order_relaxed))
                {
                    // Plugin perdu : signal sec (mono recopié si la suite attend du stéréo).
                    if (oc == 2 && w == 1) std::memcpy (a[1], a[0], sizeof (float) * (size_t) frames);
                    w = oc;
                    continue;
                }
                // Entrée = copie (t), sortie = a : le plugin ne lit jamais ce qu'il est en train d'écrire.
                float* src[2] = { s.t.data(), s.t.data() + maxBlock };
                for (int c = 0; c < w; ++c) std::memcpy (src[c], a[c], sizeof (float) * (size_t) frames);
                const int64_t t0 = qpcNs();
                const bool ok = ins.vst->processRealtime (src, w, a, oc, frames);
                const int64_t dt = qpcNs() - t0;
                if (! ok)
                {
                    // Plugin occupé (préparation) ou planté : signal sec pour ce bloc.
                    if (ins.vst->realtimeCrashed.load()) ins.st->failed = true;
                    for (int c = 0; c < oc; ++c) std::memcpy (a[c], src[std::min (c, w - 1)], sizeof (float) * (size_t) frames);
                    w = oc;
                    continue;
                }
                w = oc;
                ins.st->calls.fetch_add (1, std::memory_order_relaxed);
                ins.st->totalNs.fetch_add ((uint64_t) dt, std::memory_order_relaxed);
                ins.st->lastNs.store ((uint64_t) dt, std::memory_order_relaxed);
                if ((uint64_t) dt > ins.st->maxNs.load (std::memory_order_relaxed)) ins.st->maxNs.store ((uint64_t) dt, std::memory_order_relaxed);
            }
            for (auto& sd : s.sends)
                for (size_t k = 0; k < sd.outs.size(); ++k)
                {
                    const int o = sd.outs[k];
                    if (o < 0 || o >= numOut || out[o] == nullptr) continue;
                    const float* x = a[(int) k % w];
                    for (int i = 0; i < frames; ++i) out[o][i] += x[i] * sd.gain;
                }
        }
    }

    bool MonitorChain::anyFailed() const
    {
        for (auto& s : strips)
            for (auto& i : s.inserts)
                if (i.st->failed.load()) return true;
        return false;
    }

    Value MonitorChain::stats() const
    {
        auto arr = Value::array();
        for (auto& s : strips)
            for (auto& i : s.inserts)
            {
                auto o = Value::object();
                o.set ("strip", s.id);
                o.set ("name", i.name);
                o.set ("latency", i.latency);
                const auto calls = i.st->calls.load();
                o.set ("calls", (double) calls);
                o.set ("avg_us", calls ? (double) i.st->totalNs.load() / (double) calls / 1000.0 : 0.0);
                o.set ("max_us", (double) i.st->maxNs.load() / 1000.0);
                o.set ("failed", i.st->failed.load());
                if (i.tune)
                {
                    o.set ("id", i.id);
                    o.set ("detected_hz", (double) i.tune->detectedHz.load());
                    o.set ("target_hz", (double) i.tune->targetHz.load());
                    o.set ("correction_cents", (double) i.tune->correctionCents.load());
                    o.set ("params", i.tune->params());
                }
                arr.push (o);
            }
        return arr;
    }

    void MonitorChain::updateLatency()
    {
        maxLatency = 0;
        for (auto& s : strips)
        {
            s.latency = 0;
            for (auto& i : s.inserts)
            {
                if (i.tune) i.latency = i.tune->latencySamples();
                s.latency += i.latency;
            }
            maxLatency = std::max (maxLatency, s.latency);
        }
    }

    bool MonitorChain::setInsertParams (const std::string& strip, const Value& insert, const Value& params, Value& report, std::string& err)
    {
        for (auto& s : strips)
        {
            if (s.id != strip) continue;
            for (size_t k = 0; k < s.inserts.size(); ++k)
            {
                auto& in = s.inserts[k];
                const bool match = insert.isNumber() ? (size_t) insert.asInt (-1) == k : in.id == insert.asString ("novatune");
                if (! match) continue;
                if (! in.tune) { err = "l'insert « " + in.id + " » n'est pas un Nova Tune"; return false; }
                in.tune->setParams (params);   // atomiques : pris en compte au bloc suivant, sans coupure
                updateLatency();
                report = Value::object();
                report.set ("strip", s.id);
                report.set ("id", in.id);
                report.set ("latency", in.latency);
                report.set ("chain_latency", maxLatency);
                report.set ("params", in.tune->params());
                return true;
            }
            err = "insert introuvable dans la tranche « " + strip + " »";
            return false;
        }
        err = "tranche « " + strip + " » introuvable";
        return false;
    }
}
