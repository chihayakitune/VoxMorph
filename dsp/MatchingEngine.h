// MatchingEngine.h — Auto-Set / Refine calculation for the Matching tab
// (v0.28.0). The formulas are the SAME ones that lived inside AnalyzePanel
// in v0.27.0 (kPitchBias, kRangeBoost, per-formant halves, tilt 0.25x,
// range 0.75..1.35 damping, jlimit ranges); this header exists so the
// panel no longer computes anything itself — it just receives the
// resulting per-parameter (id, value) proposal.
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
    // 16 is enough for both Auto-Set (13) and Refine (10). No allocation.
    static constexpr int kMax = 16;

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
        bool    rangeApplied = false;

        void push (const char* id, float value, bool apply = true)
        {
            if (count >= kMax) return;
            changes[count++] = { id, value, apply };
        }
    };

    // Absolute Auto-Set: derive a full parameter set from the difference
    // between the user's own voice profile and the target profile. This is
    // the v0.27.0 formula, unchanged (byte-exact same jlimit ranges and
    // bias constants). The panel wraps every push in history.group and
    // isParamLocked-aware setValueNotifyingHost.
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
    static constexpr float kRangeBoost = 1.15f;   // intonation compensation

private:
    static float st (float a, float b) { return 12.0f * std::log2 (a / b); }
    static float cl (float v, float lo, float hi)
    {
        if (! std::isfinite (v)) return 0.0f;
        return std::clamp (v, lo, hi);
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

    r.pitch = cl (st (p2.f0Hz, p1.f0Hz) - kPitchBias, -24.0f, 24.0f);

    float sh[3];
    for (int i = 0; i < 3; ++i) sh[i] = st (p2.F[i], p1.F[i]);
    r.formant = cl ((sh[0] + sh[1] + sh[2]) / 3.0f, -24.0f, 24.0f);
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
        r.push (sid[i], cl (0.5f * (sh[i] - r.formant), -3.0f, 3.0f));
        r.push (gid[i], cl (0.5f * (p2.L[i] - p1.L[i]), -8.0f, 8.0f));
    }
    r.push ("tilt", r.tilt);
    if (p1.f0SpreadSt > 0.3f && p2.f0SpreadSt > 0.3f)
    {
        r.range  = cl (kRangeBoost * 100.0f * p2.f0SpreadSt / p1.f0SpreadSt,
                       50.0f, 200.0f);
        r.center = cl (p2.f0Hz, 80.0f, 400.0f);
        r.push ("range",  r.range);
        r.push ("center", r.center);
        r.rangeApplied = true;
    }
    r.push ("hifreq",     r.hifreq);
    r.push ("hipitch",    50.0f);
    r.push ("hiformant",  100.0f);
    r.push ("pitchfloor", r.pitchfloor);
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
    return e;
}
