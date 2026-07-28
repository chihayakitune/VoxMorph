// bitexact.cpp - before/after reference dump for the PSOLA engine.
//
// WHEN TO USE: any change to dsp/ that is supposed to leave the audio
// untouched (an optimisation, a bypass, a refactor). offline_test.cpp
// checks behaviour and quality; this checks that the SAMPLES did not move,
// which is the thing an optimisation actually has to promise.
//
// HOW (from the repo root):
//     g++ -O2 -std=c++17 -I dsp -o /tmp/be test/bitexact.cpp
//     /tmp/be /tmp/before.f32          # BEFORE your change
//     ...edit dsp/...
//     g++ -O2 -std=c++17 -I dsp -o /tmp/be test/bitexact.cpp
//     /tmp/be /tmp/after.f32           # AFTER
//     cmp /tmp/before.f32 /tmp/after.f32     # must be silent
//
// The reference file is deliberately NOT committed: it is compiler- and
// platform-specific, and the only comparison that means anything is one
// you generate on the same machine either side of your own edit.
//
// The signal deliberately includes a glide, vibrato, a silent gap and an
// unvoiced burst, and every run uses a different host block size, so the
// per-chunk state machines (pitch detection cadence, air path activation
// and drain, grain scheduling) are all exercised.
#include "PsolaEngine.h"
#include <cstdio>
#include <cstdint>
#include <random>

static std::vector<float> makeVoice (double fs, double sec)
{
    const int n = (int) (fs * sec);
    std::vector<float> x ((size_t) n, 0.0f);
    std::mt19937 rng (12345);
    std::normal_distribution<float> g (0.0f, 1.0f);
    double ph = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double t  = i / fs;
        // glide + vibrato + a silent gap + an unvoiced burst
        double f0 = 110.0 * std::pow (2.0, 0.6 * std::sin (2.0 * M_PI * 0.35 * t))
                          * (1.0 + 0.02 * std::sin (2.0 * M_PI * 5.5 * t));
        ph += f0 / fs;
        double s = 0.0;
        for (int k = 1; k <= 24; ++k)
            s += std::exp (-0.18 * (k - 1)) * std::sin (2.0 * M_PI * k * ph);
        float env = 1.0f;
        if (t > 1.2 && t < 1.5) env = 0.0f;                       // silence
        if (t > 2.0 && t < 2.2) { s = 0.0; env = 0.35f; }         // unvoiced
        x[(size_t) i] = env * (0.22f * (float) s + 0.02f * g (rng));
    }
    return x;
}

int main (int argc, char** argv)
{
    const double fs = 48000.0;
    auto in = makeVoice (fs, 3.0);

    FILE* f = std::fopen (argc > 1 ? argv[1] : "out.f32", "wb");

    auto run = [&] (const PsolaEngine::Params& p, int blk)
    {
        PsolaEngine e;
        e.prepare (fs);
        e.setParams (p);
        std::vector<float> out (in.size(), 0.0f);
        for (size_t i = 0; i < in.size(); i += (size_t) blk)
        {
            const int c = (int) std::min ((size_t) blk, in.size() - i);
            e.process (in.data() + i, out.data() + i, c);
        }
        std::fwrite (out.data(), sizeof (float), out.size(), f);
    };

    using P = PsolaEngine::Params;

    P a;  a.pitchSemi = 7.0f;  a.formantSemi = 3.0f;                 run (a, 64);
    P b;  b.pitchSemi = 12.0f; b.airPreserve = 1.0f;                 run (b, 128);
    P c;  c.pitchSemi = -5.0f; c.airPreserve = 0.6f; c.airShineDb = 4.0f;  run (c, 256);
    P d;  d.pitchSemi = 4.0f;  d.mix = 0.35f;                        run (d, 512);
    P e2; e2.pitchSemi = 6.0f; e2.f1Shift = 1.5f; e2.f2Gain = 3.0f;  run (e2, 200);
    P g2; g2.pitchSemi = 5.0f; g2.vowelAdapt = true; g2.vowelAdaptAmt = 1.0f; run (g2, 64);
    P h;  h.pitchSemi = 8.0f;  h.lowLatency = true;                  run (h, 64);
    P i2; i2.pitchSemi = 3.0f; i2.lowVoice = true; i2.airPreserve = 1.2f;  run (i2, 480);
    P j;  j.robotize = true;   j.robotHz = 150.0f;                   run (j, 128);
    P k;  k.pitchSemi = 9.0f;  k.tiltDb = 3.0f; k.jitter = 0.2f;     run (k, 96);
    P l;  l.pitchSemi = 2.0f;  l.breath = 0.3f; l.gciSync = true;    run (l, 128);
    P m;  m.pitchSemi = 7.0f;  m.mix = 1.0f; m.airPreserve = 0.0f;   run (m, 32);

    std::fclose (f);
    std::printf ("done\n");
    return 0;
}
