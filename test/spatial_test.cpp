// spatial_test.cpp — numeric checks for dsp/SpatialEngine.h (v0.45.0).
//
// Build (no JUCE, no numpy):
//   g++ -O2 -std=c++17 test/spatial_test.cpp -o /tmp/spatial && /tmp/spatial
//
// The first two checks are the ones that matter most. The ASMR stage sits on
// the finished output of the conversion engine, so the promise made in
// SpatialEngine.h is that a session which never touches the new controls
// produces the SAME SAMPLES as v0.44 did. That cannot be heard — a one-sample
// shift or a 1e-7 gain change is inaudible and would still be a regression —
// so it is checked with memcmp against a copy of the old inline maths.
#include "../dsp/SpatialEngine.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

static int g_fail = 0;
static void check (bool ok, const char* what)
{
    std::printf ("%-62s %s\n", what, ok ? "ok" : "FAIL");
    if (! ok) ++g_fail;
}

// ---------------------------------------------------------------------------
// The v0.44 ASMR stage, lifted verbatim out of PluginProcessor::processBlock.
// Do not "clean this up": it is a reference, and every reordering of the
// arithmetic changes the last bit of the result.
struct LegacyPan
{
    float panL = 1.0f, panR = 1.0f;

    void process (const float* src, float* dL, float* dR, int n, float axIn, float ayIn)
    {
        const float ax = std::max (-1.0f, std::min (1.0f, axIn));
        const float ay = std::max (-1.0f, std::min (1.0f, ayIn));
        const float dist = std::min (1.0f, std::sqrt (ax * ax + ay * ay));
        const float dg = 1.0f - 0.6f * dist;
        const float panPhase = (ax + 1.0f) * 0.25f * 3.14159265358979323846f;
        const float tL = dg * std::cos (panPhase) * 1.41421356237309504880f;
        const float tR = dg * std::sin (panPhase) * 1.41421356237309504880f;
        const bool panNeutral = (ax == 0.0f && ay == 0.0f)
                             && std::abs (panL - 1.0f) < 1.0e-6f
                             && std::abs (panR - 1.0f) < 1.0e-6f;
        if (panNeutral) { panL = 1.0f; panR = 1.0f; }
        for (int c = 0; c < 2; ++c)
        {
            float* d = c == 0 ? dL : dR;
            const float t = c == 0 ? tL : tR;
            float& sm = c == 0 ? panL : panR;
            if (panNeutral)
                std::copy (src, src + n, d);
            else
                for (int i = 0; i < n; ++i)
                {
                    sm += 0.002f * (t - sm);
                    d[i] = sm * src[i];
                }
        }
    }
};

// ---------------------------------------------------------------------------
static std::vector<float> voiceLike (int n, unsigned seed = 7)
{
    // a rough voice: three harmonics plus breath noise, so the HF checks
    // below have something above 3 kHz to lose
    std::mt19937 rng (seed);
    std::uniform_real_distribution<float> u (-1.0f, 1.0f);
    std::vector<float> v ((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        const float t = (float) i / 48000.0f;
        v[(size_t) i] = 0.35f * std::sin (2.0f * 3.14159265f * 180.0f * t)
                      + 0.20f * std::sin (2.0f * 3.14159265f * 540.0f * t)
                      + 0.12f * std::sin (2.0f * 3.14159265f * 2400.0f * t)
                      + 0.06f * u (rng);
    }
    return v;
}

// energy above ~3 kHz, via a crude one-pole high pass
static double hfEnergy (const std::vector<float>& v, float sr = 48000.0f)
{
    const float a = std::exp (-2.0f * 3.14159265f * 3000.0f / sr);
    float z = 0.0f;
    double e = 0.0;
    for (float s : v)
    {
        z = a * z + (1.0f - a) * s;
        const double hp = (double) s - z;
        e += hp * hp;
    }
    return e;
}

static double energy (const std::vector<float>& v)
{
    double e = 0.0;
    for (float s : v) e += (double) s * s;
    return e;
}

// Steady-state gain the stage applies to a pure tone, in dB, measured on the
// left channel. This is what actually resolves a filter cue: an energy ratio
// over a broadband signal is dominated by the low end, where these filters do
// nothing, so a clearly audible 4 dB dip at 6 kHz shows up there as a few
// percent and cannot be told from noise.
static double toneGainDb (float hz, const SpatialEngine::Params& p, int seconds = 2)
{
    constexpr int kSr = 48000, kBlk = 512;
    SpatialEngine sp;  sp.prepare (kSr, kBlk);
    const int n = kSr * seconds;
    std::vector<float> in ((size_t) kBlk), oL ((size_t) kBlk), oR ((size_t) kBlk);
    double num = 0.0, den = 0.0;
    for (int blk = 0; blk * kBlk < n; ++blk)
    {
        for (int i = 0; i < kBlk; ++i)
            in[(size_t) i] = std::sin (2.0f * 3.14159265f * hz
                                         * (float) (blk * kBlk + i) / (float) kSr);
        sp.process (in.data(), in.data(), oL.data(), oR.data(), kBlk, p);
        // second half only: the smoothers need ~10 ms and the room ~1 s
        if (blk * kBlk < n / 2) continue;
        for (int i = 0; i < kBlk; ++i)
        {
            num += (double) oL[(size_t) i] * oL[(size_t) i];
            den += (double) in[(size_t) i] * in[(size_t) i];
        }
    }
    return 10.0 * std::log10 (num / den);
}

// How much later `b` is than `a`, in samples, by peak cross-correlation.
// b[i] = a[i - D] maximises sum a[i]*b[i + D], so the winning offset IS the
// delay -- positive means b lags a.
static int lagOf (const std::vector<float>& a, const std::vector<float>& b, int maxLag)
{
    int best = 0;
    double bestC = -1.0e300;
    for (int L = -maxLag; L <= maxLag; ++L)
    {
        double c = 0.0;
        for (int i = maxLag; i < (int) a.size() - maxLag; ++i)
            c += (double) a[(size_t) i] * b[(size_t) (i + L)];
        if (c > bestC) { bestC = c; best = L; }
    }
    return best;
}

int main()
{
    constexpr int kSr = 48000, kBlk = 512, kBlocks = 40;
    const int kN = kBlk * kBlocks;
    const auto src = voiceLike (kN);

    // ---- 1. neutral settings are bit-identical to v0.44 ------------------
    // Swept over a path through the pad, not just the centre: the smoothers
    // have to converge the same way as the old ones as the dot moves.
    {
        SpatialEngine sp;  sp.prepare (kSr, kBlk);
        LegacyPan legacy;
        std::vector<float> aL ((size_t) kN), aR ((size_t) kN);
        std::vector<float> bL ((size_t) kN), bR ((size_t) kN);

        for (int blk = 0; blk < kBlocks; ++blk)
        {
            const float ph = (float) blk / (float) kBlocks;
            const float x = (blk < 4) ? 0.0f : 0.9f * std::sin (6.28318f * ph);
            const float y = (blk < 4) ? 0.0f : 0.7f * std::cos (6.28318f * ph);

            const int off = blk * kBlk;
            SpatialEngine::Params p;          // every field at its default
            p.x = x;  p.y = y;
            sp.process (src.data() + off, src.data() + off,
                        aL.data() + off, aR.data() + off, kBlk, p);
            legacy.process (src.data() + off, bL.data() + off, bR.data() + off, kBlk, x, y);
        }
        const bool same = std::memcmp (aL.data(), bL.data(), sizeof (float) * (size_t) kN) == 0
                       && std::memcmp (aR.data(), bR.data(), sizeof (float) * (size_t) kN) == 0;
        check (same, "neutral params are BIT-EXACT against the v0.44 pan stage");
        check (SpatialEngine::isNeutral (SpatialEngine::Params{}), "defaults report as neutral");
    }

    // ---- 2. a centred pad is an exact copy -------------------------------
    {
        SpatialEngine sp;  sp.prepare (kSr, kBlk);
        std::vector<float> oL ((size_t) kN), oR ((size_t) kN);
        SpatialEngine::Params p;
        for (int blk = 0; blk < kBlocks; ++blk)
        {
            const int off = blk * kBlk;
            sp.process (src.data() + off, src.data() + off,
                        oL.data() + off, oR.data() + off, kBlk, p);
        }
        check (std::memcmp (oL.data(), src.data(), sizeof (float) * (size_t) kN) == 0
            && std::memcmp (oR.data(), src.data(), sizeof (float) * (size_t) kN) == 0,
               "centred + neutral is a byte-for-byte copy of the input");
    }

    // ---- 3. distance amount scales the classic attenuation ---------------
    {
        auto run = [&] (float distP)
        {
            SpatialEngine sp;  sp.prepare (kSr, kBlk);
            std::vector<float> oL ((size_t) kN), oR ((size_t) kN);
            SpatialEngine::Params p;
            p.y = 1.0f;                    // straight ahead, fully distant
            p.distanceP = distP;
            for (int blk = 0; blk < kBlocks; ++blk)
            {
                const int off = blk * kBlk;
                sp.process (src.data() + off, src.data() + off,
                            oL.data() + off, oR.data() + off, kBlk, p);
            }
            // measure on the second half, after the smoother has settled
            std::vector<float> tail (oL.begin() + kN / 2, oL.end());
            return std::sqrt (energy (tail));
        };
        const double at0   = run (0.0f);
        const double at100 = run (100.0f);
        const double at200 = run (200.0f);
        // dg = 1 - 0.6*amt: 1.0 / 0.4 / -0.2 -> |0.2|
        std::printf ("   distance rms  0%%=%.4f  100%%=%.4f  200%%=%.4f\n", at0, at100, at200);
        check (std::abs (at100 / at0 - 0.4) < 0.02, "100 % distance = the classic -8 dB at the rim");
        check (at200 < at100 && at100 < at0,        "more distance amount = quieter");
    }

    // ---- 4. binaural: the far ear is later AND duller ---------------------
    // x = 0.6, not 1.0: at the very rim the constant-power pan puts the far
    // ear at EXACTLY zero gain, so there is no far-ear signal left to measure
    // a delay or a filter on. (The cue still exists there; it just cannot be
    // observed, and a test that correlates two channels one of which is
    // silence reports whatever lag its search happened to start at.)
    {
        SpatialEngine sp;  sp.prepare (kSr, kBlk);
        std::vector<float> oL ((size_t) kN), oR ((size_t) kN);
        SpatialEngine::Params p;
        p.x = 0.6f;  p.binaural = true;    // to the right: the LEFT ear is far
        for (int blk = 0; blk < kBlocks; ++blk)
        {
            const int off = blk * kBlk;
            sp.process (src.data() + off, src.data() + off,
                        oL.data() + off, oR.data() + off, kBlk, p);
        }
        std::vector<float> tL (oL.begin() + kN / 2, oL.end());
        std::vector<float> tR (oR.begin() + kN / 2, oR.end());
        const int lag = lagOf (tR, tL, 96);
        const double itdMs = 1000.0 * lag / kSr;
        // Woodworth at asin(0.6) = 36.9 deg: (0.0875/343)*(0.6435+0.6) = 0.317 ms
        std::printf ("   ITD %d samples (%.3f ms), expected 0.317 ms\n", lag, itdMs);
        check (std::abs (itdMs - 0.317) < 0.04, "far ear arrives one Woodworth ITD late");

        const double hfNear = hfEnergy (tR) / energy (tR);
        const double hfFar  = hfEnergy (tL) / energy (tL);
        std::printf ("   HF share  near ear %.4f  far ear %.4f\n", hfNear, hfFar);
        check (hfFar < hfNear * 0.8, "far ear is head-shadowed (loses high end)");

        // and with the switch off there is no delay at all
        SpatialEngine sp2;  sp2.prepare (kSr, kBlk);
        std::vector<float> nL ((size_t) kN), nR ((size_t) kN);
        SpatialEngine::Params q;  q.x = 0.6f;      // binaural stays false
        for (int blk = 0; blk < kBlocks; ++blk)
        {
            const int off = blk * kBlk;
            sp2.process (src.data() + off, src.data() + off,
                         nL.data() + off, nR.data() + off, kBlk, q);
        }
        std::vector<float> uL (nL.begin() + kN / 2, nL.end());
        std::vector<float> uR (nR.begin() + kN / 2, nR.end());
        check (lagOf (uR, uL, 96) == 0, "binaural off leaves the two ears sample-aligned");
    }

    // ---- 5. behind you is duller than in front ---------------------------
    // Measured as tone gain at 6 kHz with the level difference divided out,
    // so this is the FILTER alone and not the -1.4 dB the behind-you level
    // dip also applies.
    {
        SpatialEngine::Params front, back;
        front.y =  0.8f;  front.binaural = true;
        back .y = -0.8f;  back .binaural = true;
        const double f6 = toneGainDb (6000.0f, front), b6 = toneGainDb (6000.0f, back);
        const double f2 = toneGainDb (200.0f,  front), b2 = toneGainDb (200.0f,  back);
        std::printf ("   6 kHz  front %.2f dB  behind %.2f dB   |  200 Hz  %.2f / %.2f dB\n",
                     f6, b6, f2, b2);
        const double hfDrop = (f6 - b6) - (f2 - b2);
        std::printf ("   behind-you HF cue: %.2f dB at 6 kHz relative to 200 Hz\n", hfDrop);
        check (hfDrop > 4.0, "behind-you costs at least 4 dB of 6 kHz that in-front keeps");
        check (std::abs (f6 - f2) < 0.3, "in front, the stage is flat (no colouring)");
    }

    // ---- 6. air absorption grows with distance ---------------------------
    {
        auto hfShare = [&] (float airP, float r)
        {
            SpatialEngine sp;  sp.prepare (kSr, kBlk);
            std::vector<float> oL ((size_t) kN), oR ((size_t) kN);
            SpatialEngine::Params p;
            p.y = r;  p.airP = airP;
            for (int blk = 0; blk < kBlocks; ++blk)
            {
                const int off = blk * kBlk;
                sp.process (src.data() + off, src.data() + off,
                            oL.data() + off, oR.data() + off, kBlk, p);
            }
            std::vector<float> t (oL.begin() + kN / 2, oL.end());
            return hfEnergy (t) / energy (t);
        };
        const double off  = hfShare (0.0f,   1.0f);
        const double near = hfShare (100.0f, 0.3f);
        const double far  = hfShare (100.0f, 1.0f);
        std::printf ("   HF share  air off %.4f  near %.4f  far %.4f\n", off, near, far);
        check (far < near && near < off, "air absorption increases with distance");
    }

    // ---- 7. the room is stable, decays, and never blows up ---------------
    {
        SpatialEngine sp;  sp.prepare (kSr, kBlk);
        SpatialEngine::Params p;
        p.roomP = 100.0f;  p.sizeP = 100.0f;  p.y = 0.9f;
        std::vector<float> oL ((size_t) kBlk), oR ((size_t) kBlk);

        // 2 s of signal, then 8 s of silence: the tail must run out
        double loud = 0.0, tail1 = 0.0, tail8 = 0.0;
        bool finite = true;
        for (int blk = 0; blk < kSr * 10 / kBlk; ++blk)
        {
            std::vector<float> in ((size_t) kBlk, 0.0f);
            if (blk < kSr * 2 / kBlk)
                for (int i = 0; i < kBlk; ++i)
                    in[(size_t) i] = src[(size_t) ((blk * kBlk + i) % kN)];
            sp.process (in.data(), in.data(), oL.data(), oR.data(), kBlk, p);
            for (int i = 0; i < kBlk; ++i)
                if (! std::isfinite (oL[(size_t) i]) || ! std::isfinite (oR[(size_t) i]))
                    finite = false;
            const double e = energy (oL);
            const double sec = (double) (blk * kBlk) / kSr;
            if (sec > 1.0 && sec < 2.0) loud += e;
            if (sec > 2.5 && sec < 3.5) tail1 += e;
            if (sec > 9.0)              tail8 += e;
        }
        std::printf ("   room energy  live %.4g  +1 s %.4g  +7 s %.4g\n", loud, tail1, tail8);
        check (finite, "room output stays finite");
        check (tail1 > 0.0 && tail1 < loud * 0.2, "a tail exists and is well below the live level");
        check (tail8 < tail1 * 1.0e-3, "the tail actually dies out (no runaway feedback)");
    }

    // ---- 8. width -------------------------------------------------------
    {
        auto run = [&] (float widthP, std::vector<float>& oL, std::vector<float>& oR)
        {
            SpatialEngine sp;  sp.prepare (kSr, kBlk);
            oL.assign ((size_t) kN, 0.0f);  oR.assign ((size_t) kN, 0.0f);
            SpatialEngine::Params p;
            p.widthP = widthP;
            // a genuinely stereo source, or width has nothing to act on
            std::vector<float> right = voiceLike (kN, 99);
            for (int blk = 0; blk < kBlocks; ++blk)
            {
                const int off = blk * kBlk;
                sp.process (src.data() + off, right.data() + off,
                            oL.data() + off, oR.data() + off, kBlk, p);
            }
        };
        std::vector<float> mL, mR, wL, wR;
        run (0.0f, mL, mR);
        check (std::memcmp (mL.data(), mR.data(), sizeof (float) * (size_t) kN) == 0,
               "width 0 % collapses to identical channels");
        run (200.0f, wL, wR);
        std::vector<float> sNarrow ((size_t) kN), sWide ((size_t) kN);
        std::vector<float> dL, dR;
        run (100.0f, dL, dR);
        for (int i = 0; i < kN; ++i)
        {
            sNarrow[(size_t) i] = 0.5f * (dL[(size_t) i] - dR[(size_t) i]);
            sWide  [(size_t) i] = 0.5f * (wL[(size_t) i] - wR[(size_t) i]);
        }
        const double r = std::sqrt (energy (sWide) / energy (sNarrow));
        std::printf ("   side energy ratio at 200 %%: %.3f (expected 2.0)\n", r);
        check (std::abs (r - 2.0) < 0.01, "width 200 % doubles the side signal");
    }

    // ---- 9. the orbit really travels round the head ----------------------
    {
        SpatialEngine sp;  sp.prepare (kSr, kBlk);
        SpatialEngine::Params p;
        p.orbitHz = 0.5f;  p.orbitDepthP = 80.0f;      // 2 s per lap, centred pad
        std::vector<float> oL ((size_t) kBlk), oR ((size_t) kBlk);
        std::vector<float> in ((size_t) kBlk, 0.0f);
        float minX = 1.0f, maxX = -1.0f, minY = 1.0f, maxY = -1.0f, minR = 9.0f, maxR = 0.0f;
        for (int blk = 0; blk < kSr * 2 / kBlk; ++blk)
        {
            sp.process (in.data(), in.data(), oL.data(), oR.data(), kBlk, p);
            const float x = sp.currentX(), y = sp.currentY();
            minX = std::min (minX, x);  maxX = std::max (maxX, x);
            minY = std::min (minY, y);  maxY = std::max (maxY, y);
            const float rr = std::sqrt (x * x + y * y);
            minR = std::min (minR, rr);  maxR = std::max (maxR, rr);
        }
        std::printf ("   orbit x [%.2f %.2f]  y [%.2f %.2f]  r [%.3f %.3f]\n",
                     minX, maxX, minY, maxY, minR, maxR);
        check (minX < -0.75f && maxX > 0.75f, "one lap reaches both ears");
        check (minY < -0.75f && maxY > 0.75f, "one lap reaches front and back");
        check (std::abs (maxR - minR) < 0.01f, "the orbit is a circle (constant radius)");
        check (std::abs (maxR - 0.8f) < 0.01f, "Depth sets that radius");

        // depth 0 with a pad that is off-centre: the existing radius wins
        SpatialEngine sp2;  sp2.prepare (kSr, kBlk);
        SpatialEngine::Params q;
        q.x = 0.0f;  q.y = 0.5f;  q.orbitHz = 0.5f;  q.orbitDepthP = 0.0f;
        float r2max = 0.0f;
        for (int blk = 0; blk < kSr * 2 / kBlk; ++blk)
        {
            sp2.process (in.data(), in.data(), oL.data(), oR.data(), kBlk, q);
            r2max = std::max (r2max, std::sqrt (sp2.currentX() * sp2.currentX()
                                              + sp2.currentY() * sp2.currentY()));
        }
        check (std::abs (r2max - 0.5f) < 0.01f, "with Depth 0 the pad's own radius orbits");
    }

    // ---- 10. mono bus: no pan, no ITD, but distance and room still work ---
    {
        SpatialEngine sp;  sp.prepare (kSr, kBlk);
        SpatialEngine::Params p;
        p.x = -1.0f;  p.binaural = true;  p.widthP = 0.0f;   // all L/R-only
        std::vector<float> o ((size_t) kN);
        for (int blk = 0; blk < kBlocks; ++blk)
        {
            const int off = blk * kBlk;
            sp.process (src.data() + off, nullptr, o.data() + off, nullptr, kBlk, p);
        }
        std::vector<float> t (o.begin() + kN / 2, o.end());
        std::vector<float> ref (src.begin() + kN / 2, src.end());
        const double g = std::sqrt (energy (t) / energy (ref));
        std::printf ("   mono gain at the rim: %.3f (expected 0.40)\n", g);
        check (std::abs (g - 0.4) < 0.02, "mono keeps the distance attenuation only");
        bool finite = true;
        for (float s : o) if (! std::isfinite (s)) finite = false;
        check (finite, "mono path stays finite with every lateral feature on");
    }

    // ---- 11. nothing produces NaN under a hostile sweep -------------------
    {
        SpatialEngine sp;  sp.prepare (kSr, kBlk);
        std::mt19937 rng (1234);
        std::uniform_real_distribution<float> u (-1.5f, 1.5f);
        std::vector<float> oL ((size_t) kBlk), oR ((size_t) kBlk);
        bool finite = true;
        float peak = 0.0f;
        for (int blk = 0; blk < 600; ++blk)
        {
            SpatialEngine::Params p;
            p.x = u (rng);  p.y = u (rng);
            p.binaural = (blk % 3) != 0;
            p.airP  = 100.0f * std::abs (u (rng));
            p.roomP = 100.0f * std::abs (u (rng));
            p.sizeP = 100.0f * std::abs (u (rng));
            p.widthP = 200.0f * std::abs (u (rng));
            p.orbitHz = std::abs (u (rng));
            p.distanceP = 200.0f * std::abs (u (rng));
            const int off = (blk * kBlk) % (kN - kBlk);
            sp.process (src.data() + off, src.data() + off,
                        oL.data() + off * 0, oR.data() + off * 0, kBlk, p);
            for (int i = 0; i < kBlk; ++i)
            {
                if (! std::isfinite (oL[(size_t) i]) || ! std::isfinite (oR[(size_t) i]))
                    finite = false;
                peak = std::max (peak, std::abs (oL[(size_t) i]));
            }
        }
        std::printf ("   hostile sweep peak: %.3f\n", peak);
        check (finite, "no NaN/Inf while every control is thrashed at once");
        check (peak < 8.0f, "output stays bounded under the sweep");
    }

    std::printf ("\n%s (%d failure%s)\n", g_fail == 0 ? "ALL PASS" : "FAILURES",
                 g_fail, g_fail == 1 ? "" : "s");
    return g_fail == 0 ? 0 : 1;
}
