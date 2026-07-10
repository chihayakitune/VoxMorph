// VoiceAnalyzer.h — offline voice-profile analysis for the ANALYZE tab.
// Dependency-free C++17 (reuses PsolaEngine's FFT).
//
// Frames of 2048 samples (hop 1024, first 20 s max): YIN-style f0 on a
// 4x-decimated copy, then a zero-padded 4096-point FFT -> smoothed spectral
// envelope -> F1/F2/F3 peaks with levels, plus spectral tilt (texture).
// The profile keeps robust MEDIANS across all voiced frames, so ordinary
// talking (with moving pitch) is fine as analysis input; formant levels are
// stored relative to the strongest formant so recording level cancels out.

#pragma once
#include "PsolaEngine.h"
#include <vector>
#include <algorithm>
#include <cmath>

struct VoiceProfile
{
    float f0Hz       = 0.0f;            // median fundamental
    float f0SpreadSt = 0.0f;            // intonation spread, semitones (robust)
    float F[3] = { 0.0f, 0.0f, 0.0f };  // formant centres, Hz (medians)
    float L[3] = { 0.0f, 0.0f, 0.0f };  // formant levels, dB rel. strongest
    float tiltDb = 0.0f;                // 10*log10(E 0-1k / E 2-8k)
    int   voicedFrames = 0;

    bool valid() const { return voicedFrames >= 15 && f0Hz > 40.0f; }
};

class VoiceAnalyzer
{
public:
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
        std::vector<double> dfn ((size_t) maxLD + 2, 1.0), mag ((size_t) NB + 1), env ((size_t) NB + 1), pre ((size_t) NB + 3);
        std::vector<float>  f0v, Fv[3], Lv[3], tv;

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

            // ---- spectrum -> envelope -> formants
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

            // envelope: moving average about +-0.6 harmonic spacings wide
            const int hw = std::max (2, (int) std::lround (f0 * N / fs * 0.6));
            pre[0] = 0.0;
            for (int k = 0; k <= NB; ++k) pre[(size_t) k + 1] = pre[(size_t) k] + mag[(size_t) k];
            for (int k = 0; k <= NB; ++k)
            {
                const int a = std::max (0, k - hw), b = std::min (NB, k + hw);
                env[(size_t) k] = (pre[(size_t) b + 1] - pre[(size_t) a]) / (double) (b - a + 1);
            }

            auto binOf = [&] (double hz) { return std::clamp ((int) std::lround (hz * N / fs), 1, NB - 1); };
            const double loR[3] = { 250.0, 850.0, 1900.0 }, hiR[3] = { 1000.0, 2600.0, 3800.0 };
            float Fi[3], Li[3];
            for (int fi = 0; fi < 3; ++fi)
            {
                const int a = binOf (loR[fi]), b = binOf (std::min (hiR[fi], fs * 0.45));
                int pk = a; double pv = env[(size_t) a];
                for (int k = a + 1; k <= b; ++k)
                    if (env[(size_t) k] > pv) { pv = env[(size_t) k]; pk = k; }
                Fi[fi] = (float) (pk * fs / N);
                Li[fi] = (float) (10.0 * std::log10 (pv + 1.0e-20));
            }
            double eLo = 0.0, eHi = 0.0;
            for (int k = binOf (60.0);   k <= binOf (1000.0); ++k) eLo += mag[(size_t) k];
            for (int k = binOf (2000.0); k <= binOf (std::min (8000.0, fs * 0.45)); ++k) eHi += mag[(size_t) k];

            f0v.push_back (f0);
            for (int fi = 0; fi < 3; ++fi) { Fv[fi].push_back (Fi[fi]); Lv[fi].push_back (Li[fi]); }
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
        for (int fi = 0; fi < 3; ++fi) { out.F[fi] = median (Fv[fi]); Lmed[fi] = median (Lv[fi]); }
        const float Lmax = std::max ({ Lmed[0], Lmed[1], Lmed[2] });
        for (int fi = 0; fi < 3; ++fi) out.L[fi] = Lmed[fi] - Lmax;
        out.tiltDb = median (tv);
        return out;
    }

private:
    static float median (std::vector<float> v)
    {
        if (v.empty()) return 0.0f;
        const size_t m = v.size() / 2;
        std::nth_element (v.begin(), v.begin() + (long) m, v.end());
        return v[m];
    }
};
