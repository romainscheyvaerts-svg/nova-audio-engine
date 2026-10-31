/*
    NovaAudioEngine — fichiers WAV (voir WavFile.h).
    Copyright (C) 2026 Make Music.
    SPDX-License-Identifier: GPL-3.0-only
    Ce fichier fait partie de NovaAudioEngine, logiciel libre : vous pouvez le redistribuer et/ou le
    modifier selon les termes de la GNU General Public License version 3, publiée par la Free Software
    Foundation, avec la permission supplémentaire (article 7) décrite dans native-audio/LICENCES.md
    (pilotes audio et modules d'effets tiers chargés à l'exécution). Il est distribué SANS AUCUNE
    GARANTIE, sans même la garantie implicite de qualité marchande ou d'adéquation à un usage
    particulier. Texte de la licence : native-audio/LICENSE (https://www.gnu.org/licenses/gpl-3.0.html).
*/
#include "WavFile.h"

#include <algorithm>
#include <cstring>

namespace nova::audio
{
    namespace
    {
        void put32 (std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back ((uint8_t) (x >> (8 * i))); }
        void put16 (std::vector<uint8_t>& v, uint16_t x) { v.push_back ((uint8_t) x); v.push_back ((uint8_t) (x >> 8)); }
        void tag (std::vector<uint8_t>& v, const char* t) { v.insert (v.end(), t, t + 4); }

        std::string winError (const char* what)
        {
            char buf[256];
            std::snprintf (buf, sizeof (buf), "%s (erreur Windows %lu)", what, GetLastError());
            return buf;
        }

        bool writeAll (HANDLE h, const void* p, size_t n)
        {
            const uint8_t* b = (const uint8_t*) p;
            while (n > 0)
            {
                DWORD w = 0;
                const DWORD chunk = (DWORD) std::min<size_t> (n, 1u << 30);
                if (! WriteFile (h, b, chunk, &w, nullptr) || w == 0) return false;
                b += w;
                n -= w;
            }
            return true;
        }
    }

    std::vector<uint8_t> wavHeaderFloat32 (int channels, int sampleRate, uint64_t frames)
    {
        // Même en-tête que bridge-python/take_writer.py (_wav_header) : 58 octets.
        const uint32_t align = 4u * (uint32_t) channels;
        const uint64_t data64 = frames * align;
        const uint32_t data = (uint32_t) std::min<uint64_t> (data64, 0xFFFFFFFFull - kWavHeaderBytes);
        std::vector<uint8_t> v;
        v.reserve (kWavHeaderBytes);
        tag (v, "RIFF"); put32 (v, (uint32_t) (kWavHeaderBytes - 8) + data); tag (v, "WAVE");
        tag (v, "fmt "); put32 (v, 18); put16 (v, 3); put16 (v, (uint16_t) channels); put32 (v, (uint32_t) sampleRate);
        put32 (v, (uint32_t) sampleRate * align); put16 (v, (uint16_t) align); put16 (v, 32); put16 (v, 0);
        tag (v, "fact"); put32 (v, 4); put32 (v, (uint32_t) std::min<uint64_t> (frames, 0xFFFFFFFFull));
        tag (v, "data"); put32 (v, data);
        return v;
    }

    //==========================================================================
    bool WavWriter::open (const std::wstring& path, int channels, int sampleRate, std::string& err)
    {
        close();
        chans = std::max (1, channels);
        rate = sampleRate;
        nFrames = 0;
        lastError.clear();
        h = CreateFileW (path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (h == INVALID_HANDLE_VALUE) { err = winError ("fichier de prise non créé"); return false; }
        const auto hdr = wavHeaderFloat32 (chans, rate, 0);
        if (! writeAll (h, hdr.data(), hdr.size())) { err = winError ("en-tête WAV non écrit"); close(); return false; }
        pending.reserve (1 << 20);
        return true;
    }

    bool WavWriter::flushBuffer()
    {
        if (pending.empty()) return true;
        const bool ok = writeAll (h, pending.data(), pending.size());
        pending.clear();
        if (! ok) lastError = winError ("écriture impossible sur le disque");
        return ok;
    }

    bool WavWriter::write (const float* interleaved, int frames)
    {
        if (h == INVALID_HANDLE_VALUE || frames <= 0) return h != INVALID_HANDLE_VALUE;
        const size_t bytes = sizeof (float) * (size_t) chans * (size_t) frames;
        if (pending.size() + bytes > (1u << 20) && ! flushBuffer()) return false;
        if (bytes >= (1u << 20))
        {
            if (! writeAll (h, interleaved, bytes)) { lastError = winError ("écriture impossible sur le disque"); return false; }
        }
        else
        {
            const uint8_t* b = (const uint8_t*) interleaved;
            pending.insert (pending.end(), b, b + bytes);
        }
        nFrames += (uint64_t) frames;
        return true;
    }

    bool WavWriter::updateHeader (bool fsync)
    {
        if (h == INVALID_HANDLE_VALUE) return false;
        if (! flushBuffer()) return false;
        LARGE_INTEGER zero {}, cur {};
        SetFilePointerEx (h, zero, &cur, FILE_CURRENT);
        SetFilePointerEx (h, zero, nullptr, FILE_BEGIN);
        const auto hdr = wavHeaderFloat32 (chans, rate, nFrames);
        const bool ok = writeAll (h, hdr.data(), hdr.size());
        SetFilePointerEx (h, cur, nullptr, FILE_BEGIN);
        if (fsync) FlushFileBuffers (h);
        if (! ok) lastError = winError ("en-tête WAV non mis à jour");
        return ok;
    }

    bool WavWriter::truncateFrames (uint64_t frames)
    {
        if (h == INVALID_HANDLE_VALUE || frames >= nFrames) return true;
        if (! flushBuffer()) return false;
        LARGE_INTEGER p {};
        p.QuadPart = (LONGLONG) (kWavHeaderBytes + frames * 4ull * (uint64_t) chans);
        if (! SetFilePointerEx (h, p, nullptr, FILE_BEGIN) || ! SetEndOfFile (h)) { lastError = winError ("prise non coupée à sa fin"); return false; }
        nFrames = frames;
        return updateHeader (false);
    }

    bool WavWriter::close()
    {
        if (h == INVALID_HANDLE_VALUE) return true;
        const bool ok = updateHeader (true);
        CloseHandle (h);
        h = INVALID_HANDLE_VALUE;
        return ok;
    }

    //==========================================================================
    void WavReader::close()
    {
        if (h != INVALID_HANDLE_VALUE) CloseHandle (h);
        h = INVALID_HANDLE_VALUE;
    }

    bool WavReader::open (const std::wstring& path, std::string& err)
    {
        close();
        h = CreateFileW (path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (h == INVALID_HANDLE_VALUE) { err = winError ("fichier WAV introuvable"); return false; }
        auto rd = [this] (void* p, DWORD n) { DWORD got = 0; return ReadFile (h, p, n, &got, nullptr) && got == n; };
        uint8_t hdr[12];
        if (! rd (hdr, 12) || std::memcmp (hdr, "RIFF", 4) != 0 || std::memcmp (hdr + 8, "WAVE", 4) != 0) { err = "pas un fichier WAV"; close(); return false; }
        bool haveFmt = false;
        for (;;)
        {
            uint8_t ck[8];
            if (! rd (ck, 8)) { err = "chunk data introuvable"; close(); return false; }
            uint32_t size;
            std::memcpy (&size, ck + 4, 4);
            if (std::memcmp (ck, "fmt ", 4) == 0)
            {
                std::vector<uint8_t> f (size);
                if (! rd (f.data(), size)) { err = "fmt illisible"; close(); return false; }
                uint16_t tagv, ch, al, bps;
                uint32_t sr;
                std::memcpy (&tagv, &f[0], 2); std::memcpy (&ch, &f[2], 2); std::memcpy (&sr, &f[4], 4);
                std::memcpy (&al, &f[12], 2); std::memcpy (&bps, &f[14], 2);
                if (tagv == 0xFFFE && size >= 26) std::memcpy (&tagv, &f[24], 2);   // sous-format EXTENSIBLE
                format = tagv; chans = ch; rate = (int) sr; align = al; bits = bps;
                haveFmt = true;
                if (size & 1) { uint8_t pad; rd (&pad, 1); }
            }
            else if (std::memcmp (ck, "data", 4) == 0)
            {
                if (! haveFmt) { err = "fmt manquant"; close(); return false; }
                LARGE_INTEGER zero {}, cur {};
                SetFilePointerEx (h, zero, &cur, FILE_CURRENT);
                dataStart = (uint64_t) cur.QuadPart;
                LARGE_INTEGER fs {};
                GetFileSizeEx (h, &fs);
                const uint64_t avail = (uint64_t) fs.QuadPart - dataStart;
                total = std::min<uint64_t> (size == 0 ? avail : size, avail) / (uint64_t) std::max (1, align);
                pos = 0;
                break;
            }
            else
            {
                LARGE_INTEGER skip {};
                skip.QuadPart = (LONGLONG) size + (size & 1);
                SetFilePointerEx (h, skip, nullptr, FILE_CURRENT);
            }
        }
        const bool okFmt = (format == 1 && (bits == 16 || bits == 24 || bits == 32)) || (format == 3 && (bits == 32 || bits == 64));
        if (! okFmt || chans <= 0) { err = "format WAV non pris en charge"; close(); return false; }
        return true;
    }

    bool WavReader::seekFrame (uint64_t frame)
    {
        if (h == INVALID_HANDLE_VALUE) return false;
        pos = std::min (frame, total);
        LARGE_INTEGER p {};
        p.QuadPart = (LONGLONG) (dataStart + pos * (uint64_t) align);
        return SetFilePointerEx (h, p, nullptr, FILE_BEGIN) != 0;
    }

    int WavReader::read (float* out, int frames)
    {
        if (h == INVALID_HANDLE_VALUE) return 0;
        frames = (int) std::min<uint64_t> ((uint64_t) std::max (0, frames), total - pos);
        if (frames <= 0) return 0;
        raw.resize ((size_t) frames * (size_t) align);
        DWORD got = 0;
        if (! ReadFile (h, raw.data(), (DWORD) raw.size(), &got, nullptr)) return 0;
        frames = (int) (got / (DWORD) align);
        const int bytesPer = bits / 8;
        const size_t n = (size_t) frames * (size_t) chans;
        const uint8_t* s = raw.data();
        for (size_t i = 0; i < n; ++i, s += bytesPer)
        {
            float v = 0;
            if (format == 3 && bits == 32) std::memcpy (&v, s, 4);
            else if (format == 3) { double d; std::memcpy (&d, s, 8); v = (float) d; }
            else if (bits == 16) { int16_t x; std::memcpy (&x, s, 2); v = (float) x / 32768.0f; }
            else if (bits == 24) { const int32_t x = (int32_t) ((uint32_t) s[0] << 8 | (uint32_t) s[1] << 16 | (uint32_t) s[2] << 24) >> 8; v = (float) x / 8388608.0f; }
            else { int32_t x; std::memcpy (&x, s, 4); v = (float) ((double) x / 2147483648.0); }
            out[i] = v;
        }
        pos += (uint64_t) frames;
        return frames;
    }
}
