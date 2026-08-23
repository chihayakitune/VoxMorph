// release_probe.cpp - the ending, swept over speaker pitch and shift amount.
//
// A real take only has the one speaker at the one shift. This generates a
// phrase per condition -- silence, attack, steady, decay -- and reports what
// the release treatment does to the ending, so a setting can be checked
// against a range of voices rather than against the voice it was tuned on.
//
// Build:  g++ -O2 -std=c++17 -I dsp -o /tmp/rp test/release_probe.cpp
// Run:    /tmp/rp            (prints the whole grid as CSV on stdout)
#define PSOLA_DETECT_LOG 1
#include "PsolaEngine.h"
#include <cstdio>
#include <vector>
#include <cmath>
#include <complex>

static std::vector<float> phrase (double fs, double f0, double sec = 1.1)
{
    const int n = (int) (fs * sec);
    std::vector<float> x ((size_t) n, 0.0f);
    double ph = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double t = i / fs;
        ph += f0 / fs;  if (ph >= 1.0) ph -= 1.0;
        double s = 0.0;
        for (int k = 1; k <= 24; ++k)
            s += std::exp (-0.16 * (k - 1)) * std::sin (2.0 * M_PI * k * ph);
        double env = 0.0;
        if (t > 0.30 && t < 0.70)      env = std::min (1.0, (t - 0.30) / 0.03);
        else if (t >= 0.70 && t < 0.82) env = std::max (0.0, 1.0 - (t - 0.70) / 0.10);
        x[(size_t) i] = (float) (0.28 * env * s);
    }
    return x;
}

static double bandDb (const std::vector<float>& x, double fs, int a, int n, double lo, double hi)
{
    if (a < 0 || a + n > (int) x.size()) return -200.0;
    const int N = 32768;
    std::vector<float> re ((size_t) N, 0.0f), im ((size_t) N, 0.0f);
    for (int i = 0; i < n; ++i)
        re[(size_t) i] = x[(size_t) (a + i)] * (0.5f - 0.5f * std::cos (2.0f * (float) M_PI * i / n));
    PsolaEngine::fftForViz (re.data(), im.data(), N);
    double acc = 0.0;
    for (int k = (int) (lo * N / fs); k <= (int) (hi * N / fs) && k <= N / 2; ++k)
        acc += re[(size_t) k] * re[(size_t) k] + im[(size_t) k] * im[(size_t) k];
    return 10.0 * std::log10 (acc + 1e-20);
}

int main()
{
    const double fs = 48000.0;
    const double F0[] = { 80.0, 110.0, 160.0, 220.0 };
    const double PS[] = { -5.0, 0.0, 3.0, 9.0, 12.0 };
    std::printf ("f0in,pitch,mode,strength,f0out,cut_hint,d_inF0,d_outF0,d_60_140,d_150_600,"
                 "d_600_5k,d_env,maxd1,maxd2\n");
    for (double f0 : F0)
        for (double ps : PS)
        {
            auto in = phrase (fs, f0);
            const double f0out = f0 * std::pow (2.0, ps / 12.0);
            double lastVoiced = 0.0;
            auto run = [&] (bool repair, float amt, int mode)
            {
                PsolaEngine::Params p;
                p.pitchSemi = (float) ps;  p.formantSemi = 2.0f;
                p.grainAvg = true;  p.pulseBody = 0.75f;
                p.onsetHold = 3;    p.preLockLowCut = 0.75f;  p.onsetBackfill = true;
                p.releaseRepair = repair;  p.releaseShelf = amt;  p.releaseCutMode = mode;
                PsolaEngine e;  e.prepare (fs);  e.setParams (p);
                std::vector<float> out (in.size(), 0.0f);
                for (size_t i = 0; i < in.size(); i += 256)
                {
                    const int c = (int) std::min ((size_t) 256, in.size() - i);
                    e.process (in.data() + i, out.data() + i, c);
                }
                for (const auto& r : e.detectLog)
                    if (r.voicedAfter) lastVoiced = r.t;
                return out;
            };
            const auto base = run (false, 0.0f, 0);
            // Window the tail from where the ENGINE stopped calling it voiced,
            // not from a fixed time. RELEASE begins there, and a fixed window
            // chosen from the input's envelope missed it entirely -- every
            // reading came out 0.00 dB and the sweep looked like a no-op.
            const int n = (int) (0.030 * fs);
            // ...and only where there is still something to measure. Landing
            // in the silence past the decay makes every ratio explode: the
            // first attempt reported "+97 dB" of suppression. Walk back until
            // the untreated tail is at least -55 dBFS, and skip the condition
            // if it never is.
            int a = (int) (lastVoiced * fs) + 2048;
            auto rms = [&] (const std::vector<float>& x, int s0)
            {
                double e = 0.0;
                for (int i = 0; i < n && s0 + i < (int) x.size(); ++i) e += x[(size_t)(s0+i)] * x[(size_t)(s0+i)];
                return 10.0 * std::log10 (e / n + 1e-20);
            };
            while (a > (int) (0.35 * fs) && rms (base, a) < -55.0) a -= n / 3;
            if (rms (base, a) < -55.0) continue;
            for (int mode = 0; mode < 3; ++mode)
                for (float amt : { 0.40f, 0.50f })
                {
                    const auto y = run (true, amt, mode);
                    auto d = [&] (double lo, double hi)
                    { return bandDb (y, fs, a, n, lo, hi) - bandDb (base, fs, a, n, lo, hi); };
                    double e1 = 0.0, e2 = 0.0;
                    for (int i = a + 2; i < a + n; ++i)
                    {
                        e1 = std::max (e1, (double) std::fabs (y[(size_t) i] - y[(size_t) (i-1)]));
                        e2 = std::max (e2, (double) std::fabs (y[(size_t) i] - 2.0f * y[(size_t) (i-1)] + y[(size_t) (i-2)]));
                    }
                    double se = 0.0, sb = 0.0;
                    for (int i = a; i < a + n; ++i) { se += y[(size_t) i] * y[(size_t) i]; sb += base[(size_t) i] * base[(size_t) i]; }
                    const double hint = mode == 0 ? 200.0 : mode == 1 ? 1.6 * f0 : f0 * std::sqrt (std::pow (2.0, ps / 12.0));
                    std::printf ("%.0f,%.0f,%d,%.2f,%.0f,%.0f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.4f,%.4f\n",
                                 f0, ps, mode, amt, f0out, hint,
                                 d (f0 * 0.85, f0 * 1.15), d (f0out * 0.85, f0out * 1.15),
                                 d (60, 140), d (150, 600), d (600, 5000),
                                 10.0 * std::log10 ((se + 1e-20) / (sb + 1e-20)), e1, e2);
                }
        }
    return 0;
}
