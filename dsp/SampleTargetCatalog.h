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
    static const SampleTargetEntry list[] =
    {
        { "feminine_standard", "Feminine Standard", "自然な女性声",
          { /* f0     */ 215.0f,
            /* spread */ 2.4f,
            /* F      */ { 700.0f, 2050.0f, 2800.0f },
            /* L      */ {  0.0f,   -3.5f,   -6.0f },
            /* tilt   */ -1.5f,
            /* frames */ 240 }
        },
        { "feminine_bright", "Feminine Bright", "明るい女性声",
          { 240.0f, 3.0f,
            { 730.0f, 2200.0f, 3000.0f },
            {  0.0f,  -2.5f,   -5.0f  },
            -0.5f, 240 }
        },
        { "feminine_warm", "Feminine Warm", "柔らかい女性声",
          { 195.0f, 1.9f,
            { 680.0f, 1900.0f, 2650.0f },
            {  0.0f,  -4.0f,   -7.0f  },
            -3.0f, 240 }
        },
        { "youthful", "Youthful", "幼めの声",
          { 260.0f, 3.2f,
            { 760.0f, 2350.0f, 3150.0f },
            {  0.0f,  -2.0f,   -4.5f  },
             0.5f, 240 }
        },
        { "androgynous", "Androgynous", "中性的な声",
          { 175.0f, 2.1f,
            { 620.0f, 1750.0f, 2550.0f },
            {  0.0f,  -3.0f,   -6.5f  },
            -2.0f, 240 }
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
