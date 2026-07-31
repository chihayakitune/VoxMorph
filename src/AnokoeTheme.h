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
    // v0.34.0: the body is one flat light grey — sections no longer draw
    // their own panel, so this is what you see behind every card
    const juce::Colour bodyFill  { 0xfff3f5fb };
    const juce::Colour treeLine  { 0xffc9d3e8 };   // FORMANT group brackets

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
    constexpr int   kTitleH      = 34;
    constexpr int   kRowH        = 30;    // one slider row (v0.34.0: tighter)
    constexpr int   kToggleH     = 26;
    constexpr int   kKnobH       = 106;   // knob art box
    constexpr int   kKnobRowH    = 116;
    constexpr int   kTreeIndent  = 14;    // FORMANT child rows
    constexpr int   kTrackH      = 22;    // slider track art height
    constexpr int   kValueW      = 54;
    constexpr int   kActionsW    = 40;
    constexpr int   kLabelW      = 150;
    constexpr int   kGap         = 10;
    constexpr int   kSidebarW    = 132;
    constexpr int   kHeaderH     = 94;    // v0.34.0: 1.5x the v0.32 height
    constexpr int   kBandH       = 158;   // dark hero band
    constexpr int   kTabH        = 58;    // page tabs along the band's bottom edge
    constexpr int   kHeroR       = 112;   // character portrait radius
    constexpr int   kHeroRim     = 34;    // how far the band bulges past the
                                          // portrait, i.e. the dark collar.
                                          // Must stay wider than the inner
                                          // shadow below, or the collar gets
                                          // shadowed from both sides and goes
                                          // black.
    constexpr float kHeroFillet  = 34.0f; // radius of the sweep where the
                                          // circle meets the strip's edges

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
    // The dark area's outline: the strip, with a circle around the character
    // bulging out of its top and bottom edges, and a proper tangent FILLET at
    // each of the four crossings.
    //
    // The fillet cannot be done by unioning extra circles: a disc tangent to
    // both the edge and the hero circle touches each at a single point, so it
    // reads as four detached blobs. The sweep has to be an ARC of that disc,
    // walked as part of one outline — which is what this builds.
    //
    // Fillet geometry at an edge y = e: the disc sits r beyond the edge (so it
    // is tangent to it) and R + r from the hero centre (so it is externally
    // tangent to the circle), which fixes its horizontal offset:
    //     dx = sqrt((R + r)^2 - (d + r)^2),  d = |heroCentre.y - e|
    inline juce::Path bandPath (juce::Rectangle<int> band, juce::Point<float> c, float R)
    {
        const auto  b = band.toFloat();
        const float r = kHeroFillet;
        const float pi = juce::MathConstants<float>::pi;
        const float twoPi = juce::MathConstants<float>::twoPi;

        // JUCE arc angles: 0 = 12 o'clock, increasing clockwise
        auto ang = [] (juce::Point<float> o, juce::Point<float> pt)
        { return std::atan2 (pt.x - o.x, o.y - pt.y); };
        auto shortWay = [twoPi, pi] (float from, float to)
        {
            while (to - from >  pi) to -= twoPi;
            while (to - from < -pi) to += twoPi;
            return to;
        };
        auto clockwise = [twoPi] (float from, float to)
        {
            while (to < from) to += twoPi;
            return to;
        };

        struct Edge
        {
            bool crosses = false;
            float y = 0.0f, dx = 0.0f, fy = 0.0f;
            juce::Point<float> tan[2];              // tangency on the hero circle
        };
        auto solve = [&] (float edgeY, float outward)
        {
            Edge e;  e.y = edgeY;
            const float d = std::abs (c.y - edgeY);
            const float A = R + r, O = d + r;
            if (d >= R || A <= O) return e;          // circle misses this edge
            e.crosses = true;
            e.dx = std::sqrt (A * A - O * O);
            e.fy = edgeY + outward * r;
            for (int i = 0; i < 2; ++i)
            {
                const juce::Point<float> f (c.x + (i == 0 ? -e.dx : e.dx), e.fy);
                e.tan[i] = c + (f - c) * (R / A);
            }
            return e;
        };
        const Edge top = solve (b.getY(),      -1.0f);
        const Edge bot = solve (b.getBottom(), +1.0f);

        juce::Path p;
        if (! top.crosses || ! bot.crosses)          // degenerate: plain union
        {
            p.addRectangle (b);
            p.addEllipse (juce::Rectangle<float> (R * 2.0f, R * 2.0f).withCentre (c));
            return p;
        }

        // one closed outline, walked clockwise
        auto sweep = [&] (const Edge& e, int side, bool intoCircle)
        {
            const juce::Point<float> f (c.x + (side == 0 ? -e.dx : e.dx), e.fy);
            const juce::Point<float> onEdge (f.x, e.y);
            const float aEdge = ang (f, onEdge), aArc = ang (f, e.tan[side]);
            if (intoCircle) p.addCentredArc (f.x, f.y, r, r, 0.0f, aEdge, shortWay (aEdge, aArc), false);
            else            p.addCentredArc (f.x, f.y, r, r, 0.0f, aArc, shortWay (aArc, aEdge), false);
        };

        p.startNewSubPath (b.getX(), b.getY());
        p.lineTo (c.x - top.dx, b.getY());
        sweep (top, 0, true);
        p.addCentredArc (c.x, c.y, R, R, 0.0f, ang (c, top.tan[0]),
                         clockwise (ang (c, top.tan[0]), ang (c, top.tan[1])), false);
        sweep (top, 1, false);
        p.lineTo (b.getRight(), b.getY());
        p.lineTo (b.getRight(), b.getBottom());
        p.lineTo (c.x + bot.dx, b.getBottom());
        sweep (bot, 1, true);
        p.addCentredArc (c.x, c.y, R, R, 0.0f, ang (c, bot.tan[1]),
                         clockwise (ang (c, bot.tan[1]), ang (c, bot.tan[0])), false);
        sweep (bot, 0, false);
        p.lineTo (b.getX(), b.getBottom());
        p.closeSubPath();
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

            // INNER shadow: stroking the outline while clipped to its inside
            // leaves only the inner half of each stroke, which is exactly a
            // soft shadow hugging the edge.
            for (int i = 0; i < 8; ++i)
            {
                g.setColour (juce::Colours::black.withAlpha (0.055f - (float) i * 0.0058f));
                g.strokePath (shape, juce::PathStrokeType (3.0f + (float) i * 2.2f));
            }
        }

        // NOTE: no outline stroke here. A hairline along `shape` reads as the
        // header's and the content's borders running straight through the
        // dark area, plus a ring around the character — the inner shadow
        // above is what defines the edge.
    }

    // The body background: one flat light grey (v0.34.0). Sections used to
    // sit on a gradient inside their own translucent panels; now the whole
    // content area is a single tone and the cards draw no panel at all.
    inline void paintPage (juce::Graphics& g, juce::Rectangle<int> b)
    {
        g.setColour (bodyFill);
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
