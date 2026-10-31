/*
    NovaAudioEngine — zone de mémoire partagée avec le client (protocole : native-audio/PROTOCOLE.md).

    Contenu GÉNÉRIQUE de serveur audio, sans aucune structure propre à un logiciel client :
    échantillons audio flottants et compteurs. Créée par le moteur au démarrage du flux (nom
    « Local\NovaAudio-<pid>-<n> », rendu par la commande start avec tous les décalages : le client
    les vérifie et refuse une zone qui ne correspond pas). Elle porte :
      - l'état du flux, l'horloge et les compteurs (lus par le client sans aller-retour JSON) ;
      - l'anneau « aperçu » (moteur -> client) : les entrées de la carte à la fréquence demandée
        (project_rate), n° d'image absolu = même numérotation que les prises (rec_start) ;
      - l'anneau « lecture » (client -> moteur) : l'audio à jouer, à la même fréquence, une
        colonne par sortie de la carte.
    Les anneaux sont décrits dans Rings.h. Rien d'autre n'y passe : les prises sont écrites sur le
    disque par le moteur lui-même (fichiers WAV).

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

#include "Rings.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace nova::audio::shm
{
    constexpr uint32_t kMagic = 0x3141564Eu;   // « NVA1 »
    constexpr uint32_t kVersion = 1;
    constexpr int kMaxChannels = 32;

    struct Header
    {
        uint32_t magic;
        uint32_t version;
        uint32_t headerSize;
        uint32_t reserved0;
        uint64_t totalSize;

        // ---- flux (écrit par le moteur au démarrage)
        uint32_t running;
        uint32_t projectRate;
        uint32_t deviceRate;
        uint32_t blockFrames;
        uint32_t inChannels;
        uint32_t outChannels;
        int32_t inputLatency;        // images de la carte (annoncé par le pilote)
        int32_t outputLatency;
        int32_t playLookahead;       // images (projet) d'avance prises par le rééchantillonneur de lecture
        uint32_t reserved1;

        // ---- horloge (séquence paire = stable) : n° d'image projet <-> heure QPC (s) de son entrée
        std::atomic<uint64_t> clockSeq;
        double clockFrame;
        double clockTime;

        // ---- compteurs (moteur)
        std::atomic<uint64_t> callbacks;
        std::atomic<uint64_t> deviceFrames;
        std::atomic<uint64_t> lateCallbacks;      // callback plus long que la durée du bloc
        std::atomic<uint64_t> deviceXruns;        // pertes / surcharges vues par le périphérique
        std::atomic<uint64_t> deviceLostFrames;   // images perdues par la carte (silence inséré)
        std::atomic<uint64_t> captureLostFrames;  // images perdues entre le fil audio et la capture
        std::atomic<uint64_t> playUnderruns;      // lecture vide en plein jeu (le pont est en retard)
        std::atomic<uint64_t> playDroppedFrames;  // lecture trop en avance : images sautées
        std::atomic<uint64_t> playFrames;
        std::atomic<uint64_t> cbLastNs;
        std::atomic<uint64_t> cbMaxNs;
        std::atomic<uint64_t> cbTotalNs;

        // ---- lecture
        std::atomic<uint32_t> playTargetBlocks;   // marge actuelle (adaptative, moteur)
        std::atomic<uint32_t> playMinBlocks;      // marge de départ (pont)
        std::atomic<uint32_t> monitorLatency;     // latence de la chaîne de retour (images carte)
        uint32_t reserved2;

        float inPeak[kMaxChannels];               // crêtes (le pont les remet à zéro en les lisant)
        float outPeak[kMaxChannels];

        RingState preview;                        // moteur -> pont
        RingState play;                           // pont -> moteur
    };

    constexpr size_t align64 (size_t v) { return (v + 63) / 64 * 64; }
}
