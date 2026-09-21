// level_probe.cpp -- does input LEVEL move the engine's detection or conversion?
//
// Build:  g++ -O2 -std=c++17 -I dsp -o /tmp/lp test/level_probe.cpp && /tmp/lp
//
// Written to decide whether Auto Protection has a job at all (2026-09-21). Its
// two intended purposes were:
//   (a) stop formant/pitch DETECTION being thrown off by level changes
//   (b) stop the CONVERSION wobbling when the level changes
// Both are only worth building if the engine is actually level-sensitive, so
// this measures that directly, on the engine alone, with the SAME vowel at
// different levels and under level changes. Nothing here is a pass/fail test;
// it prints numbers for a decision.
//
// Test signal: the formant_probe generator (glottal source -> 3 resonators ->
// lip radiation), whose ground truth is known. F1/F2/F3 = 500/1500/2500 Hz,
// well separated so all three humps really exist in the signal.
#include "PsolaEngine.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

static const double kFs = 48000.0;
static const double kF[3] = { 500.0, 1500.0, 2500.0 };

struct Res
{
    double a1 = 0, a2 = 0, z1 = 0, z2 = 0, g = 1;
    void set (double f, double bw)
    {
        const double r = std::exp (-M_PI * bw / kFs), th = 2.0 * M_PI * f / kFs;
        a1 = 2.0 * r * std::cos (th);  a2 = -r * r;  g = 1.0 - a1 - a2;
    }
    double tick (double x) { const double y = g * x + a1 * z1 + a2 * z2; z2 = z1; z1 = y; return y; }
};

// unit-peak vowel (peak 1.0), so a level in dBFS is a plain gain
static std::vector<float> vowel (double f0, double sec)
{
    const int n = (int) (kFs * sec);
    std::vector<float> out ((size_t) n);
    Res r1, r2, r3;  r1.set (kF[0], 60); r2.set (kF[1], 90); r3.set (kF[2], 120);
    std::mt19937 rng (12345);  std::normal_distribution<double> nd (0.0, 1.0);
    double ph = 0.0, prevV = 0.0;  const double T = kFs / f0;
    for (int i = 0; i < n; ++i)
    {
        const double t = ph / T;
        double s = t < 0.4 ? 0.5 * (1.0 - std::cos (M_PI * t / 0.4))
                 : t < 0.6 ? std::cos (M_PI * (t - 0.4) / 0.4) : 0.0;
        s += -0.35 + 0.012 * nd (rng);
        ph += 1.0;  if (ph >= T) ph -= T;
        const double v = r3.tick (r2.tick (r1.tick (s)));
        out[(size_t) i] = (float) (0.25 * (v - prevV));  prevV = v;     // lip radiation
    }
    float pk = 1e-9f;  for (float v : out) pk = std::max (pk, std::abs (v));
    for (auto& v : out) v /= pk;
    return out;
}

static float med (std::vector<float> v) { if (v.empty()) return 0; std::sort (v.begin(), v.end()); return v[v.size()/2]; }
static float sd  (const std::vector<float>& v)
{
    if (v.size() < 2) return 0;
    double m = 0; for (float x : v) m += x;  m /= v.size();
    double s = 0; for (float x : v) s += (x - m) * (x - m);
    return (float) std::sqrt (s / (v.size() - 1));
}
static float st (float f, float ref) { return f > 0 && ref > 0 ? 12.0f * std::log2 (f / ref) : 0.0f; }

// A realistic conversion: pitch and formant moved, so the spectral layer
// (where formant detection lives) actually runs.
static PsolaEngine::Params conv()
{
    PsolaEngine::Params p;
    p.pitchSemi = 5.0f;  p.formantSemi = 2.0f;  p.airPreserve = 0.3f;
    p.vowelAdapt = true; p.vowelAdaptAmt = 1.0f;       // keeps formant tracking live
    return p;
}

struct Readout { std::vector<float> f0, F[3], conf[3]; int frames = 0, valid = 0; };

// Runs `sig` through a fresh engine; collects the analysis readouts after a
// settle time, plus the converted output.
static Readout run (const std::vector<float>& sig, std::vector<float>* out = nullptr)
{
    PsolaEngine e;  e.prepare (kFs);  e.setParams (conv());
    Readout r;
    std::vector<float> buf (512);
    if (out) out->assign (sig.size(), 0.0f);
    for (size_t q = 0; q + 512 <= sig.size(); q += 512)
    {
        std::copy (sig.begin() + (long) q, sig.begin() + (long) q + 512, buf.begin());
        e.process (buf.data(), buf.data(), 512);
        if (out) std::copy (buf.begin(), buf.end(), out->begin() + (long) q);
        if (q < kFs * 0.4) continue;                            // settle
        ++r.frames;
        if (e.analysisF0In() > 0) r.f0.push_back (e.analysisF0In());
        if (! e.formantsValid()) continue;
        ++r.valid;
        for (int k = 0; k < 3; ++k)
        {
            if (e.analysisFormantIn (k) > 20.0f) r.F[k].push_back (e.analysisFormantIn (k));
            r.conf[k].push_back (e.formantConfidence (k));
        }
    }
    return r;
}

int main()
{
    const double f0 = 140.0;
    const auto base = vowel (f0, 3.0);

    // ---- A. static: the same vowel at different levels ----------------------
    std::printf ("== A. detection vs level (same vowel, f0 %.0f Hz, true F %.0f/%.0f/%.0f) ==\n",
                 f0, kF[0], kF[1], kF[2]);
    std::printf ("  level  |  f0 med   | F1 err  F2 err  F3 err (st) | conf F1/F2/F3   | valid\n");
    const float levels[] = { -60, -54, -48, -42, -36, -30, -24, -18, -12, -6, 0, 6 };
    std::vector<float> refOut;
    const float refDb = -18.0f;
    {
        auto s = base;  const float g = std::pow (10.0f, refDb / 20.0f);
        for (auto& v : s) v *= g;
        run (s, &refOut);
    }
    std::printf ("\n");
    std::vector<std::pair<float, double>> convResid;
    for (float L : levels)
    {
        auto s = base;  const float g = std::pow (10.0f, L / 20.0f);
        for (auto& v : s) v *= g;
        std::vector<float> o;
        const auto r = run (s, &o);
        std::printf ("  %+4.0f dB | %6.1f Hz | %+6.2f  %+6.2f  %+6.2f     | %.2f/%.2f/%.2f  | %3d/%3d\n",
                     L, med (r.f0),
                     st (med (r.F[0]), (float) kF[0]), st (med (r.F[1]), (float) kF[1]),
                     st (med (r.F[2]), (float) kF[2]),
                     med (r.conf[0]), med (r.conf[1]), med (r.conf[2]), r.valid, r.frames);

        // ---- B. conversion: does the output scale linearly with the input?
        // Output at level L, divided by the level ratio, against the output at
        // the reference level. A level-invariant conversion leaves only float
        // rounding (~-120 dB).
        const float k = std::pow (10.0f, (refDb - L) / 20.0f);
        double num = 0, den = 0;
        for (size_t i = (size_t) (kFs * 0.5); i < o.size() && i < refOut.size(); ++i)
        {
            const double d = (double) o[i] * k - refOut[i];
            num += d * d;  den += (double) refOut[i] * refOut[i];
        }
        convResid.push_back ({ L, den > 0 ? 10.0 * std::log10 (std::max (num / den, 1e-30)) : 0.0 });
    }

    std::printf ("\n== B. conversion vs level (output / level ratio, against %+.0f dBFS) ==\n", refDb);
    for (auto& c : convResid)
        std::printf ("  %+4.0f dB : residual %7.1f dB%s\n", c.first, c.second,
                     c.second < -80 ? "   (level-invariant)" : "");

    // ---- C. dynamic: the level MOVES while the voice does not ----------------
    // Steady vowel vs the same vowel under a +-12 dB swell at 3 Hz and under a
    // sudden +18 dB step (a shout onset). If level changes throw detection off,
    // the f0 / formant readings will scatter more than in the steady case.
    std::printf ("\n== C. detection stability while the level moves (vowel unchanged) ==\n");
    std::printf ("  case                 | f0 sd   | F1 sd  F2 sd  F3 sd (Hz) | F2 med err | valid\n");
    auto report = [&] (const char* name, const std::vector<float>& s)
    {
        const auto r = run (s);
        std::printf ("  %-20s | %5.2f   | %5.1f  %5.1f  %5.1f      | %+6.2f st  | %3d/%3d\n",
                     name, sd (r.f0), sd (r.F[0]), sd (r.F[1]), sd (r.F[2]),
                     st (med (r.F[1]), (float) kF[1]), r.valid, r.frames);
    };
    {
        auto s = base;  const float g = std::pow (10.0f, -18.0f / 20.0f);
        for (auto& v : s) v *= g;
        report ("steady -18 dBFS", s);
    }
    {
        auto s = base;
        for (size_t i = 0; i < s.size(); ++i)
        {
            const double db = -18.0 + 12.0 * std::sin (2.0 * M_PI * 3.0 * i / kFs);
            s[i] *= (float) std::pow (10.0, db / 20.0);
        }
        report ("swell +-12 dB @3Hz", s);
    }
    {
        auto s = base;
        for (size_t i = 0; i < s.size(); ++i)
        {
            const double db = -18.0 + 24.0 * std::sin (2.0 * M_PI * 2.0 * i / kFs);
            s[i] *= (float) std::pow (10.0, std::min (db, 0.0) / 20.0);
        }
        report ("swell +-24 dB @2Hz", s);
    }
    {
        auto s = base;
        for (size_t i = 0; i < s.size(); ++i)
            s[i] *= (float) std::pow (10.0, (i < s.size() / 2 ? -24.0 : -6.0) / 20.0);
        report ("step -24 -> -6 dBFS", s);
    }
    {
        auto s = base;
        for (size_t i = 0; i < s.size(); ++i)
            s[i] *= (float) std::pow (10.0, (i < s.size() / 2 ? -54.0 : -30.0) / 20.0);
        report ("step -54 -> -30 dBFS", s);
    }
    return 0;
}
