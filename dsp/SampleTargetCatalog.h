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
//   * rel[0] is 0.00 on all four. These are high voices (f0 258-368 Hz), so
//     F1 sits under 2.5 x f0 and its POSITION is not recoverable from the
//     recording -- see the identifiability discussion in VoiceAnalyzer.h.
//     The F[0] / vow[].F[0] figures are kept as measured (several land below
//     the speaker's own f0, which is the documented signature of exactly this
//     condition) because rel is what the matcher reads, and at 0.00 the band
//     contributes nothing and F1 moves with the global formant instead. They
//     are NOT clamped, substituted or "fixed up": a plausible-looking F1
//     written in by hand would be believed by everything downstream that
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

        // ---- measured character targets (v0.37.0) -------------------------
        // Generated verbatim by test/profile_dump.cpp --cxx; see the file
        // header for why rel[0] is 0.00 and why F1 is left as measured.
        // Regenerate rather than edit by hand:
        //   profile_dump <recording>.wav --cxx <id> <En> <Jp>
        //
        // The four differ in ways Auto-Set reacts to: Soshi is a full
        // 5.4 st lower than Sara and the only one whose /a/ F1 becomes
        // measurable (rel 0.59); Kura carries the widest intonation
        // (5.99 st vs Soshi's 4.52) and the highest F3; Sara is the
        // brightest (hfDb -25.60, i.e. ~4 dB more above 6 kHz than Kura).
        { "uru", "Uru", "うる",
          { /* f0     */ 306.42f,
            /* spread */ 4.73f,
            /* F      */ { 382.5f, 1708.5f, 3041.2f },
            /* L      */ { 0.00f, -18.37f, -25.42f },
            /* tilt   */ 20.21f,
            /* frames */ 3820,
            /* rel    */ { 0.00f, 1.00f, 0.70f },
            /* hnr    */ { 23.88f, 10.66f, 4.29f },
            /* hfDb   */ -29.12f,
            /* tract  */ 1.077f,
            /* vow    */ {
              /* A */ { 1162, 262.39f, { 759.1f, 1346.7f, 2969.0f }, { 0.00f, -6.21f, -18.59f }, { 0.30f, 0.80f, 0.55f }, { 18.11f, 7.55f, 3.45f } },
              /* I */ { 314, 341.33f, { 229.1f, 2753.0f, 3242.9f }, { 0.00f, -27.22f, -26.13f }, { 0.00f, 1.00f, 0.90f }, { 31.72f, 13.39f, 5.41f } },
              /* U */ { 936, 321.24f, { 311.2f, 1719.4f, 2871.1f }, { 0.00f, -18.21f, -26.95f }, { 0.00f, 1.00f, 0.77f }, { 24.19f, 11.79f, 4.68f } },
              /* E */ { 947, 346.18f, { 259.6f, 2232.8f, 3074.0f }, { 0.00f, -25.90f, -26.37f }, { 0.00f, 1.00f, 0.74f }, { 28.36f, 11.92f, 4.52f } },
              /* O */ { 461, 291.27f, { 558.9f, 1139.1f, 3215.5f }, { 0.00f, -8.44f, -29.38f }, { 0.00f, 0.50f, 0.97f }, { 22.52f, 11.27f, 5.83f } }
            } }
        },
        { "kura", "Kura", "くら",
          { /* f0     */ 304.88f,
            /* spread */ 5.99f,
            /* F      */ { 542.8f, 1582.4f, 3272.6f },
            /* L      */ { 0.00f, -13.53f, -25.12f },
            /* tilt   */ 17.67f,
            /* frames */ 2717,
            /* rel    */ { 0.00f, 0.85f, 0.45f },
            /* hnr    */ { 18.34f, 9.45f, 2.87f },
            /* hfDb   */ -30.04f,
            /* tract  */ 1.076f,
            /* vow    */ {
              /* A */ { 738, 268.18f, { 724.3f, 1274.5f, 3368.9f }, { 0.00f, -3.40f, -24.42f }, { 0.00f, 0.68f, 0.41f }, { 15.74f, 9.02f, 2.60f } },
              /* I */ { 237, 346.12f, { 285.3f, 2595.1f, 3050.3f }, { 0.00f, -22.11f, -20.54f }, { 0.00f, 1.00f, 0.34f }, { 24.39f, 7.14f, 2.22f } },
              /* U */ { 695, 319.55f, { 491.1f, 1581.9f, 3269.4f }, { 0.00f, -13.88f, -26.25f }, { 0.00f, 1.00f, 0.55f }, { 18.51f, 11.22f, 3.44f } },
              /* E */ { 694, 319.25f, { 420.2f, 2081.4f, 3127.5f }, { 0.00f, -22.10f, -23.37f }, { 0.00f, 1.00f, 0.43f }, { 19.83f, 8.34f, 2.74f } },
              /* O */ { 353, 278.82f, { 681.1f, 1105.2f, 3485.9f }, { 0.00f, -3.69f, -27.16f }, { 0.00f, 0.37f, 0.51f }, { 18.07f, 12.49f, 3.19f } }
            } }
        },
        { "sara", "Sara", "さら",
          { /* f0     */ 368.45f,
            /* spread */ 4.86f,
            /* F      */ { 341.5f, 1567.2f, 2875.5f },
            /* L      */ { 0.00f, -17.26f, -24.83f },
            /* tilt   */ 16.67f,
            /* frames */ 1510,
            /* rel    */ { 0.00f, 0.79f, 0.57f },
            /* hnr    */ { 22.41f, 9.28f, 3.53f },
            /* hfDb   */ -25.60f,
            /* tract  */ 1.037f,
            /* vow    */ {
              /* A */ { 341, 305.73f, { 566.9f, 1244.0f, 2740.1f }, { 0.00f, -6.78f, -22.05f }, { 0.00f, 0.47f, 0.46f }, { 17.39f, 6.55f, 2.91f } },
              /* I */ { 147, 422.03f, { 215.8f, 2571.8f, 3094.9f }, { 0.00f, -25.39f, -23.78f }, { 0.00f, 1.00f, 0.79f }, { 29.64f, 12.37f, 4.78f } },
              /* U */ { 435, 376.08f, { 363.8f, 1558.7f, 2823.7f }, { 0.00f, -16.90f, -24.25f }, { 0.00f, 0.76f, 0.50f }, { 22.43f, 8.93f, 3.14f } },
              /* E */ { 397, 390.98f, { 249.0f, 2083.1f, 2905.6f }, { 0.00f, -25.63f, -26.53f }, { 0.00f, 1.00f, 0.63f }, { 25.54f, 10.74f, 3.87f } },
              /* O */ { 190, 329.15f, { 554.8f, 1059.4f, 2955.3f }, { 0.00f, -9.31f, -27.86f }, { 0.00f, 0.27f, 0.54f }, { 21.67f, 9.04f, 3.38f } }
            } }
        },
        { "soshi", "Soshi", "そし",
          { /* f0     */ 258.46f,
            /* spread */ 4.52f,
            /* F      */ { 601.4f, 1702.3f, 2877.9f },
            /* L      */ { 0.00f, -8.58f, -14.33f },
            /* tilt   */ 19.06f,
            /* frames */ 1442,
            /* rel    */ { 0.00f, 1.00f, 0.30f },
            /* hnr    */ { 18.05f, 6.74f, 1.52f },
            /* hfDb   */ -28.06f,
            /* tract  */ 1.060f,
            /* vow    */ {
              /* A */ { 501, 243.50f, { 873.2f, 1422.5f, 2990.2f }, { 0.00f, -0.39f, -9.63f }, { 0.59f, 0.95f, 0.30f }, { 16.16f, 6.17f, 1.50f } },
              /* I */ { 35, 285.31f, { 233.4f, 2760.8f, 3176.3f }, { 0.00f, -17.64f, -16.15f }, { 0.00f, 1.00f, 0.38f }, { 27.00f, 8.66f, 2.43f } },
              /* U */ { 332, 279.67f, { 416.3f, 1684.6f, 2963.5f }, { 0.00f, -19.17f, -26.49f }, { 0.00f, 1.00f, 0.30f }, { 19.33f, 7.29f, 1.75f } },
              /* E */ { 356, 271.13f, { 462.1f, 2257.1f, 2747.6f }, { 0.00f, -18.05f, -18.49f }, { 0.00f, 1.00f, 0.30f }, { 18.29f, 6.03f, 1.20f } },
              /* O */ { 218, 236.64f, { 719.9f, 1356.0f, 2873.6f }, { 0.00f, -6.91f, -16.58f }, { 0.37f, 0.91f, 0.30f }, { 18.07f, 7.93f, 1.72f } }
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
