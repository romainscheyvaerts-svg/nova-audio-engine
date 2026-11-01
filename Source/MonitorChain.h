/*
    NovaAudioEngine — chaîne du retour casque (écart n° 3 : autotune et effets au casque ≤ 10 ms).

    Une « tranche » par piste armée : entrée(s) de la carte -> gain -> inserts (VST3 chargés DANS
    le moteur, ou gain interne) -> envois vers les sorties du mix casque. Tout tourne dans le
    callback de la carte : latence = celle du pilote + celle annoncée par les plugins, aucun bloc
    de plus (mesuré par impulsion, voir tests).

    Pourquoi les VST3 dans le moteur et pas dans NovaVSTHost (un processus par plugin) : même
    nombre d'images de latence, mais un aller-retour entre processus par bloc ajoute des réveils
    de fil sous la contrainte des 2,67 ms (128 images à 48 kHz) ; mesuré par `bench_ipc` (rapport
    final). Le plugin reste sous la garde SEH de native-host (Guard.cpp) : s'il plante, son insert
    passe en direct (signal sec), le moteur continue.

    Construite sur le fil principal (chargement, préparation), publiée au fil audio par un
    échange de pointeur atomique ; l'ancienne chaîne est détruite après deux callbacks.

    Spécification JSON (commande monitor_chain) :
      { strips: [ { id, inputs: [0] | [0, 1], gain_db, sends: [ { outs: [2, 3], gain } ],
                   inserts: [ { type: "vst3", path, class?, cid?, state?: {component, controller} }
                              | { type: "gain", db }
                              | { type: "novatune", id?, params?: { speed, mix, humanize, rootKey, scale, isEnabled,
                                  lowLatency, flex, vibrato, formant, transpose, voiceType, targetNotes, chord } } ] } ] }

    Insert « novatune » (choix C2, 30/10/2026) : Nova Tune porté en C++ (NovaTune.h), mêmes réglages
    que la version web ; mode direct (lowLatency, défaut ici) = 0,5 ms + au plus une période.
    Réglage en direct sans reconstruire la chaîne : setInsertParams (commande monitor_params).

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
#pragma once

#include "Common.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace nova
{
    class VstInstance;
    namespace shm { struct Header; }
}

namespace nova::audio
{
    class NovaTune;

    struct InsertStats
    {
        std::atomic<uint64_t> calls { 0 }, totalNs { 0 }, maxNs { 0 }, lastNs { 0 };
        std::atomic<bool> failed { false };
    };

    class MonitorChain
    {
    public:
        ~MonitorChain();
        static std::unique_ptr<MonitorChain> build (const json::Value& spec, double sampleRate, int maxBlock,
                                                    json::Value& report, std::string& err);
        void process (const float* const* in, int numIn, float* const* out, int numOut, int frames) noexcept;
        int latency() const { return maxLatency; }
        // Fil principal (rappels des plugins : redémarrage, latence changée) ; défaut : appel direct.
        static void setMainThreadPoster (std::function<void (std::function<void()>)> p);
        static void postToMainThread (std::function<void()> fn);
        json::Value stats() const;
        bool anyFailed() const;
        /** Fil principal : réglages d'un insert « novatune » (tranche `strip`, insert = id ou n°), sans coupure. */
        bool setInsertParams (const std::string& strip, const json::Value& insert, const json::Value& params,
                              json::Value& report, std::string& err);

    private:
        struct Insert
        {
            std::string name;
            float gain = 1.0f;                       // insert « gain »
            std::unique_ptr<VstInstance> vst;
            std::unique_ptr<NovaTune> tune;           // insert « novatune »
            std::string id;
            int latency = 0;
            int channels = 1;                         // largeur en sortie de l'insert
            std::unique_ptr<InsertStats> st;
        };
        struct Send { std::vector<int> outs; float gain = 1.0f; };
        struct Strip
        {
            std::string id;
            int inputs[2] { 0, -1 };
            int width = 1;                            // largeur à l'entrée (entrées de la carte)
            int outWidth = 1;                         // largeur après les inserts
            float gain = 1.0f;
            std::vector<Insert> inserts;
            std::vector<Send> sends;
            std::vector<float> a, t;                  // [2][maxBlock] : travail, copie d'entrée des inserts
            int latency = 0;
        };
        void updateLatency();
        std::vector<Strip> strips;
        int maxBlock = 0;
        int maxLatency = 0;
    };
}
