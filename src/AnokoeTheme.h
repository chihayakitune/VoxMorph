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

    // ---- hero band (v0.32.0): the dark strip under the header that the
    // character circle sits in
    const juce::Colour bandTop  { 0xff262b45 };
    const juce::Colour bandBot  { 0xff141726 };
    const juce::Colour bandGrid { 0x14ffffff };   // faint diamond lattice
    const juce::Colour bandStar { 0x59ffffff };
    const juce::Colour bandInk  { 0xffe6ebff };
    const juce::Colour bandDim  { 0xff8f9bc4 };

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
    constexpr int   kHeaderH     = 62;    // v0.32.0: slimmer, the band carries the art
    constexpr int   kBandH       = 158;   // dark hero band
    constexpr int   kTabH        = 58;    // page tabs along the band's bottom edge
    constexpr int   kHeroR       = 100;   // character portrait radius
    constexpr int   kHeroRim     = 24;    // how far the band bulges past the
                                          // portrait, i.e. the dark collar.
                                          // Must stay wider than the inner
                                          // shadow below, or the collar gets
                                          // shadowed from both sides and goes
                                          // black.

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

    // A recoloured copy of an icon, keeping its alpha. The mark art is dark
    // navy, which vanishes on the dark band — this makes a light version.
    // Cached per (name, colour) because it allocates an image.
    inline juce::Image tintedImage (const char* binaryName, juce::Colour c)
    {
        struct Cache
        {
            juce::HashMap<juce::String, juce::Image> map;
            juce::CriticalSection lock;
        };
        static Cache cache;
        const juce::String key = juce::String (binaryName) + "#" + c.toString();
        const juce::ScopedLock sl (cache.lock);
        if (cache.map.contains (key))
            return cache.map[key];

        auto src = image (binaryName);
        juce::Image out;
        if (src.isValid())
        {
            out = src.convertedToFormat (juce::Image::ARGB);
            juce::Image::BitmapData bd (out, juce::Image::BitmapData::readWrite);
            for (int y = 0; y < out.getHeight(); ++y)
                for (int x = 0; x < out.getWidth(); ++x)
                {
                    const auto px = bd.getPixelColour (x, y);
                    bd.setPixelColour (x, y, c.withAlpha (px.getFloatAlpha()));
                }
        }
        cache.map.set (key, out);
        return out;
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

    // The dark hero band's outline: the strip, unioned with a circle around
    // the character so the dark area bulges up into the header and down into
    // the content exactly along the portrait's edge. Both sub-paths wind the
    // same way, so the default non-zero fill rule unions them.
    inline juce::Path bandPath (juce::Rectangle<int> band, juce::Point<float> heroCentre,
                                float heroOuterR)
    {
        juce::Path p;
        p.addRectangle (band.toFloat());
        p.addEllipse (juce::Rectangle<float> (heroOuterR * 2.0f, heroOuterR * 2.0f)
                          .withCentre (heroCentre));
        return p;
    }

    // Fills that outline with the tiled background art, adds a little depth
    // and then an INNER shadow along the whole edge, so the dark area reads
    // as recessed into the light page.
    inline void paintBand (juce::Graphics& g, const juce::Path& shape,
                           juce::Rectangle<int> b)
    {
        {
            juce::Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (shape);

            const auto tile = image ("ui_bg_tile_png");
            if (tile.isValid())
            {
                // the art is a 256 px diamond cell; ~118 px on screen keeps
                // the lattice fine without turning into noise
                const float scale = 118.0f / (float) tile.getWidth();
                g.setFillType (juce::FillType (tile, juce::AffineTransform::scale (scale)));
                g.fillAll();
            }
            else
            {
                g.setColour (bandBot);
                g.fillAll();
            }

            // depth: darker toward the bottom of the strip. Filled over the
            // STRIP only — across the whole clip the bulge that hangs below
            // would take the gradient's end colour and read as a black collar.
            g.setGradientFill (juce::ColourGradient (
                juce::Colours::transparentBlack, (float) b.getCentreX(), (float) b.getY(),
                juce::Colour (0x33000000),       (float) b.getCentreX(), (float) b.getBottom(),
                false));
            g.fillRect (b);

            // stars: deterministic, so they never shimmer between repaints
            juce::Random rng (0x5eed);
            for (int i = 0; i < 70; ++i)
            {
                const float x = (float) b.getX() + rng.nextFloat() * (float) b.getWidth();
                const float y = (float) b.getY() - 30.0f
                              + rng.nextFloat() * ((float) b.getHeight() + 90.0f);
                const float r = 0.6f + rng.nextFloat() * 1.4f;
                g.setColour (bandStar.withMultipliedAlpha (0.30f + rng.nextFloat() * 0.60f));
                g.fillEllipse (x - r, y - r, r * 2.0f, r * 2.0f);
            }

            // INNER shadow: stroking the outline while clipped to its inside
            // leaves only the inner half of each stroke, which is exactly a
            // soft shadow hugging the edge.
            for (int i = 0; i < 8; ++i)
            {
                g.setColour (juce::Colours::black.withAlpha (0.055f - (float) i * 0.0058f));
                g.strokePath (shape, juce::PathStrokeType (3.0f + (float) i * 2.2f));
            }
        }

        // a hairline lip so the recess has a crisp top edge
        g.setColour (juce::Colour (0x33ffffff));
        g.strokePath (shape, juce::PathStrokeType (1.0f));
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
