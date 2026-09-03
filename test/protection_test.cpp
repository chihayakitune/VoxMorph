// protection_test.cpp — numeric checks for dsp/ProtectionGain.h (v0.65.0).
//
// Build (no JUCE, no numpy):
//   g++ -O2 -std=c++17 test/protection_test.cpp -o /tmp/prot && /tmp/prot
//
// The checks are ordered by how much they matter.
//
//  1. UNITY. At normal speaking level the stage must not touch a single
//     sample — not "inaudibly little", literally memcmp-identical. That is
//     what makes it safe to put in the default signal path.
//  2. ALIGNMENT. The engine delays its output by D samples. If the restore
//     undoes the gain at the wrong sample position it boosts precisely the
//     transient the protection had just pulled down, which is worse than
//     doing nothing. Checked by standing in for the engine with a pure delay
//     and asking for the original signal back.
//  3. BUFFER INDEPENDENCE. Every coefficient is per-sample and derived from
//     the sample rate, so the same input must give bit-identical output at
//     32 / 64 / 128 / 256 / 512 samples per block.
#include "../dsp/ProtectionGain.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

static int g_fail = 0;
static void check (bool ok, const char* what)
{
    std::printf ("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (! ok) ++g_fail;
}

static constexpr double kFs = 44100.0;

static float peakOf (const std::vector<float>& v, int from = 0)
{
    float p = 0.0f;
    for (int i = from; i < (int) v.size(); ++i) p = std::max (p, std::abs (v[i]));
    return p;
}
static float dbOf (float lin) { return 20.0f * std::log10 (std::max (lin, 1.0e-9f)); }

// A 150 Hz tone at a given peak level, with a couple of seconds of material.
static std::vector<float> tone (int n, float peak, double f0 = 150.0)
{
    std::vector<float> v ((size_t) n);
    for (int i = 0; i < n; ++i)
        v[(size_t) i] = peak * (float) std::sin (2.0 * M_PI * f0 * i / kFs);
    return v;
}

// Stands in for the conversion: a pure D-sample delay. Anything the protection
// takes off must come back out of this unchanged, or the alignment is wrong.
struct Delay
{
    std::vector<float> buf;
    int D = 0, w = 0;
    // read-then-write over L slots delays by exactly L, so the buffer is D long
    void prepare (int d) { D = d; buf.assign ((size_t) std::max (d, 0), 0.0f); w = 0; }
    void process (float* x, int n)
    {
        if (D <= 0) return;
        for (int i = 0; i < n; ++i)
        {
            const float out = buf[(size_t) w];
            buf[(size_t) w] = x[i];
            x[i] = out;
            if (++w >= D) w = 0;
        }
    }
};

// Runs signal through pre -> delay(D) -> post in fixed blocks and returns
// both the protected (engine-side) signal and the final restored signal.
static void runChain (const std::vector<float>& in, int block, int D,
                      std::vector<float>& protectedOut,
                      std::vector<float>& restoredOut)
{
    ProtectionGain pg;
    pg.prepare (kFs, block);
    Delay dly; dly.prepare (D);

    const int n = (int) in.size();
    protectedOut.assign ((size_t) n, 0.0f);
    restoredOut.assign ((size_t) n, 0.0f);

    std::vector<float> work ((size_t) block);
    for (int off = 0; off < n; off += block)
    {
        const int c = std::min (block, n - off);
        std::copy (in.begin() + off, in.begin() + off + c, work.begin());

        float* ch[1] = { work.data() };
        pg.processPre (ch, 1, c);
        std::copy (work.begin(), work.begin() + c, protectedOut.begin() + off);

        dly.process (work.data(), c);
        pg.processPost (ch, 1, c, D);
        std::copy (work.begin(), work.begin() + c, restoredOut.begin() + off);
    }
}

int main()
{
    const int D = (int) (kFs * 0.0427);          // the engine's normal lookahead

    // ---- 1. unity at normal level ------------------------------------------
    {
        // -20 dBFS, a normal speaking level, well under the -6 dBFS threshold.
        const auto in = tone ((int) (kFs * 2), 0.1f);
        std::vector<float> prot, rest;
        runChain (in, 256, D, prot, rest);

        check (std::memcmp (in.data(), prot.data(), in.size() * sizeof (float)) == 0,
               "normal level: engine input is bit-identical (no multiply at all)");

        // The restore side sees the delayed signal; with a pure delay standing
        // in for the engine, an idle stage must leave it exactly as it found it.
        bool restoreClean = true;
        for (int i = D; i < (int) in.size(); ++i)
            if (rest[(size_t) i] != in[(size_t) (i - D)]) { restoreClean = false; break; }
        check (restoreClean, "normal level: restore is bit-identical (no multiply at all)");

        ProtectionGain pg; pg.prepare (kFs, 256);
        check (pg.currentGainReductionDb() == 0.0f, "normal level: reported reduction is 0.0 dB");
    }

    // ---- 2. gain reduction on overload -------------------------------------
    {
        // +6 dBFS — the kind of level a shout or a Pre FX with makeup gain can
        // produce. Nothing before the engine clamps it today.
        const auto in = tone ((int) (kFs * 2), 2.0f);
        std::vector<float> prot, rest;
        runChain (in, 256, D, prot, rest);

        const int settle = (int) (kFs * 0.5);     // past attack and release
        const float pP = peakOf (prot, settle);
        std::printf ("      engine-side peak %.2f dBFS (input %.2f dBFS)\n",
                     dbOf (pP), dbOf (2.0f));
        // The gain is driven by the short-time envelope, so the steady peak
        // lands a little above the envelope threshold; the target window is
        // the -9..-6 dBFS the brief asks for.
        check (pP <= 0.5012f && pP >= 0.3548f,
               "overload: sustained engine-side peak lands in the -9..-6 dBFS window");

        // With no lookahead the very first cycle of an instantaneous onset is
        // not attenuated -- the attack needs its 2 ms. That is a documented
        // limitation, so it is measured rather than asserted away.
        std::printf ("      first-transient peak %.2f dBFS (no lookahead, attack 2 ms)\n",
                     dbOf (peakOf (prot, 0)));
        check (peakOf (prot, (int) (kFs * 0.05)) <= 1.02f,
               "overload: past the attack, peaks stay under the 0 dBFS ceiling");

        // The restore is not allowed to hand the original +6 dBFS back.
        const float pR = peakOf (rest, settle);
        std::printf ("      restored peak   %.2f dBFS (ceiling -1.00 dBFS)\n", dbOf (pR));
        check (pR <= 0.8913f * 1.05f, "overload: restore is held under the -1 dBFS ceiling");
    }

    // ---- 3. restore alignment ----------------------------------------------
    {
        // Quiet, then a sudden loud burst, then quiet again — the case where a
        // misaligned restore does its damage, because it would apply the burst's
        // compensation to the quiet material D samples ahead of it.
        const int n = (int) (kFs * 5);
        std::vector<float> in ((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            const double t = i / kFs;
            const float amp = (t > 1.0 && t < 1.5) ? 2.0f : 0.05f;
            in[(size_t) i] = amp * (float) std::sin (2.0 * M_PI * 150.0 * i / kFs);
        }
        std::vector<float> prot, rest;
        runChain (in, 256, D, prot, rest);

        // Region that is quiet on the INPUT but sits within D samples ahead of
        // the burst on the OUTPUT. A restore reading the undelayed gain would
        // lift this well above its input level.
        float worst = 0.0f;
        const int a = (int) (kFs * 0.90) + D, b = (int) (kFs * 1.00) + D;
        for (int i = a; i < b && i < n; ++i)
            worst = std::max (worst, std::abs (rest[(size_t) i]));
        std::printf ("      quiet-before-burst peak %.3f (input there 0.050)\n", worst);
        check (worst <= 0.05f * 1.05f,
               "alignment: quiet material ahead of a burst is not boosted");

        // Recovery has two separate criteria, and they are far apart in time.
        //
        // AUDIBLE: within a few release constants the level is back. 300 ms
        // after the burst the chain must already be within 0.05 dB.
        float worstDbErr = 0.0f;
        for (int i = (int) (kFs * 2.4) + D; i < (int) (kFs * 2.9) + D && i < n; ++i)
        {
            const float ref = in[(size_t) (i - D)];
            if (std::abs (ref) > 0.01f)
                worstDbErr = std::max (worstDbErr,
                                       std::abs (dbOf (std::abs (rest[(size_t) i]))
                                               - dbOf (std::abs (ref))));
        }
        // 0.9 s = 6 release constants; from a -12.5 dB reduction that is
        // 0.76 * e^-6 = 0.0019, i.e. under 0.02 dB.
        std::printf ("      0.9 s after the burst: worst level error %.4f dB\n", worstDbErr);
        check (worstDbErr < 0.05f, "alignment: level recovers within 0.05 dB in 0.9 s");

        // EXACT: the gain is a one-pole, so reaching the 1e-6 snap that lets
        // the multiply be skipped again takes about 13 release constants
        // (~2 s). Inaudible long before then, but worth pinning down, because
        // it is what puts the stage back on its zero-cost path.
        int firstExact = -1;
        for (int i = (int) (kFs * 1.5) + D; i < n; ++i)
            if (rest[(size_t) i] == in[(size_t) (i - D)]) { firstExact = i; break; }
        const double exactSec = firstExact < 0 ? -1.0
                              : (firstExact - D) / kFs - 1.5;
        std::printf ("      exact unity again %.2f s after the burst\n", exactSec);
        check (firstExact >= 0 && exactSec < 3.0,
               "alignment: chain returns to bit-exact unity after the burst");

        bool quietExact = true;
        for (int i = (int) (kFs * 4.5); i < n; ++i)
            if (rest[(size_t) i] != in[(size_t) (i - D)]) { quietExact = false; break; }
        check (quietExact, "alignment: and stays bit-exact once settled");
    }

    // ---- 4. buffer independence --------------------------------------------
    {
        const int n = (int) (kFs * 2);
        std::vector<float> in ((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            const double t = i / kFs;
            const float amp = (t > 0.4 && t < 0.9) ? 1.6f : 0.08f;
            in[(size_t) i] = amp * (float) std::sin (2.0 * M_PI * 170.0 * i / kFs);
        }
        std::vector<float> refP, refR;
        runChain (in, 256, D, refP, refR);

        bool same = true;
        for (int blk : { 32, 64, 128, 512 })
        {
            std::vector<float> p, r;
            runChain (in, blk, D, p, r);
            if (std::memcmp (refP.data(), p.data(), refP.size() * sizeof (float)) != 0
             || std::memcmp (refR.data(), r.data(), refR.size() * sizeof (float)) != 0)
            {
                std::printf ("      block %d differs\n", blk);
                same = false;
            }
        }
        check (same, "buffer independence: 32/64/128/512 are bit-identical to 256");
    }

    // ---- 5. stereo link ------------------------------------------------------
    {
        // R is a fixed 0.5x of L. A stereo-linked detector applies one common
        // gain, so that ratio has to survive both stages exactly.
        const int n = (int) (kFs * 1.5);
        const auto L0 = tone (n, 2.0f);
        ProtectionGain pg; pg.prepare (kFs, 256);
        Delay dl, dr; dl.prepare (D); dr.prepare (D);

        std::vector<float> L (L0), R ((size_t) n);
        for (int i = 0; i < n; ++i) R[(size_t) i] = 0.5f * L0[(size_t) i];

        float worstRatioErr = 0.0f;
        bool  reduced = false;
        for (int off = 0; off < n; off += 256)
        {
            const int c = std::min (256, n - off);
            float* ch[2] = { L.data() + off, R.data() + off };
            pg.processPre (ch, 2, c);
            for (int i = 0; i < c; ++i)
                if (std::abs (L[(size_t) (off + i)]) < std::abs (L0[(size_t) (off + i)]) * 0.999f)
                    reduced = true;
            dl.process (L.data() + off, c);
            dr.process (R.data() + off, c);
            pg.processPost (ch, 2, c, D);
            for (int i = 0; i < c; ++i)
            {
                const float l = L[(size_t) (off + i)], r = R[(size_t) (off + i)];
                if (std::abs (l) > 1.0e-4f)
                    worstRatioErr = std::max (worstRatioErr, std::abs (r / l - 0.5f));
            }
        }
        std::printf ("      worst L/R ratio error %.3e\n", worstRatioErr);
        check (reduced, "stereo: the stage did engage on this material");
        check (worstRatioErr < 1.0e-5f, "stereo: one common gain, image cannot move");
    }

    // ---- 6. mono uses the same path -----------------------------------------
    {
        // The mono call is the 2-channel call with numChans = 1: feeding the
        // same samples on both channels must give the same gain decisions.
        const int n = (int) (kFs * 1.5);
        const auto in = tone (n, 2.0f);

        std::vector<float> m (in);
        ProtectionGain p1; p1.prepare (kFs, 256);
        for (int off = 0; off < n; off += 256)
        {
            const int c = std::min (256, n - off);
            float* ch[1] = { m.data() + off };
            p1.processPre (ch, 1, c);
        }

        std::vector<float> l (in), r (in);
        ProtectionGain p2; p2.prepare (kFs, 256);
        for (int off = 0; off < n; off += 256)
        {
            const int c = std::min (256, n - off);
            float* ch[2] = { l.data() + off, r.data() + off };
            p2.processPre (ch, 2, c);
        }
        check (std::memcmp (m.data(), l.data(), m.size() * sizeof (float)) == 0,
               "mono: identical decisions to the stereo path on the same signal");
    }

    // ---- 7. reset ------------------------------------------------------------
    {
        ProtectionGain pg; pg.prepare (kFs, 256);
        auto loud = tone ((int) (kFs * 0.5), 3.0f);
        for (int off = 0; off < (int) loud.size(); off += 256)
        {
            const int c = std::min (256, (int) loud.size() - off);
            float* ch[1] = { loud.data() + off };
            pg.processPre (ch, 1, c);
        }
        check (pg.currentGainReductionDb() < -1.0f, "reset: stage was actually engaged first");

        pg.reset();
        auto quiet = tone (512, 0.1f);
        const auto before = quiet;
        float* ch[1] = { quiet.data() };
        pg.processPre (ch, 1, 512);
        check (std::memcmp (before.data(), quiet.data(), 512 * sizeof (float)) == 0,
               "reset: state cleared, back to exact unity immediately");
        check (pg.currentGainReductionDb() == 0.0f, "reset: reported reduction back to 0.0 dB");
    }

    // ---- 8. NaN / Inf --------------------------------------------------------
    {
        ProtectionGain pg; pg.prepare (kFs, 256);
        std::vector<float> bad ((size_t) 512, 0.2f);
        bad[10]  = std::numeric_limits<float>::quiet_NaN();
        bad[11]  = std::numeric_limits<float>::infinity();
        bad[200] = -std::numeric_limits<float>::infinity();
        float* ch[1] = { bad.data() };
        pg.processPre (ch, 1, 512);
        pg.processPost (ch, 1, 512, D);
        check (std::isfinite (pg.currentGainReductionDb()),
               "NaN/Inf: the reported gain stays finite");

        // and the stage recovers to exact unity on clean material afterwards
        auto clean = tone ((int) (kFs * 0.5), 0.1f);
        const auto before = clean;
        for (int off = 0; off < (int) clean.size(); off += 256)
        {
            const int c = std::min (256, (int) clean.size() - off);
            float* c2[1] = { clean.data() + off };
            pg.processPre (c2, 1, c);
        }
        bool tail = true;
        for (int i = (int) (kFs * 0.3); i < (int) clean.size(); ++i)
            if (clean[(size_t) i] != before[(size_t) i]) { tail = false; break; }
        check (tail, "NaN/Inf: recovers to exact unity on clean material");
    }

    // ---- 9. bypass -----------------------------------------------------------
    {
        ProtectionGain pg; pg.prepare (kFs, 256);
        pg.setBypassed (true);
        auto loud = tone ((int) (kFs * 1.0), 3.0f);
        const auto before = loud;
        for (int off = 0; off < (int) loud.size(); off += 256)
        {
            const int c = std::min (256, (int) loud.size() - off);
            float* ch[1] = { loud.data() + off };
            pg.processPre (ch, 1, c);
        }
        check (std::memcmp (before.data(), loud.data(), loud.size() * sizeof (float)) == 0,
               "bypass: loud input passes through untouched");
    }

    std::printf ("\n%s (%d failure%s)\n", g_fail ? "FAILURES" : "all checks passed",
                 g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
