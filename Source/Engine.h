/*
    NovaAudioEngine — le moteur.

        carte (fil du pilote, MMCSS)                          fil de capture                 fil d'écriture
        ─────────────────────────────                         ──────────────                 ──────────────
        entrées ─┬─▶ anneau « carte » (fréq. carte) ──▶ rééchantillonnage ─┬─▶ anneau « projet » ──▶ WAV des prises
                 │                                       (si carte ≠ projet) └─▶ anneau « aperçu » (mémoire partagée) ──▶ pont ──▶ page
                 ├─▶ chaîne du retour casque (gain, VST3) ─┐
                 └─▶ routes directes ──────────────────────┤
        anneau « lecture » (mém. partagée, mix de la page) ─▶ rééchantillonnage ─┴─▶ sorties

    Fil audio : aucune allocation, aucun verrou, aucune E/S. Il copie, mixe, appelle les plugins du
    retour, signale le fil de capture (SetEvent) et compte (durée du callback, retards, pertes).

    Numérotation : les prises, l'aperçu et le calage utilisent le n° d'image ABSOLU à la fréquence
    du projet depuis le démarrage du flux (pertes de la carte comblées de silence : la suite reste
    calée). Si la carte tourne à la fréquence du projet, c'est exactement le n° d'image de la carte.

    Copyright (C) 2026 Make Music.
    SPDX-License-Identifier: GPL-3.0-only
    Ce fichier fait partie de NovaAudioEngine, logiciel libre : vous pouvez le redistribuer et/ou le
    modifier selon les termes de la GNU General Public License version 3, publiée par la Free Software
    Foundation, avec la permission supplémentaire (article 7) décrite dans native-audio/LICENCES.md
    (pilotes audio et modules d'effets tiers chargés à l'exécution). Il est distribué SANS AUCUNE
    GARANTIE, sans même la garantie implicite de qualité marchande ou d'adéquation à un usage
    particulier. Texte de la licence : native-audio/LICENSE (https://www.gnu.org/licenses/gpl-3.0.html).
*/
#pragma once

#include "Device.h"
#include "MonitorChain.h"
#include "Resampler.h"
#include "Rings.h"
#include "SharedEngine.h"
#include "WavFile.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace nova::audio
{
    struct EngineConfig
    {
        std::string device = "null";
        int projectRate = 48000;
        int deviceRate = 0;            // 0 : la fréquence du projet si la carte la prend
        int buffer = 128;
        int inChannels = 0;            // 0 : toutes (32 au plus)
        int outChannels = 0;
        double captureSeconds = 12.0;  // historique de l'anneau « projet » (pré-capture + retard d'écriture)
        int playMinBlocks = 2;
        bool shared = true;            // zone partagée avec le pont (faux : tests C++)
    };

    struct Route { int in = 0, out = 0; float gain = 1.0f; };

    class Engine : public AudioCallback
    {
    public:
        Engine();
        ~Engine() override;

        // Fil principal.
        bool start (const EngineConfig& cfg, json::Value& result, std::string& err);
        void stop();
        bool isRunning() const { return running.load(); }
        json::Value stats() const;
        json::Value layout() const;
        const DeviceInfo* deviceInfo() const { return dev ? &dev->info() : nullptr; }
        AudioDevice* device() const { return dev.get(); }

        void setRoutes (const std::vector<Route>& routes, bool enabled);
        bool setMonitorChain (const json::Value& spec, json::Value& report, std::string& err);
        void clearMonitorChain();
        int monitorLatency() const;

        // Mesure par impulsion : sorties outs, toutes les entrées enregistrées ; done (fil de capture).
        bool measureLatency (const std::vector<int>& outs, double seconds, float amp,
                             std::function<void (json::Value)> done, std::string& err);

        // Prises écrites par le moteur.
        bool recStart (const json::Value& args, json::Value& result, std::string& err);
        // Attend la fin de l'écriture (appelé hors du fil principal).
        bool recStop (const std::string& takeId, int64_t endFrame, json::Value& result, std::string& err, int timeoutMs = 15000);
        json::Value recStatus() const;

        // Fil principal, ~10 fois par seconde : pilote qui demande une remise à zéro, chaînes à libérer.
        // Rend un événement à signaler au pont (vide sinon).
        json::Value housekeeping();

        // Fil audio.
        void audioBlock (const float* const* in, int numIn, float* const* out, int numOut, int frames, const BlockTime& t) noexcept override;

        // Pour les tests : lecture (rôle du pont) et aperçu.
        FrameRing& playRing() { return play; }
        FrameRing& previewRing() { return preview; }
        uint64_t projectFramesWritten() const { return projRing.written(); }
        shm::Header* header() const { return hdr; }
        std::string sharedName() const { return shmName; }

    private:
        struct Loss { int64_t frame; int64_t frames; const char* kind; };
        struct TakeFile
        {
            std::wstring path;
            std::string trackId;
            std::vector<int> inputs;
            int mode = 0;                     // 0 mono, 1 stéréo, 2 somme
            WavWriter wav;
            std::vector<float> buf;
        };
        struct Take
        {
            std::string id;
            int64_t start = 0, next = 0, stopAt = -1;
            std::vector<std::unique_ptr<TakeFile>> files;
            std::vector<Loss> losses;
            size_t driverLossSeen = 0;
            bool done = false;
            std::string error;
            double lastHeader = 0, lastSync = 0;
        };
        struct Probe
        {
            std::atomic<int> state { 0 };      // 0 libre, 1 armé, 2 impulsion partie, 3 analyse finie
            int64_t emitFrame = 0;             // n° d'image de la carte
            int nOuts = 0;
            int outs[8] {};
            float amp = 0.5f;
            int64_t window = 0;
            std::vector<float> peak;
            std::vector<int64_t> peakAt;
            int64_t analysed = 0;
            std::function<void (json::Value)> done;
        };
        struct RouteSet { int n = 0; Route r[256]; };

        void captureLoop();
        void writerLoop();
        void renderPlayback (float* const* out, int numOut, int frames) noexcept;
        void analyseProbe (const float* inter, int64_t firstDevFrame, int frames);
        void writeTake (Take& t);
        void closeTake (Take& t);
        void freeShared();
        void lockRealtimeMemory();
        size_t lockedBytes = 0;
        bool allocShared (std::string& err);
        void publishClock (int64_t projFrame, double timeSec);
        int64_t projectFrameOf (int64_t deviceFrame) const;

        EngineConfig cfg;
        std::unique_ptr<AudioDevice> dev;
        std::atomic<bool> running { false };
        int devRate = 48000, projRate = 48000, block = 128, nIn = 0, nOut = 0;
        bool resampling = false;

        // Mémoire partagée (ou locale pour les tests)
        HANDLE mapping = nullptr;
        uint8_t* view = nullptr;
        std::vector<uint8_t> localZone;
        std::string shmName;
        shm::Header* hdr = nullptr;
        FrameRing preview, play;

        // Anneaux internes
        RingState devState {}, projState {};
        std::vector<float> devData, projData;
        FrameRing devRing, projRing;
        EventQueue<Loss, 256> lossQueue;

        // Fil audio : lecture
        uint64_t playRead = 0;
        bool playPrimed = false;
        uint32_t playTarget = 2;
        uint32_t playMaxBlocks = 24;
        Resampler playRs;
        std::vector<float> playInter, playPlanarIn, playPlanarOut;
        std::vector<float*> playInPtr, playOutPtr;
        int playMaxNeed = 0;

        // Retour casque
        std::atomic<RouteSet*> routes { nullptr };
        std::atomic<bool> routesEnabled { false };
        std::atomic<MonitorChain*> chain { nullptr };
        json::Value chainSpec;
        struct Retired { int64_t afterCallback; std::unique_ptr<MonitorChain> chain; std::unique_ptr<RouteSet> routes; };
        std::vector<Retired> retired;
        std::unique_ptr<RouteSet> routesOwned;
        std::unique_ptr<MonitorChain> chainOwned;

        Probe probe;

        // Fil audio : horloge
        std::atomic<int64_t> lastBlockFrame { 0 };
        std::atomic<double> lastBlockMs { 0 };
        int64_t frameBase = 0;                 // décalage après une réinitialisation du pilote
        int64_t expectNextFrame = 0;

        // Fil de capture
        std::thread captureThread, writerThread;
        HANDLE captureEvent = nullptr, writerEvent = nullptr;
        std::atomic<bool> quitThreads { false };
        Resampler capRs;
        std::vector<float> capInter, capPlanarIn, capPlanarOut, capOutInter;
        std::vector<float*> capInPtr, capOutPtr;
        uint64_t devReadPos = 0;
        std::mutex lossLock;                   // jamais tenu pendant une E/S disque
        std::vector<Loss> driverLosses;        // pertes de la carte, en images projet (sous lossLock)

        // Prises
        mutable std::mutex takesLock;
        std::condition_variable takesCv;
        std::vector<std::unique_ptr<Take>> takes;
        std::vector<float> writerInter;
    };
}
