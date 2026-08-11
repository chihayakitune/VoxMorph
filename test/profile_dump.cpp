// profile_dump.cpp — offline VoiceProfile measurement (v0.37.0).
//
// This is how the MEASURED entries in dsp/SampleTargetCatalog.h are produced.
// It is not a test: it takes a recording of a target character and prints the
// profile, either as a human-readable report or as a ready-to-paste catalog
// entry. Nothing in the catalog should be typed by hand -- regenerate with
// this instead, so the numbers in the header always trace back to a
// measurement someone can repeat.
//
//   c++ -std=c++17 -O2 -o profile_dump test/profile_dump.cpp
//
//   ./profile_dump take.wav                    # per-20 s windows, then FULL
//   ./profile_dump take.wav 12 20              # one window: start 12 s, 20 s long
//   ./profile_dump take.wav --cxx uru Uru うる  # SampleTargetCatalog.h entry
//
// The --cxx form measures the WHOLE file (VoiceAnalyzer's 20 s cap is a
// responsiveness budget for the interactive path, not a limit on the
// estimator). The windowed report exists to see whether a recording is
// consistent enough to summarize with one profile at all: the four v0.37.0
// characters vary by roughly 2 st in median f0 and 3-6 dB in tilt between
// their 20 s windows, which is ordinary performance variation -- a recording
// that swings much more than that is probably two different voices.
//
// Deliberately no JUCE: reads RIFF/WAVE (PCM 8/16/24/32-bit and float32,
// any channel count, downmixed to mono) directly, so it builds with a plain
// compiler like the rest of test/.
#include "../dsp/VoiceAnalyzer.h"
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

static bool readWav (const char* path, std::vector<float>& out, double& fs)
{
    FILE* f = std::fopen (path, "rb");
    if (f == nullptr) return false;
    char id[4] = {}; uint32_t sz = 0;
    if (std::fread (id, 1, 4, f) != 4 || std::memcmp (id, "RIFF", 4) != 0)
    { std::fclose (f); return false; }
    if (std::fread (&sz, 4, 1, f) != 1 || std::fread (id, 1, 4, f) != 4)
    { std::fclose (f); return false; }

    int ch = 1, bits = 16; uint16_t fmt = 1;
    while (std::fread (id, 1, 4, f) == 4)
    {
        if (std::fread (&sz, 4, 1, f) != 1) break;
        if (std::memcmp (id, "fmt ", 4) == 0)
        {
            uint16_t nch = 1, ba = 0, bps = 16; uint32_t sr = 0, br = 0;
            std::fread (&fmt, 2, 1, f); std::fread (&nch, 2, 1, f);
            std::fread (&sr,  4, 1, f); std::fread (&br,  4, 1, f);
            std::fread (&ba,  2, 1, f); std::fread (&bps, 2, 1, f);
            ch = nch; bits = bps; fs = sr;
            if (sz > 16) std::fseek (f, (long) sz - 16, SEEK_CUR);
        }
        else if (std::memcmp (id, "data", 4) == 0)
        {
            const int bytes = std::max (1, bits / 8);
            if (ch < 1) { std::fclose (f); return false; }
            const size_t nf = sz / (size_t) (bytes * ch);
            std::vector<uint8_t> raw (sz);
            if (std::fread (raw.data(), 1, sz, f) != sz) { std::fclose (f); return false; }
            out.resize (nf);
            for (size_t i = 0; i < nf; ++i)
            {
                double acc = 0.0;
                for (int c = 0; c < ch; ++c)
                {
                    const uint8_t* p = &raw[(i * (size_t) ch + (size_t) c) * (size_t) bytes];
                    if      (fmt == 3 && bits == 32) { float   v; std::memcpy (&v, p, 4); acc += v; }
                    else if (bits == 16)             { int16_t v; std::memcpy (&v, p, 2); acc += v / 32768.0; }
                    else if (bits == 24)             { const int32_t v = (int32_t) ((uint32_t) p[0] << 8
                                                                                 | (uint32_t) p[1] << 16
                                                                                 | (uint32_t) p[2] << 24);
                                                       acc += (v >> 8) / 8388608.0; }
                    else if (bits == 32)             { int32_t v; std::memcpy (&v, p, 4); acc += v / 2147483648.0; }
                    else if (bits == 8)              { acc += ((int) *p - 128) / 128.0; }
                }
                out[i] = (float) (acc / ch);
            }
            std::fclose (f);
            return fs > 0.0;
        }
        else std::fseek (f, (long) ((sz + 1) & ~1u), SEEK_CUR);   // chunks are word-aligned
    }
    std::fclose (f);
    return false;
}

static const char* kVowelNames[5] = { "A", "I", "U", "E", "O" };

static void report (const char* tag, const VoiceProfile& p)
{
    std::printf ("%-22s frames=%4d f0=%7.2f spread=%5.2f  F=%7.1f %7.1f %7.1f  "
                 "L=%6.2f %6.2f %6.2f  tilt=%6.2f  rel=%.2f %.2f %.2f  "
                 "hnr=%5.2f %5.2f %5.2f  hf=%6.2f  tract=%.3f  vowels=%d\n",
                 tag, p.voicedFrames, p.f0Hz, p.f0SpreadSt,
                 p.F[0], p.F[1], p.F[2], p.L[0], p.L[1], p.L[2], p.tiltDb,
                 p.rel[0], p.rel[1], p.rel[2], p.hnr[0], p.hnr[1], p.hnr[2],
                 p.hfDb, p.tractScale, p.vowelsMeasured());
    for (int v = 0; v < 5; ++v)
    {
        const auto& q = p.vow[v];
        // '*' marks a vowel with too few frames to be used (VowelProfile::valid)
        std::printf ("    %s n=%4d%s f0=%7.2f F=%7.1f %7.1f %7.1f L=%6.2f %6.2f %6.2f "
                     "rel=%.2f %.2f %.2f hnr=%5.2f %5.2f %5.2f\n",
                     kVowelNames[v], q.frames, q.valid() ? " " : "*", q.f0Hz,
                     q.F[0], q.F[1], q.F[2], q.L[0], q.L[1], q.L[2],
                     q.rel[0], q.rel[1], q.rel[2], q.hnr[0], q.hnr[1], q.hnr[2]);
    }
}

// A SampleTargetEntry initializer, field order matching VoiceProfile /
// VowelProfile exactly. Printed with enough digits to be the measurement and
// no more -- these are medians over hundreds of frames, not exact quantities.
static void emitCxx (const VoiceProfile& p, const char* id,
                     const char* en, const char* jp)
{
    std::printf ("        { \"%s\", \"%s\", \"%s\",\n", id, en, jp);
    std::printf ("          { /* f0     */ %.2ff,\n", p.f0Hz);
    std::printf ("            /* spread */ %.2ff,\n", p.f0SpreadSt);
    std::printf ("            /* F      */ { %.1ff, %.1ff, %.1ff },\n", p.F[0], p.F[1], p.F[2]);
    std::printf ("            /* L      */ { %.2ff, %.2ff, %.2ff },\n", p.L[0], p.L[1], p.L[2]);
    std::printf ("            /* tilt   */ %.2ff,\n", p.tiltDb);
    std::printf ("            /* frames */ %d,\n", p.voicedFrames);
    std::printf ("            /* rel    */ { %.2ff, %.2ff, %.2ff },\n", p.rel[0], p.rel[1], p.rel[2]);
    std::printf ("            /* hnr    */ { %.2ff, %.2ff, %.2ff },\n", p.hnr[0], p.hnr[1], p.hnr[2]);
    std::printf ("            /* hfDb   */ %.2ff,\n", p.hfDb);
    std::printf ("            /* tract  */ %.3ff,\n", p.tractScale);
    std::printf ("            /* vow    */ {\n");
    for (int v = 0; v < 5; ++v)
    {
        const auto& q = p.vow[v];
        std::printf ("              /* %s */ { %d, %.2ff, { %.1ff, %.1ff, %.1ff },"
                     " { %.2ff, %.2ff, %.2ff }, { %.2ff, %.2ff, %.2ff },"
                     " { %.2ff, %.2ff, %.2ff } }%s\n",
                     kVowelNames[v], q.frames, q.f0Hz, q.F[0], q.F[1], q.F[2],
                     q.L[0], q.L[1], q.L[2], q.rel[0], q.rel[1], q.rel[2],
                     q.hnr[0], q.hnr[1], q.hnr[2], v < 4 ? "," : "");
    }
    std::printf ("            } }\n        },\n");
}

int main (int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf (stderr,
            "usage: profile_dump <file.wav>                    windowed report + FULL\n"
            "       profile_dump <file.wav> <startSec> <lenSec> one window\n"
            "       profile_dump <file.wav> --cxx <id> <En> <Jp>  catalog entry\n");
        return 2;
    }

    std::vector<float> x; double fs = 0.0;
    if (! readWav (argv[1], x, fs))
    { std::fprintf (stderr, "profile_dump: cannot read %s\n", argv[1]); return 1; }

    // "measure everything" -- see the note on maxSec in VoiceAnalyzer::analyze
    constexpr double kNoCap = 1.0e9;

    if (argc >= 3 && std::string (argv[2]) == "--cxx")
    {
        if (argc < 6) { std::fprintf (stderr, "--cxx needs <id> <En> <Jp>\n"); return 2; }
        emitCxx (VoiceAnalyzer::analyze (x.data(), (int) x.size(), fs, kNoCap),
                 argv[3], argv[4], argv[5]);
        return 0;
    }

    std::printf ("# %s  fs=%.0f  length=%.2f s  samples=%zu\n",
                 argv[1], fs, (double) x.size() / fs, x.size());

    if (argc >= 4)
    {
        const int s = std::max (0, (int) (std::atof (argv[2]) * fs));
        const int n = std::min ((int) x.size() - s, (int) (std::atof (argv[3]) * fs));
        if (n <= 0) { std::fprintf (stderr, "empty window\n"); return 1; }
        char tag[64];
        std::snprintf (tag, sizeof tag, "[%ss +%ss]", argv[2], argv[3]);
        report (tag, VoiceAnalyzer::analyze (x.data() + s, n, fs, kNoCap));
        return 0;
    }

    const int win = (int) (fs * 20.0);
    for (int s = 0, k = 0; s + win / 2 < (int) x.size(); s += win, ++k)
    {
        const int n = std::min (win, (int) x.size() - s);
        char tag[64];
        std::snprintf (tag, sizeof tag, "win%d(%ds)", k, (int) (s / fs));
        report (tag, VoiceAnalyzer::analyze (x.data() + s, n, fs, kNoCap));
    }
    std::printf ("\n");
    report ("FULL", VoiceAnalyzer::analyze (x.data(), (int) x.size(), fs, kNoCap));
    return 0;
}
