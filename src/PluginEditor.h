#pragma once
#include "PluginProcessor.h"
#include "VoiceAnalyzer.h"
#include "MatchingEngine.h"
#include "SampleTargetCatalog.h"

// StandalonePluginHolder gives access to the standalone wrapper's state
// saving (used for the Cmd+S shortcut). Only present in the standalone build.
#if defined(JUCE_STANDALONE_APPLICATION) && JUCE_STANDALONE_APPLICATION \
    && __has_include(<juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>)
 #include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
 #define VOXMORPH_HAS_STANDALONE_HOLDER 1
#else
 #define VOXMORPH_HAS_STANDALONE_HOLDER 0
#endif

// VoxMorph custom editor:
//  - parameters grouped into sections, English UI
//  - hover tooltips with bilingual (EN/JP) explanations, readable typography
//  - editable value boxes (click the number, type, Enter)
//  - per-parameter reset buttons + double-click-to-default on sliders
//  - scrollable + resizable window (rows live inside a Viewport)
//
// HOW TO EDIT THIS UI (for future maintainers):
//  - To add a control: add ONE line in the constructor —
//      addSliderRow ("paramId", "Display Name", tip ("english", "日本語"));
//      addToggleRow ("paramId", "Display Name", tip ("english", "日本語"));
//      addSection   ("SECTION NAME");
//    at the position where it should appear. Layout, scrolling and window
//    size all adjust automatically. The paramId must exist in
//    PluginProcessor.cpp createLayout().
//  - Row heights / widths: see the `items.push_back` calls and layoutMainPage().
//  - Default window height is capped by kMaxInitialHeight below.
//  - Tabs: MAIN = scrolling parameter list, MATCHING = MatchingPanel,
//    PRESETS = PresetPanel (both defined below in this file).
//  - All theme colours live in the mainLnf block in the constructor.

// Spectrum visualizer: INPUT (mint) and converted OUTPUT (pink) spectra
// overlaid on a 20 Hz - 20 kHz log axis. Pulls samples from the processor's
// viz ring buffers on its own 30 Hz timer; FFT is the engine's radix-2.
class SpectrumView : public juce::Component, private juce::Timer
{
public:
    explicit SpectrumView (VoxMorphProcessor& p) : proc (p)
    {
        re.assign ((size_t) kN, 0.0f);
        im.assign ((size_t) kN, 0.0f);
        smIn .assign ((size_t) kCols, kFloor);
        smOut.assign ((size_t) kCols, kFloor);
        startTimerHz (30);
    }

private:
    static constexpr int   kN     = 4096;     // FFT size
    static constexpr int   kCols  = 220;      // drawn columns
    static constexpr float kFloor = -66.0f, kTop = 6.0f;   // dB display range

    void timerCallback() override
    {
        if (! isShowing()) return;
        analyze (proc.vizIn,  smIn);
        analyze (proc.vizOut, smOut);
        repaint();
    }

    void analyze (const std::vector<float>& src, std::vector<float>& dest)
    {
        const int pos  = proc.vizPos.load (std::memory_order_acquire);
        const int mask = VoxMorphProcessor::kVizLen - 1;
        for (int i = 0; i < kN; ++i)
        {
            const float w = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) kN);
            re[(size_t) i] = src[(size_t) ((pos - kN + i) & mask)] * w;
            im[(size_t) i] = 0.0f;
        }
        PsolaEngine::fftForViz (re.data(), im.data(), kN);

        const double fs = proc.getSampleRate() > 0 ? proc.getSampleRate() : 48000.0;
        for (int c = 0; c < kCols; ++c)
        {
            // log axis: 20 Hz .. 20 kHz over kCols columns
            const double f0 = 20.0 * std::pow (1000.0, (double)  c      / kCols);
            const double f1 = 20.0 * std::pow (1000.0, (double) (c + 1) / kCols);
            int b0 = std::clamp ((int) (f0 * kN / fs), 1, kN / 2 - 1);
            int b1 = std::clamp ((int) (f1 * kN / fs) + 1, b0 + 1, kN / 2);
            float pk = 1.0e-12f;
            for (int b = b0; b < b1; ++b)
                pk = std::max (pk, re[(size_t) b] * re[(size_t) b] + im[(size_t) b] * im[(size_t) b]);
            // 0 dB ~= full-scale sine (Hann-windowed peak = N/4)
            float db = std::clamp (10.0f * std::log10 (pk) - 60.2f, kFloor, kTop);
            float& s = dest[(size_t) c];        // fast attack, slow release
            s = db > s ? 0.45f * s + 0.55f * db
                       : 0.86f * s + 0.14f * db;
        }
    }

    void drawCurve (juce::Graphics& g, const std::vector<float>& v,
                    juce::Rectangle<float> r, juce::Colour col) const
    {
        juce::Path p;
        for (int c = 0; c < kCols; ++c)
        {
            const float x = r.getX() + r.getWidth()  * (float) c / (float) (kCols - 1);
            const float y = r.getY() + r.getHeight() * (kTop - v[(size_t) c]) / (kTop - kFloor);
            if (c == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
        }
        g.setColour (col);
        g.strokePath (p, juce::PathStrokeType (1.8f));
        p.lineTo (r.getRight(), r.getBottom());
        p.lineTo (r.getX(),     r.getBottom());
        p.closeSubPath();
        g.setColour (col.withAlpha (0.18f));
        g.fillPath (p);
    }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat().reduced (2.0f);
        g.setColour (juce::Colours::white);
        g.fillRoundedRectangle (b, 8.0f);
        g.setColour (juce::Colour (0xffe9d8dd));
        g.drawRoundedRectangle (b, 8.0f, 1.0f);

        auto r = b.reduced (10.0f, 10.0f);
        g.setColour (juce::Colour (0x12000000));                   // grid
        g.setFont (juce::Font (juce::FontOptions (10.0f)));
        for (double f : { 100.0, 1000.0, 10000.0 })
        {
            const float x = r.getX() + r.getWidth() * (float) (std::log10 (f / 20.0) / 3.0);
            g.setColour (juce::Colour (0x12000000));
            g.drawVerticalLine ((int) x, r.getY(), r.getBottom());
            g.setColour (juce::Colour (0x66000000));
            g.drawText (f >= 1000.0 ? juce::String (f / 1000.0, 0) + "k" : juce::String ((int) f),
                        (int) x + 3, (int) r.getBottom() - 13, 34, 12, juce::Justification::left);
        }
        for (float db : { 0.0f, -24.0f, -48.0f })
        {
            const float y = r.getY() + r.getHeight() * (kTop - db) / (kTop - kFloor);
            g.setColour (juce::Colour (0x12000000));
            g.drawHorizontalLine ((int) y, r.getX(), r.getRight());
        }

        drawCurve (g, smIn,  r, juce::Colour (0xff54bda1));        // input: mint
        drawCurve (g, smOut, r, juce::Colour (0xfff08ba5));        // output: pink

        g.setFont (juce::Font (juce::FontOptions (11.0f)));        // legend
        g.setColour (juce::Colour (0xff54bda1));
        g.drawText ("Input",  (int) r.getRight() - 110, (int) r.getY() + 2, 50, 14, juce::Justification::left);
        g.setColour (juce::Colour (0xfff08ba5));
        g.drawText ("Output", (int) r.getRight() - 56,  (int) r.getY() + 2, 54, 14, juce::Justification::left);
    }

    VoxMorphProcessor& proc;
    std::vector<float> re, im, smIn, smOut;
};

// Realtime status row under the visualizer. Shows the estimated internal
// latency (engine lookahead + enabled hosted FX; the standalone app also
// adds the audio device buffers) as "Latency: xx.x ms" with a LOW/MID/HIGH
// badge and a breakdown on the right. Estimate only — delays outside the
// app (OS mixer, OBS, Discord, ...) are NOT included.
class StatusView : public juce::Component, public juce::SettableTooltipClient,
                   private juce::Timer
{
public:
    explicit StatusView (VoxMorphProcessor& p) : proc (p)
    {
        setTooltip (juce::String::fromUTF8 (
            "Estimated delay inside VoxMorph: engine lookahead (43 ms; half in Low Latency "
            "Mode) + enabled external FX plugins; the standalone app also adds the audio "
            "device buffers. Pitch / Formant / Voice Quality / Breath run inside one shared "
            "pipeline and add no delay of their own. Delays outside the app (OS, OBS, "
            "Discord, audio interface driver) are NOT included. "
            "LOW < 35 ms, MID 35-70 ms, HIGH > 70 ms.")
            + "\n\n" + juce::String::fromUTF8 (
            "VoxMorph内部の推定遅延です。エンジンの先読み(43ms、Low Latency Mode時は約半分)+"
            "有効な外部FXプラグインの合計で、スタンドアロン版はオーディオバッファ分も加算します。"
            "Pitch/Formant/Voice Quality/Breathは同一パイプライン内の処理のため追加遅延は"
            "ありません。OS・OBS・Discord・オーディオインターフェースのドライバなど、アプリ外の"
            "遅延は含みません。LOW<35ms / MID 35〜70ms / HIGH>70ms。"));
        startTimerHz (4);
    }

private:
    void timerCallback() override
    {
        if (! isShowing()) return;
        const double fs = proc.getSampleRate() > 0 ? proc.getSampleRate() : 48000.0;
        const int eng = proc.uiLatencySamples.load (std::memory_order_relaxed)
                      - proc.uiFxLatSamples.load (std::memory_order_relaxed);
        const int fx  = proc.uiFxLatSamples.load (std::memory_order_relaxed);
        const int buf = proc.wrapperType == juce::AudioProcessor::wrapperType_Standalone
                          ? 2 * proc.getBlockSize() : 0;   // in + out device buffers
        engMs = (float) (eng * 1000.0 / fs);
        fxMs  = (float) (fx  * 1000.0 / fs);
        bufMs = (float) (buf * 1000.0 / fs);
        known = eng > 0;                       // no audio prepared yet -> "--"
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat().reduced (2.0f, 1.0f);
        g.setColour (juce::Colours::white);
        g.fillRoundedRectangle (b, 6.0f);
        g.setColour (juce::Colour (0xffe9d8dd));
        g.drawRoundedRectangle (b, 6.0f, 1.0f);

        auto r = b.reduced (10.0f, 0.0f).toNearestInt();
        g.setColour (juce::Colour (0xff2e2e32));
        g.setFont (juce::Font (juce::FontOptions (12.5f)));
        const float ms = engMs + fxMs + bufMs;
        g.drawText ("Latency: " + (known ? juce::String (ms, 1) + " ms" : juce::String ("--")),
                    r, juce::Justification::centredLeft);

        if (known)
        {
            const bool low = ms < 35.0f, mid = ms < 70.0f;    // badge
            g.setColour (low ? juce::Colour (0xff54c0aa)
                       : mid ? juce::Colour (0xffe3a63c)
                             : juce::Colour (0xfff08ba5));
            const juce::Rectangle<float> chip ((float) r.getX() + 132.0f,
                                               b.getCentreY() - 8.0f, 44.0f, 16.0f);
            g.fillRoundedRectangle (chip, 8.0f);
            g.setColour (juce::Colours::white);
            g.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
            g.drawText (low ? "LOW" : mid ? "MID" : "HIGH",
                        chip.toNearestInt(), juce::Justification::centred);

            g.setColour (juce::Colour (0xff9aa5a2));           // breakdown
            g.setFont (juce::Font (juce::FontOptions (11.0f)));
            juce::String d = "engine " + juce::String (engMs, 1)
                           + " + FX " + juce::String (fxMs, 1);
            if (bufMs > 0.0f) d += " + buffer " + juce::String (bufMs, 1);
            g.drawText (d + " ms", r, juce::Justification::centredRight);
        }
    }

    VoxMorphProcessor& proc;
    float engMs = 0.0f, fxMs = 0.0f, bufMs = 0.0f;
    bool  known = false;
};

// VoiceProfile <-> XML (.vmprofile files, saved next to the presets)
inline std::unique_ptr<juce::XmlElement> profileToXml (const VoiceProfile& p)
{
    auto x = std::make_unique<juce::XmlElement> ("VMPROFILE");
    x->setAttribute ("f0", p.f0Hz);
    x->setAttribute ("spread", p.f0SpreadSt);
    x->setAttribute ("tilt", p.tiltDb);
    x->setAttribute ("frames", p.voicedFrames);
    for (int i = 0; i < 3; ++i)
    {
        x->setAttribute ("f" + juce::String (i + 1), p.F[i]);
        x->setAttribute ("l" + juce::String (i + 1), p.L[i]);
    }
    return x;
}

inline bool profileFromXml (const juce::XmlElement& x, VoiceProfile& p)
{
    if (! x.hasTagName ("VMPROFILE")) return false;
    p.f0Hz         = (float) x.getDoubleAttribute ("f0");
    p.f0SpreadSt   = (float) x.getDoubleAttribute ("spread");
    p.tiltDb       = (float) x.getDoubleAttribute ("tilt");
    p.voicedFrames = x.getIntAttribute ("frames");
    for (int i = 0; i < 3; ++i)
    {
        p.F[i] = (float) x.getDoubleAttribute ("f" + juce::String (i + 1));
        p.L[i] = (float) x.getDoubleAttribute ("l" + juce::String (i + 1));
    }
    return p.valid();
}

// Static "imagined voice spectrum" for the ANALYZE tab: unfilled line
// curves rebuilt from the measured profiles (F0 bump + F1-F3 bumps whose
// heights are the measured relative levels, -9 dB/oct rolloff above F3),
// on a 50 Hz - 8 kHz log axis. Redrawn after each measurement — not live.
class ProfileGraph : public juce::Component
{
public:
    // series identity — colour AND glyph are set per series, so viewers who
    // can't tell mint from yellow can still tell Current from Target (spec 4.4)
    const VoiceProfile* you       = nullptr;   // Current  = mint, filled dot
    const VoiceProfile* target    = nullptr;   // Target   = pastel yellow, ring
    const VoiceProfile* estimated = nullptr;   // Estimated= violet, diamond, dashed
    const VoiceProfile* conv      = nullptr;   // Matched  = pink, double concentric
    std::function<float (const char*)> param;   // reads current parameter values

    enum class Glyph { filledDot, ring, diamond, concentric };

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat().reduced (2.0f);
        g.setColour (juce::Colours::white);
        g.fillRoundedRectangle (b, 8.0f);
        g.setColour (juce::Colour (0xffe9d8dd));
        g.drawRoundedRectangle (b, 8.0f, 1.0f);
        auto r = b.reduced (10.0f, 10.0f);

        g.setFont (juce::Font (juce::FontOptions (10.0f)));
        for (double f : { 100.0, 1000.0 })
        {
            const float x = xOf (r, f);
            g.setColour (juce::Colour (0x12000000));
            g.drawVerticalLine ((int) x, r.getY(), r.getBottom());
            g.setColour (juce::Colour (0x66000000));
            g.drawText (f >= 1000.0 ? "1k" : "100",
                        (int) x + 3, (int) r.getBottom() - 13, 30, 12, juce::Justification::left);
        }

        const juce::Colour cy (0xff54bda1), ct (0xffdfb545), ce (0xffa889f4),
                           cc (0xfff08ba5);
        // Correction ◤◢ hatch first, UNDER the curves (spec 4.5): draws a
        // diagonal-stripe band between Current and Estimated formant
        // positions for each of F1/F2/F3. Only when both series are valid.
        if (you != nullptr && you->valid() && estimated != nullptr && estimated->valid())
            drawCorrectionHatch (g, r, *you, *estimated);

        if (you       != nullptr && you->valid())       drawProfile (g, r, *you,       cy, Glyph::filledDot,  false);
        if (target    != nullptr && target->valid())    drawProfile (g, r, *target,    ct, Glyph::ring,       false);
        if (estimated != nullptr && estimated->valid()) drawProfile (g, r, *estimated, ce, Glyph::diamond,    true);
        if (conv      != nullptr && conv->valid())      drawProfile (g, r, *conv,      cc, Glyph::concentric, false);

        // current High Range Start / Pitch Floor parameters (dashed markers)
        if (param)
        {
            const float dashes[2] = { 4.0f, 4.0f };
            auto vline = [&] (float hz, juce::Colour col, const char* name)
            {
                if (hz < 51.0f || hz > 7900.0f) return;
                const float x = xOf (r, hz);
                g.setColour (col.withAlpha (0.85f));
                g.drawDashedLine (juce::Line<float> (x, r.getY(), x, r.getBottom()), dashes, 2, 1.2f);
                g.setFont (juce::Font (juce::FontOptions (9.5f)));
                g.drawText (name, (int) x + 3, (int) r.getBottom() - 26, 70, 12,
                            juce::Justification::left);
            };
            vline (param ("hifreq"),     juce::Colour (0xff54bda1), "High Range");
            vline (param ("pitchfloor"), juce::Colour (0xfff08ba5), "Floor");
        }

        // legend: label + tiny glyph, so viewers who can't tell colours apart
        // still see which shape belongs to which series (spec 4.4)
        g.setFont (juce::Font (juce::FontOptions (11.0f)));
        auto legendItem = [&] (int x, juce::Colour col, Glyph gl, const char* lbl)
        {
            const int y = (int) r.getY() + 4;
            drawLegendGlyph (g, (float) x + 5.5f, (float) y + 6.0f, col, gl);
            g.setColour (col);
            g.drawText (lbl, x + 14, y - 1, 80, 14, juce::Justification::left);
        };
        const int lx = (int) r.getRight() - 300;
        legendItem (lx +   0, cy, Glyph::filledDot,  "Current");
        legendItem (lx +  80, ct, Glyph::ring,       "Target");
        legendItem (lx + 155, ce, Glyph::diamond,    "Estimated");
        legendItem (lx + 235, cc, Glyph::concentric, "Matched");

        if ((you == nullptr || ! you->valid()) && (target == nullptr || ! target->valid())
            && (estimated == nullptr || ! estimated->valid())
            && (conv == nullptr || ! conv->valid()))
        {
            g.setColour (juce::Colour (0x66000000));
            g.drawText (juce::String::fromUTF8 ("測定するとここに声のイメージが表示されます"),
                        getLocalBounds(), juce::Justification::centred);
        }
    }

private:
    static float xOf (juce::Rectangle<float> r, double f)
    {
        return r.getX() + r.getWidth() * (float) (std::log (f / 50.0) / std::log (8000.0 / 50.0));
    }

    static float yOfLevel (juce::Rectangle<float> r, float db)
    {
        constexpr float kTop = 6.0f, kFloor = -42.0f;
        return r.getY() + r.getHeight() * (kTop - db) / (kTop - kFloor);
    }

    static void drawGlyph (juce::Graphics& g, float cx, float cy_, juce::Colour col, Glyph gl)
    {
        g.setColour (col);
        switch (gl)
        {
            case Glyph::filledDot:
                g.fillEllipse (cx - 2.7f, cy_ - 2.7f, 5.4f, 5.4f);
                break;
            case Glyph::ring:
                g.drawEllipse (cx - 3.2f, cy_ - 3.2f, 6.4f, 6.4f, 1.3f);
                g.drawEllipse (cx - 1.4f, cy_ - 1.4f, 2.8f, 2.8f, 1.0f);
                break;
            case Glyph::diamond:
            {
                juce::Path d;
                d.startNewSubPath (cx, cy_ - 3.4f);
                d.lineTo (cx + 3.4f, cy_);
                d.lineTo (cx, cy_ + 3.4f);
                d.lineTo (cx - 3.4f, cy_);
                d.closeSubPath();
                g.strokePath (d, juce::PathStrokeType (1.2f));
                g.fillEllipse (cx - 1.0f, cy_ - 1.0f, 2.0f, 2.0f);
                break;
            }
            case Glyph::concentric:
                g.fillEllipse    (cx - 1.6f, cy_ - 1.6f, 3.2f, 3.2f);
                g.drawEllipse    (cx - 3.6f, cy_ - 3.6f, 7.2f, 7.2f, 1.1f);
                break;
        }
    }

    static void drawLegendGlyph (juce::Graphics& g, float cx, float cy_,
                                 juce::Colour col, Glyph gl)
    {
        drawGlyph (g, cx, cy_, col, gl);
    }

    static void drawProfile (juce::Graphics& g, juce::Rectangle<float> r,
                             const VoiceProfile& p, juce::Colour col,
                             Glyph gl, bool dashed)
    {
        constexpr float kTop = 6.0f, kFloor = -42.0f;
        juce::Path path;
        const int NP = 220;
        for (int i = 0; i < NP; ++i)
        {
            const double f = 50.0 * std::pow (8000.0 / 50.0, (double) i / (NP - 1));
            double db = -38.0;                                    // floor
            auto bump = [&] (double fc, double h, double sigmaOct)
            {
                if (fc <= 0.0) return;
                const double z = std::log2 (f / fc) / sigmaOct;
                db = std::max (db, h - 12.0 * z * z);
            };
            bump (p.f0Hz, 0.0, 0.12);                             // fundamental
            for (int fi = 0; fi < 3; ++fi)
                bump (p.F[fi], p.L[fi], 0.22);                    // formants
            if (f > p.F[2] && p.F[2] > 0)                         // HF rolloff
                db = std::min (db, p.L[2] - 9.0 * std::log2 (f / p.F[2]));

            const float x = r.getX() + r.getWidth() * (float) i / (float) (NP - 1);
            const float y = r.getY() + r.getHeight() * (kTop - (float) db) / (kTop - kFloor);
            if (i == 0) path.startNewSubPath (x, y); else path.lineTo (x, y);
        }
        g.setColour (col);
        if (dashed)
        {
            juce::Path dp;
            juce::PathStrokeType (1.5f).createDashedStroke (dp, path,
                                                             (const float[]) { 5.0f, 4.0f }, 2);
            g.strokePath (dp, juce::PathStrokeType (1.5f));
        }
        else
            g.strokePath (path, juce::PathStrokeType (1.8f));

        drawGlyph (g, xOf (r, p.f0Hz), yOfLevel (r, 0.0f), col, gl);
        for (int fi = 0; fi < 3; ++fi)
            if (p.F[fi] > 0.0f)
                drawGlyph (g, xOf (r, p.F[fi]), yOfLevel (r, p.L[fi]), col, gl);

        // intonation whisker: the measured pitch range (f0 +- spread)
        if (p.f0SpreadSt > 0.05f && p.f0Hz > 0.0f)
        {
            const float y  = r.getY() + r.getHeight() * kTop / (kTop - kFloor);
            const float x1 = xOf (r, p.f0Hz * std::pow (2.0, -p.f0SpreadSt / 12.0));
            const float x2 = xOf (r, p.f0Hz * std::pow (2.0,  p.f0SpreadSt / 12.0));
            g.drawLine (x1, y, x2, y, 1.4f);
            g.drawLine (x1, y - 3.0f, x1, y + 3.0f, 1.4f);
            g.drawLine (x2, y - 3.0f, x2, y + 3.0f, 1.4f);
        }
    }

    // Correction ◤◢ band from Current to Estimated (spec 4.5): a diagonal
    // stripe fill between the two formant positions per F1/F2/F3.
    // Direction of the ◤◢ pattern indicates sign of the shift; density
    // and length reflect the correction magnitude.
    static void drawCorrectionHatch (juce::Graphics& g, juce::Rectangle<float> r,
                                     const VoiceProfile& a, const VoiceProfile& b)
    {
        const juce::Colour cyan (0x9967d8e6);   // Correction Cyan @ ~60% alpha
        for (int fi = 0; fi < 3; ++fi)
        {
            if (a.F[fi] <= 0.0f || b.F[fi] <= 0.0f) continue;
            const float x1 = xOf (r, a.F[fi]);
            const float x2 = xOf (r, b.F[fi]);
            if (std::abs (x2 - x1) < 2.0f) continue;
            const float y1 = std::min (yOfLevel (r, a.L[fi]), yOfLevel (r, b.L[fi])) - 3.0f;
            const float y2 = std::max (yOfLevel (r, a.L[fi]), yOfLevel (r, b.L[fi])) + 3.0f;
            juce::Rectangle<float> band (std::min (x1, x2), y1,
                                         std::abs (x2 - x1), std::max (7.0f, y2 - y1));
            g.saveState();
            g.reduceClipRegion (band.toNearestInt());
            const bool rightward = x2 > x1;             // positive shift -> ◤◢ →
            const float step = 6.0f;
            g.setColour (cyan);
            for (float t = band.getX() - band.getHeight();
                 t < band.getRight() + band.getHeight(); t += step)
            {
                // ◤◢: two connected triangles pointing in the shift direction
                juce::Path tri;
                const float dx = rightward ? 3.0f : -3.0f;
                tri.startNewSubPath (t, band.getBottom());
                tri.lineTo (t + dx, band.getY());
                tri.lineTo (t + 2.0f * dx, band.getBottom());
                tri.closeSubPath();
                g.strokePath (tri, juce::PathStrokeType (1.0f));
            }
            g.restoreState();
        }
    }
};

// MATCHING tab (v0.28.0, Phase 1 of the Matching redesign spec):
//   1) record CURRENT (your voice, the top action of the panel)
//   2) pick a built-in sample TARGET or load a target file / .vmprofile
//   3) MATCH  -> MatchingEngine::autoSet writes the derived parameters
//   4) MATCH AGAIN  -> record the CONVERTED output, engine.refine nudges
//      the parameters by the damped residual, iteration counter advances
//   5) SAVE PRESET -> writes a normal .vmpreset (same file format as the
//      PRESETS tab); no separate "matching result" file type.
//
// The formulas (auto-set / refine, kPitchBias, kRangeBoost, jlimit ranges,
// damping) live in dsp/MatchingEngine.h and are byte-exact copies of the
// v0.27.0 AnalyzePanel behaviour. The panel only sequences the UI, drives
// captures, applies changes through history.group / isParamLocked, and
// updates the graph. See VoxMorph_Matching_UI_Design_Spec.txt sections
// 17 / 18 for the phased plan; Phase 2 (analyzeDetailed + timeline /
// density / scatter renderers) and Phase 3 (parameter registry, matched
// AEIOU proposals) are not in this commit and are tracked in HANDOVER.
class MatchingPanel : public juce::Component, private juce::Timer
{
public:
    explicit MatchingPanel (VoxMorphProcessor& p) : proc (p)
    {
        // ── session rail: three tiny phase labels + iteration counter ──
        auto initRail = [this] (juce::Label& l, const char* txt)
        {
            l.setText (txt, juce::dontSendNotification);
            l.setFont (juce::Font (juce::FontOptions (11.5f, juce::Font::bold)));
            l.setColour (juce::Label::textColourId, juce::Colour (0xff9aa5a2));
            l.setJustificationType (juce::Justification::centred);
            addAndMakeVisible (l);
        };
        initRail (railCurrent, "01 CURRENT");
        initRail (railTarget,  "02 TARGET");
        initRail (railMatch,   "03 MATCH");
        railIter.setFont (juce::Font (juce::FontOptions (11.5f)));
        railIter.setColour (juce::Label::textColourId, juce::Colour (0xff45bda5));
        railIter.setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (railIter);

        // ── section headings (bilingual short lines) ──
        auto initHeading = [this] (juce::Label& l, const char* t)
        {
            l.setText (t, juce::dontSendNotification);
            l.setFont (juce::Font (juce::FontOptions (13.5f, juce::Font::bold)));
            l.setColour (juce::Label::textColourId, juce::Colour (0xff45bda5));
            addAndMakeVisible (l);
        };
        initHeading (hCurrent, "Current / あなたの声");
        initHeading (hTarget,  "Target / 目標の声");

        for (auto* b : { &recBtn, &loadBtn, &playBtn, &saveProfBtn,
                         &matchBtn, &refineBtn, &savePresetBtn, &savePresetOkBtn,
                         &savePresetCancelBtn })
            addAndMakeVisible (*b);
        for (auto* c : { &recPlayChk })
        {
            c->setTooltip (juce::String::fromUTF8 (
                "When checked, the target file plays while you record.\n"
                "チェックすると録音と同時にターゲットを再生します。"));
            addAndMakeVisible (*c);
        }
        srcChip.setJustificationType (juce::Justification::centred);
        srcChip.setColour (juce::Label::textColourId, juce::Colour (0xff45bda5));
        srcChip.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
        srcChip.setText ("IN", juce::dontSendNotification);
        srcChip.setTooltip (juce::String::fromUTF8 (
            "Current source: microphone input (after Pre FX and Noise Gate, "
            "before VoxMorph conversion). See spec 2.4.\n"
            "Currentの音源: マイク入力(Pre FXとNoise Gateの後、VoxMorph変換の前)。"));
        addAndMakeVisible (srcChip);

        // Sample target dropdown -- Matching works out of the box, no file
        // needed (spec 2.3). Initial selection = index 0 (Feminine Standard).
        int nSamples = 0;
        const auto* samples = getSampleTargets (nSamples);
        for (int i = 0; i < nSamples; ++i)
            sampleBox.addItem (samples[i].displayEn, i + 1);
        sampleBox.setSelectedId (1, juce::dontSendNotification);
        sampleBox.setTooltip (juce::String::fromUTF8 (
            "Built-in target voice profiles. Pick one and press MATCH; no audio "
            "file needed.\n組み込みのターゲット声プロファイル。ファイルなしで"
            "選んでMATCHが押せます。"));
        sampleBox.onChange = [this] { loadSampleTarget(); };
        addAndMakeVisible (sampleBox);

        durBox.addItem ("5 s",  5);
        durBox.addItem ("10 s", 10);
        durBox.addItem ("15 s", 15);
        durBox.setSelectedId (10, juce::dontSendNotification);
        durBox.setTooltip (juce::String::fromUTF8 (
            "Recording length. Longer = more frames = a steadier profile.\n"
            "録音時間。長いほど分析フレームが増え、プロファイルが安定します。"));
        addAndMakeVisible (durBox);

        for (auto* l : { &p1Lbl, &p2Lbl, &pCLbl, &outLbl, &matchStatus, &saveHint })
        {
            l->setJustificationType (juce::Justification::topLeft);
            l->setFont (juce::Font (juce::FontOptions (12.0f)));
            addAndMakeVisible (*l);
        }
        matchStatus.setFont (juce::Font (juce::FontOptions (11.5f, juce::Font::bold)));
        matchStatus.setColour (juce::Label::textColourId, juce::Colour (0xff45bda5));
        matchStatus.setJustificationType (juce::Justification::centred);

        graph.you       = &prof1;
        graph.target    = &prof2;
        graph.estimated = &profE;
        graph.conv      = &profC;
        graph.param = [this] (const char* id)
        {
            auto* v = proc.apvts.getRawParameterValue (id);
            return v != nullptr ? v->load() : 0.0f;
        };
        addAndMakeVisible (graph);

        recBtn.setButtonText ("Record Current");
        recBtn.setTooltip (juce::String::fromUTF8 (
            "Records your microphone input for the CURRENT profile.\n"
            "マイク入力を録音してCurrentプロファイルにします。"));
        loadBtn.setButtonText ("Load Target File...");
        loadBtn.setTooltip (juce::String::fromUTF8 (
            "Load a voice audio file (wav/aiff/mp3/m4a/flac, first 60 s) or a "
            ".vmprofile as the target.\n音声ファイル(先頭60秒まで)または.vmprofileを"
            "ターゲットとして読み込みます。"));
        playBtn.setButtonText ("Play");
        playBtn.setTooltip (juce::String::fromUTF8 (
            "Preview the loaded target audio through the plugin output.\n"
            "読み込んだファイルを出力から再生します。"));
        saveProfBtn.setButtonText ("Save Profile...");
        saveProfBtn.setTooltip (juce::String::fromUTF8 (
            "Save the currently selected target as a .vmprofile (audio-less).\n"
            "現在のターゲットを.vmprofile(音声なし)として保存します。"));
        matchBtn.setButtonText ("MATCH");
        matchBtn.setTooltip (juce::String::fromUTF8 (
            "Auto-Set: derive parameters from the Current -> Target difference. "
            "One Undo step; locked parameters keep their values.\n"
            "CurrentとTargetの差からパラメータを算出して書き込みます。1 Undo、"
            "ロック項目は保持。"));
        refineBtn.setButtonText ("MATCH AGAIN");
        refineBtn.setTooltip (juce::String::fromUTF8 (
            "Record the CONVERTED output voice and nudge the parameters by the "
            "damped residual. Iterate until close.\n変換後の声を録音し、"
            "目標との残差でパラメータを再調整します。繰り返し可。"));
        savePresetBtn.setButtonText ("SAVE PRESET");
        savePresetBtn.setTooltip (juce::String::fromUTF8 (
            "Save the current parameter set as a normal preset (.vmpreset). "
            "Available from the PRESETS tab too.\n現在の設定を通常のプリセット"
            "(.vmpreset)として保存します。PRESETSタブでも読めます。"));
        savePresetOkBtn.setButtonText ("Save");
        savePresetCancelBtn.setButtonText ("Cancel");
        saveNameEdit.setTextToShowWhenEmpty ("preset name", juce::Colours::grey);
        addAndMakeVisible (saveNameEdit);

        recBtn.onClick = [this]
        {
            if (recPlayChk.getToggleState() && ! startPlayForCapture())
                return;
            proc.capFromOutput = false;
            startCapture (recBtn, waitingCapture);
        };
        refineBtn.onClick = [this]
        {
            if (! prof2.valid())
            {
                status (juce::String::fromUTF8 ("ターゲットを先に選択/読込してください。"));
                return;
            }
            proc.capFromOutput = true;
            startCapture (refineBtn, waitingRefine);
        };
        loadBtn.onClick  = [this] { loadFile(); };
        playBtn.onClick  = [this]
        {
            if (proc.prevPos.load() >= 0) proc.prevPos = -1;
            else if (proc.prevLen.load() > 0) proc.prevPos = 0;
        };
        saveProfBtn.onClick   = [this] { saveTargetProfile(); };
        matchBtn.onClick      = [this] { doMatch(); };
        savePresetBtn.onClick = [this] { showSavePreset (true); };
        savePresetOkBtn.onClick     = [this] { savePreset(); };
        savePresetCancelBtn.onClick = [this] { showSavePreset (false); };

        loadSampleTarget();     // pre-populate prof2 from the initial sample
        showSavePreset (false); // hide the name editor
        updateMatchStatus();
        startTimerHz (10);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (16, 10);

        // Session rail (top): three phase labels + iteration counter on right
        auto rail = r.removeFromTop (22);
        {
            auto right = rail.removeFromRight (110);
            railIter.setBounds (right);
            const int cw = rail.getWidth() / 3;
            railCurrent.setBounds (rail.removeFromLeft (cw));
            railTarget .setBounds (rail.removeFromLeft (cw));
            railMatch  .setBounds (rail);
        }
        r.removeFromTop (4);

        // CURRENT section: heading + record row + profile label
        hCurrent.setBounds (r.removeFromTop (20));
        auto row1 = r.removeFromTop (30);
        recBtn.setBounds (row1.removeFromLeft (150).withHeight (26));
        durBox.setBounds (row1.removeFromLeft (72).withHeight (26).translated (8, 0));
        recPlayChk.setBounds (row1.removeFromLeft (150).withHeight (26).translated (12, 0));
        srcChip.setBounds (row1.removeFromRight (34).withHeight (18).translated (-2, 4));
        p1Lbl.setBounds (r.removeFromTop (32).withTrimmedLeft (2));

        r.removeFromTop (4);

        // TARGET section: heading + sample + file row + profile label
        hTarget.setBounds (r.removeFromTop (20));
        auto row2 = r.removeFromTop (30);
        sampleBox.setBounds (row2.removeFromLeft (200).withHeight (26));
        loadBtn.setBounds   (row2.removeFromLeft (170).withHeight (26).translated (8, 0));
        playBtn.setBounds   (row2.removeFromLeft (60) .withHeight (26).translated (12, 0));
        saveProfBtn.setBounds (row2.removeFromLeft (130).withHeight (26).translated (12, 0));
        p2Lbl.setBounds (r.removeFromTop (32).withTrimmedLeft (2));

        r.removeFromTop (4);

        // reserve the match dock at the bottom
        auto matchDock = r.removeFromBottom (60);

        // status + inline save-preset editor at the very bottom
        auto footer = r.removeFromBottom (savePresetVisible ? 60 : 24);
        outLbl.setBounds (footer.removeFromTop (24).withTrimmedLeft (2));
        if (savePresetVisible)
        {
            auto sr = footer;
            saveHint.setBounds (sr.removeFromTop (16).withTrimmedLeft (2));
            saveNameEdit.setBounds        (sr.removeFromLeft (240).withHeight (24));
            savePresetOkBtn.setBounds     (sr.removeFromLeft (80) .withHeight (24).translated (10, 0));
            savePresetCancelBtn.setBounds (sr.removeFromLeft (80) .withHeight (24).translated (14, 0));
        }
        r.removeFromBottom (4);

        // Converted profile compact caption
        pCLbl.setBounds (r.removeFromBottom (28).withTrimmedLeft (2));

        // Graph fills what's left
        graph.setBounds (r.reduced (0, 2));

        // Match dock layout
        {
            auto d = matchDock.reduced (0, 6);
            matchBtn.setBounds       (d.removeFromLeft (140).withHeight (36));
            refineBtn.setBounds      (d.removeFromLeft (170).withHeight (36).translated (10, 0));
            savePresetBtn.setBounds  (d.removeFromRight (150).withHeight (36));
            matchStatus.setBounds    (d.reduced (10, 6));
        }
    }

private:
    // ── captures ─────────────────────────────────────────────────────────
    void startCapture (juce::TextButton& b, bool& waitFlag)
    {
        const double sr = proc.getSampleRate() > 0 ? proc.getSampleRate() : 48000.0;
        proc.capTarget = (int) (sr * durBox.getSelectedId());
        proc.capLen = 0;
        proc.capturing = true;
        waitFlag = true;
        savedButtonText = b.getButtonText();
        b.setButtonText ("Recording... speak!");
    }

    bool startPlayForCapture()
    {
        if (proc.prevLen.load() <= 0)
        {
            status (juce::String::fromUTF8 ("ターゲット音声を読み込んでから使ってください。"));
            return false;
        }
        proc.prevPos = 0;
        playStartedByCapture = true;
        return true;
    }

    void stopPlayIfStartedByCapture()
    {
        if (playStartedByCapture) { proc.prevPos = -1; playStartedByCapture = false; }
    }

    VoiceProfile analyzeCapture() const
    {
        return VoiceAnalyzer::analyze (proc.capBuf.data(), proc.capLen.load(),
                                       proc.getSampleRate() > 0 ? proc.getSampleRate() : 48000.0);
    }

    void timerCallback() override
    {
        if (waitingCapture && ! proc.capturing.load())
        {
            waitingCapture = false;
            stopPlayIfStartedByCapture();
            recBtn.setButtonText ("Record Current");
            prof1 = analyzeCapture();
            proc.lastMyVoice = prof1;
            p1Lbl.setText (juce::String::fromUTF8 ("Current: ") + fmt (prof1),
                           juce::dontSendNotification);
            refreshEstimated();
            graph.repaint();
            updateMatchStatus();
        }
        if (waitingRefine && ! proc.capturing.load())
        {
            waitingRefine = false;
            stopPlayIfStartedByCapture();
            refineBtn.setButtonText ("MATCH AGAIN");
            profC = analyzeCapture();
            pCLbl.setText (juce::String::fromUTF8 ("Matched: ") + fmt (profC),
                           juce::dontSendNotification);
            doRefine();
            graph.repaint();
        }
        playBtn.setButtonText (proc.prevPos.load() >= 0 ? "Stop" : "Play");
        playBtn.setEnabled (proc.prevLen.load() > 0);
    }

    // ── target loading ───────────────────────────────────────────────────
    void loadSampleTarget()
    {
        const int idx = std::max (0, sampleBox.getSelectedId() - 1);
        int n = 0;
        const auto* samples = getSampleTargets (n);
        if (n <= 0) return;
        const auto& s = samples[std::min (idx, n - 1)];
        prof2 = s.profile;
        proc.lastTarget = prof2;
        proc.prevLen = 0;    // sample targets have no audio to Play
        proc.prevPos = -1;
        currentTargetName = juce::String::fromUTF8 (s.displayJp) + " (built-in)";
        p2Lbl.setText (juce::String::fromUTF8 ("Target: ") + currentTargetName + "\n" + fmt (prof2),
                       juce::dontSendNotification);
        // ターゲットが変わると Matched は無効化(iterationはリセット、
        // Currentは保持) -- spec 6.2 "ターゲット変更"
        profC = VoiceProfile{};
        pCLbl.setText ("", juce::dontSendNotification);
        iteration = 0;
        refreshEstimated();
        graph.repaint();
        updateMatchStatus();
    }

    void loadFile()
    {
        chooser = std::make_unique<juce::FileChooser> ("Select the target voice file or profile",
                      juce::File(), "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.m4a;*.ogg;*.vmprofile");
        chooser->launchAsync (juce::FileBrowserComponent::openMode
                            | juce::FileBrowserComponent::canSelectFiles,
            [this] (const juce::FileChooser& fc)
        {
            const auto file = fc.getResult();
            if (file == juce::File()) return;
            proc.prevPos = -1;

            if (file.hasFileExtension ("vmprofile"))
            {
                VoiceProfile p;
                if (auto xml = juce::XmlDocument::parse (file); xml != nullptr
                    && profileFromXml (*xml, p))
                {
                    prof2 = p;
                    proc.lastTarget = p;
                    proc.prevLen = 0;
                    currentTargetName = file.getFileNameWithoutExtension() + " (.vmprofile)";
                    p2Lbl.setText (juce::String::fromUTF8 ("Target: ") + currentTargetName
                                   + "\n" + fmt (prof2), juce::dontSendNotification);
                    profC = VoiceProfile{};
                    pCLbl.setText ("", juce::dontSendNotification);
                    iteration = 0;
                    refreshEstimated();
                    graph.repaint();
                    updateMatchStatus();
                }
                else
                    status (juce::String::fromUTF8 ("プロファイルを読み込めませんでした: ")
                            + file.getFileName());
                return;
            }
            juce::AudioFormatManager fm;
            fm.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (file));
            if (rd == nullptr || rd->sampleRate <= 0)
            {
                status ("Could not read: " + file.getFileName());
                return;
            }
            const int nIn = (int) std::min<juce::int64> (rd->lengthInSamples,
                                                         (juce::int64) (rd->sampleRate * 60.0));
            juce::AudioBuffer<float> tb ((int) rd->numChannels, nIn);
            rd->read (&tb, 0, nIn, 0, true, true);

            const double sr    = proc.getSampleRate() > 0 ? proc.getSampleRate() : 48000.0;
            const double ratio = rd->sampleRate / sr;
            const int nOut = std::min ((int) proc.prevBuf.size(),
                                       (int) ((nIn - 2) / ratio));
            const int nch = tb.getNumChannels();
            for (int i = 0; i < nOut; ++i)
            {
                const double pos = i * ratio;
                const int   i0 = (int) pos;
                const float t  = (float) (pos - i0);
                float sum = 0.0f;
                for (int c = 0; c < nch; ++c)
                    sum += tb.getSample (c, i0) * (1.0f - t)
                         + tb.getSample (c, std::min (i0 + 1, nIn - 1)) * t;
                proc.prevBuf[(size_t) i] = sum / (float) nch;
            }
            proc.prevLen = nOut;
            prof2 = VoiceAnalyzer::analyze (proc.prevBuf.data(), nOut, sr);
            proc.lastTarget = prof2;
            currentTargetName = file.getFileName();
            p2Lbl.setText (juce::String::fromUTF8 ("Target: ") + currentTargetName
                           + "\n" + fmt (prof2), juce::dontSendNotification);
            profC = VoiceProfile{};
            pCLbl.setText ("", juce::dontSendNotification);
            iteration = 0;
            refreshEstimated();
            graph.repaint();
            updateMatchStatus();
        });
    }

    void saveTargetProfile()
    {
        if (! prof2.valid())
        {
            status (juce::String::fromUTF8 ("有効なターゲットプロファイルがありません。"));
            return;
        }
        profChooser = std::make_unique<juce::FileChooser> (
            "Save target profile", juce::File(), "*.vmprofile");
        profChooser->launchAsync (juce::FileBrowserComponent::saveMode
                                | juce::FileBrowserComponent::canSelectFiles
                                | juce::FileBrowserComponent::warnAboutOverwriting,
            [this] (const juce::FileChooser& fc)
        {
            auto file = fc.getResult();
            if (file == juce::File()) return;
            if (! file.hasFileExtension ("vmprofile"))
                file = file.withFileExtension (".vmprofile");
            if (profileToXml (prof2)->writeTo (file))
                status (juce::String::fromUTF8 ("ターゲットプロファイルを保存しました: ")
                        + file.getFileName());
            else
                status (juce::String::fromUTF8 ("保存に失敗しました。"));
        });
    }

    // ── Match / Refine (calls MatchingEngine, applies through history.group)
    void doMatch()
    {
        if (! prof1.valid() || ! prof2.valid())
        {
            status (juce::String::fromUTF8 ("CurrentとTargetの両方が必要です。"));
            return;
        }
        const auto proposal = MatchingEngine::autoSet (prof1, prof2);
        applyProposal (proposal, /*isRefine*/ false);
        refreshEstimated();
        graph.repaint();
    }

    void doRefine()
    {
        if (! prof2.valid() || ! profC.valid())
        {
            status (juce::String::fromUTF8 ("目標と変換後の声の両方が必要です。"));
            return;
        }
        auto get = [this] (const char* id)
        {
            auto* v = proc.apvts.getRawParameterValue (id);
            return v != nullptr ? v->load() : 0.0f;
        };
        const auto proposal = MatchingEngine::refine (prof2, profC, get);
        applyProposal (proposal, /*isRefine*/ true);
        refreshEstimated();
    }

    void applyProposal (const MatchingEngine::Proposal& r, bool isRefine)
    {
        nSet = nLocked = 0;
        proc.history.group ([&]
        {
            for (int i = 0; i < r.count; ++i)
            {
                const auto& c = r.changes[i];
                if (! c.apply) continue;
                setP (c.id, c.value);
            }
        });
        if (isRefine) ++iteration;
        juce::String line;
        if (isRefine)
            line = juce::String::formatted (
                "residual: pitch %+.1f st  formant %+.1f st  tilt %+.1f dB",
                r.pitch, r.formant, r.tilt);
        else
        {
            line = juce::String::formatted ("pitch %+.1f st   formant %+.1f st   tilt %+.1f dB",
                                            r.pitch, r.formant, r.tilt);
            if (r.rangeApplied)
                line += juce::String::formatted ("   range %.0f%%  center %.0f Hz",
                                                 r.range, r.center);
            line += juce::String::formatted ("\nhigh-range start %.0f Hz   pitch floor %.0f Hz",
                                             r.hifreq, r.pitchfloor);
        }
        status ((isRefine ? juce::String::fromUTF8 ("MATCH AGAIN 完了 — ")
                          : juce::String::fromUTF8 ("MATCH 完了 — "))
                + line + "\n" + setSummary());
        updateMatchStatus();
    }

    void setP (const char* id, float v)
    {
        if (auto* p = proc.apvts.getParameter (id))
        {
            if (proc.isParamLocked (id)) { ++nLocked; return; }
            ++nSet;
            p->beginChangeGesture();
            p->setValueNotifyingHost (p->convertTo0to1 (v));
            p->endChangeGesture();
        }
    }

    void refreshEstimated()
    {
        // Predicted profile from current parameters (spec 7.3): the graph
        // renders it dashed with diamond glyphs so it's not confused with
        // an actual measurement.
        if (! prof1.valid()) { profE = VoiceProfile{}; return; }
        profE = MatchingEngine::predictEstimated (prof1,
                    [this] (const char* id) { return graph.param (id); });
    }

    // ── Save Preset (inline; same .vmpreset format as the PRESETS tab) ─
    static juce::File presetDir()
    {
        auto d = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                    .getChildFile ("VoxMorph").getChildFile ("Presets");
        d.createDirectory();
        return d;
    }

    void showSavePreset (bool visible)
    {
        savePresetVisible = visible;
        saveNameEdit       .setVisible (visible);
        savePresetOkBtn    .setVisible (visible);
        savePresetCancelBtn.setVisible (visible);
        saveHint           .setVisible (visible);
        if (visible)
        {
            saveHint.setText (juce::String::fromUTF8 (
                "Preset name (goes to PRESETS tab). / プリセット名 (PRESETSタブから読めます)"),
                juce::dontSendNotification);
            saveNameEdit.grabKeyboardFocus();
        }
        resized();
    }

    void savePreset()
    {
        auto name = saveNameEdit.getText().trim();
        if (name.isEmpty())
        {
            status (juce::String::fromUTF8 ("プリセット名を入力してください。"));
            return;
        }
        const auto file = presetDir().getChildFile (juce::File::createLegalFileName (name)
                                                    + ".vmpreset");
        if (auto xml = proc.apvts.copyState().createXml(); xml != nullptr && xml->writeTo (file))
        {
            saveNameEdit.clear();
            showSavePreset (false);
            status (juce::String::fromUTF8 ("プリセット保存: ") + name);
        }
        else
            status (juce::String::fromUTF8 ("保存に失敗しました。"));
    }

    // ── formatting / status ─────────────────────────────────────────────
    static juce::String fmt (const VoiceProfile& pr)
    {
        if (! pr.valid())
            return juce::String::fromUTF8 ("(--) 有声区間が不足しています");
        return juce::String::formatted ("F0 %.0f Hz  spread %.1f st   "
                                        "F1/F2/F3 %.0f/%.0f/%.0f Hz   "
                                        "L %+.0f/%+.0f/%+.0f dB   tilt %+.1f dB",
                                        pr.f0Hz, pr.f0SpreadSt,
                                        pr.F[0], pr.F[1], pr.F[2],
                                        pr.L[0], pr.L[1], pr.L[2], pr.tiltDb);
    }

    juce::String setSummary() const
    {
        auto s = juce::String::fromUTF8 ("自動設定: ") + juce::String (nSet)
               + juce::String::fromUTF8 ("項目を更新");
        if (nLocked > 0)
            s += juce::String::fromUTF8 ("、") + juce::String (nLocked)
               + juce::String::fromUTF8 ("項目はロック保持");
        return s;
    }

    void status (const juce::String& s) { outLbl.setText (s, juce::dontSendNotification); }

    void updateMatchStatus()
    {
        const bool canMatch  = prof1.valid() && prof2.valid();
        const bool canRefine = canMatch && profC.valid();
        matchBtn .setEnabled (canMatch);
        refineBtn.setEnabled (canMatch);   // permits recording converted after MATCH
        auto s = juce::String::fromUTF8 (canMatch ? "READY TO MATCH" : "RECORD CURRENT");
        if (nSet + nLocked > 0)
            s = juce::String (nSet) + juce::String::fromUTF8 (" APPLIED")
              + (nLocked > 0 ? juce::String (" · ") + juce::String (nLocked)
                             + juce::String::fromUTF8 (" LOCKED") : juce::String());
        matchStatus.setText (s, juce::dontSendNotification);
        railIter.setText (iteration > 0 ? juce::String::formatted ("iter %d", iteration)
                                        : juce::String(), juce::dontSendNotification);
    }

    // ── members ──────────────────────────────────────────────────────────
    VoxMorphProcessor& proc;
    VoiceProfile prof1, prof2, profE, profC;   // Current / Target / Estimated / Matched
    int nSet = 0, nLocked = 0;
    int iteration = 0;
    ProfileGraph graph;
    juce::String currentTargetName, savedButtonText;

    bool waitingCapture = false, waitingRefine = false, playStartedByCapture = false;
    bool savePresetVisible = false;

    juce::Label railCurrent, railTarget, railMatch, railIter;
    juce::Label hCurrent, hTarget;
    juce::Label p1Lbl, p2Lbl, pCLbl, outLbl, matchStatus, saveHint, srcChip;

    juce::ComboBox sampleBox, durBox;
    juce::ToggleButton recPlayChk { "With target play" };

    juce::TextButton recBtn { "Record Current" }, loadBtn { "Load Target File..." },
                     playBtn { "Play" }, saveProfBtn { "Save Profile..." },
                     matchBtn { "MATCH" }, refineBtn { "MATCH AGAIN" },
                     savePresetBtn { "SAVE PRESET" },
                     savePresetOkBtn { "Save" }, savePresetCancelBtn { "Cancel" };
    juce::TextEditor saveNameEdit;

    std::unique_ptr<juce::FileChooser> chooser, profChooser;
};

// PRESETS tab: file-based parameter snapshots, shared between the
// standalone app and the DAW plugins. One preset = the full APVTS state
// saved as XML in <userAppData>/VoxMorph/Presets/<name>.vmpreset.
// Selecting a preset in the dropdown loads it immediately.
class PresetPanel : public juce::Component
{
public:
    explicit PresetPanel (VoxMorphProcessor& p) : proc (p)
    {
        heading.setText ("PRESETS", juce::dontSendNotification);
        heading.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
        heading.setColour (juce::Label::textColourId, juce::Colour (0xff45bda5));
        addAndMakeVisible (heading);

        help.setJustificationType (juce::Justification::topLeft);
        help.setFont (juce::Font (juce::FontOptions (12.5f)));
        help.setText (juce::String::fromUTF8 (
            "現在の全パラメータをプリセットとして保存・呼び出しできます。プルダウンで選ぶと即座に\n"
            "読み込まれます。プリセットはファイル保存なのでスタンドアロンとDAWプラグインで共通です。"),
            juce::dontSendNotification);
        addAndMakeVisible (help);

        presetBox.setTextWhenNothingSelected (juce::String::fromUTF8 ("-- プリセットを選択 --"));
        presetBox.onChange = [this] { previewSelected(); };
        addAndMakeVisible (presetBox);

        loadBtn.setTooltip (juce::String::fromUTF8 ("選択中のプリセットを実際に読み込んで適用します。"));
        loadBtn.onClick = [this] { loadSelected(); };
        addAndMakeVisible (loadBtn);

        deleteBtn.setTooltip (juce::String::fromUTF8 ("選択中のプリセットを削除します(確認あり)。"));
        deleteBtn.onClick = [this] { deleteSelected(); };
        addAndMakeVisible (deleteBtn);

        // preview graph: a standard reference voice (mint) vs how this
        // preset's settings would transform it (pink) — input-independent
        pGraph.you  = &pvBase;
        pGraph.conv = &pvConv;
        pGraph.param = [this] (const char* id)
        {
            return juce::String (id) == "hifreq" ? pvHifreq
                 : juce::String (id) == "pitchfloor" ? pvFloor : 0.0f;
        };
        addAndMakeVisible (pGraph);
        pvLbl.setJustificationType (juce::Justification::topLeft);
        pvLbl.setFont (juce::Font (juce::FontOptions (11.0f)));
        pvLbl.setColour (juce::Label::textColourId, juce::Colour (0xff9aa5a2));
        pvLbl.setText (juce::String::fromUTF8 (
            "プレビュー: 標準的な声(ミント)がこのプリセットでどう変わるか(ピンク)のイメージ。"),
            juce::dontSendNotification);
        addAndMakeVisible (pvLbl);

        hProfiles.setText ("PROFILES", juce::dontSendNotification);
        hProfiles.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
        hProfiles.setColour (juce::Label::textColourId, juce::Colour (0xff45bda5));
        addAndMakeVisible (hProfiles);

        pNameEdit.setTextToShowWhenEmpty (juce::String::fromUTF8 ("プロファイル名"),
                                          juce::Colour (0xff9aa5a2));
        pNameEdit.setFont (juce::Font (juce::FontOptions (13.0f)));
        pNameEdit.setColour (juce::TextEditor::textColourId, juce::Colour (0xff2e2e32));
        pNameEdit.setColour (juce::TextEditor::backgroundColourId, juce::Colours::white);
        pNameEdit.setColour (juce::TextEditor::outlineColourId, juce::Colour (0xffdedede));
        addAndMakeVisible (pNameEdit);

        saveMyBtn.setTooltip (juce::String::fromUTF8 ("ANALYZEタブで測定したMyVoiceプロファイルを"
            "ファイル保存します(保存先を選択)。ANALYZEのLoad Target File...で読み込めます。"));
        saveMyBtn.onClick  = [this] { saveProfile (proc.lastMyVoice, "MyVoice"); };
        addAndMakeVisible (saveMyBtn);
        saveTgtBtn.setTooltip (juce::String::fromUTF8 ("ANALYZEタブのターゲットプロファイルを"
            "ファイル保存します。以後は音声ファイルなしでターゲットとして使えます(Play不可)。"));
        saveTgtBtn.onClick = [this] { saveProfile (proc.lastTarget, "Target"); };
        addAndMakeVisible (saveTgtBtn);

        nameEdit.setTextToShowWhenEmpty (juce::String::fromUTF8 ("新しいプリセット名"),
                                         juce::Colour (0xff9aa5a2));
        nameEdit.setFont (juce::Font (juce::FontOptions (13.0f)));
        nameEdit.setColour (juce::TextEditor::textColourId, juce::Colour (0xff2e2e32));
        nameEdit.setColour (juce::TextEditor::backgroundColourId, juce::Colours::white);
        nameEdit.setColour (juce::TextEditor::outlineColourId, juce::Colour (0xffdedede));
        addAndMakeVisible (nameEdit);

        saveBtn.setTooltip (juce::String::fromUTF8 ("現在の設定を上の名前で保存します(同名は上書き)。"
                                                    "名前が空なら選択中のプリセットに上書きします。"));
        saveBtn.onClick = [this] { save(); };
        addAndMakeVisible (saveBtn);

        resetBtn.setTooltip (juce::String::fromUTF8 ("全パラメータを初期値に戻します(プリセットは消えません)。"));
        resetBtn.onClick = [this] { resetAll(); };
        addAndMakeVisible (resetBtn);

        status.setJustificationType (juce::Justification::topLeft);
        status.setFont (juce::Font (juce::FontOptions (12.5f)));
        addAndMakeVisible (status);

        pathLbl.setJustificationType (juce::Justification::topLeft);
        pathLbl.setFont (juce::Font (juce::FontOptions (10.5f)));
        pathLbl.setColour (juce::Label::textColourId, juce::Colour (0xff9aa5a2));
        pathLbl.setText (juce::String::fromUTF8 ("保存先: ") + presetDir().getFullPathName(),
                         juce::dontSendNotification);
        addAndMakeVisible (pathLbl);

        refreshList();
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (16, 12);
        heading.setBounds (r.removeFromTop (24));
        help.setBounds (r.removeFromTop (40));
        auto r1 = r.removeFromTop (34);
        presetBox.setBounds (r1.removeFromLeft (250).withHeight (26));
        loadBtn.setBounds   (r1.removeFromLeft (80).withHeight (26).translated (8, 0));
        deleteBtn.setBounds (r1.removeFromLeft (80).withHeight (26).translated (16, 0));
        auto r2 = r.removeFromTop (34);
        nameEdit.setBounds (r2.removeFromLeft (250).withHeight (26));
        saveBtn.setBounds  (r2.removeFromLeft (80).withHeight (26).translated (8, 0));
        resetBtn.setBounds (r2.removeFromLeft (170).withHeight (26).translated (16, 0));
        pGraph.setBounds (r.removeFromTop (juce::jmax (120, r.getHeight() - 190)));
        pvLbl.setBounds (r.removeFromTop (18));
        r.removeFromTop (6);
        hProfiles.setBounds (r.removeFromTop (22));
        auto r3 = r.removeFromTop (34);
        pNameEdit.setBounds  (r3.removeFromLeft (200).withHeight (26));
        saveMyBtn.setBounds  (r3.removeFromLeft (160).withHeight (26).translated (8, 0));
        saveTgtBtn.setBounds (r3.removeFromLeft (160).withHeight (26).translated (16, 0));
        status.setBounds (r.removeFromTop (36).withTrimmedTop (6));
        pathLbl.setBounds (r.removeFromTop (18));
    }

private:
    static juce::File presetDir()
    {
        auto d = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                     .getChildFile ("VoxMorph").getChildFile ("Presets");
        d.createDirectory();
        return d;
    }

    void refreshList (const juce::String& select = {})
    {
        files = presetDir().findChildFiles (juce::File::findFiles, false, "*.vmpreset");
        std::sort (files.begin(), files.end(),
                   [] (const juce::File& a, const juce::File& b)
                   { return a.getFileName().compareIgnoreCase (b.getFileName()) < 0; });
        presetBox.clear (juce::dontSendNotification);
        int id = 1, selId = 0;
        for (auto& f : files)
        {
            presetBox.addItem (f.getFileNameWithoutExtension(), id);
            if (f.getFileNameWithoutExtension() == select) selId = id;
            ++id;
        }
        if (selId > 0)
            presetBox.setSelectedId (selId, juce::dontSendNotification);
    }

    void loadSelected()
    {
        const int idx = presetBox.getSelectedId() - 1;
        if (idx < 0 || idx >= files.size())
        {
            setStatus (juce::String::fromUTF8 ("プリセットを選択してください。"));
            return;
        }
        auto xml = juce::XmlDocument::parse (files.getReference (idx));
        if (xml == nullptr || ! xml->hasTagName (proc.apvts.state.getType()))
        {
            setStatus (juce::String::fromUTF8 ("読み込みに失敗しました(壊れたファイル?)"));
            return;
        }

        // Apply per parameter instead of replaceState: locked sections keep
        // their current values, and the whole load is ONE undo step.
        // Parameters missing from the file fall back to their defaults
        // (matching the old full-state replace for complete presets).
        int applied = 0, lockedKept = 0;
        proc.history.group ([&]
        {
            for (auto* p : proc.getParameters())
                if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
                {
                    float norm = rp->getDefaultValue();
                    if (auto* e = xml->getChildByAttribute ("id", rp->paramID))
                        norm = rp->convertTo0to1 ((float) e->getDoubleAttribute ("value"));
                    if (proc.isParamLocked (rp->paramID))
                    {
                        if (std::abs (norm - rp->getValue()) > 1.0e-4f) ++lockedKept;
                        continue;
                    }
                    if (norm != rp->getValue())
                    {
                        rp->beginChangeGesture();
                        rp->setValueNotifyingHost (norm);
                        rp->endChangeGesture();
                        ++applied;
                    }
                }
        });
        auto msg = juce::String::fromUTF8 ("読み込みました: ") + presetBox.getText()
                 + juce::String::fromUTF8 (" (") + juce::String (applied)
                 + juce::String::fromUTF8 ("項目を更新");
        if (lockedKept > 0)
            msg += juce::String::fromUTF8 ("、") + juce::String (lockedKept)
                 + juce::String::fromUTF8 ("項目はロック保持");
        setStatus (msg + juce::String::fromUTF8 (")。Undoで戻せます。"));
    }

    // preview: apply the preset's key settings to a standard reference voice
    void previewSelected()
    {
        const int idx = presetBox.getSelectedId() - 1;
        if (idx < 0 || idx >= files.size()) return;
        auto xml = juce::XmlDocument::parse (files.getReference (idx));
        if (xml == nullptr) return;

        auto defVal = [this] (const char* id)
        {
            auto* rp = proc.apvts.getParameter (id);
            return rp != nullptr ? rp->convertFrom0to1 (rp->getDefaultValue()) : 0.0f;
        };
        auto val = [&] (const char* id)
        {
            if (auto* e = xml->getChildByAttribute ("id", id))
                return (float) e->getDoubleAttribute ("value", defVal (id));
            return defVal (id);
        };

        pvBase = VoiceProfile();
        pvBase.f0Hz = 140.0f;  pvBase.f0SpreadSt = 2.0f;
        pvBase.F[0] = 500.0f;  pvBase.F[1] = 1500.0f;  pvBase.F[2] = 2500.0f;
        pvBase.L[0] = 0.0f;    pvBase.L[1] = -6.0f;    pvBase.L[2] = -12.0f;
        pvBase.voicedFrames = 99;

        pvConv = pvBase;
        pvConv.f0Hz       = pvBase.f0Hz * std::pow (2.0f, val ("pitch") / 12.0f);
        pvConv.f0SpreadSt = pvBase.f0SpreadSt * val ("range") * 0.01f;
        const char* sid[3] = { "f1shift", "f2shift", "f3shift" };
        const char* gid[3] = { "f1gain",  "f2gain",  "f3gain"  };
        float lmax = -1.0e9f;
        for (int i = 0; i < 3; ++i)
        {
            pvConv.F[i] = pvBase.F[i] * std::pow (2.0f, (val ("formant") + val (sid[i])) / 12.0f);
            pvConv.L[i] = pvBase.L[i] + val (gid[i]);
            lmax = std::max (lmax, pvConv.L[i]);
        }
        for (int i = 0; i < 3; ++i) pvConv.L[i] -= lmax;
        pvHifreq = val ("hifreq");
        pvFloor  = val ("pitchfloor");
        pGraph.repaint();
        setStatus (juce::String::fromUTF8 ("プレビュー表示中(Loadで適用): ") + presetBox.getText());
    }

    void saveProfile (const VoiceProfile& p, const char* kind)
    {
        if (! p.valid())
        {
            setStatus (juce::String::fromUTF8 ("先にANALYZEタブで測定してください: ")
                       + juce::String (kind));
            return;
        }
        // native save dialog: pick the folder and name freely; the preset
        // folder with the name field (or a default) is offered as a start
        juce::String name = pNameEdit.getText().trim();
        if (name.isEmpty()) name = juce::String (kind) + " Profile";
        const auto initial = presetDir().getChildFile (juce::File::createLegalFileName (name)
                                                       + ".vmprofile");
        profChooser = std::make_unique<juce::FileChooser> (
            juce::String::fromUTF8 ("プロファイルの保存先を選択"), initial, "*.vmprofile");
        profChooser->launchAsync (juce::FileBrowserComponent::saveMode
                                | juce::FileBrowserComponent::canSelectFiles
                                | juce::FileBrowserComponent::warnAboutOverwriting,
            [this, p] (const juce::FileChooser& fc)
            {
                auto file = fc.getResult();
                if (file == juce::File()) return;
                if (! file.hasFileExtension ("vmprofile"))
                    file = file.withFileExtension ("vmprofile");
                if (profileToXml (p)->writeTo (file))
                {
                    pNameEdit.clear();
                    setStatus (juce::String::fromUTF8 ("プロファイルを保存しました: ")
                               + file.getFullPathName());
                }
                else
                    setStatus (juce::String::fromUTF8 ("保存に失敗しました。"));
            });
    }

    void save()
    {
        juce::String name = nameEdit.getText().trim();
        if (name.isEmpty()) name = presetBox.getText().trim();
        if (name.isEmpty())
        {
            setStatus (juce::String::fromUTF8 ("プリセット名を入力してください。"));
            return;
        }
        const auto file = presetDir().getChildFile (juce::File::createLegalFileName (name)
                                                    + ".vmpreset");
        if (auto xml = proc.apvts.copyState().createXml(); xml != nullptr && xml->writeTo (file))
        {
            nameEdit.clear();
            refreshList (file.getFileNameWithoutExtension());
            setStatus (juce::String::fromUTF8 ("保存しました: ") + name);
        }
        else
            setStatus (juce::String::fromUTF8 ("保存に失敗しました。"));
    }

    void deleteSelected()
    {
        const int idx = presetBox.getSelectedId() - 1;
        if (idx < 0 || idx >= files.size())
        {
            setStatus (juce::String::fromUTF8 ("削除するプリセットを選択してください。"));
            return;
        }
        const auto file = files.getReference (idx);
        juce::NativeMessageBox::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon,
            "Delete preset",
            juce::String::fromUTF8 ("プリセットを削除しますか?\n") + file.getFileNameWithoutExtension(),
            this, juce::ModalCallbackFunction::create ([this, file] (int result)
            {
                if (result != 1) return;
                const auto name = file.getFileNameWithoutExtension();
                file.deleteFile();
                refreshList();
                setStatus (juce::String::fromUTF8 ("削除しました: ") + name);
            }));
    }

    void resetAll()
    {
        // ONE undo step; locked sections keep their values
        int kept = 0;
        proc.history.group ([&]
        {
            for (auto* p : proc.getParameters())
                if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
                {
                    if (proc.isParamLocked (rp->paramID)) { ++kept; continue; }
                    rp->beginChangeGesture();
                    rp->setValueNotifyingHost (rp->getDefaultValue());
                    rp->endChangeGesture();
                }
        });
        auto msg = juce::String::fromUTF8 ("全パラメータを初期値に戻しました");
        if (kept > 0)
            msg += juce::String::fromUTF8 ("(") + juce::String (kept)
                 + juce::String::fromUTF8 ("項目はロック保持)");
        setStatus (msg + juce::String::fromUTF8 ("。Undoで戻せます。"));
    }

    void setStatus (const juce::String& s) { status.setText (s, juce::dontSendNotification); }

    VoxMorphProcessor& proc;
    juce::Array<juce::File> files;
    juce::Label heading, help, status, pathLbl, pvLbl, hProfiles;
    juce::ComboBox presetBox;
    juce::TextEditor nameEdit, pNameEdit;
    juce::TextButton saveBtn { "Save" }, loadBtn { "Load" }, deleteBtn { "Delete" },
                     resetBtn { "Reset All to Defaults" },
                     saveMyBtn { "Save MyVoice Profile" }, saveTgtBtn { "Save Target Profile" };
    ProfileGraph pGraph;
    VoiceProfile pvBase, pvConv;
    float pvHifreq = 0.0f, pvFloor = 0.0f;
    std::unique_ptr<juce::FileChooser> profChooser;
};

// ASMR tab: pseudo-3D positioning. A sonar-style circular pad with a
// draggable dot; the dot's X gives constant-power L/R panning and the
// distance from the centre attenuates the volume ("further away").
// Centre = exactly unity (no effect). Drives the asmrx/asmry parameters.
class SonarPad : public juce::Component, private juce::Timer
{
public:
    explicit SonarPad (VoxMorphProcessor& p) : proc (p)
    {
        px = proc.apvts.getParameter ("asmrx");
        py = proc.apvts.getParameter ("asmry");
        startTimerHz (30);
    }

private:
    void timerCallback() override { if (isShowing()) repaint(); }

    juce::Rectangle<float> circleBounds() const
    {
        auto b = getLocalBounds().toFloat().reduced (14.0f);
        const float s = std::min (b.getWidth(), b.getHeight());
        return b.withSizeKeepingCentre (s, s);
    }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat().reduced (2.0f);
        g.setColour (juce::Colours::white);
        g.fillRoundedRectangle (b, 8.0f);
        g.setColour (juce::Colour (0xffe9d8dd));
        g.drawRoundedRectangle (b, 8.0f, 1.0f);

        const auto c = circleBounds();
        const auto ctr = c.getCentre();
        g.setColour (juce::Colour (0xffeef7f4));
        g.fillEllipse (c);
        g.setColour (juce::Colour (0xff9ed9c9));
        for (float f : { 1.0f, 2.0f / 3.0f, 1.0f / 3.0f })
            g.drawEllipse (c.withSizeKeepingCentre (c.getWidth() * f, c.getHeight() * f), 1.0f);
        g.setColour (juce::Colour (0x3354bda1));
        g.drawLine (c.getX(), ctr.y, c.getRight(), ctr.y, 1.0f);
        g.drawLine (ctr.x, c.getY(), ctr.x, c.getBottom(), 1.0f);

        g.setColour (juce::Colour (0xff9aa5a2));
        g.setFont (juce::Font (juce::FontOptions (10.5f)));
        g.drawText ("FRONT", (int) ctr.x - 24, (int) c.getY() - 13, 48, 12, juce::Justification::centred);
        g.drawText ("BACK",  (int) ctr.x - 24, (int) c.getBottom() + 1, 48, 12, juce::Justification::centred);
        g.drawText ("L", (int) c.getX() - 12,     (int) ctr.y - 6, 10, 12, juce::Justification::centred);
        g.drawText ("R", (int) c.getRight() + 3,  (int) ctr.y - 6, 10, 12, juce::Justification::centred);

        const float x = getX01 (px), y = getX01 (py);
        const float dx = ctr.x + x * c.getWidth() * 0.5f;
        const float dy = ctr.y - y * c.getHeight() * 0.5f;
        g.setColour (juce::Colour (0x33f08ba5));
        g.fillEllipse (dx - 11.0f, dy - 11.0f, 22.0f, 22.0f);
        g.setColour (juce::Colour (0xfff08ba5));
        g.fillEllipse (dx - 6.0f, dy - 6.0f, 12.0f, 12.0f);
    }

    static float getX01 (juce::RangedAudioParameter* p)
    {
        return p != nullptr ? p->convertFrom0to1 (p->getValue()) : 0.0f;
    }

    void setFromMouse (const juce::MouseEvent& e)
    {
        const auto c = circleBounds();
        float nx = (e.position.x - c.getCentreX()) / (c.getWidth()  * 0.5f);
        float ny = (c.getCentreY() - e.position.y) / (c.getHeight() * 0.5f);
        const float len = std::sqrt (nx * nx + ny * ny);
        if (len > 1.0f) { nx /= len; ny /= len; }
        if (px != nullptr) px->setValueNotifyingHost (px->convertTo0to1 (nx));
        if (py != nullptr) py->setValueNotifyingHost (py->convertTo0to1 (ny));
        repaint();
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (px) px->beginChangeGesture();
        if (py) py->beginChangeGesture();
        setFromMouse (e);
    }
    void mouseDrag (const juce::MouseEvent& e) override { setFromMouse (e); }
    void mouseUp   (const juce::MouseEvent&)   override
    {
        if (px) px->endChangeGesture();
        if (py) py->endChangeGesture();
    }
    void mouseDoubleClick (const juce::MouseEvent&) override   // back to centre
    {
        if (px) px->setValueNotifyingHost (px->convertTo0to1 (0.0f));
        if (py) py->setValueNotifyingHost (py->convertTo0to1 (0.0f));
    }

    VoxMorphProcessor& proc;
    juce::RangedAudioParameter *px = nullptr, *py = nullptr;
};

class AsmrPanel : public juce::Component
{
public:
    explicit AsmrPanel (VoxMorphProcessor& p) : proc (p), pad (p)
    {
        heading.setText ("ASMR POSITION", juce::dontSendNotification);
        heading.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
        heading.setColour (juce::Label::textColourId, juce::Colour (0xff45bda5));
        addAndMakeVisible (heading);

        help.setJustificationType (juce::Justification::topLeft);
        help.setFont (juce::Font (juce::FontOptions (12.5f)));
        help.setText (juce::String::fromUTF8 (
            "擬似立体音響: 円の中の点をドラッグすると声の聞こえる方向と距離を調整できます。\n"
            "上=正面 / 左右=耳元 / 中心から離れるほど遠く(小さく)。ダブルクリックで中央に戻り\n"
            "ます(中央=効果なし)。ステレオ出力時のみ左右に振れます(モノラル時は距離のみ)。"),
            juce::dontSendNotification);
        addAndMakeVisible (help);

        resetBtn.setTooltip (juce::String::fromUTF8 ("点を中央(効果なし)に戻します。"));
        resetBtn.onClick = [this]
        {
            for (auto* id : { "asmrx", "asmry" })
                if (auto* rp = proc.apvts.getParameter (id))
                {
                    rp->beginChangeGesture();
                    rp->setValueNotifyingHost (rp->convertTo0to1 (0.0f));
                    rp->endChangeGesture();
                }
        };
        addAndMakeVisible (resetBtn);
        addAndMakeVisible (pad);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (16, 12);
        heading.setBounds (r.removeFromTop (24));
        help.setBounds (r.removeFromTop (56));
        resetBtn.setBounds (r.removeFromTop (30).withWidth (150).withHeight (26));
        r.removeFromTop (6);
        const int s = std::min (r.getWidth(), r.getHeight());
        pad.setBounds (r.withSizeKeepingCentre (s, s));
    }

private:
    VoxMorphProcessor& proc;
    juce::Label heading, help;
    juce::TextButton resetBtn { "Center (Off)" };
    SonarPad pad;
};

// window that shows a hosted FX plugin's own editor (falls back to a
// generic parameter list when the plugin has no UI)
class FxWindow : public juce::DocumentWindow
{
public:
    explicit FxWindow (juce::AudioPluginInstance& fx)
        : juce::DocumentWindow (fx.getName(), juce::Colour (0xff3c3d42),
                                juce::DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (true);
        if (auto* ed = fx.createEditorIfNeeded())
            setContentOwned (ed, true);
        else
            setContentOwned (new juce::GenericAudioProcessorEditor (fx), true);
        centreWithSize (juce::jmax (300, getWidth()), juce::jmax (200, getHeight()));
        setVisible (true);
    }
    void closeButtonPressed() override { setVisible (false); }
};

// FX chain editor panel (lives in its own window): two sections, Pre FX
// (mic input before conversion) and Post FX (converted output), each a
// list of VST3s processed top-to-bottom with On/Off, UI and remove per
// row plus an add button. Rebuilt from the processor state after edits.
class FxChainPanel : public juce::Component
{
public:
    explicit FxChainPanel (VoxMorphProcessor& p) : proc (p)
    {
        lnf.setColour (juce::Label::textColourId,        juce::Colour (0xff2e2e32));
        lnf.setColour (juce::ToggleButton::textColourId, juce::Colour (0xff2e2e32));
        lnf.setColour (juce::ToggleButton::tickColourId, juce::Colour (0xff54c0aa));
        lnf.setColour (juce::TextButton::buttonColourId, juce::Colours::white);
        lnf.setColour (juce::TextButton::textColourOffId, juce::Colour (0xff54c0aa));
        setLookAndFeel (&lnf);

        auto initHead = [this] (juce::Label& l, const char* en, const char* jp)
        {
            l.setText (juce::String (en) + "   " + juce::String::fromUTF8 (jp),
                       juce::dontSendNotification);
            l.setFont (juce::Font (juce::FontOptions (13.5f, juce::Font::bold)));
            l.setColour (juce::Label::textColourId, juce::Colour (0xff45bda5));
            addAndMakeVisible (l);
        };
        initHead (preHead,  "Pre FX",  "(変換前・マイク入力に掛かる)");
        initHead (postHead, "Post FX", "(変換後・出力に掛かる)");
        preAdd.onClick  = [this] { add (false); };
        postAdd.onClick = [this] { add (true);  };
        addAndMakeVisible (preAdd);
        addAndMakeVisible (postAdd);
        note.setFont (juce::Font (juce::FontOptions (11.0f)));
        note.setColour (juce::Label::textColourId, juce::Colour (0xff9aa5a2));
        note.setText (juce::String::fromUTF8 ("上から順に処理されます。FXの遅延は補正されません。"
                                              "アプリ再起動後は再読み込みが必要です。"),
                      juce::dontSendNotification);
        addAndMakeVisible (note);
        rebuild();
    }

    ~FxChainPanel() override { setLookAndFeel (nullptr); }

    void rebuild()
    {
        wins.clear();
        rows.clear();
        for (int post = 0; post <= 1; ++post)
            for (int i = 0; i < proc.getNumFx (post != 0); ++i)
                if (auto* s = proc.getFxSlot (post != 0, i); s != nullptr && s->plugin != nullptr)
                {
                    auto* r = rows.add (new Row());
                    r->post = post != 0;  r->index = i;
                    r->on.setToggleState (s->enabled.load(), juce::dontSendNotification);
                    r->name.setText (s->plugin->getName(), juce::dontSendNotification);
                    r->on.onClick = [this, r]
                    {
                        proc.setFxEnabled (r->post, r->index, r->on.getToggleState());
                    };
                    r->ui.onClick = [this, r]
                    {
                        if (auto* sl = proc.getFxSlot (r->post, r->index))
                            if (sl->plugin != nullptr)
                                wins.add (new FxWindow (*sl->plugin));
                    };
                    r->del.onClick = [this, post2 = r->post, idx = r->index]
                    {
                        juce::Component::SafePointer<FxChainPanel> sp (this);
                        juce::MessageManager::callAsync ([sp, post2, idx]
                        {
                            if (sp == nullptr) return;
                            sp->proc.removeFx (post2, idx);
                            sp->rebuild();
                        });
                    };
                    addAndMakeVisible (r);
                }
        const int nPre  = proc.getNumFx (false);
        const int nPost = proc.getNumFx (true);
        setSize (460, 20 + 24 + nPre * 30 + 30 + 16 + 24 + nPost * 30 + 30 + 26 + 14);
        resized();
        repaint();
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (14, 10);
        preHead.setBounds (r.removeFromTop (24));
        for (auto* row : rows) if (! row->post) row->setBounds (r.removeFromTop (30).reduced (0, 2));
        preAdd.setBounds (r.removeFromTop (30).withWidth (150).reduced (0, 2));
        r.removeFromTop (16);
        postHead.setBounds (r.removeFromTop (24));
        for (auto* row : rows) if (row->post) row->setBounds (r.removeFromTop (30).reduced (0, 2));
        postAdd.setBounds (r.removeFromTop (30).withWidth (150).reduced (0, 2));
        note.setBounds (r.removeFromTop (26));
    }

    void paint (juce::Graphics& g) override { g.fillAll (juce::Colour (0xfffcf9f9)); }

private:
    struct Row : public juce::Component
    {
        bool post = false; int index = 0;
        juce::ToggleButton on;
        juce::Label name;
        juce::TextButton ui { "UI" }, del { "X" };
        Row()
        {
            for (auto* c : std::initializer_list<juce::Component*> { &on, &name, &ui, &del })
                addAndMakeVisible (*c);
        }
        void resized() override
        {
            auto r = getLocalBounds();
            on.setBounds (r.removeFromLeft (28));
            del.setBounds (r.removeFromRight (28).reduced (2));
            ui.setBounds (r.removeFromRight (40).reduced (2));
            name.setBounds (r);
        }
    };

    void add (bool post)
    {
       #if JUCE_MAC
        juce::File init ("/Library/Audio/Plug-Ins/VST3");
       #else
        juce::File init ("C:\\Program Files\\Common Files\\VST3");
       #endif
        chooser = std::make_unique<juce::FileChooser> (
            juce::String::fromUTF8 ("VST3プラグインを選択"), init, "*.vst3");
        chooser->launchAsync (juce::FileBrowserComponent::openMode
                            | juce::FileBrowserComponent::canSelectFiles,
            [this, post] (const juce::FileChooser& fc)
        {
            const auto f = fc.getResult();
            if (f == juce::File()) return;
            const auto err = proc.addFx (post, f);
            if (err.isNotEmpty())
                note.setText ("Error: " + err, juce::dontSendNotification);
            rebuild();
        });
    }

    VoxMorphProcessor& proc;
    juce::LookAndFeel_V4 lnf { juce::LookAndFeel_V4::getLightColourScheme() };
    juce::Label preHead, postHead, note;
    juce::TextButton preAdd { "+ Add VST3..." }, postAdd { "+ Add VST3..." };
    juce::OwnedArray<Row> rows;
    juce::OwnedArray<FxWindow> wins;
    std::unique_ptr<juce::FileChooser> chooser;
};

class FxChainWindow : public juce::DocumentWindow
{
public:
    explicit FxChainWindow (VoxMorphProcessor& p)
        : juce::DocumentWindow (juce::String::fromUTF8 ("Plugins — Pre / Post FX"),
                                juce::Colour (0xfffcf9f9), juce::DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new FxChainPanel (p), true);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }
    void closeButtonPressed() override { setVisible (false); }
};

// Standalone options bar (shown above the tabs, STANDALONE ONLY): controls
// that only make sense for the app — external FX chains and the audio
// device settings (the old title-bar Options button moved here).
class FxBar : public juce::Component
{
public:
    explicit FxBar (VoxMorphProcessor& p) : proc (p)
    {
        plugBtn.setTooltip (juce::String::fromUTF8 (
            "外部VST3プラグイン(Pre/Post FX)の管理ウィンドウを開きます。"));
        plugBtn.onClick = [this]
        {
            if (fxWin == nullptr)
                fxWin = std::make_unique<FxChainWindow> (proc);
            else
            {
                fxWin->setVisible (true);
                fxWin->toFront (true);
            }
        };
        audioBtn.setTooltip (juce::String::fromUTF8 (
            "オーディオ入出力デバイス・サンプルレート・バッファの設定を開きます。"));
        audioBtn.onClick = []
        {
           #if VOXMORPH_HAS_STANDALONE_HOLDER
            if (auto* h = juce::StandalonePluginHolder::getInstance())
                h->showAudioSettingsDialog();
           #endif
        };
        addAndMakeVisible (plugBtn);
        addAndMakeVisible (audioBtn);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (8, 3);
        plugBtn.setBounds (r.removeFromLeft (110));
        r.removeFromLeft (8);
        audioBtn.setBounds (r.removeFromLeft (130));
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xfff3eef0));
        g.setColour (juce::Colour (0xffe4dadd));
        g.drawLine (0.0f, (float) getHeight() - 0.5f, (float) getWidth(),
                    (float) getHeight() - 0.5f, 1.0f);
    }

private:
    VoxMorphProcessor& proc;
    juce::TextButton plugBtn { "Plugins..." }, audioBtn { "Audio Settings..." };
    std::unique_ptr<FxChainWindow> fxWin;
};

// simple component that forwards resized() to a lambda (used for tab pages)
struct FnComponent : public juce::Component
{
    std::function<void()> fn;
    void resized() override { if (fn) fn(); }
};

struct FnTimer : public juce::Timer
{
    std::function<void()> fn;
    void timerCallback() override { if (fn) fn(); }
};

// ---------------------------------------------------------------------------
// AEIOU Character DETAIL window (v0.26.0): per-vowel F1-F3 map viewer/editor.
// Built-in Characters are shown read-only; the Custom character attaches the
// 15 sliders to the va_*_f* APVTS parameters. A 4 Hz poll follows character
// switches and per-parameter locks (same style as the editor's histPoll).
class AEIOUCharacterPanel : public juce::Component, private juce::Timer
{
public:
    explicit AEIOUCharacterPanel (VoxMorphProcessor& p)
        : proc (p), pChar (p.apvts.getRawParameterValue ("vcharacter"))
    {
        // the DocumentWindow is outside the editor's LookAndFeel scope, so
        // give the panel the same pastel-mint light theme (otherwise JUCE's
        // default dark scheme paints labels/values white on white)
        lnf.setColour (juce::Slider::trackColourId,             juce::Colour (0xff54c0aa));
        lnf.setColour (juce::Slider::backgroundColourId,        juce::Colour (0xffe9e9e9));
        lnf.setColour (juce::Slider::thumbColourId,             juce::Colour (0xff54c0aa));
        lnf.setColour (juce::Slider::textBoxTextColourId,       juce::Colour (0xff2e2e32));
        lnf.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::white);
        lnf.setColour (juce::Slider::textBoxOutlineColourId,    juce::Colour (0xffdedede));
        lnf.setColour (juce::Label::textColourId,               juce::Colour (0xff2e2e32));
        lnf.setColour (juce::TextButton::buttonColourId,        juce::Colours::white);
        lnf.setColour (juce::TextButton::textColourOffId,       juce::Colour (0xff54c0aa));
        lnf.setColour (juce::ComboBox::textColourId,            juce::Colour (0xff2e2e32));
        setLookAndFeel (&lnf);
        charLbl.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
        charLbl.setColour (juce::Label::textColourId, juce::Colour (0xff45bda5));
        addAndMakeVisible (charLbl);

        copyBtn.setTooltip (juce::String::fromUTF8 (
            "Copies the current built-in Character's 15 values into Custom and "
            "switches to Custom for editing.\n\n"
            "現在の内蔵Characterの15値をCustomへコピーし、編集用にCustomへ切り替えます。"));
        copyBtn.onClick = [this] { copyToCustom(); };
        addAndMakeVisible (copyBtn);

        resetBtn.setTooltip (juce::String::fromUTF8 (
            "Resets the Custom map to the Natural preset values.\n\n"
            "CustomのマップをNaturalの初期値へ戻します。"));
        resetBtn.onClick = [this] { resetCustom(); };
        addAndMakeVisible (resetBtn);

        note.setText (juce::String::fromUTF8 (
            "Built-in Characters are read-only. Use \"Copy to Custom\" to edit. / "
            "内蔵Characterは読み取り専用です。編集する場合は「Copy to Custom」でCustomへコピーしてください。"),
            juce::dontSendNotification);
        note.setFont (juce::Font (juce::FontOptions (12.0f)));
        note.setColour (juce::Label::textColourId, juce::Colours::grey);
        addAndMakeVisible (note);

        static const char* colTxt[3] = { "F1", "F2", "F3" };
        for (int f = 0; f < 3; ++f)
        {
            colLbl[f].setText (colTxt[f], juce::dontSendNotification);
            colLbl[f].setFont (juce::Font (juce::FontOptions (12.5f, juce::Font::bold)));
            colLbl[f].setJustificationType (juce::Justification::centred);
            addAndMakeVisible (colLbl[f]);
        }
        static const char* secTxt[2] = { "Formant Shift (st)", "Formant Gain (dB)" };
        for (int s = 0; s < 2; ++s)
        {
            secLbl[s].setText (secTxt[s], juce::dontSendNotification);
            secLbl[s].setFont (juce::Font (juce::FontOptions (12.5f, juce::Font::bold)));
            secLbl[s].setColour (juce::Label::textColourId, juce::Colour (0xff45bda5));
            addAndMakeVisible (secLbl[s]);
        }
        static const char* vowTxt[5] = { "A / \xe3\x81\x82", "I / \xe3\x81\x84",
                                         "U / \xe3\x81\x86", "E / \xe3\x81\x88",
                                         "O / \xe3\x81\x8a" };
        static constexpr float rng[3] = { 2.0f, 3.0f, 1.5f };
        const auto& nat = getAEIOUCharacterMap (AEIOUCharacter::natural);
        for (int v = 0; v < 5; ++v)
        {
            vowLbl[v] .setText (juce::String::fromUTF8 (vowTxt[v]), juce::dontSendNotification);
            vowLblG[v].setText (juce::String::fromUTF8 (vowTxt[v]), juce::dontSendNotification);
            vowLbl[v] .setFont (juce::Font (juce::FontOptions (13.0f)));
            vowLblG[v].setFont (juce::Font (juce::FontOptions (13.0f)));
            addAndMakeVisible (vowLbl[v]);
            addAndMakeVisible (vowLblG[v]);
            for (int f = 0; f < 3; ++f)
            {
                auto& s = cell[v][f];
                s.setSliderStyle (juce::Slider::LinearHorizontal);
                s.setTextBoxStyle (juce::Slider::TextBoxRight, false, 52, 18);
                s.setRange (-rng[f], rng[f], 0.01);
                s.setDoubleClickReturnValue (true, nat.offset[v][f]);
                addAndMakeVisible (s);

                auto& g = cellG[v][f];
                g.setSliderStyle (juce::Slider::LinearHorizontal);
                g.setTextBoxStyle (juce::Slider::TextBoxRight, false, 52, 18);
                g.setRange (-3.0, 3.0, 0.1);
                g.setDoubleClickReturnValue (true, nat.gainDb[v][f]);
                addAndMakeVisible (g);
            }
        }

        closeBtn.onClick = [this]
        {
            if (auto* dw = findParentComponentOfClass<juce::DocumentWindow>())
                dw->setVisible (false);
        };
        addAndMakeVisible (closeBtn);

        setSize (700, 640);
        // slider text boxes are created before the LnF applies (same issue
        // as the main editor, see HANDOVER): force a refresh
        sendLookAndFeelChange();
        sync (true);
        startTimerHz (4);
    }

    ~AEIOUCharacterPanel() override { setLookAndFeel (nullptr); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (14, 10);
        auto top = r.removeFromTop (28);
        charLbl.setBounds (top.removeFromLeft (240));
        resetBtn.setBounds (top.removeFromRight (120).reduced (0, 2));
        top.removeFromRight (6);
        copyBtn.setBounds (top.removeFromRight (130).reduced (0, 2));
        r.removeFromTop (2);
        note.setBounds (r.removeFromTop (20));
        r.removeFromTop (6);
        r.removeFromBottom (32);   // reserve for the Close button

        auto laySection = [&] (int sectionIdx,
                               juce::Slider (&arr)[5][3], juce::Label (&vl)[5])
        {
            secLbl[sectionIdx].setBounds (r.removeFromTop (20));
            auto hdr = r.removeFromTop (18);
            hdr.removeFromLeft (76);
            const int cw = hdr.getWidth() / 3;
            for (int f = 0; f < 3; ++f)
                colLbl[f].setBounds (hdr.removeFromLeft (cw));   // shared labels: F1/F2/F3

            for (int v = 0; v < 5; ++v)
            {
                auto row = r.removeFromTop (40);
                vl[v].setBounds (row.removeFromLeft (76));
                const int cw2 = row.getWidth() / 3;
                for (int f = 0; f < 3; ++f)
                    arr[v][f].setBounds (row.removeFromLeft (cw2).reduced (4, 6));
            }
            r.removeFromTop (8);
        };
        laySection (0, cell,  vowLbl);
        laySection (1, cellG, vowLblG);

        closeBtn.setBounds (getLocalBounds().reduced (14, 10)
                                .removeFromBottom (26).removeFromRight (90));
    }

private:
    static juce::String customId (int v, int f)
    {
        static const char* vw[5] = { "a", "i", "u", "e", "o" };
        return juce::String ("va_") + vw[v] + "_f" + juce::String (f + 1);
    }
    static juce::String customIdG (int v, int f)
    {
        static const char* vw[5] = { "a", "i", "u", "e", "o" };
        return juce::String ("va_") + vw[v] + "_g" + juce::String (f + 1);
    }

    void timerCallback() override { sync (false); }

    // mirror the current character into the grid: Custom = attached and
    // editable (minus locked params), built-ins = detached read-only values
    void sync (bool force)
    {
        const int ch = juce::jlimit (0, kAEIOUNumCharacters - 1,
                                     (int) pChar->load());
        const bool custom = ch == (int) AEIOUCharacter::custom;
        if (force || ch != lastCh)
        {
            static const char* names[9] = { "Natural", "Soft", "Active", "Loli",
                                            "Anime", "Lily", "Elegant", "Uni", "Custom" };
            charLbl.setText (juce::String ("Character: ") + names[ch],
                             juce::dontSendNotification);
            const auto& mm = getAEIOUCharacterMap ((AEIOUCharacter) ch);
            for (int v = 0; v < 5; ++v)
                for (int f = 0; f < 3; ++f)
                {
                    auto& s = cell[v][f];
                    auto& g = cellG[v][f];
                    if (custom)
                    {
                        if (att[v][f] == nullptr)
                            att[v][f] = std::make_unique<
                                juce::AudioProcessorValueTreeState::SliderAttachment> (
                                    proc.apvts, customId (v, f), s);
                        if (attG[v][f] == nullptr)
                            attG[v][f] = std::make_unique<
                                juce::AudioProcessorValueTreeState::SliderAttachment> (
                                    proc.apvts, customIdG (v, f), g);
                    }
                    else
                    {
                        att[v][f].reset();
                        attG[v][f].reset();
                        s.setValue (mm.offset[v][f], juce::dontSendNotification);
                        g.setValue (mm.gainDb[v][f], juce::dontSendNotification);
                    }
                }
            lastCh = ch;
        }
        for (int v = 0; v < 5; ++v)
            for (int f = 0; f < 3; ++f)
            {
                cell[v][f] .setEnabled (custom && ! proc.isParamLocked (customId  (v, f)));
                cellG[v][f].setEnabled (custom && ! proc.isParamLocked (customIdG (v, f)));
            }
        copyBtn.setEnabled (! custom);
        resetBtn.setEnabled (custom);
    }

    void copyToCustom()
    {
        const int ch = juce::jlimit (0, kAEIOUNumCharacters - 1,
                                     (int) pChar->load());
        if (ch == (int) AEIOUCharacter::custom) return;
        const auto& m = getAEIOUCharacterMap ((AEIOUCharacter) ch);
        proc.history.group ([&]
        {
            auto applyOne = [&] (const juce::String& id, float val)
            {
                if (proc.isParamLocked (id)) return;
                if (auto* rp = proc.apvts.getParameter (id))
                {
                    rp->beginChangeGesture();
                    rp->setValueNotifyingHost (rp->convertTo0to1 (val));
                    rp->endChangeGesture();
                }
            };
            for (int v = 0; v < 5; ++v)
                for (int f = 0; f < 3; ++f)
                {
                    applyOne (customId  (v, f), m.offset[v][f]);
                    applyOne (customIdG (v, f), m.gainDb[v][f]);
                }
            if (! proc.isParamLocked ("vcharacter"))
                if (auto* cp = proc.apvts.getParameter ("vcharacter"))
                {
                    cp->beginChangeGesture();
                    cp->setValueNotifyingHost (
                        cp->convertTo0to1 ((float) AEIOUCharacter::custom));
                    cp->endChangeGesture();
                }
        });
        sync (true);
    }

    void resetCustom()   // back to the Natural defaults (no confirmation)
    {
        proc.history.group ([&]
        {
            auto resetOne = [&] (const juce::String& id)
            {
                if (proc.isParamLocked (id)) return;
                if (auto* rp = proc.apvts.getParameter (id))
                {
                    rp->beginChangeGesture();
                    rp->setValueNotifyingHost (rp->getDefaultValue());
                    rp->endChangeGesture();
                }
            };
            for (int v = 0; v < 5; ++v)
                for (int f = 0; f < 3; ++f)
                {
                    resetOne (customId  (v, f));
                    resetOne (customIdG (v, f));
                }
        });
    }

    VoxMorphProcessor& proc;
    std::atomic<float>* pChar = nullptr;
    juce::LookAndFeel_V4 lnf { juce::LookAndFeel_V4::getLightColourScheme() };
    juce::Label charLbl, note, colLbl[3], vowLbl[5], vowLblG[5], secLbl[2];
    juce::TextButton copyBtn { "Copy to Custom" }, resetBtn { "Reset Custom" },
                     closeBtn { "Close" };
    juce::Slider cell[5][3], cellG[5][3];
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> att[5][3];
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attG[5][3];
    int lastCh = -1;
};

class AEIOUCharacterWindow : public juce::DocumentWindow
{
public:
    explicit AEIOUCharacterWindow (VoxMorphProcessor& p)
        : juce::DocumentWindow ("AEIOU Character Detail",
                                juce::Colour (0xfffcf9f9), juce::DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new AEIOUCharacterPanel (p), true);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }
    void closeButtonPressed() override { setVisible (false); }
};

class VoxMorphEditor : public juce::AudioProcessorEditor,
                       private juce::Timer
{
public:
    explicit VoxMorphEditor (VoxMorphProcessor& p)
        : juce::AudioProcessorEditor (&p), proc (p)
    {
        tooltipWindow.setLookAndFeel (&tipLnf);
        setWantsKeyboardFocus (true);   // for the Cmd+S shortcut

        // pastel mint theme (all colours in one place — edit freely)
        mainLnf.setColour (juce::Slider::trackColourId,             juce::Colour (0xff54c0aa)); // mint fill
        mainLnf.setColour (juce::Slider::backgroundColourId,        juce::Colour (0xffe9e9e9)); // rest of track
        mainLnf.setColour (juce::Slider::thumbColourId,             juce::Colour (0xff54c0aa)); // mint thumb
        mainLnf.setColour (juce::Slider::textBoxTextColourId,       juce::Colour (0xff2e2e32)); // value digits
        mainLnf.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::white);
        mainLnf.setColour (juce::Slider::textBoxOutlineColourId,    juce::Colour (0xffdedede));
        mainLnf.setColour (juce::Label::textColourId,               juce::Colour (0xff2e2e32)); // body text
        mainLnf.setColour (juce::ToggleButton::textColourId,        juce::Colour (0xff2e2e32));
        mainLnf.setColour (juce::ToggleButton::tickColourId,        juce::Colour (0xff54c0aa));
        mainLnf.setColour (juce::ToggleButton::tickDisabledColourId,juce::Colour (0xffbfd9d2));
        mainLnf.setColour (juce::TextButton::buttonColourId,        juce::Colours::white);
        mainLnf.setColour (juce::TextButton::textColourOffId,       juce::Colour (0xff54c0aa)); // reset arrows
        setLookAndFeel (&mainLnf);

        // FX bar: standalone-only external plugin slots above the tabs
        if (proc.wrapperType == juce::AudioProcessor::wrapperType_Standalone)
            addAndMakeVisible (fxBar);

        // MAIN / ANALYZE tabs. The MAIN page holds the scrolling parameter
        // list (viewport -> content); ANALYZE holds the profile tools.
        addAndMakeVisible (tabs);
        tabs.setTabBarDepth (30);
        tabs.setColour (juce::TabbedComponent::outlineColourId, juce::Colours::transparentBlack);
        tabs.setColour (juce::TabbedButtonBar::tabTextColourId,   juce::Colour (0xff9aa5a2));
        tabs.setColour (juce::TabbedButtonBar::frontTextColourId, juce::Colour (0xff45bda5));
        tabs.addTab ("MAIN",    juce::Colour (0xfffcf9f9), &mainPage,     false);
        tabs.addTab ("MATCHING", juce::Colour (0xfffcf9f9), &matchingPanel, false);
        tabs.addTab ("PRESETS", juce::Colour (0xfffcf9f9), &presetPanel,  false);
        tabs.addTab ("ASMR",    juce::Colour (0xfffcf9f9), &asmrPanel,    false);
        mainPage.fn = [this] { layoutMainPage(); };

        // all rows are children of `content`, which scrolls inside `viewport`
        mainPage.addAndMakeVisible (viewport);
        viewport.setViewedComponent (&content, false);
        viewport.setScrollBarsShown (true, false);
        viewport.setScrollBarThickness (10);

        addSection ("VISUALIZER");
        content.addAndMakeVisible (spectrum);
        items.push_back ({ &spectrum, 168 });
        content.addAndMakeVisible (status);    // realtime status (latency)
        items.push_back ({ &status, 26 });

        addSection ("PITCH");
        addSliderRow ("pitch", "Pitch (semitones)",
            tip ("Shifts the pitch in semitones. The timbre (formants) stays unchanged. "
                 "Male-to-female: +5 to +7. Female-to-male: around -5.",
                 "声の高さを半音単位で変えます。声色(フォルマント)は変わりません。"
                 "女声化は+5〜+7、男声化は-5前後が目安。"));
        addToggleRow ("robot", "Robotize",
            tip ("Locks the pitch to one fixed note for a monotone robot voice. "
                 "Choose the note with 'Robot Pitch' below.",
                 "ピッチを一定の高さに固定し、抑揚のないロボット声にします。"
                 "高さは下のRobot Pitchで指定します。"));
        addSliderRow ("robotHz", "Robot Pitch (Hz)",
            tip ("The fixed pitch (Hz) used while Robotize is on.",
                 "Robotizeがオンのとき固定されるピッチ(Hz)。"));

        addSection ("HIGH RANGE");
        addSliderRow ("hifreq", "High Range Start (Hz)",
            tip ("When your INPUT pitch (before conversion) rises above this - laughing, squealing, "
                 "exclamations - the Pitch/Formant shifts blend smoothly toward the High amounts "
                 "below, reaching them fully one octave up. Stops laughs from being shifted into "
                 "unnaturally high tones. 0 = off. Try 250-350 Hz.",
                 "入力(変換前)のピッチがこの値を超えると(笑い声・叫び・感嘆など)、ピッチ/"
                 "フォルマントの変化量が下のHigh設定へ滑らかに移行し、1オクターブ上で完全に"
                 "切り替わります。笑い声が不自然な高音まで上がるのを防ぎます。0=オフ。"
                 "250〜350Hzが目安。"));
        addSliderRow ("hipitch", "High Pitch Amount (%)",
            tip ("How much of the Pitch shift remains in the high range. 100% = same as normal, "
                 "0% = no shift there (laughs keep their natural pitch). Try 30-60%.",
                 "高音域で残すPitchシフトの割合。100%=通常と同じ、0%=シフトなし(笑い声は"
                 "地声の高さのまま)。30〜60%が目安。"));
        addSliderRow ("hiformant", "High Formant Amount (%)",
            tip ("How much of the Formant shift remains in the high range. Usually leave at 100% "
                 "so the voice keeps its character while only the pitch settles down.",
                 "高音域で残すFormantシフトの割合。通常は100%のまま(声色は保ちつつピッチだけ"
                 "落ち着かせる)が自然です。"));

        addSection ("FORMANT");
        addSliderRow ("formant", "Formant (semitones)",
            tip ("Changes the vocal-tract size = the timbre, without changing pitch. "
                 "+ sounds younger/feminine, - sounds deeper/masculine. +3 to +4 for male-to-female.",
                 "声道の長さ=声の響き・声色を変えます。ピッチは変わりません。"
                 "+で若く/女性的に、-で太く/男性的に。女声化は+3〜+4が目安。"));
        addSliderRow ("consonant", "Consonant Shift (st)",
            tip ("Extra shift applied only to unvoiced consonants (s, sh...), added on top of Formant. "
                 "Female consonants are brighter: try +2 to +3. Too much sounds like a lisp.",
                 "無声子音(サ行・シャ行など)だけを追加でシフトします(Formantに加算)。"
                 "女声の子音は明るいので+2〜+3が目安。上げすぎると舌足らずに聞こえます。"));
        addSliderRow ("f1shift", "F1 Shift (st)",
            tip ("Moves only the first formant (jaw openness / throat size), on top of the global "
                 "Formant. Male-to-female sounds most natural with F1 raised LESS than F2: "
                 "try F1 +1 to +2 when F2 is +2 to +4.",
                 "第1フォルマント(顎の開き・喉の広さ)だけを動かします(全体Formantに加算)。"
                 "女声化はF1をF2より控えめに上げると自然です(F2が+2〜+4のときF1は+1〜+2)。"));
        addSliderRow ("f1gain", "F1 Gain (dB)",
            tip ("Boost or cut around the first formant. Cutting a few dB thins out a 'boomy' "
                 "chest resonance.",
                 "第1フォルマント付近の強さ。数dB下げると胸に響く「太さ」が抜けます。"));
        addSliderRow ("f2shift", "F2 Shift (st)",
            tip ("Moves only the second formant (tongue position). The strongest single cue for "
                 "perceived gender/age of the vowels: +2 to +4 sounds younger and more feminine.",
                 "第2フォルマント(舌の位置)だけを動かします。母音の性別・年齢感に最も効く帯域で、"
                 "+2〜+4で若く女性的に聞こえます。"));
        addSliderRow ("f2gain", "F2 Gain (dB)",
            tip ("Boost or cut around the second formant. A few dB of boost adds clarity and "
                 "'presence' to the vowels.",
                 "第2フォルマント付近の強さ。数dB上げると母音の明瞭さ・華やかさが出ます。"));
        addSliderRow ("f3shift", "F3 Shift (st)",
            tip ("Moves only the third formant (front cavity / lip area). Small shifts (+1 to +2) "
                 "refine the impression of a shorter vocal tract.",
                 "第3フォルマント(声道前部・唇まわり)だけを動かします。+1〜+2の小さめの操作で"
                 "「声道が短い」印象を仕上げます。"));
        addSliderRow ("f3gain", "F3 Gain (dB)",
            tip ("Boost or cut around the third formant. Boosting adds sheen and 'sparkle' - "
                 "this region carries much of a voice's charm.",
                 "第3フォルマント付近の強さ。上げると艶・張りが出ます。声の「華」が乗る帯域です。"));
        addToggleRow ("vadapt", "AEIOU Character",
            tip ("Shapes the voice character by applying different F1-F3 adjustments to the "
                 "estimated A/E/I/O/U vowel regions. Your manual F1-F3 settings remain the "
                 "base values; the per-vowel offsets are added on top. Off = previous behaviour.",
                 "発音中の「あ・い・う・え・お」を推定し、母音ごとにF1〜F3の響きを調整して"
                 "声のキャラクターを作ります。手動のF1〜F3設定は基本値としてそのまま維持され、"
                 "その上に母音別の補正が乗ります。オフ=従来どおり。"));
        addComboRow ("vcharacter", "Character",
            tip ("Choose the voice character:\n"
                 "Natural - natural feminine balance / Soft - soft and rounded / "
                 "Active - bright and energetic / Loli - small and youthful / "
                 "Anime - exaggerated vowel contrast / Lily - clear, sweet feminine / "
                 "Elegant - calm and refined / Uni - neutral and androgynous / "
                 "Custom - your own A-I-U-E-O map (DETAIL...).",
                 "声のキャラクターを選びます:\n"
                 "Natural=自然な女性声 / Soft=柔らかい声 / Active=元気な声 / "
                 "Loli=幼な声 / Anime=アニメ声 / Lily=百合声 / Elegant=お姉さん声 / "
                 "Uni=中性声 / Custom=詳細モード(DETAIL...の母音別設定を使用)。"));
        addSliderRow ("vamount", "AEIOU Amount (%)",
            tip ("Strength of the selected character's per-vowel offsets. 0 % = identical "
                 "to the feature being off, 100 % = the character map as designed, up to "
                 "200 % emphasizes it further (larger internal limits apply above 100 %).",
                 "選択したキャラクター補正の強さ。0%=機能オフと完全に同じ音、"
                 "100%=設計どおりのキャラクター、200%まで上げるとさらに強調されます"
                 "(100%超は内部上限を広げて適用)。"));
        addButtonRow ("Vowel Detail", "DETAIL...",
            tip ("Opens a window to view and edit the per-vowel F1-F3 settings. Built-in "
                 "Characters are shown read-only; \"Copy to Custom\" makes them editable.",
                 "母音別のF1〜F3設定を確認・編集するウィンドウを開きます。内蔵Characterは"
                 "読み取り専用で、「Copy to Custom」でCustomへコピーすると編集できます。"),
            [this]
            {
                if (aeiouWin == nullptr)
                    aeiouWin = std::make_unique<AEIOUCharacterWindow> (proc);
                else
                {
                    aeiouWin->setVisible (true);
                    aeiouWin->toFront (true);
                }
            });

        addSection ("INTONATION");
        addSliderRow ("range", "Intonation Amount (%)",
            tip ("Exaggerates or flattens the pitch movement (intonation). 100% = unchanged. "
                 "Unlike 'Pitch', which moves the whole voice up or down, this scales only the movement. "
                 "110-140% recommended for male-to-female.",
                 "声の抑揚(音程の上がり下がり)を強調/抑制します。100%=変化なし。"
                 "Pitchが声全体を平行移動するのに対し、こちらは動きの幅だけを変えます。"
                 "女声化では110〜140%が目安です。"));
        addSliderRow ("center", "Intonation Pivot (Hz)",
            tip ("The pitch that intonation scaling expands around. Set it near the average pitch "
                 "of the converted voice (200-250 Hz for a female voice). No effect at 100% Amount.",
                 "抑揚を拡大/縮小するときの中心になる音程。変換後の声の平均的な高さに"
                 "合わせてください(女声なら200〜250Hz)。Amountが100%のときは無効。"));

        addSection ("VOICE QUALITY");
        addSliderRow ("tilt", "Softness / Tilt (dB)",
            tip ("Spectral tilt of the voice. + is softer and warmer, - is brighter and more present. "
                 "Start around +/-2 dB.",
                 "音色の傾き。+で柔らかく暖かい声、-で明るく張りのある声。±2dB程度から。"));
        addSliderRow ("jitter", "Natural Jitter",
            tip ("Adds tiny natural pitch fluctuations to reduce the 'machine' feel. Try around 0.1.",
                 "ごく小さな音程の揺らぎを加え、変換の機械っぽさを和らげます。0.1前後から。"));
        addSliderRow ("air", "Natural Air",
            tip ("Preserves the natural breath and aperiodic detail of the voice while suppressing "
                 "old-pitch harmonic leakage. Up to 1.0 the preserved amount increases at natural "
                 "loudness; from 1.0 to 1.5 the preserved air is also emphasized. 0 = off.",
                 "声に含まれる自然な息や非周期成分を保ちながら、元のピッチ成分が重なって聞こえる"
                 "ゴーストを抑えます。1.0までは自然な音量のまま保持量が増え、1.0〜1.5では"
                 "息成分を強調します。0=オフ。"));
        addSliderRow ("airshine", "Air Shine (dB)",
            tip ("Adds high-frequency openness and air above the preserved natural breath. Only "
                 "the highest air band (above ~6 kHz) comes back louder; the mids and the "
                 "harmonic body are untouched. Try 2-4 dB.",
                 "Natural Airの高域に抜け感と明るさを加えます。約6kHz以上の空気感だけが"
                 "持ち上がり、中音域や声の芯には触れません。まずは2〜4dBがおすすめ。"));

        addSection ("ADVANCED");
        addToggleRow ("gci", "GCI Grain Sync (Beta)",
            tip ("EXPERIMENTAL. Aligns the internal grain cutting to the glottal closure instants "
                 "(the exact moments the vocal folds snap shut) and keeps them phase-locked from "
                 "period to period. Mainly helps low / slightly hoarse voices, especially with "
                 "Low Voice Mode. It automatically reverts to the classic alignment where no clear "
                 "pulses exist and while the pitch is sliding. If your voice sounds juddery or "
                 "robotic with this on, leave it off - off is the previous behaviour.",
                 "実験的機能。内部のグレイン切り出しを声帯の閉鎖瞬間(GCI)に同期させ、周期ごとの"
                 "位相を揃えます。主に低い声・少しかすれた声(特にLow Voice Mode併用時)で効果が"
                 "あります。明確な声帯パルスが無い区間や音程が動いている間は自動的に従来の整列に"
                 "戻ります。オンにしてガタつき・ロボットっぽさを感じる場合はオフのままにして"
                 "ください(オフ=従来どおり)。"));
        addToggleRow ("lowvoice", "Low Voice Mode",
            tip ("Extends pitch tracking for very low voices and vocal fry. It may retain more of "
                 "the original low-period texture depending on the voice.",
                 "非常に低い声やボーカルフライでもピッチ追跡を継続します。発声によっては、"
                 "元の低周期の質感が強く残る場合があります。"));

        addToggleRow ("lowlat", "Low Latency Mode",
            tip ("Halves the conversion delay (43 ms -> about 21 ms) for live streaming and "
                 "monitoring. Trade-off: pitch tracking bottoms out around 90 Hz, so very deep "
                 "voices may track worse. Ignored while Low Voice Mode is on.",
                 "変換遅延を半分(43ms→約21ms)にします。配信やモニタリング向け。"
                 "代わりにピッチ検出の下限が約90Hzに上がるため、非常に低い声では追跡が"
                 "落ちる場合があります。Low Voice Modeがオンの間は無効です。"));
        addToggleRow ("stereo", "Stereo Input (Binaural)",
            tip ("For binaural / ASMR stereo microphones: the left and right inputs run through "
                 "two independent conversion engines in parallel, keeping the stereo image. "
                 "Latency is unchanged, CPU roughly doubles. If the sides occasionally drift "
                 "apart on tricky voices, switch it off. Off = classic mono (inputs summed).",
                 "バイノーラル/ASMR用ステレオマイク向け。左右の入力を2つの独立した変換エンジンで"
                 "並列処理し、立体感を保ったまま変換します。遅延は変わりません(CPUは約2倍)。"
                 "声によってはまれに左右の解釈が割れて広がって聞こえる場合があり、その時はオフに。"
                 "オフ=従来どおりモノラル(左右を合成)。"));
        addToggleRow ("automute", "Auto-Mute on Feedback",
            tip ("Standalone app: if the output stays extremely loud for over a second "
                 "(a runaway feedback loop between speakers and mic), the output is muted "
                 "for 3 seconds automatically. Has no effect in a DAW.",
                 "スタンドアロン用。スピーカー→マイクのハウリングが暴走して出力が1秒以上"
                 "大音量で鳴り続けた場合、自動で3秒間ミュートして回路を切ります。"
                 "DAWプラグインとして使用中は動作しません。"));
        addSliderRow ("gate", "Noise Gate (dB)",
            tip ("Mutes the input while it stays below this level - removes fan / room noise "
                 "between phrases. -80 = off. Set it just above your noise floor (try -55 to -45).",
                 "入力がこのレベルを下回っている間ミュートし、話していない間のファンノイズや"
                 "環境音を消します。-80=オフ。ノイズの音量より少し上に設定してください"
                 "(目安 -55〜-45)。"));
        addSliderRow ("breath2", "Breath (Beta)",
            tip ("EXPERIMENTAL. Replaces the upper harmonics with aspiration noise shaped by your "
                 "vocal tract (harmonic+noise model). Small amounts (0.1-0.2) add air; the quality "
                 "is still being tuned - leave at 0 if it sounds synthetic to you.",
                 "実験的機能。高域の倍音を、声道の響きで整形した気息ノイズに置き換えます"
                 "(ハーモニック+ノイズモデル)。0.1〜0.2で空気感が出ます。品質は調整中なので、"
                 "合成的に聞こえる場合は0のままにしてください。"));
        addSliderRow ("pitchfloor", "Pitch Floor (Hz)",
            tip ("If the converted pitch falls below this, it is lifted softly toward the floor. "
                 "Useful when your voice drifts too low while speaking. 0 = off. "
                 "Try 140-180 with a female target voice.",
                 "変換後のピッチがこの値を下回ったとき、滑らかに引き上げます。"
                 "話しているうちに声が低くなりすぎる場合の補正用。0=オフ。"
                 "女声化なら140〜180が目安です。"));

        addSection ("OUTPUT");
        addSliderRow ("mix", "Mix",
            tip ("Balance between the converted voice (1.0) and the original (0.0). Usually 1.0.",
                 "変換した声(1.0)と元の声(0.0)の割合。通常は1.0のままにします。"));
        addSliderRow ("gain", "Output Gain (dB)",
            tip ("Output level of the plugin, to compensate loudness changes from the conversion.",
                 "プラグインの出力レベル。変換で音量感が変わったときの補正用。"));

        footer.setText (
            juce::String::fromUTF8 (
                "Hover any control for help. Click a value to type it. \xE2\x86\xBA or double-click = default.\n"
                "各項目にマウスを乗せると説明が出ます。数値クリックで入力、\xE2\x86\xBA かダブルクリックで初期値に戻ります。"),
            juce::dontSendNotification);
        footer.setFont (juce::Font (juce::FontOptions (11.5f)));
        footer.setColour (juce::Label::textColourId, juce::Colours::grey);
        footer.setJustificationType (juce::Justification::topLeft);
        content.addAndMakeVisible (footer);
        defaultFooterText = footer.getText();

        contentHeight = 20;
        for (auto& it : items) contentHeight += it.h;
        contentHeight += 52;

        // Undo / Redo (top-right, over the tab strip) + history poller.
        // The poller turns a settled burst of manual knob edits into one
        // undo step and keeps the buttons / lock UI in sync.
        addAndMakeVisible (undoBtn);
        addAndMakeVisible (redoBtn);
        undoBtn.setEnabled (false);
        redoBtn.setEnabled (false);
        undoBtn.setTooltip (tip (
            "Undo the last sound-changing operation: knob edits, a preset load, "
            "'Reset All' or an Analyze Auto-Set / Refine (each counts as one step). "
            "Shortcut: Cmd+Z (Mac) / Ctrl+Z (Windows).",
            "直前の「音が変わる操作」を取り消します。つまみ操作・プリセット読込・"
            "Reset All・AnalyzeのAuto-Set/Refineが対象で、一括変更は1回のUndoで戻ります。"
            "ショートカット: Cmd+Z (Mac) / Ctrl+Z (Windows)。"));
        redoBtn.setTooltip (tip (
            "Redo the operation you just undid. Shortcut: Shift+Cmd+Z (Mac) / "
            "Shift+Ctrl+Z or Ctrl+Y (Windows).",
            "取り消した操作をやり直します。ショートカット: Shift+Cmd+Z (Mac) / "
            "Shift+Ctrl+ZまたはCtrl+Y (Windows)。"));
        undoBtn.onClick = [this] { proc.history.undo(); };
        redoBtn.onClick = [this] { proc.history.redo(); };
        histPoll.fn = [this]
        {
            proc.history.poll();
            undoBtn.setEnabled (proc.history.canUndo());
            redoBtn.setEnabled (proc.history.canRedo());
            if (lastLockState != proc.lockedIds.joinIntoString (","))
                syncLockUI();                    // e.g. host restored state
        };
        histPoll.startTimerHz (3);
        syncLockUI();

        setResizable (true, true);
        setResizeLimits (440, 320, 900, contentHeight + 50);
        setSize (560, juce::jmin (contentHeight + 42, kMaxInitialHeight));

        // sliders build their value boxes before this editor's LookAndFeel is
        // attached to them, so push the theme colours down explicitly
        sendLookAndFeelChange();
    }

    ~VoxMorphEditor() override
    {
        setLookAndFeel (nullptr);
        tooltipWindow.setLookAndFeel (nullptr);
    }

    // Cmd+S (Ctrl+S on Windows) saves the standalone app's settings so they
    // survive a crash / force-quit. In a DAW the shortcut is left to the host.
    bool keyPressed (const juce::KeyPress& key) override
    {
        // Cmd+Z (Mac) / Ctrl+Z (Windows) = Undo; +Shift or Cmd/Ctrl+Y = Redo
        if (key == juce::KeyPress ('z', juce::ModifierKeys::commandModifier
                                        | juce::ModifierKeys::shiftModifier, 0)
         || key == juce::KeyPress ('y', juce::ModifierKeys::commandModifier, 0))
        {
            proc.history.redo();
            return true;
        }
        if (key == juce::KeyPress ('z', juce::ModifierKeys::commandModifier, 0))
        {
            proc.history.undo();
            return true;
        }

        if (proc.wrapperType == juce::AudioProcessor::wrapperType_Standalone
            && key == juce::KeyPress ('s', juce::ModifierKeys::commandModifier, 0))
        {
           #if VOXMORPH_HAS_STANDALONE_HOLDER
            if (auto* holder = juce::StandalonePluginHolder::getInstance())
            {
                holder->savePluginState();
                // flush to disk immediately if the settings are file-backed
                if (auto* pf = dynamic_cast<juce::PropertiesFile*> (holder->settings.get()))
                    pf->saveIfNeeded();
                flashFooter (juce::String::fromUTF8 (
                    "\xE2\x9C\x93 Settings saved / \xE8\xA8\xAD\xE5\xAE\x9A\xE3\x82\x92\xE4\xBF\x9D\xE5\xAD\x98\xE3\x81\x97\xE3\x81\xBE\xE3\x81\x97\xE3\x81\x9F"));
                return true;
            }
           #endif
        }
        return false;
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xfffcf9f9));   // near-white
    }

    void resized() override
    {
        auto r = getLocalBounds();
        if (fxBar.isVisible())
            fxBar.setBounds (r.removeFromTop (30));
        tabs.setBounds (r);
        auto tb = r.removeFromTop (30);       // over the tab strip's right end
        redoBtn.setBounds (tb.removeFromRight (58).reduced (3, 4));
        undoBtn.setBounds (tb.removeFromRight (58).reduced (3, 4));
    }

    // Standalone: switch the app window to the OS-native title bar (like
    // most apps) and hide JUCE's in-titlebar "Options" button — its
    // functions moved into the options bar (Audio Settings...).
    void parentHierarchyChanged() override
    {
        if (proc.wrapperType != juce::AudioProcessor::wrapperType_Standalone)
            return;
        if (auto* dw = dynamic_cast<juce::DocumentWindow*> (getTopLevelComponent()))
        {
            if (! dw->isUsingNativeTitleBar())
                dw->setUsingNativeTitleBar (true);
            for (int i = dw->getNumChildComponents(); --i >= 0;)
                if (auto* b = dynamic_cast<juce::Button*> (dw->getChildComponent (i)))
                    if (b->getButtonText() == "Options")
                        b->setVisible (false);
        }
    }

    void layoutMainPage()
    {
        viewport.setBounds (mainPage.getLocalBounds());
        const int w = juce::jmax (420, viewport.getMaximumVisibleWidth());
        content.setSize (w, contentHeight);
        auto r = juce::Rectangle<int> (0, 0, w, contentHeight).reduced (14, 10);
        for (auto& it : items)
            it.comp->setBounds (r.removeFromTop (it.h));
        footer.setBounds (r.removeFromTop (48));
    }

private:
    void flashFooter (const juce::String& msg)
    {
        footer.setText (msg, juce::dontSendNotification);
        footer.setColour (juce::Label::textColourId, juce::Colour (0xff9fd68a));
        startTimer (1600);
    }

    void timerCallback() override
    {
        footer.setText (defaultFooterText, juce::dontSendNotification);
        footer.setColour (juce::Label::textColourId, juce::Colours::grey);
        stopTimer();
    }

    // bilingual tooltip: English first, Japanese below, blank line between
    static juce::String tip (const char* en, const char* jpText)
    {
        return juce::String::fromUTF8 (en) + "\n\n" + juce::String::fromUTF8 (jpText);
    }

    // readable tooltips: plain (non-bold) font, wider box, extra line spacing
    struct TipLookAndFeel : public juce::LookAndFeel_V4
    {
        static juce::TextLayout layoutTip (const juce::String& text)
        {
            juce::AttributedString s;
            s.setJustification (juce::Justification::topLeft);
            s.append (text, juce::Font (juce::FontOptions (13.5f)), juce::Colour (0xffeaeaea));
            s.setLineSpacing (5.0f);
            juce::TextLayout tl;
            tl.createLayout (s, 400.0f);
            return tl;
        }

        juce::Rectangle<int> getTooltipBounds (const juce::String& text,
                                               juce::Point<int> screenPos,
                                               juce::Rectangle<int> parentArea) override
        {
            const auto tl = layoutTip (text);
            const int w = (int) std::ceil (tl.getWidth())  + 26;
            const int h = (int) std::ceil (tl.getHeight()) + 20;
            return juce::Rectangle<int> (
                       screenPos.x > parentArea.getCentreX() ? screenPos.x - (w + 12) : screenPos.x + 24,
                       screenPos.y > parentArea.getCentreY() ? screenPos.y - (h + 6)  : screenPos.y + 6,
                       w, h)
                   .constrainedWithin (parentArea);
        }

        void drawTooltip (juce::Graphics& g, const juce::String& text, int width, int height) override
        {
            const juce::Rectangle<int> b (width, height);
            g.setColour (juce::Colour (0xff26272e));
            g.fillRect (b);
            g.setColour (juce::Colour (0xff62636c));
            g.drawRect (b);
            layoutTip (text).draw (g, b.reduced (13, 10).toFloat());
        }
    };

    // ---- row components ---------------------------------------------------
    // every parameter row ends with the same right-aligned set:
    // [value box] [↺ reset] [🔒 lock]
    struct SliderRow : public juce::Component
    {
        juce::Label      name;
        juce::Slider     slider;
        juce::TextButton reset, lock;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> att;

        SliderRow()
        {
            addAndMakeVisible (name);
            addAndMakeVisible (slider);
            addAndMakeVisible (reset);
            addAndMakeVisible (lock);
        }
        void resized() override
        {
            auto r = getLocalBounds();
            name.setBounds (r.removeFromLeft (168));
            lock.setBounds  (r.removeFromRight (30).reduced (3));
            reset.setBounds (r.removeFromRight (30).reduced (3));
            slider.setBounds (r);
        }
    };

    struct ToggleRow : public juce::Component
    {
        juce::ToggleButton toggle;
        juce::TextButton   reset, lock;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> att;
        ToggleRow()
        {
            addAndMakeVisible (toggle);
            addAndMakeVisible (reset);
            addAndMakeVisible (lock);
        }
        void resized() override
        {
            auto r = getLocalBounds();
            lock.setBounds  (r.removeFromRight (30).reduced (2));
            reset.setBounds (r.removeFromRight (30).reduced (2));
            toggle.setBounds (r.withTrimmedLeft (4));
        }
    };

    struct ComboRow : public juce::Component
    {
        juce::Label      name;
        juce::ComboBox   combo;
        juce::TextButton reset, lock;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> att;
        ComboRow()
        {
            addAndMakeVisible (name);
            addAndMakeVisible (combo);
            addAndMakeVisible (reset);
            addAndMakeVisible (lock);
        }
        void resized() override
        {
            auto r = getLocalBounds();
            name.setBounds (r.removeFromLeft (168));
            lock.setBounds  (r.removeFromRight (30).reduced (3));
            reset.setBounds (r.removeFromRight (30).reduced (3));
            combo.setBounds (r.reduced (0, 3));
        }
    };

    struct ButtonRow : public juce::Component
    {
        juce::Label      name;
        juce::TextButton btn;
        ButtonRow()
        {
            addAndMakeVisible (name);
            addAndMakeVisible (btn);
        }
        void resized() override
        {
            auto r = getLocalBounds();
            name.setBounds (r.removeFromLeft (168));
            btn.setBounds (r.removeFromLeft (150).reduced (0, 3));
        }
    };

    // ---- builders -----------------------------------------------------------
    void addSection (const char* text)
    {
        auto lbl = std::make_unique<juce::Label>();
        lbl->setText (text, juce::dontSendNotification);
        lbl->setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
        lbl->setColour (juce::Label::textColourId, juce::Colour (0xff45bda5));   // section mint
        content.addAndMakeVisible (*lbl);
        items.push_back ({ lbl.get(), 26 });
        owned.push_back (std::move (lbl));
    }

    // wires one row's 🔒 button to the per-parameter lock and registers a
    // refresher that mirrors the lock state into the row (button glyph,
    // tooltip, greyed controls). `controls` = what to disable while locked.
    void wireLock (juce::TextButton& lockBtn, const juce::String& id,
                   std::vector<juce::Component*> controls)
    {
        lockBtn.onClick = [this, id]
        {
            proc.setParamLocked (id, ! proc.isParamLocked (id));
            syncLockUI();
        };
        lockRefreshers.push_back ([this, &lockBtn, id, controls]
        {
            const bool locked = proc.isParamLocked (id);
            lockBtn.setButtonText (juce::String::fromUTF8 (locked ? "\xF0\x9F\x94\x92"      // 🔒
                                                                  : "\xF0\x9F\x94\x93"));   // 🔓
            lockBtn.setColour (juce::TextButton::buttonColourId,
                               locked ? juce::Colour (0xffffe9ef) : juce::Colours::white);
            lockBtn.setTooltip (locked
                ? tip ("Locked: this value cannot be changed - not by knobs, the reset arrow, "
                       "presets, Reset All or Analyze Auto-Set. Click to unlock.",
                       "ロック中のため変更できません(手動操作・↺・プリセット・Reset All・"
                       "Auto-Setのすべてから保護)。クリックで解除します。")
                : tip ("Lock this parameter: protects the value from manual edits, the reset "
                       "arrow, preset loading, Reset All and Analyze Auto-Set / Refine.",
                       "この項目をロックします。手動操作・↺・プリセット読込・Reset All・"
                       "AnalyzeのAuto-Set/Refineから値を保護します。"));
            for (auto* c : controls)
                c->setEnabled (! locked);
        });
    }

    // re-apply every row's lock state (also called when a host restores it)
    void syncLockUI()
    {
        for (auto& f : lockRefreshers) f();
        lastLockState = proc.lockedIds.joinIntoString (",");
    }

    void addSliderRow (const juce::String& id, const juce::String& displayName, const juce::String& tipText)
    {
        auto row = std::make_unique<SliderRow>();
        row->name.setText (displayName, juce::dontSendNotification);
        row->name.setTooltip (tipText);
        row->name.setFont (juce::Font (juce::FontOptions (13.0f)));

        row->slider.setSliderStyle (juce::Slider::LinearHorizontal);
        row->slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 74, 22);
        row->slider.setTextBoxIsEditable (true);
        row->slider.setTooltip (tipText);
        row->att = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
                        proc.apvts, id, row->slider);

        auto* rp = proc.apvts.getParameter (id);
        row->slider.setDoubleClickReturnValue (true, (double) rp->convertFrom0to1 (rp->getDefaultValue()));

        row->reset.setButtonText (juce::String::fromUTF8 ("\xE2\x86\xBA"));
        row->reset.setTooltip (tip ("Reset to default.", "初期値に戻します。"));
        row->reset.onClick = [rp]
        {
            rp->beginChangeGesture();
            rp->setValueNotifyingHost (rp->getDefaultValue());
            rp->endChangeGesture();
        };
        wireLock (row->lock, id, { &row->slider, &row->reset, &row->name });

        content.addAndMakeVisible (*row);
        items.push_back ({ row.get(), 30 });
        owned.push_back (std::move (row));
    }

    void addComboRow (const juce::String& id, const juce::String& displayName, const juce::String& tipText)
    {
        auto row = std::make_unique<ComboRow>();
        row->name.setText (displayName, juce::dontSendNotification);
        row->name.setTooltip (tipText);
        row->name.setFont (juce::Font (juce::FontOptions (13.0f)));

        auto* rp = proc.apvts.getParameter (id);
        if (auto* cp = dynamic_cast<juce::AudioParameterChoice*> (rp))
            for (int i = 0; i < cp->choices.size(); ++i)
                row->combo.addItem (cp->choices[i], i + 1);
        row->combo.setTooltip (tipText);
        row->att = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
                        proc.apvts, id, row->combo);

        row->reset.setButtonText (juce::String::fromUTF8 ("\xE2\x86\xBA"));
        row->reset.setTooltip (tip ("Reset to default.", "初期値に戻します。"));
        row->reset.onClick = [rp]
        {
            rp->beginChangeGesture();
            rp->setValueNotifyingHost (rp->getDefaultValue());
            rp->endChangeGesture();
        };
        wireLock (row->lock, id, { &row->combo, &row->reset, &row->name });

        content.addAndMakeVisible (*row);
        items.push_back ({ row.get(), 30 });
        owned.push_back (std::move (row));
    }

    void addButtonRow (const juce::String& displayName, const juce::String& btnText,
                       const juce::String& tipText, std::function<void()> onClick)
    {
        auto row = std::make_unique<ButtonRow>();
        row->name.setText (displayName, juce::dontSendNotification);
        row->name.setTooltip (tipText);
        row->name.setFont (juce::Font (juce::FontOptions (13.0f)));
        row->btn.setButtonText (btnText);
        row->btn.setTooltip (tipText);
        row->btn.onClick = std::move (onClick);
        content.addAndMakeVisible (*row);
        items.push_back ({ row.get(), 30 });
        owned.push_back (std::move (row));
    }

    void addToggleRow (const juce::String& id, const juce::String& displayName, const juce::String& tipText)
    {
        auto row = std::make_unique<ToggleRow>();
        row->toggle.setButtonText (displayName);
        row->toggle.setTooltip (tipText);
        row->att = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
                        proc.apvts, id, row->toggle);

        auto* rp = proc.apvts.getParameter (id);
        row->reset.setButtonText (juce::String::fromUTF8 ("\xE2\x86\xBA"));
        row->reset.setTooltip (tip ("Reset to default.", "初期値に戻します。"));
        row->reset.onClick = [rp]
        {
            rp->beginChangeGesture();
            rp->setValueNotifyingHost (rp->getDefaultValue());
            rp->endChangeGesture();
        };
        wireLock (row->lock, id, { &row->toggle, &row->reset });

        content.addAndMakeVisible (*row);
        items.push_back ({ row.get(), 28 });
        owned.push_back (std::move (row));
    }

    // ---- members --------------------------------------------------------------
    static constexpr int kMaxInitialHeight = 720;   // window opens no taller

    VoxMorphProcessor& proc;
    TipLookAndFeel tipLnf;
    juce::LookAndFeel_V4 mainLnf { juce::LookAndFeel_V4::getLightColourScheme() };
    juce::TooltipWindow tooltipWindow { this, 350 };
    juce::Viewport  viewport;    // scroll container
    juce::Component content;     // holds every row; taller than the window
    SpectrumView    spectrum { proc };
    StatusView      status { proc };
    MatchingPanel   matchingPanel { proc };
    PresetPanel     presetPanel { proc };
    AsmrPanel       asmrPanel { proc };
    FxBar           fxBar { proc };
    FnComponent     mainPage;    // MAIN tab page (declared before tabs!)
    juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };
    int contentHeight = 0;

    struct Item { juce::Component* comp; int h; };
    std::vector<Item> items;
    std::vector<std::unique_ptr<juce::Component>> owned;
    // AEIOU Character DETAIL window (child of this editor: destroyed with
    // it, re-fronted instead of duplicated on repeated DETAIL clicks)
    std::unique_ptr<AEIOUCharacterWindow> aeiouWin;
    juce::Label footer;
    juce::String defaultFooterText;

    // undo/redo + per-parameter locks
    juce::TextButton undoBtn { "Undo" }, redoBtn { "Redo" };
    FnTimer histPoll;
    std::vector<std::function<void()>> lockRefreshers;   // one per 🔒 row
    juce::String lastLockState;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VoxMorphEditor)
};
