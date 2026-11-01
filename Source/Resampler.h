/*
    NovaAudioEngine — rééchantillonneur polyphase (sinc fenêtré de Kaiser), rapport rationnel L/M.

    Cible (comparatif, écart 2) : plat à ±0,1 dB jusqu'à 20 kHz, repliement et images sous -90 dB.
    Le filtre est calculé pour ~100 dB d'atténuation : bande passante jusqu'à min(20 kHz, 0,4535 × la
    plus petite fréquence), bande coupée à partir de la plus petite fréquence de Nyquist. Nombre de
    coefficients par phase = ce qu'il faut pour cette pente (environ 140 à 160 pour 44,1 <-> 48 kHz).

    Sans retard : la sortie j correspond exactement à l'instant j·M/L de l'entrée (le retard du
    filtre est compensé par la phase de départ) ; il faut seulement « taps/2 » images d'avance.
    Flux continu, plusieurs canaux (planaires), aucune allocation après setup().

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

#include <cstdint>
#include <vector>

namespace nova::audio
{
    class Resampler
    {
    public:
        // maxInBlock : plus grand bloc d'entrée passé à process().
        bool setup (int inRate, int outRate, int channels, int maxInBlock, double attenuationDb = 100.0, double passHz = 20000.0);
        void reset();

        bool isPassthrough() const { return L == M; }
        int upFactor() const { return L; }
        int downFactor() const { return M; }
        int tapsPerPhase() const { return taps; }
        double passbandHz() const { return fPass; }
        double stopbandHz() const { return fStop; }
        // Images d'entrée nécessaires en avance (latence de la conversion, en images d'entrée).
        int lookahead() const { return taps / 2; }

        // Entrée nIn images (nIn <= maxInBlock), sortie au plus maxOut images ; rend le nombre produit.
        int process (const float* const* in, int nIn, float* const* out, int maxOut);
        // Images d'entrée encore nécessaires pour pouvoir produire nOut images.
        int inputNeeded (int nOut) const;

    private:
        int L = 1, M = 1, taps = 0, chans = 0, cap = 0, maxIn = 0;
        double fPass = 0, fStop = 0;
        std::vector<float> coeffs;     // [phase][tap], coefficients inversés (produit scalaire contigu)
        std::vector<float> buf;        // [canal][cap] historique + entrée
        int fill = 0;                  // images valides dans buf (par canal)
        int64_t baseAbs = 0;           // n° absolu de buf[0]
        int64_t pushed = 0;            // images d'entrée reçues
        int64_t nextN = 0;             // n° absolu de l'image d'entrée « courante » de la prochaine sortie
        int phase = 0;
        int64_t startN = 0;
        int startPhase = 0;
    };
}
