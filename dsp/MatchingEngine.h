// MatchingEngine.h — Auto-Set / Refine calculation for the Matching tab.
//
// v0.29.0: vowel-matched, reliability-weighted matching.
//
// The v0.27.0-v0.28.4 formula compared the two recordings' GLOBAL median
// F1/F2/F3 and averaged the three resulting semitone shifts. Two things break
// that, and both were reproduced on the user's own recordings:
//
//  1. A median over all frames mixes vowels. F2 alone swings ~16 st across
//     the vowel space (850 Hz on /o/ to 2200 Hz on /i/), so unless the two
//     recordings contain the SAME vowels in the same proportion, the medians
//     describe different sounds. On the reported pair the three bands
//     demanded +7.0 / -7.3 / -4.1 st; their average (-1.5 st) described
//     nothing, and f1shift railed against its +-3 st clamp trying to make up
//     the difference -- exactly the "F1 never reaches the target" symptom.
//  2. A formant's position is only identifiable where the harmonics sample it
//     densely enough. The target sits at f0 = 352 Hz (confirmed by plain
//     autocorrelation, 60/60 frames, and by the 176 Hz line being 32 dB down,
//     so this is not an octave error), which puts its lowest spectral samples
//     at 352/704/1056 Hz -- a first resonance anywhere from ~250 to ~500 Hz
//     fits them equally well. The old code matched against whatever number
//     fell out, with full confidence.
//
// So: compare each vowel with the SAME vowel, weight every comparison by how
// measurable that band actually was (VoiceAnalyzer::reliability), and take a
// weighted median rather than a mean. A vocal tract of a different length
// scales all formants by roughly one factor, so the global shift is that
// factor; what remains per band and per vowel is the speaker's individual
// vowel colouring, which is what the AEIOU map is for.
//
// Dependency-free, no JUCE, no allocation on the hot paths, no strings
// beyond the compile-time parameter ids returned by name(). Every value
// is finite (clamped through the same jlimit-equivalent stdClamp with
// std::isfinite). The panel remains responsible for actually writing the
// parameters through history.group / isParamLocked / setValueNotifyingHost.
#pragma once
#include <cmath>
#include <algorithm>

struct VoiceProfile;   // fwd; concrete type defined in dsp/VoiceAnalyzer.h

class MatchingEngine
{
public:
    // one recommended parameter write. If `apply` is false the panel must
    // not touch this parameter (e.g. the source spread was too narrow to
    // derive a meaningful range/center).
    struct Change
    {
        const char* id;
        float       value;
        bool        apply;
    };

    // A proposal is a fixed set of up to N changes. The engine writes into
    // `count` slots; the panel then applies them in a single history.group.
    // Auto-Set now also writes the AEIOU map (vadapt + vamount + vcharacter
    // + 15 shift ids + 15 gain ids), so the ceiling is much higher than the
    // 16 that covered v0.28.4. Still a fixed array: no allocation.
    static constexpr int kMax = 64;

    struct Proposal
    {
        Change  changes[kMax] {};
        int     count = 0;
        // summary values that the panel needs for its status line
        float   pitch = 0.0f;
        float   formant = 0.0f;
        float   tilt = 0.0f;
        float   range = 0.0f;
        float   center = 0.0f;
        float   hifreq = 0.0f;
        float   pitchfloor = 0.0f;
        float   air = 0.0f;
        float   airshine = 0.0f;
        bool    rangeApplied = false;
        bool    airApplied = false;

        // ---- v0.29.0 measurement report (drives the status line) ----------
        // Per-band shift the measurement asks for, over and above the global
        // formant, and how far the per-vowel comparisons disagreed.
        float   bandShiftSt[3] = { 0.0f, 0.0f, 0.0f };
        float   bandSpreadSt = 0.0f;
        // reliability actually available per band, 0..1 (min of both sides)
        float   bandRel[3] = { 0.0f, 0.0f, 0.0f };
        // vowels compared, and the weighted MAD of all comparisons (st).
        int     vowelsMatched = 0;
        float   agreementSt = 0.0f;
        // true when the vowel-matched path could not run and the engine fell
        // back to comparing global medians (the v0.28.4 behaviour).
        bool    fellBack = false;
        // true when the result should not be trusted without a re-record
        bool    lowConfidence = false;
        // per-vowel residual shifts written into the AEIOU map (A,I,U,E,O)
        float   vowelShiftSt[5][3] = {};
        bool    vowelUsed[5] = { false, false, false, false, false };

        void push (const char* id, float value, bool apply = true)
        {
            if (count >= kMax) return;
            changes[count++] = { id, value, apply };
        }
    };

    // Absolute Auto-Set: derive a full parameter set from the difference
    // between the user's own voice profile and the target profile.
    static Proposal autoSet (const VoiceProfile& current, const VoiceProfile& target);

    // Refine: nudge the current parameter values by the damped residual
    // between the last CONVERTED profile and the target. Needs the current
    // parameter values (id -> float lookup) because it works additively.
    template <class ParamGetter>
    static Proposal refine (const VoiceProfile& target,
                            const VoiceProfile& converted,
                            ParamGetter        getCurrent);

    // Estimated profile prediction (spec 7.3): apply the current parameter
    // values to the CURRENT profile without actually running the engine.
    // Used only for the Matching graph — not for parameter writes.
    template <class ParamGetter>
    static VoiceProfile predictEstimated (const VoiceProfile& current,
                                          ParamGetter getCurrent);

    static constexpr float kPitchBias  = 1.0f;    // st below the plain F0 match
    // Intonation. The engine computes
    //     f_out = center * ((f_in * 2^(pitch/12)) / center)^range
    // so the CONVERTED MEDIAN only lands on the target when the pitch-shifted
    // median coincides with the pivot -- otherwise the gap between them is
    // raised to the power of `range` and gets amplified.
    //
    // That is what was wrong. pitch aimed 1 st under the target while center
    // sat exactly ON the target, so the 1 st gap became `range` semitones:
    // with a wide-intonation target it railed range at 200 % and the result
    // came out a full 2.00 st under the target median. Measured on the anju
    // sample: target f0 276.1 Hz, converted median 246.0 Hz.
    //
    // Fixed by aiming BOTH pitch and center at the same biased target, so the
    // shifted median sits on the pivot and the median becomes invariant to
    // range. The 1.15 boost is gone with it: it was compensating for the very
    // sag this created, and with the sag removed it is just extra intonation.
    static constexpr float kRangeBoost = 1.0f;
    // Upper bound on Intonation Amount. The honest ratio can ask for well
    // over 200 % against an expressive target, which is unusable; past ~140 %
    // the expansion stops reading as expression and starts sounding unstable.
    static constexpr float kRangeMax = 140.0f;
    static constexpr float kRangeMin = 50.0f;

    // Air / Air Shine, both derived from measurements and both deliberately
    // capped well below their parameter maxima (1.5 and 6 dB): these are
    // texture, and overshooting them is far more damaging than leaving them
    // at zero. Only ever positive -- there is no "negative breath", so a
    // target cleaner than the source correctly asks for nothing.
    static constexpr float kAirPerDb   = 0.06f;   // per dB of extra breathiness
    static constexpr float kAirMax     = 0.60f;
    static constexpr float kShinePerDb = 0.35f;   // per dB of extra >6 kHz
    static constexpr float kShineMax   = 3.0f;
    // Band-HNR ramp for reading breathiness off the TARGET alone (v0.40.0).
    // 0 dB is pure noise by construction and ordinary speech runs +7..+19,
    // so "clean" sits at the bottom of the speech range and "breathy" where
    // the built-in characters actually land (3.2-9.4 dB over bands 2-3).
    static constexpr float kAirHnrClean   = 12.0f;
    static constexpr float kAirHnrBreathy =  3.0f;

    // A band only contributes when both sides could LOCATE it this well.
    static constexpr float kMinRel = 0.25f;
    // Weight of each band as a VOCAL TRACT LENGTH indicator. F1 and F2 move
    // ~5.9 st (sd) across the vowel space against F3's ~1.7 st, so any
    // leftover vowel mismatch corrupts them roughly three times as much;
    // weights are 1/sd normalized to F3 = 1. This applies ONLY to the global
    // shift — the per-band and per-vowel residuals use their own band.
    static constexpr float kVtlW[3] = { 0.29f, 0.29f, 1.0f };
    // Disagreement (weighted MAD, st) above which the two recordings are
    // reported as not comparable. Matched-content pairs land at 0.08-0.37 st
    // (0.78 st on the user's real recordings); the disjoint-vowel case that
    // still misses by 2.2 st reports 2.38, so the line goes between them.
    static constexpr float kMadWarn = 2.0f;
    // Shrinkage applied to the per-band trims. On synthetic pairs that differ
    // ONLY by a uniform vocal-tract scale -- where the true per-band residual
    // is exactly zero -- the measured residual still comes out at 0.7 st
    // typical and occasionally rails at the 2 st clamp. Real per-band
    // differences between speakers are the same order, so the measurement is
    // roughly half signal and half noise and gets halved to match. The global
    // shift, which is measured an order of magnitude better (0.03-0.34 st on
    // those same pairs), carries the bulk of the correction.
    static constexpr float kBandTrust = 0.5f;
    // Safety clamps for the AEIOU map (must match the va_* parameter ranges
    // in PluginProcessor and VowelAdaptiveWarp::kMaxMap).
    static constexpr float kVowelClamp[3] = { 2.0f, 3.0f, 1.5f };
    static constexpr float kVowelGainClamp = 3.0f;

private:
    static float st (float a, float b) { return 12.0f * std::log2 (a / b); }
    static float cl (float v, float lo, float hi)
    {
        if (! std::isfinite (v)) return 0.0f;
        return std::clamp (v, lo, hi);
    }

    // weighted median over a small fixed set (n <= 15 here: 5 vowels x 3 bands)
    static float wMedian (const float* v, const float* w, int n)
    {
        if (n <= 0) return 0.0f;
        int idx[15];
        n = std::min (n, 15);
        for (int i = 0; i < n; ++i) idx[i] = i;
        for (int i = 1; i < n; ++i)          // insertion sort by value
            for (int j = i; j > 0 && v[idx[j]] < v[idx[j-1]]; --j)
                std::swap (idx[j], idx[j-1]);
        float total = 0.0f;
        for (int i = 0; i < n; ++i) total += w[idx[i]];
        if (! (total > 0.0f)) return v[idx[n / 2]];
        float acc = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            acc += w[idx[i]];
            if (acc >= 0.5f * total) return v[idx[i]];
        }
        return v[idx[n - 1]];
    }
};

// Concrete VoiceProfile field access — defined here as a template so this
// header stays independent of VoiceAnalyzer.h include order.
#include "VoiceAnalyzer.h"

inline MatchingEngine::Proposal
MatchingEngine::autoSet (const VoiceProfile& p1, const VoiceProfile& p2)
{
    Proposal r;
    if (! p1.valid() || ! p2.valid()) return r;

    // Aim pitch and pivot at the SAME point: the target median, taken
    // kPitchBias semitones low (the long-standing perceptual trim -- the
    // converted voice reads as too high at a literal F0 match). Because the
    // shifted median then lands exactly on the pivot, the intonation stage
    // leaves the median alone whatever `range` turns out to be.
    const float targetMedianHz = p2.f0Hz * std::pow (2.0f, -kPitchBias / 12.0f);
    r.pitch = cl (st (targetMedianHz, p1.f0Hz), -24.0f, 24.0f);

    // ---- collect every (vowel, band) comparison both sides could measure --
    float sv[15], sw[15];          // shift value / weight, for the global fit
    int   svV[15], svB[15];        // which vowel / band each came from
    int   ns = 0;
    for (int v = 0; v < 5 && ns < 15; ++v)
    {
        const VowelProfile& a = p1.vow[v];
        const VowelProfile& b = p2.vow[v];
        if (! a.valid() || ! b.valid()) continue;
        for (int b3 = 0; b3 < 3 && ns < 15; ++b3)
        {
            const float rel = std::min (a.rel[b3], b.rel[b3]);
            if (rel < kMinRel) continue;
            if (! (a.F[b3] > 0.0f) || ! (b.F[b3] > 0.0f)) continue;
            const float s = st (b.F[b3], a.F[b3]);
            if (! std::isfinite (s)) continue;
            const int   nmin = std::min (a.frames, b.frames);
            sv[ns] = s;
            sw[ns] = rel * (float) nmin * kVtlW[b3];
            svV[ns] = v; svB[ns] = b3;
            ++ns;
        }
    }

    // How many DISTINCT vowels the comparison rests on. One vowel is not a
    // vocal-tract measurement: it is one articulation, and whatever that
    // vowel's individual colouring is gets read as the speaker's tract size.
    // The engine already called this case lowConfidence; v0.37.1 makes it
    // ACT on that instead of merely reporting it, and take the global path,
    // which averages over the whole recording rather than over one bucket.
    //
    // This became reachable in practice once the F1 estimate stopped being
    // pinned to the envelope's flat extrapolation (see VoiceAnalyzer.h
    // extractPeaks): F1 feeds classifyVowels, so correcting it re-sorts the
    // vowel buckets, and a recording whose buckets do not overlap the other
    // side's can end up with a single vowel in common. Measured on the
    // 160 Hz male -> 352 Hz female pair: the single surviving vowel demanded
    // -0.37 st, i.e. a LONGER vocal tract for the smaller speaker, which is
    // physically impossible; the global path gives +1.14 st against a truth
    // of +2.87 st -- still short, but the right sign and inside tolerance.
    int nVowelsSeen = 0;
    { bool sv5[5] = {}; for (int i = 0; i < ns; ++i) sv5[svV[i]] = true;
      for (int v = 0; v < 5; ++v) if (sv5[v]) ++nVowelsSeen; }

    if (ns > 0 && nVowelsSeen >= 2)
    {
        r.formant = cl (wMedian (sv, sw, ns), -24.0f, 24.0f);

        // agreement: weighted MAD about the chosen global shift
        float dv[15];
        for (int i = 0; i < ns; ++i) dv[i] = std::abs (sv[i] - r.formant);
        r.agreementSt = wMedian (dv, sw, ns);

        bool seen[5] = {};
        for (int i = 0; i < ns; ++i) seen[svV[i]] = true;
        for (int v = 0; v < 5; ++v) if (seen[v]) ++r.vowelsMatched;

        // ---- per-band residual: weighted median over the vowels ----------
        for (int b3 = 0; b3 < 3; ++b3)
        {
            float bv[5], bw[5]; int nb = 0;
            float relMax = 0.0f;
            for (int i = 0; i < ns && nb < 5; ++i)
                if (svB[i] == b3)
                {
                    bv[nb] = sv[i] - r.formant;
                    bw[nb] = sw[i];
                    ++nb;
                }
            for (int v = 0; v < 5; ++v)
                if (p1.vow[v].valid() && p2.vow[v].valid())
                    relMax = std::max (relMax, std::min (p1.vow[v].rel[b3], p2.vow[v].rel[b3]));
            r.bandRel[b3] = relMax;
            // Scale by reliability so an unmeasurable band contributes
            // nothing instead of noise: F1 against a very high-pitched
            // target lands here at 0, and F1 then moves purely with the
            // global formant, which is the physically right answer for a
            // uniformly scaled vocal tract.
            r.bandShiftSt[b3] = nb > 0
                ? cl (kBandTrust * relMax * wMedian (bv, bw, nb), -2.0f, 2.0f)
                : 0.0f;
        }
        r.bandSpreadSt = std::max ({ r.bandShiftSt[0], r.bandShiftSt[1], r.bandShiftSt[2] })
                       - std::min ({ r.bandShiftSt[0], r.bandShiftSt[1], r.bandShiftSt[2] });
        r.lowConfidence = (r.agreementSt > kMadWarn) || (r.vowelsMatched < 2);

        // ---- per-vowel residual -> AEIOU map ------------------------------
        for (int i = 0; i < ns; ++i)
        {
            const int v = svV[i], b3 = svB[i];
            const float resid = sv[i] - r.formant - r.bandShiftSt[b3];
            r.vowelShiftSt[v][b3] = cl (resid, -kVowelClamp[b3], kVowelClamp[b3]);
            r.vowelUsed[v] = true;
        }
    }
    else
    {
        // ---- fallback: no vowel could be matched on either side -----------
        // Compare global medians, but still only where the band was
        // measurable, and still with a weighted median rather than a mean.
        r.fellBack = true;
        float gv[3], gw[3]; int ng = 0;
        for (int b3 = 0; b3 < 3; ++b3)
        {
            const float rel = std::min (p1.rel[b3], p2.rel[b3]);
            r.bandRel[b3] = rel;
            if (rel < kMinRel) continue;
            if (! (p1.F[b3] > 0.0f) || ! (p2.F[b3] > 0.0f)) continue;
            const float s = st (p2.F[b3], p1.F[b3]);
            if (! std::isfinite (s)) continue;
            gv[ng] = s; gw[ng] = rel * kVtlW[b3]; ++ng;
        }
        // Weighted MEAN here, not the weighted median used on the
        // vowel-matched path: with only three points and F3 weighted 1.0
        // against 0.29, a median always returns F3 outright and throws away
        // F1 and F2 entirely. There are no outliers to guard against in a
        // three-band global comparison, so every band should contribute.
        float gsum = 0.0f, wsum = 0.0f;
        for (int i = 0; i < ng; ++i) { gsum += gv[i] * gw[i]; wsum += gw[i]; }
        r.formant = wsum > 0.0f ? cl (gsum / wsum, -24.0f, 24.0f) : 0.0f;
        for (int b3 = 0; b3 < 3; ++b3)
        {
            const float rel = std::min (p1.rel[b3], p2.rel[b3]);
            const float s = (rel >= kMinRel && p1.F[b3] > 0.0f && p2.F[b3] > 0.0f)
                          ? st (p2.F[b3], p1.F[b3]) : r.formant;
            r.bandShiftSt[b3] = cl (kBandTrust * rel * (s - r.formant), -2.0f, 2.0f);
        }
        r.bandSpreadSt = std::max ({ r.bandShiftSt[0], r.bandShiftSt[1], r.bandShiftSt[2] })
                       - std::min ({ r.bandShiftSt[0], r.bandShiftSt[1], r.bandShiftSt[2] });
        // Falling back is only a WARNING when a per-vowel comparison should
        // have been possible. A built-in catalog target (or a .vmprofile
        // saved before v0.29.0) simply has no vowel table, which is expected
        // and not something re-recording would fix.
        r.lowConfidence = (p1.vowelsMeasured() > 0 && p2.vowelsMeasured() > 0);
    }

    r.tilt    = cl (0.25f * (p2.tiltDb - p1.tiltDb), -4.0f, 4.0f);
    r.hifreq  = cl (p1.f0Hz * std::pow (2.0f, (p1.f0SpreadSt + 2.0f) / 12.0f),
                    150.0f, 600.0f);
    r.pitchfloor = cl (p2.f0Hz * std::pow (2.0f, -(p2.f0SpreadSt + 1.0f) / 12.0f),
                       0.0f, 300.0f);

    r.push ("pitch",   r.pitch);
    r.push ("formant", r.formant);
    const char* sid[3] = { "f1shift", "f2shift", "f3shift" };
    const char* gid[3] = { "f1gain",  "f2gain",  "f3gain"  };
    for (int i = 0; i < 3; ++i)
    {
        // The per-band trim is a MEASURED residual: reliability scaled,
        // shrunk by kBandTrust and clamped to +-2 st before it gets here.
        r.push (sid[i], cl (r.bandShiftSt[i], -3.0f, 3.0f));
        // Levels are relative to each profile's own strongest formant, which
        // makes them comparable across recordings but noisy; keep the
        // long-standing 0.5 damping here.
        //
        // v0.37.1: gated on the same reliability as the shift. A band nobody
        // could LOCATE has no level either -- VoiceAnalyzer now reports L = 0
        // for such a band (previously it was a median over frames that never
        // saw it, polluted by the -60 dB not-found sentinel), and 0 means
        // "level of the strongest formant", so an ungated trim would read a
        // missing band as being exactly as loud as the loudest one and ask
        // for several dB of correction on the strength of nothing.
        const bool bandUsable = r.bandRel[i] >= kMinRel;
        r.push (gid[i], bandUsable ? cl (0.5f * (p2.L[i] - p1.L[i]), -8.0f, 8.0f) : 0.0f);
    }
    r.push ("tilt", r.tilt);
    if (p1.f0SpreadSt > 0.3f && p2.f0SpreadSt > 0.3f)
    {
        r.range  = cl (kRangeBoost * 100.0f * p2.f0SpreadSt / p1.f0SpreadSt,
                       kRangeMin, kRangeMax);
        // the pivot is the pitch-shifted median, i.e. the same biased target
        // the pitch aims at -- see kRangeBoost above
        r.center = cl (targetMedianHz, 80.0f, 400.0f);
        r.push ("range",  r.range);
        r.push ("center", r.center);
        r.rangeApplied = true;
    }
    // ---- Air / Air Shine -------------------------------------------------
    // Breathiness shows up as LOW harmonic-to-noise ratio in the upper bands,
    // so a target with less harmonic structure than the source is the breathy
    // one. Only the bands that carry usable structure on the source side are
    // compared, otherwise a noisy recording reads as "very breathy" and asks
    // for maximum air.
    // A built-in catalog target (and any .vmprofile older than this) carries
    // no texture measurement at all. Comparing against its zeroed defaults
    // would read as "infinitely breathy, no shine" and ask for air the target
    // never asked for, so those two are simply left alone in that case --
    // whatever the user already has stays.
    {
        const bool tgtMeasured = (p2.hnr[0] != 0.0f || p2.hnr[1] != 0.0f || p2.hnr[2] != 0.0f);
        const bool srcMeasured = (p1.hnr[0] != 0.0f || p1.hnr[1] != 0.0f || p1.hnr[2] != 0.0f);
        if (tgtMeasured)
        {
            // ---- (a) how breathy the TARGET is, on its own ---------------
            // v0.40.0. The difference term below used to be the whole story,
            // and it asks the wrong question. Band HNR conflates the
            // speaker's breathiness with the recording's noise floor, mic and
            // codec, so a cross-recording SUBTRACTION is only meaningful when
            // both takes were made under comparable conditions -- and the one
            // pairing that never is, is "my microphone" against "a character
            // sample". A user on a slightly noisy mic reads as breathier than
            // every target and gets air = 0 from a formula that is really
            // reporting their room.
            //
            // What Air actually does is preserve the breath that IS in the
            // source through the pitch shift, instead of letting grain
            // relocation stamp the new period onto it. You want that in
            // proportion to how breathy the RESULT should sound -- a property
            // of the target alone. So the target's own upper-band HNR sets a
            // floor, and the difference can still ask for more on top.
            //
            // Scale, from VoiceAnalyzer's own measurements: a band that is
            // pure noise sits at 0 dB by construction, ordinary speech runs
            // +7..+19 dB. The seven built-in characters land between 3.2 and
            // 9.4 dB averaged over bands 2-3, so the ramp spans that range
            // and gives them 0.17 (Funi, the least breathy) to 0.59 (Yuni and
            // Maki, the most).
            //
            // This is an ESTIMATE of how breathy the recording sounds, not a
            // pure property of the voice -- a noisy sample reads as breathy.
            // It is deliberately preferred over reporting nothing, because
            // Air at 0 is audibly wrong on these voices and "no opinion" is
            // not a neutral answer for a parameter whose default is off.
            float tAcc = 0.0f; int tN = 0;
            for (int b = 1; b < 3; ++b)
                if (p2.hnr[b] != 0.0f) { tAcc += p2.hnr[b]; ++tN; }
            const float tgtHnr = tN > 0 ? tAcc / (float) tN : kAirHnrClean;
            const float breathy = std::clamp ((kAirHnrClean - tgtHnr)
                                              / (kAirHnrClean - kAirHnrBreathy), 0.0f, 1.0f);
            const float airFromTarget = kAirMax * breathy;

            // ---- (b) the source-vs-target difference, as before ----------
            float airFromDiff = 0.0f;
            if (srcMeasured)
            {
                float acc = 0.0f; int n = 0;
                for (int b = 1; b < 3; ++b)
                    if (p1.hnr[b] > 1.0f || p2.hnr[b] > 1.0f)
                    { acc += p1.hnr[b] - p2.hnr[b]; ++n; }
                if (n > 0) airFromDiff = kAirPerDb * (acc / (float) n);
            }

            r.air = cl (std::max (airFromTarget, airFromDiff), 0.0f, kAirMax);
            // Shine stays a DIFFERENCE: hfDb is a ratio taken within one
            // recording (energy above 6 kHz against the speech band), so it
            // survives a change of level and gain in a way absolute HNR does
            // not, and comparing two of them is fair. It still needs both
            // sides, and is left alone when the source carries no texture.
            r.airshine = srcMeasured ? cl (kShinePerDb * (p2.hfDb - p1.hfDb), 0.0f, kShineMax)
                                     : 0.0f;
            r.push ("air",      r.air);
            if (srcMeasured) r.push ("airshine", r.airshine);
            r.airApplied = true;
        }
    }
    r.push ("hifreq",     r.hifreq);
    r.push ("hipitch",    50.0f);
    r.push ("hiformant",  100.0f);
    r.push ("pitchfloor", r.pitchfloor);

    // ---- AEIOU map: the per-vowel colouring the global shift cannot carry -
    if (r.vowelsMatched > 0)
    {
        // ids are compile-time constants held in a static table so
        // Change::id stays a plain const char* with static lifetime.
        static const char* fid[5][3] = {
            { "va_a_f1", "va_a_f2", "va_a_f3" }, { "va_i_f1", "va_i_f2", "va_i_f3" },
            { "va_u_f1", "va_u_f2", "va_u_f3" }, { "va_e_f1", "va_e_f2", "va_e_f3" },
            { "va_o_f1", "va_o_f2", "va_o_f3" } };
        static const char* gidv[5][3] = {
            { "va_a_g1", "va_a_g2", "va_a_g3" }, { "va_i_g1", "va_i_g2", "va_i_g3" },
            { "va_u_g1", "va_u_g2", "va_u_g3" }, { "va_e_g1", "va_e_g2", "va_e_g3" },
            { "va_o_g1", "va_o_g2", "va_o_g3" } };

        for (int v = 0; v < 5; ++v)
            for (int b3 = 0; b3 < 3; ++b3)
            {
                // vowels that were not measured are flattened to 0 rather
                // than left holding whatever preset was there before, so the
                // map describes THIS match and nothing else.
                r.push (fid[v][b3], r.vowelUsed[v] ? r.vowelShiftSt[v][b3] : 0.0f);

                float g = 0.0f;
                if (r.vowelUsed[v] && p1.vow[v].valid() && p2.vow[v].valid())
                    g = cl (0.5f * (p2.vow[v].L[b3] - p1.vow[v].L[b3]),
                            -kVowelGainClamp, kVowelGainClamp);
                r.push (gidv[v][b3], g);
            }
        r.push ("vcharacter", 8.0f);   // Custom — index 8 of the choice list
        r.push ("vadapt",     1.0f);
        r.push ("vamount",  100.0f);   // apply the measured map as written
    }
    return r;
}

template <class ParamGetter>
MatchingEngine::Proposal
MatchingEngine::refine (const VoiceProfile& p2,
                        const VoiceProfile& pc,
                        ParamGetter get)
{
    Proposal r;
    if (! p2.valid() || ! pc.valid()) return r;

    r.pitch = cl (st (p2.f0Hz, pc.f0Hz) - kPitchBias, -6.0f, 6.0f);
    r.push ("pitch", cl (get ("pitch") + 0.8f * r.pitch, -24.0f, 24.0f));

    float sh[3];
    for (int i = 0; i < 3; ++i) sh[i] = st (p2.F[i], pc.F[i]);
    r.formant = cl ((sh[0] + sh[1] + sh[2]) / 3.0f, -6.0f, 6.0f);
    r.push ("formant", cl (get ("formant") + 0.7f * r.formant, -24.0f, 24.0f));

    const char* sid[3] = { "f1shift", "f2shift", "f3shift" };
    const char* gid[3] = { "f1gain",  "f2gain",  "f3gain"  };
    for (int i = 0; i < 3; ++i)
    {
        r.push (sid[i], cl (get (sid[i]) + 0.4f * (sh[i] - r.formant), -3.0f, 3.0f));
        r.push (gid[i], cl (get (gid[i]) + 0.4f * (p2.L[i] - pc.L[i]),  -8.0f, 8.0f));
    }
    const float dTilt = 0.25f * (p2.tiltDb - pc.tiltDb);
    r.tilt = cl (dTilt, -1.5f, 1.5f);
    r.push ("tilt", cl (get ("tilt") + r.tilt, -6.0f, 6.0f));
    if (p2.f0SpreadSt > 0.3f && pc.f0SpreadSt > 0.3f)
    {
        const float ratio = cl (kRangeBoost * p2.f0SpreadSt / pc.f0SpreadSt,
                                0.75f, 1.35f);
        r.range = cl (get ("range") * ratio, 50.0f, 200.0f);
        r.push ("range", r.range);
        r.rangeApplied = true;
    }
    return r;
}

template <class ParamGetter>
VoiceProfile
MatchingEngine::predictEstimated (const VoiceProfile& p1, ParamGetter get)
{
    VoiceProfile e = p1;
    if (! p1.valid()) return e;

    const float pitchDelta = get ("pitch");
    const float fmtDelta   = get ("formant");
    e.f0Hz = cl (p1.f0Hz * std::pow (2.0f, pitchDelta / 12.0f), 40.0f, 800.0f);

    const char* sid[3] = { "f1shift", "f2shift", "f3shift" };
    const char* gid[3] = { "f1gain",  "f2gain",  "f3gain"  };
    for (int i = 0; i < 3; ++i)
    {
        e.F[i] = cl (p1.F[i] * std::pow (2.0f, (fmtDelta + get (sid[i])) / 12.0f),
                     80.0f, 6000.0f);
        e.L[i] = cl (p1.L[i] + get (gid[i]), -60.0f, 12.0f);
    }
    // range knob is a percentage of the intonation spread; center pulls it
    // toward the pivot but doesn't change spread itself
    const float rangeRatio = std::max (0.01f, get ("range") * 0.01f);
    e.f0SpreadSt = cl (p1.f0SpreadSt * rangeRatio, 0.0f, 24.0f);
    e.tiltDb     = cl (p1.tiltDb + get ("tilt"),   -12.0f, 12.0f);

    // ---- texture (v0.40.0) ----------------------------------------------
    // hnr is INHERITED rather than modelled. Natural Air preserves the
    // source's own breath through the pitch shift instead of synthesizing
    // new noise, so the converted voice's harmonic-to-noise ratio is
    // essentially the source's; inventing a shift here would be a guess
    // dressed as a measurement.
    //
    // hfDb IS adjusted, because Air Shine is a plain gain on the >6 kHz
    // noise component and its effect was measured: +6 dB of Shine moved the
    // 6-16 kHz band ratio 0.0176 -> 0.0291, i.e. +2.2 dB, so about 0.37 dB
    // of band ratio per dB of Shine. Without this a character saved with
    // Shine applied would read back darker than it sounds, and matching to
    // it later would under-ask for Shine by the same amount.
    e.hfDb = cl (p1.hfDb + 0.37f * get ("airshine"), -60.0f, 0.0f);

    // v0.39.0: carry the per-vowel table through the same transform.
    //
    // The graph only ever read the global fields, so this was left alone --
    // but NEW CHARACTER saves this profile to disk, and a profile whose
    // global F has moved while vow[].F has not is internally inconsistent:
    // load it back as a Target and the vowel-matched path (which reads
    // vow[]) would compare against untransformed formants and disagree with
    // the global figures by exactly the shift being applied.
    //
    // The AEIOU map is included because it is part of what the parameters do
    // to each vowel. vamount scales it, and vadapt gates it, the same way
    // VowelAdaptiveWarp applies it in the engine.
    static const char* vsid[5][3] = {
        { "va_a_f1", "va_a_f2", "va_a_f3" }, { "va_i_f1", "va_i_f2", "va_i_f3" },
        { "va_u_f1", "va_u_f2", "va_u_f3" }, { "va_e_f1", "va_e_f2", "va_e_f3" },
        { "va_o_f1", "va_o_f2", "va_o_f3" } };
    static const char* vgid[5][3] = {
        { "va_a_g1", "va_a_g2", "va_a_g3" }, { "va_i_g1", "va_i_g2", "va_i_g3" },
        { "va_u_g1", "va_u_g2", "va_u_g3" }, { "va_e_g1", "va_e_g2", "va_e_g3" },
        { "va_o_g1", "va_o_g2", "va_o_g3" } };
    const float vaOn  = get ("vadapt") > 0.5f ? 1.0f : 0.0f;
    const float vaAmt = vaOn * std::max (0.0f, get ("vamount")) * 0.01f;
    for (int v = 0; v < 5; ++v)
    {
        auto& q = e.vow[v];
        if (! q.valid()) continue;
        q.f0Hz = cl (q.f0Hz * std::pow (2.0f, pitchDelta / 12.0f), 40.0f, 800.0f);
        for (int b = 0; b < 3; ++b)
        {
            if (! (q.F[b] > 0.0f)) continue;
            const float st12 = fmtDelta + get (sid[b]) + vaAmt * get (vsid[v][b]);
            q.F[b] = cl (q.F[b] * std::pow (2.0f, st12 / 12.0f), 80.0f, 6000.0f);
            q.L[b] = cl (q.L[b] + get (gid[b]) + vaAmt * get (vgid[v][b]), -60.0f, 12.0f);
        }
    }
    return e;
}
