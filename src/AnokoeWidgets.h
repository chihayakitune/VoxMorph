#pragma once
#include "AnokoeTheme.h"

// ===========================================================================
// ANOKOE widgets (v0.31.0)
//
// The skinned controls. Everything here is presentation only — the rows in
// PluginEditor.h still attach plain juce::Slider / ToggleButton / ComboBox to
// the APVTS, so parameter behaviour, undo, locking and host automation are
// untouched by the skin.
// ===========================================================================
namespace anokoe
{

// ---------------------------------------------------------------------------
// LookAndFeel that paints the image-based knob and slider art. One instance
// per tone; the tone decides which coloured variant of the art is used.
class ToneLookAndFeel : public juce::LookAndFeel_V4
{
public:
    explicit ToneLookAndFeel (Tone t) : tone (t)
    {
        setColour (juce::Label::textColourId,               ink);
        setColour (juce::Slider::textBoxTextColourId,       ink);
        setColour (juce::Slider::textBoxBackgroundColourId, valueFill);
        setColour (juce::Slider::textBoxOutlineColourId,    valueLine);
        setColour (juce::Slider::textBoxHighlightColourId,  toneInk (t).withAlpha (0.3f));
        setColour (juce::ToggleButton::textColourId,        ink);
        setColour (juce::ToggleButton::tickColourId,        toneInk (t));
        setColour (juce::ToggleButton::tickDisabledColourId,line);
        setColour (juce::TextButton::buttonColourId,        juce::Colours::white);
        setColour (juce::TextButton::textColourOffId,       ctrlInk);
        setColour (juce::TextButton::textColourOnId,        ctrlInk);
        setColour (juce::ComboBox::backgroundColourId,      juce::Colours::white);
        setColour (juce::ComboBox::textColourId,            ctrlInk);
        setColour (juce::ComboBox::outlineColourId,         line);
        setColour (juce::ComboBox::arrowColourId,           ctrlInk);
        setColour (juce::PopupMenu::backgroundColourId,     juce::Colours::white);
        setColour (juce::PopupMenu::textColourId,           ink);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, pluginFill);
        setColour (juce::PopupMenu::highlightedTextColourId,       ink);
        setColour (juce::TextEditor::backgroundColourId,    juce::Colours::white);
        setColour (juce::TextEditor::textColourId,          ink);
        setColour (juce::TextEditor::outlineColourId,       line);
        setColour (juce::ScrollBar::thumbColourId,          juce::Colour (0xffc3cee6));
    }

    // ---- knob: base art + arc-clipped ring art + rotated pointer ---------
    void drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h,
                           float posProportional, float startAngle, float endAngle,
                           juce::Slider&) override
    {
        const auto box = juce::Rectangle<int> (x, y, w, h).toFloat();
        const float side = juce::jmin (box.getWidth(), box.getHeight());
        const auto art  = juce::Rectangle<float> (side, side).withCentre (box.getCentre());

        drawFitted (g, toned ("ui_knob_base", tone), art);

        // the ring lights up over the swept part of the dial
        if (posProportional > 0.001f)
        {
            const auto ring = toned ("ui_knob_ring", tone);
            if (ring.isValid())
            {
                juce::Graphics::ScopedSaveState ss (g);
                juce::Path pie;
                pie.addPieSegment (art.expanded (side), startAngle,
                                   startAngle + posProportional * (endAngle - startAngle), 0.0f);
                g.reduceClipRegion (pie);
                drawFitted (g, ring, art);
            }
        }

        const auto ptr = toned ("ui_knob_pointer", tone);
        if (ptr.isValid())
        {
            const float rot = startAngle + posProportional * (endAngle - startAngle)
                            - juce::MathConstants<float>::twoPi;   // art points up at 0
            const auto src = juce::Rectangle<float> ((float) ptr.getWidth(), (float) ptr.getHeight());
            auto t = juce::RectanglePlacement (juce::RectanglePlacement::centred)
                        .getTransformToFit (src, art)
                        .followedBy (juce::AffineTransform::rotation (
                            rot, art.getCentreX(), art.getCentreY()));
            g.drawImageTransformed (ptr, t, false);
        }
    }

    // ---- horizontal slider: cap / centre / fill art + heart thumb --------
    void drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h,
                           float sliderPos, float, float,
                           const juce::Slider::SliderStyle, juce::Slider& s) override
    {
        if (s.isTwoValue() || s.isThreeValue())
        {
            juce::LookAndFeel_V4::drawLinearSlider (g, x, y, w, h, sliderPos, 0, 0,
                                                    juce::Slider::LinearHorizontal, s);
            return;
        }

        const float th  = (float) juce::jmin (kTrackH, h);
        const float cap = th;                                  // caps are square
        auto row = juce::Rectangle<float> ((float) x, (float) y + ((float) h - th) * 0.5f,
                                           (float) w, th);
        if (row.getWidth() <= cap * 2.0f + 1.0f) return;

        const auto capL = row.withWidth (cap);
        const auto capR = row.withX (row.getRight() - cap).withWidth (cap);
        const auto mid  = juce::Rectangle<float> (row.getX() + cap, row.getY(),
                                                  row.getWidth() - cap * 2.0f, th);

        g.drawImage (image ("ui_slider_track_center_png"), mid, juce::RectanglePlacement::stretchToFit, false);
        g.drawImage (toned ("ui_slider_track_left", tone), capL, juce::RectanglePlacement::stretchToFit, false);
        g.drawImage (image ("ui_slider_track_right_png"), capR, juce::RectanglePlacement::stretchToFit, false);

        // sliderPos is an absolute x inside [x + cap, x + w - cap]
        const float span = mid.getWidth();
        const float ratio = span > 0.0f
                              ? juce::jlimit (0.0f, 1.0f, (sliderPos - mid.getX()) / span) : 0.0f;
        if (ratio > 0.002f)
            g.drawImage (toned ("ui_slider_fill", tone),
                         mid.withWidth (span * ratio),
                         juce::RectanglePlacement::stretchToFit, false);

        const auto heart = toned ("ui_slider_knob_heart", tone);
        if (heart.isValid())
        {
            const float hs = th * (s.isEnabled() ? 1.0f : 0.86f);
            drawFitted (g, heart,
                        juce::Rectangle<float> (hs, hs)
                            .withCentre ({ mid.getX() + span * ratio, row.getCentreY() }),
                        s.isEnabled() ? 1.0f : 0.55f);
        }
    }

    int getSliderThumbRadius (juce::Slider&) override { return kTrackH / 2; }

    juce::Label* createSliderTextBox (juce::Slider& s) override
    {
        auto* l = juce::LookAndFeel_V4::createSliderTextBox (s);
        l->setFont (font (12.0f));
        l->setJustificationType (juce::Justification::centredRight);
        l->setBorderSize ({ 1, 4, 1, 5 });
        return l;
    }

    void drawLabel (juce::Graphics& g, juce::Label& l) override
    {
        // the value read-outs get the rounded outline from the prototype.
        // Rows name their read-out "vmValue"; slider-owned text boxes are
        // matched by their parent.
        if (! l.isBeingEdited()
            && (l.getName() == "vmValue"
                || dynamic_cast<juce::Slider*> (l.getParentComponent()) != nullptr))
        {
            auto b = l.getLocalBounds().toFloat().reduced (0.5f, 1.5f);
            g.setColour (valueFill);
            g.fillRoundedRectangle (b, 6.0f);
            g.setColour (valueLine);
            g.drawRoundedRectangle (b, 6.0f, 1.0f);
            g.setColour (l.isEnabled() ? ink : ink.withAlpha (0.45f));
            g.setFont (font (12.0f));
            g.drawText (l.getText(), l.getLocalBounds().reduced (5, 0),
                        juce::Justification::centredRight, false);
            return;
        }
        juce::LookAndFeel_V4::drawLabel (g, l);
    }

    // ---- checkbox: soft rounded box, tinted tick ------------------------
    void drawToggleButton (juce::Graphics& g, juce::ToggleButton& b,
                           bool /*highlighted*/, bool /*down*/) override
    {
        const float boxSide = 15.0f;
        auto r = b.getLocalBounds().toFloat();
        auto box = juce::Rectangle<float> (boxSide, boxSide)
                       .withCentre ({ r.getX() + 2.0f + boxSide * 0.5f, r.getCentreY() });

        const bool on = b.getToggleState();
        g.setColour (on ? toneInk (tone) : juce::Colours::white);
        g.fillRoundedRectangle (box, 4.0f);
        g.setColour (on ? toneInk (tone) : line);
        g.drawRoundedRectangle (box.reduced (0.5f), 4.0f, 1.0f);
        if (on)
        {
            juce::Path tick;
            tick.startNewSubPath (box.getX() + 3.4f,  box.getCentreY() + 0.4f);
            tick.lineTo         (box.getX() + 6.1f,  box.getBottom() - 4.0f);
            tick.lineTo         (box.getRight() - 3.2f, box.getY() + 4.2f);
            g.setColour (juce::Colours::white);
            g.strokePath (tick, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved,
                                                            juce::PathStrokeType::rounded));
        }

        g.setColour (b.isEnabled() ? ink : ink.withAlpha (0.45f));
        g.setFont (font (12.5f));
        g.drawText (b.getButtonText(),
                    b.getLocalBounds().withTrimmedLeft ((int) (boxSide + 10.0f)),
                    juce::Justification::centredLeft, false);
    }

    // ---- buttons: pill with hairline border ----------------------------
    void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour& colour,
                               bool highlighted, bool down) override
    {
        auto r = b.getLocalBounds().toFloat().reduced (0.5f);
        const float rad = juce::jmin (10.0f, r.getHeight() * 0.32f);
        auto fill = colour;
        if (down)             fill = fill.darker (0.06f);
        else if (highlighted) fill = fill.brighter (0.04f);
        g.setColour (fill);
        g.fillRoundedRectangle (r, rad);
        g.setColour (b.findColour (juce::ComboBox::outlineColourId, true).withAlpha (
                         b.isEnabled() ? 1.0f : 0.4f));
        g.drawRoundedRectangle (r, rad, 1.0f);
    }

    void drawButtonText (juce::Graphics& g, juce::TextButton& b, bool, bool) override
    {
        g.setColour (b.findColour (b.getToggleState() ? juce::TextButton::textColourOnId
                                                     : juce::TextButton::textColourOffId)
                       .withAlpha (b.isEnabled() ? 1.0f : 0.45f));
        g.setFont (font (12.0f));
        g.drawText (b.getButtonText(), b.getLocalBounds(), juce::Justification::centred, false);
    }

    void drawComboBox (juce::Graphics& g, int w, int h, bool, int, int, int, int,
                       juce::ComboBox& box) override
    {
        auto r = juce::Rectangle<float> (0.0f, 0.0f, (float) w, (float) h).reduced (0.5f);
        g.setColour (box.findColour (juce::ComboBox::backgroundColourId));
        g.fillRoundedRectangle (r, 8.0f);
        g.setColour (box.findColour (juce::ComboBox::outlineColourId));
        g.drawRoundedRectangle (r, 8.0f, 1.0f);

        juce::Path arrow;                                     // chevron
        const float cx = (float) w - 15.0f, cy = (float) h * 0.5f;
        arrow.startNewSubPath (cx - 4.0f, cy - 2.0f);
        arrow.lineTo (cx, cy + 2.5f);
        arrow.lineTo (cx + 4.0f, cy - 2.0f);
        g.setColour (box.findColour (juce::ComboBox::arrowColourId)
                       .withAlpha (box.isEnabled() ? 0.9f : 0.35f));
        g.strokePath (arrow, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved,
                                                         juce::PathStrokeType::rounded));
    }

    void positionComboBoxText (juce::ComboBox& box, juce::Label& l) override
    {
        l.setBounds (10, 1, box.getWidth() - 30, box.getHeight() - 2);
        l.setFont (font (12.5f));
    }

private:
    Tone tone;
};

// ---------------------------------------------------------------------------
// A tiny 17 px icon button (row reset / lock, header gear, preset save+delete)
class IconButton : public juce::Button
{
public:
    IconButton (const juce::String& name, const char* binaryName, int iconPx = 17)
        : juce::Button (name), px (iconPx)
    {
        setImage (binaryName);
    }

    void setImage (const char* binaryName)
    {
        name = binaryName;
        img  = tint.isTransparent() ? image (binaryName) : tintedImage (binaryName, tint);
        repaint();
    }

    void setFramed (bool shouldBeFramed) { framed = shouldBeFramed; repaint(); }

    // Recolours the glyph — the mark art is dark navy and disappears on the
    // dark band, so anything placed there asks for a light tint.
    void setTint (juce::Colour c)
    {
        tint = c;
        if (name != nullptr) setImage (name);
    }

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        auto r = getLocalBounds().toFloat();
        if (framed)
        {
            auto b = r.reduced (0.5f);
            g.setColour (down ? pluginFill : highlighted ? juce::Colour (0xfff4f7fd)
                                                        : juce::Colours::white);
            g.fillRoundedRectangle (b, 8.0f);
            g.setColour (line);
            g.drawRoundedRectangle (b, 8.0f, 1.0f);
        }
        else if (highlighted)
        {
            g.setColour (juce::Colour (0x14000000));
            g.fillRoundedRectangle (r.reduced (1.0f), 6.0f);
        }

        if (! img.isValid()) return;
        const float s = (float) px * (down ? 0.92f : 1.0f);
        g.setOpacity (isEnabled() ? 1.0f : 0.4f);
        g.drawImage (img, juce::Rectangle<float> (s, s).withCentre (r.getCentre()),
                     juce::RectanglePlacement::centred, false);
        g.setOpacity (1.0f);
    }

private:
    juce::Image img;
    const char* name = nullptr;
    int  px;
    bool framed = false;
    juce::Colour tint { juce::Colours::transparentBlack };
};

// ---------------------------------------------------------------------------
// Card: a titled panel. Children are stacked top to bottom by add(); the
// caller only says how tall each row is.
class Card : public juce::Component
{
public:
    Card() = default;

    void setTitle (const juce::String& text, const char* iconBinaryName)
    {
        title = text;
        icon  = iconBinaryName != nullptr ? image (iconBinaryName) : juce::Image();
        repaint();
    }

    // h < 0 means "share the leftover height with the other flexible rows"
    void add (juce::Component& c, int h)
    {
        addAndMakeVisible (c);
        rows.push_back ({ &c, h });
    }

    void addGap (int h) { rows.push_back ({ nullptr, h }); }

    int preferredHeight() const
    {
        int total = title.isNotEmpty() ? kTitleH : kGap;
        for (auto& r : rows) total += juce::jmax (0, r.h);
        return total + 8;
    }

    // v0.34.0: sections no longer draw a panel — no fill, no outline. The
    // body is one flat tone and the headings alone separate the groups.
    void paint (juce::Graphics& g) override
    {
        if (title.isEmpty()) return;
        auto t = getLocalBounds().withHeight (kTitleH).reduced (kCardPadX, 0).withTrimmedTop (4);
        if (icon.isValid())
        {
            drawFitted (g, icon, juce::Rectangle<float> (26.0f, 26.0f)
                                     .withCentre ({ (float) t.getX() + 13.0f, (float) t.getCentreY() }));
            t.removeFromLeft (32);
        }
        g.setColour (heading);
        g.setFont (font (15.0f, true));
        g.drawText (title, t, juce::Justification::centredLeft, false);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        r.removeFromTop (title.isNotEmpty() ? kTitleH : kGap);
        r.removeFromBottom (4);
        r = r.reduced (kCardPadX, 0);

        int flexCount = 0, fixed = 0;
        for (auto& row : rows)
            if (row.h < 0) ++flexCount; else fixed += row.h;
        const int flexH = flexCount > 0 ? juce::jmax (0, (r.getHeight() - fixed) / flexCount) : 0;

        for (auto& row : rows)
        {
            auto slice = r.removeFromTop (row.h < 0 ? flexH : row.h);
            if (row.comp != nullptr) row.comp->setBounds (slice);
        }
    }

private:
    struct Row { juce::Component* comp; int h; };
    juce::String title;
    juce::Image  icon;
    std::vector<Row> rows;
};

// ---------------------------------------------------------------------------
// Sidebar page button: big icon over a label, white + outlined when selected.
class SidebarButton : public juce::Button
{
public:
    SidebarButton (const juce::String& text, const char* iconBinaryName)
        : juce::Button (text), label (text), icon (image (iconBinaryName))
    {
        setClickingTogglesState (false);
    }

    void paintButton (juce::Graphics& g, bool highlighted, bool /*down*/) override
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        if (getToggleState())
        {
            g.setColour (juce::Colour (0x1452679a));
            g.fillRoundedRectangle (r.translated (0.0f, 3.0f), 16.0f);
            g.setColour (juce::Colours::white);
            g.fillRoundedRectangle (r, 16.0f);
            g.setColour (sidebarSel);
            g.drawRoundedRectangle (r.reduced (0.5f), 16.0f, 1.0f);
        }
        else if (highlighted)
        {
            g.setColour (juce::Colours::white.withAlpha (0.55f));
            g.fillRoundedRectangle (r, 16.0f);
        }

        auto inner = r.reduced (6.0f);
        const float iconSide = juce::jmin (52.0f, inner.getHeight() - 20.0f);
        drawFitted (g, icon, juce::Rectangle<float> (iconSide, iconSide)
                                 .withCentre ({ inner.getCentreX(),
                                                inner.getY() + iconSide * 0.5f + 2.0f }),
                    getToggleState() ? 1.0f : 0.78f);
        g.setColour (getToggleState() ? sidebarSel : sidebarInk);
        g.setFont (font (12.0f, getToggleState()));
        g.drawText (label, inner.removeFromBottom (18.0f).toNearestInt(),
                    juce::Justification::centred, false);
    }

private:
    juce::String label;
    juce::Image  icon;
};

// ---------------------------------------------------------------------------
// Page tab (v0.32.0): sits along the bottom edge of the dark hero band with
// rounded top corners, so the selected one reads as the sheet of content
// below coming forward. Replaces the v0.31 sidebar.
class TabButton : public juce::Button
{
public:
    TabButton (const juce::String& text, const char* iconBinaryName)
        : juce::Button (text), label (text), icon (image (iconBinaryName)),
          // the mark art is dark navy: on the band an unselected tab needs a
          // light copy of it, the selected one sits on white and does not
          iconDim (tintedImage (iconBinaryName, bandInk))
    {
        setClickingTogglesState (false);
    }

    void paintButton (juce::Graphics& g, bool highlighted, bool /*down*/) override
    {
        auto r = getLocalBounds().toFloat();
        const bool on = getToggleState();

        juce::Path tab;                       // rounded top, square bottom
        tab.addRoundedRectangle (r.getX(), r.getY(), r.getWidth(), r.getHeight() + 14.0f,
                                 14.0f, 14.0f, true, true, false, false);
        g.setColour (on ? juce::Colours::white
                        : juce::Colours::white.withAlpha (highlighted ? 0.16f : 0.07f));
        g.fillPath (tab);

        auto inner = r.reduced (8.0f, 6.0f);
        const float iconSide = juce::jmin (26.0f, inner.getHeight() - 16.0f);
        drawFitted (g, on ? icon : iconDim,
                    juce::Rectangle<float> (iconSide, iconSide)
                        .withCentre ({ inner.getCentreX(),
                                       inner.getY() + iconSide * 0.5f + 1.0f }),
                    on ? 1.0f : 0.86f);
        g.setColour (on ? sidebarSel : bandInk.withAlpha (0.88f));
        g.setFont (font (11.5f, on));
        g.drawText (label, inner.removeFromBottom (15.0f).toNearestInt(),
                    juce::Justification::centred, false);
    }

private:
    juce::String label;
    juce::Image  icon, iconDim;
};

// ---------------------------------------------------------------------------
// Header status button (Monitor / Mute): icon + text, lit when on.
class StatusButton : public juce::Button
{
public:
    StatusButton (const juce::String& text, const char* iconBinaryName, bool pinkWhenOn)
        : juce::Button (text), label (text), icon (image (iconBinaryName)), pink (pinkWhenOn)
    {
        setClickingTogglesState (true);
    }

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        auto r = getLocalBounds().toFloat().reduced (0.5f);
        if (getToggleState())
        {
            g.setColour (pink ? muteFill : onFill);
            g.fillRoundedRectangle (r, 12.0f);
            g.setColour (pink ? muteLine : onLine);
            g.drawRoundedRectangle (r, 12.0f, 1.0f);
        }
        else if (highlighted || down)
        {
            g.setColour (juce::Colours::white.withAlpha (down ? 0.9f : 0.6f));
            g.fillRoundedRectangle (r, 12.0f);
        }

        auto inner = r.reduced (10.0f, 0.0f);
        drawFitted (g, icon, juce::Rectangle<float> (20.0f, 20.0f)
                                 .withCentre ({ inner.getX() + 10.0f, inner.getCentreY() }),
                    isEnabled() ? 1.0f : 0.4f);
        g.setColour (actionInk.withAlpha (isEnabled() ? 1.0f : 0.4f));
        g.setFont (font (12.5f, getToggleState()));
        g.drawText (label, inner.withTrimmedLeft (24.0f).toNearestInt(),
                    juce::Justification::centredLeft, false);
    }

private:
    juce::String label;
    juce::Image  icon;
    bool pink;
};

// ---------------------------------------------------------------------------
// The latency donut from the prototype: base ring art with the centre puck on
// top, the reading underneath and a LOW / MID / HIGH chip.
class LatencyDonut : public juce::Component, public juce::SettableTooltipClient
{
public:
    void setReading (float ms, bool valid, const juce::String& detail)
    {
        if (ms == shownMs && valid == shownValid && detail == shownDetail) return;
        shownMs = ms;  shownValid = valid;  shownDetail = detail;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        paintCard (g, getLocalBounds().toFloat());

        auto r = getLocalBounds().reduced (10);
        auto textArea = r.removeFromBottom (44);
        const float side = (float) juce::jmin (r.getWidth(), r.getHeight());
        const auto art = juce::Rectangle<float> (side, side)
                             .withCentre ({ (float) r.getCentreX(), (float) r.getCentreY() });

        drawFitted (g, image ("ui_donuts_base_png"), art);

        // the filled part of the ring grows with the latency (0..120 ms)
        if (shownValid)
        {
            const float frac = juce::jlimit (0.0f, 1.0f, shownMs / 120.0f);
            juce::Path arc;
            const float rad = side * 0.365f;
            arc.addCentredArc (art.getCentreX(), art.getCentreY(), rad, rad, 0.0f,
                               0.0f, frac * juce::MathConstants<float>::twoPi, true);
            g.setColour (chipColour().withAlpha (0.85f));
            g.strokePath (arc, juce::PathStrokeType (side * 0.075f,
                              juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
        drawFitted (g, image ("ui_donuts_center_png"), art);

        g.setColour (ink);
        g.setFont (font (12.5f));
        auto line1 = textArea.removeFromTop (20);
        const auto text = "Latency: " + (shownValid ? juce::String (shownMs, 1) + " ms"
                                                    : juce::String ("--"));
        const int textW = 108;
        auto tb = line1.withSizeKeepingCentre (juce::jmin (line1.getWidth(), textW + 52), 20);
        g.drawText (text, tb.removeFromLeft (textW), juce::Justification::centredRight, false);
        if (shownValid)
        {
            auto chip = tb.removeFromLeft (46).reduced (4, 2);
            g.setColour (chipColour());
            g.fillRoundedRectangle (chip.toFloat(), 9.0f);
            g.setColour (juce::Colours::white);
            g.setFont (font (10.0f, true));
            g.drawText (chipText(), chip, juce::Justification::centred, false);
        }
        g.setColour (heading.withAlpha (0.75f));
        g.setFont (font (10.0f));
        g.drawText (shownDetail, textArea, juce::Justification::centredTop, false);
    }

private:
    juce::String chipText() const
    {
        return shownMs < 35.0f ? "LOW" : shownMs < 70.0f ? "MID" : "HIGH";
    }
    juce::Colour chipColour() const
    {
        return shownMs < 35.0f ? badge : shownMs < 70.0f ? badgeMid : badgeHigh;
    }

    float shownMs = 0.0f;
    bool  shownValid = false;
    juce::String shownDetail;
};

// ---------------------------------------------------------------------------
// The OUTPUT card's volume bar: the prototype's base strip with the fill strip
// clipped to the current level and a peak marker on top.
class VolumeBar : public juce::Component, public juce::SettableTooltipClient
{
public:
    void setLevel (float rms01, float peak01, bool clipping)
    {
        if (rms01 == r && peak01 == p && clipping == clip) return;
        r = rms01;  p = peak01;  clip = clipping;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto bar = getLocalBounds().toFloat().reduced (0.0f, 2.0f);
        if (bar.getWidth() < 10.0f) return;

        const auto base = image ("ui_volume_basew_png");
        const auto fill = image ("ui_volume_fill_png");
        g.drawImage (base, bar, juce::RectanglePlacement::stretchToFit, false);

        if (r > 0.002f)
        {
            juce::Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (bar.withWidth (bar.getWidth() * r).getSmallestIntegerContainer());
            g.drawImage (fill, bar, juce::RectanglePlacement::stretchToFit, false);
        }
        if (p > 0.002f)
        {
            g.setColour (clip ? juce::Colour (0xffe23b52) : heading.withAlpha (0.6f));
            const float x = bar.getX() + bar.getWidth() * juce::jmin (1.0f, p);
            g.fillRect (juce::Rectangle<float> (x - 1.0f, bar.getY(), 2.0f, bar.getHeight()));
        }
    }

private:
    float r = 0.0f, p = 0.0f;
    bool  clip = false;
};

} // namespace anokoe
