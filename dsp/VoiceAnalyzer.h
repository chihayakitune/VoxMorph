// VoiceAnalyzer.h — offline voice-profile analysis for the Matching tab.
// Dependency-free C++17 (reuses PsolaEngine's FFT).
//
// Frames of 2048 samples (hop 1024, first 20 s max): YIN-style f0 on a
// 4x-decimated copy, then a zero-padded 4096-point FFT -> TRUE (iterative
// cepstral) spectral envelope -> F1/F2/F3 chosen by candidate assignment,
// plus spectral tilt (texture). The profile keeps robust MEDIANS across all
// voiced frames, so ordinary talking (with moving pitch) is fine as analysis
// input; formant levels are stored relative to the strongest formant so
// recording level cancels out.
//
// v0.29.0 rewrite of the estimator, and per-vowel (A I U E O) sub-profiles.
// Why the previous design had to go (measured on synthetic vowels with KNOWN
// formants, see test/offline_test.cpp "analyzer accuracy"):
//
//   * The old envelope was a moving average +-0.8 harmonic spacings wide.
//     A box average leaves ~19 % of the harmonic ripple (sinc(1.6)), so on
//     high-pitched voices its "local maxima" are harmonics, not formants.
//     Measured against ground truth at f0 = 352 Hz it was off by 3.3 st on
//     F1 and 4.8 st on F2. The true envelope (cepstral liftering iterated
//     against the original spectrum) is built to pass through the harmonic
//     PEAKS instead of averaging across them: same test, 1.4 st and 2.4 st.
//   * The old search used three fixed, non-overlapping windows
//     (250-1000 / 850-2600 / 1900-3800 Hz). Real vowels cross those edges --
//     /u/ and /o/ have F2 near 800 Hz, below the F2 floor -- so the answer
//     was pinned to a band edge. Candidates are now assigned jointly with
//     ordering constraints and a soft range prior, so no formant can be
//     forced onto an edge.
//
// The other half of the rewrite is RELIABILITY. A formant sitting at or
// below the fundamental leaves no trace in the spectrum: at f0 = 352 Hz the
// /i/ and /u/ F1 (285-350 Hz) is simply not measurable, and every estimator
// tested "recovers" it with a large positive bias, because the lowest thing
// it can find is the fundamental itself. Rather than emit a confident-looking
// number, each band now carries rel[] in 0..1 derived from F/f0, and the
// Matching stage weights by it. This is a property of the SIGNAL, computed
// before the value is known -- not a guard that inspects an output and
// decides it looks wrong (two of those have already been tried and withdrawn;
// they misfire on voices whose formants genuinely are unusual).
#pragma once
#include "PsolaEngine.h"
#include <vector>
#include <algorithm>
#include <cmath>

// Per-vowel sub-profile (A, I, U, E, O — same order as
// AEIOUCharacterMap / VowelAdaptiveWarp anchors).
struct VowelProfile
{
    int   frames = 0;
    float f0Hz   = 0.0f;
    float F[3]   = { 0.0f, 0.0f, 0.0f };   // formant centres, Hz (medians)
    float L[3]   = { 0.0f, 0.0f, 0.0f };   // levels, dB rel. this vowel's strongest
    float rel[3] = { 0.0f, 0.0f, 0.0f };   // measurement reliability, 0..1

    static constexpr int kMinFrames = 8;
    bool valid() const { return frames >= kMinFrames; }
};

struct VoiceProfile
{
    float f0Hz       = 0.0f;            // median fundamental
    float f0SpreadSt = 0.0f;            // intonation spread, semitones (robust)
    float F[3] = { 0.0f, 0.0f, 0.0f };  // formant centres, Hz (medians)
    float L[3] = { 0.0f, 0.0f, 0.0f };  // formant levels, dB rel. strongest
    float tiltDb = 0.0f;                // 10*log10(E 0-1k / E 2-8k)
    int   voicedFrames = 0;
    float rel[3] = { 0.0f, 0.0f, 0.0f };  // per-band reliability, 0..1 (v0.29.0)
    VowelProfile vow[5];                  // per-vowel sub-profiles (v0.29.0)

    bool valid() const { return voicedFrames >= 15 && f0Hz > 40.0f; }

    // how many vowels carry enough frames to be matched individually
    int vowelsMeasured() const
    {
        int k = 0;
        for (int v = 0; v < 5; ++v) if (vow[v].valid()) ++k;
        return k;
    }
};

class VoiceAnalyzer
{
public:
    // ---- envelope / assignment constants (validated against ground truth) --
    static constexpr float kCepDiv   = 1.6f;  // cepstral order = fs / (kCepDiv*f0)
    static constexpr int   kCepIters = 8;     // true-envelope iterations
    static constexpr float kPreEmphDb = 6.0f; // dB per octave, above kPreEmphF0
    static constexpr float kPreEmphF0 = 100.0f;

    // A formant is only measurable when enough harmonics fall around it.
    // Measured on synthetic vowels with known formants (offline_test
    // "formant accuracy"): the median error over all bands is 0.23 st, but
    // every outlier above 1.5 st is an F1 whose F/f0 ratio sits between 2.6
    // and 3.3 -- close enough to the fundamental that the envelope peak
    // snaps onto a harmonic (at f0 300 an /a/ F1 of 775 Hz reads back as
    // 552 Hz, i.e. the second harmonic). The ramp therefore only starts
    // trusting a band at 2.5x f0 and reaches full trust at 4x.
    static constexpr float kRelLo = 2.5f, kRelHi = 4.0f;

    static float reliability (float F, float f0)
    {
        if (! (f0 > 0.0f) || ! std::isfinite (F)) return 0.0f;
        return std::clamp ((F / f0 - kRelLo) / (kRelHi - kRelLo), 0.0f, 1.0f);
    }

    static VoiceProfile analyze (const float* x, int n, double fs)
    {
        VoiceProfile out;
        constexpr int W = 2048, hop = 1024, N = 4096, NB = N / 2;
        n = std::min (n, (int) (fs * 20.0));
        const int maxLag = (int) (fs / 60.0), minLag = std::max (2, (int) (fs / 500.0));
        if (n < W + maxLag + 8) return out;

        const int WD = W / 4, minLD = std::max (2, minLag / 4), maxLD = maxLag / 4;
        std::vector<float>  dec ((size_t) (WD + maxLD + 4), 0.0f);
        std::vector<float>  re ((size_t) N), im ((size_t) N);
        std::vector<double> dfn ((size_t) maxLD + 2, 1.0), mag ((size_t) NB + 1);
        std::vector<float>  A ((size_t) N), V ((size_t) N), Acur ((size_t) N);
        std::vector<float>  f0v, Fv[3], Lv[3], Rv[3], tv;
        std::vector<int>    vv;                 // per-frame vowel class

        for (int s = 0; s + W + maxLag < n; s += hop)
        {
            double e0 = 0.0;
            for (int i = 0; i < W; ++i) e0 += (double) x[s + i] * x[s + i];
            if (e0 / W < 1.0e-6) continue;                      // silence

            // ---- f0: cumulative-mean-normalized difference, 4x decimated
            for (int i = 0; i < WD + maxLD; ++i)
            {
                const int j = s + 4 * i;
                dec[(size_t) i] = 0.25f * (x[j] + x[j+1] + x[j+2] + x[j+3]);
            }
            double cum = 0.0;
            for (int lag = 1; lag <= maxLD; ++lag)
            {
                double ss = 0.0;
                for (int i = 0; i < WD; ++i)
                {
                    const double d = (double) dec[(size_t) i] - dec[(size_t) (i + lag)];
                    ss += d * d;
                }
                cum += ss;
                dfn[(size_t) lag] = cum > 0.0 ? ss * lag / cum : 1.0;
            }
            int lag = -1; double bestV = 1.0e18; int best = -1;
            for (int t = minLD; t <= maxLD; ++t)
                if (dfn[(size_t) t] < bestV) { bestV = dfn[(size_t) t]; best = t; }
            for (int t = minLD + 1; t < maxLD; ++t)
                if (dfn[(size_t) t] < 0.2 && dfn[(size_t) t] <= dfn[(size_t) t-1]
                                          && dfn[(size_t) t] <= dfn[(size_t) t+1]) { lag = t; break; }
            if (lag < 0 && bestV < 0.3) lag = best;
            if (lag < 0) continue;                              // unvoiced frame

            double lagf = lag;
            if (lag > minLD && lag < maxLD)                     // parabolic refine
            {
                const double y0 = dfn[(size_t) lag-1], y1 = dfn[(size_t) lag], y2 = dfn[(size_t) lag+1];
                const double den = y0 - 2.0 * y1 + y2;
                if (std::abs (den) > 1.0e-12) lagf += 0.5 * (y0 - y2) / den;
            }
            const float f0 = (float) (fs / (4.0 * lagf));
            if (f0 < 60.0f || f0 > 500.0f) continue;

            // ---- spectrum
            for (int i = 0; i < N; ++i) { re[(size_t) i] = 0.0f; im[(size_t) i] = 0.0f; }
            for (int i = 0; i < W; ++i)
            {
                const float w = 0.5f - 0.5f * std::cos (2.0f * (float) M_PI * (float) i / (float) (W - 1));
                re[(size_t) i] = x[s + i] * w;
            }
            PsolaEngine::fftForViz (re.data(), im.data(), N);
            for (int k = 0; k <= NB; ++k)
                mag[(size_t) k] = (double) re[(size_t) k] * re[(size_t) k]
                                + (double) im[(size_t) k] * im[(size_t) k];

            // ---- log spectrum with a +6 dB/oct tilt correction, mirrored to
            // a real EVEN sequence so the forward FFT serves both directions
            // (for real-even data the inverse DFT is the forward DFT / N).
            for (int k = 0; k <= NB; ++k)
            {
                const double hz = (double) k * fs / N;
                const double lift = kPreEmphDb * std::log2 (std::max (hz, (double) kPreEmphF0)
                                                           / (double) kPreEmphF0);
                A[(size_t) k] = (float) (10.0 * std::log10 (mag[(size_t) k] + 1.0e-20) + lift);
            }
            for (int k = NB + 1; k < N; ++k) A[(size_t) k] = A[(size_t) (N - k)];

            trueEnvelope (A.data(), Acur.data(), V.data(), re.data(), im.data(), N, f0, fs);

            // ---- formant candidates -> joint assignment
            float Fi[3], Li[3];
            if (! assignFormants (V.data(), N, NB, fs, Fi, Li)) continue;

            double eLo = 0.0, eHi = 0.0;
            auto binOf = [&] (double hz) { return std::clamp ((int) std::lround (hz * N / fs), 1, NB - 1); };
            for (int k = binOf (60.0);   k <= binOf (1000.0); ++k) eLo += mag[(size_t) k];
            for (int k = binOf (2000.0); k <= binOf (std::min (8000.0, fs * 0.45)); ++k) eHi += mag[(size_t) k];

            f0v.push_back (f0);
            for (int fi = 0; fi < 3; ++fi)
            {
                Fv[fi].push_back (Fi[fi]);
                Lv[fi].push_back (Li[fi]);
                Rv[fi].push_back (reliability (Fi[fi], f0));
            }
            tv.push_back ((float) (10.0 * std::log10 ((eLo + 1.0e-20) / (eHi + 1.0e-20))));
        }

        out.voicedFrames = (int) f0v.size();
        if (out.voicedFrames < 5) return out;

        out.f0Hz = median (f0v);
        // intonation spread: half the p10..p90 width in semitones. (A plain
        // MAD collapses to ~0 on bimodal pitch material, so percentiles.)
        {
            std::vector<float> st;
            st.reserve (f0v.size());
            for (float f : f0v) st.push_back (12.0f * std::log2 (f / out.f0Hz));
            std::sort (st.begin(), st.end());
            const size_t lo = st.size() / 10, hi = st.size() - 1 - st.size() / 10;
            out.f0SpreadSt = 0.5f * (st[hi] - st[lo]);
        }

        float Lmed[3];
        for (int fi = 0; fi < 3; ++fi)
        {
            out.F[fi]   = median (Fv[fi]);
            Lmed[fi]    = median (Lv[fi]);
            out.rel[fi] = median (Rv[fi]);
        }
        const float Lmax = std::max ({ Lmed[0], Lmed[1], Lmed[2] });
        for (int fi = 0; fi < 3; ++fi) out.L[fi] = Lmed[fi] - Lmax;
        out.tiltDb = median (tv);

        classifyVowels (Fv, vv);
        buildVowelProfiles (out, f0v, Fv, Lv, Rv, vv);
        return out;
    }

    // ---- vowel coordinate, shared with VowelAdaptiveWarp -------------------
    // height ~ openness (log F1), frontness ~ log(F2/F1).
    static void vowelCoord (float f1, float f2, float& h, float& fr)
    {
        static const float kLoF1 = std::log2 (280.0f), kHiF1 = std::log2 (950.0f);
        constexpr float kLoRat = 0.55f, kHiRat = 2.90f;
        const float l1 = std::log2 (std::max (f1, 1.0f));
        const float l2 = std::log2 (std::max (f2, 1.0f));
        h  = std::clamp ((l1 - kLoF1) / (kHiF1 - kLoF1), 0.0f, 1.0f);
        fr = std::clamp (((l2 - l1) - kLoRat) / (kHiRat - kLoRat), 0.0f, 1.0f);
    }

private:
    // Anchor coordinates, order A, I, U, E, O — the same table and the same
    // order VowelAdaptiveWarp uses (typical Japanese vowel F1/F2:
    // a 800/1250, i 300/2350, u 350/1300, e 480/2100, o 500/900 Hz), so a
    // residual measured here can be written straight into the AEIOU map.
    static constexpr float kAnchorH[5] = { 0.86f, 0.06f, 0.18f, 0.44f, 0.47f };
    static constexpr float kAnchorF[5] = { 0.04f, 1.00f, 0.57f, 0.67f, 0.13f };

    // Iterative cepstral "true envelope": lifter, then clamp the result back
    // up to the original spectrum and repeat, so the envelope converges onto
    // the harmonic peaks instead of the mean of peak and valley.
    static void trueEnvelope (const float* A, float* Acur, float* V,
                              float* re, float* im, int N, float f0, double fs)
    {
        const int K = std::max (4, (int) (fs / (kCepDiv * f0)));
        for (int i = 0; i < N; ++i) Acur[i] = A[i];
        for (int it = 0; it < kCepIters; ++it)
        {
            for (int i = 0; i < N; ++i) { re[i] = Acur[i]; im[i] = 0.0f; }
            PsolaEngine::fftForViz (re, im, N);          // == inverse * N
            const float inv = 1.0f / (float) N;
            for (int i = 0; i < N; ++i) re[i] *= inv;
            for (int i = K + 1; i < N - K; ++i) re[i] = 0.0f;   // lifter
            for (int i = 0; i < N; ++i) im[i] = 0.0f;
            PsolaEngine::fftForViz (re, im, N);          // back to log-spectrum
            for (int i = 0; i < N; ++i)
            {
                V[i] = std::isfinite (re[i]) ? re[i] : A[i];
                Acur[i] = std::max (A[i], V[i]);
            }
        }
    }

    // Joint F1/F2/F3 choice. Candidates are the envelope's local maxima;
    // the winner maximizes peak prominence minus a soft log-distance prior,
    // subject to ordering. Overlapping ranges are intentional: /u/ and /o/
    // put F2 near 800 Hz while /a/ puts F1 there, and only the ordering
    // constraint can separate those two readings.
    static bool assignFormants (const float* V, int N, int NB, double fs,
                                float* Fout, float* Lout)
    {
        constexpr float LO[3]  = { 200.0f,  550.0f, 1800.0f };
        constexpr float HI[3]  = { 1100.0f, 3300.0f, 4500.0f };
        constexpr float REF[3] = { 500.0f,  1500.0f, 2600.0f };
        constexpr float WG[3]  = { 1.4f,    1.0f,    0.8f    };
        constexpr float kProm  = 0.35f;
        constexpr int   kMaxC  = 12;

        const int kl = std::max (1, (int) (150.0 * N / fs));
        const int kh = std::min (NB - 1, (int) (4700.0 * N / fs));

        float cf[3][kMaxC], cl[3][kMaxC];
        int   nc[3] = { 0, 0, 0 };
        float lmax = -1.0e30f;

        for (int k = kl; k <= kh; ++k)
        {
            if (! (V[k] >= V[k-1] && V[k] >= V[k+1])) continue;
            const float d = V[k-1] - 2.0f * V[k] + V[k+1];
            float dk = std::abs (d) > 1.0e-12f ? 0.5f * (V[k-1] - V[k+1]) / d : 0.0f;
            dk = std::clamp (dk, -1.0f, 1.0f);
            const float hz = (float) ((k + dk) * fs / N);
            if (V[k] > lmax) lmax = V[k];
            for (int b = 0; b < 3; ++b)
                if (hz >= LO[b] && hz <= HI[b] && nc[b] < kMaxC)
                { cf[b][nc[b]] = hz; cl[b][nc[b]] = V[k]; ++nc[b]; }
        }
        if (nc[0] == 0 || nc[1] == 0 || nc[2] == 0) return false;

        float bestSc = -1.0e30f; int bi = -1, bj = -1, bk2 = -1;
        for (int i = 0; i < nc[0]; ++i)
            for (int j = 0; j < nc[1]; ++j)
            {
                if (cf[1][j] < 1.15f * cf[0][i]) continue;
                for (int k = 0; k < nc[2]; ++k)
                {
                    if (cf[2][k] < 1.10f * cf[1][j]) continue;
                    const float f[3] = { cf[0][i], cf[1][j], cf[2][k] };
                    const float l[3] = { cl[0][i], cl[1][j], cl[2][k] };
                    float sc = 0.0f;
                    for (int b = 0; b < 3; ++b)
                    {
                        const float lg = std::log2 (f[b] / REF[b]);
                        sc += kProm * (l[b] - lmax) / 6.0f - WG[b] * lg * lg;
                    }
                    if (sc > bestSc) { bestSc = sc; bi = i; bj = j; bk2 = k; }
                }
            }
        if (bi < 0) return false;
        Fout[0] = cf[0][bi];  Lout[0] = cl[0][bi];
        Fout[1] = cf[1][bj];  Lout[1] = cl[1][bj];
        Fout[2] = cf[2][bk2]; Lout[2] = cl[2][bk2];
        return true;
    }

    // Speaker-normalized vowel classification. Each recording is first
    // recentred on its OWN median log-F1/F2 and mapped onto a reference
    // geometry, so a small and a large speaker saying the same vowel land on
    // the same anchor. Without this the target's whole vowel space shifts and
    // every frame classifies one anchor over.
    static void classifyVowels (const std::vector<float>* Fv, std::vector<int>& vv)
    {
        const size_t n = Fv[0].size();
        vv.assign (n, -1);
        if (n == 0) return;
        std::vector<float> l1, l2;
        l1.reserve (n); l2.reserve (n);
        for (size_t i = 0; i < n; ++i)
        {
            l1.push_back (std::log2 (std::max (Fv[0][i], 1.0f)));
            l2.push_back (std::log2 (std::max (Fv[1][i], 1.0f)));
        }
        const float m1 = median (l1), m2 = median (l2);
        static const float R1 = std::log2 (480.0f), R2 = std::log2 (1450.0f);
        for (size_t i = 0; i < n; ++i)
        {
            const float f1n = std::exp2 (l1[i] - m1 + R1);
            const float f2n = std::exp2 (l2[i] - m2 + R2);
            float h, fr;
            vowelCoord (f1n, f2n, h, fr);
            int bestA = 0; float bestD = 1.0e30f;
            for (int a = 0; a < 5; ++a)
            {
                const float dh = h - kAnchorH[a], df = fr - kAnchorF[a];
                const float d = dh * dh + df * df;
                if (d < bestD) { bestD = d; bestA = a; }
            }
            vv[i] = bestA;
        }
    }

    static void buildVowelProfiles (VoiceProfile& out,
                                    const std::vector<float>& f0v,
                                    const std::vector<float>* Fv,
                                    const std::vector<float>* Lv,
                                    const std::vector<float>* Rv,
                                    const std::vector<int>& vv)
    {
        for (int a = 0; a < 5; ++a)
        {
            std::vector<float> g0, gF[3], gL[3], gR[3];
            for (size_t i = 0; i < vv.size(); ++i)
            {
                if (vv[i] != a) continue;
                g0.push_back (f0v[i]);
                for (int b = 0; b < 3; ++b)
                {
                    gF[b].push_back (Fv[b][i]);
                    gL[b].push_back (Lv[b][i]);
                    gR[b].push_back (Rv[b][i]);
                }
            }
            auto& vp = out.vow[a];
            vp.frames = (int) g0.size();
            // frame count is kept either way (the UI shows coverage); the
            // statistics are only filled in once there are enough frames.
            if (! vp.valid()) continue;
            vp.f0Hz = median (g0);
            float lm[3];
            for (int b = 0; b < 3; ++b)
            {
                vp.F[b]   = median (gF[b]);
                lm[b]     = median (gL[b]);
                vp.rel[b] = median (gR[b]);
            }
            const float mx = std::max ({ lm[0], lm[1], lm[2] });
            for (int b = 0; b < 3; ++b) vp.L[b] = lm[b] - mx;
        }
    }

    static float median (std::vector<float> v)
    {
        if (v.empty()) return 0.0f;
        const size_t m = v.size() / 2;
        std::nth_element (v.begin(), v.begin() + (long) m, v.end());
        return v[m];
    }
};
