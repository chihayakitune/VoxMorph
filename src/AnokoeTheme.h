#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include <BinaryData.h>

// ===========================================================================
// ANOKOE theme (v0.31.0)
//
// Colours and art for the skinned UI. Every value here comes from the HTML
// prototype in ANOKOE_UI_Offline (styles.css / script.js), so if the design
// changes, this file and the widgets in AnokoeWidgets.h are the only places
// that need touching — the parameter rows themselves are layout-only.
// ===========================================================================
namespace anokoe
{
    // ---- palette (styles.css) ---------------------------------------------
    const juce::Colour heading   { 0xff778dbc };   // --heading: section titles
    const juce::Colour ink       { 0xff202332 };   // --ink: body text
    const juce::Colour line      { 0xffdce3f1 };   // --line: card borders
    const juce::Colour cardFill  { 0xc7ffffff };   // translucent white card
    const juce::Colour headerFill{ 0xc9ffffff };
    const juce::Colour pageTop   { 0xffe9edfb };   // page background gradient
    const juce::Colour pageMid   { 0xfffaf8fd };
    const juce::Colour pageBot   { 0xffe9f2ff };
    const juce::Colour sidebarA  { 0xffe4ebfc };
    const juce::Colour sidebarB  { 0xfff8faff };
    const juce::Colour sidebarInk{ 0xff596fa5 };
    const juce::Colour sidebarSel{ 0xff7999db };
    const juce::Colour actionInk { 0xff506697 };
    const juce::Colour valueLine { 0xffdce2ef };
    const juce::Colour valueFill { 0xfffafbff };
    const juce::Colour ctrlInk   { 0xff5c72a7 };
    const juce::Colour presetFill{ 0xfff0f3fa };
    const juce::Colour pluginFill{ 0xffe8ecf8 };
    const juce::Colour onFill    { 0xfffff1ca };   // Monitor lit
    const juce::Colour onLine    { 0xffefcb77 };
    const juce::Colour muteFill  { 0xffffe1ea };   // Mute lit
    const juce::Colour muteLine  { 0xffeeadc2 };
    const juce::Colour badge     { 0xff8fc5ed };   // latency LOW/MID/HIGH chip
    const juce::Colour badgeMid  { 0xffefcb77 };
    const juce::Colour badgeHigh { 0xffeeadc2 };
    const juce::Colour advInk    { 0xff6c82b6 };

    // graph series colours (kept from the old skin so the DSP-facing meaning
    // of mint = input / pink = output does not change)
    const juce::Colour seriesIn  { 0xff54bda1 };
    const juce::Colour seriesOut { 0xfff08ba5 };

    // ---- geometry (styles.css) --------------------------------------------
    constexpr float kCardRadius  = 17.0f;
    constexpr int   kCardPadX    = 13;
    constexpr int   kTitleH      = 40;
    constexpr int   kRowH        = 36;    // one slider row
    constexpr int   kToggleH     = 30;
    constexpr int   kKnobH       = 110;   // knob art box
    constexpr int   kKnobRowH    = 124;
    constexpr int   kTrackH      = 22;    // slider track art height
    constexpr int   kValueW      = 54;
    constexpr int   kActionsW    = 40;
    constexpr int   kLabelW      = 150;
    constexpr int   kGap         = 10;
    constexpr int   kSidebarW    = 132;
    constexpr int   kHeaderH     = 86;

    // knob sweep: 7:30 clockwise through 270 deg to 4:30 (script.js uses a
    // 135 deg canvas start and a 2.7 deg/% pointer rotation, same thing)
    inline float knobStart() { return 1.25f * juce::MathConstants<float>::pi; }
    inline float knobEnd()   { return 2.75f * juce::MathConstants<float>::pi; }

    // ---- art -------------------------------------------------------------
    // Images are decoded once and shared. Names are the asset file names with
    // '.' -> '_' (what juce_add_binary_data produces).
    inline juce::Image image (const char* binaryName)
    {
        struct Cache
        {
            juce::HashMap<juce::String, juce::Image> map;
            juce::CriticalSection lock;
        };
        static Cache cache;
        const juce::ScopedLock sl (cache.lock);
        const juce::String key (binaryName);
        if (cache.map.contains (key))
            return cache.map[key];

        juce::Image img;
        int size = 0;
        if (auto* data = BinaryData::getNamedResource (binaryName, size); data != nullptr && size > 0)
            img = juce::ImageFileFormat::loadFrom (data, (size_t) size);
        cache.map.set (key, img);
        return img;
    }

    // Tones select which coloured variant of the knob / slider art is used.
    enum class Tone { blue, pink, yellow };

    inline const char* toneSuffix (Tone t)
    {
        return t == Tone::pink ? "_pink" : t == Tone::yellow ? "_yellow" : "";
    }

    inline juce::Image toned (const char* stem, Tone t)
    {
        return image ((juce::String (stem) + toneSuffix (t) + "_png").toRawUTF8());
    }

    inline juce::Colour toneInk (Tone t)
    {
        return t == Tone::pink   ? juce::Colour (0xffe07a99)
             : t == Tone::yellow ? juce::Colour (0xffd9a441)
                                 : juce::Colour (0xff7999db);
    }

    // ---- text ------------------------------------------------------------
    inline juce::Font font (float h, bool bold = false)
    {
        return juce::Font (juce::FontOptions (h, bold ? juce::Font::bold : juce::Font::plain));
    }

    // Draws an image scaled to fit `area` while keeping its aspect ratio.
    inline void drawFitted (juce::Graphics& g, const juce::Image& img,
                            juce::Rectangle<float> area, float alpha = 1.0f)
    {
        if (! img.isValid() || area.isEmpty()) return;
        g.setOpacity (alpha);
        g.drawImage (img, area, juce::RectanglePlacement::centred, false);
        g.setOpacity (1.0f);
    }

    // The page background gradient used behind everything.
    inline void paintPage (juce::Graphics& g, juce::Rectangle<int> b)
    {
        juce::ColourGradient grad (pageTop, (float) b.getX(), (float) b.getY(),
                                   pageBot, (float) b.getRight(), (float) b.getBottom(), false);
        grad.addColour (0.52, pageMid);
        g.setGradientFill (grad);
        g.fillRect (b);
    }

    // A card panel: translucent white, hairline border, top inset highlight.
    inline void paintCard (juce::Graphics& g, juce::Rectangle<float> b,
                           juce::Colour fill = cardFill)
    {
        g.setColour (fill);
        g.fillRoundedRectangle (b, kCardRadius);
        g.setColour (juce::Colours::white.withAlpha (0.9f));
        g.drawLine (b.getX() + kCardRadius, b.getY() + 1.0f,
                    b.getRight() - kCardRadius, b.getY() + 1.0f, 1.0f);
        g.setColour (line);
        g.drawRoundedRectangle (b.reduced (0.5f), kCardRadius, 1.0f);
    }
}
