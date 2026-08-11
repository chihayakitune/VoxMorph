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
//   * REMEASURED TWICE since v0.37.0, so do not compare any two of these
//     vintages as if they described the same thing. v0.37.1 fixed F1: the
//     original cut reported 342-601 Hz with rel = 0.00 throughout, several
//     BELOW the speaker's own fundamental, which was not an uncertain
//     estimate but the flat extrapolation under the first harmonic being
//     reported as a resonance; F1 is now 774-880 Hz, ordinary open-vowel
//     values for a small tract. v0.37.2 fixed F2 and F3: the three bands
//     used to be allowed to pick the SAME envelope peak, which on these
//     voices happened in 30-46 % of frames, and the reported F2 was then
//     partly F1. F2 was 1284-1643 Hz and is now 1972-2033; F3 was 2658-3251
//     and is now 3586-3876.
//   * That the four now agree closely on F2 (1972-2033 Hz) and F3
//     (3586-3876) is a sanity check passing, not a suspicious coincidence:
//     they are four voices of very similar type and size, so they SHOULD
//     land near one another. Under the old assignment they spread over
//     1284-1643 Hz, and the odd one out (Sara, the highest-pitched, whose
//     frames duplicated most often) asked for a formant shift of +0.48 st
//     where the others asked +4.19 and +4.98 -- the symptom that started
//     this. She now asks +6.81, in line with the rest.
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

        // ---- measured character targets (v0.37.0, remeasured v0.37.2) ------
        // Generated verbatim by test/profile_dump.cpp --cxx; see the file
        // header for how far to trust each band. Regenerate, never hand-edit:
        //   profile_dump <recording>.wav --cxx <id> <En> <Jp>
        //
        // The four differ in ways Auto-Set reacts to: Soshi is 3.8 st lower
        // than Sara and the only one whose F1 is solidly measurable
        // (rel 0.45 against Sara's 0.02); Kura carries the widest intonation
        // (5.63 st against Soshi's 4.25); Sara is the brightest (hfDb -24.86,
        // i.e. ~5.7 dB more above 6 kHz than Kura) and has the least
        // low-frequency weight (tilt 14.72 against Uru's 20.45). Their F2/F3
        // now agree within about 0.3 / 1.4 st, which is what four voices of
        // this type should look like.
        { "uru", "Uru", "うる",
          { /* f0     */ 291.93f,
            /* spread */ 4.08f,
            /* F      */ { 879.9f, 2027.4f, 3875.6f },
            /* L      */ { 0.00f, -13.31f, -21.38f },
            /* tilt   */ 20.45f,
            /* frames */ 3018,
            /* rel    */ { 0.25f, 1.00f, 0.66f },
            /* hnr    */ { 22.18f, 9.54f, 4.06f },
            /* hfDb   */ -28.68f,
            /* tract  */ 1.210f,
            /* vow    */ {
              /* A */ { 1539, 280.99f, { 934.1f, 1887.6f, 3835.9f }, { 0.00f, -8.67f, -19.78f }, { 0.54f, 1.00f, 0.60f }, { 20.75f, 8.87f, 3.75f } },
              /* I */ { 16, 247.54f, { 676.9f, 2999.1f, 3782.6f }, { 0.00f, -24.32f, -27.96f }, { 0.00f, 0.73f, 0.30f }, { 16.17f, 4.46f, 2.42f } },
              /* U */ { 876, 317.04f, { 791.3f, 2299.0f, 3851.9f }, { 0.00f, -18.99f, -24.24f }, { 0.00f, 1.00f, 0.77f }, { 25.42f, 10.99f, 4.68f } },
              /* E */ { 333, 285.68f, { 858.0f, 2776.7f, 3944.9f }, { 0.00f, -12.09f, -14.71f }, { 0.22f, 1.00f, 0.68f }, { 23.50f, 9.30f, 4.11f } },
              /* O */ { 254, 276.34f, { 704.9f, 1494.0f, 4033.4f }, { 0.00f, -9.06f, -27.01f }, { 0.00f, 0.98f, 0.85f }, { 21.24f, 9.40f, 5.12f } }
            } }
        },
        { "kura", "Kura", "くら",
          { /* f0     */ 278.47f,
            /* spread */ 5.63f,
            /* F      */ { 774.2f, 1986.3f, 3826.4f },
            /* L      */ { 0.00f, -15.77f, -25.52f },
            /* tilt   */ 18.56f,
            /* frames */ 2168,
            /* rel    */ { 0.13f, 1.00f, 0.41f },
            /* hnr    */ { 16.90f, 8.77f, 2.61f },
            /* hfDb   */ -30.54f,
            /* tract  */ 1.198f,
            /* vow    */ {
              /* A */ { 1013, 277.72f, { 812.5f, 1861.3f, 3729.4f }, { 0.00f, -14.54f, -25.06f }, { 0.20f, 1.00f, 0.43f }, { 16.09f, 8.63f, 2.68f } },
              /* I */ { 11, 279.80f, { 754.9f, 3048.0f, 3932.9f }, { -5.86f, 0.00f, -1.42f }, { 0.00f, 1.00f, 0.41f }, { 21.03f, 7.46f, 2.64f } },
              /* U */ { 676, 309.89f, { 745.9f, 2252.8f, 3817.1f }, { 0.00f, -19.82f, -25.22f }, { 0.00f, 1.00f, 0.46f }, { 18.25f, 8.77f, 2.83f } },
              /* E */ { 198, 272.11f, { 840.8f, 2649.6f, 3882.5f }, { 0.00f, -18.45f, -20.37f }, { 0.25f, 1.00f, 0.30f }, { 18.95f, 6.26f, 1.68f } },
              /* O */ { 270, 248.37f, { 684.7f, 1483.0f, 4068.7f }, { 0.00f, -8.00f, -29.40f }, { 0.17f, 1.00f, 0.37f }, { 16.20f, 11.22f, 2.41f } }
            } }
        },
        { "sara", "Sara", "さら",
          { /* f0     */ 317.59f,
            /* spread */ 4.76f,
            /* F      */ { 845.2f, 1972.5f, 3635.5f },
            /* L      */ { 0.00f, -13.40f, -23.98f },
            /* tilt   */ 14.72f,
            /* frames */ 866,
            /* rel    */ { 0.02f, 0.98f, 0.46f },
            /* hnr    */ { 17.85f, 6.55f, 2.89f },
            /* hfDb   */ -24.86f,
            /* tract  */ 1.178f,
            /* vow    */ {
              /* A */ { 425, 292.80f, { 889.1f, 1824.5f, 3599.2f }, { 0.00f, -12.79f, -23.83f }, { 0.27f, 0.84f, 0.37f }, { 17.36f, 5.41f, 2.43f } },
              /* I */ { 2, 0.00f, { 0.0f, 0.0f, 0.0f }, { 0.00f, 0.00f, 0.00f }, { 0.00f, 0.00f, 0.00f }, { 0.00f, 0.00f, 0.00f } },
              /* U */ { 332, 357.67f, { 853.8f, 2179.0f, 3649.3f }, { 0.00f, -14.31f, -24.52f }, { 0.00f, 1.00f, 0.57f }, { 20.58f, 9.07f, 3.49f } },
              /* E */ { 45, 337.38f, { 786.8f, 2539.6f, 3650.6f }, { 0.00f, -12.64f, -20.03f }, { 0.00f, 0.70f, 0.30f }, { 14.84f, 4.28f, 1.72f } },
              /* O */ { 62, 284.56f, { 593.8f, 1250.7f, 3671.1f }, { 0.00f, -10.93f, -19.19f }, { 0.00f, 0.96f, 0.68f }, { 4.39f, 9.08f, 4.17f } }
            } }
        },
        { "soshi", "Soshi", "そし",
          { /* f0     */ 252.04f,
            /* spread */ 4.25f,
            /* F      */ { 871.8f, 2033.4f, 3585.5f },
            /* L      */ { 0.00f, -8.13f, -15.39f },
            /* tilt   */ 19.08f,
            /* frames */ 1235,
            /* rel    */ { 0.45f, 1.00f, 0.30f },
            /* hnr    */ { 17.11f, 6.45f, 1.44f },
            /* hfDb   */ -27.43f,
            /* tract  */ 1.184f,
            /* vow    */ {
              /* A */ { 638, 245.29f, { 925.4f, 1902.2f, 3596.6f }, { 0.00f, -6.02f, -14.46f }, { 0.72f, 1.00f, 0.30f }, { 16.46f, 6.45f, 1.50f } },
              /* I */ { 24, 238.31f, { 954.9f, 2839.6f, 3627.1f }, { 0.00f, -5.47f, -7.01f }, { 0.00f, 0.51f, 0.30f }, { 11.59f, 3.21f, 0.59f } },
              /* U */ { 329, 268.95f, { 714.5f, 2083.2f, 3470.0f }, { 0.00f, -18.86f, -25.68f }, { 0.05f, 1.00f, 0.30f }, { 18.27f, 7.09f, 1.65f } },
              /* E */ { 177, 238.04f, { 804.9f, 2537.1f, 3618.8f }, { 0.00f, -6.74f, -9.11f }, { 0.39f, 0.78f, 0.30f }, { 16.31f, 4.76f, 0.53f } },
              /* O */ { 67, 257.06f, { 808.1f, 1535.6f, 4073.6f }, { 0.00f, -11.53f, -29.90f }, { 0.25f, 1.00f, 0.48f }, { 21.25f, 11.81f, 3.02f } }
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
