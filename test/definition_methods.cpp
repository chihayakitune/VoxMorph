// definition_methods.cpp — can Formant Definition be measured in a way that
// does NOT move with pitch?
//
//   c++ -std=c++17 -O2 -o definition_methods test/definition_methods.cpp
//
// Phase 2.5 established that the shipped measure (contrast of the
// harmonic-peak envelope against a broad reference) swings ~4 dB purely from
// f0 with the vocal tract held fixed, which is several times the difference
// between real voices -- so Matching on it would chase pitch. This asks
// whether another estimator escapes that.
//
// The test is two-axis, on synthetic vowels where the answer is known:
//
//   axis B (bandwidth): f0 fixed, resonator bandwidths 60 / 110 / 200 Hz.
//                       This is the REAL quantity -- a narrow resonance is
//                       a sharp formant. A useful estimator must respond.
//   axis A (pitch):     bandwidth fixed, f0 100..400 Hz. The tract does not
//                       change, so an ideal estimator does not move.
//
// Figure of merit = (response to bandwidth) / (response to pitch). Above ~2
// the estimator is measuring the voice; near or below 1 it is measuring the
// harmonic comb wearing the voice's clothes.
#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>

static const double FS = 48000.0;

struct Reso
{
    double b0=1,a1=0,a2=0,z1=0,z2=0;
    Reso (double f, double bw)
    {
        const double r = std::exp (-M_PI * bw / FS);
        a1 = -2.0 * r * std::cos (2.0 * M_PI * f / FS);
        a2 = r * r;
        b0 = (1.0 - r) * std::sqrt (1.0 + r*r - 2.0*r*std::cos (4.0*M_PI*f/FS));
    }
    double tick (double x) { const double y = b0*x - a1*z1 - a2*z2; z2 = z1; z1 = y; return y; }
};

static std::vector<float> vowel (double f0, const double F[3], double bwScale, double sec)
{
    const int n = (int) (FS * sec);
    std::vector<double> src ((size_t) n, 0.0);
    double ph = 0.0;
    for (int i = 0; i < n; ++i) { ph += f0/FS; if (ph >= 1.0) { ph -= 1.0; src[(size_t)i] = 1.0; } }
    Reso r1 (F[0], 110.0*bwScale), r2 (F[1], 120.0*bwScale), r3 (F[2], 160.0*bwScale);
    std::vector<double> y ((size_t) n, 0.0); double mx = 1e-12;
    for (int i = 0; i < n; ++i)
    {
        const double s = r1.tick (src[(size_t)i]) + 0.7*r2.tick (src[(size_t)i]) + 0.35*r3.tick (src[(size_t)i]);
        y[(size_t)i] = s; mx = std::max (mx, std::fabs (s));
    }
    std::vector<float> v ((size_t) n, 0.0f);
    for (int i = 0; i < n; ++i) v[(size_t)i] = (float)(0.7*y[(size_t)i]/mx);
    return v;
}

static void fft (float* re, float* im, int n, bool inv)
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
            for (int k = 0; k < half/2; ++k)
            {
                const int a = i+k, b = i+k+half/2;
                const float xr = (float)(re[b]*cr - im[b]*ci);
                const float xi = (float)(re[b]*ci + im[b]*cr);
                re[b] = re[a]-xr; im[b] = im[a]-xi;
                re[a] += xr;      im[a] += xi;
                const double ncr = cr*wr - ci*wi;
                ci = cr*wi + ci*wr; cr = ncr;
            }
        }
    }
    if (inv) { const float s = 1.0f/(float)n; for (int i = 0; i < n; ++i) { re[i]*=s; im[i]*=s; } }
}

// ---------- method A: the shipped one (harmonic-peak envelope) ----------
static void envHarmonic (const float* g, int len, double f0, std::vector<double>& lg, int& N)
{
    N = (len <= 2040) ? 2048 : 4096;
    const int NB = N/2, H = (len-1)/2;
    std::vector<float> fr ((size_t)N, 0.0f), fi ((size_t)N, 0.0f);
    for (int i = 0; i < len; ++i)
        fr[(size_t)i] = g[i] * 0.5f * (1.0f + std::cos ((float)M_PI * (float)(i-H) / (float)H));
    fft (fr.data(), fi.data(), N, false);
    std::vector<float> mag ((size_t)NB+1);
    for (int k = 0; k <= NB; ++k) mag[(size_t)k] = std::sqrt (fr[(size_t)k]*fr[(size_t)k] + fi[(size_t)k]*fi[(size_t)k]);
    const double sp = std::max (2.5, f0*N/FS);
    std::vector<int> pb; std::vector<float> pv;
    for (double cb = sp; cb < (double)(NB-2); cb += sp)
    {
        const int a = std::max (1,(int)(cb-0.45*sp)), b = std::min (NB-1,(int)(cb+0.45*sp));
        int bm = a; float vm = mag[(size_t)a];
        for (int t = a+1; t <= b; ++t) if (mag[(size_t)t] > vm) { vm = mag[(size_t)t]; bm = t; }
        pb.push_back (bm); pv.push_back (std::log (vm + 1e-12f));
    }
    std::vector<float> env ((size_t)NB+1, 0.0f);
    if (pb.size() >= 2)
    {
        size_t seg = 0;
        for (int k = 0; k <= NB; ++k)
        {
            float lv;
            if (k <= pb.front()) lv = pv.front();
            else if (k >= pb.back()) lv = pv.back();
            else { while (seg+1 < pb.size() && pb[seg+1] < k) ++seg;
                   const int a = pb[seg], b = pb[seg+1];
                   const float t = (float)(k-a)/(float)(b-a);
                   lv = pv[seg]*(1.0f-t) + pv[seg+1]*t; }
            env[(size_t)k] = std::exp (lv);
        }
    }
    std::vector<float> sm ((size_t)NB+1, 0.0f);
    std::vector<double> pfx ((size_t)NB+3, 0.0);
    for (int pass = 0; pass < 2; ++pass)
    {
        pfx[0] = 0.0;
        for (int k = 0; k <= NB; ++k) pfx[(size_t)k+1] = pfx[(size_t)k] + env[(size_t)k];
        for (int k = 0; k <= NB; ++k)
        { const int a = std::max (0,k-3), b = std::min (NB,k+3);
          sm[(size_t)k] = (float)((pfx[(size_t)b+1]-pfx[(size_t)a])/(double)(b-a+1)); }
        std::swap (env, sm);
    }
    lg.assign ((size_t)NB+1, 0.0);
    for (int k = 0; k <= NB; ++k) lg[(size_t)k] = std::log ((double)env[(size_t)k] + 1e-12);
}

// ---------- method B: all-pole (LPC) spectrum ----------
// Fits poles to the whole spectrum instead of joining harmonic tips, so it is
// not built out of interpolation between sample points that get sparser as
// the voice rises. Pre-emphasis and a Hamming window are the standard
// formant-analysis front end.
static void envLpc (const float* g, int len, int order, std::vector<double>& lg, int& N)
{
    std::vector<double> x ((size_t) len);
    for (int i = 0; i < len; ++i) x[(size_t)i] = g[i] - (i ? 0.97*g[i-1] : 0.0);
    for (int i = 0; i < len; ++i)
        x[(size_t)i] *= 0.54 - 0.46*std::cos (2.0*M_PI*i/(len-1));

    std::vector<double> r ((size_t) order+1, 0.0);
    for (int k = 0; k <= order; ++k)
    { double s = 0.0; for (int i = k; i < len; ++i) s += x[(size_t)i]*x[(size_t)(i-k)]; r[(size_t)k] = s; }
    if (r[0] < 1e-30) { lg.assign (1025, 0.0); N = 2048; return; }

    // Levinson-Durbin
    std::vector<double> a ((size_t) order+1, 0.0), tmp ((size_t) order+1, 0.0);
    double e = r[0];
    a[0] = 1.0;
    for (int i = 1; i <= order; ++i)
    {
        double acc = r[(size_t)i];
        for (int j = 1; j < i; ++j) acc -= a[(size_t)j]*r[(size_t)(i-j)];
        const double k = e > 1e-30 ? acc/e : 0.0;
        tmp = a;
        for (int j = 1; j < i; ++j) a[(size_t)j] = tmp[(size_t)j] - k*tmp[(size_t)(i-j)];
        a[(size_t)i] = k;
        e *= (1.0 - k*k);
        if (e < 1e-30) break;
    }
    // |H(w)| = sqrt(e) / |A(w)|
    N = 2048;
    const int NB = N/2;
    std::vector<float> ar ((size_t)N, 0.0f), ai ((size_t)N, 0.0f);
    ar[0] = 1.0f;
    for (int i = 1; i <= order; ++i) ar[(size_t)i] = (float)(-a[(size_t)i]);
    fft (ar.data(), ai.data(), N, false);
    lg.assign ((size_t)NB+1, 0.0);
    for (int k = 0; k <= NB; ++k)
    {
        const double m = std::sqrt ((double)ar[(size_t)k]*ar[(size_t)k] + (double)ai[(size_t)k]*ai[(size_t)k]);
        lg[(size_t)k] = 0.5*std::log (std::max (e,1e-30)) - std::log (std::max (m,1e-12));
    }
}

// contrast of a log envelope at the three formants, vs a 400 Hz box average
static void contrast (const std::vector<double>& lg, int N, double f0, const double F[3], double out[3])
{
    const int NB = (int) lg.size() - 1;
    std::vector<double> pfx ((size_t)NB+3, 0.0);
    for (int k = 0; k <= NB; ++k) pfx[(size_t)k+1] = pfx[(size_t)k] + lg[(size_t)k];
    const int r = std::max (1, (int) std::lround (400.0*N/FS));
    const int firstH = (int) std::floor (f0*N/FS);
    for (int i = 0; i < 3; ++i)
    {
        // search near the KNOWN formant so the comparison is about contrast,
        // not about which peak each method happened to find
        const int c  = (int) std::lround (F[i]*N/FS);
        const int lo = std::max (firstH+1, c - (int)std::lround (250.0*N/FS));
        const int hi = std::min (NB-2,     c + (int)std::lround (250.0*N/FS));
        int pk = lo; double pv = -1e30;
        for (int k = lo; k <= hi; ++k) if (lg[(size_t)k] > pv) { pv = lg[(size_t)k]; pk = k; }
        const int a = std::max (0, pk-r), b = std::min (NB, pk+r);
        const double mean = (pfx[(size_t)b+1]-pfx[(size_t)a])/(double)(b-a+1);
        out[i] = 20.0/std::log (10.0) * (pv - mean);
    }
}

int main()
{
    const double F[3] = { 730.0, 1090.0, 2440.0 };
    const double f0s[] = { 100, 150, 200, 260, 320, 400 };
    const double bws[] = { 0.55, 1.0, 1.8 };      // narrow / normal / wide

    // LPC gets a FIXED 30 ms window. Giving it two pitch periods like the
    // shipped estimator would vary the analysis length by 4x across the f0
    // axis, and a high order on a short frame starts fitting the harmonics
    // themselves -- that is a property of the test, not of the method.
    struct M { const char* name; int lpcOrder; };
    const M methods[] = { { "harmonic-peak (shipped)", 0 },
                          { "all-pole / LPC order 24, 30 ms", 24 },
                          { "all-pole / LPC order 48, 30 ms", 48 } };

    for (const auto& m : methods)
    {
        std::printf ("\n===== %s =====\n", m.name);
        std::printf ("            ");
        for (double b : bws) std::printf ("  bw x%.2f ", b);
        std::printf ("\n");
        // rows = f0, cols = bandwidth; F3 reported (the best-observed band)
        std::vector<std::vector<double>> grid;
        for (double f0 : f0s)
        {
            std::printf ("  f0 %4.0f   ", f0);
            std::vector<double> row;
            for (double b : bws)
            {
                auto v = vowel (f0, F, b, 1.0);
                const int P = (int) std::lround (FS/f0);
                const int len = 2*P+1;
                std::vector<double> acc (3, 0.0); int cnt = 0;
                for (int t = 0; t < 8; ++t)
                {
                    const size_t pos = (size_t)(FS*0.3) + (size_t)t*(size_t)P*3;
                    if (pos + (size_t)std::max (len, (int)(FS*0.030)) >= v.size()) break;
                    std::vector<double> lg; int N = 0;
                    if (m.lpcOrder) envLpc (v.data()+pos, (int)(FS*0.030), m.lpcOrder, lg, N);
                    else            envHarmonic (v.data()+pos, len, f0, lg, N);
                    double d[3]; contrast (lg, N, f0, F, d);
                    for (int i = 0; i < 3; ++i) acc[(size_t)i] += d[i];
                    ++cnt;
                }
                const double v3 = cnt ? acc[2]/cnt : 0.0;
                row.push_back (v3);
                std::printf ("  %7.2f ", v3);
            }
            grid.push_back (row);
            std::printf ("\n");
        }
        // figure of merit
        double bwResp = 0.0;   // narrow - wide, averaged over f0
        for (auto& row : grid) bwResp += row.front() - row.back();
        bwResp /= grid.size();
        double f0Resp = 0.0;   // spread over f0 at fixed bandwidth, averaged
        for (size_t c = 0; c < 3; ++c)
        {
            double lo = 1e30, hi = -1e30;
            for (auto& row : grid) { lo = std::min (lo, row[c]); hi = std::max (hi, row[c]); }
            f0Resp += hi - lo;
        }
        f0Resp /= 3.0;
        std::printf ("  response to BANDWIDTH (narrow-wide) : %5.2f dB\n", bwResp);
        std::printf ("  response to PITCH     (spread f0)   : %5.2f dB\n", f0Resp);
        std::printf ("  figure of merit                     : %5.2f\n", bwResp/(f0Resp+1e-9));
    }
    return 0;
}
