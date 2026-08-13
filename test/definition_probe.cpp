// definition_probe.cpp — measures Formant Definition on a real recording.
//
// Phase 2.5 question set, ahead of any Matching work:
//   1. is the quantity measurable on real speech at all, or does the
//      estimator just report noise?
//   2. how far apart are real voices on it?
//   3. do F1/F2/F3 move together, or does the feature need to be split
//      into three?
//
//   c++ -std=c++17 -O2 -o definition_probe test/definition_probe.cpp
//   ./definition_probe voice.wav [halfHz]
//   ./definition_probe --compare a.wav b.wav ...      # one line per file
//
// The envelope stage is the engine's, copied verbatim from
// PsolaEngine::spectralProcess() (window -> FFT -> harmonic-peak log
// interpolation -> two radius-3 box passes) so that what is reported is what
// the DSP would act on. The reference average is the same constant-Hz box in
// the log domain; halfHz defaults to 400 to match PsolaEngine::kDefHalfHz and
// is settable so the width can be re-examined per formant.
//
// Definition per formant, per frame:
//     definition_i = 20*log10( E(Fi) / B(Fi) )
// reported as a MEDIAN over voiced frames, with the inter-quartile range,
// because a mean over speech is dominated by whatever the mouth was doing.
//
// Deliberately no JUCE and no engine dependency beyond the copied stage, so
// it builds with a plain compiler like the rest of test/.
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>

// ------------------------------- WAV in -------------------------------
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
                    const uint8_t* p = raw.data() + (i * (size_t) ch + (size_t) c) * (size_t) bytes;
                    double v = 0.0;
                    if (fmt == 3 && bits == 32)       { float t; std::memcpy (&t, p, 4); v = t; }
                    else if (bits == 8)               v = ((int) p[0] - 128) / 128.0;
                    else if (bits == 16)              { int16_t t; std::memcpy (&t, p, 2); v = t / 32768.0; }
                    else if (bits == 24)              { int32_t t = (p[0] << 8) | (p[1] << 16) | (p[2] << 24); v = t / 2147483648.0; }
                    else if (bits == 32)              { int32_t t; std::memcpy (&t, p, 4); v = t / 2147483648.0; }
                    acc += v;
                }
                out[i] = (float) (acc / ch);
            }
            std::fclose (f);
            return true;
        }
        else std::fseek (f, (long) sz + ((long) sz & 1), SEEK_CUR);
    }
    std::fclose (f);
    return false;
}

// ------------------------------- FFT ----------------------------------
static void fftRadix2 (float* re, float* im, int n, bool inv)
{
    for (int i = 1, j = 0; i < n; ++i)
    {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j |= bit;
        if (i < j) { std::swap (re[i], re[j]); std::swap (im[i], im[j]); }
    }
    for (int half = 2; half <= n; half <<= 1)
    {
        const double ang = (inv ? 2.0 : -2.0) * M_PI / half;
        const double wr = std::cos (ang), wi = std::sin (ang);
        for (int i = 0; i < n; i += half)
        {
            double cr = 1.0, ci = 0.0;
            for (int k = 0; k < half / 2; ++k)
            {
                const int a = i + k, b = i + k + half / 2;
                const float xr = (float) (re[b] * cr - im[b] * ci);
                const float xi = (float) (re[b] * ci + im[b] * cr);
                re[b] = re[a] - xr;  im[b] = im[a] - xi;
                re[a] += xr;         im[a] += xi;
                const double ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;  cr = ncr;
            }
        }
    }
    if (inv) { const float s = 1.0f / (float) n; for (int i = 0; i < n; ++i) { re[i] *= s; im[i] *= s; } }
}

// ------------------------------ pitch (YIN) ---------------------------
// Plain autocorrelation is the wrong tool here and both ways of using it
// fail: taking the strongest lag lands on a sub-harmonic (every integer
// multiple of the period correlates just as well), and taking the shortest
// lag above a ratio of the best lands an octave HIGH on voices with a strong
// second harmonic -- that read the seven catalog voices 7-27 % sharp.
// YIN's cumulative mean normalised difference removes the bias by dividing
// each lag by the running mean of all shorter lags, which suppresses the
// zero-lag pull that causes both errors. Threshold and dip-following are the
// standard ones (de Cheveigne & Kawahara 2002).
//
// Getting f0 wrong is not a side issue for this probe: f0 sets the harmonic
// spacing the envelope is built from, so an octave error silently rewrites
// the very quantity being measured.
static double detectF0 (const float* x, int n, double fs, double& clarity)
{
    const int tauMin = std::max (2, (int) (fs / 500.0));
    const int tauMax = std::min (n / 2, (int) (fs / 60.0));
    if (tauMax <= tauMin) { clarity = 0.0; return 0.0; }

    std::vector<double> d ((size_t) tauMax + 1, 0.0), cm ((size_t) tauMax + 1, 1.0);
    for (int tau = 1; tau <= tauMax; ++tau)
    {
        double acc = 0.0;
        for (int i = 0; i + tau < n; ++i)
        {
            const double dif = (double) x[i] - x[i + tau];
            acc += dif * dif;
        }
        d[(size_t) tau] = acc;
    }
    double run = 0.0;
    for (int tau = 1; tau <= tauMax; ++tau)
    {
        run += d[(size_t) tau];
        cm[(size_t) tau] = run > 1e-30 ? d[(size_t) tau] * tau / run : 1.0;
    }

    const double thr = 0.15;
    int tau = -1;
    for (int t = tauMin; t <= tauMax; ++t)
        if (cm[(size_t) t] < thr)
        {
            while (t + 1 <= tauMax && cm[(size_t) (t + 1)] < cm[(size_t) t]) ++t;  // local dip
            tau = t; break;
        }
    if (tau < 0)   // nothing crossed: take the global minimum, low confidence
    {
        double bv = 1e30;
        for (int t = tauMin; t <= tauMax; ++t)
            if (cm[(size_t) t] < bv) { bv = cm[(size_t) t]; tau = t; }
    }
    if (tau <= 0) { clarity = 0.0; return 0.0; }

    // parabolic refinement on the CMNDF dip
    double ref = tau;
    if (tau > 1 && tau < tauMax)
    {
        const double a = cm[(size_t) tau - 1], b = cm[(size_t) tau], c = cm[(size_t) tau + 1];
        const double den = 2.0 * (2.0 * b - a - c);
        if (std::fabs (den) > 1e-30) ref = tau + (c - a) / den;
    }
    clarity = 1.0 - std::min (1.0, cm[(size_t) tau]);
    return ref > 0.0 ? fs / ref : 0.0;
}

// ------------- the engine's envelope stage, copied verbatim -----------
static void engineEnv (const float* grain, int len, double fs, double f0,
                       std::vector<float>& env, int& NBout, int& Nout)
{
    const int N = (len <= 2040) ? 2048 : 4096;
    const int NB = N / 2;
    const int H = (len - 1) / 2;
    std::vector<float> fr ((size_t) N, 0.0f), fi ((size_t) N, 0.0f);
    for (int i = 0; i < len; ++i)
    {
        const float w = 0.5f * (1.0f + std::cos ((float) M_PI * (float) (i - H) / (float) H));
        fr[(size_t) i] = grain[i] * w;
    }
    fftRadix2 (fr.data(), fi.data(), N, false);
    std::vector<float> mag ((size_t) NB + 1, 0.0f);
    for (int k = 0; k <= NB; ++k)
        mag[(size_t) k] = std::sqrt (fr[(size_t) k] * fr[(size_t) k] + fi[(size_t) k] * fi[(size_t) k]);

    const double spacing = std::max (2.5, f0 * N / fs);
    std::vector<int> pkB; std::vector<float> pkV;
    for (double cb = spacing; cb < (double) (NB - 2); cb += spacing)
    {
        const int a = std::max (1, (int) (cb - 0.45 * spacing));
        const int b = std::min (NB - 1, (int) (cb + 0.45 * spacing));
        int bm = a; float vm = mag[(size_t) a];
        for (int t = a + 1; t <= b; ++t) if (mag[(size_t) t] > vm) { vm = mag[(size_t) t]; bm = t; }
        pkB.push_back (bm); pkV.push_back (std::log (vm + 1.0e-12f));
    }
    env.assign ((size_t) NB + 1, 0.0f);
    if (pkB.size() >= 2)
    {
        size_t seg = 0;
        for (int k = 0; k <= NB; ++k)
        {
            float lv;
            if (k <= pkB.front())      lv = pkV.front();
            else if (k >= pkB.back())  lv = pkV.back();
            else
            {
                while (seg + 1 < pkB.size() && pkB[seg + 1] < k) ++seg;
                const int a = pkB[seg], b = pkB[seg + 1];
                const float t = (float) (k - a) / (float) (b - a);
                lv = pkV[seg] * (1.0f - t) + pkV[seg + 1] * t;
            }
            env[(size_t) k] = std::exp (lv);
        }
    }
    else for (int k = 0; k <= NB; ++k) env[(size_t) k] = mag[(size_t) k] + 1.0e-12f;

    std::vector<float> envSm ((size_t) NB + 1, 0.0f);
    std::vector<double> prefix ((size_t) NB + 3, 0.0);
    for (int pass = 0; pass < 2; ++pass)
    {
        prefix[0] = 0.0;
        for (int k = 0; k <= NB; ++k) prefix[(size_t) k + 1] = prefix[(size_t) k] + (double) env[(size_t) k];
        for (int k = 0; k <= NB; ++k)
        {
            const int a = std::max (0, k - 3), b = std::min (NB, k + 3);
            envSm[(size_t) k] = (float) ((prefix[(size_t) b + 1] - prefix[(size_t) a]) / (double) (b - a + 1));
        }
        std::swap (env, envSm);
    }
    NBout = NB; Nout = N;
}

static double medianOf (std::vector<double> v)
{
    if (v.empty()) return 0.0;
    std::sort (v.begin(), v.end());
    return v[v.size() / 2];
}
static double quantile (std::vector<double> v, double q)
{
    if (v.empty()) return 0.0;
    std::sort (v.begin(), v.end());
    return v[std::min (v.size() - 1, (size_t) (q * v.size()))];
}

struct Result
{
    std::string name;
    int    frames = 0;
    double f0 = 0.0;
    double F[3]   = { 0, 0, 0 };
    double def[3] = { 0, 0, 0 };   // median definition, dB
    double iqr[3] = { 0, 0, 0 };   // p75 - p25, dB
    double hit[3] = { 0, 0, 0 };   // fraction of frames that placed the band
};

static Result measure (const char* path, const char* name, double halfHz, bool verbose,
                       double t0 = 0.0, double t1 = 1.0)
{
    Result R; R.name = name;
    std::vector<float> x; double fs = 48000.0;
    if (! readWav (path, x, fs)) { std::fprintf (stderr, "cannot read %s\n", path); return R; }

    // engine's own formant search ranges
    const double loR[3] = { 250, 850, 1900 }, hiR[3] = { 1000, 2600, 3800 };
    std::vector<double> d[3], f0s, Fs[3];
    const int hop = (int) (fs * 0.02);          // 20 ms

    const size_t sFrom = (size_t) (t0 * x.size()), sTo = (size_t) (t1 * x.size());
    for (size_t pos = sFrom; pos + (size_t) (fs * 0.06) < sTo; pos += (size_t) hop)
    {
        double clarity = 0.0;
        const int probeN = (int) (fs * 0.045);
        const double f0 = detectF0 (x.data() + pos, probeN, fs, clarity);
        if (f0 < 60.0 || clarity < 0.80) continue;      // voiced frames only

        const int P = (int) std::lround (fs / f0);
        const int len = 2 * P + 1;
        if (pos + (size_t) len >= x.size()) continue;
        // reject near-silence: the envelope of a quiet frame is noise
        double e = 0.0;
        for (int i = 0; i < len; ++i) e += (double) x[pos + i] * x[pos + i];
        if (std::sqrt (e / len) < 0.005) continue;

        std::vector<float> env; int NB = 0, N = 0;
        engineEnv (x.data() + pos, len, fs, f0, env, NB, N);

        // constant-Hz reference average in the log domain (the DSP's baseEnv)
        std::vector<double> pfx ((size_t) NB + 3, 0.0);
        for (int k = 0; k <= NB; ++k)
            pfx[(size_t) k + 1] = pfx[(size_t) k] + std::log ((double) env[(size_t) k] + 1e-12);
        const int r = std::max (1, (int) std::lround (halfHz * N / fs));

        ++R.frames;
        f0s.push_back (f0);
        for (int i = 0; i < 3; ++i)
        {
            const int lo = std::clamp ((int) std::lround (loR[i] * N / fs), 1, NB - 2);
            const int hi = std::clamp ((int) std::lround (hiR[i] * N / fs), 1, NB - 2);
            // Same rule the engine tracks with, INCLUDING the v0.44.1 fix:
            // start at the first harmonic (below it env[] is extrapolation,
            // not measurement) and require a strict rise on the left so a
            // flat run is not mistaken for a peak. Without this the band
            // reports the plateau under H1 -- that is where the 281 Hz "F1"
            // on a 318 Hz voice came from.
            const int loH = std::max (lo, (int) std::floor (f0 * N / fs));
            int pk = -1; float pv = 0.0f;
            for (int k = loH + 1; k < hi - 1; ++k)
                if (env[(size_t) k] > env[(size_t) k - 1] && env[(size_t) k] >= env[(size_t) k + 1]
                    && env[(size_t) k] > pv) { pv = env[(size_t) k]; pk = k; }
            if (pk < 0) continue;                        // band not placed
            const int a = std::max (0, pk - r), b = std::min (NB, pk + r);
            const double mean = (pfx[(size_t) b + 1] - pfx[(size_t) a]) / (double) (b - a + 1);
            d[i].push_back (20.0 / std::log (10.0) * (std::log ((double) pv + 1e-12) - mean));
            Fs[i].push_back (pk * fs / N);
        }
    }

    R.f0 = medianOf (f0s);
    for (int i = 0; i < 3; ++i)
    {
        R.def[i] = medianOf (d[i]);
        R.iqr[i] = quantile (d[i], 0.75) - quantile (d[i], 0.25);
        R.F[i]   = medianOf (Fs[i]);
        R.hit[i] = R.frames > 0 ? (double) d[i].size() / R.frames : 0.0;
    }
    if (verbose)
        std::printf ("%-8s frames %5d  f0 %6.1f Hz | F %5.0f/%5.0f/%5.0f | "
                     "def %+5.2f/%+5.2f/%+5.2f dB | iqr %4.2f/%4.2f/%4.2f | hit %.2f/%.2f/%.2f\n",
                     R.name.c_str(), R.frames, R.f0, R.F[0], R.F[1], R.F[2],
                     R.def[0], R.def[1], R.def[2], R.iqr[0], R.iqr[1], R.iqr[2],
                     R.hit[0], R.hit[1], R.hit[2]);
    return R;
}

int main (int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf ("usage: definition_probe file.wav [halfHz]\n"
                     "       definition_probe --compare [halfHz=400] a.wav b.wav ...\n");
        return 1;
    }
    if (std::strcmp (argv[1], "--compare") == 0)
    {
        int a = 2; double halfHz = 400.0;
        if (argc > 2 && argv[2][0] >= '0' && argv[2][0] <= '9') { halfHz = std::atof (argv[2]); a = 3; }
        std::printf ("reference half-width %.0f Hz\n", halfHz);
        std::vector<Result> all;
        for (int i = a; i < argc; ++i)
        {
            std::string nm (argv[i]);
            const size_t slash = nm.find_last_of ('/');
            if (slash != std::string::npos) nm = nm.substr (slash + 1);
            const size_t dot = nm.find_last_of ('.');
            if (dot != std::string::npos) nm = nm.substr (0, dot);
            all.push_back (measure (argv[i], nm.c_str(), halfHz, true));
        }
        // spread across voices, and whether the three bands agree
        std::printf ("\n");
        for (int i = 0; i < 3; ++i)
        {
            std::vector<double> v;
            for (auto& R : all) if (R.frames > 0 && R.hit[i] > 0.3) v.push_back (R.def[i]);
            if (v.empty()) continue;
            std::sort (v.begin(), v.end());
            std::printf ("F%d across voices: min %+5.2f  median %+5.2f  max %+5.2f  "
                         "spread %4.2f dB  (n=%d)\n",
                         i + 1, v.front(), medianOf (v), v.back(), v.back() - v.front(), (int) v.size());
        }
        // do the bands move together? correlation over the voices
        for (int i = 0; i < 3; ++i)
            for (int j = i + 1; j < 3; ++j)
            {
                std::vector<double> a2, b2;
                for (auto& R : all)
                    if (R.frames > 0 && R.hit[i] > 0.3 && R.hit[j] > 0.3)
                    { a2.push_back (R.def[i]); b2.push_back (R.def[j]); }
                if (a2.size() < 3) continue;
                double ma = 0, mb = 0;
                for (size_t k = 0; k < a2.size(); ++k) { ma += a2[k]; mb += b2[k]; }
                ma /= a2.size(); mb /= b2.size();
                double num = 0, da = 0, db = 0;
                for (size_t k = 0; k < a2.size(); ++k)
                {
                    num += (a2[k] - ma) * (b2[k] - mb);
                    da  += (a2[k] - ma) * (a2[k] - ma);
                    db  += (b2[k] - mb) * (b2[k] - mb);
                }
                std::printf ("F%d vs F%d correlation across voices: %+.2f\n",
                             i + 1, j + 1, num / (std::sqrt (da * db) + 1e-30));
            }
        return 0;
    }

    if (std::strcmp (argv[1], "--split") == 0)
    {
        // Split-half reliability. The question a Matching feature has to pass
        // is not "do these voices differ" but "does the SAME voice measure the
        // same twice" -- if half of one recording disagrees with its other
        // half by as much as two different speakers do, the number describes
        // the take, not the talker.
        int a = 2; double halfHz = 400.0;
        if (argc > 2 && argv[2][0] >= '0' && argv[2][0] <= '9') { halfHz = std::atof (argv[2]); a = 3; }
        std::printf ("reference half-width %.0f Hz\n", halfHz);
        std::printf ("%-8s %-24s %-24s %s\n", "", "first half", "second half", "|difference|");
        double worst[3] = { 0, 0, 0 };
        for (int i = a; i < argc; ++i)
        {
            std::string nm (argv[i]);
            const size_t slash = nm.find_last_of ('/');
            if (slash != std::string::npos) nm = nm.substr (slash + 1);
            const size_t dot = nm.find_last_of ('.');
            if (dot != std::string::npos) nm = nm.substr (0, dot);
            const Result A = measure (argv[i], nm.c_str(), halfHz, false, 0.0, 0.5);
            const Result B = measure (argv[i], nm.c_str(), halfHz, false, 0.5, 1.0);
            std::printf ("%-8s %+5.2f/%+5.2f/%+5.2f dB      %+5.2f/%+5.2f/%+5.2f dB      "
                         "%4.2f/%4.2f/%4.2f\n", nm.c_str(),
                         A.def[0], A.def[1], A.def[2], B.def[0], B.def[1], B.def[2],
                         std::fabs (A.def[0]-B.def[0]), std::fabs (A.def[1]-B.def[1]),
                         std::fabs (A.def[2]-B.def[2]));
            for (int k = 0; k < 3; ++k)
                if (A.hit[k] > 0.3 && B.hit[k] > 0.3)
                    worst[k] = std::max (worst[k], std::fabs (A.def[k] - B.def[k]));
        }
        std::printf ("\nworst same-voice disagreement: F1 %.2f  F2 %.2f  F3 %.2f dB\n",
                     worst[0], worst[1], worst[2]);
        return 0;
    }

    const double halfHz = argc > 2 ? std::atof (argv[2]) : 400.0;
    measure (argv[1], argv[1], halfHz, true);
    return 0;
}
