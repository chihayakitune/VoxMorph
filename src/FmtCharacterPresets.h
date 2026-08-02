// FmtCharacterPresets.h — built-in presets for the "Fmt Character" dropdown
// (v0.36.7). SINGLE source of truth for those values.
//
// Unlike AEIOUCharacterPresets.h this is NOT read by the DSP. A character
// here is just a set of values the editor writes into the seven global
// formant parameters:
//
//     consonant, f1shift, f1gain, f2shift, f2gain, f3shift, f3gain
//
// so picking one is exactly equivalent to moving those seven sliders by
// hand, and moving any of them by hand puts the dropdown back to Custom.
// Nothing is hidden or locked while a character is selected.
//
// The character NAMES deliberately match AEIOUCharacterPresets.h, so the two
// dropdowns read as the same vocabulary. The values do not correspond
// one-to-one: AEIOU shapes the five vowels apart from each other, this
// shifts the whole vocal tract.
//
// Ranges (from createLayout): consonant +-12 st, F1-F3 shift +-6 st,
// F1-F3 gain +-12 dB. Everything below sits well inside those.
#pragma once

struct FmtCharacterMap
{
    float consonantSt;    // extra shift on unvoiced consonants
    float shiftSt[3];     // F1, F2, F3 shift, semitones
    float gainDb[3];      // F1, F2, F3 resonance gain, dB
};

// index order matches the "fcharacter" choice list; Custom (8) has no map
constexpr int kFmtNumCharacters = 9;
constexpr int kFmtCustom        = 8;

inline const FmtCharacterMap& getFmtCharacterMap (int index)
{
    // F2 carries most of the perceived gender/age of the vowels, so it leads
    // in every character; F1 is deliberately raised LESS than F2 (raising
    // them together reads as "chipmunk" rather than as a shorter tract), and
    // its gain comes down slightly to thin out the chest resonance.
    static const FmtCharacterMap maps[kFmtCustom] =
    {
        // natural: 自然な女性寄り(誇張なし)
        { 2.0f, { 1.0f, 2.0f, 1.0f }, { -0.5f, 1.0f, 0.5f } },
        // soft: 丸く穏やか。F2/F3を抑えて明るさを落とす
        { 1.0f, { 0.5f, 0.5f, 0.0f }, {  0.5f, -0.5f, -1.0f } },
        // active: 明るく元気。F2/F3を持ち上げる
        { 3.0f, { 1.0f, 3.0f, 1.5f }, { -0.5f, 1.5f, 1.0f } },
        // loli: 声道を短く。F1を抑えF2/F3を強く上げる
        { 3.0f, { 1.5f, 3.0f, 2.0f }, { -1.0f, 1.5f, 1.5f } },
        // anime: 誇張。実在の声道からは離れる
        { 4.0f, { 2.0f, 4.0f, 2.5f }, { -1.0f, 2.0f, 2.0f } },
        // lily: 澄んだ甘さ。F3の艶を残しつつ控えめ
        { 2.0f, { 0.8f, 2.5f, 1.5f }, { -0.5f, 1.2f, 1.2f } },
        // elegant: 落ち着いた大人。全体に控えめ
        { 1.0f, { 0.5f, 1.5f, 0.5f }, {  0.0f, 0.5f, 0.0f } },
        // uni: 中性。フラット = 何も足さない
        { 0.0f, { 0.0f, 0.0f, 0.0f }, {  0.0f, 0.0f, 0.0f } },
    };
    return maps[juce::jlimit (0, kFmtCustom - 1, index)];
}
