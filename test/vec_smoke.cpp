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
#include "ProtectionGain.h"

#include <cstdio>
#include <limits>
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

    // ---- 4. baseline warmup completes near Protection's threshold ----------
    // The old gate was a flat peak >= -8 dBFS, so ordinary loud-ish speech
    // never finished warming up and the feature silently did nothing. -7.5
    // dBFS peak is inside normal speech and must learn.
    {
        VocalEffortEstimator est;  est.prepare (fs);
        auto v = voice (fs, (int) (fs * 4.0), 1.0f, 120.0f);
        float pk = 0.0f;
        for (float x : v) pk = std::max (pk, std::abs (x));
        const float want = std::pow (10.0f, -7.5f / 20.0f);     // -7.5 dBFS
        for (auto& x : v) x *= want / std::max (pk, 1.0e-9f);
        float got = 0.0f;
        for (float x : v) got = std::max (got, std::abs (x));

        const float* ch[1] = { v.data() };
        est.process (ch, 1, (int) v.size(), [] (const VocalEffortEstimator::Output&) {});
        std::printf ("   peak %.2f dBFS -> warm %.2f\n",
                     20.0f * std::log10 (got), est.current().warm);
        check (est.current().warm >= 1.0f,
               "baseline warms up on speech peaking at -7.5 dBFS (Protection's doorstep)");
    }

    // ---- 4b. Protection actually reducing -> the baseline must NOT learn ---
    // The other half of the gate, and the half the peak approximations kept
    // getting wrong. Driven loud enough that ProtectionGain -- the real one,
    // run here on the same samples -- reports a reduction, the estimator has
    // to refuse to warm up.
    {
        VocalEffortEstimator est;  est.prepare (fs);
        ProtectionGain pg;         pg.prepare (fs, 512);

        auto v = voice (fs, (int) (fs * 6.0), 1.0f, 120.0f);
        float pk = 0.0f;
        for (float x : v) pk = std::max (pk, std::abs (x));
        const float want = std::pow (10.0f, -1.0f / 20.0f);      // -1 dBFS: well inside
        for (auto& x : v) x *= want / std::max (pk, 1.0e-9f);

        // what ProtectionGain itself does with this material
        auto copy = v;
        float worstGr = 0.0f;
        for (int off = 0; off < (int) copy.size(); off += 512)
        {
            const int c = std::min (512, (int) copy.size() - off);
            float* ch[1] = { copy.data() + off };
            pg.processPre (ch, 1, c);
            worstGr = std::min (worstGr, pg.currentGainReductionDb());
        }

        const float* ch[1] = { v.data() };
        bool sawActive = false;
        est.process (ch, 1, (int) v.size(), [&] (const VocalEffortEstimator::Output&)
        {
            sawActive = sawActive || est.protectionActive();
        });
        std::printf ("   loud input: ProtectionGain reduction %.2f dB, estimator warm %.2f\n",
                     worstGr, est.current().warm);
        check (worstGr < -0.5f, "the reference ProtectionGain really is reducing on this input");
        check (sawActive, "the estimator's mirrored detector agrees Protection is active");
        check (est.current().warm < 1.0f,
               "baseline does NOT warm up while Protection is actually reducing");

        // ...and with Protection bypassed the same input is allowed to learn
        VocalEffortEstimator est2;  est2.prepare (fs);
        est2.setProtectionBypassed (true);
        est2.process (ch, 1, (int) v.size(), [] (const VocalEffortEstimator::Output&) {});
        std::printf ("   same input, Protection bypassed: warm %.2f\n", est2.current().warm);
        check (est2.current().warm >= 1.0f,
               "Protection bypassed is not a reason to hold the baseline back");
    }

    // ---- 4c. the Protection hold must not outlive a reset or a bypass ------
    // Both are "the tail from a moment that no longer applies". A 50 ms hold
    // left over from before a full reset, or from before Protection was
    // switched off, would silently keep goodFrame shut.
    {
        auto loud = voice (fs, (int) (fs * 1.0), 1.0f, 120.0f);
        float pk = 0.0f;
        for (float x : loud) pk = std::max (pk, std::abs (x));
        const float want = std::pow (10.0f, -1.0f / 20.0f);
        for (auto& x : loud) x *= want / std::max (pk, 1.0e-9f);
        const float* lc[1] = { loud.data() };
        auto quiet = voice (fs, (int) (fs * 3.0), 0.25f, 120.0f);
        const float* qc[1] = { quiet.data() };

        // bypass while the hold is still running: quiet speech must learn
        VocalEffortEstimator est;  est.prepare (fs);
        est.process (lc, 1, (int) loud.size(), [] (const VocalEffortEstimator::Output&) {});
        check (est.protectionActive(), "loud input leaves Protection active (precondition)");
        est.setProtectionBypassed (true);
        est.process (qc, 1, (int) quiet.size(), [] (const VocalEffortEstimator::Output&) {});
        std::printf ("   bypass right after a loud passage: warm %.2f\n", est.current().warm);
        check (est.current().warm >= 1.0f,
               "bypass clears the Protection hold (no stale tail blocking goodFrame)");

        // full reset while the hold is running: same requirement
        VocalEffortEstimator est2;  est2.prepare (fs);
        est2.process (lc, 1, (int) loud.size(), [] (const VocalEffortEstimator::Output&) {});
        est2.resetAll();
        check (! est2.protectionActive(), "resetAll clears the Protection verdict");
        est2.setProtectionBypassed (true);
        est2.process (qc, 1, (int) quiet.size(), [] (const VocalEffortEstimator::Output&) {});
        check (est2.current().warm >= 1.0f,
               "resetAll clears the Protection hold as well");
    }

    // ---- 5. full reset sends it back to warming up -------------------------
    {
        VocalEffortEstimator est;  est.prepare (fs);
        auto v = voice (fs, (int) (fs * 4.0), 0.25f, 120.0f);
        const float* ch[1] = { v.data() };
        est.process (ch, 1, (int) v.size(), [] (const VocalEffortEstimator::Output&) {});
        const float warmBefore = est.current().warm;

        est.resetAll();
        check (warmBefore >= 1.0f && est.current().warm == 0.0f,
               "resetAll drops the baseline and returns to warmup");

        // resetSignalState must NOT: it is the Low Latency / time-line case
        VocalEffortEstimator est2;  est2.prepare (fs);
        est2.process (ch, 1, (int) v.size(), [] (const VocalEffortEstimator::Output&) {});
        est2.resetSignalState();
        check (est2.current().warm >= 1.0f,
               "resetSignalState keeps the baseline (time line only)");
    }

    // ---- 6. a huge FINITE input must not kill the estimator -----------------
    // 1e20 is finite, but squared it is +inf and would stick in the envelopes
    // for good. After the burst, normal speech has to read finite again.
    {
        VocalEffortEstimator est;  est.prepare (fs);
        auto warm = voice (fs, (int) (fs * 3.0), 0.25f, 120.0f);
        const float* wc[1] = { warm.data() };
        est.process (wc, 1, (int) warm.size(), [] (const VocalEffortEstimator::Output&) {});

        // Extreme finite, NaN and Inf together: all three have to leave the
        // estimator able to read normal speech again afterwards.
        std::vector<float> bad ((size_t) (int) (fs * 0.2), 1.0e20f);
        for (size_t i = 0; i < bad.size(); i += 7)
            bad[i] = std::numeric_limits<float>::quiet_NaN();
        for (size_t i = 3; i < bad.size(); i += 11)
            bad[i] = (i % 2) ? std::numeric_limits<float>::infinity()
                             : -std::numeric_limits<float>::infinity();
        const float* bc[1] = { bad.data() };
        est.process (bc, 1, (int) bad.size(), [] (const VocalEffortEstimator::Output&) {});

        auto back = voice (fs, (int) (fs * 2.0), 0.25f, 120.0f);
        const float* rc[1] = { back.data() };
        bool finite = true;
        est.process (rc, 1, (int) back.size(), [&] (const VocalEffortEstimator::Output& o)
        {
            finite = finite && std::isfinite (o.effort) && std::isfinite (o.warm)
                            && std::isfinite (o.bodyEx) && std::isfinite (o.presEx);
        });
        const auto& o = est.current();
        std::printf ("   after 1e20 burst: effort %.3f warm %.2f\n", o.effort, o.warm);
        check (finite, "estimator output stays finite through a 1e20 / NaN / Inf burst");
        check (std::isfinite (o.effort) && std::isfinite (o.warm),
               "and recovers to finite values on normal input afterwards");
    }

    // ---- 7. back to a normal voice -> the view is dropped ------------------
    // Item 4: without the exact-zero snap the gains decay towards 0 but never
    // reach it, so engineNeedsView() stays true for ever and both engines keep
    // filtering an inaudible correction indefinitely.
    {
        AvdControl ctl;  ctl.prepare (fs, 2048, 512);
        const int64_t d = 2048;
        // drive a correction, then ask for none while staying ENABLED
        for (int i = 0; i < (int) (fs * 1.0); ++i)
            ctl.push (ctl.now() + i, true, 1.0f, 1.0f, 1.0f, 1.0f);
        ctl.advance ((int) (fs * 1.0));
        check (ctl.engineNeedsView (ctl.now()), "correction active: the engine gets a view");

        int64_t droppedAt = -1;
        for (int i = 0; i < (int) (fs * 3.0); ++i)
        {
            const int64_t t = ctl.now();
            ctl.push (t, true, 1.0f, 0.0f, 0.0f, 0.0f);   // enabled, effort 0
            ctl.advance (1);
            if (droppedAt < 0 && ! ctl.engineNeedsView (ctl.now())) droppedAt = i;
        }
        std::printf ("   view dropped %.0f ms after the voice returned to normal\n",
                     droppedAt < 0 ? -1.0 : 1000.0 * (double) droppedAt / fs);
        check (droppedAt >= 0,
               "enabled but effort back to 0: the view is dropped (gains snap to true zero)");
        check (droppedAt < (int) (fs * 1.5),
               "and it happens promptly, not after minutes of decay");
        (void) d;
    }

    std::printf (fails == 0 ? "ALL PASS\n" : "%d FAIL\n", fails);
    return fails == 0 ? 0 : 1;
}
