/*
    NovaAudioEngine — fichiers WAV.

    WavWriter : WAV IEEE float 32 bits, EXACTEMENT le format des prises du pont
    (bridge-python/take_writer.py, en-tête de 58 octets : RIFF + fmt (18) + fact + data), pour que
    `repair_wav` et la page relisent les prises du moteur comme celles du pont. En-tête réécrit à la
    demande (fichier lisible après un plantage), écriture tamponnée (1 Mo).
    WavReader : PCM 16/24/32 bits, float 32/64 bits (WAVE_FORMAT_EXTENSIBLE compris).

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

#ifndef NOMINMAX
 #define NOMINMAX 1
#endif
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace nova::audio
{
    constexpr int kWavHeaderBytes = 58;

    class WavWriter
    {
    public:
        ~WavWriter() { close(); }
        bool open (const std::wstring& path, int channels, int sampleRate, std::string& err);
        bool write (const float* interleaved, int frames);       // tamponné
        bool updateHeader (bool fsync);                          // vide le tampon, en-tête à jour
        bool truncateFrames (uint64_t frames);                   // coupe le fichier à « frames » images
        bool close();
        bool isOpen() const { return h != INVALID_HANDLE_VALUE; }
        uint64_t frames() const { return nFrames; }
        int channels() const { return chans; }
        const std::string& error() const { return lastError; }

    private:
        bool flushBuffer();
        HANDLE h = INVALID_HANDLE_VALUE;
        int chans = 1, rate = 48000;
        uint64_t nFrames = 0;
        std::vector<uint8_t> pending;
        std::string lastError;
    };

    class WavReader
    {
    public:
        ~WavReader() { close(); }
        bool open (const std::wstring& path, std::string& err);
        // Lit au plus frames images en float entrelacé ; rend le nombre lu (0 à la fin).
        int read (float* interleaved, int frames);
        bool seekFrame (uint64_t frame);
        void close();
        int channels() const { return chans; }
        int sampleRate() const { return rate; }
        uint64_t totalFrames() const { return total; }
        uint64_t position() const { return pos; }

    private:
        HANDLE h = INVALID_HANDLE_VALUE;
        int chans = 0, rate = 0, bits = 0, format = 0, align = 0;
        uint64_t dataStart = 0, total = 0, pos = 0;
        std::vector<uint8_t> raw;
    };

    std::vector<uint8_t> wavHeaderFloat32 (int channels, int sampleRate, uint64_t frames);
}
