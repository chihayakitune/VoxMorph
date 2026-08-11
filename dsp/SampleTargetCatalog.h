// SampleTargetCatalog.h — built-in Target voice profiles for Matching
// (v0.28.0). Lets Matching work out of the box, without the user having
// to find an audio file first (spec 2.3 / 2.4). These are static
// summary profiles only — no bundled audio, no license concerns.
//
// Values are typical female / neutral / bright-child voice statistics
// (F0, F1..F3, level ratios, tilt, spread), chosen so Auto-Set produces
// distinct, sensible starting points per target. They ARE approximations
// of a class of voices, not any specific person.
//
// Stable ids are exposed so preset metadata (Phase 4) can point at the
// selected target without depending on display strings.
//
// v0.29.0: the five GENERIC entries carry rel = {1,1,1}. Those are idealized
// statistics rather than measurements, and every F sits well above 2 x f0 (the
// point below which a formant stops being observable), so the Matching stage
// is told to trust all three bands. Leaving rel at its zero default would make
// the reliability weighting discard them entirely. They carry NO per-vowel
// table -- there is no recording to measure one from, and inventing one would
// put fabricated numbers into the AEIOU map -- so matching against a generic
// target takes MatchingEngine's global fallback path by design.
//
// v0.37.0: four MEASURED character entries (Uru / Kura / Sara / Soshi). These
// are the opposite case in every respect, and the distinction matters when
// reading the numbers below:
//
//   * Every field is a measurement, produced by running VoiceAnalyzer over
//     the whole of a real recording (84-165 s, 1442-3820 voiced frames) with
//     test/profile_dump.cpp --cxx. Nothing here was chosen to make Auto-Set
//     behave; if a value looks odd it is because the voice is like that.
//   * They carry a full per-vowel table, so matching against them takes the
//     vowel-matched path and writes a real AEIOU map -- which is the whole
//     point of measuring a specific character rather than a voice class.
//   * F1 is now measurable on these voices. REGENERATED in v0.37.1 after
//     the estimator was fixed -- do not compare these numbers with the
//     v0.37.0 ones as if both were measurements of the same thing. The
//     v0.37.0 cut reported F1 = 342-601 Hz with rel = 0.00 across the board,
//     several of them BELOW the speaker's own fundamental; that was not an
//     uncertain estimate but the flat extrapolation under the first harmonic
//     being reported as a resonance (VoiceAnalyzer.h, extractPeaks). With
//     that removed, F1 comes out at 774-880 Hz -- ordinary open-vowel values
//     for a small tract -- and rel rises to 0.02-0.45.
//   * rel[0] still varies a lot between them, and that is the honest signal,
//     not a defect: Soshi (f0 252 Hz) reaches 0.45, Sara (f0 318 Hz) only
//     0.02. The higher the voice, the fewer frames put F1 far enough above
//     f0 to place it. Below MatchingEngine::kMinRel the band simply does not
//     vote and F1 moves with the global formant, which is the right answer
//     for a uniformly scaled vocal tract.
//   * Nothing here is clamped, substituted or "fixed up". A plausible-looking
//     F1 written in by hand would be believed by everything downstream that
//     does not check rel, and would be a fabricated measurement.
//   * hnr / hfDb are populated, so Auto-Set's Air and Air Shine stage runs
//     against these targets (it self-disables against the generic five,
//     whose zeroed texture fields would read as "infinitely breathy").
//
// Still no bundled audio: these are summary statistics, a few hundred bytes
// per character, from which the source recording cannot be reconstructed.
#pragma once
#include <string_view>
#include "VoiceAnalyzer.h"

struct SampleTargetEntry
{
    const char*  id;        // stable, for metadata / logs
    const char*  displayEn; // "Feminine Standard"
    const char*  displayJp; // "自然な女性声"
    VoiceProfile profile;
};

inline const SampleTargetEntry* getSampleTargets (int& count)
{
    // The voicedFrames field only has to pass VoiceProfile::valid()
    // (>= 15 frames + F0 > 40 Hz). Values below are typical mid-range
    // adult female (Standard), softer/lower (Warm), and a bright/young
    // profile (Bright); they cover the space Auto-Set actually reacts to.
    //
    // NOTE on tiltDb: VoiceAnalyzer defines it as
    //     10 * log10(energy 60-1000 Hz / energy 2000-8000 Hz)
    // which is strongly POSITIVE for speech (~+8 on a synthetic vowel,
    // higher on real voices) -- it is not a small +-3 dB "slope". The
    // v0.28.0 values here were written on that wrong scale, so Auto-Set's
    // 0.25 * (target - mine) always railed against its -4 dB clamp and
    // pushed the voice to maximum brightness no matter which target was
    // picked. These are on the measured scale, and only their RELATIVE
    // order matters to matching.
    static const SampleTargetEntry list[] =
    {
        { "feminine_standard", "Feminine Standard", "自然な女性声",
          { /* f0     */ 215.0f,
            /* spread */ 2.4f,
            /* F      */ { 700.0f, 2050.0f, 2800.0f },
            /* L      */ {  0.0f,   -3.5f,   -6.0f },
            /* tilt   */ 13.0f,
            /* frames */ 240,
            /* rel    */ { 1.0f, 1.0f, 1.0f } }
        },
        { "feminine_bright", "Feminine Bright", "明るい女性声",
          { 240.0f, 3.0f,
            { 730.0f, 2200.0f, 3000.0f },
            {  0.0f,  -2.5f,   -5.0f  },
            10.0f, 240, { 1.0f, 1.0f, 1.0f } }
        },
        { "feminine_warm", "Feminine Warm", "柔らかい女性声",
          { 195.0f, 1.9f,
            { 680.0f, 1900.0f, 2650.0f },
            {  0.0f,  -4.0f,   -7.0f  },
            17.0f, 240, { 1.0f, 1.0f, 1.0f } }
        },
        { "youthful", "Youthful", "幼めの声",
          { 260.0f, 3.2f,
            { 760.0f, 2350.0f, 3150.0f },
            {  0.0f,  -2.0f,   -4.5f  },
             9.0f, 240, { 1.0f, 1.0f, 1.0f } }
        },
        { "androgynous", "Androgynous", "中性的な声",
          { 175.0f, 2.1f,
            { 620.0f, 1750.0f, 2550.0f },
            {  0.0f,  -3.0f,   -6.5f  },
            15.0f, 240, { 1.0f, 1.0f, 1.0f } }
        },

        // ---- measured character targets (v0.37.0, remeasured v0.37.1) ------
        // Generated verbatim by test/profile_dump.cpp --cxx; see the file
        // header for how far to trust each band. Regenerate, never hand-edit:
        //   profile_dump <recording>.wav --cxx <id> <En> <Jp>
        //
        // The four differ in ways Auto-Set reacts to: Soshi is 3.8 st lower
        // than Sara and the only one whose F1 is solidly measurable
        // (rel 0.45 against Sara's 0.02); Kura carries the widest intonation
        // (5.63 st against Soshi's 4.25) and by far the highest F3 (3251 Hz);
        // Sara is the brightest (hfDb -24.86, i.e. ~5.7 dB more above 6 kHz
        // than Kura) and the darkest in tilt (14.72 against Uru's 20.45).
        { "uru", "Uru", "うる",
          { /* f0     */ 291.93f,
            /* spread */ 4.08f,
            /* F      */ { 879.9f, 1530.6f, 2979.6f },
            /* L      */ { 0.00f, -9.30f, -17.99f },
            /* tilt   */ 20.45f,
            /* frames */ 3018,
            /* rel    */ { 0.25f, 0.98f, 0.66f },
            /* hnr    */ { 22.18f, 9.54f, 4.06f },
            /* hfDb   */ -28.68f,
            /* tract  */ 1.041f,
            /* vow    */ {
              /* A */ { 380, 308.85f, { 776.0f, 1237.7f, 3133.8f }, { 0.00f, -5.98f, -24.08f }, { 0.00f, 0.50f, 1.00f }, { 21.07f, 10.25f, 6.13f } },
              /* I */ { 320, 317.18f, { 707.1f, 2604.1f, 3101.6f }, { 0.00f, -24.54f, -23.86f }, { 0.00f, 1.00f, 0.68f }, { 29.36f, 10.79f, 4.19f } },
              /* U */ { 679, 307.65f, { 759.3f, 1557.1f, 2838.1f }, { 0.00f, -11.67f, -21.71f }, { 0.00f, 1.00f, 0.73f }, { 22.66f, 10.70f, 4.44f } },
              /* E */ { 799, 299.61f, { 864.7f, 1975.7f, 2961.4f }, { 0.00f, -11.07f, -14.41f }, { 0.18f, 1.00f, 0.60f }, { 23.35f, 9.39f, 3.71f } },
              /* O */ { 840, 264.57f, { 941.6f, 1157.3f, 2979.4f }, { 0.00f, -0.05f, -16.89f }, { 0.64f, 0.78f, 0.61f }, { 19.25f, 8.04f, 3.77f } }
            } }
        },
        { "kura", "Kura", "くら",
          { /* f0     */ 278.47f,
            /* spread */ 5.63f,
            /* F      */ { 774.2f, 1476.9f, 3250.5f },
            /* L      */ { 0.00f, -10.87f, -23.88f },
            /* tilt   */ 18.56f,
            /* frames */ 2168,
            /* rel    */ { 0.13f, 0.85f, 0.41f },
            /* hnr    */ { 16.90f, 8.77f, 2.61f },
            /* hfDb   */ -30.54f,
            /* tract  */ 1.055f,
            /* vow    */ {
              /* A */ { 305, 320.91f, { 806.1f, 1206.6f, 3280.1f }, { 0.00f, -4.72f, -25.92f }, { 0.00f, 0.46f, 0.56f }, { 17.69f, 12.35f, 3.49f } },
              /* I */ { 177, 268.85f, { 607.8f, 2463.8f, 3157.4f }, { 0.00f, -23.29f, -22.66f }, { 0.00f, 1.00f, 0.30f }, { 20.85f, 5.99f, 1.91f } },
              /* U */ { 599, 312.02f, { 695.3f, 1457.8f, 3356.1f }, { 0.00f, -11.74f, -26.62f }, { 0.00f, 0.89f, 0.43f }, { 15.95f, 9.86f, 2.77f } },
              /* E */ { 613, 278.43f, { 724.8f, 1901.1f, 3134.3f }, { 0.00f, -16.51f, -21.48f }, { 0.16f, 1.00f, 0.37f }, { 17.42f, 7.37f, 2.39f } },
              /* O */ { 474, 254.10f, { 862.3f, 1134.9f, 3305.5f }, { 0.00f, 0.00f, -23.95f }, { 0.55f, 0.82f, 0.38f }, { 15.87f, 9.01f, 2.48f } }
            } }
        },
        { "sara", "Sara", "さら",
          { /* f0     */ 317.59f,
            /* spread */ 4.76f,
            /* F      */ { 845.2f, 1283.7f, 2657.5f },
            /* L      */ { 0.00f, -7.20f, -17.47f },
            /* tilt   */ 14.72f,
            /* frames */ 866,
            /* rel    */ { 0.02f, 0.55f, 0.46f },
            /* hnr    */ { 17.85f, 6.55f, 2.89f },
            /* hfDb   */ -24.86f,
            /* tract  */ 0.968f,
            /* vow    */ {
              /* A */ { 126, 284.97f, { 706.4f, 1074.8f, 2529.5f }, { 0.00f, -4.97f, -18.05f }, { 0.00f, 0.29f, 0.99f }, { 20.79f, 9.04f, 5.96f } },
              /* I */ { 75, 329.08f, { 655.8f, 2199.7f, 2864.1f }, { 0.00f, -13.96f, -13.96f }, { 0.00f, 0.79f, 0.47f }, { 16.24f, 4.77f, 3.00f } },
              /* U */ { 279, 337.07f, { 803.3f, 1250.2f, 2619.1f }, { 0.00f, -6.76f, -18.68f }, { 0.00f, 0.45f, 0.56f }, { 17.12f, 8.48f, 3.48f } },
              /* E */ { 251, 327.73f, { 831.7f, 1617.8f, 2731.7f }, { 0.00f, -10.33f, -17.04f }, { 0.04f, 0.86f, 0.34f }, { 17.27f, 6.49f, 2.20f } },
              /* O */ { 135, 287.49f, { 982.4f, 1163.6f, 2580.7f }, { 0.00f, -0.23f, -16.19f }, { 0.43f, 0.59f, 0.30f }, { 17.70f, 5.24f, 1.99f } }
            } }
        },
        { "soshi", "Soshi", "そし",
          { /* f0     */ 252.04f,
            /* spread */ 4.25f,
            /* F      */ { 871.8f, 1642.9f, 2832.9f },
            /* L      */ { 0.00f, -5.60f, -12.01f },
            /* tilt   */ 19.08f,
            /* frames */ 1235,
            /* rel    */ { 0.45f, 0.97f, 0.30f },
            /* hnr    */ { 17.11f, 6.45f, 1.44f },
            /* hfDb   */ -27.43f,
            /* tract  */ 1.046f,
            /* vow    */ {
              /* A */ { 120, 289.09f, { 764.8f, 1360.6f, 3296.8f }, { 0.00f, -13.12f, -27.57f }, { 0.07f, 1.00f, 0.39f }, { 20.48f, 10.83f, 2.50f } },
              /* I */ { 35, 252.97f, { 722.4f, 2622.5f, 3003.5f }, { 0.00f, -2.39f, -1.48f }, { 0.04f, 0.42f, 0.30f }, { 15.22f, 2.67f, 0.59f } },
              /* U */ { 352, 256.72f, { 709.2f, 1619.5f, 2863.9f }, { 0.00f, -15.23f, -22.71f }, { 0.12f, 1.00f, 0.30f }, { 18.27f, 7.52f, 1.53f } },
              /* E */ { 408, 250.67f, { 871.8f, 2077.3f, 2773.2f }, { 0.00f, -3.95f, -5.13f }, { 0.40f, 0.86f, 0.30f }, { 15.70f, 5.20f, 1.04f } },
              /* O */ { 320, 238.32f, { 948.3f, 1254.4f, 2750.2f }, { -0.04f, 0.00f, -11.20f }, { 0.83f, 0.85f, 0.30f }, { 16.17f, 6.35f, 1.75f } }
            } }
        },
    };
    count = (int) (sizeof (list) / sizeof (list[0]));
    return list;
}

// index 0 = default selection at startup
inline int sampleTargetIndexById (const char* id)
{
    int n = 0;
    const auto* list = getSampleTargets (n);
    for (int i = 0; i < n; ++i)
        if (id != nullptr && std::string_view (list[i].id) == std::string_view (id))
            return i;
    return 0;
}
