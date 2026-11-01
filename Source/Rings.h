/*
    NovaAudioEngine — anneaux d'images audio sans verrou (un producteur, un ou plusieurs lecteurs).

    Positions ABSOLUES (n° d'image depuis le démarrage, 64 bits) : un lecteur sait toujours où il
    en est et combien d'images il a manquées. Les images sont entrelacées (canal le plus rapide).
    L'état (RingState, 64 octets) peut vivre en mémoire partagée : le pont Python le lit avec les
    décalages publiés par la commande « start » (voir SharedEngine.h).

    Deux usages :
      - « écrasement » (capture, aperçu vers le pont) : le producteur n'attend JAMAIS. Il annonce
        la zone qu'il va réécrire (claim) avant d'y écrire, puis publie write. Un lecteur copie,
        relit claim et jette ce qui a pu être réécrit pendant sa copie (comme un seqlock).
      - « borné » (lecture du mix venu du pont) : le producteur écrit seulement dans la place
        libre (write - read < capacité), le lecteur avance read.

    Code pour Windows x64 (ordre des écritures TSO) : les barrières C++ ci-dessous suffisent aussi
    au compilateur ; le pont Python, qui écrit des entiers 64 bits alignés, est cohérent avec ça.

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

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace nova::audio
{
    struct RingState
    {
        std::atomic<uint64_t> write { 0 };   // images publiées
        std::atomic<uint64_t> claim { 0 };   // images en cours d'écriture (>= write)
        std::atomic<uint64_t> read { 0 };    // lecteur (mode borné seulement)
        uint32_t capacity = 0;               // images, puissance de 2
        uint32_t channels = 0;
        uint64_t dataOffset = 0;             // en mémoire partagée : décalage des données
        uint64_t reserved[3] {};
    };
    static_assert (sizeof (RingState) == 64, "RingState : 64 octets");

    inline uint32_t nextPow2 (uint64_t v)
    {
        uint32_t p = 1;
        while (p < v && p < (1u << 30)) p <<= 1;
        return p;
    }

    class FrameRing
    {
    public:
        void attach (RingState* s, float* d) { st = s; data = d; mask = s->capacity - 1; }
        bool valid() const { return st != nullptr && data != nullptr; }
        RingState* state() const { return st; }
        uint32_t capacity() const { return st->capacity; }
        uint32_t channels() const { return st->channels; }
        uint64_t written() const { return st->write.load (std::memory_order_acquire); }

        //---------------------------------------------------------------- producteur (écrasement)
        // Bloc planaire (nch canaux fournis, les autres canaux de l'anneau à zéro).
        void writePlanar (const float* const* planar, int nch, int frames)
        {
            const uint32_t ch = st->channels;
            const uint64_t w = st->write.load (std::memory_order_relaxed);
            frames = std::min<int> (frames, (int) st->capacity);
            st->claim.store (w + (uint64_t) frames, std::memory_order_relaxed);
            std::atomic_thread_fence (std::memory_order_seq_cst);
            for (int i = 0; i < frames; ++i)
            {
                float* f = data + (size_t) ((w + (uint64_t) i) & mask) * ch;
                uint32_t c = 0;
                for (; c < (uint32_t) nch && c < ch; ++c) f[c] = planar[c] != nullptr ? planar[c][i] : 0.0f;
                for (; c < ch; ++c) f[c] = 0.0f;
            }
            st->write.store (w + (uint64_t) frames, std::memory_order_release);
        }

        void writeSilence (int frames)
        {
            const uint32_t ch = st->channels;
            const uint64_t w = st->write.load (std::memory_order_relaxed);
            frames = std::min<int> (frames, (int) st->capacity);
            st->claim.store (w + (uint64_t) frames, std::memory_order_relaxed);
            std::atomic_thread_fence (std::memory_order_seq_cst);
            for (int i = 0; i < frames; ++i)
                std::memset (data + (size_t) ((w + (uint64_t) i) & mask) * ch, 0, sizeof (float) * ch);
            st->write.store (w + (uint64_t) frames, std::memory_order_release);
        }

        // Bloc entrelacé de exactement channels() canaux.
        void writeInterleaved (const float* src, int frames)
        {
            const uint32_t ch = st->channels;
            const uint64_t w = st->write.load (std::memory_order_relaxed);
            frames = std::min<int> (frames, (int) st->capacity);
            st->claim.store (w + (uint64_t) frames, std::memory_order_relaxed);
            std::atomic_thread_fence (std::memory_order_seq_cst);
            const uint64_t p = w & mask;
            const uint64_t first = std::min<uint64_t> ((uint64_t) frames, st->capacity - p);
            std::memcpy (data + p * ch, src, sizeof (float) * ch * first);
            if ((uint64_t) frames > first)
                std::memcpy (data, src + first * ch, sizeof (float) * ch * ((uint64_t) frames - first));
            st->write.store (w + (uint64_t) frames, std::memory_order_release);
        }

        //---------------------------------------------------------------- lecteur (écrasement)
        // Copie au plus maxFrames images à partir de pos (avancé). « skipped » reçoit les images
        // perdues (réécrites avant d'être lues) : le lecteur saute par-dessus, pos reste absolu.
        int readOverwrite (uint64_t& pos, float* dst, int maxFrames, uint64_t& skipped) const
        {
            const uint32_t ch = st->channels;
            for (int attempt = 0; attempt < 4; ++attempt)
            {
                const uint64_t w = st->write.load (std::memory_order_acquire);
                if (pos > w) pos = w;
                // claim peut dépasser write (écriture en cours) : ces positions - capacité sont en danger.
                const uint64_t c0 = st->claim.load (std::memory_order_acquire);
                const uint64_t lim = std::max (w, c0);
                const uint64_t oldest = lim > st->capacity ? lim - st->capacity : 0;
                if (pos < oldest) { skipped += oldest - pos; pos = oldest; }
                const int n = (int) std::min<uint64_t> ((uint64_t) maxFrames, w - pos);
                if (n <= 0) return 0;
                const uint64_t p = pos & mask;
                const uint64_t first = std::min<uint64_t> ((uint64_t) n, st->capacity - p);
                std::memcpy (dst, data + p * ch, sizeof (float) * ch * first);
                if ((uint64_t) n > first)
                    std::memcpy (dst + first * ch, data, sizeof (float) * ch * ((uint64_t) n - first));
                std::atomic_thread_fence (std::memory_order_seq_cst);
                const uint64_t c1 = st->claim.load (std::memory_order_relaxed);
                const uint64_t badEnd = c1 > st->capacity ? c1 - st->capacity : 0;
                if (badEnd <= pos) { pos += (uint64_t) n; return n; }
                // Une partie a été réécrite pendant la copie : on recommence plus loin.
            }
            return 0;
        }

        //---------------------------------------------------------------- mode borné
        uint64_t available (uint64_t readPos) const { return st->write.load (std::memory_order_acquire) - readPos; }

        // Lecteur borné (fil audio) : lit n images à partir de read, sans avancer.
        void peekPlanar (uint64_t readPos, float* const* planar, int nch, int frames, int outOffset) const
        {
            const uint32_t ch = st->channels;
            for (int i = 0; i < frames; ++i)
            {
                const float* f = data + (size_t) ((readPos + (uint64_t) i) & mask) * ch;
                for (int c = 0; c < nch && c < (int) ch; ++c)
                    if (planar[c] != nullptr) planar[c][outOffset + i] += f[c];
            }
        }
        void peekInterleaved (uint64_t readPos, float* dst, int frames) const
        {
            const uint32_t ch = st->channels;
            const uint64_t p = readPos & mask;
            const uint64_t first = std::min<uint64_t> ((uint64_t) frames, st->capacity - p);
            std::memcpy (dst, data + p * ch, sizeof (float) * ch * first);
            if ((uint64_t) frames > first)
                std::memcpy (dst + first * ch, data, sizeof (float) * ch * ((uint64_t) frames - first));
        }
        // Producteur borné (tests C++ ; le vrai producteur est le pont Python) : écrit ce qui tient.
        int writeBounded (const float* src, int frames)
        {
            const uint64_t w = st->write.load (std::memory_order_relaxed);
            const uint64_t r = st->read.load (std::memory_order_acquire);
            const int n = (int) std::min<uint64_t> ((uint64_t) frames, st->capacity - (w - r));
            if (n <= 0) return 0;
            const uint32_t ch = st->channels;
            const uint64_t p = w & mask;
            const uint64_t first = std::min<uint64_t> ((uint64_t) n, st->capacity - p);
            std::memcpy (data + p * ch, src, sizeof (float) * ch * first);
            if ((uint64_t) n > first) std::memcpy (data, src + first * ch, sizeof (float) * ch * ((uint64_t) n - first));
            st->write.store (w + (uint64_t) n, std::memory_order_release);
            return n;
        }

    private:
        RingState* st = nullptr;
        float* data = nullptr;
        uint64_t mask = 0;
    };

    // Petite file d'événements (fil audio -> fil de capture), sans verrou, taille fixe.
    template <typename T, int N>
    class EventQueue
    {
    public:
        bool push (const T& v)
        {
            const uint32_t w = wpos.load (std::memory_order_relaxed);
            if (w - rpos.load (std::memory_order_acquire) >= (uint32_t) N) { dropped.fetch_add (1, std::memory_order_relaxed); return false; }
            items[w % N] = v;
            wpos.store (w + 1, std::memory_order_release);
            return true;
        }
        bool pop (T& v)
        {
            const uint32_t r = rpos.load (std::memory_order_relaxed);
            if (r == wpos.load (std::memory_order_acquire)) return false;
            v = items[r % N];
            rpos.store (r + 1, std::memory_order_release);
            return true;
        }
        std::atomic<uint32_t> dropped { 0 };
    private:
        T items[N] {};
        std::atomic<uint32_t> wpos { 0 }, rpos { 0 };
    };
}
