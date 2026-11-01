/*
    NovaAudioEngine — rééchantillonneur polyphase (voir Resampler.h).
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
#include "Resampler.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>

namespace nova::audio
{
    namespace
    {
        constexpr double kPi = 3.14159265358979323846;

        double besselI0 (double x)
        {
            double sum = 1.0, term = 1.0;
            const double q = x * x / 4.0;
            for (int k = 1; k < 200; ++k)
            {
                term *= q / ((double) k * (double) k);
                sum += term;
                if (term < sum * 1e-17) break;
            }
            return sum;
        }
    }

    bool Resampler::setup (int inRate, int outRate, int channels, int maxInBlock, double attenuationDb, double passHz)
    {
        if (inRate <= 0 || outRate <= 0 || channels <= 0 || maxInBlock <= 0) return false;
        const int g = std::gcd (inRate, outRate);
        L = outRate / g;
        M = inRate / g;
        chans = channels;
        maxIn = maxInBlock;
        if (L == M) { taps = 0; reset(); return true; }

        const double minRate = (double) std::min (inRate, outRate);
        fStop = minRate / 2.0;
        fPass = std::min (passHz, 0.4536 * minRate);
        const double df = fStop - fPass;
        const double fUp = (double) L * (double) inRate;
        const double A = attenuationDb;
        const double beta = A > 50 ? 0.1102 * (A - 8.7) : (A >= 21 ? 0.5842 * std::pow (A - 21, 0.4) + 0.07886 * (A - 21) : 0.0);
        const double nTotal = (A - 7.95) / (2.285 * 2.0 * kPi * df / fUp);
        taps = (int) std::ceil (nTotal / (double) L);
        taps += taps & 1;                        // pair : centre entier (longueur taps·L - 1, impaire)
        taps = std::max (taps, 8);

        // Prototype passe-bas à la fréquence L·inRate, longueur N = taps·L - 1, centre c entier.
        const int N = taps * L - 1;
        const int c = (N - 1) / 2;
        const double fc = (fPass + fStop) / 2.0;
        std::vector<double> h ((size_t) taps * L, 0.0);
        const double i0b = besselI0 (beta);
        double sum = 0;
        for (int m = 0; m < N; ++m)
        {
            const double t = (double) (m - c);
            const double x = 2.0 * fc / fUp * t;
            const double sinc = t == 0 ? 1.0 : std::sin (kPi * x) / (kPi * x);
            const double r = t / (double) c;
            const double w = besselI0 (beta * std::sqrt (std::max (0.0, 1.0 - r * r))) / i0b;
            h[(size_t) m] = 2.0 * fc / fUp * sinc * w;
            sum += h[(size_t) m];
        }
        // Gain : L (insertion de zéros), moyenne des phases = 1.
        coeffs.assign ((size_t) L * taps, 0.0f);
        for (int p = 0; p < L; ++p)
            for (int k = 0; k < taps; ++k)
            {
                const size_t idx = (size_t) k * L + (size_t) p;
                const double v = idx < h.size() ? h[idx] * (double) L / sum : 0.0;
                // inversé : coeffs[p][taps-1-k] multiplie x[n-k]
                coeffs[(size_t) p * taps + (size_t) (taps - 1 - k)] = (float) v;
            }

        // Phase de départ : sortie 0 à la position c (unités « haute fréquence ») -> pas de retard.
        startN = c / L;
        startPhase = c % L;
        cap = taps + maxInBlock + 8;
        buf.assign ((size_t) chans * cap, 0.0f);
        reset();
        return true;
    }

    void Resampler::reset()
    {
        if (L == M) return;
        std::fill (buf.begin(), buf.end(), 0.0f);
        // Historique : taps-1 zéros (entrée avant l'instant 0).
        fill = taps - 1;
        baseAbs = -(int64_t) (taps - 1);
        pushed = 0;
        nextN = startN;
        phase = startPhase;
    }

    int Resampler::inputNeeded (int nOut) const
    {
        if (L == M) return nOut;
        if (nOut <= 0) return 0;
        const int64_t last = nextN + ((int64_t) phase + (int64_t) (nOut - 1) * M) / L;
        const int64_t need = last + 1 - pushed;
        return need > 0 ? (int) need : 0;
    }

    int Resampler::process (const float* const* in, int nIn, float* const* out, int maxOut)
    {
        if (L == M)
        {
            const int n = std::min (nIn, maxOut);
            for (int ch = 0; ch < chans; ++ch)
                if (out[ch] != in[ch]) std::memcpy (out[ch], in[ch], sizeof (float) * (size_t) n);
            return n;
        }
        nIn = std::max (0, std::min (nIn, cap - fill));
        for (int ch = 0; ch < chans; ++ch)
            std::memcpy (&buf[(size_t) ch * cap + (size_t) fill], in[ch], sizeof (float) * (size_t) nIn);
        fill += nIn;
        pushed += nIn;

        int produced = 0;
        const int64_t lastAbs = pushed - 1;
        while (produced < maxOut && nextN <= lastAbs)
        {
            const int rel0 = (int) (nextN - baseAbs) - (taps - 1);
            const float* h = &coeffs[(size_t) phase * taps];
            for (int ch = 0; ch < chans; ++ch)
            {
                const float* x = &buf[(size_t) ch * cap + (size_t) rel0];
                float a0 = 0, a1 = 0, a2 = 0, a3 = 0;
                int k = 0;
                for (; k + 4 <= taps; k += 4)
                {
                    a0 += h[k] * x[k];
                    a1 += h[k + 1] * x[k + 1];
                    a2 += h[k + 2] * x[k + 2];
                    a3 += h[k + 3] * x[k + 3];
                }
                for (; k < taps; ++k) a0 += h[k] * x[k];
                out[ch][produced] = (a0 + a1) + (a2 + a3);
            }
            ++produced;
            phase += M;
            nextN += phase / L;
            phase %= L;
        }

        // Compactage : on garde ce qui sert encore (à partir de nextN - taps + 1).
        const int64_t keepFromAbs = std::min (nextN - (taps - 1), baseAbs + fill);
        const int drop = (int) (keepFromAbs - baseAbs);
        if (drop > 0)
        {
            const int keep = fill - drop;
            for (int ch = 0; ch < chans; ++ch)
            {
                float* b = &buf[(size_t) ch * cap];
                std::memmove (b, b + drop, sizeof (float) * (size_t) keep);
            }
            fill = keep;
            baseAbs += drop;
        }
        return produced;
    }
}
