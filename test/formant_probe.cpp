// formant_probe.cpp - ground truth for the engine's F1-F3 tracker (v0.47.1).
//
// Build:  g++ -O2 -std=c++17 -I dsp -o /tmp/fp test/formant_probe.cpp && /tmp/fp
//
// Synthesises vowels with KNOWN formants (glottal source -> 3 cascaded
// resonators -> lip radiation) and compares the engine's own reading, taken
// through its analysis taps, against them across f0 and pitch shift.
//
// TWO THINGS THIS TOOL EXISTS TO STOP YOU DOING, both of which cost a day:
//
//  1. Do not omit the lip radiation term. Without it the cascade falls about
//     -33 dB/oct, F2 and F3 are not maxima at all, and the tool reports a
//     13 st "tracker error" that is entirely the test signal's.
//
//  2. Do not trust the ground truth without the signal check below. An /a/
//     at F1 730 / F2 1090 genuinely merges into ONE hump once the source
//     tilt is included: the resonator is there, the spectral maximum is not.
//     Measuring a peak-picking tracker against a peak that does not exist
//     tells you nothing. The check prints the humps the signal actually
//     carries; if there are fewer than three, that vowel cannot be used to
//     judge F2.
//
// See HANDOVER.md v0.47.1 for what was measured with it and why the obvious
// fixes (ordering, prominence gating, curvature) were not shipped.
#include "PsolaEngine.h"
#include <cstdio>
#include <random>
#include <algorithm>
#include <vector>
#include <cmath>

static const double kFs = 48000.0;

// one two-pole resonator
struct Res
{
    double a1 = 0, a2 = 0, z1 = 0, z2 = 0, g = 1;
    void set (double f, double bw)
    {
        const double r = std::exp (-M_PI * bw / kFs);
        const double th = 2.0 * M_PI * f / kFs;
        a1 = 2.0 * r * std::cos (th);
        a2 = -r * r;
        g  = (1.0 - a1 + -a2);            // unity at DC-ish; scale is irrelevant
    }
    double tick (double x)
    {
        const double y = g * x + a1 * z1 + a2 * z2;
        z2 = z1; z1 = y;
        return y;
    }
};

// glottal source + 3 cascaded resonators + a little aspiration
static std::vector<float> vowel (double f0, double F1, double F2, double F3,
                                 const double* BW, double sec)
{
    const int n = (int) (kFs * sec);
    std::vector<float> out ((size_t) n, 0.0f);
    Res r1, r2, r3;
    r1.set (F1, BW[0]); r2.set (F2, BW[1]); r3.set (F3, BW[2]);
    std::mt19937 rng (12345);
    std::normal_distribution<double> nd (0.0, 1.0);
    double phase = 0.0, prevV = 0.0;
    const double T = kFs / f0;
    for (int i = 0; i < n; ++i)
    {
        // Rosenberg-ish glottal pulse over the first 60 % of the period
        const double t = phase / T;
        double src;
        if (t < 0.4)        src = 0.5 * (1.0 - std::cos (M_PI * t / 0.4));
        else if (t < 0.6)   src = std::cos (M_PI * (t - 0.4) / 0.4);
        else                src = 0.0;
        src -= 0.35;                              // remove most of the DC
        src += 0.012 * nd (rng);                  // aspiration
        phase += 1.0;
        if (phase >= T) phase -= T;
        const double v = r3.tick (r2.tick (r1.tick (src)));
        // LIP RADIATION. Without this the signal is a flow waveform, not a
        // pressure one: the cascade is DC-normalised, so each section adds
        // -12 dB/oct above its own resonance and the composite falls about
        // -33 dB/oct. F2 and F3 then are not maxima at all -- the first
        // version of this probe "measured" the engine on a signal whose F2
        // did not exist, and reported a 13 st error that was mine.
        out[(size_t) i] = (float) (0.25 * (v - prevV));
        prevV = v;
    }
    // normalise
    float pk = 1e-9f;
    for (auto v : out) pk = std::max (pk, std::abs (v));
    for (auto& v : out) v *= 0.35f / pk;
    return out;
}

static float median (std::vector<float>& v)
{
    if (v.empty()) return 0.0f;
    std::sort (v.begin(), v.end());
    return v[v.size() / 2];
}

// Where are the maxima of the signal's own long-window spectral envelope?
// If they are not near the ground truth then the test material is wrong and
// nothing measured against it means anything.
static std::vector<double> signalPeakList (const std::vector<float>& sig, double f0)
{
    std::vector<double> found;
    const int N = 8192;
    std::vector<float> re ((size_t) N, 0.0f), im ((size_t) N, 0.0f);
    const int off = (int) (kFs * 0.5);
    for (int i = 0; i < N; ++i)
    {
        const float w = 0.5f * (1.0f - std::cos (2.0f * (float) M_PI * i / N));
        re[(size_t) i] = sig[(size_t) (off + i)] * w;
    }
    PsolaEngine::fftForViz (re.data(), im.data(), N);
    std::vector<double> mag ((size_t) (N / 2));
    for (int k = 0; k < N / 2; ++k)
        mag[(size_t) k] = std::sqrt (re[(size_t) k] * re[(size_t) k]
                                   + im[(size_t) k] * im[(size_t) k]);
    // Smooth over a bit more than one harmonic spacing to get an envelope,
    // then take maxima of THAT with a prominence floor. Comparing raw
    // neighbouring bins of a smoothed curve finds dozens of "peaks" that are
    // leftover harmonic ripple -- the first version of this check printed 77
    // of them and told me nothing.
    const int r = std::max (2, (int) (1.2 * f0 * N / kFs));
    std::vector<double> smth ((size_t) (N / 2), 0.0);
    for (int k = 0; k < N / 2; ++k)
    {
        double acc = 0.0; int n = 0;
        for (int t = -r; t <= r; ++t)
        {
            const int q = k + t;
            if (q >= 0 && q < N / 2) { acc += mag[(size_t) q]; ++n; }
        }
        smth[(size_t) k] = 20.0 * std::log10 (acc / std::max (1, n) + 1e-12);
    }
    const int gap = std::max (3, (int) (150.0 * N / kFs));   // 150 Hz apart
    for (int k = gap; k < N / 2 - gap; ++k)
    {
        const double hz = k * kFs / N;
        if (hz < 200.0 || hz > 4000.0) continue;
        bool top = true;
        for (int t = -gap; t <= gap && top; ++t) if (smth[(size_t) (k + t)] > smth[(size_t) k]) top = false;
        if (! top) continue;
        double lo = smth[(size_t) k];
        for (int t = -gap; t <= gap; ++t) lo = std::min (lo, smth[(size_t) (k + t)]);
        if (smth[(size_t) k] - lo < 1.0) continue;           // prominence, dB
        found.push_back (hz);
    }
    return found;
}

// returns the envelope maxima the SIGNAL actually carries (Hz)
static std::vector<double> signalPeakList (const std::vector<float>& sig, double f0);

int main (int argc, char** argv)
{
    const bool dump = argc > 1 && std::string (argv[1]) == "--dump";
    (void) dump;

    struct V { const char* name; double F[3]; double BW[3]; };
    const V vowels[] = {
        { "/a/ merged", { 730, 1090, 2440 }, { 80, 110, 150 } },
        { "/e/",        { 530, 1840, 2480 }, { 70, 110, 150 } },
        { "/i/",        { 300, 2200, 3000 }, { 60, 110, 160 } },
        { "/u/ wide",   { 350, 1250, 2200 }, { 60, 100, 150 } },
    };

    for (const auto& vw : vowels)
    {
        std::printf ("\n=== %s  true %.0f / %.0f / %.0f Hz ===\n",
                     vw.name, vw.F[0], vw.F[1], vw.F[2]);
        for (double f0 : { 110.0, 140.0, 200.0 })
        {
            auto sig = vowel (f0, vw.F[0], vw.F[1], vw.F[2], vw.BW, 1.2);
            const auto pk = signalPeakList (sig, f0);
            std::printf (" f0=%3.0f  signal humps:", f0);
            for (auto h : pk) std::printf (" %.0f", h);
            const bool usable = pk.size() >= 3;
            std::printf ("  -> %s\n", usable ? "3 humps, usable" : "NOT usable for F2");
            if (! usable) continue;

            for (double st : { 0.0, 2.0, 4.0, 5.0, 7.0, 9.0, 12.0 })
            {
                PsolaEngine eng;
                eng.prepare (kFs);
                PsolaEngine::Params p;
                p.pitchSemi = (float) st;
                p.vowelAdapt = true;
                p.vowelAdaptAmt = 1.0f;
                eng.setParams (p);

                std::vector<float> meas[3];
                std::vector<float> buf (512);
                for (size_t q = 0; q + 512 <= sig.size(); q += 512)
                {
                    std::copy (sig.begin() + (long) q, sig.begin() + (long) q + 512, buf.begin());
                    eng.process (buf.data(), buf.data(), 512);
                    if (q < kFs * 0.35 || ! eng.formantsValid()) continue;
                    for (int k = 0; k < 3; ++k)
                        if (eng.analysisFormantIn (k) > 20.0f)
                            meas[k].push_back (eng.analysisFormantIn (k));
                }
                float m[3];
                for (int k = 0; k < 3; ++k) m[k] = median (meas[k]);
                std::printf ("   %4.0f st |", st);
                for (int k = 0; k < 3; ++k)
                    std::printf (" %6.0f (%+6.2f st) |", m[k],
                                 m[k] > 0 ? 12.0 * std::log2 (m[k] / vw.F[k]) : 0.0);
                std::printf ("\n");
            }
        }
    }
    return 0;
}
