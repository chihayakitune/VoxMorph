// Adaptive Voice Dynamics -- DSP-only smoke test (no JUCE).
//
//   g++ -O2 -std=c++17 -I dsp test/vec_smoke.cpp -o vec_smoke && ./vec_smoke
//
// The comparison against the base commit's engine is done separately with
// test/bitexact.cpp (see its header), built once per tree and cmp'd.
//
// Checks
//   1. engine with no view == engine with an all-zero view
//   2. a single non-zero control sample at input time T0 first changes the
//      output at sample T0 + D -- normal and Low Latency D -- identically on
//      two engines (L/R)
//   3. estimator + control + engine: silence -> normal -> loud -> OFF stays
//      finite, effort rises on the loud part, and ramps back to exact neutral

#include "PsolaEngine.h"
#include "VocalEffortEstimator.h"
#include "AdaptiveVoiceDynamics.h"

#include <cstdio>
#include <cstring>
#include <vector>

static int fails = 0;
static void check (bool ok, const char* what)
{
    std::printf ("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (! ok) ++fails;
}

// crude voiced signal: glottal-ish pulse train through two resonances + a bit of noise
static std::vector<float> voice (double fs, int n, float amp, float f0, unsigned seed = 1)
{
    std::vector<float> x ((size_t) n);
    double ph = 0; float r1a = 0, r1b = 0, r2a = 0, r2b = 0;
    auto res = [fs] (float in, float& a, float& b, double f, double bw)
    {
        const double r = std::exp (-M_PI * bw / fs), c = 2 * r * std::cos (2 * M_PI * f / fs);
        const float y = (float) (in + c * a - r * r * b);  b = a;  a = y;  return y;
    };
    for (int i = 0; i < n; ++i)
    {
        ph += f0 / fs;
        float e = 0.0f;
        if (ph >= 1.0) { ph -= 1.0; e = 1.0f; }
        seed = seed * 1664525u + 1013904223u;
        e += 0.01f * ((float) (seed >> 8) / 16777216.0f - 0.5f);
        const float y = res (e, r1a, r1b, 700, 90) * 0.02f + res (e, r2a, r2b, 1200, 120) * 0.01f;
        x[(size_t) i] = amp * y;
    }
    return x;
}

int main()
{
    const double fs = 48000.0;
    const int    N  = (int) (fs * 3.0);
    const int    blk = 256;
    PsolaEngine::Params p;
    p.pitchSemi = 5.0f;  p.formantSemi = 2.0f;  p.airPreserve = 0.3f;

    auto x = voice (fs, N, 1.0f, 140.0f);

    auto run = [&] (PsolaEngine& e, const AvdView* v, bool lowLatAt1s = false)
    {
        std::vector<float> y ((size_t) N);
        e.prepare (fs);  e.setParams (p);
        for (int off = 0; off < N; off += blk)
        {
            if (lowLatAt1s && off >= (int) fs) { auto q = p; q.lowLatency = true; e.setParams (q); }
            const int c = std::min (blk, N - off);
            e.process (x.data() + off, y.data() + off, c, v, off);
        }
        return y;
    };

    // ---- 1. identity -------------------------------------------------------
    {
        PsolaEngine a, b;
        auto ya = run (a, nullptr);
        AvdControl ctl;  ctl.prepare (fs, 2050, 512);
        for (int t = 0; t < 4096; ++t) ctl.push (t, false, 0.5f, 0.0f, 0.0f, 0.0f);   // zeros only
        const AvdView zv = ctl.view();
        auto yb = run (b, &zv);
        check (std::memcmp (ya.data(), yb.data(), ya.size() * sizeof (float)) == 0,
               "no view == all-zero view (bit-exact)");
    }

    // ---- 2. n - D synchronisation, L/R -----------------------------------
    for (int mode = 0; mode < 2; ++mode)
    {
        const bool ll = mode == 1;
        // impulse well after the Low Latency switch (1 s) has been taken
        const int64_t T0 = (int64_t) (fs * 1.6) + 17;
        std::vector<float> tb (262144, 0.0f), bb (262144, 0.0f), pb (262144, 0.0f);   // > N: no aliasing
        bb[(size_t) T0] = -2.5f;  tb[(size_t) T0] = 2.0f;
        AvdView v;  v.tilt = tb.data();  v.body = bb.data();  v.pres = pb.data();
        v.mask = 262143;  v.validFrom = 0;  v.validTo = N;

        PsolaEngine ref, L, R;
        auto y0 = run (ref, nullptr, ll);
        auto yl = run (L, &v, ll);
        auto yr = run (R, &v, ll);
        int firstL = -1, firstR = -1;
        for (int i = 0; i < N; ++i) { if (firstL < 0 && yl[(size_t) i] != y0[(size_t) i]) firstL = i;
                                      if (firstR < 0 && yr[(size_t) i] != y0[(size_t) i]) firstR = i; }
        const int D = ref.latencySamples();
        std::printf ("   %s: D=%d  first change at %d, expected %lld\n", ll ? "low latency" : "normal",
                     D, firstL, (long long) (T0 + D));
        check (firstL == (int) (T0 + D), ll ? "impulse lands at T0 + D (after Low Latency switch)"
                                            : "impulse lands at T0 + D (normal)");
        check (firstL == firstR && std::memcmp (yl.data(), yr.data(), yl.size() * sizeof (float)) == 0,
               "L and R read the same control at the same instant");
    }

    // ---- 3. estimator chain, finite, ramp to neutral -------------------
    {
        // 0.5 s silence, 2.5 s normal, 1.5 s loud (+14 dB), 1 s normal with OFF
        const int n0 = (int) (fs * 0.5), n1 = (int) (fs * 2.5), n2 = (int) (fs * 1.5), n3 = (int) (fs * 1.0);
        const int NT = n0 + n1 + n2 + n3;
        auto a = voice (fs, NT, 1.0f, 140.0f, 7);
        std::vector<float> in ((size_t) NT);
        for (int i = 0; i < NT; ++i)
        {
            float g = i < n0 ? 0.0f : (i < n0 + n1 ? 1.0f : (i < n0 + n1 + n2 ? 5.0f : 1.0f));
            in[(size_t) i] = a[(size_t) i] * g;
        }
        VocalEffortEstimator est;  est.prepare (fs);
        AvdControl ctl;  ctl.prepare (fs, 2050, 512);
        PsolaEngine e;  e.prepare (fs);  e.setParams (p);
        std::vector<float> y ((size_t) NT);
        float maxEff = 0, effNormal = 0, warmEnd = 0;
        bool finite = true;
        int64_t lastNZ = -1;
        for (int off = 0; off < NT; off += blk)
        {
            const int c = std::min (blk, NT - off);
            const bool on = off < n0 + n1 + n2;
            const int64_t base = ctl.now();
            const float* ch[1] = { in.data() + off };
            if (ctl.wantsRun (on))
            {
                int64_t t = base;
                est.process (ch, 1, c, [&] (const VocalEffortEstimator::Output& o)
                             { ctl.push (t++, on, 1.0f, o.effort, o.bodyEx, o.presEx); });
            }
            const AvdView v = ctl.view();
            const bool need = ctl.engineNeedsView (base);
            if (need) lastNZ = base;
            e.process (in.data() + off, y.data() + off, c, need ? &v : nullptr, base);
            ctl.advance (c);
            const auto& o = est.current();
            if (off > n0 + n1 - (int) fs / 2 && off < n0 + n1) effNormal = std::max (effNormal, o.effort);
            if (off >= n0 + n1 && off < n0 + n1 + n2) maxEff = std::max (maxEff, o.effort);
            if (off < n0 + n1) warmEnd = o.warm;
            for (int i = 0; i < c; ++i) finite = finite && std::isfinite (y[(size_t) (off + i)]);
        }
        std::printf ("   warm at end of normal = %.2f, effort normal max = %.2f, loud max = %.2f\n",
                     warmEnd, effNormal, maxEff);
        check (finite, "silence -> normal -> loud -> OFF: output finite");
        check (warmEnd >= 1.0f, "baseline collected within the 2.5 s of normal speech");
        check (maxEff > 0.3f && effNormal < 0.15f, "effort low on normal, raised on loud");
        check (lastNZ >= 0 && lastNZ < n0 + n1 + n2 + (int) (0.2 * fs),
               "after OFF the engine stops receiving a view within ramp + D");
    }

    std::printf (fails == 0 ? "ALL PASS\n" : "%d FAIL\n", fails);
    return fails == 0 ? 0 : 1;
}
