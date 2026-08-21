// pulse_render.cpp - render a REAL recording through the engine (v0.52.0).
//
// Written for the Pulse Softness A/B: the question "is the output too
// pulse-like?" cannot be answered on a synthetic vowel, because a synthetic
// vowel has no glottal pulse shape of its own to preserve or lose. This
// takes the user's own take, runs it through PsolaEngine with one parameter
// varied, and writes a wav that can be measured AND listened to.
//
// Build:  g++ -O2 -std=c++17 -I dsp -o /tmp/pr test/pulse_render.cpp
// Run:    /tmp/pr <in.wav> <out.wav> <pitchSt> <formantSt> <disperse> [smooth]
//           smooth: 1 (default) = Pulse Smoothing on, as the plugin ships
//           body:   Pulse Body 0..1 (0 = the v0.51.0 grain width)
//
// Recordings stay OUT of the repository (project policy) - pass a path.
#include "PsolaEngine.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>

static bool loadWav (const char* path, std::vector<float>& out, double& fs)
{
    FILE* f = std::fopen (path, "rb");
    if (! f) return false;
    std::vector<unsigned char> d;
    unsigned char buf[65536]; size_t n;
    while ((n = std::fread (buf, 1, sizeof buf, f)) > 0) d.insert (d.end(), buf, buf + n);
    std::fclose (f);
    if (d.size() < 44 || std::memcmp (d.data(), "RIFF", 4)) return false;
    size_t i = 12; int ch = 1, bits = 16, fmt = 1; fs = 44100;
    while (i + 8 <= d.size())
    {
        const char* id = (const char*) &d[i];
        unsigned sz; std::memcpy (&sz, &d[i+4], 4);
        if (! std::memcmp (id, "fmt ", 4))
        {
            unsigned short t; std::memcpy (&t, &d[i+8],  2); fmt  = t;
            unsigned short c; std::memcpy (&c, &d[i+10], 2); ch   = c;
            unsigned r;       std::memcpy (&r, &d[i+12], 4); fs   = r;
            unsigned short b; std::memcpy (&b, &d[i+22], 2); bits = b;
        }
        else if (! std::memcmp (id, "data", 4))
        {
            const size_t bytes = std::min ((size_t) sz, d.size() - i - 8);
            const size_t ns    = bytes / (size_t) (bits / 8);
            out.assign (ns / (size_t) ch, 0.0f);
            for (size_t k = 0; k < out.size(); ++k)
            {
                double acc = 0.0;                       // mono-sum
                for (int c = 0; c < ch; ++c)
                {
                    const unsigned char* p = &d[i + 8 + (k * (size_t) ch + (size_t) c) * (size_t) (bits / 8)];
                    if (bits == 16)      { int16_t v; std::memcpy (&v, p, 2); acc += v / 32768.0; }
                    else if (bits == 24) { int32_t v = (p[0] << 8) | (p[1] << 16) | (p[2] << 24); acc += (v >> 8) / 8388608.0; }
                    else if (bits == 32 && fmt == 3) { float v; std::memcpy (&v, p, 4); acc += v; }
                    else if (bits == 32) { int32_t v; std::memcpy (&v, p, 4); acc += v / 2147483648.0; }
                }
                out[k] = (float) (acc / ch);
            }
            return true;
        }
        i += 8 + sz + (sz & 1);
    }
    return false;
}

static void writeWav (const std::string& path, const std::vector<float>& x, double fs)
{
    FILE* f = std::fopen (path.c_str(), "wb");
    if (! f) { std::fprintf (stderr, "cannot write %s\n", path.c_str()); return; }
    const uint32_t sr = (uint32_t) std::lround (fs);
    const uint32_t nb = (uint32_t) (x.size() * 2);
    const uint32_t rl = 36 + nb;
    const uint16_t one = 1, bps = 16, ba = 2;
    const uint32_t br = sr * 2, sz16 = 16;
    std::fwrite ("RIFF", 1, 4, f); std::fwrite (&rl, 4, 1, f);
    std::fwrite ("WAVEfmt ", 1, 8, f); std::fwrite (&sz16, 4, 1, f);
    std::fwrite (&one, 2, 1, f); std::fwrite (&one, 2, 1, f);
    std::fwrite (&sr, 4, 1, f);  std::fwrite (&br, 4, 1, f);
    std::fwrite (&ba, 2, 1, f);  std::fwrite (&bps, 2, 1, f);
    std::fwrite ("data", 1, 4, f); std::fwrite (&nb, 4, 1, f);
    for (float v : x)
    {
        const int16_t s = (int16_t) std::lround (std::clamp (v, -1.0f, 1.0f) * 32767.0f);
        std::fwrite (&s, 2, 1, f);
    }
    std::fclose (f);
}

int main (int argc, char** argv)
{
    if (argc < 6)
    {
        std::fprintf (stderr, "usage: %s <in.wav> <out.wav> <pitchSt> <formantSt> <disperse> [smooth] [body] [onsetHold] [holdTracksPeriod]\n", argv[0]);
        return 2;
    }
    std::vector<float> in; double fs = 0.0;
    if (! loadWav (argv[1], in, fs)) { std::fprintf (stderr, "cannot read %s\n", argv[1]); return 1; }

    PsolaEngine::Params p;
    p.pitchSemi     = (float) std::atof (argv[3]);
    p.formantSemi   = (float) std::atof (argv[4]);
    p.pulseDisperse = (float) std::atof (argv[5]);
    p.grainAvg      = argc > 6 ? std::atoi (argv[6]) != 0 : true;   // plugin default
    p.pulseBody     = argc > 7 ? (float) std::atof (argv[7]) : 0.0f; // 0 = v0.51.0 width
    p.onsetHold     = argc > 8 ? std::atoi (argv[8]) : 0;            // 0 = off
    p.holdTracksPeriod = argc > 9 ? std::atoi (argv[9]) != 0 : false;

    PsolaEngine e;
    e.prepare (fs);
    e.setParams (p);

    std::vector<float> out (in.size(), 0.0f);
    const size_t blk = 256;                       // the recommended host buffer
    for (size_t i = 0; i < in.size(); i += blk)
    {
        const int c = (int) std::min (blk, in.size() - i);
        e.process (in.data() + i, out.data() + i, c);
    }
    writeWav (argv[2], out, fs);
#ifdef PSOLA_DETECT_LOG
    {
        FILE* lf = std::fopen ("/tmp/dlog.csv", "w");
        std::fprintf (lf, "t,energy,lastVoicedE,zcr,rho,bestVal,pick,lag,f0Before,f0After,confident,vBefore,vAfter,why\n");
        for (const auto& r : e.detectLog)
            std::fprintf (lf, "%.6f,%.9g,%.9g,%.4f,%.4f,%.5f,%d,%d,%.2f,%.2f,%d,%d,%d,%d\n",
                          r.t, r.energy, r.lastVoicedE, r.zcr, r.rho, r.bestVal, r.pick, r.lag,
                          r.f0Before, r.f0After, (int) r.confident,
                          (int) r.voicedBefore, (int) r.voicedAfter, r.why);
        std::fclose (lf);
    }
#endif
    std::printf ("%s  %.0f Hz  %zu samples  pitch %+.1f  formant %+.1f  disperse %.2f  smooth %d  body %.2f  onsetHold %d  track %d\n",
                 argv[2], fs, out.size(), p.pitchSemi, p.formantSemi, p.pulseDisperse, (int) p.grainAvg, p.pulseBody, p.onsetHold, (int) p.holdTracksPeriod);
    return 0;
}
