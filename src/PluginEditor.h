#pragma once
#include "PluginProcessor.h"
#include "VoiceAnalyzer.h"
#include "MatchingEngine.h"
#include "SampleTargetCatalog.h"
#include "AnokoeWidgets.h"

namespace ak = anokoe;   // ANOKOE skin: colours, art and skinned widgets

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

// bilingual tooltip: English first, Japanese below, blank line between.
// (VoxMorphEditor keeps its own private tip() for its existing call sites.)
inline juce::String vmTip (const char* en, const char* jp)
{
    return juce::String::fromUTF8 (en) + "\n\n" + juce::String::fromUTF8 (jp);
}

// Performance Mode (v0.31.0): refresh rate for views that only DRAW. Nothing
// that touches audio, parameter handling or a clip warning goes through here
// — this exists so the redraw/analysis load on the message thread drops while
// the user is running a 64 / 128-sample device buffer. Views call it from
// their own timerCallback and re-arm the timer when the answer changes, which
// is why the helper is a plain function rather than a base class: the timer
// classes below are unrelated and already derive from juce::Timer privately.
inline int vmDrawHz (const VoxMorphProcessor& p, int baseHz)
{
    return p.uiPerfMode.load (std::memory_order_relaxed)
             ? juce::jmax (2, (baseHz * 2) / 3) : baseHz;
}

// Shared spectrum analysis (v0.30.1). One FFT pair per frame feeds BOTH the
// linear SpectrumView and the radial donut view — each frame is two
// 4096-point FFTs, so letting the donut analyse separately would double that
// cost for identical numbers. Views register themselves and are repainted
// after each frame; the analysis is skipped entirely while none is on screen.
class SpectrumData : private juce::Timer
{
public:
    static constexpr int   kCols  = 220;                   // analysed columns
    static constexpr float kFloor = -66.0f, kTop = 6.0f;   // dB display range

    explicit SpectrumData (VoxMorphProcessor& p) : proc (p)
    {
        re.assign ((size_t) kN, 0.0f);
        im.assign ((size_t) kN, 0.0f);
        winIn .assign ((size_t) kN, 0.0f);
        winOut.assign ((size_t) kN, 0.0f);
        smIn .assign ((size_t) kCols, kFloor);
        smOut.assign ((size_t) kCols, kFloor);
        startTimerHz (30);
    }

    // the editor is going away, so the audio thread must stop filling the taps
    ~SpectrumData() override { proc.uiWantsViz.store (false, std::memory_order_relaxed); }

    void addView (juce::Component* c) { views.push_back (c); }

    const std::vector<float>& in()  const { return smIn;  }   // INPUT column dB
    const std::vector<float>& out() const { return smOut; }   // OUTPUT column dB

private:
    static constexpr int kN = 4096;           // FFT size

    void timerCallback() override
    {
        if (const int hz = vmDrawHz (proc, 30); hz != rateHz)
        {
            rateHz = hz;                 // Performance Mode changed the rate
            startTimerHz (hz);
        }

        bool anyShowing = false;
        for (auto& v : views)
            if (v != nullptr && v->isShowing()) { anyShowing = true; break; }

        // Tell the audio thread whether the input/output taps are worth
        // filling at all. While no spectrum view is on screen nobody reads
        // them, and writing them costs two passes over every block.
        if (anyShowing != wanted)
        {
            wanted = anyShowing;
            if (anyShowing) onPos = proc.vizPos.load (std::memory_order_acquire);
            proc.uiWantsViz.store (anyShowing, std::memory_order_relaxed);
        }
        if (! anyShowing) return;

        // after switching the taps back on, wait until a full FFT window of
        // fresh material has been written — otherwise the first frame would
        // analyse whatever the ring still held from the last time it was on
        if (proc.vizPos.load (std::memory_order_acquire) - onPos < (unsigned) kN)
            return;

        // Take a copy of both windows against ONE write position (the two
        // curves should show the same moment), then check how far the audio
        // thread got while we were copying. It writes forward from `pos`, so
        // it needs kVizLen - kN samples before it reaches the oldest sample
        // we took; past that the copy mixes two different moments and the
        // frame is dropped rather than drawn. The margin is 12288 samples
        // (~256 ms at 48 kHz), so this only ever fires if the message thread
        // was stalled for that long -- normal frames are never lost.
        const unsigned pos = proc.vizPos.load (std::memory_order_acquire);
        grabWindow (proc.vizIn,  pos, winIn);
        grabWindow (proc.vizOut, pos, winOut);
        if (proc.vizPos.load (std::memory_order_acquire) - pos
              > (unsigned) (VoxMorphProcessor::kVizLen - kN))
            return;

        analyze (winIn,  smIn);
        analyze (winOut, smOut);

        for (auto& v : views)
            if (v != nullptr && v->isShowing()) v->repaint();
    }

    // copy the kN samples ending at `pos` out of one tap ring. Relaxed loads:
    // the ordering that matters is the acquire on vizPos around the call.
    void grabWindow (const std::vector<std::atomic<float>>& src,
                     unsigned pos, std::vector<float>& dest) const
    {
        const unsigned mask = (unsigned) VoxMorphProcessor::kVizLen - 1;
        for (int i = 0; i < kN; ++i)
            dest[(size_t) i] = src[(size_t) ((pos - (unsigned) kN + (unsigned) i) & mask)]
                                   .load (std::memory_order_relaxed);
    }

    void analyze (const std::vector<float>& src, std::vector<float>& dest)
    {
        for (int i = 0; i < kN; ++i)
        {
            const float w = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) kN);
            re[(size_t) i] = src[(size_t) i] * w;
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

    VoxMorphProcessor& proc;
    std::vector<float> re, im, smIn, smOut;
    std::vector<float> winIn, winOut;   // the tap windows this frame analyses
    std::vector<juce::Component::SafePointer<juce::Component>> views;
    int      rateHz = 30;     // current timer rate (Performance Mode lowers it)
    bool     wanted = false;  // last value published to proc.uiWantsViz
    unsigned onPos  = 0;      // vizPos when the taps were switched back on
};

// Spectrum visualizer: INPUT (mint) and converted OUTPUT (pink) spectra
// overlaid on a 20 Hz - 20 kHz log axis. Pure painter — the numbers come
// from the shared SpectrumData.
class SpectrumView : public juce::Component
{
public:
    explicit SpectrumView (const SpectrumData& d) : data (d) {}

private:
    static constexpr int   kCols  = SpectrumData::kCols;
    static constexpr float kFloor = SpectrumData::kFloor, kTop = SpectrumData::kTop;

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
        ak::paintCard (g, b);

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

        drawCurve (g, data.in(),  r, ak::seriesIn);   // input: mint
        drawCurve (g, data.out(), r, ak::seriesOut);   // output: pink

        g.setFont (juce::Font (juce::FontOptions (11.0f)));        // legend
        g.setColour (juce::Colour (0xff54bda1));
        g.drawText ("Input",  (int) r.getRight() - 110, (int) r.getY() + 2, 50, 14, juce::Justification::left);
        g.setColour (juce::Colour (0xfff08ba5));
        g.drawText ("Output", (int) r.getRight() - 56,  (int) r.getY() + 2, 54, 14, juce::Justification::left);
    }

    const SpectrumData& data;
};

// ---------------------------------------------------------------------------
// Radial ("donut") visualizer, v0.30.1 — the same INPUT/OUTPUT spectra as
// SpectrumView wrapped around a circle. Frequency runs clockwise from the top
// and the radius is the level (inner edge = the -66 dB floor, outer edge =
// +6 dB), so a voice shows up as a star whose points are its formants.
//
// Two deliberate differences from the linear graph next to it, both needed to
// make a ~150 px circle readable (checked against rendered test spectra):
//  * Span is 60 Hz - 12 kHz, not 20 Hz - 20 kHz. On a log axis the empty
//    20-110 Hz region would eat the first QUARTER of the circle and leave the
//    formants squashed into the bottom half.
//  * The 220 analysis columns are averaged down to 72 angular points and
//    drawn as a smooth closed curve. Wrapping all 220 round a ~50 px annulus
//    turns every single harmonic into a needle instead of a lobe.
class RadialSpectrumView : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit RadialSpectrumView (const SpectrumData& d) : data (d)
    {
        setTooltip (vmTip (
            "The spectrum from the graph on the left, wrapped around a circle. Frequency "
            "runs clockwise from 60 Hz at the top through 12 kHz, and the distance from "
            "the centre is the level (inner edge -66 dB, outer edge +6 dB). Mint = your "
            "input, pink = the converted output. The points of the star are the formants "
            "of your voice, so the two shapes show at a glance how far the conversion "
            "moved them.",
            "左のグラフと同じスペクトラムを円形に巻いたものです。周波数は真上の60Hzから"
            "時計回りに12kHzまで進み、中心からの距離がレベルを表します(内側=-66dB、"
            "外側=+6dB)。ミントが入力、ピンクが変換後の出力です。星のとがった部分が声の"
            "フォルマントなので、2つの形を見比べると変換でどれだけ動いたかが一目で分かります。"));
    }

private:
    static constexpr int    kPts  = 72;                  // angular points
    static constexpr double kLoHz = 60.0, kHiHz = 12000.0;

    // SpectrumData column index of a frequency on its 20 Hz - 20 kHz log grid
    static int colFor (double hz)
    {
        return juce::jlimit (0, SpectrumData::kCols,
                   (int) std::lround (SpectrumData::kCols * std::log10 (hz / 20.0) / 3.0));
    }

    // columns -> kPts angular points: mean dB per bucket, then a circular
    // 3-tap smooth (the averaging is what turns needles into lobes)
    static void reduce (const std::vector<float>& v, float* outPts)
    {
        const int lo = colFor (kLoHz), hi = colFor (kHiHz);
        const int span = juce::jmax (kPts, hi - lo);
        float acc[kPts];
        for (int i = 0; i < kPts; ++i)
        {
            const int c0 = lo + i * span / kPts;
            const int c1 = juce::jmax (c0 + 1, lo + (i + 1) * span / kPts);
            double s = 0.0;  int n = 0;
            for (int c = c0; c < c1 && c < (int) v.size(); ++c) { s += v[(size_t) c]; ++n; }
            acc[i] = n > 0 ? (float) (s / n) : SpectrumData::kFloor;
        }
        for (int i = 0; i < kPts; ++i)
            outPts[i] = 0.25f * acc[(i + kPts - 1) % kPts]
                      + 0.50f * acc[i]
                      + 0.25f * acc[(i + 1) % kPts];
    }

    static float radiusFor (float db, float rIn, float rOut)
    {
        const float t = juce::jlimit (0.0f, 1.0f,
            (db - SpectrumData::kFloor) / (SpectrumData::kTop - SpectrumData::kFloor));
        // 6 % margin so a silent band rests just clear of the centre puck
        return rIn + (rOut - rIn) * (0.06f + 0.94f * t);
    }

    static juce::Rectangle<float> circle (juce::Point<float> c, float r)
    {
        return juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (c);
    }

    void drawRing (juce::Graphics& g, const std::vector<float>& v, juce::Point<float> c,
                   float rIn, float rOut, juce::Colour col, float thick) const
    {
        if ((int) v.size() < kPts) return;
        float db[kPts];
        reduce (v, db);

        juce::Point<float> pt[kPts];
        for (int i = 0; i < kPts; ++i)
        {
            const float a = juce::MathConstants<float>::twoPi * (float) i / (float) kPts
                          - juce::MathConstants<float>::halfPi;      // 0 = top
            const float r = radiusFor (db[i], rIn, rOut);
            pt[i] = { c.x + r * std::cos (a), c.y + r * std::sin (a) };
        }

        // smooth closed curve: quadratic segments anchored at the midpoints
        // with each sample as the control point — wraps seamlessly where
        // 12 kHz meets 60 Hz again at the top
        auto mid = [&] (int i, int j) { return (pt[i] + pt[j]) * 0.5f; };
        juce::Path p;
        p.startNewSubPath (mid (kPts - 1, 0));
        for (int i = 0; i < kPts; ++i)
            p.quadraticTo (pt[i], mid (i, (i + 1) % kPts));
        p.closeSubPath();

        g.setColour (col);
        g.strokePath (p, juce::PathStrokeType (thick, juce::PathStrokeType::curved,
                                                      juce::PathStrokeType::rounded));
    }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat().reduced (2.0f);
        ak::paintCard (g, b);

        const auto  c    = b.getCentre();
        const float rOut = 0.5f * juce::jmin (b.getWidth(), b.getHeight()) - 9.0f;
        if (rOut < 26.0f) return;                       // too small to be readable
        const float rIn  = rOut * 0.30f;                // the donut hole

        // the dish the curves sit in: a soft raised disc
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xfffafcfd), c.x, c.y - rOut,
                                                 juce::Colour (0xffeceff3), c.x, c.y + rOut, false));
        g.fillEllipse (circle (c, rOut));
        g.setColour (juce::Colour (0x14000000));
        g.drawEllipse (circle (c, rOut), 1.0f);

        g.setColour (juce::Colour (0x12000000));        // level guide rings
        for (float db : { -48.0f, -24.0f, 0.0f })
            g.drawEllipse (circle (c, radiusFor (db, rIn, rOut)), 0.8f);

        drawRing (g, data.in(),  c, rIn, rOut, juce::Colour (0xff54bda1).withAlpha (0.8f), 1.2f);
        drawRing (g, data.out(), c, rIn, rOut, juce::Colour (0xfff08ba5),                  1.9f);

        // centre puck: covers the crowded low-radius area and gives the donut
        // its raised look
        juce::Path hole;
        hole.addEllipse (circle (c, rIn));
        juce::DropShadow (juce::Colour (0x3a000000), 11, { 0, 3 }).drawForPath (g, hole);
        g.setGradientFill (juce::ColourGradient (juce::Colours::white,       c.x, c.y - rIn,
                                                 juce::Colour (0xffe7ebef),  c.x, c.y + rIn, false));
        g.fillPath (hole);
    }

    const SpectrumData& data;
};

// ---------------------------------------------------------------------------
// AEIOU vowel-mix donut, v0.30.2 — a true donut chart whose five arcs are
// proportional to how much /a/ /i/ /u/ /e/ /o/ the engine currently hears.
//
// The numbers are NOT re-estimated here. They are the engine's own vowel
// coordinate (height = openness from F1, frontness = log F2/F1) run through
// VowelAdaptiveWarp::anchorWeights — literally the mix the AEIOU Character
// warp is acting on. Deriving a second vowel estimate from the visualizer
// spectrum would be a fresh unvalidated estimator that disagrees with the
// DSP, which is the trap v0.28.3 / v0.28.4 already fell into.
//
// Consequence: the engine only tracks vowels while AEIOU Character is ON with
// Amount > 0 (that feature is what drives the tracking). With it off there is
// no data, so the chart says so instead of drawing stale values.
class VowelDonut : public juce::Component, public juce::SettableTooltipClient,
                   private juce::Timer
{
public:
    explicit VowelDonut (VoxMorphProcessor& p) : proc (p)
    {
        setTooltip (vmTip (
            "How much of each Japanese vowel the engine hears in your voice right now, "
            "as a share of the whole. This is the exact vowel mix the AEIOU Character "
            "feature uses to pick its per-vowel formant offsets, so it shows you why a "
            "Character is doing what it is doing. It needs AEIOU Character switched on "
            "(FORMANT section) with Amount above 0 - the vowel tracking is part of that "
            "feature and does not run otherwise. The chart fades while you are not "
            "speaking, because the estimate is only meaningful on voiced sound.",
            "いま話している声に含まれる母音(あいうえお)の割合です。AEIOU Character機能が"
            "母音ごとのフォルマント補正を選ぶのに使っている値そのものなので、Characterが"
            "なぜその効き方をしているのかが分かります。表示にはFORMANTセクションの"
            "AEIOU Characterがオンで、Amountが0より大きいことが必要です(母音の推定自体が"
            "この機能の一部で、オフのときは動作しません)。声を出していない間は推定が"
            "無意味なため薄く表示されます。"));
        startTimerHz (30);
    }

private:
    static constexpr int kV = 5;                                  // a i u e o
    static constexpr const char* kLbl[kV] = { "A", "I", "U", "E", "O" };

    static juce::Colour colourFor (int i)
    {
        static const juce::Colour c[kV] = {
            juce::Colour (0xfff08ba5),   // A  pink   (matches the output curve)
            juce::Colour (0xff7999db),   // I  mint   (matches the input curve)
            juce::Colour (0xffa79ee0),   // U  lavender
            juce::Colour (0xffe3a63c),   // E  amber
            juce::Colour (0xff6fb2dc)    // O  sky
        };
        return c[juce::jlimit (0, kV - 1, i)];
    }

    void timerCallback() override
    {
        if (const int hz = vmDrawHz (proc, 30); hz != rateHz)
        { rateHz = hz; startTimerHz (hz); }   // Performance Mode
        if (! isShowing()) return;

        const bool  live = proc.uiVowelActive.load (std::memory_order_relaxed);
        const float conf = proc.uiVowelConf  .load (std::memory_order_relaxed);
        const bool  good = live && conf > 0.02f;

        if (good)
        {
            float w[kV];
            VowelAdaptiveWarp::anchorWeights (proc.uiVowelH.load (std::memory_order_relaxed),
                                              proc.uiVowelF.load (std::memory_order_relaxed), w);
            for (int i = 0; i < kV; ++i)
                sm[i] += 0.22f * (w[i] - sm[i]);      // glide, ~150 ms
        }
        // hold the last mix while silent, but fade it out so a frozen shape is
        // never mistaken for a live reading
        fade   += 0.15f * ((good ? 1.0f : 0.22f) - fade);
        active  = live;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat().reduced (2.0f);
        ak::paintCard (g, b);

        const auto  c    = b.getCentre();
        const float rOut = 0.5f * juce::jmin (b.getWidth(), b.getHeight()) - 9.0f;
        if (rOut < 26.0f) return;
        const float rIn  = rOut * 0.52f;              // wider hole: this is a
        const auto  box  = juce::Rectangle<float> (rOut * 2.0f, rOut * 2.0f).withCentre (c);

        g.setGradientFill (juce::ColourGradient (juce::Colour (0xfffafcfd), c.x, c.y - rOut,
                                                 juce::Colour (0xffeceff3), c.x, c.y + rOut, false));
        g.fillEllipse (box);
        g.setColour (juce::Colour (0x14000000));
        g.drawEllipse (box, 1.0f);

        if (! active)
        {
            drawPuck (g, c, rIn);          // puck FIRST — the message sits on top
            g.setColour (juce::Colour (0xff8f9ab5));
            g.setFont (juce::Font (juce::FontOptions (10.5f)));
            g.drawFittedText ("AEIOU Character\nis off",
                              box.toNearestInt(), juce::Justification::centred, 2);
            return;
        }

        float sum = 0.0f;
        for (int i = 0; i < kV; ++i) sum += juce::jmax (0.0f, sm[i]);
        if (sum <= 1.0e-6f) { drawPuck (g, c, rIn); return; }

        const float twoPi = juce::MathConstants<float>::twoPi;
        const float gap   = 0.018f;                   // radians between arcs
        float a0 = 0.0f;                              // JUCE: 0 = top, clockwise
        int   top = 0;
        for (int i = 0; i < kV; ++i)
        {
            const float share = juce::jmax (0.0f, sm[i]) / sum;
            const float a1    = a0 + share * twoPi;
            if (share > sm[top] / sum) top = i;

            if (a1 - a0 > gap * 2.0f)
            {
                juce::Path seg;
                seg.addPieSegment (box, a0 + gap, a1 - gap, rIn / rOut);
                g.setColour (colourFor (i).withMultipliedAlpha (fade));
                g.fillPath (seg);

                // vowel letter on the arc, once the arc is wide enough to hold it
                if (share > 0.085f)
                {
                    const float am = 0.5f * (a0 + a1);
                    const float rm = 0.5f * (rIn + rOut);
                    const juce::Point<float> lp (c.x + rm * std::sin (am),
                                                 c.y - rm * std::cos (am));
                    g.setColour (juce::Colours::white.withMultipliedAlpha (fade));
                    g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
                    g.drawText (kLbl[i], juce::Rectangle<float> (18.0f, 14.0f).withCentre (lp)
                                                                             .toNearestInt(),
                                juce::Justification::centred);
                }
            }
            a0 = a1;
        }

        drawPuck (g, c, rIn);

        // dominant vowel + its share in the hole
        const float topShare = juce::jmax (0.0f, sm[top]) / sum;
        g.setColour (ak::ink.withMultipliedAlpha (juce::jmax (0.35f, fade)));
        g.setFont (juce::Font (juce::FontOptions (juce::jmin (22.0f, rIn * 0.9f),
                                                  juce::Font::bold)));
        g.drawText (kLbl[top],
                    juce::Rectangle<float> (rIn * 1.6f, rIn * 0.95f)
                        .withCentre ({ c.x, c.y - rIn * 0.20f }).toNearestInt(),
                    juce::Justification::centred);
        g.setColour (juce::Colour (0xff8f9ab5));
        g.setFont (juce::Font (juce::FontOptions (juce::jmin (11.0f, rIn * 0.42f))));
        g.drawText (juce::String (juce::roundToInt (topShare * 100.0f)) + "%",
                    juce::Rectangle<float> (rIn * 1.6f, rIn * 0.6f)
                        .withCentre ({ c.x, c.y + rIn * 0.44f }).toNearestInt(),
                    juce::Justification::centred);
    }

    static void drawPuck (juce::Graphics& g, juce::Point<float> c, float rIn)
    {
        juce::Path hole;
        hole.addEllipse (juce::Rectangle<float> (rIn * 2.0f, rIn * 2.0f).withCentre (c));
        juce::DropShadow (juce::Colour (0x3a000000), 11, { 0, 3 }).drawForPath (g, hole);
        g.setGradientFill (juce::ColourGradient (juce::Colours::white,      c.x, c.y - rIn,
                                                 juce::Colour (0xffe7ebef), c.x, c.y + rIn, false));
        g.fillPath (hole);
    }

    VoxMorphProcessor& proc;
    int rateHz = 30;   // current timer rate; Performance Mode lowers it
    float sm[kV] = { 0.2f, 0.2f, 0.2f, 0.2f, 0.2f };
    float fade = 0.22f;
    bool  active = false;
};

// ---------------------------------------------------------------------------
// Level rings, v0.30.3 (re-laid out v0.30.4) — the four level meters as one
// donut, split by CHANNEL SIDE:
//
//        left half = L            right half = R
//        outer lane = input       outer lane = input      (mint)
//        inner lane = output      inner lane = output     (pink)
//
// Both halves start at the bottom and rise to 12 o'clock, so the top of the
// dial is the top of the scale on both sides and the two halves are mirror
// images — a glance at the symmetry tells you the L/R balance.
//
// Input is measured before the noise gate and the Pre FX (so you can see your
// mic even while the gate has it shut); output is what actually leaves the
// plugin. With a mono bus the two halves read the same.
class LevelRingsDonut : public juce::Component, public juce::SettableTooltipClient,
                        private juce::Timer
{
public:
    explicit LevelRingsDonut (VoxMorphProcessor& p) : proc (p)
    {
        setTooltip (vmTip (
            "All four level meters in one dial, split by channel: the LEFT half is the "
            "left channel and the RIGHT half is the right channel, each with two lanes - "
            "the outer lane is the input (mint) and the inner lane is the converted "
            "output (pink). Both halves rise from the bottom to 12 o'clock, which is the "
            "top of the scale, so the L and R halves are mirror images and any imbalance "
            "shows up as asymmetry. The scale is -60 to +6 dB with a red zone above "
            "0 dBFS; the small tick is the recent peak and turns red on a clip. The input "
            "is measured before the noise gate, so it keeps showing your mic even while "
            "the gate is closed. With a mono input or output both halves read the same.",
            "入力L/R・出力L/Rの4つのレベルを1つにまとめた表示です。左半分がLチャンネル、"
            "右半分がRチャンネルで、それぞれ2列あります(外側=入力(ミント)、内側="
            "変換後の出力(ピンク))。左右とも下から12時方向へ伸び、12時がスケールの最大"
            "です。左右が鏡写しになるので、バランスが崩れると非対称になってすぐ分かります。"
            "目盛りは-60〜+6dBで、0dBFSより上は赤いゾーンです。細い目盛りが直近のピークで、"
            "クリップすると赤くなります。入力はノイズゲートより前で測っているので、ゲートが"
            "閉じている間もマイクの状態が分かります。モノラルの場合は左右が同じ値になります。"));
        startTimerHz (30);
    }

private:
    // meters, in draw order: [side][lane], side 0 = L, lane 0 = input
    static constexpr int kSide = 2, kLane = 2, kR = kSide * kLane;
    static constexpr float kZeroDb = 60.0f / 66.0f;    // where 0 dBFS lands

    // Both halves run bottom -> top, mirrored. The bottom gap is the wider of
    // the two: with a small one the L and R arcs joined into a single U and
    // the dial read as one meter instead of two. The top gap keeps the two
    // scales from merging where they both hit maximum.
    // JUCE angles: 0 = 12 o'clock, increasing clockwise.
    static constexpr float kGapB = 0.17f, kGapT = 0.10f;

    static float angleAt (int side, float p)
    {
        const float pi   = juce::MathConstants<float>::pi;
        const float span = pi - kGapB - kGapT;
        return side == 0 ? (pi + kGapB) + p * span      // left half, clockwise
                         : (pi - kGapB) - p * span;     // right half, anticlockwise
    }

    static float pos (float lin)                    // linear -> 0..1 on the scale
    {
        const float db = juce::Decibels::gainToDecibels (lin, -60.0f);
        return juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 66.0f);
    }

    static void strokeArc (juce::Graphics& g, juce::Point<float> c, float r, int side,
                           float from, float to, float w, juce::Colour col, bool round)
    {
        const float a0 = angleAt (side, from), a1 = angleAt (side, to);
        if (std::abs (a1 - a0) < 1.0e-4f) return;
        juce::Path a;
        a.addCentredArc (c.x, c.y, r, r, 0.0f, a0, a1, true);
        g.setColour (col);
        g.strokePath (a, juce::PathStrokeType (w, juce::PathStrokeType::curved,
                                round ? juce::PathStrokeType::rounded
                                      : juce::PathStrokeType::butt));
    }

    // lane colour: input mint / output pink, the same on both sides — the
    // side is already carried by which half of the dial you are looking at
    static juce::Colour laneColour (int lane)
    {
        return lane == 0 ? juce::Colour (0xff7999db) : juce::Colour (0xfff08ba5);
    }

    static int idx (int side, int lane) { return side * kLane + lane; }

    void timerCallback() override
    {
        if (const int hz = vmDrawHz (proc, 30); hz != rateHz)
        { rateHz = hz; startTimerHz (hz); }   // Performance Mode
        if (! isShowing()) return;
        // order must match idx(): L-in, L-out, R-in, R-out
        const VoxMorphProcessor::LevelMeter* src[kR] = {
            &proc.uiInL, &proc.uiOutL, &proc.uiInR, &proc.uiOutR };
        bool changed = false;
        for (int i = 0; i < kR; ++i)
        {
            const float pk  = src[i]->peak.load (std::memory_order_relaxed);
            const float r   = pos (src[i]->rms.load (std::memory_order_relaxed));
            const float p   = pos (pk);
            if (r != lvl[i] || p != pkPos[i]) changed = true;
            lvl[i] = r;  pkPos[i] = p;
            clip[i] = juce::Decibels::gainToDecibels (pk, -60.0f) >= -0.1f;
        }
        if (changed) repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat().reduced (2.0f);
        ak::paintCard (g, b);

        const auto  c    = b.getCentre();
        const float rOut = 0.5f * juce::jmin (b.getWidth(), b.getHeight()) - 9.0f;
        if (rOut < 26.0f) return;
        const float rIn  = rOut * 0.40f;

        g.setGradientFill (juce::ColourGradient (juce::Colour (0xfffafcfd), c.x, c.y - rOut,
                                                 juce::Colour (0xffeceff3), c.x, c.y + rOut, false));
        g.fillEllipse (juce::Rectangle<float> (rOut * 2.0f, rOut * 2.0f).withCentre (c));
        g.setColour (juce::Colour (0x14000000));
        g.drawEllipse (juce::Rectangle<float> (rOut * 2.0f, rOut * 2.0f).withCentre (c), 1.0f);

        const float rTop  = rOut - 3.0f;             // clear of the dish outline
        const float band  = rTop - rIn;
        const float gap   = band * 0.09f;            // only two lanes now: wider
        const float ringW = (band - gap) / (float) kLane;

        for (int side = 0; side < kSide; ++side)
            for (int lane = 0; lane < kLane; ++lane)
            {
                const int   i    = idx (side, lane);
                const float rMid = rTop - ((float) lane * (ringW + gap)) - ringW * 0.5f;

                strokeArc (g, c, rMid, side, 0.0f, 1.0f, ringW,           // track
                           juce::Colour (0x12000000), false);
                strokeArc (g, c, rMid, side, kZeroDb, 1.0f, ringW,        // 0 dB..+6
                           juce::Colour (0x22e23b52), false);             // danger zone

                if (lvl[i] > 0.004f)
                    strokeArc (g, c, rMid, side, 0.0f, lvl[i], ringW * 0.92f,
                               laneColour (lane), true);

                if (pkPos[i] > 0.004f)                                    // peak tick
                {
                    const float a  = angleAt (side, pkPos[i]);
                    const float s  = std::sin (a), co = std::cos (a);
                    const float r0 = rMid - ringW * 0.5f, r1 = rMid + ringW * 0.5f;
                    g.setColour (clip[i] ? juce::Colour (0xffe23b52)
                                         : laneColour (lane).darker (0.45f));
                    g.drawLine (c.x + r0 * s, c.y - r0 * co,
                                c.x + r1 * s, c.y - r1 * co, 1.8f);
                }
            }

        // 0 dBFS marker across both lanes, on each side
        g.setColour (juce::Colour (0x66e23b52));
        for (int side = 0; side < kSide; ++side)
        {
            const float a = angleAt (side, kZeroDb);
            const float s = std::sin (a), co = std::cos (a);
            g.drawLine (c.x + (rIn + 1.0f) * s, c.y - (rIn + 1.0f) * co,
                        c.x + rTop * s,         c.y - rTop * co, 1.0f);
        }

        // centre puck: L | R, matching the half you are looking at
        juce::Path hole;
        hole.addEllipse (juce::Rectangle<float> (rIn * 2.0f, rIn * 2.0f).withCentre (c));
        juce::DropShadow (juce::Colour (0x3a000000), 11, { 0, 3 }).drawForPath (g, hole);
        g.setGradientFill (juce::ColourGradient (juce::Colours::white,      c.x, c.y - rIn,
                                                 juce::Colour (0xffe7ebef), c.x, c.y + rIn, false));
        g.fillPath (hole);

        if (rIn >= 17.0f)
        {
            g.setColour (juce::Colour (0x1a000000));
            g.drawLine (c.x, c.y - rIn * 0.52f, c.x, c.y + rIn * 0.52f, 1.0f);
            g.setColour (juce::Colour (0xff8d9694));
            g.setFont (juce::Font (juce::FontOptions (juce::jmin (13.0f, rIn * 0.62f),
                                                      juce::Font::bold)));
            g.drawText ("L", juce::Rectangle<float> (rIn * 0.8f, rIn * 0.9f)
                                 .withCentre ({ c.x - rIn * 0.44f, c.y }).toNearestInt(),
                        juce::Justification::centred);
            g.drawText ("R", juce::Rectangle<float> (rIn * 0.8f, rIn * 0.9f)
                                 .withCentre ({ c.x + rIn * 0.44f, c.y }).toNearestInt(),
                        juce::Justification::centred);
        }
    }

    VoxMorphProcessor& proc;
    int rateHz = 30;   // current timer rate; Performance Mode lowers it
    float lvl[kR]   = { 0.0f, 0.0f, 0.0f, 0.0f };
    float pkPos[kR] = { 0.0f, 0.0f, 0.0f, 0.0f };
    bool  clip[kR]  = { false, false, false, false };
};

// VISUALIZER row (v0.30.1, extended v0.30.2 / v0.30.3): the linear spectrum
// on the left, then three square donuts — the radial spectrum, the AEIOU
// vowel mix and the four level rings. On narrow windows the donuts shrink
// rather than disappearing.
class VisualizerRow : public juce::Component
{
public:
    VisualizerRow (juce::Component& lin, std::initializer_list<juce::Component*> donutList)
        : linear (lin), donuts (donutList)
    {
        addAndMakeVisible (linear);
        for (auto* d : donuts) addAndMakeVisible (*d);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        const int n    = juce::jmax (1, (int) donuts.size());
        const int side = juce::jlimit (juce::jmin (70, r.getWidth() / (n + 1)), r.getHeight(),
                                       (int) ((float) r.getWidth() * 0.20f));
        for (int i = (int) donuts.size(); --i >= 0;)
        {
            donuts[(size_t) i]->setBounds (r.removeFromRight (side));
            r.removeFromRight (4);
        }
        linear.setBounds (r);
    }

private:
    juce::Component& linear;
    std::vector<juce::Component*> donuts;
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
            "Estimated delay inside VoxMorph: engine lookahead (43 ms; half with Legacy Low "
            "Latency) + enabled external FX plugins; the standalone app also adds the audio "
            "device buffers. Pitch / Formant / Voice Quality / Breath run inside one shared "
            "pipeline and add no delay of their own. Delays outside the app (OS, OBS, "
            "Discord, audio interface driver) are NOT included. "
            "LOW < 35 ms, MID 35-70 ms, HIGH > 70 ms.")
            + "\n\n" + juce::String::fromUTF8 (
            "VoxMorph内部の推定遅延です。エンジンの先読み(43ms、Legacy Low Latency時は約半分)+"
            "有効な外部FXプラグインの合計で、スタンドアロン版はオーディオバッファ分も加算します。"
            "Pitch/Formant/Voice Quality/Breathは同一パイプライン内の処理のため追加遅延は"
            "ありません。OS・OBS・Discord・オーディオインターフェースのドライバなど、アプリ外の"
            "遅延は含みません。LOW<35ms / MID 35〜70ms / HIGH>70ms。"));
        startTimerHz (4);
    }

private:
    void timerCallback() override
    {
        if (const int hz = vmDrawHz (proc, 4); hz != rateHz)
        { rateHz = hz; startTimerHz (hz); }   // Performance Mode
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

    // v0.35.0: a compact read-out that lives inside the OUTPUT section.
    // The numbers, thresholds and breakdown are unchanged since v0.17.0.
    void paint (juce::Graphics& g) override
    {
        const float ms = engMs + fxMs + bufMs;
        auto r = getLocalBounds();
        auto l1 = r.removeFromTop (16);

        g.setColour (ak::ink);
        g.setFont (ak::font (12.0f));
        const auto text = "Latency: " + (known ? juce::String (ms, 1) + " ms"
                                               : juce::String ("--"));
        const int tw = 96;
        g.drawText (text, l1.removeFromLeft (tw), juce::Justification::centredLeft, false);
        if (known)
        {
            auto chip = l1.removeFromLeft (40).reduced (4, 1);
            g.setColour (chipColour());
            g.fillRoundedRectangle (chip.toFloat(), 8.0f);
            g.setColour (juce::Colours::white);
            g.setFont (ak::font (9.5f, true));
            g.drawText (chipText(), chip, juce::Justification::centred, false);

            juce::String d = "engine " + juce::String (engMs, 1)
                           + " + FX " + juce::String (fxMs, 1);
            if (bufMs > 0.0f) d += " + buf " + juce::String (bufMs, 1);
            g.setColour (ak::heading.withAlpha (0.8f));
            g.setFont (ak::font (10.0f));
            g.drawText (d + " ms", r, juce::Justification::topLeft, false);
        }
    }

    juce::String chipText() const
    {
        const float ms = engMs + fxMs + bufMs;
        return ms < 35.0f ? "LOW" : ms < 70.0f ? "MID" : "HIGH";
    }
    juce::Colour chipColour() const
    {
        const float ms = engMs + fxMs + bufMs;
        return ms < 35.0f ? ak::badge : ms < 70.0f ? ak::badgeMid : ak::badgeHigh;
    }

    VoxMorphProcessor& proc;
    int rateHz = 4;   // current timer rate; Performance Mode lowers it
    float engMs = 0.0f, fxMs = 0.0f, bufMs = 0.0f;
    bool  known = false;
};

// ===========================================================================
// Shared helpers (v0.30.0)
// ===========================================================================

// ---- presets --------------------------------------------------------------
// One folder for every preset writer: the PRESETS tab, the MATCHING tab's
// "SAVE PRESET" and the MAIN tab's preset bar all use this.
inline juce::File voxMorphPresetDir()
{
    auto d = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                 .getChildFile ("VoxMorph").getChildFile ("Presets");
    d.createDirectory();
    return d;
}

inline juce::Array<juce::File> voxMorphPresetFiles()
{
    auto files = voxMorphPresetDir().findChildFiles (juce::File::findFiles, false, "*.vmpreset");
    std::sort (files.begin(), files.end(),
               [] (const juce::File& a, const juce::File& b)
               { return a.getFileName().compareIgnoreCase (b.getFileName()) < 0; });
    return files;
}

// Apply a .vmpreset to the processor: ONE undo step, locked parameters keep
// their current value, and parameters missing from the file fall back to
// their default (the semantics PresetPanel has had since v0.19.0 — this is
// that code, lifted out so the MAIN tab's preset bar behaves identically).
inline bool voxMorphApplyPreset (VoxMorphProcessor& proc, const juce::File& file,
                                 int& applied, int& lockedKept)
{
    applied = 0;  lockedKept = 0;
    auto xml = juce::XmlDocument::parse (file);
    if (xml == nullptr || ! xml->hasTagName (proc.apvts.state.getType()))
        return false;

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
    return true;
}

// ---- standalone audio device access (MONITOR + Audio Settings window) -----
// All of these return "nothing" outside the standalone app, so every caller
// degrades to a no-op in a DAW instead of needing its own #if.
inline juce::AudioDeviceManager* vmDeviceManager()
{
   #if VOXMORPH_HAS_STANDALONE_HOLDER
    if (auto* h = juce::StandalonePluginHolder::getInstance())
        return &h->deviceManager;
   #endif
    return nullptr;
}

// Performance Mode buffer helper (v0.31.0, STANDALONE ONLY).
//
// Device buffer size and engine lookahead are different things: this only
// touches the device buffer, and the engine's analysis is never shortened to
// match it. Prefers 64, then 128, then 256 samples, taking the first size the
// current device actually offers; if applying it fails the previous setup is
// restored, and the caller keeps the old size so the user can go back by hand.
// Returns the size that ended up active, or 0 if nothing could be applied.
inline int vmApplyBufferSize (int wanted)
{
    auto* dm = vmDeviceManager();
    if (dm == nullptr) return 0;
    auto* dev = dm->getCurrentAudioDevice();
    if (dev == nullptr) return 0;

    const auto sizes = dev->getAvailableBufferSizes();
    if (! sizes.contains (wanted)) return 0;

    auto setup = dm->getAudioDeviceSetup();
    const int previous = setup.bufferSize;
    if (previous == wanted) return wanted;

    setup.bufferSize = wanted;
    if (dm->setAudioDeviceSetup (setup, true).isNotEmpty())
    {
        setup.bufferSize = previous;          // re-init failed: put it back
        dm->setAudioDeviceSetup (setup, true);
        return 0;
    }
    return wanted;
}

inline int vmCurrentBufferSize()
{
    if (auto* dm = vmDeviceManager())
        if (auto* dev = dm->getCurrentAudioDevice())
            return dev->getCurrentBufferSizeSamples();
    return 0;
}

inline juce::StringArray vmOutputDeviceNames()
{
    juce::StringArray names;
    if (auto* dm = vmDeviceManager())
        if (auto* t = dm->getCurrentDeviceTypeObject())
        {
            t->scanForDevices();
            names = t->getDeviceNames (false);      // false = output side
        }
    return names;
}

inline juce::String vmCurrentOutputDevice()
{
    if (auto* dm = vmDeviceManager())
    {
        juce::AudioDeviceManager::AudioDeviceSetup s;
        dm->getAudioDeviceSetup (s);
        return s.outputDeviceName;
    }
    return {};
}

// Switches ONLY the output device; the input device, sample rate and buffer
// size are left exactly as they are. "" = success, otherwise an error text.
inline juce::String vmSetOutputDevice (const juce::String& name)
{
    auto* dm = vmDeviceManager();
    if (dm == nullptr)
        return juce::String::fromUTF8 ("スタンドアロン版でのみ使用できます。");
    juce::AudioDeviceManager::AudioDeviceSetup s;
    dm->getAudioDeviceSetup (s);
    if (s.outputDeviceName == name) return {};
    s.outputDeviceName = name;
    s.useDefaultOutputChannels = true;
    return dm->setAudioDeviceSetup (s, true);
}

// ---- reusable parameter row (v0.31.0: the only row type in the skinned UI) -
// One row = [control] [value] [reset] [lock], skinned with the ANOKOE art.
// Kinds cover everything the UI needs: a horizontal slider, a checkbox, a big
// rotary knob, a dropdown, or a plain action button.
//
// The row owns its lock wiring but does NOT poll: whoever hosts a set of rows
// calls refreshLock() on them (the editor does it from its 3 Hz history poll,
// the BETA / Audio Settings windows from their own small timer), so a lock
// toggled anywhere — or restored by the host — stays in sync everywhere.
class ParamRow : public juce::Component
{
public:
    enum class Kind { slider, toggle, knob, combo, button };

    // slider / toggle / knob / combo
    ParamRow (VoxMorphProcessor& p, const juce::String& paramId, Kind k,
              const juce::String& displayName, const juce::String& tipText,
              ak::Tone t = ak::Tone::blue)
        : proc (&p), id (paramId), kind (k), tone (t)
    {
        rp = proc->apvts.getParameter (id);

        // v0.36.5: the unit moves out of the label and into the read-out, so
        // "F1 Shift (st)" becomes the label "F1 Shift" next to a box reading
        // "-0.00st". Only rows that HAVE a read-out can take one; a trailing
        // "(...)" on a toggle or a dropdown is left alone.
        juce::String label = displayName.trim();
        if ((k == Kind::slider || k == Kind::knob) && label.endsWithChar (')'))
        {
            const int open = label.lastIndexOfChar ('(');
            if (open > 0)
            {
                unit  = label.substring (open + 1, label.length() - 1).trim();
                label = label.substring (0, open).trim();
            }
        }

        name.setText (label, juce::dontSendNotification);
        name.setTooltip (tipText);
        name.setFont (ak::font (12.5f));
        name.setColour (juce::Label::textColourId, ak::ink);
        if (kind != Kind::toggle)
            addAndMakeVisible (name);

        switch (kind)
        {
            case Kind::toggle:
                toggle.setButtonText (displayName);
                toggle.setTooltip (tipText);
                addAndMakeVisible (toggle);
                if (rp != nullptr)
                    bAtt = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
                               proc->apvts, id, toggle);
                break;

            case Kind::slider:
            case Kind::knob:
                slider.setSliderStyle (kind == Kind::knob
                                         ? juce::Slider::RotaryHorizontalVerticalDrag
                                         : juce::Slider::LinearHorizontal);
                if (kind == Kind::knob)
                    slider.setRotaryParameters (ak::knobStart(), ak::knobEnd(), true);
                slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
                slider.setTooltip (tipText);
                addAndMakeVisible (slider);
                addAndMakeVisible (value);
                value.setName ("vmValue");
                value.setTooltip (tipText);
                value.setFont (ak::font (12.0f));
                value.setJustificationType (juce::Justification::centredRight);
                value.setEditable (false, true, false);
                value.setColour (juce::Label::textColourId, ak::ink);
                value.onTextChange = [this]
                {
                    slider.setValue (slider.getValueFromText (value.getText()),
                                     juce::sendNotificationSync);
                    syncValueText();
                };
                // Type over the number, not the unit: the editor opens on the
                // bare value, so "2.00st" offers "2.00", and typing 3 + Enter
                // reads back "3.00st". Slider::getValueFromText drops the
                // suffix anyway, so leaving it in place also still works.
                value.onEditorShow = [this]
                {
                    if (auto* ed = value.getCurrentTextEditor())
                    {
                        ed->setText (withoutUnit (value.getText()), false);
                        ed->selectAll();
                    }
                };
                slider.onValueChange = [this] { syncValueText(); };
                if (rp != nullptr)
                {
                    sAtt = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
                               proc->apvts, id, slider);
                    slider.setDoubleClickReturnValue (
                        true, (double) rp->convertFrom0to1 (rp->getDefaultValue()));
                }
                // after the attachment: it rewrites the text conversion but
                // never touches the suffix, which is appended on top of it
                slider.setTextValueSuffix (unit);
                syncValueText();
                break;

            case Kind::combo:
                if (auto* cp = dynamic_cast<juce::AudioParameterChoice*> (rp))
                    for (int i = 0; i < cp->choices.size(); ++i)
                        combo.addItem (cp->choices[i], i + 1);
                combo.setTooltip (tipText);
                addAndMakeVisible (combo);
                if (rp != nullptr)
                    cAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
                               proc->apvts, id, combo);
                break;

            case Kind::button:
                break;
        }

        reset.setTooltip (vmTip ("Reset to default.", "初期値に戻します。"));
        reset.onClick = [this]
        {
            if (rp == nullptr) return;
            rp->beginChangeGesture();
            rp->setValueNotifyingHost (rp->getDefaultValue());
            rp->endChangeGesture();
        };
        lock.onClick = [this]
        {
            proc->setParamLocked (id, ! proc->isParamLocked (id));
            refreshLock();
            if (onLockChanged) onLockChanged();
        };
        addAndMakeVisible (reset);
        addAndMakeVisible (lock);
        refreshLock();
    }

    // plain action button row (no parameter): [label] [button]
    ParamRow (const juce::String& displayName, const juce::String& btnText,
              const juce::String& tipText, std::function<void()> onClick)
        : kind (Kind::button)
    {
        juce::ignoreUnused (displayName);
        action.setButtonText (btnText);
        action.setTooltip (tipText);
        action.onClick = std::move (onClick);
        addAndMakeVisible (action);
    }

    // Called by the host panel to mirror the current lock state into the row.
    void refreshLock()
    {
        if (kind == Kind::button || proc == nullptr) return;
        const bool locked = proc->isParamLocked (id);
        lock.setImage (locked ? "ui_mark_S_Lock_png" : "ui_mark_S_Unlock_png");
        lock.setTooltip (locked
            ? vmTip ("Locked: this value cannot be changed - not by knobs, the reset arrow, "
                     "presets, Reset All or Matching. Click to unlock.",
                     "ロック中のため変更できません(手動操作・リセット・プリセット・Reset All・"
                     "Matchingのすべてから保護)。クリックで解除します。")
            : vmTip ("Lock this parameter: protects the value from manual edits, the reset "
                     "arrow, preset loading, Reset All and Matching.",
                     "この項目をロックします。手動操作・リセット・プリセット読込・Reset All・"
                     "Matchingから値を保護します。"));
        toggle.setEnabled (! locked);
        slider.setEnabled (! locked);
        combo .setEnabled (! locked);
        value .setEnabled (! locked);
        name  .setEnabled (! locked);
        reset .setEnabled (! locked);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        if (kind != Kind::button && proc != nullptr && proc->isParamLocked (id))
        {
            g.setColour (ak::muteFill.withAlpha (0.55f));
            g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (1.0f, 1.0f), 8.0f);
        }

        if (tree == Tree::none) return;
        const float x  = (float) ak::kTreeRail;
        const float cy = (float) getHeight() * 0.5f;
        g.setColour (ak::treeLine);
        g.fillRect (x, 0.0f, 1.0f, tree == Tree::last ? cy : (float) getHeight());
        g.fillRect (x, cy, (float) ak::kTreeIndent - 6.0f, 1.0f);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        if (tree != Tree::none) r.removeFromLeft (ak::kTreeIndent);

        if (kind == Kind::button)
        {
            action.setBounds (r.removeFromLeft (juce::jmin (128, r.getWidth()))
                                .withSizeKeepingCentre (juce::jmin (128, r.getWidth()),
                                                        juce::jmin (26, r.getHeight())));
            return;
        }

        if (kind == Kind::knob)
        {
            // knob centred above a read-out line: [label] .. [value][reset][lock]
            auto readout = r.removeFromBottom (26);
            lock .setBounds (readout.removeFromRight (20).withSizeKeepingCentre (20, 22));
            reset.setBounds (readout.removeFromRight (20).withSizeKeepingCentre (20, 22));
            readout.removeFromRight (2);
            value.setBounds (readout.removeFromRight (ak::kValueW).reduced (0, 2));
            readout.removeFromRight (6);
            name.setBounds (readout);

            const int side = juce::jmin (juce::jmin (r.getHeight(), r.getWidth()), ak::kKnobH);
            slider.setBounds (juce::Rectangle<int> (side, side)
                                  .withCentre ({ r.getCentreX(), r.getCentreY() }));
            return;
        }

        lock .setBounds (r.removeFromRight (20).withSizeKeepingCentre (20, 22));
        reset.setBounds (r.removeFromRight (20).withSizeKeepingCentre (20, 22));
        r.removeFromRight (2);

        if (kind == Kind::toggle)
        {
            toggle.setBounds (r.withTrimmedLeft (2));
            return;
        }

        // slider / combo rows: [label] [control] [value]
        const bool tall = r.getHeight() >= 50;      // stacked (label над control)
        auto ctrl = r;
        if (tall)
        {
            name.setBounds (r.removeFromTop (r.getHeight() / 2 - 2));
            ctrl = r;
        }
        else
        {
            const int lw = juce::jlimit (60, ak::kLabelW, r.getWidth() / 3);
            name.setBounds (r.removeFromLeft (lw));
            ctrl = r;
        }
        if (kind == Kind::combo)
        {
            combo.setBounds (ctrl.reduced (0, juce::jmax (0, (ctrl.getHeight() - 28) / 2)));
            return;
        }
        value.setBounds (ctrl.removeFromRight (ak::kValueW).reduced (0, 4));
        ctrl.removeFromRight (4);
        slider.setBounds (ctrl);
    }

    // A row can be drawn as a child of the heading above it: a bracket down
    // the left edge with a tick into the label. `last` closes the bracket
    // with an L instead of running it through.
    enum class Tree { none, mid, last };
    void setTree (Tree t) { tree = t; resized(); repaint(); }

    // for the flow lines: where the knob sits, and the label's TEXT box —
    // the routes connect to the glyphs, so the slack in the label component
    // must not count as part of the node
    juce::Rectangle<int> knobBounds()  const { return slider.getBounds(); }
    juce::Rectangle<int> labelBounds() const { return name.getBounds(); }
    juce::Rectangle<int> labelTextBounds() const
    {
        const int w = 2 + (int) std::ceil (juce::GlyphArrangement::getStringWidth (
                                               ak::font (12.5f), name.getText()));
        return name.getBounds().withWidth (juce::jmin (w, name.getWidth()));
    }

    juce::ToggleButton& getToggle() { return toggle; }
    juce::ComboBox&     getCombo()  { return combo; }
    juce::TextButton&   getButton() { return action; }
    std::function<void()> onLockChanged;

private:
    void syncValueText()
    {
        if (rp == nullptr) return;
        value.setText (slider.getTextFromValue (slider.getValue()), juce::dontSendNotification);
    }

    juce::String withoutUnit (juce::String t) const
    {
        t = t.trim();
        return (unit.isNotEmpty() && t.endsWith (unit))
                 ? t.dropLastCharacters (unit.length()).trim() : t;
    }

    VoxMorphProcessor* proc = nullptr;
    juce::String id;
    juce::String unit;                 // "st" / "dB" / "%" / "Hz", "" if none
    Kind kind;
    Tree tree { Tree::none };
    ak::Tone tone { ak::Tone::blue };
    juce::RangedAudioParameter* rp = nullptr;
    juce::Label        name, value;
    juce::Slider       slider;
    juce::ToggleButton toggle;
    juce::ComboBox     combo;
    juce::TextButton   action;
    ak::IconButton     reset { "reset", "ui_mark_S_Reset_png" };
    ak::IconButton     lock  { "lock",  "ui_mark_S_Unlock_png" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>   sAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>   bAtt;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> cAtt;
};

// ---- OUTPUT section level meter (v0.30.0) ---------------------------------
// Horizontal bar of the plugin's REAL output level (after mute, output gain,
// ASMR pan and the Post FX chain). Filled bar = RMS, thin marker = peak, on a
// -60 .. +6 dB scale; the fill turns amber near 0 dBFS and pink when clipping.
class OutputMeter : public juce::Component, public juce::SettableTooltipClient,
                    private juce::Timer
{
public:
    explicit OutputMeter (VoxMorphProcessor& p) : proc (p)
    {
        setTooltip (vmTip (
            "Level of the signal actually leaving VoxMorph: after Mute, Output Gain, the "
            "ASMR position and any Post FX. The bar is the average (RMS) level and the thin "
            "marker is the recent peak. Aim for the peak to sit in the mint / amber area; "
            "pink means 0 dBFS is being hit and the output may be clipping - lower Output Gain.",
            "VoxMorphから実際に出ている音のレベルです(Mute・Output Gain・ASMR位置・Post FXの"
            "すべてを通過した後)。バーが平均(RMS)レベル、細い線が直近のピークです。ピークが"
            "ミント〜アンバーの範囲に収まるのが目安で、ピンクは0dBFSに達している状態=音が"
            "割れる可能性があります。その場合はOutput Gainを下げてください。"));
        startTimerHz (30);
    }

private:
    static float pos (float lin)                       // linear -> 0..1 on the scale
    {
        const float db = juce::Decibels::gainToDecibels (lin, -60.0f);
        return juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 66.0f);   // -60 .. +6 dB
    }

    void timerCallback() override
    {
        if (const int hz = vmDrawHz (proc, 30); hz != rateHz)
        { rateHz = hz; startTimerHz (hz); }   // Performance Mode
        if (! isShowing()) return;
        // the louder of the two output channels — the standard reading for a
        // single-bar level meter
        const float r = juce::jmax (proc.uiOutL.rms .load (std::memory_order_relaxed),
                                    proc.uiOutR.rms .load (std::memory_order_relaxed));
        const float p = juce::jmax (proc.uiOutL.peak.load (std::memory_order_relaxed),
                                    proc.uiOutR.peak.load (std::memory_order_relaxed));
        if (r == rms && p == pk) return;               // idle: no repaint
        rms = r;  pk = p;
        repaint();
    }

    // v0.31.0: the ANOKOE volume strip art. The -60..+6 dB reading and the
    // ballistics are unchanged; only the drawing moved to the new skin.
    void paint (juce::Graphics& g) override
    {
        auto row = getLocalBounds();
        auto valBox = row.removeFromRight (56).reduced (0, 4);
        row.removeFromRight (4);

        auto bar = row.toFloat().reduced (0.0f, 3.0f);
        const auto base = ak::image ("ui_volume_basew_png");
        const auto fill = ak::image ("ui_volume_fill_png");
        if (base.isValid())
            g.drawImage (base, bar, juce::RectanglePlacement::stretchToFit, false);
        else
        {
            g.setColour (juce::Colour (0xffe9edfb));
            g.fillRoundedRectangle (bar, 5.0f);
        }

        const float peakDb = juce::Decibels::gainToDecibels (pk, -60.0f);
        const float w = bar.getWidth() * pos (rms);
        if (w > 1.0f)
        {
            juce::Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (bar.withWidth (w).getSmallestIntegerContainer());
            if (fill.isValid())
                g.drawImage (fill, bar, juce::RectanglePlacement::stretchToFit, false);
            else
            {
                g.setColour (ak::seriesOut);
                g.fillRoundedRectangle (bar.withWidth (w), 5.0f);
            }
        }
        if (pk > 0.0f)
        {
            g.setColour (peakDb >= -0.1f ? juce::Colour (0xffe23b52)
                                         : ak::heading.withAlpha (0.7f));
            g.fillRect (bar.getX() + juce::jmax (1.0f, bar.getWidth() * pos (pk) - 1.0f),
                        bar.getY(), 2.0f, bar.getHeight());
        }

        g.setColour (ak::ink);
        g.setFont (ak::font (11.0f));
        g.drawText (pk > 0.0f ? juce::String (peakDb, 1) + " dB" : juce::String ("-inf"),
                    valBox, juce::Justification::centredRight);
    }

    VoxMorphProcessor& proc;
    int rateHz = 30;   // current timer rate; Performance Mode lowers it
    float rms = -1.0f, pk = -1.0f;
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
        x->setAttribute ("r" + juce::String (i + 1), p.rel[i]);
    }
    // v0.29.0 per-vowel table. Written as child elements so a profile saved
    // by an older build (which simply has none) still loads — Matching then
    // takes its global path instead of the vowel-matched one.
    static const char* vw[5] = { "a", "i", "u", "e", "o" };
    for (int v = 0; v < 5; ++v)
    {
        if (! p.vow[v].valid()) continue;
        auto* e = x->createNewChildElement ("VOWEL");
        e->setAttribute ("id", vw[v]);
        e->setAttribute ("frames", p.vow[v].frames);
        e->setAttribute ("f0", p.vow[v].f0Hz);
        for (int i = 0; i < 3; ++i)
        {
            e->setAttribute ("f" + juce::String (i + 1), p.vow[v].F[i]);
            e->setAttribute ("l" + juce::String (i + 1), p.vow[v].L[i]);
            e->setAttribute ("r" + juce::String (i + 1), p.vow[v].rel[i]);
        }
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
        // Profiles written before v0.29.0 carry no reliability. Treating a
        // missing value as 0 would make the matcher discard every band, so
        // an absent attribute means "assume measurable" (which is what the
        // old code implicitly did).
        p.rel[i] = x.hasAttribute ("r" + juce::String (i + 1))
                 ? (float) x.getDoubleAttribute ("r" + juce::String (i + 1)) : 1.0f;
    }
    static const char* vw[5] = { "a", "i", "u", "e", "o" };
    for (auto* e : x.getChildWithTagNameIterator ("VOWEL"))
    {
        const auto id = e->getStringAttribute ("id");
        for (int v = 0; v < 5; ++v)
        {
            if (id != vw[v]) continue;
            auto& w = p.vow[v];
            w.frames = e->getIntAttribute ("frames");
            w.f0Hz   = (float) e->getDoubleAttribute ("f0");
            for (int i = 0; i < 3; ++i)
            {
                w.F[i]   = (float) e->getDoubleAttribute ("f" + juce::String (i + 1));
                w.L[i]   = (float) e->getDoubleAttribute ("l" + juce::String (i + 1));
                w.rel[i] = (float) e->getDoubleAttribute ("r" + juce::String (i + 1));
            }
        }
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
        ak::paintCard (g, b);
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
        // still see which shape belongs to which series (spec 4.4). Only
        // series that are actually valid appear -- the panel can drop a
        // series entirely (e.g. Matched is not produced without a Refine
        // step) and the legend follows without leaving a ghost entry.
        struct LI { juce::Colour col; Glyph gl; const char* lbl; bool on; };
        const LI items[4] = {
            { cy, Glyph::filledDot,  "Current",   you       != nullptr && you->valid()       },
            { ct, Glyph::ring,       "Target",    target    != nullptr && target->valid()    },
            { ce, Glyph::diamond,    "Estimated", estimated != nullptr && estimated->valid() },
            { cc, Glyph::concentric, "Matched",   conv      != nullptr && conv->valid()      },
        };
        g.setFont (juce::Font (juce::FontOptions (11.0f)));
        const int y = (int) r.getY() + 4;
        int x = (int) r.getRight() - 4;
        for (int i = 3; i >= 0; --i)
        {
            if (! items[i].on) continue;
            const int w = 14 + (int) juce::Font (juce::FontOptions (11.0f))
                                        .getStringWidth (items[i].lbl) + 12;
            x -= w;
            drawLegendGlyph (g, (float) x + 5.5f, (float) y + 6.0f,
                             items[i].col, items[i].gl);
            g.setColour (items[i].col);
            g.drawText (items[i].lbl, x + 14, y - 1, w - 14, 14, juce::Justification::left);
        }

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
            static constexpr float dashes[2] = { 5.0f, 4.0f };
            juce::Path dp;
            juce::PathStrokeType (1.5f).createDashedStroke (dp, path, dashes, 2);
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

// Square tile shared by the Matching tab's TargetCharacter row and the
// MyVoice section (Record, MyVoiceFile). Top ~60 % of the tile is
// reserved for a future icon (a placeholder shape stands in for now);
// the bottom carries a short label. Selected / unselected states differ
// in both fill AND outline so the state is visible without colour.
//
// TargetCharacter tiles set clickingToggles = true and share a radio
// group; MyVoice tiles are momentary and never light up "selected"
// (they trigger an action instead).
enum class TileIconKind { character, file, record };

class VoiceTileButton : public juce::TextButton
{
public:
    VoiceTileButton (juce::String stableIdIn,
                     juce::String labelIn,
                     TileIconKind iconKindIn,
                     bool         clickingToggles = true)
        : juce::TextButton (labelIn),           // so setButtonText() updates the tile label
          targetId (std::move (stableIdIn)),
          fileTarget (iconKindIn == TileIconKind::file),
          iconKind (iconKindIn)
    {
        setClickingTogglesState (clickingToggles);
        setTriggeredOnMouseDown (false);
    }

    const juce::String targetId;
    const bool         fileTarget;
    const TileIconKind iconKind;

    void paintButton (juce::Graphics& g, bool hover, bool /*down*/) override
    {
        const auto b        = getLocalBounds().toFloat().reduced (1.5f);
        const bool selected = getToggleState();

        const juce::Colour bg = selected ? juce::Colour (0xff54bda1)  // mint
                                         : juce::Colour (0xfffcfaf9); // near-white
        const juce::Colour edge = selected ? ak::heading
                                           : juce::Colour (0xffe4dadd);
        g.setColour (bg);
        g.fillRoundedRectangle (b, 10.0f);
        if (hover && ! selected)
        {
            g.setColour (juce::Colour (0x22a889f4));
            g.fillRoundedRectangle (b, 10.0f);
        }
        g.setColour (edge);
        g.drawRoundedRectangle (b, 10.0f, selected ? 2.0f : 1.0f);

        // icon area: top ~60 %. Placeholder shape until real icons ship.
        const float iconH = b.getHeight() * 0.55f;
        auto iconR = b.reduced (10.0f).withHeight (iconH);
        g.setColour (selected ? juce::Colour (0xfffcfaf9)
                              : juce::Colour (0xffbfd9d2));
        switch (iconKind)
        {
            case TileIconKind::file:
                g.drawRoundedRectangle (iconR.reduced (iconR.getWidth() * 0.18f,
                                                        iconR.getHeight() * 0.12f),
                                        4.0f, 1.6f);
                g.drawLine (iconR.getX() + iconR.getWidth() * 0.30f,
                            iconR.getY() + iconR.getHeight() * 0.18f,
                            iconR.getX() + iconR.getWidth() * 0.55f,
                            iconR.getY() + iconR.getHeight() * 0.18f, 1.6f);
                break;
            case TileIconKind::record:
            {
                // record dot: solid circle in the accent colour
                const float rad = std::min (iconR.getWidth(), iconR.getHeight()) * 0.28f;
                g.setColour (juce::Colour (0xffd65a7a));   // Error/record red
                g.fillEllipse (iconR.getCentreX() - rad, iconR.getCentreY() - rad,
                               rad * 2.0f, rad * 2.0f);
                break;
            }
            case TileIconKind::character:
            default:
                for (int i = 0; i < 3; ++i)
                {
                    juce::Path arc;
                    const float rad = 5.0f + (float) i * 5.0f;
                    arc.addCentredArc (iconR.getCentreX(), iconR.getCentreY(),
                                       rad, rad, 0.0f,
                                       -0.9f, 0.9f, true);
                    g.strokePath (arc, juce::PathStrokeType (1.4f));
                }
                break;
        }

        // label: bottom area
        g.setColour (selected ? juce::Colour (0xfffcfaf9)
                              : juce::Colour (0xff606a68));
        g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
        auto textR = b.withY (b.getY() + iconH + 4.0f)
                      .withHeight (b.getHeight() - iconH - 6.0f);
        // read via getButtonText() so recBtn.setButtonText("Recording...")
        // during a capture updates the tile visibly
        g.drawFittedText (getButtonText(), textR.toNearestInt(),
                          juce::Justification::centred, 2);
    }
};

// Old name kept as an alias so the TargetCharacterButton member/vector
// types in MatchingPanel don't have to change in this commit.
using TargetCharacterButton = VoiceTileButton;

// MATCHING tab (v0.28.x). Sections top to bottom:
//   1) TargetCharacter -- pick a built-in target voice profile, or load a
//      target file / .vmprofile
//   2) MyVoice        -- record your own voice for the CURRENT profile
//   3) AutoMatching   -- MATCH runs MatchingEngine::autoSet and writes the
//      derived parameters; the graph compares Current, Target and the
//      Estimated prediction (see MatchingEngine::predictEstimated).
//      SAVE PRESET writes a normal .vmpreset (same file format as the
//      PRESETS tab); no separate "matching result" file type.
//
// The Auto-Set formulas (kPitchBias, kRangeBoost, jlimit ranges) live in
// dsp/MatchingEngine.h and are byte-exact copies of the v0.27.0
// AnalyzePanel behaviour. The panel only sequences the UI, drives the
// capture, applies changes through history.group / isParamLocked, and
// updates the graph. MatchingEngine::refine() remains in the header for
// future use / API compatibility, but MATCH AGAIN was retired from the UI
// in the v0.28 spec revision -- Matched is not rendered in the graph. See
// VoxMorph_Matching_UI_Design_Spec.txt and v028 correction instructions
// for context; Phase 2+ items (analyzeDetailed, GraphCard renderers,
// parameter registry) are tracked in HANDOVER.
class MatchingPanel : public juce::Component, private juce::Timer
{
public:
    explicit MatchingPanel (VoxMorphProcessor& p) : proc (p)
    {
        auto initHeading = [this] (juce::Label& l, const char* t)
        {
            l.setText (t, juce::dontSendNotification);
            l.setFont (juce::Font (juce::FontOptions (13.5f, juce::Font::bold)));
            l.setColour (juce::Label::textColourId, ak::heading);
            addAndMakeVisible (l);
        };
        // ASCII only -- avoids any codepage confusion on Windows toolchains
        // and matches the "TargetCharacter / MyVoice / AutoMatching" spec
        initHeading (hTargetCharacter, "TargetCharacter");
        initHeading (hMyVoice,         "MyVoice");
        initHeading (hAutoMatching,    "AutoMatching");

        addAndMakeVisible (recBtn);
        addAndMakeVisible (myVoiceFileBtn);
        for (auto* b : { &playBtn, &saveTargetProfBtn, &saveMyVoiceProfBtn,
                         &resetAllBtn, &matchBtn, &savePresetBtn,
                         &savePresetOkBtn, &savePresetCancelBtn })
            addAndMakeVisible (*b);
        recBtn.setTooltip (juce::String::fromUTF8 (
            "Records your microphone input for the CURRENT profile.\n"
            "マイク入力を録音してMyVoiceプロファイルにします。"));
        myVoiceFileBtn.setTooltip (juce::String::fromUTF8 (
            "Load an audio file or .vmprofile as MyVoice (does not touch "
            "the Target selection).\n音声ファイルまたは.vmprofileを"
            "MyVoiceとして読み込みます(Target選択は変更されません)。"));
        myVoiceFileBtn.onClick = [this] { loadMyVoiceFile(); };
        recPlayChk.setTooltip (juce::String::fromUTF8 (
            "When checked, the target file plays while you record.\n"
            "チェックすると録音と同時にターゲットを再生します。"));
        recPlayChk.setToggleState (true, juce::dontSendNotification);   // default ON
        addAndMakeVisible (recPlayChk);

        // TargetCharacter buttons: N built-in profiles + one "TargetFile"
        // tile, all in a shared radio group so exactly one is selected at
        // a time. Initial selection = index 0 (Feminine Standard).
        int nSamples = 0;
        const auto* samples = getSampleTargets (nSamples);
        for (int i = 0; i < nSamples; ++i)
        {
            auto btn = std::make_unique<VoiceTileButton> (
                juce::String (samples[i].id), juce::String (samples[i].displayEn),
                TileIconKind::character, /*clickingToggles*/ true);
            btn->setRadioGroupId (kTargetRadioGroup, juce::dontSendNotification);
            btn->setTooltip (juce::String::fromUTF8 (samples[i].displayJp));
            // Only act when this button is being turned ON. A shared radio
            // group turns the OTHERS off with a notification, which fires
            // their onClick too -- see the TargetFile handler below for what
            // that was costing.
            auto* raw = btn.get();
            btn->onClick = [this, i, raw]
            {
                if (! raw->getToggleState()) return;
                selectSampleTarget (i);
            };
            addAndMakeVisible (*btn);
            targetButtons.push_back (std::move (btn));
        }
        {
            auto btn = std::make_unique<VoiceTileButton> (
                juce::String ("target_file"), juce::String ("TargetFile"),
                TileIconKind::file, /*clickingToggles*/ true);
            targetFileButton = btn.get();
            btn->setRadioGroupId (kTargetRadioGroup, juce::dontSendNotification);
            btn->setTooltip (juce::String::fromUTF8 (
                "Load a voice audio file (wav/aiff/mp3/m4a/flac, first 60 s) or a "
                ".vmprofile as the target.\n音声ファイル(60秒まで)または.vmprofileを"
                "ターゲットとして読み込みます。"));
            // Clicking TargetFile toggles it selected; we immediately restore
            // the previous selection so a cancelled chooser leaves the UI
            // where it was, then re-select TargetFile on load success.
            //
            // The toggle-state test is what stops the file chooser opening
            // when the user picks a different target. JUCE turns the other
            // buttons in a radio group off via setToggleState(false,
            // sendNotification), and that DOES fire their onClick -- so
            // selecting any character used to open the file dialog, because
            // TargetFile was being switched off.
            btn->onClick = [this]
            {
                if (targetFileButton == nullptr || ! targetFileButton->getToggleState())
                    return;                       // being switched off, not chosen
                restoreTargetSelectionUi();
                loadTargetFile();
            };
            addAndMakeVisible (*btn);
            targetButtons.push_back (std::move (btn));
        }

        durBox.addItem ("5 s",  5);
        durBox.addItem ("10 s", 10);
        durBox.addItem ("15 s", 15);
        durBox.setSelectedId (10, juce::dontSendNotification);
        durBox.setTooltip (juce::String::fromUTF8 (
            "Recording length. Longer = more frames = a steadier profile.\n"
            "録音時間。長いほど分析フレームが増え、プロファイルが安定します。"));
        addAndMakeVisible (durBox);

        for (auto* l : { &p1Lbl, &p2Lbl, &outLbl, &matchStatus, &saveHint })
        {
            l->setJustificationType (juce::Justification::topLeft);
            l->setFont (juce::Font (juce::FontOptions (12.0f)));
            addAndMakeVisible (*l);
        }
        matchStatus.setFont (juce::Font (juce::FontOptions (11.5f, juce::Font::bold)));
        matchStatus.setColour (juce::Label::textColourId, ak::heading);
        matchStatus.setJustificationType (juce::Justification::centred);

        graph.you       = &prof1;
        graph.target    = &prof2;
        graph.estimated = &profE;
        graph.conv      = nullptr;      // Matched series retired with MATCH AGAIN
        graph.param = [this] (const char* id)
        {
            auto* v = proc.apvts.getRawParameterValue (id);
            return v != nullptr ? v->load() : 0.0f;
        };
        addAndMakeVisible (graph);

        playBtn.setButtonText ("Play");
        playBtn.setTooltip (juce::String::fromUTF8 (
            "Preview the loaded target audio through the plugin output.\n"
            "読み込んだファイルを出力から再生します。"));
        saveTargetProfBtn.setButtonText ("Save Profile...");
        saveTargetProfBtn.setTooltip (juce::String::fromUTF8 (
            "Save the currently selected target as a .vmprofile (audio-less).\n"
            "現在のターゲットを.vmprofile(音声なし)として保存します。"));
        saveMyVoiceProfBtn.setButtonText ("Save Profile...");
        saveMyVoiceProfBtn.setTooltip (juce::String::fromUTF8 (
            "Save your last analysed MyVoice as a .vmprofile.\n"
            "直前に測定/読込したMyVoiceを.vmprofileとして保存します。"));
        resetAllBtn.setButtonText ("Reset All to Defaults");
        resetAllBtn.setTooltip (juce::String::fromUTF8 (
            "Resets every conversion parameter to its default, exactly like the "
            "button on the PRESETS tab. Locked parameters keep their values and "
            "it is one Undo step.\n"
            "全ての変換パラメータを初期値に戻します(PRESETSタブの同名ボタンと同じ動作)。"
            "ロック中の項目は保持され、Undoで元に戻せます。"));
        matchBtn.setButtonText ("MATCH");
        matchBtn.setTooltip (juce::String::fromUTF8 (
            "Auto-Set: derive parameters from the Current -> Target difference. "
            "One Undo step; locked parameters keep their values.\n"
            "CurrentとTargetの差からパラメータを算出して書き込みます。1 Undo、"
            "ロック項目は保持。"));
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
            // "With target play" is a convenience, not a precondition: the
            // built-in Characters and .vmprofile targets carry no audio, so
            // recording must still go ahead when there is nothing to play.
            if (recPlayChk.getToggleState())
                startPlayForCapture();
            proc.capFromOutput = false;
            startCapture (recBtn, waitingCapture);
        };
        playBtn.onClick  = [this]
        {
            if (proc.prevPos.load() >= 0) proc.prevPos = -1;
            else if (proc.prevLen.load() > 0) proc.prevPos = 0;
        };
        saveTargetProfBtn.onClick   = [this] { saveTargetProfile(); };
        saveMyVoiceProfBtn.onClick  = [this] { saveMyVoiceProfile(); };
        resetAllBtn.onClick         = [this] { resetAllParameters(); };
        matchBtn.onClick      = [this] { doMatch(); };
        savePresetBtn.onClick = [this] { showSavePreset (true); };
        savePresetOkBtn.onClick     = [this] { savePreset(); };
        savePresetCancelBtn.onClick = [this] { showSavePreset (false); };

        // Start with NO target selected -- the user picks a Character
        // tile or loads a TargetFile first, then MATCH becomes available.
        clearTargetButtonSelection();
        selectedSampleIndex = -1;
        targetFileActive    = false;
        prof2                = VoiceProfile{};
        proc.lastTarget      = VoiceProfile{};
        currentTargetName    .clear();
        p2Lbl.setText ("Target: --", juce::dontSendNotification);
        proc.prevPos = -1;
        proc.prevLen = 0;
        showSavePreset (false); // hide the name editor
        updateMatchStatus();
        startTimerHz (10);
    }

    // v0.31.2: sits on the ANOKOE page gradient, so it draws its own card
    void paint (juce::Graphics& g) override
    {
        ak::paintCard (g, getLocalBounds().toFloat());
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (16, 10);

        // shared tile size for both TargetCharacter and MyVoice rows so
        // Record / MyVoiceFile match the Target tiles pixel-for-pixel.
        const int tileGap  = 8;
        const int cols     = getWidth() >= 650 ? (int) targetButtons.size() : 3;
        const int tileSize = std::clamp ((r.getWidth() - tileGap * (cols - 1)) / cols,
                                         72, 96);

        // ── TargetCharacter (top) ──
        hTargetCharacter.setBounds (r.removeFromTop (20));
        {
            const int nb   = (int) targetButtons.size();
            const int rows = (nb + cols - 1) / cols;
            auto grid = r.removeFromTop (rows * tileSize + (rows - 1) * tileGap);
            for (int i = 0; i < nb; ++i)
            {
                const int col = i % cols;
                const int row = i / cols;
                targetButtons[(size_t) i]->setBounds (grid.getX() + col * (tileSize + tileGap),
                                                      grid.getY() + row * (tileSize + tileGap),
                                                      tileSize, tileSize);
            }
        }
        r.removeFromTop (4);
        auto arow = r.removeFromTop (28);
        playBtn.setBounds     (arow.removeFromLeft (72).withHeight (26));
        saveTargetProfBtn.setBounds (arow.removeFromLeft (140).withHeight (26).translated (8, 0));
        p2Lbl.setBounds (r.removeFromTop (32).withTrimmedLeft (2));

        r.removeFromTop (6);

        // ── MyVoice (middle): Record + MyVoiceFile as square tiles ──
        hMyVoice.setBounds (r.removeFromTop (20));
        {
            auto tiles = r.removeFromTop (tileSize);
            recBtn.setBounds (tiles.removeFromLeft (tileSize));
            tiles.removeFromLeft (tileGap);
            myVoiceFileBtn.setBounds (tiles.removeFromLeft (tileSize));
        }
        r.removeFromTop (4);
        auto vopts = r.removeFromTop (28);
        durBox.setBounds (vopts.removeFromLeft (72).withHeight (26));
        vopts.removeFromLeft (8);
        recPlayChk.setBounds (vopts.removeFromLeft (140).withHeight (26));
        vopts.removeFromLeft (10);
        saveMyVoiceProfBtn.setBounds (vopts.removeFromLeft (
            std::min (130, std::max (90, vopts.getWidth() / 2))).withHeight (26));
        vopts.removeFromLeft (8);
        resetAllBtn.setBounds (vopts.withHeight (26));   // takes what's left
        p1Lbl.setBounds (r.removeFromTop (32).withTrimmedLeft (2));

        r.removeFromTop (6);

        // ── AutoMatching (bottom): heading, action row, optional save
        //    editor, THEN the comparison graph, THEN status line ──
        hAutoMatching.setBounds (r.removeFromTop (20));

        {
            auto actionRow = r.removeFromTop (44);
            matchBtn.setBounds      (actionRow.removeFromLeft (140).withHeight (36));
            savePresetBtn.setBounds (actionRow.removeFromRight (150).withHeight (36));
            matchStatus.setBounds   (actionRow.reduced (10, 6));
        }
        r.removeFromTop (4);

        if (savePresetVisible)
        {
            auto sr = r.removeFromTop (40);
            saveHint.setBounds (sr.removeFromTop (16).withTrimmedLeft (2));
            saveNameEdit.setBounds        (sr.removeFromLeft (240).withHeight (24));
            savePresetOkBtn.setBounds     (sr.removeFromLeft (80) .withHeight (24).translated (10, 0));
            savePresetCancelBtn.setBounds (sr.removeFromLeft (80) .withHeight (24).translated (14, 0));
            r.removeFromTop (4);
        }

        // Status pinned to the very bottom of the AutoMatching block; the
        // graph fills the rest below the action / save editor. It carries
        // several lines after a MATCH (values, thresholds, applied count and
        // the "bands disagree" warning), so give it real room.
        outLbl.setBounds (r.removeFromBottom (juce::jlimit (24, 76, r.getHeight() / 3))
                            .withTrimmedLeft (2));
        r.removeFromBottom (2);
        graph.setBounds (r.reduced (0, 2));
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
        b.setButtonText ("REC...");   // short enough for the square tile
    }

    // Starts target playback alongside a capture. Built-in Characters and
    // .vmprofile targets have no audio, so this is simply a no-op there --
    // the recording itself always proceeds.
    void startPlayForCapture()
    {
        if (proc.prevLen.load() <= 0) return;
        proc.prevPos = 0;
        playStartedByCapture = true;
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
            recBtn.setButtonText ("Record");
            const auto captured = analyzeCapture();
            if (captured.valid())
                applyMyVoiceProfile (captured, juce::String::fromUTF8 ("Recorded"));
            else
                status (juce::String::fromUTF8 (
                    "録音の解析に失敗しました(有声区間が不足)。もう一度お試しください。"));
        }
        playBtn.setButtonText (proc.prevPos.load() >= 0 ? "Stop" : "Play");
        playBtn.setEnabled (proc.prevLen.load() > 0);
    }

    // ── target loading ───────────────────────────────────────────────────
    void invalidateForTargetChange()
    {
        // stale "N APPLIED" from the previous match no longer describes
        // the new Target vs the current parameters -- clear it so the
        // status line matches reality (spec 2.6 / 4.1 / 7.2).
        nSet = nLocked = 0;
        refreshEstimated();
        graph.repaint();
        updateMatchStatus();
    }

    // ── target selection: single source of truth for tile toggle state ──
    // Radio-group behaviour alone can't cover every path (initial pre-
    // populate, FileChooser cancel, re-click on the already selected
    // tile). All of them go through these two helpers so the visible
    // state and (selectedSampleIndex / targetFileActive) stay in sync.
    void clearTargetButtonSelection()
    {
        for (auto& b : targetButtons)
        {
            b->setToggleState (false, juce::dontSendNotification);
            b->repaint();
        }
    }

    void selectOnlyTargetButton (TargetCharacterButton* selected)
    {
        for (auto& b : targetButtons)
        {
            const bool on = b.get() == selected;
            b->setToggleState (on, juce::dontSendNotification);
            b->repaint();
        }
    }

    void selectSampleTarget (int index)
    {
        int n = 0;
        const auto* samples = getSampleTargets (n);
        if (! juce::isPositiveAndBelow (index, n)) return;

        selectedSampleIndex = index;
        targetFileActive    = false;
        selectOnlyTargetButton (targetButtons[(size_t) index].get());

        const auto& s = samples[index];
        prof2 = s.profile;
        proc.lastTarget = prof2;
        proc.prevLen = 0;    // sample targets have no audio to Play
        proc.prevPos = -1;
        currentTargetName = juce::String::fromUTF8 (s.displayJp) + " (built-in)";
        p2Lbl.setText (juce::String::fromUTF8 ("Target: ") + currentTargetName + "\n" + fmt (prof2),
                       juce::dontSendNotification);
        invalidateForTargetChange();
    }

    void selectTargetFileButton()
    {
        targetFileActive    = true;
        selectedSampleIndex = -1;
        selectOnlyTargetButton (targetFileButton);
    }

    // Called at the start of a TargetFile click (so the UI reflects the
    // previous choice while the FileChooser is up) and on any load
    // failure / cancel path. On load success the callback re-selects
    // TargetFile via selectTargetFileButton().
    void restoreTargetSelectionUi()
    {
        if (targetFileActive && targetFileButton != nullptr)
        {
            selectOnlyTargetButton (targetFileButton);
            return;
        }
        if (selectedSampleIndex >= 0
            && (size_t) selectedSampleIndex < targetButtons.size())
        {
            selectOnlyTargetButton (targetButtons[(size_t) selectedSampleIndex].get());
            return;
        }
        clearTargetButtonSelection();
    }

    // ── shared audio-file helper (used by Target and MyVoice loads) ──
    struct DecodedMono
    {
        std::vector<float> samples;
        double             sampleRate = 0.0;
        juce::String       error;      // empty on success
    };

    static DecodedMono decodeMonoForAnalysis (const juce::File& file,
                                              double outputSampleRate,
                                              double maxSeconds)
    {
        DecodedMono r;
        r.sampleRate = outputSampleRate;
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (file));
        if (rd == nullptr || rd->sampleRate <= 0
            || rd->numChannels == 0 || rd->lengthInSamples < 3)
        {
            r.error = "Could not read: " + file.getFileName();
            return r;
        }
        const int nIn = (int) std::min<juce::int64> (
            rd->lengthInSamples, (juce::int64) (rd->sampleRate * maxSeconds));
        juce::AudioBuffer<float> tb ((int) rd->numChannels, nIn);
        if (! rd->read (&tb, 0, nIn, 0, true, true))
        {
            r.error = "Could not decode: " + file.getFileName();
            return r;
        }
        const double ratio = rd->sampleRate / outputSampleRate;
        const int nOut = std::max (0, (int) std::floor ((nIn - 2) / ratio));
        if (nOut <= 0)
        {
            r.error = juce::String::fromUTF8 ("音声が短すぎます: ") + file.getFileName();
            return r;
        }
        r.samples.assign ((size_t) nOut, 0.0f);
        const int nch = tb.getNumChannels();
        for (int i = 0; i < nOut; ++i)
        {
            const double pos = i * ratio;
            const int    i0  = (int) pos;
            const float  t   = (float) (pos - i0);
            float sum = 0.0f;
            for (int c = 0; c < nch; ++c)
                sum += tb.getSample (c, i0) * (1.0f - t)
                     + tb.getSample (c, std::min (i0 + 1, nIn - 1)) * t;
            r.samples[(size_t) i] = sum / (float) nch;
        }
        return r;
    }

    void applyMyVoiceProfile (const VoiceProfile& profile, const juce::String& sourceName)
    {
        if (! profile.valid()) return;
        prof1 = profile;
        proc.lastMyVoice = profile;
        p1Lbl.setText (juce::String::fromUTF8 ("MyVoice: ") + sourceName
                       + "\n" + fmt (profile), juce::dontSendNotification);
        nSet = nLocked = 0;
        refreshEstimated();
        graph.repaint();
        updateMatchStatus();
    }

    void loadTargetFile()
    {
        targetChooser = std::make_unique<juce::FileChooser> (
            "Select the target voice file or profile", juce::File(),
            "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.m4a;*.ogg;*.vmprofile");
        targetChooser->launchAsync (juce::FileBrowserComponent::openMode
                                  | juce::FileBrowserComponent::canSelectFiles,
            [this] (const juce::FileChooser& fc)
        {
            const auto file = fc.getResult();
            if (file == juce::File())
            {
                restoreTargetSelectionUi();
                return;
            }

            if (file.hasFileExtension ("vmprofile"))
            {
                VoiceProfile p;
                if (auto xml = juce::XmlDocument::parse (file); xml != nullptr
                    && profileFromXml (*xml, p) && p.valid())
                {
                    prof2 = p;
                    proc.lastTarget = p;
                    proc.prevPos = -1;
                    proc.prevLen = 0;      // profile-only target has no audio
                    currentTargetName = file.getFileNameWithoutExtension() + " (.vmprofile)";
                    p2Lbl.setText (juce::String::fromUTF8 ("Target: ") + currentTargetName
                                   + "\n" + fmt (prof2), juce::dontSendNotification);
                    selectTargetFileButton();
                    invalidateForTargetChange();
                }
                else
                    status (juce::String::fromUTF8 ("プロファイルを読み込めませんでした: ")
                            + file.getFileName());
                return;
            }

            const double sr = proc.getSampleRate() > 0 ? proc.getSampleRate() : 48000.0;
            auto decoded = decodeMonoForAnalysis (file, sr, 60.0);
            if (! decoded.error.isEmpty())
            {
                status (decoded.error);
                return;
            }
            auto analysed = VoiceAnalyzer::analyze (decoded.samples.data(),
                                                    (int) decoded.samples.size(), sr);
            if (! analysed.valid())
            {
                // do NOT overwrite proc.prevBuf on analysis failure
                status (juce::String::fromUTF8 (
                    "Target解析に失敗しました(有声区間が不足): ") + file.getFileName());
                return;
            }

            // commit to Processor buffers only after a successful analysis
            const int copyN = std::min ((int) decoded.samples.size(),
                                        (int) proc.prevBuf.size());
            std::copy_n (decoded.samples.data(), copyN, proc.prevBuf.data());
            proc.prevLen = copyN;
            proc.prevPos = -1;

            prof2 = analysed;
            proc.lastTarget = prof2;
            currentTargetName = file.getFileName();
            p2Lbl.setText (juce::String::fromUTF8 ("Target: ") + currentTargetName
                           + "\n" + fmt (prof2), juce::dontSendNotification);
            selectTargetFileButton();
            invalidateForTargetChange();
        });
    }

    // MyVoice audio / .vmprofile load: touches ONLY prof1 / lastMyVoice.
    // proc.prevBuf, prof2, Target selection and Target preview are kept
    // intact (spec 5.6).
    void loadMyVoiceFile()
    {
        myVoiceChooser = std::make_unique<juce::FileChooser> (
            "Select the MyVoice audio file or profile", juce::File(),
            "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.m4a;*.ogg;*.vmprofile");
        myVoiceChooser->launchAsync (juce::FileBrowserComponent::openMode
                                   | juce::FileBrowserComponent::canSelectFiles,
            [this] (const juce::FileChooser& fc)
        {
            const auto file = fc.getResult();
            if (file == juce::File()) return;

            if (file.hasFileExtension ("vmprofile"))
            {
                VoiceProfile p;
                if (auto xml = juce::XmlDocument::parse (file); xml != nullptr
                    && profileFromXml (*xml, p) && p.valid())
                    applyMyVoiceProfile (p, file.getFileNameWithoutExtension()
                                              + " (.vmprofile)");
                else
                    status (juce::String::fromUTF8 (
                        "MyVoiceプロファイルを読み込めませんでした: ") + file.getFileName());
                return;
            }

            const double sr = proc.getSampleRate() > 0 ? proc.getSampleRate() : 48000.0;
            auto decoded = decodeMonoForAnalysis (file, sr, 60.0);
            if (! decoded.error.isEmpty())
            {
                status (decoded.error);
                return;
            }
            auto analysed = VoiceAnalyzer::analyze (decoded.samples.data(),
                                                    (int) decoded.samples.size(), sr);
            if (! analysed.valid())
            {
                status (juce::String::fromUTF8 (
                    "MyVoiceの解析に失敗しました(有声区間が不足): ") + file.getFileName());
                return;
            }
            applyMyVoiceProfile (analysed, file.getFileName());
        });
    }

    // ── save profile (shared by Target / MyVoice) ────────────────────
    void saveProfileImpl (const VoiceProfile& profile,
                          const juce::String& defaultName,
                          const juce::String& successPrefix)
    {
        if (! profile.valid())
        {
            status (juce::String::fromUTF8 ("有効なプロファイルがありません。"));
            return;
        }
        // snapshot the profile at click time -- prof1/prof2 could change
        // while the async chooser is up
        const VoiceProfile snapshot = profile;
        profileSaveChooser = std::make_unique<juce::FileChooser> (
            "Save profile",
            juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                .getChildFile (defaultName + ".vmprofile"),
            "*.vmprofile");
        profileSaveChooser->launchAsync (juce::FileBrowserComponent::saveMode
                                       | juce::FileBrowserComponent::canSelectFiles
                                       | juce::FileBrowserComponent::warnAboutOverwriting,
            [this, snapshot, successPrefix] (const juce::FileChooser& fc)
        {
            auto file = fc.getResult();
            if (file == juce::File()) return;
            if (! file.hasFileExtension ("vmprofile"))
                file = file.withFileExtension (".vmprofile");
            if (profileToXml (snapshot)->writeTo (file))
                status (successPrefix + juce::String::fromUTF8 ("を保存しました: ")
                        + file.getFileName());
            else
                status (juce::String::fromUTF8 ("保存に失敗しました。"));
        });
    }

    // Same behaviour as the PRESETS tab's "Reset All to Defaults": every
    // parameter back to its default in ONE undo step, locked parameters
    // untouched. Kept here (rather than shared) so this stays a plain UI
    // action -- PresetStore extraction is a Phase 2+ item.
    void resetAllParameters()
    {
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
        status (msg + juce::String::fromUTF8 ("。Undoで戻せます。"));
        // the match result no longer describes the current parameters
        nSet = nLocked = 0;
        refreshEstimated();
        graph.repaint();
        updateMatchStatus();
    }

    void saveTargetProfile()
    {
        saveProfileImpl (prof2, "Target Profile",
                         juce::String::fromUTF8 ("ターゲットプロファイル"));
    }

    void saveMyVoiceProfile()
    {
        saveProfileImpl (prof1, "MyVoice Profile",
                         juce::String::fromUTF8 ("MyVoiceプロファイル"));
    }

    // ── Match (calls MatchingEngine, applies through history.group) ──
    void doMatch()
    {
        if (! prof1.valid() || ! prof2.valid())
        {
            status (juce::String::fromUTF8 ("CurrentとTargetの両方が必要です。"));
            return;
        }
        const auto proposal = MatchingEngine::autoSet (prof1, prof2);
        applyProposal (proposal);
        refreshEstimated();
        graph.repaint();
    }

    void applyProposal (const MatchingEngine::Proposal& r)
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
        juce::String line = juce::String::formatted (
            "pitch %+.1f st   formant %+.1f st   tilt %+.1f dB",
            r.pitch, r.formant, r.tilt);
        if (r.rangeApplied)
            line += juce::String::formatted ("   range %.0f%%  center %.0f Hz",
                                             r.range, r.center);
        line += juce::String::formatted ("\nhigh-range start %.0f Hz   pitch floor %.0f Hz",
                                         r.hifreq, r.pitchfloor);
        if (r.airApplied)
            line += juce::String::formatted ("   air %.2f   shine %.1f dB",
                                             r.air, r.airshine);
        // ---- v0.29.0 measurement report ------------------------------
        // Say what was actually compared, so the number above can be
        // trusted or distrusted on evidence rather than on faith.
        static const char* vw[5] = { "A", "I", "U", "E", "O" };
        if (r.vowelsMatched > 0)
        {
            juce::String vs;
            for (int v = 0; v < 5; ++v)
                if (r.vowelUsed[v])
                {
                    if (vs.isNotEmpty()) vs << "/";
                    vs << vw[v];
                }
            line += juce::String::fromUTF8 ("\n母音一致: ") + vs
                  + juce::String::formatted (" (%d)   ", r.vowelsMatched)
                  + juce::String::fromUTF8 ("ばらつき ")
                  + juce::String::formatted ("%.1f st", r.agreementSt)
                  + juce::String::fromUTF8 (
                        "\nAEIOU Character を Custom に設定し、母音別の差を書き込みました");
        }
        else if (r.fellBack)
        {
            line += juce::String::fromUTF8 (
                        "\n母音別データが無いため全体平均で一致させました");
        }
        // A formant at or below the fundamental leaves no trace in the
        // spectrum, so it cannot be matched at all. Tell the user which
        // band that happened to and why, instead of silently moving it.
        {
            juce::String un;
            for (int i = 0; i < 3; ++i)
                if (r.bandRel[i] < MatchingEngine::kMinRel)
                {
                    if (un.isNotEmpty()) un << "/";
                    un << "F" << (i + 1);
                }
            if (un.isNotEmpty())
                line << juce::String::fromUTF8 ("\n") << un
                     << juce::String::fromUTF8 (
                            " は基音に近すぎて測定できないため個別補正せず、"
                            "全体のFormantに任せています");
        }
        if (r.lowConfidence)
            line += juce::String::fromUTF8 (
                        "\n\xe2\x9a\xa0 一致の根拠が不足しています(録音内容が違う可能性)。"
                        "\nターゲットを再生しながら同じ内容を録音し直すと精度が上がります"
                        "(With target play)。");
        status (juce::String::fromUTF8 ("MATCH 完了 — ") + line + "\n" + setSummary());
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
        if (auto xml = proc.apvts.copyState().createXml();
            xml != nullptr && xml->writeTo (file) && file.existsAsFile())
        {
            saveNameEdit.clear();
            showSavePreset (false);
            status (juce::String::fromUTF8 ("プリセット保存: ") + name
                    + juce::String::fromUTF8 (" (PRESETSタブで確認)"));
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
        const bool canMatch = prof1.valid() && prof2.valid();
        matchBtn.setEnabled (canMatch);
        saveTargetProfBtn.setEnabled  (prof2.valid());
        saveMyVoiceProfBtn.setEnabled (prof1.valid());
        // Only surface an APPLIED/LOCKED count from the LAST match; if
        // there hasn't been one (or a Target / MyVoice change wiped the
        // counters), stay silent -- the enabled/disabled MATCH button is
        // enough of a "not ready yet" signal.
        juce::String s;
        if (nSet + nLocked > 0)
            s = juce::String (nSet) + juce::String::fromUTF8 (" APPLIED")
              + (nLocked > 0 ? juce::String (" · ") + juce::String (nLocked)
                             + juce::String::fromUTF8 (" LOCKED") : juce::String());
        else if (canMatch)
            s = juce::String::fromUTF8 ("READY");
        matchStatus.setText (s, juce::dontSendNotification);
    }

    // ── members ──────────────────────────────────────────────────────────
    VoxMorphProcessor& proc;
    VoiceProfile prof1, prof2, profE;   // Current / Target / Estimated
    int nSet = 0, nLocked = 0;
    ProfileGraph graph;
    juce::String currentTargetName, savedButtonText;

    bool waitingCapture = false, playStartedByCapture = false;
    bool savePresetVisible = false;

    juce::Label hTargetCharacter, hMyVoice, hAutoMatching;
    juce::Label p1Lbl, p2Lbl, outLbl, matchStatus, saveHint;

    static constexpr int kTargetRadioGroup = 0x564d01;
    std::vector<std::unique_ptr<TargetCharacterButton>> targetButtons;
    TargetCharacterButton* targetFileButton = nullptr;
    int  selectedSampleIndex = -1;   // -1 = no built-in target selected
    bool targetFileActive    = false;

    juce::ComboBox durBox;
    juce::ToggleButton recPlayChk { "With target play" };

    VoiceTileButton recBtn         { "record_my_voice", "Record",       TileIconKind::record, /*clickingToggles*/ false };
    VoiceTileButton myVoiceFileBtn { "my_voice_file",   "MyVoiceFile",  TileIconKind::file,   /*clickingToggles*/ false };
    juce::TextButton playBtn { "Play" },
                     saveTargetProfBtn  { "Save Profile..." },
                     saveMyVoiceProfBtn { "Save Profile..." },
                     resetAllBtn { "Reset All to Defaults" },
                     matchBtn { "MATCH" }, savePresetBtn { "SAVE PRESET" },
                     savePresetOkBtn { "Save" }, savePresetCancelBtn { "Cancel" };
    juce::TextEditor saveNameEdit;

    // one chooser per role -- they never overlap in the same session but
    // keeping them separate makes it obvious which async callback belongs
    // to which action (target load / MyVoice load / profile save)
    std::unique_ptr<juce::FileChooser> targetChooser;
    std::unique_ptr<juce::FileChooser> myVoiceChooser;
    std::unique_ptr<juce::FileChooser> profileSaveChooser;
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
        heading.setColour (juce::Label::textColourId, ak::heading);
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
        pvLbl.setColour (juce::Label::textColourId, juce::Colour (0xff8f9ab5));
        pvLbl.setText (juce::String::fromUTF8 (
            "プレビュー: 標準的な声(ミント)がこのプリセットでどう変わるか(ピンク)のイメージ。"),
            juce::dontSendNotification);
        addAndMakeVisible (pvLbl);

        hProfiles.setText ("PROFILES", juce::dontSendNotification);
        hProfiles.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
        hProfiles.setColour (juce::Label::textColourId, ak::heading);
        addAndMakeVisible (hProfiles);

        pNameEdit.setTextToShowWhenEmpty (juce::String::fromUTF8 ("プロファイル名"),
                                          juce::Colour (0xff8f9ab5));
        pNameEdit.setFont (juce::Font (juce::FontOptions (13.0f)));
        pNameEdit.setColour (juce::TextEditor::textColourId, ak::ink);
        pNameEdit.setColour (juce::TextEditor::backgroundColourId, juce::Colours::white);
        pNameEdit.setColour (juce::TextEditor::outlineColourId, ak::line);
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
                                         juce::Colour (0xff8f9ab5));
        nameEdit.setFont (juce::Font (juce::FontOptions (13.0f)));
        nameEdit.setColour (juce::TextEditor::textColourId, ak::ink);
        nameEdit.setColour (juce::TextEditor::backgroundColourId, juce::Colours::white);
        nameEdit.setColour (juce::TextEditor::outlineColourId, ak::line);
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
        pathLbl.setColour (juce::Label::textColourId, juce::Colour (0xff8f9ab5));
        pathLbl.setText (juce::String::fromUTF8 ("保存先: ") + presetDir().getFullPathName(),
                         juce::dontSendNotification);
        addAndMakeVisible (pathLbl);

        refreshList();
    }

    // Re-scan the presets folder whenever the tab becomes visible again --
    // MatchingPanel writes the same .vmpreset format into the same folder,
    // and without this the PRESETS list would only pick up new files at
    // panel construction / an explicit local Save. Cheap directory listing;
    // safe on the message thread (JUCE flips visibility during tab
    // switches).
    void visibilityChanged() override
    {
        if (isVisible())
            refreshList();
    }

    // v0.31.2: sits on the ANOKOE page gradient, so it draws its own card
    void paint (juce::Graphics& g) override
    {
        ak::paintCard (g, getLocalBounds().toFloat());
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
    static juce::File presetDir() { return voxMorphPresetDir(); }

    void refreshList (const juce::String& select = {})
    {
        files = voxMorphPresetFiles();
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
        // Apply per parameter instead of replaceState: locked parameters keep
        // their current values, and the whole load is ONE undo step.
        int applied = 0, lockedKept = 0;
        if (! voxMorphApplyPreset (proc, files.getReference (idx), applied, lockedKept))
        {
            setStatus (juce::String::fromUTF8 ("読み込みに失敗しました(壊れたファイル?)"));
            return;
        }
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

// ---------------------------------------------------------------------------
// MAIN tab preset bar (v0.30.0): [preset ▼] [Save] [Delete].
//
// Unlike the PRESETS tab (select, then press Load), picking a preset here
// APPLIES it immediately — that is the requested behaviour for the main
// screen. It is still one undo step and locked parameters are still honoured,
// so a mis-click is always recoverable with Cmd+Z.
class PresetBar : public juce::Component
{
public:
    // onStatus receives short messages to show in the MAIN tab footer
    PresetBar (VoxMorphProcessor& p, std::function<void (const juce::String&)> onStatus)
        : proc (p), status (std::move (onStatus))
    {
        box.setTextWhenNothingSelected (juce::String::fromUTF8 ("-- プリセットを選択 / Preset --"));
        box.setTooltip (vmTip (
            "Choose a preset - it is applied to every parameter immediately. Locked "
            "parameters keep their value, and the whole load counts as ONE undo step "
            "(Cmd+Z / Ctrl+Z). The same presets appear in the PRESETS tab.",
            "プリセットを選ぶと、その場で全パラメータに反映されます。ロック中の項目は"
            "そのまま保持され、読み込み全体がUndo 1回分(Cmd+Z / Ctrl+Z)で元に戻せます。"
            "PRESETSタブと同じプリセットです。"));
        // re-scan just before the list drops down, so presets saved from the
        // PRESETS / MATCHING tabs show up without restarting
        box.beforePopup = [this] { refreshList (currentName()); };
        box.onChange = [this] { applySelected(); };
        addAndMakeVisible (box);

        saveBtn.setTooltip (vmTip (
            "Save the current settings as a preset. You are asked whether to overwrite "
            "the selected preset or to save under a new name.",
            "現在の設定をプリセットとして保存します。押すと「選択中のプリセットに上書き」か"
            "「別名で保存」かを選べます。"));
        saveBtn.onClick = [this] { saveMenu(); };
        addAndMakeVisible (saveBtn);

        deleteBtn.setTooltip (vmTip (
            "Delete the selected preset file. A confirmation is asked first.",
            "選択中のプリセットファイルを削除します(削除前に確認が出ます)。"));
        deleteBtn.onClick = [this] { deleteSelected(); };
        addAndMakeVisible (deleteBtn);

        refreshList();
    }

    ~PresetBar() override
    {
        if (nameWin != nullptr)          // dismiss a still-open Save-as dialog
        {
            nameWin->setLookAndFeel (nullptr);
            nameWin->exitModalState (0);
        }
    }

    void visibilityChanged() override
    {
        if (isVisible()) refreshList (currentName());
    }

    // v0.33.0: dark styling for the hero band, and symbol buttons sized like
    // the body's own dropdowns instead of wide "Save" / "Delete" text.
    void setDarkStyle()
    {
        box.setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff2f3550));
        box.setColour (juce::ComboBox::textColourId,       ak::bandInk);
        box.setColour (juce::ComboBox::outlineColourId,    juce::Colour (0x40ffffff));
        box.setColour (juce::ComboBox::arrowColourId,      ak::bandInk);
        box.setTextWhenNothingSelected (juce::String::fromUTF8 ("Preset"));
        saveBtn  .setFramed (false);
        deleteBtn.setFramed (false);
        saveBtn  .setTint (ak::bandInk);
        deleteBtn.setTint (ak::bandInk);
        dark = true;
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        if (! dark) return;
        auto r = getLocalBounds().toFloat();
        auto btns = r.removeFromRight (66.0f).reduced (0.0f, 3.0f);
        g.setColour (juce::Colour (0x26ffffff));
        g.fillRoundedRectangle (btns, 8.0f);
        g.setColour (juce::Colour (0x33ffffff));
        g.drawRoundedRectangle (btns, 8.0f, 1.0f);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (0, dark ? 0 : 3);
        deleteBtn.setBounds (r.removeFromRight (32).reduced (2, 4));
        saveBtn  .setBounds (r.removeFromRight (32).reduced (2, 4));
        r.removeFromRight (dark ? 6 : 4);
        box.setBounds (r.reduced (0, dark ? 3 : 0));
    }

private:
    // ComboBox that lets us re-scan the folder before the menu appears
    struct RescanComboBox : public juce::ComboBox
    {
        std::function<void()> beforePopup;
        void showPopup() override
        {
            if (beforePopup) beforePopup();
            juce::ComboBox::showPopup();
        }
    };

    juce::String currentName() const
    {
        const int idx = box.getSelectedId() - 1;
        return juce::isPositiveAndBelow (idx, files.size())
                 ? files.getReference (idx).getFileNameWithoutExtension() : juce::String();
    }

    void refreshList (const juce::String& select = {})
    {
        const juce::ScopedValueSetter<bool> guard (updating, true);
        files = voxMorphPresetFiles();
        box.clear (juce::dontSendNotification);
        int id = 1, selId = 0;
        for (auto& f : files)
        {
            box.addItem (f.getFileNameWithoutExtension(), id);
            if (f.getFileNameWithoutExtension() == select) selId = id;
            ++id;
        }
        box.setSelectedId (selId, juce::dontSendNotification);
    }

    void applySelected()
    {
        if (updating) return;                     // list rebuild, not a user pick
        const int idx = box.getSelectedId() - 1;
        if (! juce::isPositiveAndBelow (idx, files.size())) return;

        int applied = 0, lockedKept = 0;
        if (! voxMorphApplyPreset (proc, files.getReference (idx), applied, lockedKept))
        {
            report (juce::String::fromUTF8 ("読み込みに失敗しました(壊れたファイル?)"));
            return;
        }
        auto msg = juce::String::fromUTF8 ("\xE2\x9C\x93 ") + box.getText()
                 + juce::String::fromUTF8 (" を適用 (") + juce::String (applied)
                 + juce::String::fromUTF8 ("項目");
        if (lockedKept > 0)
            msg += juce::String::fromUTF8 ("、") + juce::String (lockedKept)
                 + juce::String::fromUTF8 ("項目はロック保持");
        report (msg + juce::String::fromUTF8 (") — Undoで戻せます"));
    }

    void saveMenu()
    {
        const auto sel = currentName();
        juce::PopupMenu m;
        m.addItem (1, juce::String::fromUTF8 ("上書き保存 / Overwrite: ")
                      + (sel.isNotEmpty() ? sel : juce::String::fromUTF8 ("(未選択)")),
                   sel.isNotEmpty());
        m.addItem (2, juce::String::fromUTF8 ("別名で保存 / Save as new..."));
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&saveBtn),
            [this, sel] (int r)
            {
                if (r == 1)      writePreset (sel);
                else if (r == 2) askNewName();
            });
    }

    void askNewName()
    {
        nameWin = std::make_unique<juce::AlertWindow> (
            juce::String::fromUTF8 ("プリセットを保存 / Save preset"),
            juce::String::fromUTF8 ("新しいプリセット名を入力してください。\n"
                                    "Enter a name for the new preset."),
            juce::MessageBoxIconType::NoIcon, this);
        nameWin->setLookAndFeel (&alertLnf);
        nameWin->addTextEditor ("name", currentName(), juce::String::fromUTF8 ("名前 / Name"));
        nameWin->addButton (juce::String::fromUTF8 ("保存 / Save"), 1,
                            juce::KeyPress (juce::KeyPress::returnKey));
        nameWin->addButton (juce::String::fromUTF8 ("キャンセル / Cancel"), 0,
                            juce::KeyPress (juce::KeyPress::escapeKey));
        nameWin->enterModalState (true, juce::ModalCallbackFunction::create (
            [this] (int r)
            {
                const auto name = r == 1 && nameWin != nullptr
                                    ? nameWin->getTextEditorContents ("name").trim()
                                    : juce::String();
                if (nameWin != nullptr)
                {
                    nameWin->setLookAndFeel (nullptr);
                    nameWin->exitModalState (0);
                    nameWin->setVisible (false);
                }
                if (name.isNotEmpty()) writePreset (name);
                else if (r == 1)
                    report (juce::String::fromUTF8 ("プリセット名を入力してください。"));
            }), false);
    }

    void writePreset (const juce::String& name)
    {
        if (name.isEmpty()) return;
        const auto file = voxMorphPresetDir()
                            .getChildFile (juce::File::createLegalFileName (name) + ".vmpreset");
        auto xml = proc.apvts.copyState().createXml();
        if (xml != nullptr && xml->writeTo (file))
        {
            refreshList (file.getFileNameWithoutExtension());
            report (juce::String::fromUTF8 ("\xE2\x9C\x93 保存しました: ") + name);
        }
        else
            report (juce::String::fromUTF8 ("保存に失敗しました。"));
    }

    void deleteSelected()
    {
        const int idx = box.getSelectedId() - 1;
        if (! juce::isPositiveAndBelow (idx, files.size()))
        {
            report (juce::String::fromUTF8 ("削除するプリセットを選択してください。"));
            return;
        }
        const auto file = files.getReference (idx);
        juce::NativeMessageBox::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon,
            "Delete preset",
            juce::String::fromUTF8 ("このプリセットを削除しますか?\nDelete this preset?\n\n")
              + file.getFileNameWithoutExtension(),
            this, juce::ModalCallbackFunction::create ([this, file] (int result)
            {
                if (result != 1) return;                       // 0 = cancel
                const auto name = file.getFileNameWithoutExtension();
                if (file.deleteFile())
                    report (juce::String::fromUTF8 ("削除しました: ") + name);
                else
                    report (juce::String::fromUTF8 ("削除に失敗しました: ") + name);
                refreshList();
            }));
    }

    void report (const juce::String& s) { if (status) status (s); }

    VoxMorphProcessor& proc;
    std::function<void (const juce::String&)> status;
    juce::Array<juce::File> files;
    RescanComboBox   box;
    ak::IconButton saveBtn   { "save",   "ui_mark_S_Save_png",   17 };
    ak::IconButton deleteBtn { "delete", "ui_mark_S_Delete_png", 17 };
    std::unique_ptr<juce::AlertWindow> nameWin;
    juce::LookAndFeel_V4 alertLnf { juce::LookAndFeel_V4::getLightColourScheme() };
    bool updating = false, dark = false;
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
    void timerCallback() override
    {
        if (const int hz = vmDrawHz (proc, 30); hz != rateHz)
        { rateHz = hz; startTimerHz (hz); }   // Performance Mode
        if (isShowing()) repaint();
    }

    juce::Rectangle<float> circleBounds() const
    {
        auto b = getLocalBounds().toFloat().reduced (14.0f);
        const float s = std::min (b.getWidth(), b.getHeight());
        return b.withSizeKeepingCentre (s, s);
    }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat().reduced (2.0f);
        ak::paintCard (g, b);

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

        g.setColour (juce::Colour (0xff8f9ab5));
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
    int rateHz = 30;   // current timer rate; Performance Mode lowers it
    juce::RangedAudioParameter *px = nullptr, *py = nullptr;
};

class AsmrPanel : public juce::Component
{
public:
    explicit AsmrPanel (VoxMorphProcessor& p) : proc (p), pad (p)
    {
        heading.setText ("ASMR POSITION", juce::dontSendNotification);
        heading.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
        heading.setColour (juce::Label::textColourId, ak::heading);
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

    // v0.31.2: sits on the ANOKOE page gradient, so it draws its own card
    void paint (juce::Graphics& g) override
    {
        ak::paintCard (g, getLocalBounds().toFloat());
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
        lnf.setColour (juce::Label::textColourId,        ak::ink);
        lnf.setColour (juce::ToggleButton::textColourId, ak::ink);
        lnf.setColour (juce::ToggleButton::tickColourId, juce::Colour (0xff7999db));
        lnf.setColour (juce::TextButton::buttonColourId, juce::Colours::white);
        lnf.setColour (juce::TextButton::textColourOffId, juce::Colour (0xff7999db));
        setLookAndFeel (&lnf);

        auto initHead = [this] (juce::Label& l, const char* en, const char* jp)
        {
            l.setText (juce::String (en) + "   " + juce::String::fromUTF8 (jp),
                       juce::dontSendNotification);
            l.setFont (juce::Font (juce::FontOptions (13.5f, juce::Font::bold)));
            l.setColour (juce::Label::textColourId, ak::heading);
            addAndMakeVisible (l);
        };
        initHead (preHead,  "Pre FX",  "(変換前・マイク入力に掛かる)");
        initHead (postHead, "Post FX", "(変換後・出力に掛かる)");
        preAdd.onClick  = [this] { add (false); };
        postAdd.onClick = [this] { add (true);  };
        addAndMakeVisible (preAdd);
        addAndMakeVisible (postAdd);
        note.setFont (juce::Font (juce::FontOptions (11.0f)));
        note.setColour (juce::Label::textColourId, juce::Colour (0xff8f9ab5));
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

    void paint (juce::Graphics& g) override { g.fillAll (juce::Colour (0xfffafbff)); }

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
                                juce::Colour (0xfffafbff), juce::DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new FxChainPanel (p), true);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }
    void closeButtonPressed() override { setVisible (false); }
};

// ---------------------------------------------------------------------------
// Audio Settings window (v0.30.0, STANDALONE ONLY).
//
// Replaces JUCE's plain showAudioSettingsDialog(): it still hosts the same
// AudioDeviceSelectorComponent, but adds the two settings that belong with
// the audio routing — the MONITOR output device used by the options bar's
// MONITOR button, and Auto-Mute on Feedback (moved here from ADVANCED).
class AudioSettingsPanel : public juce::Component
{
public:
    explicit AudioSettingsPanel (VoxMorphProcessor& p) : proc (p)
    {
        // this window is outside the editor's LookAndFeel scope, so give it
        // the same pastel-mint light theme (see AEIOUCharacterPanel)
        lnf.setColour (juce::Slider::trackColourId,             juce::Colour (0xff7999db));
        lnf.setColour (juce::Slider::backgroundColourId,        juce::Colour (0xffe9e9e9));
        lnf.setColour (juce::Slider::thumbColourId,             juce::Colour (0xff7999db));
        lnf.setColour (juce::Slider::textBoxTextColourId,       ak::ink);
        lnf.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::white);
        lnf.setColour (juce::Slider::textBoxOutlineColourId,    ak::line);
        lnf.setColour (juce::Label::textColourId,               ak::ink);
        lnf.setColour (juce::ToggleButton::textColourId,        ak::ink);
        lnf.setColour (juce::ToggleButton::tickColourId,        juce::Colour (0xff7999db));
        lnf.setColour (juce::TextButton::buttonColourId,        juce::Colours::white);
        lnf.setColour (juce::TextButton::textColourOffId,       juce::Colour (0xff7999db));
        lnf.setColour (juce::ComboBox::textColourId,            ak::ink);
        setLookAndFeel (&lnf);

        auto initHead = [this] (juce::Label& l, const char* text)
        {
            l.setText (juce::String::fromUTF8 (text), juce::dontSendNotification);
            l.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
            l.setColour (juce::Label::textColourId, ak::heading);
            addAndMakeVisible (l);
        };
        initHead (hDevice,  "AUDIO DEVICE");
        initHead (hMonitor, "MONITOR");
        initHead (hPerf,    "PERFORMANCE");
        initHead (hSafety,  "SAFETY");

        // ---- Performance Mode buffer helper (v0.31.0) ----------------------
        // Assistance, not automation: Performance Mode never rewrites the
        // device setup on its own, because the smallest buffer a machine can
        // sustain depends on the interface and on whatever else is running.
        // The user presses the button, and can always press Restore (or use
        // the device panel above) to go back.
        perfRow = std::make_unique<ParamRow> (proc, "perfmode", ParamRow::Kind::toggle,
            "Performance Mode",
            vmTip ("Reduces the work VoxMorph does OUTSIDE the audio conversion (display and "
                   "analysis refresh) so a small device buffer is easier to sustain. The "
                   "converted sound is identical with it on or off.",
                   "変換そのもの以外の処理(表示・解析の更新)を軽くして、小さいバッファでも"
                   "音が途切れにくくします。変換後の音はオン/オフで完全に同じです。"));
        addAndMakeVisible (*perfRow);

        bufLbl.setFont (juce::Font (juce::FontOptions (13.0f)));
        addAndMakeVisible (bufLbl);

        bufBtn.setTooltip (vmTip (
            "Tries to set the audio buffer to 64 samples, then 128, then 256 - whichever the "
            "current device offers first. Lower buffer = lower in/out delay, but more risk of "
            "dropouts, so listen for crackles afterwards and press Restore (or pick a size in "
            "the device panel above) if you hear any. This changes the DEVICE buffer only: "
            "the conversion engine's own lookahead and its analysis quality are untouched.",
            "オーディオバッファを64サンプル、無理なら128、それも無理なら256サンプルに"
            "設定します(現在のデバイスが対応する最初のサイズ)。バッファが小さいほど"
            "入出力の遅延は減りますが音切れのリスクは上がるので、設定後にプチプチ音が"
            "出ないか確認し、出るようならRestore(または上のデバイス欄で手動選択)で"
            "戻してください。変更するのはデバイスのバッファだけで、変換エンジンの"
            "先読みや解析品質には一切触れません。"));
        bufBtn.onClick = [this] { optimizeBuffer(); };
        addAndMakeVisible (bufBtn);

        bufBackBtn.setTooltip (vmTip (
            "Puts the audio buffer back to the size it had before the last Optimize.",
            "直前のOptimizeを押す前のバッファサイズに戻します。"));
        bufBackBtn.setEnabled (false);
        bufBackBtn.onClick = [this]
        {
            if (bufBefore > 0 && vmApplyBufferSize (bufBefore) > 0)
                bufBackBtn.setEnabled (false);
            refreshBufferLabel();
        };
        addAndMakeVisible (bufBackBtn);

        bufNote.setFont (juce::Font (juce::FontOptions (11.5f)));
        bufNote.setColour (juce::Label::textColourId, juce::Colour (0xff8f9ab5));
        bufNote.setJustificationType (juce::Justification::topLeft);
        addAndMakeVisible (bufNote);

        if (auto* dm = vmDeviceManager())
        {
            sel = std::make_unique<juce::AudioDeviceSelectorComponent> (
                      *dm, 0, 2, 0, 2,
                      /*showMidiIn*/ false, /*showMidiOut*/ false,
                      /*channelsAsStereoPairs*/ true, /*hideAdvanced*/ false);
            addAndMakeVisible (*sel);
        }

        monLbl.setText (juce::String::fromUTF8 ("モニター出力デバイス / Monitor output"),
                        juce::dontSendNotification);
        monLbl.setFont (juce::Font (juce::FontOptions (13.0f)));
        addAndMakeVisible (monLbl);

        monBox.setTooltip (vmTip (
            "The output device the MONITOR button switches to. Typically your headphones, "
            "while the normal output goes to a virtual cable feeding OBS / Discord. "
            "Leave it unset and the MONITOR button stays inactive.",
            "MONITORボタンを押したときに一時的に切り替わる出力先です。通常はヘッドホンを"
            "指定し、普段の出力はOBSやDiscordへ送る仮想オーディオデバイスにしておきます。"
            "未設定のままだとMONITORボタンは動作しません。"));
        monBox.onChange = [this]
        {
            if (updating) return;
            proc.monitorDeviceName = monBox.getSelectedId() <= 1 ? juce::String()
                                                                 : monBox.getText();
        };
        addAndMakeVisible (monBox);

        rescanBtn.setTooltip (vmTip ("Re-scan the audio devices.",
                                     "オーディオデバイスを再検索します。"));
        rescanBtn.onClick = [this] { refreshMonitorList(); };
        addAndMakeVisible (rescanBtn);

        monNote.setText (juce::String::fromUTF8 (
            "MONITORをオンにすると出力先が上のデバイスへ一時的に切り替わり、MUTEも自動でオンに"
            "なります。MONITORをオフに戻しても出力が消えたままなのは意図的な安全動作です"
            "(MUTEを手で解除すると配信に戻り、同時にMONITORもオフになります)。"),
            juce::dontSendNotification);
        monNote.setFont (juce::Font (juce::FontOptions (11.5f)));
        monNote.setColour (juce::Label::textColourId, juce::Colour (0xff8f9ab5));
        monNote.setJustificationType (juce::Justification::topLeft);
        addAndMakeVisible (monNote);

        autoMuteRow = std::make_unique<ParamRow> (proc, "automute", ParamRow::Kind::toggle,
            "Auto-Mute on Feedback",
            vmTip ("Standalone app: if the output stays extremely loud for over a second "
                   "(a runaway feedback loop between speakers and mic), the output is muted "
                   "for 3 seconds automatically. Has no effect in a DAW.",
                   "スタンドアロン用。スピーカー→マイクのハウリングが暴走して出力が1秒以上"
                   "大音量で鳴り続けた場合、自動で3秒間ミュートして回路を切ります。"
                   "DAWプラグインとして使用中は動作しません。"));
        addAndMakeVisible (*autoMuteRow);

        closeBtn.onClick = [this]
        {
            if (auto* dw = findParentComponentOfClass<juce::DocumentWindow>())
                dw->setVisible (false);
        };
        addAndMakeVisible (closeBtn);

        setSize (600, 780);
        sendLookAndFeelChange();
        refreshMonitorList();
        refreshBufferLabel();
    }

    ~AudioSettingsPanel() override { setLookAndFeel (nullptr); }

    void visibilityChanged() override
    {
        if (isVisible()) { refreshMonitorList(); refreshBufferLabel(); }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (14, 10);

        closeBtn.setBounds (r.removeFromBottom (30).removeFromRight (100).reduced (0, 2));
        r.removeFromBottom (6);
        autoMuteRow->setBounds (r.removeFromBottom (28));
        hSafety.setBounds (r.removeFromBottom (24));
        r.removeFromBottom (10);

        bufNote.setBounds (r.removeFromBottom (46));
        {
            auto br = r.removeFromBottom (30);
            bufBackBtn.setBounds (br.removeFromRight (90).reduced (2, 2));
            br.removeFromRight (4);
            bufBtn    .setBounds (br.removeFromRight (150).reduced (2, 2));
            bufLbl    .setBounds (br);
        }
        r.removeFromBottom (4);
        perfRow->setBounds (r.removeFromBottom (28));
        hPerf.setBounds (r.removeFromBottom (24));
        r.removeFromBottom (6);
        monNote.setBounds (r.removeFromBottom (46));
        auto mr = r.removeFromBottom (30);
        monLbl.setBounds (mr.removeFromLeft (230));
        rescanBtn.setBounds (mr.removeFromRight (86).reduced (2, 2));
        monBox.setBounds (mr.reduced (2, 2));
        hMonitor.setBounds (r.removeFromBottom (24));
        r.removeFromBottom (8);

        hDevice.setBounds (r.removeFromTop (24));
        if (sel != nullptr) sel->setBounds (r);
    }

    void paint (juce::Graphics& g) override { g.fillAll (juce::Colour (0xfffafbff)); }

private:
    // Try 64 -> 128 -> 256 and keep the first size that the device accepts.
    // Nothing is changed unless a size actually applies; on failure
    // vmApplyBufferSize() has already restored the previous setup.
    void optimizeBuffer()
    {
        const int before = vmCurrentBufferSize();
        int applied = 0;
        for (int want : { 64, 128, 256 })
            if ((applied = vmApplyBufferSize (want)) > 0)
                break;

        if (applied > 0 && applied != before)
        {
            bufBefore = before;
            bufBackBtn.setEnabled (before > 0);
        }
        else if (applied == 0)
        {
            bufNote.setText (juce::String::fromUTF8 (
                "64 / 128 / 256サンプルはこのデバイスでは選べませんでした。設定は変更して"
                "いません。上のデバイス欄で使えるサイズを確認してください。\n"
                "None of 64 / 128 / 256 could be applied - nothing was changed."),
                juce::dontSendNotification);
            refreshBufferLabel (false);
            return;
        }
        refreshBufferLabel();
    }

    void refreshBufferLabel (bool resetNote = true)
    {
        const int b = vmCurrentBufferSize();
        const double sr = proc.getSampleRate() > 0 ? proc.getSampleRate() : 48000.0;
        // what the user cares about: in + out buffer, i.e. the round trip
        const double ms = b > 0 ? 2000.0 * b / sr : 0.0;
        bufLbl.setText (b > 0
            ? juce::String::fromUTF8 ("バッファ / Buffer: ") + juce::String (b)
                + " samples  (" + juce::String (ms, 1) + " ms in+out)"
            : juce::String::fromUTF8 ("バッファ / Buffer: -- (デバイス未起動)"),
            juce::dontSendNotification);

        if (resetNote)
            bufNote.setText (juce::String::fromUTF8 (
                "小さいほど遅延は減りますが、音切れが出たらRestoreか上のデバイス欄で戻して"
                "ください。変換エンジンの先読み・音質はこの設定では変わりません。\n"
                "Smaller = less delay but more dropout risk. The engine's lookahead and "
                "audio quality do not change with this."),
                juce::dontSendNotification);
    }

    void refreshMonitorList()
    {
        const juce::ScopedValueSetter<bool> guard (updating, true);
        const auto names = vmOutputDeviceNames();
        monBox.clear (juce::dontSendNotification);
        monBox.addItem (juce::String::fromUTF8 ("-- 未設定 / not set --"), 1);
        int id = 2, selId = 1;
        for (const auto& n : names)
        {
            monBox.addItem (n, id);
            if (n == proc.monitorDeviceName) selId = id;
            ++id;
        }
        // a device saved earlier but not currently connected must not be lost
        if (selId == 1 && proc.monitorDeviceName.isNotEmpty())
        {
            monBox.addItem (proc.monitorDeviceName
                              + juce::String::fromUTF8 ("  (未接続 / offline)"), id);
            selId = id;
        }
        monBox.setSelectedId (selId, juce::dontSendNotification);
    }

    VoxMorphProcessor& proc;
    juce::LookAndFeel_V4 lnf { juce::LookAndFeel_V4::getLightColourScheme() };
    juce::TooltipWindow  tips { this, 400 };
    juce::Label hDevice, hMonitor, hPerf, hSafety, monLbl, monNote, bufLbl, bufNote;
    std::unique_ptr<juce::AudioDeviceSelectorComponent> sel;
    juce::ComboBox   monBox;
    juce::TextButton rescanBtn { "Rescan" }, closeBtn { "Close" };
    juce::TextButton bufBtn { juce::String::fromUTF8 ("Optimize buffer") },
                     bufBackBtn { "Restore" };
    std::unique_ptr<ParamRow> autoMuteRow, perfRow;
    int  bufBefore = 0;      // buffer size before the last Optimize (0 = none)
    bool updating = false;
};

class AudioSettingsWindow : public juce::DocumentWindow
{
public:
    explicit AudioSettingsWindow (VoxMorphProcessor& p)
        : juce::DocumentWindow (juce::String::fromUTF8 ("Audio Settings"),
                                juce::Colour (0xfffafbff), juce::DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new AudioSettingsPanel (p), true);
        setResizable (true, false);
        setResizeLimits (480, 480, 1000, 1100);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }
    void closeButtonPressed() override { setVisible (false); }
};

// ---------------------------------------------------------------------------
// BETA window (v0.30.0): the experimental controls, kept out of the MAIN tab
// so the main screen only shows features that are considered finished.
// Currently GCI Grain Sync and Breath.
class BetaPanel : public juce::Component
{
public:
    explicit BetaPanel (VoxMorphProcessor& p) : proc (p)
    {
        lnf.setColour (juce::Slider::trackColourId,             juce::Colour (0xff7999db));
        lnf.setColour (juce::Slider::backgroundColourId,        juce::Colour (0xffe9e9e9));
        lnf.setColour (juce::Slider::thumbColourId,             juce::Colour (0xff7999db));
        lnf.setColour (juce::Slider::textBoxTextColourId,       ak::ink);
        lnf.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::white);
        lnf.setColour (juce::Slider::textBoxOutlineColourId,    ak::line);
        lnf.setColour (juce::Label::textColourId,               ak::ink);
        lnf.setColour (juce::ToggleButton::textColourId,        ak::ink);
        lnf.setColour (juce::ToggleButton::tickColourId,        juce::Colour (0xff7999db));
        lnf.setColour (juce::TextButton::buttonColourId,        juce::Colours::white);
        lnf.setColour (juce::TextButton::textColourOffId,       juce::Colour (0xff7999db));
        setLookAndFeel (&lnf);

        heading.setText ("BETA", juce::dontSendNotification);
        heading.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
        heading.setColour (juce::Label::textColourId, ak::heading);
        addAndMakeVisible (heading);

        note.setText (juce::String::fromUTF8 (
            "Experimental features. They are still being tuned and may sound worse on some "
            "voices - each one is off / 0 by default, and that default is the classic "
            "behaviour.\n"
            "実験中の機能です。声によっては品質が落ちる場合があります。既定値(オフ/0)は"
            "従来どおりの動作なので、合わなければそのままにしてください。"),
            juce::dontSendNotification);
        note.setFont (juce::Font (juce::FontOptions (11.5f)));
        note.setColour (juce::Label::textColourId, juce::Colour (0xff8f9ab5));
        note.setJustificationType (juce::Justification::topLeft);
        addAndMakeVisible (note);

        gciRow = std::make_unique<ParamRow> (proc, "gci", ParamRow::Kind::toggle,
            "GCI Grain Sync",
            vmTip ("EXPERIMENTAL. Aligns the internal grain cutting to the glottal closure instants "
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
        addAndMakeVisible (*gciRow);

        breathRow = std::make_unique<ParamRow> (proc, "breath2", ParamRow::Kind::slider,
            "Breath",
            vmTip ("EXPERIMENTAL. Replaces the upper harmonics with aspiration noise shaped by your "
                   "vocal tract (harmonic+noise model). Small amounts (0.1-0.2) add air; the quality "
                   "is still being tuned - leave at 0 if it sounds synthetic to you.",
                   "実験的機能。高域の倍音を、声道の響きで整形した気息ノイズに置き換えます"
                   "(ハーモニック+ノイズモデル)。0.1〜0.2で空気感が出ます。品質は調整中なので、"
                   "合成的に聞こえる場合は0のままにしてください。"));
        addAndMakeVisible (*breathRow);

        // Legacy Low Latency (moved here in v0.31.0, parameter id "lowlat"
        // unchanged). This is the OLD approach to latency: it shortens the
        // engine's lookahead and narrows its analysis, so it costs quality.
        // The new Performance Mode (MAIN tab, ADVANCED) is the quality-neutral
        // route and the two are completely independent — this one is kept
        // frozen so existing presets, sessions and automation keep working.
        lowLatRow = std::make_unique<ParamRow> (proc, "lowlat", ParamRow::Kind::toggle,
            "Legacy Low Latency",
            vmTip ("EXPERIMENTAL / LEGACY. The older way of cutting delay: it halves the "
                   "engine lookahead (43 ms -> about 21 ms) by shortening the analysis, so "
                   "pitch tracking bottoms out near 90 Hz, grains get narrower and the "
                   "Natural Air spectral cleanup cannot run. Deep voices and sustained "
                   "vowels can audibly suffer. It is kept for people already relying on it. "
                   "For lower delay WITHOUT a quality cost, use Performance Mode on the MAIN "
                   "tab together with a smaller device buffer. Ignored while Low Voice Mode "
                   "is on. Off = the normal, full-quality engine.",
                   "実験的機能 / 旧方式。遅延を削る古いやり方で、解析条件を短くすることで"
                   "エンジンの先読みを半分(43ms→約21ms)にします。その代償として"
                   "ピッチ検出の下限が約90Hzまで上がり、グレイン幅も狭くなり、"
                   "Natural Airのスペクトルクリーンアップが動作しません。低い声や"
                   "持続した母音では音質低下が分かる場合があります。すでにこの機能を"
                   "使っている方のために残しています。音質を落とさずに遅延を下げたい"
                   "場合は、MAINタブのPerformance Modeとデバイスバッファの縮小を"
                   "使ってください。Low Voice Modeがオンの間は無効です。"
                   "オフ=通常のフル品質エンジン。"));
        addAndMakeVisible (*lowLatRow);

        closeBtn.onClick = [this]
        {
            if (auto* dw = findParentComponentOfClass<juce::DocumentWindow>())
                dw->setVisible (false);
        };
        addAndMakeVisible (closeBtn);

        setSize (600, 280);
        sendLookAndFeelChange();
    }

    ~BetaPanel() override { setLookAndFeel (nullptr); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (14, 10);
        heading.setBounds (r.removeFromTop (24));
        note.setBounds (r.removeFromTop (46));
        r.removeFromTop (6);
        gciRow->setBounds (r.removeFromTop (28));
        r.removeFromTop (4);
        breathRow->setBounds (r.removeFromTop (30));
        r.removeFromTop (4);
        lowLatRow->setBounds (r.removeFromTop (28));
        closeBtn.setBounds (r.removeFromBottom (30).removeFromRight (100).reduced (0, 2));
    }

    void paint (juce::Graphics& g) override { g.fillAll (juce::Colour (0xfffafbff)); }

private:
    VoxMorphProcessor& proc;
    juce::LookAndFeel_V4 lnf { juce::LookAndFeel_V4::getLightColourScheme() };
    juce::TooltipWindow  tips { this, 400 };
    juce::Label heading, note;
    std::unique_ptr<ParamRow> gciRow, breathRow, lowLatRow;
    juce::TextButton closeBtn { "Close" };
};

class BetaWindow : public juce::DocumentWindow
{
public:
    explicit BetaWindow (VoxMorphProcessor& p)
        : juce::DocumentWindow (juce::String::fromUTF8 ("BETA \xE2\x80\x94 Experimental features"),
                                juce::Colour (0xfffafbff), juce::DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new BetaPanel (p), true);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }
    void closeButtonPressed() override { setVisible (false); }
};

// ---------------------------------------------------------------------------
// ANOKOE header bar (v0.31.0). Replaces the old standalone options strip:
// branding on the left, and on the right the four controls from the design —
// Monitor, Mute, Plugin and the gear (audio settings). Undo / Redo sit next to
// them because the tab strip that used to carry them is gone.
//
// The four action controls are standalone-only, exactly as before: in a DAW
// the host owns the audio device and the plugin chain.
//
// MUTE / MONITOR semantics (see also PluginProcessor.h):
//   * MONITOR on  -> the output device is switched to the monitor device
//                    chosen in Audio Settings, and MUTE is turned on too.
//   * MONITOR off -> the previous output device is restored. MUTE is left
//                    ON deliberately, so you never go live just by ending a
//                    monitoring session.
//   * MUTE off while monitoring -> "I want to be heard again", so monitoring
//                    ends as well and the normal output device comes back.
//   * While monitoring, MUTE does not silence anything (you are listening to
//     yourself); it only marks where you land when monitoring stops.
class HeaderBar : public juce::Component, private juce::Timer
{
public:
    explicit HeaderBar (VoxMorphProcessor& p) : proc (p)
    {
        standalone = proc.wrapperType == juce::AudioProcessor::wrapperType_Standalone;

        subtitle.setText ("VoiceChanger & PluginHost", juce::dontSendNotification);
        subtitle.setFont (ak::font (14.0f));
        subtitle.setColour (juce::Label::textColourId, ak::heading);
        addAndMakeVisible (subtitle);

        msg.setFont (ak::font (11.0f));
        msg.setColour (juce::Label::textColourId, juce::Colour (0xff8a7f83));
        msg.setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (msg);

        if (standalone)
        {
            monBtn.setTooltip (vmTip (
                "Temporarily switches the app's OUTPUT to the monitor device set in Audio "
                "Settings (typically your headphones), so you can check your voice without "
                "sending it out. MUTE is switched on automatically and stays on when you turn "
                "MONITOR off, so you never go live by accident.",
                "出力先を Audio Settings で設定したモニター用デバイス(通常はヘッドホン)に"
                "一時的に切り替え、配信に流さずに自分の声を確認できます。MUTEも自動でオンになり、"
                "MONITORをオフに戻してもMUTEはオンのままです(不意に配信へ復帰しないための"
                "安全動作)。"));
            monBtn.onClick = [this] { setMonitor (monBtn.getToggleState()); };
            addAndMakeVisible (monBtn);

            muteBtn.setTooltip (vmTip (
                "Silences the output so nothing reaches your stream / virtual cable. The "
                "conversion keeps running. Turning MUTE off while MONITOR is on also ends "
                "monitoring (you are going live again).",
                "出力を消音し、配信や仮想オーディオデバイスへ音が届かないようにします"
                "(変換自体は動き続けます)。MONITORがオンのときにMUTEを解除すると、"
                "配信に戻る操作とみなしてMONITORも自動でオフになります。"));
            muteBtn.onClick = [this] { setMute (muteBtn.getToggleState(), true); };
            addAndMakeVisible (muteBtn);

            plugBtn.setTooltip (juce::String::fromUTF8 (
                "外部VST3プラグイン(Pre/Post FX)の管理ウィンドウを開きます。"));
            plugBtn.setColour (juce::TextButton::buttonColourId, ak::pluginFill);
            plugBtn.onClick = [this]
            {
                if (fxWin == nullptr) fxWin = std::make_unique<FxChainWindow> (proc);
                else { fxWin->setVisible (true); fxWin->toFront (true); }
            };
            addAndMakeVisible (plugBtn);

            gearBtn.setFramed (true);
            gearBtn.setTooltip (juce::String::fromUTF8 (
                "オーディオ入出力デバイス・サンプルレート・バッファ、モニター出力先、"
                "Auto-Mute on Feedback の設定を開きます。"));
            gearBtn.onClick = [this]
            {
                if (audioWin == nullptr) audioWin = std::make_unique<AudioSettingsWindow> (proc);
                else { audioWin->setVisible (true); audioWin->toFront (true); }
            };
            addAndMakeVisible (gearBtn);

            syncButtons();
        }
        startTimerHz (4);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (20, 8);
        auto right = r.removeFromRight (juce::jlimit (60, standalone ? 400 : 60,
                                                      r.getWidth() - 220));
        auto row = right.withSizeKeepingCentre (right.getWidth(), 34);
        if (standalone)
        {
            gearBtn.setBounds (row.removeFromRight (34));
            row.removeFromRight (9);
            plugBtn.setBounds (row.removeFromRight (juce::jmin (118, row.getWidth() / 3)));
            row.removeFromRight (10);
            muteBtn.setBounds (row.removeFromRight (juce::jmin (92, row.getWidth() / 2)));
            row.removeFromRight (4);
            monBtn.setBounds (row.removeFromRight (juce::jmin (108, row.getWidth())));
        }

        // branding: wordmark then subtitle (the app icon was removed in
        // v0.33.0 — the character already owns the centre of the screen)
        const int logoW = juce::jlimit (110, 178, r.getWidth() / 4);
        auto brand = r;
        brand.removeFromLeft (logoW + 22);
        subtitle.setBounds (brand.removeFromLeft (juce::jmax (0, brand.getWidth() - 4)));
        msg.setBounds (subtitle.getBounds());
        subtitle.setVisible (msg.getText().isEmpty());
    }

    // The white bar itself is painted by the editor, BEFORE the band, so the
    // band's circular bulge can cut into it. This only draws the branding.
    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().reduced (20, 8);
        const int logoW = juce::jlimit (110, 178, r.getWidth() / 4);
        auto logoArea = r.removeFromLeft (logoW);
        if (auto lg = ak::image ("ui_logo_ANOKOE_rendered_png"); lg.isValid())
        {
            const float h = juce::jmin ((float) logoArea.getHeight(),
                                        (float) logoW * (float) lg.getHeight() / (float) lg.getWidth());
            ak::drawFitted (g, lg, logoArea.toFloat().withSizeKeepingCentre ((float) logoW, h));
        }
    }

private:
    void setMute (bool on, bool userAction)
    {
        proc.muted.store (on);
        if (userAction && ! on && proc.monitoring.load())
            setMonitor (false);
        syncButtons();
    }

    void setMonitor (bool on)
    {
        if (on == proc.monitoring.load()) { syncButtons(); return; }

        if (on)
        {
            if (proc.monitorDeviceName.isEmpty())
            {
                flash (juce::String::fromUTF8 (
                    "モニター出力先が未設定です → 歯車ボタンの Audio Settings で選択してください"));
                syncButtons();
                return;
            }
            proc.preMonitorDeviceName = vmCurrentOutputDevice();
            const auto err = vmSetOutputDevice (proc.monitorDeviceName);
            if (err.isNotEmpty())
            {
                proc.preMonitorDeviceName.clear();
                flash (juce::String::fromUTF8 ("モニターに切り替えられませんでした: ") + err);
                syncButtons();
                return;
            }
            proc.monitoring.store (true);
            proc.muted.store (true);          // monitoring implies "not live"
            flash (juce::String::fromUTF8 ("モニター中: ") + proc.monitorDeviceName);
        }
        else
        {
            if (proc.preMonitorDeviceName.isNotEmpty())
                vmSetOutputDevice (proc.preMonitorDeviceName);
            proc.preMonitorDeviceName.clear();
            proc.monitoring.store (false);
            flash (juce::String::fromUTF8 ("モニター終了 — MUTEは継続中です"));
        }
        syncButtons();
    }

    void syncButtons()
    {
        muteBtn.setToggleState (proc.muted.load(),      juce::dontSendNotification);
        monBtn .setToggleState (proc.monitoring.load(), juce::dontSendNotification);
    }

    void flash (const juce::String& s)
    {
        msg.setText (s, juce::dontSendNotification);
        subtitle.setVisible (false);
        msgSec = 5.0f;
    }

    void timerCallback() override
    {
        if (standalone)
        {
            // if the output device changed behind our back, monitoring is no
            // longer what the button claims — drop it, keeping MUTE on
            if (proc.monitoring.load() && vmCurrentOutputDevice() != proc.monitorDeviceName)
            {
                proc.monitoring.store (false);
                proc.preMonitorDeviceName.clear();
                flash (juce::String::fromUTF8 ("出力デバイスが変更されたためモニターを終了しました"));
            }
            syncButtons();
        }

        if (msgSec > 0.0f && (msgSec -= 0.25f) <= 0.0f)
        {
            msg.setText ({}, juce::dontSendNotification);
            subtitle.setVisible (true);
        }
    }

    VoxMorphProcessor& proc;
    bool standalone = false;
    juce::Label subtitle, msg;
    ak::StatusButton monBtn  { "Monitor", "ui_mark_M_Monitor_png", false };
    ak::StatusButton muteBtn { "Mute",    "ui_mark_M_Mute_png",    true  };
    juce::TextButton plugBtn { "Plugin" };
    ak::IconButton   gearBtn { "options", "ui_mark_S_Option_png", 20 };
    float msgSec = 0.0f;
    std::unique_ptr<FxChainWindow>       fxWin;
    std::unique_ptr<AudioSettingsWindow> audioWin;
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
        lnf.setColour (juce::Slider::trackColourId,             juce::Colour (0xff7999db));
        lnf.setColour (juce::Slider::backgroundColourId,        juce::Colour (0xffe9e9e9));
        lnf.setColour (juce::Slider::thumbColourId,             juce::Colour (0xff7999db));
        lnf.setColour (juce::Slider::textBoxTextColourId,       ak::ink);
        lnf.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::white);
        lnf.setColour (juce::Slider::textBoxOutlineColourId,    ak::line);
        lnf.setColour (juce::Label::textColourId,               ak::ink);
        lnf.setColour (juce::TextButton::buttonColourId,        juce::Colours::white);
        lnf.setColour (juce::TextButton::textColourOffId,       juce::Colour (0xff7999db));
        lnf.setColour (juce::ComboBox::textColourId,            ak::ink);
        setLookAndFeel (&lnf);
        charLbl.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
        charLbl.setColour (juce::Label::textColourId, ak::heading);
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
            secLbl[s].setColour (juce::Label::textColourId, ak::heading);
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
                                juce::Colour (0xfffafbff), juce::DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new AEIOUCharacterPanel (p), true);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
    }
    void closeButtonPressed() override { setVisible (false); }
};

// ---------------------------------------------------------------------------
// Hero circle (v0.32.0): the character portrait that sits in the dark band,
// wearing the live AEIOU balance as a ring. The ring is the same measurement
// the VISUALIZER page's vowel donut shows — the engine's own vowel coordinate
// through VowelAdaptiveWarp::anchorWeights — so it needs AEIOU Character on;
// with it off the ring rests as a quiet idle band and the caption says so.
//
// A second, outer halo follows the OUTPUT level, so the circle still breathes
// with your voice even when the vowel tracker is not running.
class HeroCircle : public juce::Component, public juce::SettableTooltipClient,
                   private juce::Timer
{
public:
    explicit HeroCircle (VoxMorphProcessor& p) : proc (p)
    {
        setTooltip (vmTip (
            "The AEIOU balance of your voice right now, as a ring around the character: "
            "how much /a/ /i/ /u/ /e/ /o/ the engine hears. It needs AEIOU Character "
            "switched on (FORMANT) with Amount above 0, because the vowel tracking is "
            "part of that feature. The soft outer halo follows the output level and works "
            "at all times.",
            "いま話している声の母音バランス(あいうえおの割合)を、キャラクターの周りの"
            "リングで表示します。FORMANTのAEIOU CharacterがオンでAmountが0より大きい"
            "ときに動きます(母音の推定がこの機能の一部のため)。外側のふんわりした光は"
            "出力レベルに追従し、こちらは常に動作します。"));
        startTimerHz (24);
    }

private:
    static constexpr int kV = 5;
    static constexpr const char* kLbl[kV] = { "A", "I", "U", "E", "O" };

    static juce::Colour vowelColour (int i)
    {
        static const juce::Colour c[kV] = {
            juce::Colour (0xfff08ba5), juce::Colour (0xff54c0aa), juce::Colour (0xffa79ee0),
            juce::Colour (0xffe3a63c), juce::Colour (0xff6fb2dc)
        };
        return c[juce::jlimit (0, kV - 1, i)];
    }

    void timerCallback() override
    {
        if (! isShowing()) return;
        const bool  live = proc.uiVowelActive.load (std::memory_order_relaxed);
        const float conf = proc.uiVowelConf  .load (std::memory_order_relaxed);
        const bool  good = live && conf > 0.02f;
        if (good)
        {
            float w[kV];
            VowelAdaptiveWarp::anchorWeights (proc.uiVowelH.load (std::memory_order_relaxed),
                                              proc.uiVowelF.load (std::memory_order_relaxed), w);
            for (int i = 0; i < kV; ++i) sm[i] += 0.22f * (w[i] - sm[i]);
        }
        fade   += 0.14f * ((good ? 1.0f : 0.15f) - fade);
        active  = live;

        const float pk = juce::jmax (proc.uiOutL.peak.load (std::memory_order_relaxed),
                                     proc.uiOutR.peak.load (std::memory_order_relaxed));
        const float db = juce::Decibels::gainToDecibels (pk, -60.0f);
        halo += 0.25f * (juce::jlimit (0.0f, 1.0f, (db + 54.0f) / 54.0f) - halo);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        const float side = juce::jmin (b.getWidth(), b.getHeight());
        const auto  c    = b.getCentre();
        const float portR = side * 0.5f - 2.0f;
        const auto  port  = juce::Rectangle<float> (portR * 2.0f, portR * 2.0f).withCentre (c);

        // v0.33.0: the coloured AEIOU ring is gone — the dark collar around
        // the portrait is drawn by the band itself, and the only live
        // decoration left is the rim, which brightens with the output level.
        float sum = 0.0f;
        for (int i = 0; i < kV; ++i) sum += juce::jmax (0.0f, sm[i]);

        {
            juce::Graphics::ScopedSaveState ss (g);
            juce::Path clip;
            clip.addEllipse (port);
            g.reduceClipRegion (clip);
            g.setColour (juce::Colour (0xfff2f6ff));
            g.fillEllipse (port);

            // the art is a rounded-square app icon, so oversize it: the
            // square edges land outside the circular clip
            if (auto ic = ak::image ("icon_png"); ic.isValid())
                g.drawImage (ic, port.expanded (portR * 0.16f).translated (0.0f, -portR * 0.10f),
                             juce::RectanglePlacement::fillDestination, false);

            // caption plate: a chord across the WIDE part of the circle (not
            // the very bottom, where it would be too narrow for five
            // readings), faded in from the top so it does not cut a hard
            // line across her face
            auto plate = juce::Rectangle<float> (port.getX(), c.y + portR * 0.14f,
                                                 port.getWidth(), portR * 0.66f);
            g.setGradientFill (juce::ColourGradient (
                juce::Colour (0x00161a2c), plate.getCentreX(), plate.getY(),
                juce::Colour (0xe6161a2c), plate.getCentreX(), plate.getY() + plate.getHeight() * 0.45f,
                false));
            g.fillRect (plate);

            auto line1 = plate.removeFromTop (plate.getHeight() * 0.52f)
                              .withTrimmedTop (portR * 0.14f);
            g.setColour (ak::bandDim);
            g.setFont (ak::font (juce::jmax (8.0f, portR * 0.125f), true));
            g.drawText (active ? "AEIOU BALANCE" : "AEIOU CHARACTER OFF",
                        line1.toNearestInt(), juce::Justification::centred, false);

            if (active && sum > 1.0e-6f)
            {
                // the circle narrows toward the bottom, so keep the row well
                // inside the chord width or the outer cells get clipped
                auto rowArea = plate.reduced (portR * 0.30f, 0.0f);
                const float cw = rowArea.getWidth() / (float) kV;
                g.setFont (ak::font (juce::jmax (7.5f, portR * 0.115f)));
                for (int i = 0; i < kV; ++i)
                {
                    auto cell = rowArea.withWidth (cw).translated (cw * (float) i, 0.0f);
                    g.setColour (vowelColour (i).withMultipliedAlpha (0.45f + 0.55f * fade));
                    g.drawText (juce::String (kLbl[i]) + " "
                                  + juce::String (juce::roundToInt (
                                        juce::jmax (0.0f, sm[i]) / sum * 100.0f)) + "%",
                                cell.toNearestInt(), juce::Justification::centred, false);
                }
            }
        }

        // rim: a quiet ring that brightens with the output level, so the
        // circle still reacts to your voice without the busy coloured donut
        g.setColour (juce::Colours::white.withAlpha (0.30f + 0.45f * halo));
        g.drawEllipse (port.reduced (0.75f), 1.5f);
    }

    VoxMorphProcessor& proc;
    float sm[kV] = { 0.2f, 0.2f, 0.2f, 0.2f, 0.2f };
    float fade = 0.15f, halo = 0.0f;
    bool  active = false;
};

// ===========================================================================
// ANOKOE editor (v0.31.0)
//
// Layout (from the ANOKOE_UI_Offline prototype):
//
//   +-------------------------------------------------------------+
//   | header: icon  ANOKOE  subtitle      Undo Redo Mon Mute Plug |
//   +---------+---------------------------------------------------+
//   | sidebar |  Main / Matching / ASMR / Visualizer / Presets     |
//   |  Main   |  page content (Main = the card grid below)         |
//   |  ...    |                                                   |
//   +---------+---------------------------------------------------+
//
// Main page card grid (3 columns + a bottom row spanning all of them):
//   col 1  PITCH · INTONATION · ADVANCED
//   col 2  FORMANT
//   col 3  preset bar · AIR · VOICE QUALITY
//   bottom HIGH RANGE / LOW LIMIT · latency donut · OUTPUT
//
// HOW TO EDIT THIS UI (for future maintainers):
//  - To add a control, add ONE line inside the relevant buildXxxCard() —
//    slider / toggle / knob / combo / button rows are all ParamRow. The card
//    stacks whatever it is given and the grid sizes itself from
//    Card::preferredHeight(), so nothing else needs touching.
//  - Colours, art and geometry live in AnokoeTheme.h; the skinned drawing of
//    knobs / sliders / checkboxes lives in AnokoeWidgets.h.
//  - The three analysis donuts (radial spectrum, AEIOU mix, level rings) and
//    the linear spectrum live on the VISUALIZER page.
class VoxMorphEditor : public juce::AudioProcessorEditor,
                       private juce::Timer
{
public:
    explicit VoxMorphEditor (VoxMorphProcessor& p)
        : juce::AudioProcessorEditor (&p), proc (p)
    {
        tooltipWindow.setLookAndFeel (&tipLnf);
        setWantsKeyboardFocus (true);          // Cmd+S / Cmd+Z shortcuts
        setLookAndFeel (&lnfBlue);

        addAndMakeVisible (header);
        addAndMakeVisible (hero);

        // ---- sidebar ----------------------------------------------------
        static const struct { const char* label; const char* icon; } kPages[] = {
            { "Main",       "ui_mark_L_Main_png"     },
            { "Matching",   "ui_mark_L_Matching_png" },
            { "ASMR",       "ui_mark_L_ASMR_png"     },
            { "Visualizer", "ui_mark_L_Analyzer_png" },
            { "Presets",    "ui_mark_S_Save_png"     },
        };
        for (int i = 0; i < (int) std::size (kPages); ++i)
        {
            auto* b = navButtons.add (new ak::TabButton (kPages[i].label, kPages[i].icon));
            b->onClick = [this, i] { showPage (i); };
            addAndMakeVisible (b);
        }

        // ---- pages ------------------------------------------------------
        mainScroll.setViewedComponent (&mainPage, false);
        mainScroll.setScrollBarsShown (true, false);
        mainScroll.setScrollBarThickness (10);
        pages.push_back (&mainScroll);
        pages.push_back (&matchingPanel);
        pages.push_back (&asmrPanel);
        pages.push_back (&vizPage);
        pages.push_back (&presetPanel);
        for (auto* pg : pages) addChildComponent (pg);

        buildMainPage();
        buildVisualizerPage();

        footer.setFont (ak::font (11.0f));
        footer.setColour (juce::Label::textColourId, ak::heading.withAlpha (0.85f));
        footer.setJustificationType (juce::Justification::centredLeft);
        footer.setText (juce::String::fromUTF8 (
            "各項目にマウスを乗せると説明が出ます。数値クリックで入力、リセットボタンで初期値。"),
            juce::dontSendNotification);
        mainPage.addAndMakeVisible (footer);
        defaultFooterText = footer.getText();

        // history poll: coalesces a settled burst of knob edits into one undo
        // step and keeps every row's lock indicator in sync
        histPoll.fn = [this]
        {
            proc.history.poll();
            if (lastLockState != proc.lockedIds.joinIntoString (","))
                syncLockUI();                    // e.g. host restored state
            // Level metering costs the audio thread a pass per channel, so
            // only ask for it while a meter is actually on screen. The
            // ballistics settle in ~20 ms, well inside the ~330 ms it takes
            // this poller to notice a page switch.
            proc.uiWantsMeters.store (levels.isShowing() || outMeter.isShowing(),
                                      std::memory_order_relaxed);
        };
        histPoll.startTimerHz (3);
        syncLockUI();

        showPage (0);
        setResizable (true, true);
        setResizeLimits (kMinW, kMinH, 2400, 1800);
        // The card grid plus the hero band needs room, so the minimum is
        // deliberately large. Open at the design size but never bigger than
        // the screen allows (a 1440x900 laptop would otherwise get a window
        // it cannot see).
        int w = 1400, h = 1080;
        if (auto* d = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
        {
            w = juce::jlimit (kMinW, w, d->userArea.getWidth()  - 40);
            h = juce::jlimit (kMinH, h, d->userArea.getHeight() - 80);
        }
        setSize (w, h);
        sendLookAndFeelChange();
    }

    ~VoxMorphEditor() override
    {
        // nothing is left to read the display taps: stop the audio thread
        // filling them (SpectrumData clears uiWantsViz in its own destructor)
        proc.uiWantsMeters.store (false, std::memory_order_relaxed);
        setLookAndFeel (nullptr);
        tooltipWindow.setLookAndFeel (nullptr);
    }

    // Cmd+Z / Shift+Cmd+Z / Cmd+Y = undo / redo, Cmd+S = save app settings
    bool keyPressed (const juce::KeyPress& key) override
    {
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
                if (auto* pf = dynamic_cast<juce::PropertiesFile*> (holder->settings.get()))
                    pf->saveIfNeeded();
                flashFooter (juce::String::fromUTF8 ("\xE2\x9C\x93 設定を保存しました"));
                return true;
            }
           #endif
        }
        return false;
    }

    // Order matters: the white header bar is painted HERE, before the band,
    // so the band's circular bulge cuts a notch into it. HeaderBar itself is
    // transparent and only draws its wordmark.
    void paint (juce::Graphics& g) override
    {
        ak::paintPage (g, getLocalBounds());
        g.setColour (juce::Colours::white);
        g.fillRect (getLocalBounds().withHeight (ak::kHeaderH));
        ak::paintBand (g, bandShape, bandArea);
        paintFlowLines (g);
    }

    // ---- signal-chain flow routes (v0.36.0) ------------------------------
    // A node diagram, not a drawn curve: the conversion chain is
    //
    //     collar -> [marker] -> [HEADING] -> [label] -> (knob)
    //
    // and every edge attaches to the BOUNDARY of the node it joins — the
    // glyph box of the heading and label text, the marker's disc, the knob's
    // and the collar's rim. Nothing is drawn across a node; the node itself
    // is the break in the run. That is what keeps the lines off the text.
    //
    // Runs are horizontal wherever the two nodes allow it, and every change
    // of direction is a tangent arc, so straights and curves meet smoothly.
    //
    // v0.36.4: the collar angles, marker placement and handle fractions below
    // are FITTED to the user's annotated reference (RMS under 3 px on all
    // three routes), not chosen by eye.
    //
    // Purely decorative; nothing here reads or writes a parameter.
    void paintFlowLines (juce::Graphics& g)
    {
        if (currentPage != 0 || cardPitch == nullptr || heroOuter <= 0.0f) return;

        auto onCollar = [this] (float deg)
        {
            const float a = juce::degreesToRadians (deg);
            return heroC + juce::Point<float> (std::sin (a), -std::cos (a)) * heroOuter;
        };

        // Angles and marker positions are measured off the annotated
        // reference, per column: the PITCH run is nearly 3x longer than the
        // other two, so one shared fraction cannot place all three markers.
        // markDy drops the marker below the heading's line. 0 keeps step 2 a
        // pure horizontal straight (PITCH, FORMANT); the AIR run would
        // otherwise be dead flat end to end, so its marker hangs below and
        // the run S-curves back up into the heading.
        struct Route
        {
            float collarDeg, markT, markDy;
            ak::Card* card; ParamRow* row;
        };
        const Route routes[3] = {
            { 218.0f, 0.620f,  0.0f, cardPitch,   rowPitch   },
            { 188.5f, 0.546f,  0.0f, cardFormant, rowFormant },
            { 132.4f, 0.397f, 18.0f, cardAir,     rowAir     },
        };

        constexpr float kMarkR = 13.0f;   // marker disc
        constexpr float kGapN  = 6.0f;    // clearance around every node
        // Bezier handles as a FRACTION of each run's own length. Fixed
        // lengths were the bug behind the old shapes: 34 px and 56 px are
        // longer than the AIR and FORMANT runs themselves (~42 px), so those
        // curves doubled back on their own start before reaching the marker.
        constexpr float kHRadial = 0.19f; // leaving the collar, along the rim's radius
        constexpr float kHFlat   = 0.73f; // arriving at the marker, horizontal

        juce::Graphics::ScopedSaveState ss (g);
        g.reduceClipRegion (juce::Rectangle<int> (0, bandArea.getBottom(),
                                                  getWidth(), getHeight()));

        for (const auto& rt : routes)
        {
            if (rt.card == nullptr || rt.row == nullptr) continue;

            const auto collar = onCollar (rt.collarDeg);

            // --- node boxes, in editor coordinates ---------------------
            const auto headBox = getLocalArea (rt.card, rt.card->titleTextBounds()).toFloat();
            const auto lblBox  = getLocalArea (rt.row,  rt.row->labelTextBounds()).toFloat();
            const auto kb      = rt.row->knobBounds();
            const auto knobC   = getLocalPoint (rt.row, kb.getCentre()).toFloat();
            const float knobR  = (float) juce::jmin (kb.getWidth(), kb.getHeight()) * 0.5f;

            // The heading node is the mark AND its text. Coming from the
            // right the run meets the text's end, but coming from the LEFT it
            // has to stop at the MARK — aiming at the first letter would run
            // the line straight through the glyph. Only the AIR route
            // approaches from that side, which is why this shows up there.
            const auto headNode = getLocalArea (rt.card, rt.card->titleNodeBounds()).toFloat();
            const bool  fromRight = collar.x > headNode.getCentreX();
            const juce::Point<float> headIn (fromRight ? headBox.getRight() + kGapN
                                                       : headNode.getX() - kGapN,
                                             headBox.getCentreY());
            // The drop runs down the SAME column as the row brackets, so the
            // decorative connector and the tree lines read as one rail.
            const float colX = (float) getLocalPoint (
                                   rt.card, juce::Point<int> (rt.card->treeRailX(), 0)).x;

            const juce::Point<float> mark (collar.x + (headIn.x - collar.x) * rt.markT,
                                           headIn.y + rt.markDy);
            // Both legs meet the disc on its SIDES, so each one leaves and
            // arrives horizontally. Stopping along the chord instead (what
            // this used to do) left the line hanging in the air beside the
            // disc rather than touching it — clearest on FORMANT, where the
            // run came in steeply and stopped 13 px above the marker.
            const float side = fromRight ? 1.0f : -1.0f;
            const juce::Point<float> markIn  (mark.x + side * (kMarkR + 2.0f), mark.y);
            const juce::Point<float> markOut (mark.x - side * (kMarkR + 2.0f), mark.y);

            g.setColour (ak::treeLine);

            // 1  collar -> marker: leaves the rim radially, eases over into
            //    horizontal and meets the disc square on its side
            {
                const auto  dir = (collar - heroC) / heroOuter;
                const auto  from = collar + dir * kGapN;
                const float leg  = from.getDistanceFrom (markIn);
                juce::Path pth;
                pth.startNewSubPath (from);
                pth.cubicTo (from + dir * (kHRadial * leg),
                             markIn + juce::Point<float> (side * kHFlat * leg, 0.0f),
                             markIn);
                stroke (g, pth);
            }

            // 2  marker -> heading: a horizontal straight when the two sit on
            //    one line, otherwise an S that leaves and arrives horizontally
            {
                juce::Path pth;
                pth.startNewSubPath (markOut);
                if (std::abs (headIn.y - markOut.y) < 1.5f)
                    pth.lineTo (headIn);                                  // pure horizontal
                else
                {
                    const float dx = headIn.x - markOut.x;
                    pth.cubicTo (markOut + juce::Point<float> (dx * 0.36f, 0.0f),
                                 headIn  - juce::Point<float> (dx * 0.64f, 0.0f),
                                 headIn);
                }
                stroke (g, pth);
            }

            // 3  heading -> label: a straight drop down their shared column,
            //    clear of both glyph boxes
            {
                juce::Path pth;
                pth.startNewSubPath (colX, headBox.getBottom() + kGapN);
                pth.lineTo (colX, lblBox.getY() - kGapN);
                stroke (g, pth);
            }

            // 4  label -> knob: a horizontal STRAIGHT out of the label, then
            //    one curve onto the knob's rim along 7:30. The old single
            //    cubic left the label horizontally but started bending (and
            //    dipping below the label's line) immediately, so nothing of
            //    the run actually read as flat.
            {
                const juce::Point<float> dir (-0.7071f, 0.7071f);          // 7:30
                const auto rim  = knobC + dir * (knobR + kGapN);
                const auto from = juce::Point<float> (lblBox.getRight() + kGapN,
                                                      lblBox.getCentreY());
                const float span = rim.x - from.x;

                juce::Path pth;
                pth.startNewSubPath (from);
                if (span > 8.0f)
                {
                    const float flat  = juce::jmax (16.0f, span * 0.45f);
                    // the second handle leans back down the 7:30 radius, so
                    // cap it at the height the knob sits above the label —
                    // any longer and the handle drops under the straight and
                    // the curve sags on its way up
                    const float rise  = juce::jmax (0.0f, from.y - rim.y);
                    const float ease  = juce::jlimit (10.0f, juce::jmax (10.0f, rise * 1.35f),
                                                      (span - flat) * 0.55f);
                    const juce::Point<float> elbow (from.x + flat, from.y);
                    pth.lineTo (elbow);
                    pth.cubicTo (elbow + juce::Point<float> (ease, 0.0f),
                                 rim + dir * ease, rim);
                }
                else
                {
                    pth.cubicTo (from + juce::Point<float> (40.0f, 0.0f),
                                 rim + dir * 46.0f, rim);
                }
                stroke (g, pth);
            }

            // the marker disc last, so it sits cleanly on top
            const auto sym = ak::tintedImage ("ui_mark_M_Advanced_png", ak::treeLine);
            g.setColour (ak::bodyFill);
            g.fillEllipse (juce::Rectangle<float> (kMarkR * 2.0f, kMarkR * 2.0f).withCentre (mark));
            ak::drawFitted (g, sym, juce::Rectangle<float> (21.0f, 21.0f).withCentre (mark));
        }
    }

    // same weight as the tree brackets (they fill a 1 px rect), so both kinds
    // of connector read as one family
    static void stroke (juce::Graphics& g, const juce::Path& p)
    {
        g.strokePath (p, juce::PathStrokeType (1.0f, juce::PathStrokeType::curved,
                                                     juce::PathStrokeType::rounded));
    }

    void resized() override
    {
        auto r = getLocalBounds();
        header.setBounds (r.removeFromTop (ak::kHeaderH));

        bandArea = r.removeFromTop (ak::kBandH);

        // the character straddles the band's lower edge; the band bulges past
        // the portrait by kHeroRim, which is the dark collar around her
        heroC = { (float) bandArea.getCentreX(), (float) bandArea.getBottom() - 74.0f };
        heroOuter = (float) (ak::kHeroR + ak::kHeroRim);
        bandShape = ak::bandPath (bandArea, heroC, heroOuter);
        hero.setBounds (juce::Rectangle<int> (ak::kHeroR * 2, ak::kHeroR * 2)
                            .withCentre (heroC.roundToInt()));

        // page tabs hang off the band's bottom edge: Main / Matching / ASMR on
        // the left, Visualizer / Presets on the right, clear of the character
        {
            auto strip = bandArea.withTop (bandArea.getBottom() - ak::kTabH);
            const int tw = juce::jlimit (74, 112, strip.getWidth() / 12);
            auto left  = strip.withTrimmedLeft (18);
            auto right = strip.withTrimmedRight (18);
            for (int i = 0; i < navButtons.size(); ++i)
            {
                if (i < kLeftTabs)
                {
                    navButtons[i]->setBounds (left.removeFromLeft (tw));
                    left.removeFromLeft (6);
                }
                else
                {
                    navButtons[navButtons.size() - 1 - (i - kLeftTabs)]
                        ->setBounds (right.removeFromRight (tw));
                    right.removeFromRight (6);
                }
            }
        }

        // preset row: top-right of the band, sized like a body dropdown
        if (presetBar != nullptr)
        {
            auto slot = bandArea.withTrimmedRight (20).withTrimmedTop (16).withHeight (30);
            const int pw = juce::jlimit (200, 268, slot.getWidth() / 4);
            presetBar->setBounds (slot.removeFromRight (pw));
        }

        // Only the MIDDLE column sits under the character, so the content as
        // a whole starts right below the strip and just the FORMANT column is
        // pushed down past the bulge (see layoutMainPage).
        bulgeBottom = bandArea.getBottom() - 74 + ak::kHeroR + ak::kHeroRim;
        pageArea = r.reduced (12, 8).withTop (bandArea.getBottom() + 8);
        for (auto* pg : pages) pg->setBounds (pageArea);
        layoutMainPage();
    }

    // Standalone: native title bar, and hide JUCE's in-titlebar Options button
    // (its functions live in the header's gear).
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

private:
    static constexpr int kMinW = 1180, kMinH = 920;

    // ---- page switching -------------------------------------------------
    void showPage (int index)
    {
        currentPage = juce::jlimit (0, (int) pages.size() - 1, index);
        for (int i = 0; i < (int) pages.size(); ++i)
            pages[(size_t) i]->setVisible (i == currentPage);
        for (int i = 0; i < navButtons.size(); ++i)
            navButtons[i]->setToggleState (i == currentPage, juce::dontSendNotification);
    }

    // ---- row / card builders --------------------------------------------
    ParamRow& row (ak::Card& card, const juce::String& id, ParamRow::Kind kind,
                   const juce::String& label, const juce::String& tipText,
                   ak::Tone tone, int height)
    {
        auto r = std::make_unique<ParamRow> (proc, id, kind, label, tipText, tone);
        r->setLookAndFeel (lnfFor (tone));
        r->onLockChanged = [this] { syncLockUI(); };
        auto* raw = r.get();
        card.add (*raw, height);
        rows.push_back (raw);
        owned.push_back (std::move (r));
        return *raw;
    }

    ParamRow& slider (ak::Card& c, const juce::String& id, const juce::String& label,
                      const juce::String& tipText, ak::Tone tone = ak::Tone::blue)
    {
        return row (c, id, ParamRow::Kind::slider, label, tipText, tone, ak::kRowH);
    }
    ParamRow& knob (ak::Card& c, const juce::String& id, const juce::String& label,
                    const juce::String& tipText, ak::Tone tone)
    {
        return row (c, id, ParamRow::Kind::knob, label, tipText, tone, ak::kKnobRowH);
    }
    ParamRow& toggle (ak::Card& c, const juce::String& id, const juce::String& label,
                      const juce::String& tipText, ak::Tone tone = ak::Tone::blue)
    {
        return row (c, id, ParamRow::Kind::toggle, label, tipText, tone, ak::kToggleH);
    }
    ParamRow& combo (ak::Card& c, const juce::String& id, const juce::String& label,
                     const juce::String& tipText, ak::Tone tone = ak::Tone::blue)
    {
        return row (c, id, ParamRow::Kind::combo, label, tipText, tone, 34);
    }
    ParamRow& button (ak::Card& c, const juce::String& label, const juce::String& btnText,
                      const juce::String& tipText, std::function<void()> onClick,
                      ak::Tone tone = ak::Tone::blue)
    {
        auto r = std::make_unique<ParamRow> (label, btnText, tipText, std::move (onClick));
        r->setLookAndFeel (lnfFor (tone));
        auto* raw = r.get();
        card_add (c, *raw, 30);
        owned.push_back (std::move (r));
        return *raw;
    }

    void card_add (ak::Card& c, juce::Component& comp, int h) { c.add (comp, h); }

    juce::LookAndFeel* lnfFor (ak::Tone t)
    {
        return t == ak::Tone::pink ? (juce::LookAndFeel*) &lnfPink
             : t == ak::Tone::yellow ? (juce::LookAndFeel*) &lnfYellow
                                     : (juce::LookAndFeel*) &lnfBlue;
    }

    ak::Card& newCard (const char* title, const char* icon,
                       juce::Colour tint = ak::heading,
                       juce::Colour iconTint = juce::Colours::transparentBlack)
    {
        auto c = std::make_unique<ak::Card>();
        if (title != nullptr) c->setTitle (title, icon, tint, iconTint);
        auto* raw = c.get();
        mainPage.addAndMakeVisible (*raw);
        cards.push_back (std::move (c));
        return *raw;
    }

    // ---- MAIN page -------------------------------------------------------
    void buildMainPage()
    {
        using K = ParamRow::Kind;
        juce::ignoreUnused ((int) K::slider);

        // -- column 1 --------------------------------------------------
        cardPitch = &newCard ("PITCH", "ui_mark_M_Pitch_png", ak::headBlue);
        rowPitch = &knob (*cardPitch, "pitch", "Pitch",
            tip ("Shifts the pitch in semitones. The timbre (formants) stays unchanged. "
                 "Male-to-female: +5 to +7. Female-to-male: around -5.",
                 "声の高さを半音単位で変えます。声色(フォルマント)は変わりません。"
                 "女声化は+5〜+7、男声化は-5前後が目安。"), ak::Tone::blue);
        // Robotize / Robot Pitch were dropped from the UI in v0.35.0 (unused).
        // The parameters stay registered, so saved sessions and presets that
        // carry them still load and behave exactly as before.

        cardInton = &newCard ("INTONATION", "ui_mark_M_Intonation_png", ak::headBlue);
        slider (*cardInton, "range", "Intonation Amount (%)",
            tip ("Exaggerates or flattens the pitch movement (intonation). 100% = unchanged. "
                 "Unlike 'Pitch', which moves the whole voice up or down, this scales only the movement. "
                 "110-140% recommended for male-to-female.",
                 "声の抑揚(音程の上がり下がり)を強調/抑制します。100%=変化なし。"
                 "Pitchが声全体を平行移動するのに対し、こちらは動きの幅だけを変えます。"
                 "女声化では110〜140%が目安です。"));
        slider (*cardInton, "center", "Intonation Pivot (Hz)",
            tip ("The pitch that intonation scaling expands around. Set it near the average pitch "
                 "of the converted voice (200-250 Hz for a female voice). No effect at 100% Amount.",
                 "抑揚を拡大/縮小するときの中心になる音程。変換後の声の平均的な高さに"
                 "合わせてください(女声なら200〜250Hz)。Amountが100%のときは無効。"));

        // The ADVANCED mark shares its art with the flow-line markers, which
        // draw it in treeLine — tinting it here pins it to the heading blue
        // no matter what else asks for a recoloured copy.
        cardAdvanced = &newCard ("ADVANCED", "ui_mark_M_Advanced_png",
                                 ak::headBlue, ak::markBlue);
        toggle (*cardAdvanced, "lowvoice", "Low Voice Mode",
            tip ("Extends pitch tracking for very low voices and vocal fry. It may retain more of "
                 "the original low-period texture depending on the voice.",
                 "非常に低い声やボーカルフライでもピッチ追跡を継続します。発声によっては、"
                 "元の低周期の質感が強く残る場合があります。"));
        // Performance Mode (their v0.31.0): replaces the old Low Latency toggle
        // here; that one is now "Legacy Low Latency" in the BETA window.
        toggle (*cardAdvanced, "perfmode", "Performance Mode",
            tip ("Helps VoxMorph run steadily at small audio buffers (64 / 128 / 256 samples) "
                 "WITHOUT changing the sound. Pitch, Formant, Natural Air and the engine "
                 "lookahead are all exactly the same with this on or off - what it lowers is "
                 "the refresh rate of the displays, which frees up the machine for the audio. "
                 "In the standalone app it also unlocks the buffer helper in Audio Settings. "
                 "It is not a 'make the delay smaller' switch on its own: the delay drops "
                 "because a smaller device buffer becomes practical.",
                 "小さいオーディオバッファ(64/128/256サンプル)でも安定して動くように"
                 "支援するモードです。音は一切変わりません - Pitch・Formant・Natural Air・"
                 "エンジンの先読みはオン/オフで完全に同じで、下げるのは表示の更新頻度だけ"
                 "です(その分の余力が音声処理に回ります)。スタンドアロン版では"
                 "Audio Settings内のバッファ調整も使えるようになります。"
                 "これ自体が遅延を減らすスイッチではなく、小さいバッファを実用にできる"
                 "結果として遅延が縮む、という機能です。"));
        toggle (*cardAdvanced, "stereo", "Stereo Input",
            tip ("For binaural / ASMR stereo microphones: the left and right inputs run through "
                 "two independent conversion engines in parallel, keeping the stereo image. "
                 "Latency is unchanged, CPU roughly doubles. Off = classic mono (inputs summed).",
                 "バイノーラル/ASMR用ステレオマイク向け。左右の入力を2つの独立した変換エンジンで"
                 "並列処理し、立体感を保ったまま変換します。遅延は変わりません(CPUは約2倍)。"
                 "オフ=従来どおりモノラル(左右を合成)。"));
        slider (*cardAdvanced, "gate", "Noise Gate (dB)",
            tip ("Mutes the input while it stays below this level - removes fan / room noise "
                 "between phrases. -80 = off. Set it just above your noise floor (try -55 to -45).",
                 "入力がこのレベルを下回っている間ミュートし、話していない間のファンノイズや"
                 "環境音を消します。-80=オフ。ノイズの音量より少し上に設定してください"
                 "(目安 -55〜-45)。"));
        button (*cardAdvanced, "Experimental", "BETA",
            tip ("Opens the BETA window with the experimental controls (GCI Grain Sync, "
                 "Breath, Legacy Low Latency). They are kept out of the main list because "
                 "their quality is still being tuned; every one defaults to the classic behaviour.",
                 "実験中の機能(GCI Grain Sync、Breath、Legacy Low Latency)をまとめた"
                 "BETAウィンドウを開きます。"
                 "品質を調整中のため通常の一覧からは外していますが、既定値はいずれも"
                 "従来どおりの動作です。"),
            [this]
            {
                if (betaWin == nullptr) betaWin = std::make_unique<BetaWindow> (proc);
                else { betaWin->setVisible (true); betaWin->toFront (true); }
            });

        // -- column 2: FORMANT ----------------------------------------
        cardFormant = &newCard ("FORMANT", "ui_mark_M_Formant_png", ak::headBlue);
        rowFormant = &knob (*cardFormant, "formant", "Formant",
            tip ("Changes the vocal-tract size = the timbre, without changing pitch. "
                 "+ sounds younger/feminine, - sounds deeper/masculine. +3 to +4 for male-to-female.",
                 "声道の長さ=声の響き・声色を変えます。ピッチは変わりません。"
                 "+で若く/女性的に、-で太く/男性的に。女声化は+3〜+4が目安。"), ak::Tone::pink);
        auto& rConst = slider (*cardFormant, "consonant", "Const (st)",
            tip ("Extra shift applied only to unvoiced consonants (s, sh...), added on top of Formant. "
                 "Female consonants are brighter: try +2 to +3. Too much sounds like a lisp.",
                 "無声子音(サ行・シャ行など)だけを追加でシフトします(Formantに加算)。"
                 "女声の子音は明るいので+2〜+3が目安。上げすぎると舌足らずに聞こえます。"), ak::Tone::pink);
        auto& rF1S = slider (*cardFormant, "f1shift", "F1 Shift (st)",
            tip ("Moves only the first formant (jaw openness / throat size), on top of the global "
                 "Formant. Male-to-female sounds most natural with F1 raised LESS than F2: "
                 "try F1 +1 to +2 when F2 is +2 to +4.",
                 "第1フォルマント(顎の開き・喉の広さ)だけを動かします(全体Formantに加算)。"
                 "女声化はF1をF2より控えめに上げると自然です(F2が+2〜+4のときF1は+1〜+2)。"), ak::Tone::pink);
        auto& rF1G = slider (*cardFormant, "f1gain", "F1 Gain (dB)",
            tip ("Boost or cut around the first formant. Cutting a few dB thins out a 'boomy' "
                 "chest resonance.",
                 "第1フォルマント付近の強さ。数dB下げると胸に響く「太さ」が抜けます。"), ak::Tone::pink);
        auto& rF2S = slider (*cardFormant, "f2shift", "F2 Shift (st)",
            tip ("Moves only the second formant (tongue position). The strongest single cue for "
                 "perceived gender/age of the vowels: +2 to +4 sounds younger and more feminine.",
                 "第2フォルマント(舌の位置)だけを動かします。母音の性別・年齢感に最も効く帯域で、"
                 "+2〜+4で若く女性的に聞こえます。"), ak::Tone::pink);
        auto& rF2G = slider (*cardFormant, "f2gain", "F2 Gain (dB)",
            tip ("Boost or cut around the second formant. A few dB of boost adds clarity and "
                 "'presence' to the vowels.",
                 "第2フォルマント付近の強さ。数dB上げると母音の明瞭さ・華やかさが出ます。"), ak::Tone::pink);
        auto& rF3S = slider (*cardFormant, "f3shift", "F3 Shift (st)",
            tip ("Moves only the third formant (front cavity / lip area). Small shifts (+1 to +2) "
                 "refine the impression of a shorter vocal tract.",
                 "第3フォルマント(声道前部・唇まわり)だけを動かします。+1〜+2の小さめの操作で"
                 "「声道が短い」印象を仕上げます。"), ak::Tone::pink);
        auto& rF3G = slider (*cardFormant, "f3gain", "F3 Gain (dB)",
            tip ("Boost or cut around the third formant. Boosting adds sheen and 'sparkle' - "
                 "this region carries much of a voice's charm.",
                 "第3フォルマント付近の強さ。上げると艶・張りが出ます。声の「華」が乗る帯域です。"), ak::Tone::pink);
        bracket ({ &rConst, &rF1S, &rF1G, &rF2S, &rF2G, &rF3S, &rF3G });
        cardFormant->addGap (14);          // blank line before AEIOU

        toggle (*cardFormant, "vadapt", "AEIOU Character",
            tip ("Shapes the voice character by applying different F1-F3 adjustments to the "
                 "estimated A/E/I/O/U vowel regions. Your manual F1-F3 settings remain the "
                 "base values; the per-vowel offsets are added on top. Off = previous behaviour.",
                 "発音中の「あ・い・う・え・お」を推定し、母音ごとにF1〜F3の響きを調整して"
                 "声のキャラクターを作ります。手動のF1〜F3設定は基本値としてそのまま維持され、"
                 "その上に母音別の補正が乗ります。オフ=従来どおり。"), ak::Tone::pink);
        addAeiouRow();
        auto& rAmount = slider (*cardFormant, "vamount", "AEIOU Amount (%)",
            tip ("Strength of the selected character's per-vowel offsets. 0 % = identical "
                 "to the feature being off, 100 % = the character map as designed, up to "
                 "200 % emphasizes it further (larger internal limits apply above 100 %).",
                 "選択したキャラクター補正の強さ。0%=機能オフと完全に同じ音、"
                 "100%=設計どおりのキャラクター、200%まで上げるとさらに強調されます"
                 "(100%超は内部上限を広げて適用)。"), ak::Tone::pink);
        rAmount.setTree (ParamRow::Tree::last);

        // -- column 3 --------------------------------------------------
        presetBar = std::make_unique<PresetBar> (proc,
                        [this] (const juce::String& s) { flashFooter (s); });
        addAndMakeVisible (*presetBar);      // lives in the band, not the grid
        presetBar->setDarkStyle();

        cardAir = &newCard ("AIR", "ui_mark_M_Air_png", ak::headBlue);
        rowAir = &knob (*cardAir, "air", "Air",
            tip ("Preserves the natural breath and aperiodic detail of the voice while suppressing "
                 "old-pitch harmonic leakage. Up to 1.0 the preserved amount increases at natural "
                 "loudness; from 1.0 to 1.5 the preserved air is also emphasized. 0 = off.",
                 "声に含まれる自然な息や非周期成分を保ちながら、元のピッチ成分が重なって聞こえる"
                 "ゴーストを抑えます。1.0までは自然な音量のまま保持量が増え、1.0〜1.5では"
                 "息成分を強調します。0=オフ。"), ak::Tone::yellow);
        slider (*cardAir, "airshine", "Air Shine (dB)",
            tip ("Adds high-frequency openness and air above the preserved natural breath. Only "
                 "the highest air band (above ~6 kHz) comes back louder; the mids and the "
                 "harmonic body are untouched. Try 2-4 dB.",
                 "Natural Airの高域に抜け感と明るさを加えます。約6kHz以上の空気感だけが"
                 "持ち上がり、中音域や声の芯には触れません。まずは2〜4dBがおすすめ。"), ak::Tone::yellow);

        // The star art ships gold; recoloured so it matches PITCH and the
        // rest of the blue heading column.
        cardQuality = &newCard ("VOICE QUALITY", "ui_mark_M_VoiceQuality_png",
                                ak::headBlue, ak::markBlue);
        slider (*cardQuality, "tilt", "Softness / Tilt (dB)",
            tip ("Spectral tilt of the voice. + is softer and warmer, - is brighter and more present. "
                 "Start around +/-2 dB.",
                 "音色の傾き。+で柔らかく暖かい声、-で明るく張りのある声。±2dB程度から。"));
        slider (*cardQuality, "jitter", "Natural Jitter",
            tip ("Adds tiny natural pitch fluctuations to reduce the 'machine' feel. Try around 0.1.",
                 "ごく小さな音程の揺らぎを加え、変換の機械っぽさを和らげます。0.1前後から。"));

        // -- bottom row ------------------------------------------------
        cardHigh = &newCard ("HIGH RANGE / LOW LIMIT", "ui_mark_M_HighRange_png", ak::headBlue);
        slider (*cardHigh, "hifreq", "High Range Start (Hz)",
            tip ("When your INPUT pitch (before conversion) rises above this - laughing, squealing, "
                 "exclamations - the Pitch/Formant shifts blend smoothly toward the High amounts "
                 "below, reaching them fully one octave up. Stops laughs from being shifted into "
                 "unnaturally high tones. 0 = off. Try 250-350 Hz.",
                 "入力(変換前)のピッチがこの値を超えると(笑い声・叫び・感嘆など)、ピッチ/"
                 "フォルマントの変化量が下のHigh設定へ滑らかに移行し、1オクターブ上で完全に"
                 "切り替わります。笑い声が不自然な高音まで上がるのを防ぎます。0=オフ。"
                 "250〜350Hzが目安。"));
        slider (*cardHigh, "hipitch", "High Pitch Amount (%)",
            tip ("How much of the Pitch shift remains in the high range. 100% = same as normal, "
                 "0% = no shift there (laughs keep their natural pitch). Try 30-60%.",
                 "高音域で残すPitchシフトの割合。100%=通常と同じ、0%=シフトなし(笑い声は"
                 "地声の高さのまま)。30〜60%が目安。"));
        slider (*cardHigh, "hiformant", "High Fmt Amount (%)",
            tip ("How much of the Formant shift remains in the high range. Usually leave at 100% "
                 "so the voice keeps its character while only the pitch settles down.",
                 "高音域で残すFormantシフトの割合。通常は100%のまま(声色は保ちつつピッチだけ"
                 "落ち着かせる)が自然です。"));
        slider (*cardHigh, "pitchfloor", "Low Pitch Floor (Hz)",
            tip ("If the converted pitch falls below this, it is lifted softly toward the floor. "
                 "Useful when your voice drifts too low while speaking. 0 = off. "
                 "Try 140-180 with a female target voice.",
                 "変換後のピッチがこの値を下回ったとき、滑らかに引き上げます。"
                 "話しているうちに声が低くなりすぎる場合の補正用。0=オフ。"
                 "女声化なら140〜180が目安です。"));

        cardOutput = &newCard ("OUTPUT", "ui_mark_M_Output_png", ak::headBlue);
        slider (*cardOutput, "gain", "Gain (dB)",
            tip ("Output level of the plugin, to compensate loudness changes from the conversion.",
                 "プラグインの出力レベル。変換で音量感が変わったときの補正用。"));
        slider (*cardOutput, "mix", "Mix",
            tip ("Balance between the converted voice (1.0) and the original (0.0). Usually 1.0.",
                 "変換した声(1.0)と元の声(0.0)の割合。通常は1.0のままにします。"));
        cardOutput->add (outMeter, 24);
        cardOutput->add (status, 30);   // latency read-out (v0.35.0: no donut)
    }

    // marks a run of consecutive rows as one bracketed group
    void bracket (std::initializer_list<ParamRow*> group)
    {
        int i = 0;
        const int n = (int) group.size();
        for (auto* r : group)
            r->setTree (++i == n ? ParamRow::Tree::last : ParamRow::Tree::mid);
    }

    // AEIOU character dropdown, then DETAIL... on its own line beneath it
    void addAeiouRow()
    {
        aeiouCombo = std::make_unique<ParamRow> (proc, "vcharacter", ParamRow::Kind::combo,
            "Character",
            tip ("Choose the voice character:\n"
                 "Natural - natural feminine balance / Soft - soft and rounded / "
                 "Active - bright and energetic / Loli - small and youthful / "
                 "Anime - exaggerated vowel contrast / Lily - clear, sweet feminine / "
                 "Elegant - calm and refined / Uni - neutral and androgynous / "
                 "Custom - your own A-I-U-E-O map (DETAIL...).",
                 "声のキャラクターを選びます:\n"
                 "Natural=自然な女性声 / Soft=柔らかい声 / Active=元気な声 / "
                 "Loli=幼な声 / Anime=アニメ声 / Lily=百合声 / Elegant=お姉さん声 / "
                 "Uni=中性声 / Custom=詳細モード(DETAIL...の母音別設定を使用)。"),
            ak::Tone::pink);
        aeiouCombo->setLookAndFeel (&lnfPink);
        aeiouCombo->onLockChanged = [this] { syncLockUI(); };
        aeiouCombo->setTree (ParamRow::Tree::mid);
        rows.push_back (aeiouCombo.get());
        cardFormant->add (*aeiouCombo, 32);

        button (*cardFormant, "Vowel Detail", "DETAIL...",
            tip ("Opens a window to view and edit the per-vowel F1-F3 settings. Built-in "
                 "Characters are shown read-only; \"Copy to Custom\" makes them editable.",
                 "母音別のF1〜F3設定を確認・編集するウィンドウを開きます。内蔵Characterは"
                 "読み取り専用で、「Copy to Custom」でCustomへコピーすると編集できます。"),
            [this]
            {
                if (aeiouWin == nullptr) aeiouWin = std::make_unique<AEIOUCharacterWindow> (proc);
                else { aeiouWin->setVisible (true); aeiouWin->toFront (true); }
            }, ak::Tone::pink).setTree (ParamRow::Tree::mid);
    }

    // ---- VISUALIZER page -------------------------------------------------
    void buildVisualizerPage()
    {
        specData.addView (&spectrum);        // one FFT pair feeds both views
        specData.addView (&radial);
        vizPage.addAndMakeVisible (spectrum);
        vizPage.addAndMakeVisible (radial);
        vizPage.addAndMakeVisible (vowel);
        vizPage.addAndMakeVisible (levels);
        vizNote.setFont (ak::font (11.5f));
        vizNote.setColour (juce::Label::textColourId, ak::heading.withAlpha (0.9f));
        vizNote.setJustificationType (juce::Justification::centredLeft);
        vizNote.setText (juce::String::fromUTF8 (
            "上: 入力(ミント)と変換後(ピンク)のスペクトラム。 "
            "下: 同じスペクトラムの円形表示 / AEIOU母音率 / 入出力レベル(L・R)。"),
            juce::dontSendNotification);
        vizPage.addAndMakeVisible (vizNote);

        vizPage.fn = [this]
        {
            auto r = vizPage.getLocalBounds();
            vizNote.setBounds (r.removeFromTop (22));
            r.removeFromTop (4);
            spectrum.setBounds (r.removeFromTop (juce::jmax (180, r.getHeight() * 45 / 100)));
            r.removeFromTop (ak::kGap);
            const int side = juce::jmin (r.getHeight(), r.getWidth() / 3 - ak::kGap);
            auto strip = r.withHeight (juce::jmax (140, side));
            const int w = (strip.getWidth() - 2 * ak::kGap) / 3;
            radial.setBounds (strip.removeFromLeft (w));
            strip.removeFromLeft (ak::kGap);
            vowel .setBounds (strip.removeFromLeft (w));
            strip.removeFromLeft (ak::kGap);
            levels.setBounds (strip);
        };
    }

    // ---- MAIN page grid --------------------------------------------------
    // Three columns, no bottom row (v0.35.0):
    //   1  PITCH / INTONATION / HIGH RANGE / VOICE QUALITY
    //   2  FORMANT              (pushed down past the character's bulge)
    //   3  AIR / ADVANCED / OUTPUT
    void layoutMainPage()
    {
        mainScroll.setBounds (pageArea);
        const int vw = juce::jmax (kMinW - 40, mainScroll.getMaximumVisibleWidth());

        const int c2Top = juce::jmax (0, bulgeBottom + 10 - pageArea.getY());
        auto stack = [] (std::initializer_list<ak::Card*> cs)
        {
            int h = 0;
            for (auto* c : cs) h += c->preferredHeight() + ak::kGap;
            return juce::jmax (0, h - ak::kGap);
        };
        const int col1 = stack ({ cardPitch, cardInton, cardHigh, cardQuality });
        const int col2 = c2Top + cardFormant->preferredHeight();
        const int col3 = stack ({ cardAir, cardAdvanced, cardOutput });

        const int footerH = 22;
        mainPage.setSize (vw, juce::jmax (juce::jmax (col1, juce::jmax (col2, col3)) + footerH + 10,
                                          pageArea.getHeight()));

        auto r = mainPage.getLocalBounds();
        footer.setBounds (r.removeFromBottom (footerH + 4).withTrimmedTop (4));

        const int usable = r.getWidth() - 2 * ak::kGap;
        const int w1 = juce::jmax (250, (int) ((float) usable * 0.92f / 3.16f));
        const int w2 = usable - w1 * 2;

        auto c1 = r.removeFromLeft (w1);   r.removeFromLeft (ak::kGap);
        auto c2 = r.removeFromLeft (w2);   r.removeFromLeft (ak::kGap);
        auto c3 = r;

        auto place = [] (juce::Rectangle<int>& col, ak::Card* card)
        {
            card->setBounds (col.removeFromTop (card->preferredHeight()));
            col.removeFromTop (ak::kGap);
        };
        place (c1, cardPitch);  place (c1, cardInton);
        place (c1, cardHigh);   place (c1, cardQuality);

        c2.removeFromTop (c2Top);            // clear the character's bulge
        cardFormant->setBounds (c2.removeFromTop (cardFormant->preferredHeight()));

        place (c3, cardAir);  place (c3, cardAdvanced);  place (c3, cardOutput);
    }

    // ---- misc ------------------------------------------------------------
    void syncLockUI()
    {
        for (auto* r : rows) r->refreshLock();
        lastLockState = proc.lockedIds.joinIntoString (",");
    }

    void flashFooter (const juce::String& msg)
    {
        footer.setText (msg, juce::dontSendNotification);
        footer.setColour (juce::Label::textColourId, juce::Colour (0xff5a9c7f));
        startTimer (2600);
    }

    void timerCallback() override
    {
        footer.setText (defaultFooterText, juce::dontSendNotification);
        footer.setColour (juce::Label::textColourId, ak::heading.withAlpha (0.85f));
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

    // a component that forwards resized() to a lambda
    struct LayoutBox : public juce::Component
    {
        std::function<void()> onLayout;
        void resized() override { if (onLayout) onLayout(); }
    };

    static constexpr int kPresetH = 62;

    // ---- members ---------------------------------------------------------
    VoxMorphProcessor& proc;
    TipLookAndFeel  tipLnf;
    ak::ToneLookAndFeel lnfBlue   { ak::Tone::blue };
    ak::ToneLookAndFeel lnfPink   { ak::Tone::pink };
    ak::ToneLookAndFeel lnfYellow { ak::Tone::yellow };
    juce::TooltipWindow tooltipWindow { this, 380 };

    HeaderBar header { proc };
    juce::OwnedArray<ak::TabButton> navButtons;
    HeroCircle hero { proc };
    juce::Rectangle<int> pageArea, bandArea;
    juce::Path bandShape;
    int bulgeBottom = 0;
    juce::Point<float> heroC;
    float heroOuter = 0.0f;
    static constexpr int kLeftTabs = 3;
    std::vector<juce::Component*> pages;
    int currentPage = 0;

    // MAIN page
    juce::Viewport  mainScroll;
    juce::Component mainPage;
    std::vector<std::unique_ptr<ak::Card>> cards;
    std::vector<std::unique_ptr<juce::Component>> owned;
    std::vector<ParamRow*> rows;
    ak::Card* cardPitch = nullptr; ak::Card* cardInton = nullptr; ak::Card* cardAdvanced = nullptr;
    ak::Card* cardFormant = nullptr; ak::Card* cardAir = nullptr; ak::Card* cardQuality = nullptr;
    ak::Card* cardHigh = nullptr;  ak::Card* cardOutput = nullptr;
    ParamRow* rowPitch = nullptr; ParamRow* rowFormant = nullptr; ParamRow* rowAir = nullptr;
    LayoutBox aeiouRow;
    std::unique_ptr<ParamRow> aeiouCombo;
    juce::TextButton detailBtn;
    std::unique_ptr<PresetBar> presetBar;
    StatusView  status   { proc };
    OutputMeter outMeter { proc };
    juce::Label footer;
    juce::String defaultFooterText;

    // VISUALIZER page
    FnComponent        vizPage;
    juce::Label        vizNote;
    SpectrumData       specData { proc };
    SpectrumView       spectrum { specData };
    RadialSpectrumView radial   { specData };
    VowelDonut         vowel    { proc };
    LevelRingsDonut    levels   { proc };

    // other pages
    MatchingPanel matchingPanel { proc };
    AsmrPanel     asmrPanel     { proc };
    PresetPanel   presetPanel   { proc };

    // child windows
    std::unique_ptr<AEIOUCharacterWindow> aeiouWin;
    std::unique_ptr<BetaWindow>           betaWin;

    FnTimer histPoll;
    juce::String lastLockState;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VoxMorphEditor)
};
