/*
    NovaAudioEngine — abstraction des périphériques audio.

    Un périphérique appelle AudioCallback::audioBlock depuis SON fil temps réel, avec des tampons
    planaires float (conversion du format de la carte faite par le périphérique). Le moteur ne
    sait pas d'où vient le son.

    Périphériques (spécification texte, voir parseSpec) :
      asio:<nom du pilote>                 pilote ASIO installé (registre lu seulement) — option de build
      asio-dll:<chemin.dll>?clsid={…}      pilote ASIO chargé directement depuis sa DLL, SANS inscription
                                           COM (pilote factice des tests)
      wasapi[:<id>]?exclusive=1            WASAPI partagé (défaut) ou exclusif
      file?in=<a.wav>&out=<b.wav>&speed=0  fichier WAV en entrée, WAV en sortie (speed 0 = au plus vite)
      null?speed=1&input=silence|noise     nul cadencé (speed 1 = temps réel, 4 = 4× plus vite, 0 = au plus vite)

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
#include <map>
#include <memory>
#include <string>
#include <vector>

struct IMMDeviceEnumerator;   // mmdeviceapi.h (mode audio pro)

namespace nova::audio
{
    struct BlockTime
    {
        int64_t deviceFrame = 0;   // 1re image du bloc (horloge de la carte, pertes comprises)
        int64_t lostBefore = 0;    // images perdues par la carte juste avant ce bloc
        double hostMs = 0;         // heure (QPC, ms) du début du callback
    };

    class AudioCallback
    {
    public:
        virtual ~AudioCallback() = default;
        virtual void audioBlock (const float* const* in, int numIn, float* const* out, int numOut, int frames, const BlockTime& t) noexcept = 0;
    };

    struct DeviceRequest
    {
        double sampleRate = 48000;
        int bufferFrames = 128;
        int inChannels = 0;     // 0 = toutes
        int outChannels = 0;
    };

    struct DeviceInfo
    {
        std::string type, name, format;
        double sampleRate = 0;
        int bufferFrames = 0, inChannels = 0, outChannels = 0;
        int inputLatency = 0, outputLatency = 0;      // images, comme annoncé par le pilote
        int minBuffer = 0, maxBuffer = 0, preferredBuffer = 0, granularity = 0;
        bool outputReady = false;
        std::vector<std::string> inNames, outNames;
        std::vector<double> rates;                    // fréquences acceptées parmi 44,1 / 48 / 88,2 / 96 kHz
        std::string warning;                          // à montrer à l'utilisateur (ex. entrée indisponible), vide sinon
        json::Value toJson() const;
    };

    enum DeviceEvent : uint32_t
    {
        kEvReset = 1,          // le pilote demande une réinitialisation (kAsioResetRequest…)
        kEvResync = 2,
        kEvLatency = 4,
        kEvBufferSize = 8,
        kEvOverload = 16,
        kEvEnded = 32,         // fin du fichier d'entrée (périphérique « file »)
        kEvError = 64,
        kEvLost = 128,         // périphérique débranché / invalidé (veille…) : le moteur rouvre dès qu'il revient
        kEvDefaultChanged = 256, // le périphérique par défaut de Windows a changé (flux qui le suit : rouvert)
    };

    class AudioDevice
    {
    public:
        virtual ~AudioDevice() = default;
        virtual bool open (const DeviceRequest& r, std::string& err) = 0;
        virtual bool start (AudioCallback* cb, std::string& err) = 0;
        virtual void stop() = 0;
        virtual void close() = 0;
        virtual const DeviceInfo& info() const = 0;
        virtual json::Value extraStats() const { return json::Value::object(); }

        std::atomic<uint32_t> events { 0 };
        std::atomic<uint64_t> xruns { 0 };          // échéances manquées / surcharges vues par le périphérique
        std::atomic<uint64_t> lostFrames { 0 };     // images perdues annoncées par le pilote
    };

    struct DeviceSpec
    {
        std::string type, name;
        std::map<std::string, std::string> opts;
        std::string opt (const std::string& k, const std::string& def = {}) const
        {
            auto it = opts.find (k);
            return it != opts.end() ? it->second : def;
        }
        double optNum (const std::string& k, double def) const;
    };
    DeviceSpec parseSpec (const std::string& spec);

    std::unique_ptr<AudioDevice> createDevice (const std::string& spec, std::string& err);
    std::unique_ptr<AudioDevice> createNullDevice (const DeviceSpec& s);
    std::unique_ptr<AudioDevice> createFileDevice (const DeviceSpec& s);
    std::unique_ptr<AudioDevice> createWasapiDevice (const DeviceSpec& s);
#if NOVA_AUDIO_WITH_ASIO
    std::unique_ptr<AudioDevice> createAsioDevice (const DeviceSpec& s);
    json::Value listAsioDrivers();
#endif
    json::Value listWasapiEndpoints();
#if NOVA_AUDIO_PRO
    // Mode audio pro Nova (Source/pro/, propriétaire) : WASAPI exclusif / partagé faible latence, agrégation.
    std::unique_ptr<AudioDevice> createProDevice (const DeviceSpec& s);
    json::Value listProEndpoints();
    // Tests : faux énumérateur (aucun vrai périphérique) ; nullptr = celui de Windows.
    void setProEnumeratorFactory (HRESULT (*make) (::IMMDeviceEnumerator**));
    // Vrai dans les fils temps réel du mode pro (boucles de rendu / capture) : les tests vérifient
    // qu'aucune allocation n'y a lieu.
    bool inProRealtimeThread();
#endif
    json::Value listDevices (bool wasapi);

    // Fil temps réel : tâche MMCSS « Pro Audio » (une fois par fil) ; repli : priorité critique.
    void enterProAudioThread();
    void leaveProAudioThread();

    // Attente précise jusqu'à l'instant QPC donné (minuterie haute résolution + fin en boucle courte).
    class PreciseWaiter
    {
    public:
        PreciseWaiter();
        ~PreciseWaiter();
        void waitUntil (int64_t qpcTarget);
        static int64_t now();
        static double freq();
    private:
        HANDLE timer = nullptr;
    };

    // Bruit déterministe des tests (même formule dans le pilote factice et les tests Python) :
    // valeur exacte en float (24 bits), dans [-0,5 ; 0,5).
    inline float testNoise (uint64_t seed, uint32_t channel, uint64_t frame)
    {
        uint64_t z = frame * 0x9E3779B97F4A7C15ull + (uint64_t) channel * 0xD1B54A32D192ED03ull + seed;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z = z ^ (z >> 31);
        return (float) ((double) (z >> 40) / 16777216.0 - 0.5);
    }
}
