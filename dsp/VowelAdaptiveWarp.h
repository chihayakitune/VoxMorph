// VowelAdaptiveWarp.h — vowel-adaptive per-formant offset estimator (Beta)
//
// Estimates a CONTINUOUS vowel coordinate (height, frontness) from the
// engine's tracked F1/F2 (input domain, Hz) and blends a small per-vowel
// offset map (5 anchors ~ /a i u e o/) into F1/F2/F3 semitone offsets that
// the engine ADDS on top of the user's fixed F1/F2/F3 shifts. The goal is
// NOT vowel conversion ("a" stays "a") — it nudges each vowel toward how
// the target speaker would produce that same vowel, since a single global
// F1-F3 setting is a compromise across the whole vowel space.
//
// Deliberately continuous (no hard vowel classification): intermediate
// vowels blend smoothly, transitions never jump, and a misjudged frame can
// only be slightly wrong instead of category-wrong.
//
// Runs at CONTROL RATE (one call per 512-sample detection frame). All
// state is a handful of floats: no allocation, no locks, no exceptions,
// every input validated and every output clamped and finite. C++17,
// dependency-free (same policy as PsolaEngine.h).
#pragma once
#include <cmath>
#include <algorithm>
#include "AEIOUCharacterPresets.h"

class VowelAdaptiveWarp
{
public:
    static constexpr int kAnchors = 5;            // a, i, u, e, o
    // safety caps for the FINAL offsets (semitones). Larger than the map
    // value range (+-2/+-3/+-1.5) so an Amount above 100 % (up to 200 %)
    // has room to actually emphasize the character instead of flattening
    // every vowel onto the same ceiling; still well inside the +-6 st the
    // manual F1-F3 shifts already allow.
    static constexpr float kMaxOff[3] = { 3.0f, 4.5f, 2.25f };
    // map VALUES are still entered/stored in the original safe range
    static constexpr float kMaxMap[3] = { 2.0f, 3.0f, 1.5f };
    // per-vowel resonance gain (v0.27.0): map input range and the final
    // cap after Amount (200 % of the +-3 dB map values, limited to +-4.5)
    static constexpr float kMaxMapGain = 3.0f;   // dB, map values
    static constexpr float kMaxGain    = 4.5f;   // dB, smoothed output

    VowelAdaptiveWarp()
    {
        setMap (getAEIOUCharacterMap (AEIOUCharacter::natural));
    }

    struct Input
    {
        float f1Hz = -1.0f;      // tracked formants in INPUT-domain Hz
        float f2Hz = -1.0f;      // (engine trackF divided by the global
        float f3Hz = -1.0f;      //  formant ratio); <= 0 = not tracked
        float pitchConf = 0.0f;  // YIN clarity 0..1
        bool  voiced = false;
    };

    void prepare (double sampleRate, int controlIntervalSamples)
    {
        const float dt = (float) (controlIntervalSamples / sampleRate);
        aAtk  = 1.0f - std::exp (-dt / 0.035f);   // offset attack   ~35 ms
        aRel  = 1.0f - std::exp (-dt / 0.090f);   // offset release  ~90 ms
        aGone = 1.0f - std::exp (-dt / 0.150f);   // unvoiced fade  ~150 ms
        reset();
    }

    void reset()
    {
        off[0]  = off[1]  = off[2]  = 0.0f;
        offG[0] = offG[1] = offG[2] = 0.0f;
        height = 0.5f;  frontness = 0.5f;  conf = 0.0f;
        motSm = 0.0f;  prevL1 = 0.0f;  prevL2 = 0.0f;  havePrev = false;
    }

    void setAmount (float a)   // 0..2 (100 % = the map as written, 200 % = doubled)
    {
        amount = std::isfinite (a) ? std::clamp (a, 0.0f, 2.0f) : 0.0f;
    }

    // Replace the whole per-vowel map at once (AEIOU Character presets /
    // Custom values, copied at control rate — 30 plain floats, RT-safe).
    // Values are clamped to the safety caps.
    void setMap (const AEIOUCharacterMap& m)
    {
        for (int a = 0; a < kAnchors; ++a)
            for (int i = 0; i < 3; ++i)
            {
                mapOff[a][i] = std::isfinite (m.offset[a][i])
                             ? std::clamp (m.offset[a][i], -kMaxMap[i], kMaxMap[i]) : 0.0f;
                mapGain[a][i] = std::isfinite (m.gainDb[a][i])
                              ? std::clamp (m.gainDb[a][i], -kMaxMapGain, kMaxMapGain) : 0.0f;
            }
    }

    // Per-anchor variant (kept for the planned measured-target flow).
    // Values are clamped to the safety caps above.
    void setAnchorOffsets (int anchor, float f1Semi, float f2Semi, float f3Semi)
    {
        if (anchor < 0 || anchor >= kAnchors) return;
        const float v[3] = { f1Semi, f2Semi, f3Semi };
        for (int i = 0; i < 3; ++i)
            mapOff[anchor][i] = std::isfinite (v[i])
                              ? std::clamp (v[i], -kMaxMap[i], kMaxMap[i]) : 0.0f;
    }

    // one call per control frame; smoothed offsets then read via offsetSemi()
    void process (const Input& in)
    {
        // ---- validity gate: finite, plausible ranges, correct ordering ----
        const bool finiteAll = std::isfinite (in.f1Hz) && std::isfinite (in.f2Hz)
                            && std::isfinite (in.f3Hz);
        const bool valid = finiteAll && in.voiced
                        && in.f1Hz > 150.0f && in.f1Hz < 1100.0f
                        && in.f2Hz > 500.0f && in.f2Hz < 3200.0f
                        && in.f3Hz > 1500.0f && in.f3Hz < 4500.0f
                        && in.f2Hz > 1.18f * in.f1Hz
                        && in.f3Hz > 1.08f * in.f2Hz;

        float gate = 0.0f;
        if (valid && amount > 1.0e-4f)
        {
            // ---- continuous vowel coordinate (log-frequency domain) ----
            // height    ~ openness, mainly log F1
            // frontness ~ tongue advancement, log(F2/F1) (the ratio damps
            //             speaker-size / gender scaling vs absolute Hz)
            const float l1 = std::log2 (in.f1Hz);
            const float l2 = std::log2 (in.f2Hz);
            height    = std::clamp ((l1 - kLoF1) / (kHiF1 - kLoF1), 0.0f, 1.0f);
            frontness = std::clamp (((l2 - l1) - kLoRat) / (kHiRat - kLoRat), 0.0f, 1.0f);

            // ---- stability: fast formant motion = unreliable tracking ----
            const float mot = havePrev
                            ? std::abs (l1 - prevL1) + std::abs (l2 - prevL2)
                            : 0.0f;
            prevL1 = l1;  prevL2 = l2;  havePrev = true;
            motSm += 0.2f * (mot - motSm);       // ~5-frame (50 ms) average
            const float stab = std::clamp (1.0f - motSm / 0.06f, 0.0f, 1.0f);

            // ---- confidence gate (all factors smooth 0..1, no hard edges)
            const float pc = std::clamp ((in.pitchConf - 0.15f) / 0.35f, 0.0f, 1.0f);
            gate = amount * pc * stab;
        }
        else
        {
            havePrev = false;
            motSm = 0.0f;
        }
        conf = gate;

        // ---- map lookup: inverse-square-distance blend over the anchors --
        // (shifts and gains share the same weights and gate)
        float target[3]  = { 0.0f, 0.0f, 0.0f };
        float targetG[3] = { 0.0f, 0.0f, 0.0f };
        if (gate > 0.0f)
        {
            float wSum = 0.0f;
            float acc[3]  = { 0.0f, 0.0f, 0.0f };
            float accG[3] = { 0.0f, 0.0f, 0.0f };
            for (int a = 0; a < kAnchors; ++a)
            {
                const float dh = height    - kAnchorH[a];
                const float df = frontness - kAnchorF[a];
                const float w  = 1.0f / (dh * dh + df * df + 0.02f);
                wSum += w;
                for (int i = 0; i < 3; ++i)
                {
                    acc[i]  += w * mapOff[a][i];
                    accG[i] += w * mapGain[a][i];
                }
            }
            for (int i = 0; i < 3; ++i)
            {
                target[i]  = std::clamp (gate * acc[i]  / wSum,
                                         -kMaxOff[i], kMaxOff[i]);
                targetG[i] = std::clamp (gate * accG[i] / wSum,
                                         -kMaxGain, kMaxGain);
            }
        }

        // ---- per-formant attack/release smoothing toward the targets -----
        for (int i = 0; i < 3; ++i)
        {
            const float a = gate <= 0.0f ? aGone
                          : (std::abs (target[i]) > std::abs (off[i]) ? aAtk : aRel);
            off[i] += a * (target[i] - off[i]);
            if (! std::isfinite (off[i])) off[i] = 0.0f;
            off[i] = std::clamp (off[i], -kMaxOff[i], kMaxOff[i]);

            const float ag = gate <= 0.0f ? aGone
                           : (std::abs (targetG[i]) > std::abs (offG[i]) ? aAtk : aRel);
            offG[i] += ag * (targetG[i] - offG[i]);
            if (! std::isfinite (offG[i])) offG[i] = 0.0f;
            offG[i] = std::clamp (offG[i], -kMaxGain, kMaxGain);
        }
    }

    float offsetSemi (int i) const { return off[std::clamp (i, 0, 2)]; }
    float gainDb (int i)     const { return offG[std::clamp (i, 0, 2)]; }
    float vowelHeight()      const { return height; }
    float vowelFrontness()   const { return frontness; }
    float confidence()       const { return conf; }

private:
    // coordinate normalization ranges (log2 Hz / log2 ratio). Fixed for the
    // first prototype; clamped, so out-of-range voices saturate gracefully.
    static inline const float kLoF1  = std::log2 (280.0f);
    static inline const float kHiF1  = std::log2 (950.0f);
    static constexpr float kLoRat = 0.55f;   // log2(F2/F1): /a,o/ side
    static constexpr float kHiRat = 2.90f;   // /i/ side

    // anchor coordinates: typical Japanese vowel formants pushed through the
    // same normalization ((h, f) of F1/F2 = a:800/1250 i:300/2350 u:350/1300
    // e:480/2100 o:500/900 Hz)
    static constexpr float kAnchorH[kAnchors] = { 0.86f, 0.06f, 0.18f, 0.44f, 0.47f };
    static constexpr float kAnchorF[kAnchors] = { 0.04f, 1.00f, 0.57f, 0.67f, 0.13f };

    // active anchor offsets (semitones, {F1, F2, F3} per vowel a,i,u,e,o).
    // Set through setMap() — the AEIOU Character presets or the user's
    // Custom values (see dsp/AEIOUCharacterPresets.h); the constructor
    // starts on the Natural map.
    float mapOff[kAnchors][3]  = {};
    float mapGain[kAnchors][3] = {};   // per-vowel resonance gains (dB)

    float amount = 0.0f;
    float aAtk = 0.3f, aRel = 0.12f, aGone = 0.07f;
    float off[3]  = { 0.0f, 0.0f, 0.0f };
    float offG[3] = { 0.0f, 0.0f, 0.0f };   // smoothed gains (dB)
    float height = 0.5f, frontness = 0.5f, conf = 0.0f;
    float motSm = 0.0f, prevL1 = 0.0f, prevL2 = 0.0f;
    bool  havePrev = false;
};
