// AEIOUCharacterPresets.h — built-in vowel-character maps for the
// "AEIOU Character" feature (v0.26.0, gains added in v0.27.0). SINGLE
// source of truth for the preset values: the DSP, the UI detail window
// and the Custom parameter defaults all read from here.
//
// A map holds per-vowel {F1, F2, F3} SHIFT offsets in semitones and
// per-vowel {F1, F2, F3} relative resonance GAINS in dB, vowel order
// A, I, U, E, O (matching the VowelAdaptiveWarp anchor order). Shifts
// stay inside the +-2/+-3/+-1.5 st map range, gains inside +-3 dB
// (the runtime caps after Amount are wider — see VowelAdaptiveWarp).
// The presets shape vowel-resonance CHARACTER — they do not reproduce
// any specific person's voice (hence no "Target" anywhere).
//
// Dependency-free, audio-thread safe: enum in, reference to a static
// const array out. No strings, no allocation, no lookup cost.
#pragma once

enum class AEIOUCharacter
{
    natural = 0,   // 自然な女性声 / Natural feminine balance
    soft,          // 柔らかい声   / Soft and rounded
    active,        // 元気な声     / Bright and energetic
    loli,          // 幼な声       / Small and youthful
    anime,         // アニメ声     / Exaggerated vowel contrast
    lily,          // 百合声       / Clear, sweet feminine (v0.26.2)
    elegant,       // お姉さん声   / Calm and refined
    uni,           // 中性声       / Neutral and androgynous
    custom         // 詳細モード   / User-defined A-I-U-E-O map
};

constexpr int kAEIOUNumCharacters = 9;   // including custom

struct AEIOUCharacterMap
{
    float offset[5][3];   // [A,I,U,E,O][F1,F2,F3] shift, semitones
    float gainDb[5][3];   // [A,I,U,E,O][F1,F2,F3] resonance gain, dB
};

inline const AEIOUCharacterMap& getAEIOUCharacterMap (AEIOUCharacter c)
{
    // one immutable table; custom falls back to natural (the DSP never
    // asks for custom — the processor substitutes the APVTS values).
    // Shift values revised in v0.26.2, gains added in v0.27.0 (both from
    // the ChatGPT preset specs) for clear contrast BETWEEN characters.
    static const AEIOUCharacterMap maps[8] =
    {
        // natural: 一般的な女性寄りの母音バランス(誇張なし)
        {{ {  1.20f, 0.55f, 0.10f },     // A  (shift, st)
           {  0.10f, 1.10f, 0.45f },     // I
           {  0.35f, 1.05f, 0.25f },     // U
           {  0.70f, 0.95f, 0.30f },     // E
           {  0.80f, 0.35f, 0.05f } },   // O
         { {  0.8f,  0.3f,  0.0f },      // A  (gain, dB)
           {  0.0f,  0.8f,  0.4f },      // I
           {  0.2f,  0.6f,  0.2f },      // U
           {  0.5f,  0.7f,  0.3f },      // E
           {  0.6f,  0.1f,  0.0f } }},   // O
        // soft: 母音差とF2/F3を抑えて丸く穏やかに
        {{ {  0.55f, -0.25f, -0.65f },
           { -0.35f,  0.15f, -0.75f },
           {  0.05f, -0.20f, -0.70f },
           {  0.15f,  0.05f, -0.70f },
           {  0.35f, -0.45f, -0.80f } },
         { {  1.4f, -1.0f, -2.2f },
           {  0.4f, -0.8f, -2.5f },
           {  0.8f, -1.2f, -2.4f },
           {  0.6f, -0.7f, -2.3f },
           {  1.5f, -1.4f, -2.5f } }},
        // active: 口の開きと前方共鳴を強めて明るく
        {{ {  2.00f, 1.20f, 0.45f },
           {  0.45f, 1.80f, 0.65f },
           {  0.90f, 1.35f, 0.45f },
           {  1.55f, 1.70f, 0.60f },
           {  1.75f, 0.80f, 0.30f } },
         { {  2.8f,  2.0f,  0.8f },
           {  0.8f,  2.8f,  1.4f },
           {  1.6f,  2.0f,  0.8f },
           {  2.2f,  2.7f,  1.2f },
           {  2.6f,  1.5f,  0.5f } }},
        // loli: 短い声道風の高共鳴(通常利用40〜70%想定)
        {{ {  1.10f, 1.85f, 1.10f },
           {  0.25f, 2.75f, 1.45f },
           {  0.50f, 2.35f, 1.30f },
           {  0.80f, 2.65f, 1.45f },
           {  0.85f, 1.60f, 1.00f } },
         { { -1.2f,  2.2f,  2.6f },
           { -2.0f,  3.0f,  3.0f },
           { -1.5f,  2.6f,  2.7f },
           { -1.6f,  3.0f,  3.0f },
           { -0.8f,  1.8f,  2.2f } }},
        // anime: 母音間コントラストを誇張(Loliとの差別化)
        {{ {  1.85f, -0.20f, 0.30f },
           { -0.55f,  3.00f, 1.50f },
           { -0.20f,  1.65f, 0.55f },
           {  0.35f,  2.85f, 1.35f },
           {  1.65f, -0.55f, 0.10f } },
         { {  3.0f, -1.5f,  0.5f },
           { -2.5f,  3.0f,  3.0f },
           { -1.8f,  2.0f,  1.2f },
           {  0.2f,  3.0f,  2.8f },
           {  3.0f, -2.0f,  0.0f } }},
        // lily: 百合声(前方共鳴強めの澄んだ甘い響き)
        {{ {  1.20f, 1.40f, 0.60f },
           {  0.20f, 2.60f, 1.20f },
           {  0.45f, 2.20f, 0.95f },
           {  0.85f, 2.40f, 1.10f },
           {  0.95f, 1.30f, 0.50f } },
         { { -0.3f,  2.0f,  2.1f },
           { -1.6f,  3.0f,  3.0f },
           { -0.8f,  2.7f,  2.5f },
           { -1.0f,  3.0f,  2.9f },
           {  0.2f,  1.7f,  1.8f } }},
        // elegant: F1控えめ、F2/F3で落ち着いた明瞭感
        {{ {  0.25f, -0.20f, 0.45f },
           { -0.65f,  0.55f, 0.75f },
           { -0.45f,  0.20f, 0.55f },
           { -0.20f,  0.45f, 0.70f },
           { -0.10f, -0.45f, 0.35f } },
         { { -0.5f, -0.6f,  1.6f },
           { -1.8f,  0.4f,  2.2f },
           { -1.4f,  0.0f,  1.8f },
           { -1.2f,  0.3f,  2.1f },
           { -0.6f, -1.0f,  1.4f } }},
        // uni: 女性化方向を限定しない均整の取れた微補正
        {{ { -0.35f, -0.35f, -0.15f },
           { -0.65f,  0.10f,  0.05f },
           { -0.45f, -0.20f, -0.15f },
           { -0.50f,  0.05f,  0.00f },
           { -0.25f, -0.55f, -0.20f } },
         { {  0.8f, -0.8f, -0.8f },
           {  0.6f, -1.0f, -0.6f },
           {  0.8f, -0.8f, -0.8f },
           {  0.7f, -0.9f, -0.7f },
           {  1.0f, -1.2f, -0.9f } }},
    };
    const int i = (int) c;
    return maps[(i >= 0 && i < 8) ? i : 0];
}
