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
// The other half of the rewrite is RELIABILITY, and it is about
// IDENTIFIABILITY rather than about a formant being "missing". A voiced
// spectrum only samples the vocal-tract resonance curve at multiples of f0.
// When f0 is 352 Hz the samples sit at 352, 704, 1056 Hz -- so a first
// resonance anywhere in roughly 250-500 Hz fits those samples about equally
// well, and the estimator cannot say which. It does not mean the speaker has
// no F1 (every vocal tract has one), nor that F1 must lie below f0. It means
// F1's POSITION is not recoverable from this recording, and every estimator
// tested returns one anyway, biased toward the lowest thing present.
//
// (F0 above F1 is, separately, entirely possible -- source and filter are
// independent mechanisms, which is why sopranos sing above the F1 of close
// vowels. It is just not something this data can be used to establish.)
//
// So each band carries rel[] in 0..1 derived from F/f0 -- how densely the
// harmonics sample that region -- and the Matching stage weights by it. This
// is a property of the SIGNAL, computed before the value is known: not a
// guard that inspects an output and decides it looks wrong (two of those have
// already been tried and withdrawn; they misfire on voices whose formants
// genuinely are unusual).
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
    float hnr[3] = { 0.0f, 0.0f, 0.0f };   // band harmonic-to-noise ratio, dB

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
    float hnr[3] = { 0.0f, 0.0f, 0.0f };  // band harmonic-to-noise ratio, dB
    float tractScale = 1.0f;              // vocal-tract size vs the reference
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
    // Cepstral order = fs / (kCepDiv * f0). The harmonic-resolving limit is
    // fs / (2*f0): ABOVE it the envelope starts following individual
    // harmonics instead of formants. v0.29.0 shipped 1.6, i.e. 25 % past that
    // limit, which put 6-8 spurious maxima in the F3 band on real speech and
    // made the picked F3 jump frame to frame. The classical true-envelope
    // order is exactly fs / (2*f0); that is what this is now.
    static constexpr float kCepDiv   = 2.0f;
    static constexpr int   kCepIters = 8;     // true-envelope iterations
    static constexpr float kPreEmphDb = 6.0f; // dB per octave, above kPreEmphF0
    static constexpr float kPreEmphF0 = 100.0f;

    // A formant can only be LOCATED when enough harmonics sample it.
    // Measured on synthetic vowels with known formants (offline_test
    // "formant accuracy"): the median error over all bands is 0.23 st, but
    // every outlier above 1.5 st is an F1 whose F/f0 ratio sits between 2.6
    // and 3.3 -- close enough to the fundamental that the envelope peak
    // snaps onto a harmonic (at f0 300 an /a/ F1 of 775 Hz reads back as
    // 552 Hz, i.e. the second harmonic). The ramp therefore only starts
    // trusting a band at 2.5x f0 and reaches full trust at 4x.
    static constexpr float kRelLo = 2.5f, kRelHi = 4.0f;
    // reference speaker geometry the vowel anchors are written for
    static inline const float kRefL1 = std::log2 (480.0f);
    // Band HNR ramp, dB. Measured: ordinary speech and synthetic vowels run
    // +7..+19 dB per band; a band that is pure noise sits at 0 dB by
    // construction. The user's dark recording reads +1.5 (F2) and +0.4 (F3).
    static constexpr float kHnrLo = 2.0f, kHnrHi = 6.0f;

    // A band is only worth believing when BOTH are true:
    //   (a) the harmonics sample that region densely enough to place a peak
    //       (F/f0; below ~2.5 the resonance position is not identifiable), and
    //   (b) there is actually voiced structure there rather than noise.
    // (b) was missing in the first v0.29.0 cut and it is what broke matching
    // on the user's own recording: above 1 kHz that take is a flat noise floor
    // (band HNR +1.5 dB at F2, +0.4 dB at F3, against +12.6/+8.3 dB for
    // ordinary speech), so the "F3" being matched against was noise -- and
    // because F3 carries the largest vocal-tract weight, the global formant
    // it produced could come out near zero or negative, which for a
    // male-to-female match is not physically possible.
    static float density (float F, float f0)
    {
        if (! (f0 > 0.0f) || ! std::isfinite (F)) return 0.0f;
        return std::clamp ((F / f0 - kRelLo) / (kRelHi - kRelLo), 0.0f, 1.0f);
    }
    static float harmonicity (float hnrDb)
    {
        if (! std::isfinite (hnrDb)) return 0.0f;
        return std::clamp ((hnrDb - kHnrLo) / (kHnrHi - kHnrLo), 0.0f, 1.0f);
    }
    static float reliability (float F, float f0, float hnrDb)
    {
        return density (F, f0) * harmonicity (hnrDb);
    }
    // kept for callers that only have the frequencies (built-in catalog)
    static float reliability (float F, float f0) { return density (F, f0); }

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
        std::vector<float>  f0v, Fv[3], Lv[3], Rv[3], Hv[3], tv;
        std::vector<float>  pkF, pkL;           // cached peaks, kMaxPeaks/frame
        std::vector<int>    pkN, vv;            // peak count / vowel class

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

            harmonicEnvelope (mag.data(), NB, N, fs, f0, V.data(), Acur.data());

            // ---- formant candidates. The ASSIGNMENT is deferred: which
            // peak is F1/F2/F3 depends on a range prior, and that prior has
            // to be scaled to this speaker's vocal tract before it is safe
            // to apply (see the two-pass step after this loop).
            float pf[kMaxPeaks], pl[kMaxPeaks];
            const int npk = extractPeaks (V.data(), N, NB, fs, pf, pl);
            if (npk < 3) continue;

            double eLo = 0.0, eHi = 0.0;
            auto binOf = [&] (double hz) { return std::clamp ((int) std::lround (hz * N / fs), 1, NB - 1); };
            for (int k = binOf (60.0);   k <= binOf (1000.0); ++k) eLo += mag[(size_t) k];
            for (int k = binOf (2000.0); k <= binOf (std::min (8000.0, fs * 0.45)); ++k) eHi += mag[(size_t) k];

            float hn[3];
            bandHnr (mag.data(), NB, N, fs, f0, hn);

            f0v.push_back (f0);
            pkN.push_back (npk);
            for (int i = 0; i < kMaxPeaks; ++i)
            {
                pkF.push_back (i < npk ? pf[i] : 0.0f);
                pkL.push_back (i < npk ? pl[i] : 0.0f);
            }
            for (int fi = 0; fi < 3; ++fi) Hv[fi].push_back (hn[fi]);
            tv.push_back ((float) (10.0 * std::log10 ((eLo + 1.0e-20) / (eHi + 1.0e-20))));
        }

        // ---- two-pass assignment ------------------------------------------
        // The range prior (F1~500, F2~1500, F3~2600 Hz) describes an average
        // adult tract. Applied as-is it OUTVOTES the evidence for anyone far
        // from that: an anime/high-female /i/ with a true F2 of 3269 Hz is
        // penalized 1.26 while the whole level term can only repay ~0.35, so
        // the assignment picks a wrong, more central peak and the voice reads
        // back as having a longer tract than it does. That is a systematic
        // pull toward "average" -- and it is why matching to bright, small
        // -tract voices under-shot, sometimes to the point of a negative
        // formant shift on a male-to-female match.
        //
        // So: pass 1 assigns with the prior weakened, purely to estimate how
        // this speaker is scaled relative to the reference; pass 2 re-assigns
        // with the prior re-centred on that estimate. Only the cached peaks
        // are re-used, so the expensive envelope work is not repeated.
        {
            const size_t nf = f0v.size();
            auto runPass = [&] (float refScale,
                                std::vector<float>* Fo, std::vector<float>* Lo)
            {
                for (int b = 0; b < 3; ++b) { Fo[b].clear(); Lo[b].clear(); }
                float prev[3] = { -1.0f, -1.0f, -1.0f };
                for (size_t i = 0; i < nf; ++i)
                {
                    float Fi[3], Li[3];
                    if (! assignFormants (&pkF[i * kMaxPeaks], &pkL[i * kMaxPeaks],
                                          pkN[i], refScale, prev, Fi, Li))
                    { for (int b = 0; b < 3; ++b) { Fo[b].push_back (0.0f); Lo[b].push_back (0.0f); } }
                    else
                    {
                        for (int b = 0; b < 3; ++b)
                        { Fo[b].push_back (Fi[b]); Lo[b].push_back (Li[b]); prev[b] = Fi[b]; }
                    }
                }
            };
            std::vector<float> F1p[3], L1p[3];
            runPass (1.0f, F1p, L1p);
            // scale from the two bands that survive on real speech
            std::vector<float> r2, r3;
            for (size_t i = 0; i < nf; ++i)
            {
                if (F1p[1][i] > 0.0f) r2.push_back (F1p[1][i] / 1500.0f);
                if (F1p[2][i] > 0.0f) r3.push_back (F1p[2][i] / 2600.0f);
            }
            float sc = 1.0f;
            if (! r2.empty() && ! r3.empty())
                sc = std::sqrt (median (r2) * median (r3));
            else if (! r2.empty()) sc = median (r2);
            else if (! r3.empty()) sc = median (r3);
            if (! std::isfinite (sc)) sc = 1.0f;
            // Damped and tightly clamped on purpose: the scale is estimated
            // from the same noisy measurements it then re-centres, so an
            // undamped correction amplifies its own error. Measured on the
            // published-formant cases: 3/5 correct with no adaptation,
            // 5/5 at 0.5 damping, and undamped it starts overshooting.
            out.tractScale = std::clamp (1.0f + 0.5f * (sc - 1.0f), 0.85f, 1.25f);
            runPass (out.tractScale, Fv, Lv);
        }
        // drop frames the final pass could not assign
        {
            size_t w = 0;
            for (size_t i = 0; i < f0v.size(); ++i)
            {
                if (! (Fv[0][i] > 0.0f)) continue;
                f0v[w] = f0v[i]; tv[w] = tv[i];
                for (int b = 0; b < 3; ++b)
                { Fv[b][w] = Fv[b][i]; Lv[b][w] = Lv[b][i]; Hv[b][w] = Hv[b][i]; }
                ++w;
            }
            f0v.resize (w); tv.resize (w);
            for (int b = 0; b < 3; ++b) { Fv[b].resize (w); Lv[b].resize (w); Hv[b].resize (w); }
        }
        for (size_t i = 0; i < f0v.size(); ++i)
            for (int b = 0; b < 3; ++b)
                Rv[b].push_back (reliability (Fv[b][i], f0v[i], Hv[b][i]));

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
            out.hnr[fi] = median (Hv[fi]);
        }
        const float Lmax = std::max ({ Lmed[0], Lmed[1], Lmed[2] });
        for (int fi = 0; fi < 3; ++fi) out.L[fi] = Lmed[fi] - Lmax;
        out.tiltDb = median (tv);

        classifyVowels (Fv, Rv, vv);
        buildVowelProfiles (out, f0v, Fv, Lv, Rv, Hv, vv);
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
    static constexpr int kMaxPeaks = 24;
    static constexpr float kAnchorH[5] = { 0.86f, 0.06f, 0.18f, 0.44f, 0.47f };
    static constexpr float kAnchorF[5] = { 0.04f, 1.00f, 0.57f, 0.67f, 0.13f };
    // the same five anchors expressed as a function of F2 only (F1 held at
    // the reference), for frames whose F1 could not be located
    static constexpr float kAnchorF2[5] = { 0.354f, 0.741f, 0.378f, 0.672f, 0.152f };

    // Spectral envelope, IDENTICAL IN METHOD to the one PsolaEngine uses per
    // grain in the realtime path (spectralProcess): take one peak per
    // harmonic, connect them by straight lines in the log domain, then smooth
    // lightly. Two reasons to use the engine's own method here rather than
    // something of our own:
    //
    //  * It is structurally immune to the failure the cepstral version had.
    //    It samples exactly ONE point per harmonic, so it cannot follow
    //    individual harmonics no matter what the pitch is -- the cepstral
    //    order had to be tuned to avoid that, and got it wrong.
    //  * Matching measures the voice, then hands parameters to the engine,
    //    which warps that same envelope. Measuring with the engine's own
    //    estimator means any systematic bias appears on BOTH sides and
    //    largely cancels, instead of being a discrepancy between what
    //    Matching thinks it set and what the engine actually does.
    //
    // This is a copy of the method, not a call into it: PsolaEngine's version
    // is per-grain, resampled-domain and stateful, and the realtime path must
    // not be disturbed. Keep the two in step if either changes.
    static void harmonicEnvelope (const double* mag, int NB, int N, double fs,
                                  float f0, float* env, float* tmp)
    {
        const double spacing = std::max (2.5, (double) f0 * N / fs);
        int   pkB[512]; float pkV[512]; int np = 0;
        for (double cb = spacing; cb < (double) (NB - 2) && np < 512; cb += spacing)
        {
            const int a = std::max (1, (int) (cb - 0.45 * spacing));
            const int b = std::min (NB - 1, (int) (cb + 0.45 * spacing));
            int bm = a; double vm = mag[(size_t) a];
            for (int t = a + 1; t <= b; ++t)
                if (mag[(size_t) t] > vm) { vm = mag[(size_t) t]; bm = t; }
            pkB[np] = bm; pkV[np] = (float) std::log (vm + 1.0e-12);
            ++np;
        }
        if (np >= 2)
        {
            int seg = 0;
            for (int k = 0; k <= NB; ++k)
            {
                float lv;
                if (k <= pkB[0])         lv = pkV[0];
                else if (k >= pkB[np-1]) lv = pkV[np-1];
                else
                {
                    while (seg + 1 < np && pkB[seg + 1] < k) ++seg;
                    const int a = pkB[seg], b = pkB[seg + 1];
                    const float t = (float) (k - a) / (float) std::max (1, b - a);
                    lv = pkV[seg] * (1.0f - t) + pkV[seg + 1] * t;
                }
                env[k] = lv;                       // kept in LOG domain (dB-like)
            }
        }
        else
            for (int k = 0; k <= NB; ++k) env[k] = (float) std::log (mag[(size_t) k] + 1.0e-12);

        // light smoothing to remove the interpolation kinks (same 2 passes)
        for (int pass = 0; pass < 2; ++pass)
        {
            for (int k = 0; k <= NB; ++k)
            {
                const int a = std::max (0, k - 3), b = std::min (NB, k + 3);
                double acc = 0.0;
                for (int t = a; t <= b; ++t) acc += env[t];
                tmp[k] = (float) (acc / (b - a + 1));
            }
            for (int k = 0; k <= NB; ++k) env[k] = tmp[k];
        }
        // `mag` is POWER here (the tilt and HNR stages downstream need it that
        // way), so the envelope is ln(power); 10/ln(10) converts it to the
        // same dB scale the levels used before.
        for (int k = 0; k <= NB; ++k) env[k] *= 4.3429448f;
    }

    // Interior local maxima of the envelope, parabolically refined. Interior
    // only: a point that is merely the largest value at the edge of a
    // decaying envelope is not a resonance.
    static int extractPeaks (const float* V, int N, int NB, double fs,
                             float* outF, float* outL)
    {
        const int kl = std::max (1, (int) (150.0 * N / fs));
        const int kh = std::min (NB - 1, (int) (4800.0 * N / fs));
        int n = 0;
        for (int k = kl; k <= kh && n < kMaxPeaks; ++k)
        {
            if (! (V[k] >= V[k-1] && V[k] >= V[k+1])) continue;
            const float d = V[k-1] - 2.0f * V[k] + V[k+1];
            float dk = std::abs (d) > 1.0e-12f ? 0.5f * (V[k-1] - V[k+1]) / d : 0.0f;
            dk = std::clamp (dk, -1.0f, 1.0f);
            outF[n] = (float) ((k + dk) * fs / N);
            outL[n] = V[k];
            ++n;
        }
        return n;
    }

    // Formant selection, again the SAME RULE the realtime engine uses: the
    // highest interior local maximum inside each band's search range, and if
    // there is no interior maximum at all, keep the previous frame's value
    // (or a default on the first frame) rather than pinning to a band edge.
    //
    // That fallback is the important part. The pre-v0.29.0 analyzer took the
    // plain range maximum, which on a decaying envelope IS the range edge --
    // that is how a target profile came to claim "F1 = 1.35 x f0". The engine
    // never had that bug because it has always fallen back instead.
    //
    // Deliberately NOT carried over from the engine: its 0.7/0.3 inter-frame
    // smoothing. That is right for a continuous audio stream and wrong here,
    // where consecutive frames may be different vowels and get pooled into
    // per-vowel statistics -- smoothing across a vowel boundary would blend
    // two vowels into one measurement.
    //
    // `sc` scales the search ranges to this speaker's vocal tract, so a small
    // tract's bright F2 (an anime voice can sit past 3 kHz) does not fall off
    // the top of a box sized for an average adult.
    static bool assignFormants (const float* pf, const float* pl, int npk,
                                float sc, const float* prev,
                                float* Fout, float* Lout)
    {
        constexpr float loR[3]  = { 250.0f,  850.0f, 1900.0f };
        constexpr float hiR[3]  = { 1000.0f, 2600.0f, 3800.0f };
        constexpr float defR[3] = { 500.0f,  1500.0f, 2500.0f };
        if (npk < 1) return false;
        const float s = (std::isfinite (sc) && sc > 0.1f) ? sc : 1.0f;

        for (int b = 0; b < 3; ++b)
        {
            const float lo = loR[b] * s, hi = hiR[b] * s;
            int best = -1; float bv = -1.0e30f;
            for (int i = 0; i < npk; ++i)
                if (pf[i] >= lo && pf[i] <= hi && pl[i] > bv) { bv = pl[i]; best = i; }
            if (best >= 0) { Fout[b] = pf[best]; Lout[b] = pl[best]; }
            else
            {
                Fout[b] = (prev != nullptr && prev[b] > 0.0f) ? prev[b] : defR[b] * s;
                Lout[b] = -60.0f;
            }
        }
        // same minimum spacing the engine enforces, so the three can never
        // collapse onto one resonance
        Fout[1] = std::max (Fout[1], Fout[0] + 150.0f * s);
        Fout[2] = std::max (Fout[2], Fout[1] + 200.0f * s);
        return true;
    }

    // Vowel identity lives in (F1, F2). When F1 cannot be located -- which is
    // exactly what happens on a high-pitched target, the case this feature
    // exists for -- feeding the raw F1 in classifies on noise and scatters one
    // vowel across several buckets. Those frames are placed on F2 ALONE, using
    // a one-dimensional distance against kAnchorF2 below.
    //
    // (Pinning F1 to a fixed value instead does NOT work: the height
    // coordinate then sits permanently next to the /e/ anchor and every frame
    // classifies as /e/. Measured: 262 of 264 frames, on the anime case this
    // is meant to rescue.)
    //
    // F2 alone cannot separate /a/ from /u/ -- their F2 differs by under
    // 1 st -- so those two may share a bucket on a very high voice. That
    // costs some per-vowel detail; it does not bias the global shift, which
    // is what the formant conversion actually rides on.
    static void classifyVowels (const std::vector<float>* Fv,
                                const std::vector<float>* Rv,
                                std::vector<int>& vv)
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
        static const float R1 = kRefL1, R2 = std::log2 (1450.0f);
        for (size_t i = 0; i < n; ++i)
        {
            const bool f1ok = Rv[0][i] >= 0.25f;
            const float f2n = std::exp2 (l2[i] - m2 + R2);
            int bestA = 0; float bestD = 1.0e30f;
            if (f1ok)
            {
                const float f1n = std::exp2 (l1[i] - m1 + R1);
                float h, fr;
                vowelCoord (f1n, f2n, h, fr);
                for (int a = 0; a < 5; ++a)
                {
                    const float dh = h - kAnchorH[a], df = fr - kAnchorF[a];
                    const float d = dh * dh + df * df;
                    if (d < bestD) { bestD = d; bestA = a; }
                }
            }
            else
            {
                // F2-only: same normalization, F1 held at the reference so the
                // coordinate is a pure function of F2, compared against
                // anchors computed the same way.
                const float fr = std::clamp (((std::log2 (std::max (f2n, 1.0f)) - kRefL1)
                                              - 0.55f) / (2.90f - 0.55f), 0.0f, 1.0f);
                for (int a = 0; a < 5; ++a)
                {
                    const float df = fr - kAnchorF2[a];
                    const float d = df * df;
                    if (d < bestD) { bestD = d; bestA = a; }
                }
            }
            vv[i] = bestA;
        }
    }

    static void buildVowelProfiles (VoiceProfile& out,
                                    const std::vector<float>& f0v,
                                    const std::vector<float>* Fv,
                                    const std::vector<float>* Lv,
                                    const std::vector<float>* Rv,
                                    const std::vector<float>* Hv,
                                    const std::vector<int>& vv)
    {
        for (int a = 0; a < 5; ++a)
        {
            std::vector<float> g0, gF[3], gL[3], gR[3], gH[3];
            for (size_t i = 0; i < vv.size(); ++i)
            {
                if (vv[i] != a) continue;
                g0.push_back (f0v[i]);
                for (int b = 0; b < 3; ++b)
                {
                    gF[b].push_back (Fv[b][i]);
                    gL[b].push_back (Lv[b][i]);
                    gR[b].push_back (Rv[b][i]);
                    gH[b].push_back (Hv[b][i]);
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
                vp.hnr[b] = median (gH[b]);
            }
            const float mx = std::max ({ lm[0], lm[1], lm[2] });
            for (int b = 0; b < 3; ++b) vp.L[b] = lm[b] - mx;
        }
    }

    // Band harmonic-to-noise ratio: energy AT the harmonics against energy
    // BETWEEN them, per band, in dB. Pure noise gives ~0 dB by construction;
    // voiced formant structure gives +7..+19 dB. This is what tells the
    // matcher whether there is anything in a band worth measuring, as
    // opposed to how densely the harmonics sample it (see density()).
    static void bandHnr (const double* mag, int NB, int N, double fs,
                         float f0, float* out)
    {
        constexpr double LOB[3] = { 250.0, 700.0, 1700.0 };
        constexpr double HIB[3] = { 1100.0, 2600.0, 4200.0 };
        for (int b = 0; b < 3; ++b)
        {
            float on[64], off[64];
            int n = 0;
            const int w = std::max (1, (int) std::lround (0.25 * f0 * N / fs));
            for (int h = (int) std::ceil (LOB[b] / f0); n < 64; ++h)
            {
                const double fc = h * (double) f0;
                if (fc >= HIB[b] || fc >= fs * 0.45) break;
                const int k  = (int) std::lround (fc * N / fs);
                const int kb = (int) std::lround ((fc + 0.5 * f0) * N / fs);
                if (k - w < 0 || kb + w > NB) break;
                double a = 0.0, c = 0.0;
                for (int i = k - w;  i <= k + w;  ++i) a = std::max (a, mag[(size_t) i]);
                for (int i = kb - w; i <= kb + w; ++i) c = std::max (c, mag[(size_t) i]);
                on[n] = (float) a; off[n] = (float) c; ++n;
            }
            if (n < 2) { out[b] = 0.0f; continue; }
            std::vector<float> vo (on, on + n), vf (off, off + n);
            const float mo = median (vo), mf = median (vf);
            const double r = (double) mo / ((double) mf + 1.0e-20);
            out[b] = (float) (10.0 * std::log10 (std::max (r, 1.0e-6)));
            if (! std::isfinite (out[b])) out[b] = 0.0f;
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
