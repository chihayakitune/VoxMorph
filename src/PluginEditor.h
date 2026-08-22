#pragma once
#include "PluginProcessor.h"
#include "VoiceAnalyzer.h"
#include "MatchingEngine.h"
#include "SampleTargetCatalog.h"
#include "AnokoeWidgets.h"
#include "FmtCharacterPresets.h"

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

// Shared spectrum analysis (v0.30.1). Each frame is two 4096-point FFTs, so
// any view that wants the numbers registers itself here instead of analysing
// its own copy. v0.30.1 - v0.45.0 had a second (radial) view sharing them;
// only SpectrumView is left, and the registration stays because the cost that
// motivated it has not changed — and because the gate below is what tells the
// audio thread to stop filling the taps once nothing is on screen.
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

// The VISUALIZER's frequency axis, and the ONE place its horizontal geometry
// lives (v0.47.0). The detection lane below the spectrum is a separate
// component of the same width and x, and its markers only line up with the
// curve above because both ask this for the same numbers — an inset that
// existed only inside SpectrumView::paint would drift them apart silently,
// and a marker that is a few pixels off the peak it names is worse than no
// marker at all.
namespace vmAxis
{
    constexpr double kLoHz = 20.0, kHiHz = 20000.0;   // 3 decades
    constexpr float  kCardInset = 2.0f;               // card edge
    constexpr float  kPlotInset = 10.0f;              // plot inside the card

    // the x range the curve occupies, for a component of these bounds
    inline juce::Range<float> span (juce::Rectangle<int> local)
    {
        const float in = kCardInset + kPlotInset;
        return { (float) local.getX() + in, (float) local.getRight() - in };
    }

    inline float xFor (juce::Range<float> s, double hz)
    {
        const double t = std::log10 (juce::jmax (kLoHz, hz) / kLoHz) / 3.0;
        return s.getStart() + s.getLength() * (float) juce::jlimit (0.0, 1.0, t);
    }

    inline bool visible (double hz) { return hz >= kLoHz && hz <= kHiHz; }
}

// Spectrum visualizer: INPUT (blue) and converted OUTPUT (pink) spectra
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
            // Each column is placed at the frequency it actually analysed,
            // through the shared axis — the same mapping the grid lines and
            // the detection lane use. It used to be a plain c / (kCols - 1),
            // which is the same curve stretched by one column; that is
            // invisible on its own but it would put every marker in the lane
            // a pixel or two off the lobe it names.
            const float x = vmAxis::xFor ({ r.getX(), r.getRight() },
                                          vmAxis::kLoHz * std::pow (1000.0, (double) c / (double) kCols));
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
        auto b = getLocalBounds().toFloat().reduced (vmAxis::kCardInset);
        ak::paintCard (g, b);

        auto r = b.reduced (vmAxis::kPlotInset, vmAxis::kPlotInset);
        const auto sp = vmAxis::span (getLocalBounds());
        g.setColour (juce::Colour (0x12000000));                   // grid
        g.setFont (juce::Font (juce::FontOptions (10.0f)));
        for (double f : { 100.0, 1000.0, 10000.0 })
        {
            const float x = vmAxis::xFor (sp, f);
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

        drawCurve (g, data.in(),  r, ak::seriesIn);   // input: blue
        drawCurve (g, data.out(), r, ak::seriesOut);   // output: pink

        g.setFont (juce::Font (juce::FontOptions (11.0f)));        // legend
        g.setColour (ak::seriesIn);
        g.drawText ("Input",  (int) r.getRight() - 110, (int) r.getY() + 2, 50, 14, juce::Justification::left);
        g.setColour (ak::seriesOut);
        g.drawText ("Output", (int) r.getRight() - 56,  (int) r.getY() + 2, 54, 14, juce::Justification::left);
    }

    const SpectrumData& data;
};

// ---------------------------------------------------------------------------
// Detection lane, v0.47.0 — a short strip directly under the spectrum, on the
// SAME width and the SAME frequency axis (vmAxis), so everything drawn here
// sits at the x of the thing it describes in the curve above.
//
// Two rows: what came IN (blue, upper) and what is going OUT (pink, lower),
// with a connector between each pair so the direction and size of the move
// read at a glance. On each row:
//   * f0        the pitch, as a diamond. Output f0 is what the grain is
//               really emitted at, so Intonation, the Low Limit and the High
//               Range guard are all already in it.
//   * F1-F3     the tracked formants, as dots.
//   * a faint harmonic comb from f0, which is what makes the picket fence in
//     the spectrum above — input comb under the input row, output comb over
//     the output row.
// Plus dashed verticals for the frequency-valued PARAMETERS: Low Limit,
// High Range Start and the 6 kHz edge Air Shine works above.
//
// Every number is the engine's own — republished from its analysis taps, in
// the same way as the vowel coordinate. Nothing here re-estimates anything:
// a second estimator that disagrees with the DSP is exactly the trap v0.28.3
// and v0.28.4 fell into.
//
// The formant row is EMPTY unless the engine's spectral layer ran, because
// then it has not measured a formant. That layer is skipped unless a formant
// feature is on, and switching it on for the sake of a display would change
// the CPU cost and the output. The lane says "F1-F3 not tracked" instead.
class DetectionLane : public juce::Component, public juce::SettableTooltipClient,
                      private juce::Timer
{
public:
    explicit DetectionLane (VoxMorphProcessor& p) : proc (p)
    {
        setTooltip (vmTip (
            "What the engine is detecting, on the same frequency axis as the spectrum "
            "directly above - so a marker sits under the peak it belongs to. The upper "
            "blue row is your voice going in, the lower pink row is the converted voice "
            "coming out, and the sloped line between a pair shows how far that feature "
            "moved. The diamond is the pitch f0 (the output one already includes "
            "Intonation, Low Limit and the High Range guard) and the dots are the "
            "formants F1-F3; the fine ticks are the harmonics of f0, which are the comb "
            "you can see in the spectrum. The dashed verticals are your frequency "
            "settings: Low Limit, High Range Start and the 6 kHz edge Air Shine lifts "
            "above. F1-F3 only appear while a formant feature is running (F1-F3 Shift or "
            "Gain, AEIOU Character, or Formant Definition) - the engine skips its "
            "formant analysis entirely when none of them is on, and turning it on just "
            "to draw this would cost CPU and change the sound. A marker labelled F2-F3 "
            "means those two landed on the same resonance and the engine is only holding "
            "them apart by its minimum spacing - that is one reading, not two, and it "
            "happens on vowels where the two really do merge into a single hump. A HOLLOW "
            "marker with a question mark means the engine did not find that formant in "
            "the last moment and is holding its previous value, and when it cannot find "
            "it at all the marker is left out rather than drawn on a default. High "
            "voices lose F1 first: above roughly 250 Hz the fundamental is already close "
            "to where F1 sits, so there is no separate peak left to measure.",
            "エンジンがいま検出している位置を、真上のスペクトラムと同じ周波数軸で表示します"
            "(マーカーが対応する山の真下に来ます)。上の青い行が入力、下のピンクの行が変換後で、"
            "2つを結ぶ斜めの線がその成分の移動量です。ひし形がピッチf0(出力側はIntonation・"
            "Low Limit・High Rangeガードを通した後の実際の値)、丸がフォルマントF1〜F3です。"
            "細かい目盛りはf0の倍音で、スペクトラムに見える櫛状の山がこれにあたります。"
            "破線は周波数に関わる設定値(Low Limit / High Range Start / Air Shineが効く6kHz)です。"
            "F1〜F3は、フォルマント系の機能(F1〜F3 Shift/Gain、AEIOU Character、"
            "Formant Definition)のいずれかが動作している間だけ表示されます。どれもオフのときは"
            "エンジンがフォルマント解析自体を省略しており、表示のためだけに動かすとCPUが増えて"
            "音も変わってしまうためです。「F2·F3」のように連結表示されたマーカーは、"
            "その2つが同じ共鳴を指しており、エンジンが最小間隔で引き離しているだけの状態です"
            "(2つではなく1つの測定値)。実際に2つの山が1つに融合している母音で起こります。"
            "中抜きのマーカーと「?」は、直前の瞬間にそのフォルマントを見つけられず"
            "前の値を保持している状態です。まったく見つけられない時はマーカー自体を出しません"
            "(既定値の位置に点を打たないため)。高い声ではF1から失われます: f0が250Hzを超えると"
            "基音がF1の位置に近づき、分離した山が残らないためです。"));
        startTimerHz (30);
    }

    // the lane needs this much height to hold two rows plus its labels
    static constexpr int kHeight = 66;

private:
    static constexpr int kN = 4;                     // f0, F1, F2, F3
    static constexpr const char* kLbl[kN] = { "f0", "F1", "F2", "F3" };

    void timerCallback() override
    {
        if (const int hz = vmDrawHz (proc, 30); hz != rateHz)
        { rateHz = hz; startTimerHz (hz); }   // Performance Mode
        if (! isShowing()) return;

        bool moved = false;
        const bool fv = proc.uiFmtValid.load (std::memory_order_relaxed);
        float wantIn[kN], wantOut[kN];
        wantIn [0] = proc.uiF0In .load (std::memory_order_relaxed);
        wantOut[0] = proc.uiF0Out.load (std::memory_order_relaxed);
        for (int i = 0; i < 3; ++i)
        {
            wantIn [i + 1] = fv ? proc.uiFmtIn [i].load (std::memory_order_relaxed) : 0.0f;
            wantOut[i + 1] = fv ? proc.uiFmtOut[i].load (std::memory_order_relaxed) : 0.0f;
            const bool mg = fv && proc.uiFmtMerged[i].load (std::memory_order_relaxed);
            if (mg != merged[i + 1]) { merged[i + 1] = mg; moved = true; }
            // Hold the confidence through a dropout rather than snapping it
            // to 0: the spectral layer only runs on voiced grains, so every
            // consonant and every breath makes formantsValid() false for a
            // moment. Zeroing it there is what made F1-F3 blink out while f0
            // sat still -- f0 never blinks because voiced detection does not.
            // The alpha release below is what actually fades them.
            if (fv)
            {
                const float cf = proc.uiFmtConf[i].load (std::memory_order_relaxed);
                if (std::abs (cf - conf[i + 1]) > 0.01f) { conf[i + 1] = cf; moved = true; }
            }
        }

        for (int i = 0; i < kN; ++i)
        {
            // A marker is live only when BOTH ends of its pair are known:
            // drawing one end of a connector is worse than drawing neither.
            const bool live = wantIn[i] > 20.0f && wantOut[i] > 20.0f;
            if (live)
            {
                moved = glide (hzIn [i], wantIn [i]) || moved;
                moved = glide (hzOut[i], wantOut[i]) || moved;
            }
            // Fade rather than blink, and fade SLOWLY: speech is full of
            // short unvoiced gaps, and at 0.12 per frame (~0.3 s) the
            // formant markers were gone before they could be read. Attack
            // stays quick so a new vowel appears at once.
            const float target = live ? 1.0f : 0.0f;
            const float rate   = target > alpha[i] ? 0.25f : kFadeOut;
            if (std::abs (target - alpha[i]) > 0.004f)
            { alpha[i] += rate * (target - alpha[i]); moved = true; }
            else if (alpha[i] != target)
            { alpha[i] = target; moved = true; if (target == 0.0f) { hzIn[i] = hzOut[i] = 0.0f; } }
        }
        if (fv != fmtValid) { fmtValid = fv; moved = true; }
        if (moved) repaint();
    }

    // Glide in LOG frequency, so a marker moves at the same apparent speed
    // wherever it sits on the axis — the same 40 Hz step is a third of an
    // octave down at f0 and a rounding error up at F3. Returns whether it
    // actually moved, so a still picture stops repainting.
    static bool glide (float& cur, float want)
    {
        if (cur < 20.0f) { cur = want; return true; }
        const float n = std::exp2 (std::log2 (cur)
                                   + 0.35f * (std::log2 (want) - std::log2 (cur)));
        if (std::abs (n - cur) < 0.15f) return false;
        cur = n;
        return true;
    }

    float param (const char* id) const
    {
        if (auto* v = proc.apvts.getRawParameterValue (id))
            return v->load (std::memory_order_relaxed);
        return 0.0f;
    }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat().reduced (vmAxis::kCardInset);
        ak::paintCard (g, b);
        const auto sp = vmAxis::span (getLocalBounds());
        if (sp.getLength() < 80.0f || b.getHeight() < 40.0f) return;

        const float yIn  = b.getY() + b.getHeight() * 0.34f;
        const float yOut = b.getY() + b.getHeight() * 0.72f;

        // ---- the frequency-valued parameters, behind everything ----------
        {
            const float dash[2] = { 3.0f, 3.5f };
            auto vline = [&] (double hz, const char* name)
            {
                if (! vmAxis::visible (hz)) return;
                const float x = vmAxis::xFor (sp, hz);
                g.setColour (ak::treeLine);
                g.drawDashedLine ({ x, b.getY() + 4.0f, x, b.getBottom() - 12.0f }, dash, 2, 1.0f);
                g.setColour (ak::heading.withAlpha (0.65f));
                g.setFont (ak::font (8.5f));
                g.drawText (name, (int) x + 3, (int) b.getBottom() - 13, 74, 11,
                            juce::Justification::left, false);
            };
            const float lo = param ("pitchfloor"), hi = param ("hifreq");
            if (lo > 20.0f) vline (lo, "Low Limit");
            if (hi > 20.0f) vline (hi, "High Range");
            if (param ("airshine") > 0.01f) vline (6000.0, "Air Shine");
        }

        // ---- harmonic combs: the picket fence in the curve above ---------
        auto comb = [&] (float f0, float y, float dir, juce::Colour col, float a)
        {
            if (f0 < 20.0f || a < 0.02f) return;
            g.setColour (col.withAlpha (0.20f * a));
            float prevX = -1.0e9f;
            for (int k = 2; k <= 80; ++k)
            {
                const double hz = (double) f0 * k;
                if (hz > vmAxis::kHiHz) break;
                const float x = vmAxis::xFor (sp, hz);
                // The log axis crowds the upper harmonics together; past the
                // point where consecutive ticks are less than 3 px apart they
                // stop being ticks and become a grey smear that reads as a
                // filled band. Stop there rather than drawing a lie about
                // how far up the comb is resolvable.
                if (x - prevX < 3.0f) break;
                prevX = x;
                g.fillRect (juce::Rectangle<float> (x - 0.5f, y + dir * 6.0f, 1.0f, 4.0f));
            }
        };
        comb (hzIn[0],  yIn,  1.0f, ak::seriesIn,  alpha[0]);
        comb (hzOut[0], yOut, -1.0f, ak::seriesOut, alpha[0]);

        // ---- the pairs: in marker, connector, out marker ------------------
        for (int i = 0; i < kN; ++i)
        {
            // merged[i] means slot i and slot i-1 came from the SAME peak and
            // are only held apart by the engine's ordering clamp. Draw the
            // pair once, on the LOWER one (which carries the combined label),
            // rather than two confident dots at a separation nothing
            // measured -- so it is the upper slot that is skipped here.
            if (merged[i]) continue;
            if (alpha[i] < 0.02f || hzIn[i] < 20.0f || hzOut[i] < 20.0f) continue;
            // Below kConfHide the published frequency is the band's default
            // constant, not a measurement, and drawing a dot on it is simply
            // a lie -- on a 251-317 Hz voice F1 reads 495 Hz (= defR[0]) on
            // every vowel. Between the two thresholds the value did come
            // from a peak recently but is now being held, which is worth
            // showing as long as it is shown as held: hollow, and labelled
            // with a question mark.
            // A smooth gate, not a step: crossing the threshold should fade
            // the marker, not switch it off between two frames.
            const float cg = juce::jlimit (0.0f, 1.0f,
                                (conf[i] - kConfHide) / (kConfSolid - kConfHide));
            if (alpha[i] * cg < 0.02f) continue;
            const bool solid = conf[i] >= kConfSolid;
            if (! vmAxis::visible (hzIn[i]) && ! vmAxis::visible (hzOut[i])) continue;
            const float xi = vmAxis::xFor (sp, hzIn[i]);
            const float xo = vmAxis::xFor (sp, hzOut[i]);
            const float a  = alpha[i] * cg;

            g.setGradientFill (juce::ColourGradient (ak::seriesIn .withAlpha (0.55f * a), xi, yIn,
                                                     ak::seriesOut.withAlpha (0.55f * a), xo, yOut,
                                                     false));
            g.drawLine (xi, yIn + 5.0f, xo, yOut - 5.0f, 1.2f);

            marker (g, xi, yIn,  ak::seriesIn,  a, i == 0, solid);
            marker (g, xo, yOut, ak::seriesOut, a, i == 0, solid);

            // "F2·F3" when the two are one reading -- the dot is drawn hollow
            // for the same reason, so a merged pair never looks like a
            // measurement it is not
            g.setColour (ak::ink.withAlpha ((solid ? 0.75f : 0.5f) * a));
            g.setFont (ak::font (9.0f, i == 0));
            g.drawText (label (i) + (solid ? juce::String() : juce::String (" ?")),
                        juce::Rectangle<float> (40.0f, 11.0f)
                            .withCentre ({ xi, yIn - 11.0f }).toNearestInt(),
                        juce::Justification::centred, false);
        }

        // ---- row tags, and the honest note when formants are not tracked --
        // Both live at the far right, above 15 kHz: f0 and F1-F3 never reach
        // there, so nothing can collide with them.
        g.setFont (ak::font (9.0f, true));
        g.setColour (ak::seriesIn.withAlpha (0.9f));
        g.drawText ("IN",  (int) sp.getEnd() - 22, (int) yIn - 6, 22, 12,
                    juce::Justification::right, false);
        g.setColour (ak::seriesOut.withAlpha (0.9f));
        g.drawText ("OUT", (int) sp.getEnd() - 22, (int) yOut - 6, 22, 12,
                    juce::Justification::right, false);

        if (! fmtValid)
        {
            g.setColour (ak::heading.withAlpha (0.75f));
            g.setFont (ak::font (9.5f));
            g.drawText ("F1-F3 not tracked", (int) sp.getEnd() - 152, (int) b.getY() + 3, 128, 12,
                        juce::Justification::right, false);
        }
    }

    // f0 is a diamond and the formants are dots, so the two kinds of reading
    // stay apart for anyone who cannot separate the two row colours
    static void marker (juce::Graphics& g, float x, float y, juce::Colour col,
                        float a, bool diamond, bool solid)
    {
        g.setColour (col.withAlpha (solid ? a : 0.85f * a));
        if (diamond)
        {
            juce::Path p;
            p.addQuadrilateral (x, y - 4.6f, x + 4.6f, y, x, y + 4.6f, x - 4.6f, y);
            if (solid) g.fillPath (p);
            else       g.strokePath (p, juce::PathStrokeType (1.3f));
        }
        else
        {
            const auto r = juce::Rectangle<float> (7.0f, 7.0f).withCentre ({ x, y });
            if (solid) g.fillEllipse (r);
            else       g.drawEllipse (r.reduced (0.7f), 1.3f);
        }
    }

    // "F2" normally; "F2·F3" when the slot above it is the same measurement
    juce::String label (int i) const
    {
        if (i + 1 < kN && merged[i + 1])
            return juce::String (kLbl[i]) + juce::String::fromUTF8 ("·") + kLbl[i + 1];
        return kLbl[i];
    }

    VoxMorphProcessor& proc;
    int   rateHz = 30;   // current timer rate; Performance Mode lowers it
    // A hollow marker means "held, not measured"; below kConfHide nothing is
    // drawn at all. The gap between them is deliberately wide: the number is
    // a smoothed proportion of recent grains that found a peak, so anything
    // in between genuinely is intermittent.
    static constexpr float kConfSolid = 0.5f, kConfHide = 0.15f;
    // ~0.75 s to fade out at 30 Hz. Slow on purpose: these markers are read,
    // not glanced at, and the engine's own formant tracking drops out on
    // every unvoiced moment.
    static constexpr float kFadeOut = 0.045f;
    bool  merged[kN] = { false, false, false, false };
    float conf[kN]   = { 1.0f, 0.0f, 0.0f, 0.0f };   // f0 is always measured
    float hzIn[kN]  = { 0.0f, 0.0f, 0.0f, 0.0f };
    float hzOut[kN] = { 0.0f, 0.0f, 0.0f, 0.0f };
    float alpha[kN] = { 0.0f, 0.0f, 0.0f, 0.0f };
    bool  fmtValid = false;
};

// ---------------------------------------------------------------------------
// AEIOU vowel meter, v0.46.0 — five bars whose heights are how much /a/ /i/
// /u/ /e/ /o/ the engine currently hears. (v0.30.2 - v0.45.0 drew this as a
// donut chart; the bars say the same thing in a third of the height and read
// without a legend.)
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
//
// v0.46.0: with no voice arriving the mix now GLIDES BACK TO NEUTRAL (an even
// fifth each) over about a second, the same way the spectrum falls back to
// its floor. Until now it froze on the last shape and only dimmed, and a
// frozen shape at 22 % alpha still reads as a reading — of a vowel nobody is
// saying.
class VowelMeter : public juce::Component, public juce::SettableTooltipClient,
                   private juce::Timer
{
public:
    explicit VowelMeter (VoxMorphProcessor& p) : proc (p)
    {
        setTooltip (vmTip (
            "How much of each Japanese vowel the engine hears in your voice right now, "
            "as a share of the whole. This is the exact vowel mix the AEIOU Character "
            "feature uses to pick its per-vowel formant offsets, so it shows you why a "
            "Character is doing what it is doing. It needs AEIOU Character switched on "
            "(FORMANT section) with Amount above 0 - the vowel tracking is part of that "
            "feature and does not run otherwise. When you stop speaking the bars settle "
            "back to an even mix over about a second, because the estimate is only "
            "meaningful on voiced sound.",
            "いま話している声に含まれる母音(あいうえお)の割合です。AEIOU Character機能が"
            "母音ごとのフォルマント補正を選ぶのに使っている値そのものなので、Characterが"
            "なぜその効き方をしているのかが分かります。表示にはFORMANTセクションの"
            "AEIOU Characterがオンで、Amountが0より大きいことが必要です(母音の推定自体が"
            "この機能の一部で、オフのときは動作しません)。声を出すのをやめると、推定が"
            "無意味になるため約1秒かけて均等(ニュートラル)へ戻ります。"
            "\n\n声を出しているのに「vowel not measurable at this pitch」と出る場合は、"
            "その声のF1/F2が測れていない状態です(高い声ほど起きます: 基音が高いと"
            "フォルマントの位置を分離できるだけの倍音が残りません)。v0.49.0以降、この状態では"
            "AEIOU Characterは補正を適用しません。測れていない母音に自信を持って補正するより、"
            "何もしない方が正しいという判断です。"));
        startTimerHz (30);
    }

private:
    static constexpr int kV = 5;                                  // a i u e o
    static constexpr const char* kLbl[kV] = { "A", "I", "U", "E", "O" };
    static constexpr float kNeutral = 1.0f / (float) kV;   // "nothing to say"
    // A bar is full at this share. Even an unmistakable vowel rarely takes
    // much more than 0.6 of the anchor weight, so scaling to 1.0 would park
    // every bar in the bottom third and waste the panel.
    static constexpr float kFull = 0.62f;
    // A vowel counts as dominant once it is this far clear of an even mix.
    // Without the margin the chip names a "winner" out of rounding noise
    // while the bars are all sitting at neutral — "A 21 %" out of five
    // vowels is not a reading, it is the absence of one.
    static constexpr float kCalled = kNeutral + 0.08f;

    void timerCallback() override
    {
        if (const int hz = vmDrawHz (proc, 30); hz != rateHz)
        { rateHz = hz; startTimerHz (hz); }   // Performance Mode
        if (! isShowing()) return;

        const bool  live = proc.uiVowelActive.load (std::memory_order_relaxed);
        const float conf = proc.uiVowelConf  .load (std::memory_order_relaxed);
        const bool  good = live && conf > 0.02f;

        float w[kV];
        if (good)
            VowelAdaptiveWarp::anchorWeights (proc.uiVowelH.load (std::memory_order_relaxed),
                                              proc.uiVowelF.load (std::memory_order_relaxed), w);
        else
            for (int i = 0; i < kV; ++i) w[i] = kNeutral;

        // rising: ~150 ms, fast enough to follow running speech.
        // falling back to neutral: ~1.1 s, slow enough to read as settling
        // rather than as the chart being switched off.
        const float k = good ? 0.22f : 0.030f;
        bool moved = false;
        for (int i = 0; i < kV; ++i)
        {
            if (std::abs (w[i] - sm[i]) > 0.0015f) { sm[i] += k * (w[i] - sm[i]); moved = true; }
            else                                     sm[i] = w[i];
        }

        // Voiced, the feature on, and still no usable vowel: that is not
        // "nobody is speaking", it is "this voice's formants cannot be
        // measured", and since v0.49.0 it also means AEIOU Character is
        // applying nothing. Neutral bars alone would read as silence.
        const bool voicedNow = proc.uiF0In.load (std::memory_order_relaxed) > 20.0f;
        const bool stuck = live && voicedNow && ! good;
        if (stuck != unmeasurable) { unmeasurable = stuck; moved = true; }

        const float fadeTo = good ? 1.0f : 0.55f;
        if (std::abs (fadeTo - fade) > 0.002f) { fade += 0.15f * (fadeTo - fade); moved = true; }
        else                                     fade = fadeTo;

        if (active != live) { active = live; moved = true; }
        if (moved) repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat().reduced (2.0f);
        ak::paintCard (g, b);
        auto r = b.reduced (14.0f, 10.0f);
        if (r.getWidth() < 90.0f || r.getHeight() < 60.0f) return;

        float sum = 0.0f;
        for (int i = 0; i < kV; ++i) sum += juce::jmax (0.0f, sm[i]);
        if (sum <= 1.0e-6f) sum = 1.0f;
        int top = 0;
        for (int i = 1; i < kV; ++i) if (sm[i] > sm[top]) top = i;
        const float topShare = juce::jmax (0.0f, sm[top]) / sum;
        const bool  called   = active && topShare > kCalled;

        // ---- heading row: the panel's name, and the winner as a chip ------
        auto head = r.removeFromTop (16.0f);
        g.setColour (ak::headBlue);
        g.setFont (ak::font (11.5f, true));
        g.drawText ("VOWEL MIX", head.removeFromLeft (head.getWidth() - 62.0f).toNearestInt(),
                    juce::Justification::centredLeft, false);
        if (called)
        {
            auto chip = head.removeFromRight (58.0f).reduced (0.0f, 1.0f);
            g.setColour (ak::seriesOut.withMultipliedAlpha (fade));
            g.fillRoundedRectangle (chip, chip.getHeight() * 0.5f);
            g.setColour (juce::Colours::white.withMultipliedAlpha (juce::jmax (0.7f, fade)));
            g.setFont (ak::font (10.0f, true));
            g.drawText (juce::String (kLbl[top]) + "  "
                          + juce::String (juce::roundToInt (topShare * 100.0f)) + " %",
                        chip.toNearestInt(), juce::Justification::centred, false);
        }

        r.removeFromTop (7.0f);
        auto letters = r.removeFromBottom (14.0f);
        auto lane    = r;
        if (lane.getHeight() < 20.0f) return;

        // ---- the five bars ------------------------------------------------
        const float cellW = lane.getWidth() / (float) kV;
        const float barW  = juce::jlimit (10.0f, 26.0f, cellW - 12.0f);
        for (int i = 0; i < kV; ++i)
        {
            const float cx = lane.getX() + cellW * ((float) i + 0.5f);
            const auto track = juce::Rectangle<float> (barW, lane.getHeight())
                                   .withCentre ({ cx, lane.getCentreY() });
            const float rad = barW * 0.5f;

            g.setColour (ak::valueFill);
            g.fillRoundedRectangle (track, rad);
            g.setColour (ak::valueLine);
            g.drawRoundedRectangle (track.reduced (0.5f), rad, 1.0f);

            // With the feature off nothing is measuring a vowel, so the bars
            // are left EMPTY rather than parked at a neutral fifth each: a
            // filled bar is a reading, and there is no reading to show.
            const float t = juce::jlimit (0.0f, 1.0f, juce::jmax (0.0f, sm[i]) / kFull);
            if (active && t > 0.02f)
            {
                // A floor is needed because a rounded fill shorter than its
                // own cap radius degenerates into a sliver. Keep it at HALF
                // the bar width, not the full width: at a full width a 6 %
                // vowel was drawn the same height as a 30 % one (measured
                // 0.29 of the lane against a true 0.10), which is the chart
                // lying about the reading it exists to show.
                const float h = juce::jmax (barW * 0.5f, track.getHeight() * t);
                const auto  fill = track.withTop (track.getBottom() - h);
                const bool  win  = called && i == top;
                g.setColour ((win ? ak::seriesOut : ak::seriesIn)
                                 .withMultipliedAlpha (win ? fade : 0.55f + 0.45f * fade));
                g.fillRoundedRectangle (fill, rad);
            }

            g.setColour (called && i == top ? ak::ink : ak::heading.withAlpha (0.85f));
            g.setFont (ak::font (11.0f, called && i == top));
            g.drawText (kLbl[i], juce::Rectangle<float> (cellW, letters.getHeight())
                                     .withCentre ({ cx, letters.getCentreY() }).toNearestInt(),
                        juce::Justification::centred, false);
        }

        // ---- voiced, but the vowel cannot be measured --------------------
        if (active && unmeasurable)
        {
            auto plate = juce::Rectangle<float> (juce::jmin (lane.getWidth(), 210.0f), 30.0f)
                             .withCentre (lane.getCentre());
            g.setColour (juce::Colour (0xe8ffffff));
            g.fillRoundedRectangle (plate, 10.0f);
            g.setColour (ak::line);
            g.drawRoundedRectangle (plate.reduced (0.5f), 10.0f, 1.0f);
            g.setColour (ak::heading);
            g.setFont (ak::font (10.5f));
            g.drawFittedText ("vowel not measurable\nat this pitch", plate.toNearestInt(),
                              juce::Justification::centred, 2);
        }

        // ---- the feature is off: say so, on the empty tracks --------------
        if (! active)
        {
            auto plate = juce::Rectangle<float> (juce::jmin (lane.getWidth(), 200.0f), 30.0f)
                             .withCentre (lane.getCentre());
            g.setColour (juce::Colour (0xe8ffffff));
            g.fillRoundedRectangle (plate, 10.0f);
            g.setColour (ak::line);
            g.drawRoundedRectangle (plate.reduced (0.5f), 10.0f, 1.0f);
            g.setColour (ak::heading);
            g.setFont (ak::font (11.0f));
            g.drawFittedText ("AEIOU Character is off", plate.toNearestInt(),
                              juce::Justification::centred, 2);
        }
    }

    VoxMorphProcessor& proc;
    int rateHz = 30;   // current timer rate; Performance Mode lowers it
    float sm[kV] = { kNeutral, kNeutral, kNeutral, kNeutral, kNeutral };
    float fade = 0.55f;
    bool  active = false;
    bool  unmeasurable = false;
};

// ---------------------------------------------------------------------------
// Level meters, v0.46.0 — the four levels as four slim horizontal bars, which
// is the same shape the OUTPUT card's volume bar uses on MAIN. (v0.30.3 -
// v0.45.0 wrapped them round a dial split L | R; it was handsome and it cost
// a square panel to say what four 16 px rows say.)
//
// Input is measured before the noise gate and the Pre FX (so you can see your
// mic even while the gate has it shut); output is what actually leaves the
// plugin. With a mono bus the two channels of a pair read the same.
class LevelMeters : public juce::Component, public juce::SettableTooltipClient,
                    private juce::Timer
{
public:
    explicit LevelMeters (VoxMorphProcessor& p) : proc (p)
    {
        setTooltip (vmTip (
            "All four level meters: the two INPUT rows are what arrives at VoxMorph and "
            "the two OUTPUT rows are what leaves it, left and right channel each. The "
            "scale is -60 to +6 dB and the pink zone at the right is above 0 dBFS, where "
            "the signal may clip - the thin tick is the recent peak and turns red when it "
            "gets there. The input is measured before the noise gate, so it keeps showing "
            "your mic even while the gate is closed. With a mono input or output the two "
            "rows of that pair read the same.",
            "入力L/R・出力L/Rの4つのレベルです。上2行がVoxMorphに入ってくる音、下2行が"
            "実際に出ていく音で、それぞれ左右のチャンネルです。目盛りは-60〜+6dBで、右端の"
            "ピンクの帯は0dBFSより上=音が割れる可能性がある領域です。細い縦線が直近の"
            "ピークで、そこに達すると赤くなります。入力はノイズゲートより前で測っている"
            "ので、ゲートが閉じている間もマイクの状態が分かります。モノラルの場合はその"
            "ペアの2行が同じ値になります。"));
        startTimerHz (30);
    }

private:
    static constexpr int   kR      = 4;                // in L, in R, out L, out R
    static constexpr float kSpan   = 66.0f;            // -60 dB .. +6 dB
    static constexpr float kZeroDb = 60.0f / kSpan;    // where 0 dBFS lands

    static float pos (float lin)                       // linear -> 0..1
    {
        const float db = juce::Decibels::gainToDecibels (lin, -60.0f);
        return juce::jlimit (0.0f, 1.0f, (db + 60.0f) / kSpan);
    }

    void timerCallback() override
    {
        if (const int hz = vmDrawHz (proc, 30); hz != rateHz)
        { rateHz = hz; startTimerHz (hz); }   // Performance Mode
        if (! isShowing()) return;
        // order must match the labels in paint(): in L, in R, out L, out R
        const VoxMorphProcessor::LevelMeter* src[kR] = {
            &proc.uiInL, &proc.uiInR, &proc.uiOutL, &proc.uiOutR };
        bool changed = false;
        for (int i = 0; i < kR; ++i)
        {
            const float pk = src[i]->peak.load (std::memory_order_relaxed);
            const float rm = src[i]->rms .load (std::memory_order_relaxed);
            const float r  = pos (rm), p = pos (pk);
            if (r != lvl[i] || p != pkPos[i]) changed = true;
            lvl[i] = r;  pkPos[i] = p;
            db[i]  = juce::Decibels::gainToDecibels (rm, -100.0f);
            clip[i] = juce::Decibels::gainToDecibels (pk, -60.0f) >= -0.1f;
        }
        if (changed) repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat().reduced (2.0f);
        ak::paintCard (g, b);
        auto r = b.reduced (14.0f, 10.0f);
        if (r.getWidth() < 150.0f || r.getHeight() < 60.0f) return;

        g.setColour (ak::headBlue);
        g.setFont (ak::font (11.5f, true));
        g.drawText ("LEVELS", r.removeFromTop (16.0f).toNearestInt(),
                    juce::Justification::centredLeft, false);
        r.removeFromTop (4.0f);

        auto scale = r.removeFromBottom (12.0f);
        const float rowH = juce::jmax (13.0f, r.getHeight() / (float) kR);
        const float labW = 46.0f, valW = 50.0f;

        static const char* names[kR] = { "IN  L", "IN  R", "OUT L", "OUT R" };
        juce::Rectangle<float> lastTrack;
        for (int i = 0; i < kR; ++i)
        {
            auto row = r.removeFromTop (rowH);
            g.setColour (ak::heading.withAlpha (0.9f));
            g.setFont (ak::font (10.0f, i == 0 || i == 2));
            g.drawText (names[i], row.removeFromLeft (labW).toNearestInt(),
                        juce::Justification::centredLeft, false);

            auto val = row.removeFromRight (valW);
            auto track = row.withTrimmedRight (6.0f)
                            .withSizeKeepingCentre (row.getWidth() - 6.0f,
                                                    juce::jmin (10.0f, rowH - 4.0f));
            lastTrack = track;
            drawTrack (g, track, i);

            g.setColour (ak::ink.withAlpha (0.85f));
            g.setFont (ak::font (10.0f));
            g.drawText (db[i] <= -59.5f ? juce::String ("--")
                                        : juce::String (db[i], 1),
                        val.toNearestInt(), juce::Justification::centredRight, false);
        }

        // ---- the scale, under the bars and aligned to them ----------------
        if (! lastTrack.isEmpty())
        {
            g.setColour (ak::heading.withAlpha (0.6f));
            g.setFont (ak::font (9.0f));
            for (const float mark : { -60.0f, -40.0f, -20.0f, 0.0f })
            {
                const float x = lastTrack.getX()
                              + lastTrack.getWidth() * ((mark + 60.0f) / kSpan);
                g.drawText (mark == 0.0f ? juce::String ("0")
                                         : juce::String ((int) mark),
                            juce::Rectangle<float> (30.0f, scale.getHeight())
                                .withCentre ({ x, scale.getCentreY() }).toNearestInt(),
                            juce::Justification::centred, false);
            }
        }
    }

    // one bar: track, the above-0 dBFS zone, the level fill and a peak tick
    void drawTrack (juce::Graphics& g, juce::Rectangle<float> track, int i) const
    {
        const float rad = track.getHeight() * 0.5f;
        const bool  out = i >= 2;

        g.setColour (ak::valueFill);
        g.fillRoundedRectangle (track, rad);

        {   // the danger zone, clipped to the rounded track so it keeps the cap
            juce::Graphics::ScopedSaveState ss (g);
            juce::Path clipTo;
            clipTo.addRoundedRectangle (track, rad);
            g.reduceClipRegion (clipTo);
            g.setColour (juce::Colour (0x22e23b52));
            g.fillRect (track.withLeft (track.getX() + track.getWidth() * kZeroDb));
        }

        if (lvl[i] > 0.004f)
        {
            const float w = juce::jmax (track.getHeight(), track.getWidth() * lvl[i]);
            g.setColour (out ? ak::seriesOut : ak::seriesIn);
            g.fillRoundedRectangle (track.withWidth (w), rad);
        }

        if (pkPos[i] > 0.004f)                                    // peak tick
        {
            const float x = track.getX() + track.getWidth() * pkPos[i];
            g.setColour (clip[i] ? juce::Colour (0xffe23b52)
                                 : (out ? ak::seriesOut : ak::seriesIn).darker (0.5f));
            g.fillRect (juce::Rectangle<float> (x - 1.0f, track.getY(), 2.0f, track.getHeight()));
        }

        g.setColour (juce::Colour (0x55e23b52));                  // the 0 dBFS mark
        const float zx = track.getX() + track.getWidth() * kZeroDb;
        g.fillRect (juce::Rectangle<float> (zx - 0.5f, track.getY() - 1.0f,
                                            1.0f, track.getHeight() + 2.0f));

        g.setColour (ak::valueLine);
        g.drawRoundedRectangle (track.reduced (0.5f), rad, 1.0f);
    }

    VoxMorphProcessor& proc;
    int rateHz = 30;   // current timer rate; Performance Mode lowers it
    float lvl[kR]   = { 0.0f, 0.0f, 0.0f, 0.0f };
    float pkPos[kR] = { 0.0f, 0.0f, 0.0f, 0.0f };
    float db[kR]    = { -100.0f, -100.0f, -100.0f, -100.0f };
    bool  clip[kR]  = { false, false, false, false };
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

// Format stamp for .vmpreset. Bump only when old files need treating
// differently on load, and say why here.
//   59 - v0.59.0 made Onset Repair, its Strength and Pulse Body recommended
//        defaults. A preset saved before that either predates the parameters
//        (and gets the defaults anyway, see below) or stores them at the OFF
//        values that were current when it was written -- which was never a
//        decision anybody made. Those are migrated to the defaults on load.
inline constexpr int kVoxMorphPresetVersion = 59;

// Parameters whose stored value an older preset should NOT be trusted for.
// Everything else in a preset is honoured exactly as saved.
inline bool voxMorphIsRecommendedOnsetParam (const juce::String& id)
{
    return id == "onsetbackfill" || id == "prelowcut" || id == "pulsebody";
}

// A .vmpreset is the APVTS state plus the few non-parameter things a preset
// should carry. Every writer goes through here (the MAIN bar, the PRESETS tab
// and MATCHING all save presets) so the format cannot drift between them.
inline std::unique_ptr<juce::XmlElement> voxMorphPresetXml (VoxMorphProcessor& proc)
{
    auto xml = proc.apvts.copyState().createXml();
    if (xml != nullptr)
    {
        xml->setAttribute ("characterImage", proc.characterImagePath);
        // Stamp the format so a preset can say whether its stored values for
        // the onset-repair group are a deliberate choice or just whatever the
        // defaults happened to be on the day it was saved. See
        // voxMorphApplyPreset. Presets written before this have no attribute
        // and read as 0.
        xml->setAttribute ("presetVersion", kVoxMorphPresetVersion);
    }
    return xml;
}

// Apply a .vmpreset to the processor: ONE undo step, locked parameters keep
// their current value, and parameters missing from the file fall back to
// their default (the semantics PresetPanel has had since v0.19.0 — this is
// that code, lifted out so the MAIN tab's preset bar behaves identically).
//
// `migrated` counts parameters whose stored value was deliberately ignored
// because the file is older than the recommendation that made them defaults.
inline bool voxMorphApplyPreset (VoxMorphProcessor& proc, const juce::File& file,
                                 int& applied, int& lockedKept, int* migrated = nullptr)
{
    applied = 0;  lockedKept = 0;
    if (migrated != nullptr) *migrated = 0;
    auto xml = juce::XmlDocument::parse (file);
    if (xml == nullptr || ! xml->hasTagName (proc.apvts.state.getType()))
        return false;

    // A preset that predates a parameter simply has no entry for it and falls
    // back to the default below, which is already what we want. This handles
    // the other case: the parameter existed, so the file HAS a value, but that
    // value is the old default rather than anything the author chose.
    const bool oldFormat = xml->getIntAttribute ("presetVersion", 0) < kVoxMorphPresetVersion;

    proc.history.group ([&]
    {
        for (auto* p : proc.getParameters())
            if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
            {
                float norm = rp->getDefaultValue();
                if (auto* e = xml->getChildByAttribute ("id", rp->paramID))
                {
                    if (oldFormat && voxMorphIsRecommendedOnsetParam (rp->paramID))
                    {
                        if (migrated != nullptr) ++*migrated;   // keep the default
                    }
                    else
                        norm = rp->convertTo0to1 ((float) e->getDoubleAttribute ("value"));
                }
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
    // Presets written before v0.36.14 have no picture in them; leave whatever
    // is showing alone rather than clearing it.
    if (xml->hasAttribute ("characterImage"))
        proc.characterImagePath = xml->getStringAttribute ("characterImage");
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
    juce::AudioDeviceManager::AudioDeviceSetup before;
    dm->getAudioDeviceSetup (before);
    if (before.outputDeviceName == name) return {};

    // Check the name against the live list FIRST. JUCE's
    // setAudioDeviceSetup() calls deleteCurrentDevice() before it verifies
    // that the requested device exists, so handing it a name that is not
    // there tears down the device that is currently open and then returns
    // "No such device" — leaving the app with neither an input nor an output
    // selected. That was the MONITOR bug: the picker stored the decorated
    // "... (offline)" label as if it were a device name.
    if (name.isNotEmpty() && ! vmOutputDeviceNames().contains (name))
        return juce::String::fromUTF8 ("デバイスが見つかりません: ") + name;

    auto wanted = before;
    wanted.outputDeviceName = name;
    wanted.useDefaultOutputChannels = true;
    const auto err = dm->setAudioDeviceSetup (wanted, true);

    // Belt and braces: anything else that fails (device busy, sample rate
    // refused) has also already closed the old device, so put it back.
    if (err.isNotEmpty())
    {
        auto restore = before;
        dm->setAudioDeviceSetup (restore, true);
    }
    return err;
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
                    if (onUserEdit) onUserEdit();
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
                if (unit.isNotEmpty()) slider.setTextValueSuffix (" " + unit);
                // onUserEdit fires for a HAND edit only — a drag, or a click
                // on the track, which JUCE also starts a drag for. NOT
                // slider.onValueChange: that fires for a preset, the host or
                // a Fmt Character too, none of which mean the user has taken
                // the row off its character. Set after the attachment and
                // chained onto whatever is already there: JUCE 8.0.4's
                // SliderParameterAttachment happens to use a Listener rather
                // than this callback, but nothing guarantees that.
                {
                    auto previous = slider.onDragStart;
                    slider.onDragStart = [this, previous]
                    {
                        if (previous) previous();
                        if (onUserEdit) onUserEdit();
                    };
                }
                // A parameter sitting at exactly zero prints "-0.00": its raw
                // value lands a hair below zero and the formatter keeps the
                // minus. Drop it on every row — a stray minus on a default
                // value reads as a bug. setSignedValue() builds on top.
                {
                    auto base = slider.textFromValueFunction;
                    slider.textFromValueFunction = [base] (double v)
                    {
                        auto s = (base != nullptr ? base (v) : juce::String (v, 2)).trim();
                        return (s.startsWithChar ('-') && s.substring (1).containsOnly ("0.,"))
                                 ? s.substring (1) : s;
                    };
                }
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
            if (onUserEdit) onUserEdit();
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

    // Reads the value as a SHIFT, so an upward one says so: "+9.00 st". The
    // parameter prints its own minus and prints an exact zero as "-0.00", so
    // the sign is decided from the DIGITS rather than from the raw value.
    // Only affects the read-out; typing is unchanged (the sign is optional).
    void setSignedValue()
    {
        auto base = slider.textFromValueFunction;
        slider.textFromValueFunction = [base] (double v)
        {
            auto s = (base != nullptr ? base (v) : juce::String (v, 2)).trim();
            const bool neg = s.startsWithChar ('-');
            s = s.trimCharactersAtStart ("+-");
            if (s.containsOnly ("0.,")) return s;              // zero: no sign
            return (neg ? "-" : "+") + s;
        };
        syncValueText();
    }

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
    std::function<void()> onUserEdit;      // the user moved THIS row by hand

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
// ---------------------------------------------------------------------------
// OUTPUT lamps (v0.36.14, replacing the level bar). One per channel, because
// a dead channel is the failure you actually want to catch — a single summed
// bar hides it.
//
// Three states in one lamp: dark = silence, blue = signal, pink = the channel
// touched 0 dBFS. The pink LATCHES for a moment: a clip is a handful of
// samples and would be over before you looked up.
class OutputLamps : public juce::Component, public juce::SettableTooltipClient,
                    private juce::Timer
{
public:
    explicit OutputLamps (VoxMorphProcessor& p) : proc (p)
    {
        setTooltip (vmTip (
            "The signal actually leaving VoxMorph, per channel (after Mute, Output "
            "Gain, the ASMR position and any Post FX). Dark = silence, blue = sound "
            "is going out, and pink means that channel hit 0 dBFS and may be "
            "clipping - lower Output Gain. Pink stays lit for a moment so a brief "
            "clip cannot slip past.",
            "VoxMorphから実際に出ている音を左右それぞれ表示します(Mute・Output Gain・"
            "ASMR位置・Post FXをすべて通過した後)。消灯=無音、青=出力あり、"
            "ピンクはそのチャンネルが0dBFSに達した状態で音が割れる可能性があります"
            "(Output Gainを下げてください)。ピンクは一瞬のクリップを見逃さないよう"
            "少しの間点灯し続けます。"));
        startTimerHz (30);
    }

private:
    static constexpr float kPeakDb  = -0.5f;   // "this channel is at the ceiling"
    static constexpr float kHoldSec = 1.2f;    // how long pink stays lit

    struct Lamp { float lvl = 0.0f, hold = 0.0f; };

    void timerCallback() override
    {
        if (const int hz = vmDrawHz (proc, 30); hz != rateHz)
        { rateHz = hz; startTimerHz (hz); }   // Performance Mode
        if (! isShowing()) return;

        const float dt = 1.0f / (float) juce::jmax (1, rateHz);
        bool dirty = false;
        auto step = [&] (Lamp& lamp, const VoxMorphProcessor::LevelMeter& m)
        {
            const float pk = m.peak.load (std::memory_order_relaxed);
            const float db = juce::Decibels::gainToDecibels (pk, -100.0f);
            const float want = juce::jlimit (0.0f, 1.0f, (db + 54.0f) / 54.0f);
            if (std::abs (want - lamp.lvl) > 0.004f) { lamp.lvl += 0.4f * (want - lamp.lvl); dirty = true; }
            if (db >= kPeakDb)                       { lamp.hold = kHoldSec; dirty = true; }
            else if (lamp.hold > 0.0f)               { lamp.hold = juce::jmax (0.0f, lamp.hold - dt); dirty = true; }
        };
        step (l, proc.uiOutL);
        step (r, proc.uiOutR);
        if (dirty) repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat().reduced (0.0f, 2.0f);
        const float d = juce::jlimit (10.0f, 15.0f, b.getHeight() - 4.0f);

        auto one = [&] (juce::Rectangle<float> area, const char* name, const Lamp& lamp)
        {
            const auto dot = juce::Rectangle<float> (d, d)
                                 .withCentre ({ area.getX() + d * 0.5f, area.getCentreY() });
            const bool peaking = lamp.hold > 0.0f;

            g.setColour (ak::valueFill);                       // the unlit body
            g.fillEllipse (dot);
            if (peaking || lamp.lvl > 0.02f)
            {
                g.setColour (peaking ? juce::Colour (0xffe0607f)
                                     : ak::seriesIn.withAlpha (0.30f + 0.70f * lamp.lvl));
                g.fillEllipse (dot);
                // a soft bloom so a lit lamp reads from across the room
                g.setColour ((peaking ? juce::Colour (0xffe0607f) : ak::seriesIn)
                                 .withAlpha (peaking ? 0.22f : 0.18f * lamp.lvl));
                g.fillEllipse (dot.expanded (d * 0.28f));
            }
            g.setColour (ak::valueLine);
            g.drawEllipse (dot.reduced (0.5f), 1.0f);

            g.setColour (ak::ink);
            g.setFont (ak::font (11.0f, true));
            g.drawText (name, area.withTrimmedLeft (d + 5.0f).toNearestInt(),
                        juce::Justification::centredLeft, false);
        };

        const float cell = juce::jmin (52.0f, b.getWidth() * 0.5f);
        one (b.removeFromLeft (cell), "L", l);
        one (b.removeFromLeft (cell), "R", r);
    }

    VoxMorphProcessor& proc;
    int  rateHz = 30;      // current timer rate; Performance Mode lowers it
    Lamp l, r;
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
    // v0.40.0 TEXTURE. These were measured all along and simply never
    // written, so every .vmprofile silently lost them -- and Auto-Set gates
    // its Air / Air Shine stage on "did the target carry texture", which no
    // saved profile ever did. A NEW CHARACTER profile in particular is
    // supposed to describe a voice; a voice with no breathiness recorded is
    // not one. hnr = harmonic-to-noise per band, hfDb = energy above 6 kHz.
    for (int i = 0; i < 3; ++i)
        x->setAttribute ("h" + juce::String (i + 1), p.hnr[i]);
    x->setAttribute ("hf", p.hfDb);
    x->setAttribute ("tract", p.tractScale);
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
        // Absent (every profile written before v0.40.0) stays 0, which is
        // exactly what MatchingEngine reads as "no texture measured" and is
        // why it leaves Air alone rather than deriving one from nothing.
        // Do NOT default these to a plausible number: 0 dB HNR is a real
        // value meaning "pure noise", so a fabricated default would read as
        // maximum breathiness.
        p.hnr[i] = (float) x.getDoubleAttribute ("h" + juce::String (i + 1), 0.0);
    }
    p.hfDb       = (float) x.getDoubleAttribute ("hf",    -30.0);
    p.tractScale = (float) x.getDoubleAttribute ("tract",   1.0);
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
    // can't tell blue from yellow can still tell Current from Target (spec 4.4)
    const VoiceProfile* you       = nullptr;   // Current  = blue, filled dot
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

        const juce::Colour cy = ak::seriesIn, ct (0xffdfb545), ce (0xffa889f4),
                           cc = ak::seriesOut;
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
            vline (param ("hifreq"),     ak::seriesIn,  "High Range");
            vline (param ("pitchfloor"), ak::seriesOut, "Floor");
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

        // v0.36.11: on the ANOKOE palette. These used to be a warm near-white
        // with a mint selection, which read as a different app next to MAIN.
        const juce::Colour bg   = selected ? ak::sidebarSel : juce::Colours::white;
        const juce::Colour edge = selected ? ak::headBlue   : ak::line;
        g.setColour (bg);
        g.fillRoundedRectangle (b, 10.0f);
        if (hover && ! selected)
        {
            g.setColour (ak::pluginFill.withAlpha (0.75f));
            g.fillRoundedRectangle (b, 10.0f);
        }
        g.setColour (edge);
        g.drawRoundedRectangle (b, 10.0f, selected ? 2.0f : 1.0f);

        // icon area: top ~60 %. Placeholder shape until real icons ship.
        const float iconH = b.getHeight() * 0.55f;
        auto iconR = b.reduced (10.0f).withHeight (iconH);
        g.setColour (selected ? juce::Colours::white : ak::treeLine);
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
                g.setColour (juce::Colour (0xffe07a99));   // the app's pink
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
        g.setColour (selected ? juce::Colours::white : ak::ctrlInk);
        g.setFont (ak::font (11.0f, true));
        auto textR = b.withY (b.getY() + iconH + 4.0f)
                      .withHeight (b.getHeight() - iconH - 6.0f);
        // read via getButtonText() so recBtn.setButtonText("Recording...")
        // during a capture updates the tile visibly
        g.drawFittedText (getButtonText(), textR.toNearestInt(),
                          juce::Justification::centred, 2);
    }
};


// The character badge's picture is either a FILE the user chose or one of
// the shipped portraits, in which case the stored string is this prefix plus
// the BinaryData resource name. One field keeps preset round-trip, state
// persistence and the badge's change-watching timer all working unchanged,
// and a built-in picture survives moving between machines where a file path
// would not.
inline constexpr const char* kBuiltinImagePrefix = "builtin:";

// A MATCH-sized action button that can carry a leading dot (RECORD). The dot
// is drawn here rather than baked into the label so it can turn red while a
// capture is running without rebuilding the text.
class PillButton : public juce::TextButton
{
public:
    using juce::TextButton::TextButton;
    void setDot (juce::Colour c) { dot = c; repaint(); }

    void paintButton (juce::Graphics& g, bool hover, bool down) override
    {
        juce::TextButton::paintButton (g, hover, down);
        if (dot.isTransparent()) return;
        const float d = 9.0f;
        g.setColour (dot);
        g.fillEllipse (14.0f, ((float) getHeight() - d) * 0.5f, d, d);
    }

private:
    juce::Colour dot { juce::Colours::transparentBlack };
};

// ===========================================================================
// CharacterCard (v0.41.0) — a target character as a PORTRAIT card.
//
// Replaces the square icon tile for the TARGET CHARACTER row. The art is the
// point: seven voices whose numbers differ by fractions of a semitone are not
// told apart by reading "Uru" against "Kura", and the previous row of
// identical wifi-looking glyphs said nothing at all about who they were.
//
// Cards sit on the dark strip the Matching page paints behind them, so the
// card itself is the light surface -- the reverse of every other control on
// the page, and the reason it draws its own background rather than using the
// shared card helper.
//
// A character with no art yet draws the same frame with a soft placeholder
// ring. That is a normal state, not an error: art arrives one character at a
// time, and an entry without it still matches perfectly well.
class CharacterCard : public juce::Button
{
public:
    CharacterCard (const juce::String& stableId, const juce::String& label,
                   const char* binaryImage)
        : juce::Button (stableId), targetId (stableId)
    {
        // the caption lives in the Button's own text, not a private copy:
        // it is what accessibility and the UI audit read, and it lets the
        // label change later without touching this class
        setButtonText (label);
        setClickingTogglesState (true);
        if (binaryImage != nullptr && *binaryImage != 0)
            art = ak::image (binaryImage);
    }

    const juce::String targetId;

    void paintButton (juce::Graphics& g, bool hover, bool /*down*/) override
    {
        const auto b   = getLocalBounds().toFloat().reduced (1.5f);
        const bool sel = getToggleState();
        const float rad = 12.0f;

        g.setColour (juce::Colours::white.withAlpha (sel ? 1.0f : 0.94f));
        g.fillRoundedRectangle (b, rad);
        // Selection reads as a heavier, bluer frame. On a dark strip a glow
        // would bloom into the neighbouring cards, so it stays a border.
        g.setColour (sel ? ak::sidebarSel : (hover ? ak::treeLine : ak::line));
        g.drawRoundedRectangle (b, rad, sel ? 2.4f : 1.0f);

        // The portrait fills the whole card, clipped to the same rounded
        // rectangle as the frame. A circle inside a rectangle wasted most of
        // the card and made every character a small distant head; filling it
        // is what makes the row readable at a glance.
        auto inner  = b.reduced (3.0f);
        auto textR  = inner.removeFromBottom (18.0f);
        auto artR   = inner;

        if (art.isValid())
        {
            juce::Path clip;
            clip.addRoundedRectangle (b.reduced (2.0f), rad - 2.0f);
            juce::Graphics::ScopedSaveState ss (g);
            g.reduceClipRegion (clip);
            g.drawImage (art, artR, juce::RectanglePlacement::fillDestination);

            // Unselected cards are washed toward white so the chosen one is
            // obviously the chosen one. A veil rather than a desaturate: the
            // art is already near-monochrome, so removing colour would do
            // nothing, while lifting it toward the page tone reads instantly.
            if (! sel)
            {
                g.setColour (juce::Colours::white.withAlpha (hover ? 0.34f : 0.52f));
                g.fillRect (artR);
            }
        }
        else
        {
            const float d = std::min (artR.getWidth(), artR.getHeight()) * 0.72f;
            auto circle = juce::Rectangle<float> (d, d).withCentre (artR.getCentre());
            g.setColour (ak::line);
            g.drawEllipse (circle, 1.2f);
            g.setColour (ak::treeLine);
            g.setFont (ak::font (11.0f, false));
            g.drawText ("?", circle, juce::Justification::centred);
        }

        // the caption sits ON the art now, so it needs its own backing
        g.setColour (juce::Colours::white.withAlpha (sel ? 0.86f : 0.70f));
        g.fillRoundedRectangle (textR.reduced (3.0f, 0.0f), 6.0f);
        g.setColour (sel ? ak::sidebarSel : ak::ctrlInk);
        g.setFont (ak::font (12.0f, true));
        g.drawFittedText (getButtonText(), textR.toNearestInt(),
                          juce::Justification::centred, 1);
    }

private:
    juce::Image art;
};


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
            ak::styleSectionHeading (l);
            addAndMakeVisible (l);
        };
        // ASCII only -- avoids any codepage confusion on Windows toolchains.
        // Spaced caps to match the MAIN page's card titles.
        initHeading (hTargetCharacter, "TARGET CHARACTER");
        initHeading (hMyVoice,         "MY VOICE");
        initHeading (hAutoMatching,    "AUTO MATCHING");

        descLbl.setText (juce::String::fromUTF8 (
            "プロファイルとあなたの差分を測定し自動設定するシステムです。\n"
            "AI変換を使わない、あなた自身の声のコーディネートです。"),
            juce::dontSendNotification);
        descLbl.setJustificationType (juce::Justification::topLeft);
        descLbl.setColour (juce::Label::textColourId, ak::ctrlInk);
        descLbl.setFont (ak::font (12.0f, false));
        addAndMakeVisible (descLbl);

        // Right-hand pair: what the page IS on the left, what to DO on the
        // right, one on each side of the character so the two read as a pair
        // rather than as one note and some spare space.
        stepsLbl.setText (juce::String::fromUTF8 (
            "1 - キャラクターを選びます。\n"
            "2 - あなたの声を録音します。\n"
            "3 - オートマッチングを行います。"),
            juce::dontSendNotification);
        stepsLbl.setJustificationType (juce::Justification::topLeft);
        stepsLbl.setColour (juce::Label::textColourId, ak::ctrlInk);
        stepsLbl.setFont (ak::font (12.0f, false));
        addAndMakeVisible (stepsLbl);

        addAndMakeVisible (recBtn);
        addAndMakeVisible (myVoiceFileBtn);
        recBtn.setDot (ak::headPink);
        for (auto* b : { &matchBtn, &newCharBtn })
            addAndMakeVisible (*b);
        recBtn.setTooltip (juce::String::fromUTF8 (
            "Records your microphone input for the CURRENT profile.\n"
            "マイク入力を録音してMyVoiceプロファイルにします。"));
        myVoiceFileBtn.setTooltip (juce::String::fromUTF8 (
            "Opens a menu: load an audio file or .vmprofile as MyVoice, play "
            "the loaded audio back, or save the measured profile. Loading "
            "does not touch the Target selection.\n"
            "メニューが開きます: 音声ファイル/.vmprofileの読み込み、読み込んだ"
            "音声の再生、測定したプロファイルの保存。Target選択は変更されません。"));
        myVoiceFileBtn.onClick = [this] { showMyVoiceMenu(); };
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
            auto btn = std::make_unique<CharacterCard> (
                juce::String (samples[i].id), juce::String (samples[i].displayEn),
                samples[i].image);
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
            auto btn = std::make_unique<CharacterCard> (
                juce::String ("target_file"), juce::String ("TargetFile"), nullptr);
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
                // v0.39.0: the tile opens a menu rather than going straight to
                // a chooser, so Play and Save Profile can live here instead of
                // as separate buttons. The selection is restored first for the
                // same reason it always was -- dismissing the menu must leave
                // the UI where it was.
                restoreTargetSelectionUi();
                showTargetFileMenu();
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

        for (auto* l : { &outLbl, &matchStatus })
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

        matchBtn.setButtonText ("MATCH");
        matchBtn.setTooltip (juce::String::fromUTF8 (
            "Auto-Set: derive parameters from the Current -> Target difference. "
            "One Undo step; locked parameters keep their values.\n"
            "CurrentとTargetの差からパラメータを算出して書き込みます。1 Undo、"
            "ロック項目は保持。"));

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
        matchBtn.onClick      = [this] { doMatch(); };

        // Start with NO target selected -- the user picks a Character
        // tile or loads a TargetFile first, then MATCH becomes available.
        clearTargetButtonSelection();
        selectedSampleIndex = -1;
        targetFileActive    = false;
        prof2                = VoiceProfile{};
        proc.lastTarget      = VoiceProfile{};
        currentTargetName    .clear();
        proc.prevPos = -1;
        proc.prevLen = 0;
        updateMatchStatus();
        startTimerHz (10);
    }

    // v0.41.0 layout. Three bands top to bottom -- who you want to sound
    // like, what you sound like, and the match between them -- with the
    // helix drawn between the last two because that is where the comparison
    // happens.
    void paint (juce::Graphics& g) override
    {
        // description card, top left. Pastel so it reads as a note rather
        // than a control, and it is the first thing on the page because the
        // one question this tab has to answer is "what IS this".
        for (auto card : { descArea, stepsArea })
            if (! card.isEmpty())
            {
                g.setGradientFill (juce::ColourGradient (
                    juce::Colour (0xffece8fb), card.getTopLeft().toFloat(),
                    juce::Colour (0xfff6ecf6), card.getBottomRight().toFloat(), false));
                g.fillRoundedRectangle (card.toFloat(), 10.0f);
            }

        // The character strip is DARK: the portraits are pale line art on
        // near-white, so on the page's own light grey they would have no edge
        // at all. It is painted with ak::paintBand -- the SAME tiled lattice,
        // depth gradient and inner shadow as the hero band at the top of the
        // window -- rather than an approximation of it, so the two can never
        // drift apart. It runs to both window edges for the same reason the
        // band does: a dark block with white margins reads as a mistake.
        if (! stripArea.isEmpty())
        {
            juce::Path strip;
            strip.addRectangle (stripArea);
            ak::paintBand (g, strip, stripArea);
        }

        // ---- decoration line (v0.42.1) --------------------------------
        // Three strokes, all in the MAIN tab's 1 px ak::treeLine:
        //
        //   A  collar -> top edge of the character band. It STOPS there; the
        //      band is a solid object and a line crossing it read as a scratch
        //      on the artwork rather than as a connection.
        //   B  bottom edge of the band -> straight down the window centre ->
        //      one clockwise quarter turn -> west, into AUTO MATCHING.
        //   C  MY VOICE -> east -> U-turn -> west, into AUTO MATCHING.
        //
        // B and C arrive along the same run, three pixels apart, which is why
        // the approach to AUTO MATCHING is a DOUBLE line: two things feed it
        // (the character you picked, and your own voice) and they stay
        // legible as two right up to the heading.
        {
            const float xc    = (float) getWidth() * 0.5f;
            const float yMy   = (float) hMyVoice.getBounds().getCentreY();
            const float yAuto = (float) hAutoMatching.getBounds().getCentreY();
            // half the pair's spacing. 3 px read as one slightly thick line at
            // 1 px stroke; 5 reads as two, which is the point of it.
            const float dy    = 5.0f;
            auto textEnd = [this] (const juce::Label& l)
            {
                return (float) (l.getBounds().getX() + textWidthOf (l) + 14);
            };
            const float xEndAuto = textEnd (hAutoMatching);
            const juce::PathStrokeType stroke (1.0f, juce::PathStrokeType::curved,
                                                     juce::PathStrokeType::rounded);
            g.setColour (ak::treeLine);

            // ONE radius for both turns (v0.42.2). C's comes from the gap
            // between the two headings, which is the only vertical distance
            // on this page that has to be respected; B then borrows it, so
            // the two corners are visibly the same curve rather than one
            // sweeping bend and one tight elbow.
            const float yC = yAuto - dy;             // C lands above the pair
            const float yB = yAuto + dy;             // B below it
            float r = juce::jmax (10.0f, (yC - yMy) * 0.5f);
            // B's straight descent has to survive the radius too: a corner
            // taller than the run would start above the band it comes from.
            if (! stripArea.isEmpty())
                r = juce::jmin (r, juce::jmax (10.0f,
                                               yB - (float) stripArea.getBottom() - 12.0f));

            // The turns are placed so they cannot overlap: B's corner opens
            // leftward from the centre and ends at xc - r, so C's is put a
            // clear 40 px further left again.
            const float xrC = xc - r - 40.0f;        // C's right extremity
            const float xsC = xrC - r;               // where C's turn begins

            // A -- collar down to the band
            if (! stripArea.isEmpty() && stripArea.getY() > bulgeBot)
                g.drawLine (xc, (float) bulgeBot, xc, (float) stripArea.getY(), 1.0f);

            // B -- band down the centre, quarter turn, west to the heading
            if (! stripArea.isEmpty() && yB - r > (float) stripArea.getBottom())
            {
                juce::Path b2;
                b2.startNewSubPath (xc, (float) stripArea.getBottom());
                b2.lineTo (xc, yB - r);
                b2.quadraticTo (xc, yB, xc - r, yB);   // clockwise: south -> west
                b2.lineTo (xEndAuto, yB);
                g.strokePath (b2, stroke);
            }

            // C -- the U, landing dy above B so the pair reads as two
            const float xEndMy = textEnd (hMyVoice);
            if (xsC > juce::jmax (xEndMy, xEndAuto) + 20.0f)
            {
                juce::Path u;
                u.startNewSubPath (xEndMy, yMy);
                u.lineTo (xsC, yMy);
                u.quadraticTo (xrC, yMy, xrC, yMy + r);
                u.quadraticTo (xrC, yC, xsC, yC);
                u.lineTo (xEndAuto, yC);
                g.strokePath (u, stroke);
            }
        }
    }

    // width of a heading's own text, so the rule starts after it rather than
    // through it (the Label is laid out full-width for centring reasons)
    static int textWidthOf (const juce::Label& l)
    {
        return (int) std::ceil (juce::GlyphArrangement::getStringWidth (
                                    l.getFont(), l.getText()));
    }

    // Told by the editor where the character circle intrudes: its CENTRE and
    // RADIUS in this panel's coordinates, not a bounding box. The overhang is
    // a circle, so it is at its widest level with the centre (which is up in
    // the band, off this page) and narrows on the way down -- using the box
    // would throw away most of the room the description actually has.
    void setBulge (juce::Point<int> centre, int radius, int bottomY)
    {
        if (bulgeC == centre && bulgeR == radius && bulgeBot == bottomY) return;
        bulgeC = centre; bulgeR = radius; bulgeBot = bottomY;
        resized();
    }

    // half-width of the overhang at a given y, 0 once past it
    int bulgeHalfWidthAt (int y) const
    {
        const int dy = std::abs (y - bulgeC.y);
        if (bulgeR <= 0 || dy >= bulgeR) return 0;
        return (int) std::round (std::sqrt ((double) bulgeR * bulgeR - (double) dy * dy));
    }

    void resized() override
    {
        auto full = getLocalBounds();
        auto r = full.reduced (kEdge, 10);

        // ── description: LEFT of the circle's overhang, on the same rows ──
        {
            const int h = 52;
            const int y = bulgeR > 0 ? juce::jmax (r.getY(), bulgeBot - h - 6) : r.getY();
            // the narrowest point of the overhang across the card's own rows
            const int half = bulgeR > 0 ? bulgeHalfWidthAt (y) : 0;
            const int stop = bulgeR > 0 ? bulgeC.x - half - 14 : r.getRight();
            const int w = juce::jlimit (200, 560, stop - r.getX());
            descArea = juce::Rectangle<int> (r.getX(), y, w, h);
            descLbl.setBounds (descArea.reduced (12, 7));

            const int sx = bulgeR > 0 ? bulgeC.x + half + 14 : r.getRight() - w;
            const int sw = juce::jmax (180, r.getRight() - sx);
            stepsArea = juce::Rectangle<int> (sx, y, sw, h);
            stepsLbl.setBounds (stepsArea.reduced (12, 5));

            r.setTop (juce::jmax (descArea.getBottom(), bulgeR > 0 ? bulgeBot : 0) + 10);
        }

        // ── TARGET CHARACTER: heading, then a full-bleed dark strip ──
        hTargetCharacter.setBounds (r.removeFromTop (20));
        r.removeFromTop (4);
        {
            const int nb   = (int) targetButtons.size();
            const int gap  = 10;
            // Portrait proportions, sized to the strip we can afford, then
            // capped so a short window does not turn them into stamps.
            const int cardH = juce::jlimit (96, 150, r.getHeight() / 4);
            const int cardW = juce::jlimit (72, 116, (int) std::round (cardH * 0.72f));
            // 12 px of dark above and below the cards: enough to read as a
            // band, not so much that the row floats in a void.
            auto strip = r.removeFromTop (cardH + 12);
            stripArea  = strip.withX (full.getX()).withWidth (full.getWidth());

            const int rowW = nb * cardW + (nb - 1) * gap;
            int x = strip.getX() + (strip.getWidth() - rowW) / 2;   // CENTRED
            const int y = strip.getY() + (strip.getHeight() - cardH) / 2;
            for (int i = 0; i < nb; ++i)
            {
                targetButtons[(size_t) i]->setBounds (x, y, cardW, cardH);
                x += cardW + gap;
            }
        }
        // Air below the strip, so TARGET CHARACTER -> MY VOICE and
        // MY VOICE -> AUTO MATCHING are spaced alike instead of the first
        // pair being tight and the second loose.
        r.removeFromTop (juce::jlimit (18, 54, r.getHeight() / 12));

        // The controls keep the left column; the helix is positioned after
        // both headings exist, because it has to START on one and END on the
        // other -- a line that only nearly touches what it connects reads as
        // decoration rather than as a relationship.
        auto body = r;

        // ── MY VOICE ──
        hMyVoice.setBounds (body.removeFromTop (20));
        body.removeFromTop (8);
        {
            auto row = body.removeFromTop (36);
            recBtn.setBounds         (row.removeFromLeft (kActionW));
            row.removeFromLeft (10);
            myVoiceFileBtn.setBounds (row.removeFromLeft (kActionW));
        }
        body.removeFromTop (8);
        {
            auto vopts = body.removeFromTop (28);
            durBox.setBounds (vopts.removeFromLeft (72).withHeight (26));
            vopts.removeFromLeft (8);
            recPlayChk.setBounds (vopts.removeFromLeft (150).withHeight (26));
        }
        // Deliberate air between the two sections: this gap is where the
        // helix lives, and a cramped one turns it into a squiggle.
        body.removeFromTop (juce::jlimit (40, 110, body.getHeight() / 5));

        // ── AUTO MATCHING ──
        hAutoMatching.setBounds (body.removeFromTop (20));
        body.removeFromTop (8);
        {
            auto actionRow = body.removeFromTop (40);
            matchBtn.setBounds   (actionRow.removeFromLeft (kActionW).withHeight (36));
            matchStatus.setBounds (actionRow.reduced (12, 4));
        }
        // NEW CHARACTER belongs to this section but sits on the far right,
        // clear of the helix, where SAVE PRESET used to be.
        newCharBtn.setBounds (full.getRight() - kEdge - 170,
                              hAutoMatching.getBounds().getY() + 26, 170, 36);



        body.removeFromTop (6);
        outLbl.setBounds (body.removeFromBottom (juce::jlimit (30, 76, body.getHeight() / 3))
                              .withTrimmedLeft (2));
        body.removeFromBottom (2);
        graph.setBounds (full.withTrimmedLeft (kEdge).withTrimmedRight (kEdge)
                             .withTop (body.getY()).withBottom (outLbl.getBounds().getY() - 4));
    }

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
        // The tiles carry no play state of their own; the menus read
        // prevPos / myPos when they open, so there is nothing to poll here.
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

    void selectOnlyTargetButton (CharacterCard* selected)
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
        // remembered, not applied: the badge changes when the user commits to
        // this character by pressing MATCH, not while they are browsing
        selectedArt = samples[index].image != nullptr ? samples[index].image : "";
        selectOnlyTargetButton (targetButtons[(size_t) index].get());

        const auto& s = samples[index];
        prof2 = s.profile;
        proc.lastTarget = prof2;
        proc.prevLen = 0;    // sample targets have no audio to Play
        proc.prevPos = -1;
        currentTargetName = juce::String::fromUTF8 (s.displayJp) + " (built-in)";
        status (juce::String::fromUTF8 ("Target: ") + currentTargetName);
        invalidateForTargetChange();
    }

    void selectTargetFileButton()
    {
        targetFileActive    = true;
        selectedSampleIndex = -1;
        selectedArt.clear();          // a loaded file brings no portrait
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
        status (juce::String::fromUTF8 ("MyVoice: ") + sourceName);
        nSet = nLocked = 0;
        refreshEstimated();
        graph.repaint();
        updateMatchStatus();
    }

    // ── tile menus (v0.39.0) ─────────────────────────────────────────────
    // TargetFile and MyVoiceFile each open a menu instead of a file chooser.
    // Load / Play / Save Profile were three separate widgets before; folding
    // them into the tile they belong to removes two buttons per section and
    // keeps every action about "the target file" in one place.
    //
    // Play state is read when the menu OPENS rather than polled, which is why
    // the panel timer no longer touches these at all.
    void stopAllPreview()
    {
        proc.prevPos = -1;
        proc.myPos   = -1;
    }

    void showTargetFileMenu()
    {
        const bool playing = proc.prevPos.load() >= 0;
        const bool hasAudio = proc.prevLen.load() > 0;
        juce::PopupMenu m;
        m.addItem (1, juce::String::fromUTF8 (
            "音声/プロファイルを読み込み... / Load audio or profile..."));
        m.addSeparator();
        m.addItem (2, playing ? juce::String::fromUTF8 ("停止 / Stop")
                              : juce::String::fromUTF8 ("再生 / Play"),
                   hasAudio);
        m.addItem (3, juce::String::fromUTF8 (
            "プロファイルとして保存... / Save as profile..."), prof2.valid());
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (targetFileButton),
            [this, playing] (int r)
            {
                if (r == 1) loadTargetFile();
                else if (r == 2)
                {
                    if (playing) proc.prevPos = -1;
                    else { stopAllPreview(); proc.prevPos = 0; }
                }
                else if (r == 3) saveTargetProfile();
            });
    }

    void showMyVoiceMenu()
    {
        const bool playing  = proc.myPos.load() >= 0;
        const bool hasAudio = proc.myLen.load() > 0;
        juce::PopupMenu m;
        m.addItem (1, juce::String::fromUTF8 (
            "音声/プロファイルを読み込み... / Load audio or profile..."));
        m.addSeparator();
        // A .vmprofile MyVoice carries no audio, and neither does a recording
        // made with Record (that path analyses and discards), so Play is only
        // offered when an audio FILE was loaded here.
        m.addItem (2, playing ? juce::String::fromUTF8 ("停止 / Stop")
                              : juce::String::fromUTF8 ("再生 / Play"),
                   hasAudio);
        m.addItem (3, juce::String::fromUTF8 (
            "プロファイルとして保存... / Save as profile..."), prof1.valid());
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&myVoiceFileBtn),
            [this, playing] (int r)
            {
                if (r == 1) loadMyVoiceFile();
                else if (r == 2)
                {
                    if (playing) proc.myPos = -1;
                    else { stopAllPreview(); proc.myPos = 0; }
                }
                else if (r == 3) saveMyVoiceProfile();
            });
    }

    // ── NEW CHARACTER (v0.39.0) ──────────────────────────────────────────
    // Saves the voice the current parameters would PRODUCE from MyVoice, as a
    // .vmprofile that can then be chosen as a Target. Not a .vmpreset: a
    // preset is a set of knob positions, and what is wanted here is the
    // resulting VOICE, which is a different kind of object and is what the
    // Target side consumes.
    //
    // It is a PREDICTION -- MyVoice's measurement with the parameters applied
    // arithmetically (MatchingEngine::predictEstimated) -- not a measurement
    // of the converted output. Recording the real output and analysing that
    // is what Record + capFromOutput already does, and it needs the user to
    // actually speak; this deliberately does not.
    void saveNewCharacter()
    {
        if (! prof1.valid())
        {
            status (juce::String::fromUTF8 (
                "MyVoiceが必要です。RecordかMyVoiceFileで先に測定してください。"));
            return;
        }
        refreshEstimated();                 // parameters may have moved since MATCH
        if (! profE.valid())
        {
            status (juce::String::fromUTF8 ("推定プロファイルを計算できませんでした。"));
            return;
        }
        saveProfileImpl (profE, "New Character",
                         juce::String::fromUTF8 ("キャラクター(変換後の推定音声)"));
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
                    status (juce::String::fromUTF8 ("Target: ") + currentTargetName);
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
            status (juce::String::fromUTF8 ("Target: ") + currentTargetName);
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
                {
                    proc.myPos = -1;
                    proc.myLen = 0;          // profile-only MyVoice has no audio
                    applyMyVoiceProfile (p, file.getFileNameWithoutExtension()
                                              + " (.vmprofile)");
                }
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
                // do NOT overwrite proc.myBuf on analysis failure, for the
                // same reason the Target path does not touch prevBuf: a
                // failed load must leave the previous state playable.
                status (juce::String::fromUTF8 (
                    "MyVoiceの解析に失敗しました(有声区間が不足): ") + file.getFileName());
                return;
            }
            // keep the audio so the tile menu can play it back. Written only
            // while stopped, which is what makes the lock-free handoff safe.
            proc.myPos = -1;
            const int copyN = std::min ((int) decoded.samples.size(),
                                        (int) proc.myBuf.size());
            std::copy_n (decoded.samples.data(), copyN, proc.myBuf.data());
            proc.myLen = copyN;
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

        // The badge follows the character you actually matched to. Doing it
        // here rather than on selection means the picture tracks what the
        // plugin is SET to, not what the mouse last touched -- and it lands
        // in the preset with the parameters that produced it.
        if (selectedArt.isNotEmpty())
        {
            proc.characterImagePath = juce::String (kBuiltinImagePrefix) + selectedArt;
            // HeroCircle notices on its own 4 Hz timer; no direct call, so
            // this stays correct when the editor is rebuilt underneath us.
        }
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

    // ── status ──────────────────────────────────────────────────────────
    // v0.41.0: fmt() went with the readout labels. Nothing displays a raw
    // profile any more -- the page shows WHO is selected, and the status
    // line reports what happened.

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
        // NEW CHARACTER predicts what the CURRENT parameters would make of
        // MyVoice, so MyVoice is the hard requirement; a Target is not.
        newCharBtn.setEnabled (prof1.valid());
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

    juce::Label hTargetCharacter, hMyVoice, hAutoMatching;
    // v0.41.0: the per-profile readouts ("Target: Sara ... F0 318 Hz spread
    // 4.76") are gone. They were the numbers the estimator happens to hold,
    // shown to someone choosing a VOICE -- unreadable at a glance and noisy
    // next to the portraits. Selection is now shown by the card itself, and
    // anything that needs saying goes to the status line like every other
    // message on the page.
    juce::Label descLbl, stepsLbl, outLbl, matchStatus;
    static constexpr int kActionW = 170;      // MATCH / RECORD / MyVoiceFile
    // This page is bounded to the FULL window width, not the inset content
    // area every other page gets, because the character strip has to bleed
    // to both edges. The inset the other pages receive from the editor is
    // therefore applied here instead, and kEdge is the only thing that keeps
    // the content lined up with the other tabs -- change it and the columns
    // stop agreeing across pages.
    static constexpr int kEdge = 28;          // 12 (page inset) + 16 (content)
    juce::Rectangle<int> descArea, stepsArea, stripArea;
    // The hero circle hangs below the band and over the top of this page.
    // The page is given the FULL content area (not the leftovers below the
    // bulge) and tucks the description into the space beside it, so the
    // window has no dead band across its whole width.
    juce::Point<int> bulgeC;
    int bulgeR = 0, bulgeBot = 0;

    static constexpr int kTargetRadioGroup = 0x564d01;
    std::vector<std::unique_ptr<CharacterCard>> targetButtons;
    CharacterCard* targetFileButton = nullptr;
    int  selectedSampleIndex = -1;   // -1 = no built-in target selected
    juce::String selectedArt;        // BinaryData name of the chosen portrait
    bool targetFileActive    = false;

    juce::ComboBox durBox;
    juce::ToggleButton recPlayChk { "With target play" };

    // Sized like MATCH so the three things you actually press on this page
    // are the same object at three moments, instead of two square tiles and
    // a wide button that look like different kinds of control.
    PillButton recBtn         { "RECORD" };
    PillButton myVoiceFileBtn { "MyVoiceFile" };
    // v0.39.0: Play / Save Profile / Reset All are gone as separate buttons.
    // The first two moved into the TargetFile and MyVoiceFile tile menus (a
    // tile now opens a menu instead of going straight to a file chooser), and
    // Reset All moved to the preset dropdown in the header, where it is
    // reachable from every tab instead of only this one.
    // SAVE PRESET removed in v0.41.0: the header carries a preset dropdown
    // with its own save on every tab, so this was a second door to the same
    // room. NEW CHARACTER stays -- it saves a different kind of object.
    juce::TextButton matchBtn { "MATCH" }, newCharBtn { "NEW CHARACTER" };

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
        ak::styleSectionHeading (heading);
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

        // preview graph: a standard reference voice (blue) vs how this
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
            "プレビュー: 標準的な声(青)がこのプリセットでどう変わるか(ピンク)のイメージ。"),
            juce::dontSendNotification);
        addAndMakeVisible (pvLbl);

        hProfiles.setText ("PROFILES", juce::dontSendNotification);
        ak::styleSectionHeading (hProfiles);
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
        // no panel: the page sits on the same flat body tone as MAIN
        juce::ignoreUnused (g);
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
        int applied = 0, lockedKept = 0, migrated = 0;
        if (! voxMorphApplyPreset (proc, files.getReference (idx), applied, lockedKept, &migrated))
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
        if (migrated > 0)
            msg += juce::String::fromUTF8 ("、語頭修正は推奨値を適用");
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
        if (auto xml = voxMorphPresetXml (proc); xml != nullptr && xml->writeTo (file))
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
        box.setTooltip (box.getTooltip() + juce::String::fromUTF8 (
            "\nリストの末尾には、全項目のリセットとキャラクター画像の選択もあります。"));
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

    // v0.39.0: "Reset All to Defaults" lives here now, at the end of the
    // preset list. It used to be a button on the MATCHING tab only, which is
    // an odd home for something that resets every parameter in the plugin --
    // this dropdown is in the header and reachable from every tab.
    //
    // An ACTION inside a selection list needs care: picking it must not leave
    // it looking like the selected preset, so applySelected() restores the
    // previous selection before running it. The id is far above any preset
    // index so the two can never collide.
    static constexpr int kResetId    = 90001;
    static constexpr int kChooseImgId = 90002;
    static constexpr int kDefaultImgId = 90003;

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
        if (! files.isEmpty()) box.addSeparator();
        box.addItem (juce::String::fromUTF8 ("Reset All to Defaults / 全項目を初期値へ"),
                     kResetId);
        box.addItem (juce::String::fromUTF8 ("キャラクター画像を選択... / Choose character image..."),
                     kChooseImgId);
        box.addItem (juce::String::fromUTF8 ("既定の画像に戻す / Use the default image"),
                     kDefaultImgId);
        box.setItemEnabled (kDefaultImgId, proc.characterImagePath.isNotEmpty());
        box.setSelectedId (selId, juce::dontSendNotification);
    }

    // Every parameter back to its default in ONE undo step, locked
    // parameters untouched -- the same contract the PRESETS tab's button has.
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
        report (msg + juce::String::fromUTF8 ("。Undoで戻せます。"));
    }

    void applySelected()
    {
        if (updating) return;                     // list rebuild, not a user pick
        // Action items. Every one of these puts the box back where it was
        // FIRST: they are actions, not selections, and leaving one showing
        // would claim it is the loaded preset. The guard stops the restoring
        // write from re-entering this function.
        const int action = box.getSelectedId();
        if (action == kResetId || action == kChooseImgId || action == kDefaultImgId)
        {
            {
                const juce::ScopedValueSetter<bool> guard (updating, true);
                box.setSelectedId (lastPresetId, juce::dontSendNotification);
            }
            if (action == kResetId) resetAllParameters();
            else if (action == kChooseImgId) chooseCharacterImage();
            else
            {
                proc.characterImagePath.clear();
                report (juce::String::fromUTF8 ("キャラクター画像を既定に戻しました"
                                                " (保存するとプリセットにも反映されます)"));
            }
            return;
        }
        lastPresetId = box.getSelectedId();
        const int idx = box.getSelectedId() - 1;
        if (! juce::isPositiveAndBelow (idx, files.size())) return;

        int applied = 0, lockedKept = 0, migrated = 0;
        if (! voxMorphApplyPreset (proc, files.getReference (idx), applied, lockedKept, &migrated))
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
        if (migrated > 0)
            msg += juce::String::fromUTF8 ("、語頭修正は推奨値");
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
        // v0.42.0: the two picture items moved to the preset DROPDOWN, next
        // to Reset All. This button is "save", and choosing a picture is not
        // saving anything.
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

    // Sets proc.characterImagePath; HeroCircle notices on its own timer.
    // Only a file that actually decoded is committed, so a bad pick cannot
    // leave the badge blank at the next launch.
    void chooseCharacterImage()
    {
        imgChooser = std::make_unique<juce::FileChooser> (
            juce::String::fromUTF8 ("キャラクター画像を選択 / Choose a character image"),
            juce::File(), "*.png;*.jpg;*.jpeg;*.gif;*.bmp");
        imgChooser->launchAsync (juce::FileBrowserComponent::openMode
                               | juce::FileBrowserComponent::canSelectFiles,
            [this] (const juce::FileChooser& fc)
            {
                const auto f = fc.getResult();
                if (f == juce::File()) return;                       // cancelled
                if (! juce::ImageFileFormat::loadFrom (f).isValid())
                {
                    report (juce::String::fromUTF8 ("この画像は読み込めませんでした: ")
                              + f.getFileName());
                    return;
                }
                proc.characterImagePath = f.getFullPathName();
                report (juce::String::fromUTF8 ("\xE2\x9C\x93 キャラクター画像: ")
                          + f.getFileName()
                          + juce::String::fromUTF8 (" (保存するとプリセットにも入ります)"));
            });
    }

    void writePreset (const juce::String& name)
    {
        if (name.isEmpty()) return;
        const auto file = voxMorphPresetDir()
                            .getChildFile (juce::File::createLegalFileName (name) + ".vmpreset");
        auto xml = voxMorphPresetXml (proc);
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
    std::unique_ptr<juce::FileChooser> imgChooser;
    std::function<void (const juce::String&)> status;
    juce::Array<juce::File> files;
    RescanComboBox   box;
    ak::IconButton saveBtn   { "save",   "ui_mark_S_Save_png",   17 };
    ak::IconButton deleteBtn { "delete", "ui_mark_S_Delete_png", 17 };
    std::unique_ptr<juce::AlertWindow> nameWin;
    juce::LookAndFeel_V4 alertLnf { juce::LookAndFeel_V4::getLightColourScheme() };
    bool updating = false, dark = false;
    int  lastPresetId = 0;          // restored after the Reset All action
};

// ===========================================================================
// ASMR tab (v0.45.0) — the spatial page.
//
// The pad still drives asmrx / asmry exactly as it did; what changed is that
// the ASMR output stage behind it is now SpatialEngine (see dsp/
// SpatialEngine.h) rather than a bare pan, and this page exposes the rest of
// it: binaural cues, air absorption, a room, width and an auto-orbit.
//
// Colours come from the ANOKOE palette only — ak::seriesOut (pink) is the
// source, ak::seriesIn (blue) is the listener, and the dish is the same
// gradient and guide-ring tone the VISUALIZER's panels use, so the two pages
// read as one instrument. The old pad had its own mint-green disc and rings
// that appear nowhere else in the app.
// ===========================================================================
class SonarPad : public juce::Component, public juce::SettableTooltipClient,
                 private juce::Timer
{
public:
    explicit SonarPad (VoxMorphProcessor& p) : proc (p)
    {
        px = proc.apvts.getParameter ("asmrx");
        py = proc.apvts.getParameter ("asmry");
        setTooltip (vmTip (
            "Drag the dot to place the voice around your head. Up is in front of you, "
            "left and right are at your ears, and the further from the centre the further "
            "away (and quieter) the voice gets. Double-click to bring it back to the "
            "centre, which is exactly no effect. The left/right half needs a stereo "
            "output; on a mono bus only the distance applies. While Orbit is running the "
            "dot shows where the voice actually is, not where you last left it.",
            "点をドラッグすると、声の位置を頭のまわりに配置できます。上=正面、"
            "左右=耳元、中心から離れるほど遠く(小さく)なります。ダブルクリックで中央"
            "(=効果なし)に戻ります。左右方向はステレオ出力時のみ有効です"
            "(モノラル時は距離のみ)。Orbitが動いている間は、点が実際の現在位置を示します。"));
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
        auto b = getLocalBounds().toFloat().reduced (24.0f);
        const float s = std::min (b.getWidth(), b.getHeight());
        return b.withSizeKeepingCentre (s, s);
    }

    static float paramValue (juce::RangedAudioParameter* p)
    {
        return p != nullptr ? p->convertFrom0to1 (p->getValue()) : 0.0f;
    }
    float raw (const char* id, float fallback) const
    {
        if (auto* p = proc.apvts.getRawParameterValue (id))
            return p->load (std::memory_order_relaxed);
        return fallback;
    }

    // The head in the middle: this is YOU, so it is drawn as a listener and
    // not as another source. Mint, to match the "input" side of every graph.
    static void drawHead (juce::Graphics& g, juce::Point<float> c, float r)
    {
        g.setColour (ak::seriesIn.withAlpha (0.16f));
        g.fillEllipse (juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (c));
        g.setColour (ak::seriesIn.withAlpha (0.75f));
        g.drawEllipse (juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (c), 1.4f);
        // ears, so which way the head is facing is not a matter of opinion
        const float e = r * 0.34f;
        for (int s = 0; s < 2; ++s)
        {
            const float ex = c.x + (s == 0 ? -r : r);
            g.fillEllipse (juce::Rectangle<float> (e, e * 1.5f).withCentre ({ ex, c.y }));
        }
        // nose: a small notch at the top marks "front"
        juce::Path nose;
        nose.startNewSubPath (c.x - r * 0.24f, c.y - r * 0.92f);
        nose.lineTo (c.x, c.y - r * 1.34f);
        nose.lineTo (c.x + r * 0.24f, c.y - r * 0.92f);
        g.strokePath (nose, juce::PathStrokeType (1.4f));
    }

    static void dashedCircle (juce::Graphics& g, juce::Point<float> c, float r, float thick)
    {
        juce::Path p;
        p.addEllipse (juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (c));
        const float dashes[] = { 5.0f, 5.0f };
        juce::PathStrokeType (thick).createDashedStroke (p, p, dashes, 2);
        g.strokePath (p, juce::PathStrokeType (thick));
    }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat().reduced (2.0f);
        ak::paintCard (g, b);

        const auto c   = circleBounds();
        const auto ctr = c.getCentre();
        const float rad = c.getWidth() * 0.5f;
        if (rad < 30.0f) return;

        // the dish: the theme's soft raised-disc gradient
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xfffafcfd), ctr.x, ctr.y - rad,
                                                 juce::Colour (0xffeceff3), ctr.x, ctr.y + rad, false));
        g.fillEllipse (c);
        g.setColour (juce::Colour (0x14000000));
        g.drawEllipse (c, 1.0f);

        const bool  bin   = raw ("asmrbin", 0.0f) > 0.5f;
        const float distP = raw ("asmrdist", 100.0f);
        const float orbit = raw ("asmrorbit", 0.0f);

        // Behind-you shading: while Binaural Cues is on, the lower half is
        // where the extra HF loss and level dip live. Showing the region is
        // the only way that cue is discoverable — it is a filter, and a
        // filter has no control to look at.
        if (bin)
        {
            juce::Path back;
            back.addPieSegment (c, juce::MathConstants<float>::halfPi,
                                juce::MathConstants<float>::halfPi * 3.0f, 0.0f);
            g.setColour (ak::heading.withAlpha (0.07f));
            g.fillPath (back);
        }

        // Distance rings, labelled with the attenuation they actually apply,
        // so "further away" has a number on it. The labels sit on the BACK-
        // LEFT diagonal: on the vertical axis (the obvious place) the
        // outermost one lands under the FRONT caption and the two overprint
        // each other.
        g.setFont (ak::font (9.5f));
        for (float f : { 1.0f / 3.0f, 2.0f / 3.0f, 1.0f })
        {
            g.setColour (juce::Colour (0x12000000));
            g.drawEllipse (c.withSizeKeepingCentre (c.getWidth() * f, c.getHeight() * f), 1.0f);
            const float gainAt = 1.0f - 0.6f * (distP * 0.01f) * f;
            if (gainAt <= 0.0f) continue;
            const float db = 20.0f * std::log10 (gainAt);
            if (db > -0.05f) continue;                      // 0 dB needs no label
            const float k = 0.70710678f;                    // 225 degrees
            g.setColour (ak::heading.withAlpha (0.6f));
            g.drawText (juce::String (db, 1) + " dB",
                        (int) (ctr.x - rad * f * k) - 54, (int) (ctr.y + rad * f * k) - 6,
                        50, 12, juce::Justification::right, false);
        }

        g.setColour (ak::treeLine);                          // axes
        g.drawLine (c.getX(), ctr.y, c.getRight(), ctr.y, 1.0f);
        g.drawLine (ctr.x, c.getY(), ctr.x, c.getBottom(), 1.0f);

        g.setColour (ak::heading);
        g.setFont (ak::font (10.5f, true));
        g.drawText ("FRONT", (int) ctr.x - 30, (int) c.getY() - 15, 60, 13, juce::Justification::centred);
        g.drawText ("BACK",  (int) ctr.x - 30, (int) c.getBottom() + 2, 60, 13, juce::Justification::centred);
        g.drawText ("L", (int) c.getX() - 15,    (int) ctr.y - 7, 12, 14, juce::Justification::centred);
        g.drawText ("R", (int) c.getRight() + 4, (int) ctr.y - 7, 12, 14, juce::Justification::centred);

        // Where the source IS. With the orbit running that is not where the
        // pad was left, so the engine's own published position wins; with it
        // off the parameters are used directly, which keeps the dot following
        // the mouse even when no audio is being processed.
        float x = paramValue (px), y = paramValue (py);
        if (orbit > 0.0f)
        {
            x = proc.uiSpaceX.load (std::memory_order_relaxed);
            y = proc.uiSpaceY.load (std::memory_order_relaxed);
            const float rr = std::sqrt (x * x + y * y);
            if (rr > 0.001f)
            {
                g.setColour (ak::seriesOut.withAlpha (0.35f));
                dashedCircle (g, ctr, rr * rad, 1.0f);
            }
        }

        const float dx = ctr.x + x * rad;
        const float dy = ctr.y - y * rad;

        // the line of sight from the listener to the source
        g.setColour (ak::seriesOut.withAlpha (0.35f));
        g.drawLine (ctr.x, ctr.y, dx, dy, 1.0f);

        drawHead (g, ctr, rad * 0.15f);

        // Ear gains: the two constant-power channel gains, as short arcs just
        // outside each ear. This is the pan made visible, and the numbers are
        // the same ones the audio stage uses. Kept thin and pale on purpose —
        // at the centre both gains are unity, so a bold version draws two big
        // brackets on a pad that is doing nothing.
        {
            const float dist = std::min (1.0f, std::sqrt (x * x + y * y));
            const float dg = 1.0f - 0.6f * (distP * 0.01f) * dist;
            const float ph = (x + 1.0f) * 0.25f * juce::MathConstants<float>::pi;
            const float gL = std::max (0.0f, dg * std::cos (ph) * juce::MathConstants<float>::sqrt2);
            const float gR = std::max (0.0f, dg * std::sin (ph) * juce::MathConstants<float>::sqrt2);
            const float pi = juce::MathConstants<float>::pi;
            for (int s = 0; s < 2; ++s)
            {
                const float gain = std::min (1.4f, s == 0 ? gL : gR);
                juce::Path arc;
                const float mid = s == 0 ? -juce::MathConstants<float>::halfPi
                                         :  juce::MathConstants<float>::halfPi;
                arc.addCentredArc (ctr.x, ctr.y, rad * 0.215f, rad * 0.215f, 0.0f,
                                   mid - 0.20f * pi, mid + 0.20f * pi, true);
                g.setColour (ak::seriesIn.withAlpha (juce::jlimit (0.05f, 0.55f, gain * 0.42f)));
                g.strokePath (arc, juce::PathStrokeType (2.0f + 2.6f * std::min (1.0f, gain),
                                                         juce::PathStrokeType::curved,
                                                         juce::PathStrokeType::rounded));
            }
        }

        // the source itself: halo, then core
        g.setColour (ak::seriesOut.withAlpha (0.20f));
        g.fillEllipse (dx - 15.0f, dy - 15.0f, 30.0f, 30.0f);
        g.setColour (ak::seriesOut.withAlpha (0.38f));
        g.fillEllipse (dx - 10.0f, dy - 10.0f, 20.0f, 20.0f);
        g.setColour (ak::seriesOut);
        g.fillEllipse (dx - 6.0f, dy - 6.0f, 12.0f, 12.0f);
        g.setColour (juce::Colours::white.withAlpha (0.9f));
        g.drawEllipse (dx - 6.0f, dy - 6.0f, 12.0f, 12.0f, 1.2f);
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

// ---------------------------------------------------------------------------
// A named starting point for the whole page. Scenes exist because the spatial
// controls interact — "behind you, in a room, slightly muffled" is five
// parameters, and nobody discovers that combination by turning one knob at a
// time. Each scene is applied as ONE undo step and respects locked rows,
// exactly like loading a preset.
struct AsmrScene
{
    const char* name;
    const char* jp;
    float x, y;
    bool  binaural;
    float distance, air, room, size, width, orbit, depth;
};

inline const AsmrScene* getAsmrScenes (int& count)
{
    static const AsmrScene k[] = {
        //  name          jp             x      y   bin   dist  air  room  size width orbit depth
        { "Off",        "効果なし",    0.0f,  0.0f, false, 100,   0,    0,  50,  100, 0.0f,  60 },
        { "Close Left", "左耳ささやき", -0.62f, 0.28f, true,  70,  10,   12,  30,  110, 0.0f,  60 },
        { "Close Right","右耳ささやき",  0.62f, 0.28f, true,  70,  10,   12,  30,  110, 0.0f,  60 },
        { "Behind",     "背後から",     0.0f, -0.75f, true, 100,  25,   26,  55,  100, 0.0f,  60 },
        { "Across",     "少し離れて",   0.0f,  0.85f, true, 100,  45,   40,  70,  100, 0.0f,  60 },
        { "Orbit",      "ぐるぐる",     0.0f,  0.0f, true,  90,  20,   22,  45,  115, 0.12f, 75 },
    };
    count = (int) (sizeof (k) / sizeof (k[0]));
    return k;
}

class AsmrPanel : public juce::Component
{
public:
    explicit AsmrPanel (VoxMorphProcessor& p) : proc (p), pad (p)
    {
        heading.setText ("ASMR SPACE", juce::dontSendNotification);
        ak::styleSectionHeading (heading);
        addAndMakeVisible (heading);

        help.setJustificationType (juce::Justification::topLeft);
        help.setFont (ak::font (11.5f));
        help.setColour (juce::Label::textColourId, ak::heading.withAlpha (0.9f));
        help.setText (juce::String::fromUTF8 (
            "変換後の音を頭のまわりに配置します。エンジン(MAINタブ)より後ろの段なので、"
            "声質・ピッチには一切影響しません。\n"
            "すべての項目は既定値=オフで、その状態の音は従来と完全に同一です。"
            "まず下の Scene から選び、そこから微調整するのがおすすめです。"),
            juce::dontSendNotification);
        addAndMakeVisible (help);

        // ---- scene buttons ------------------------------------------------
        sceneLbl.setText ("SCENE", juce::dontSendNotification);
        sceneLbl.setFont (ak::font (10.5f, true));
        sceneLbl.setColour (juce::Label::textColourId, ak::heading.withAlpha (0.8f));
        addAndMakeVisible (sceneLbl);

        int nScenes = 0;
        const auto* sc = getAsmrScenes (nScenes);
        for (int i = 0; i < nScenes; ++i)
        {
            auto* b = scenes.add (new juce::TextButton (sc[i].name));
            b->setTooltip (juce::String::fromUTF8 (sc[i].jp)
                           + juce::String::fromUTF8 ("。この場面の設定を一括で適用します"
                                                     "(ロック中の項目はそのまま)。"));
            b->onClick = [this, i] { applyScene (i); };
            addAndMakeVisible (b);
        }

        addAndMakeVisible (pad);

        // ---- control cards ------------------------------------------------
        cardPos.setTitle ("POSITION", "ui_mark_L_ASMR_png", ak::headBlue, ak::markBlue);
        add (cardPos, "asmrdist", ParamRow::Kind::slider, "Distance Amount (%)", ak::Tone::blue,
             tipOf ("How much the distance from the centre lowers the level. 100 % is the "
                    "classic behaviour (about -8 dB at the rim), 0 % keeps the level flat so "
                    "the pad only decides the direction, and 200 % makes stepping back "
                    "dramatic. This is a level control only - it does not change the tone.",
                    "中心からの距離で音量をどれだけ下げるかです。100%=従来どおり"
                    "(外周で約-8dB)、0%=音量は変えず方向だけを決める、200%=離れると"
                    "大きく小さくなります。音色は変わりません。"));
        add (cardPos, "asmrair", ParamRow::Kind::slider, "Air Absorption (%)", ak::Tone::blue,
             tipOf ("Distant sound loses its high end on the way to you - that is what makes "
                    "far away sound far away rather than just quiet. This adds that loss, and "
                    "it grows as the dot moves out; at the centre it does nothing whatever the "
                    "value. Works on a mono output too.",
                    "遠くの音は高域を失いながら届きます。「小さい」だけでなく「遠い」と"
                    "感じさせるのはこの高域の減衰です。点を外側に動かすほど強くかかり、"
                    "中心では値に関係なく無効です。モノラル出力でも有効。"));

        cardBin.setTitle ("BINAURAL", "ui_mark_M_Monitor_png", ak::headPink, ak::markBlue);
        add (cardBin, "asmrbin", ParamRow::Kind::toggle, "Binaural Cues", ak::Tone::pink,
             tipOf ("Adds the two cues your ears actually use to locate a voice: the far ear "
                    "hears it a fraction of a millisecond LATER (up to 0.66 ms), and duller, "
                    "because your head is in the way. It also makes a voice placed behind you "
                    "sound behind you rather than merely quiet. Best on headphones - this is "
                    "what turns left/right panning into 'she is at my ear'. Needs a stereo "
                    "output; off is the previous behaviour.",
                    "耳が実際に方向を判断している2つの手がかりを加えます。遠い側の耳には"
                    "わずかに遅れて(最大0.66ms)、頭に遮られて少しこもって届きます。"
                    "後ろに置いた声が「小さい」ではなく「後ろにいる」と聞こえるようにも"
                    "なります。ヘッドホン推奨。左右の音量差だけだったパンが"
                    "「耳元にいる」感覚に変わります。ステレオ出力が必要。オフ=従来どおり。"));
        add (cardBin, "asmrwidth", ParamRow::Kind::slider, "Stereo Width (%)", ak::Tone::pink,
             tipOf ("Widens or narrows the whole stereo picture. 100 % leaves it untouched, 0 % "
                    "collapses it to the centre, 200 % pushes it out past the speakers. Useful "
                    "with a binaural microphone (Stereo Input) where the recording already "
                    "carries its own width.",
                    "ステレオの広がりを調整します。100%=変更なし、0%=中央にまとめる、"
                    "200%=スピーカーの外側まで広げる。バイノーラルマイク(Stereo Input)の"
                    "録音のように、元から広がりがある音に対して有効です。"));

        cardRoom.setTitle ("ROOM", "ui_mark_M_Advanced_png", ak::headGold, ak::markBlue);
        add (cardRoom, "asmrroom", ParamRow::Kind::slider, "Ambience (%)", ak::Tone::yellow,
             tipOf ("Puts the voice in a small room: a few early reflections and a short, "
                    "damped tail. Deliberately subtle - this is a bedroom, not a hall. The "
                    "further out the dot sits the more of the room you hear, which is most of "
                    "what 'further away, indoors' actually sounds like. 0 % switches the "
                    "reverb off entirely.",
                    "声を小さな部屋の中に置きます。初期反射と短い残響を少しだけ加えます"
                    "(ホールではなく室内程度の控えめな量)。点が外側にあるほど残響の"
                    "割合が増え、屋内で「遠ざかった」感じになります。0%で完全にオフ。"));
        add (cardRoom, "asmrsize", ParamRow::Kind::slider, "Room Size (%)", ak::Tone::yellow,
             tipOf ("How big that room is. Small values give a tight booth-like closeness, "
                    "large values a longer, more open decay. Has no effect while Ambience is 0.",
                    "部屋の大きさです。小さいほど狭くタイトに、大きいほど長く開放的に"
                    "響きます。Ambienceが0のときは効果がありません。"));

        cardMove.setTitle ("MOTION", "ui_mark_M_Intonation_png", ak::headBlue, ak::markBlue);
        add (cardMove, "asmrorbit", ParamRow::Kind::slider, "Orbit Rate (Hz)", ak::Tone::blue,
             tipOf ("Slowly circles the voice around your head, hands-free. 0 = off. 0.1 Hz is "
                    "one lap every ten seconds, which is about as fast as this stays pleasant; "
                    "anything above roughly 0.5 Hz starts to sound like an effect rather than "
                    "a person moving. The pad's dot follows the real position while this runs.",
                    "声を頭のまわりにゆっくり周回させます。0=オフ。0.1Hzで10秒に1周で、"
                    "心地よさを保てるのはこのあたりまでです。0.5Hzを超えると人の移動では"
                    "なくエフェクトに聞こえ始めます。動作中はパッドの点が実際の位置を"
                    "示します。"));
        add (cardMove, "asmrdepth", ParamRow::Kind::slider, "Orbit Radius (%)", ak::Tone::blue,
             tipOf ("How wide the orbit is when the pad is sitting at the centre. If you have "
                    "already dragged the dot further out than this, that larger radius is used "
                    "instead - so the orbit never pulls the voice closer than you put it.",
                    "パッドが中央にあるときの周回半径です。すでに点をこれより外側に"
                    "ドラッグしている場合は、そちらの半径が使われます"
                    "(周回によって声が近づいてしまうことはありません)。"));

        for (auto* c : { &cardPos, &cardBin, &cardRoom, &cardMove })
            addAndMakeVisible (*c);
    }

    // Locks are honoured here for the same reason presets honour them: a
    // scene is a bulk write, and a locked row means "not by anything".
    void applyScene (int index)
    {
        int n = 0;
        const auto* sc = getAsmrScenes (n);
        if (index < 0 || index >= n) return;
        const auto& s = sc[index];

        const struct { const char* id; float v; } vals[] = {
            { "asmrx",     s.x },        { "asmry",     s.y },
            { "asmrbin",   s.binaural ? 1.0f : 0.0f },
            { "asmrdist",  s.distance }, { "asmrair",   s.air },
            { "asmrroom",  s.room },     { "asmrsize",  s.size },
            { "asmrwidth", s.width },    { "asmrorbit", s.orbit },
            { "asmrdepth", s.depth },
        };
        proc.history.group ([&]
        {
            for (auto& v : vals)
            {
                if (proc.isParamLocked (v.id)) continue;
                if (auto* rp = proc.apvts.getParameter (v.id))
                {
                    rp->beginChangeGesture();
                    rp->setValueNotifyingHost (rp->convertTo0to1 (v.v));
                    rp->endChangeGesture();
                }
            }
        });
    }

    void refreshLocks() { for (auto& r : rows) r->refreshLock(); }
    std::function<void()> onLockChanged;   // set by the editor

    void paint (juce::Graphics&) override
    {
        // no panel: the page sits on the same flat body tone as MAIN
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (ak::kPageMarginX + 8, 12);
        heading.setBounds (r.removeFromTop (24));
        help.setBounds (r.removeFromTop (40));
        r.removeFromTop (6);

        // left column: the pad, with the scene row under it
        const int leftW = juce::jlimit (280, 460, r.getWidth() * 42 / 100);
        auto left = r.removeFromLeft (leftW);
        r.removeFromLeft (ak::kGap * 2);
        // Cap the control column: a slider row stretched across 900 px reads
        // as a different instrument from the MAIN grid, where the same rows
        // sit in a third of the window. Extra width is left as margin.
        r = r.withWidth (juce::jmin (r.getWidth(), 560));

        // The pad takes a square off the TOP of the column and the scenes sit
        // straight underneath it. Reserving the scene row off the bottom
        // instead leaves the buttons pinned to the foot of the window, a long
        // way from the pad they act on.
        const int s = juce::jmax (200, std::min (left.getWidth(), left.getHeight() - 60));
        pad.setBounds (left.removeFromTop (s).withSizeKeepingCentre (s, s));
        left.removeFromTop (8);
        sceneLbl.setBounds (left.removeFromTop (15));
        auto sceneRow = left.removeFromTop (30);
        const int nb = scenes.size();
        if (nb > 0)
        {
            const int bw = (sceneRow.getWidth() - (nb - 1) * 6) / nb;
            for (int i = 0; i < nb; ++i)
            {
                scenes[i]->setBounds (sceneRow.removeFromLeft (bw).reduced (0, 2));
                sceneRow.removeFromLeft (6);
            }
        }

        // right column: the cards, stacked
        for (auto* c : { &cardPos, &cardBin, &cardRoom, &cardMove })
        {
            c->setBounds (r.removeFromTop (c->preferredHeight()));
            r.removeFromTop (ak::kGap);
        }
    }

private:
    static juce::String tipOf (const char* en, const char* jp) { return vmTip (en, jp); }

    void add (ak::Card& card, const char* id, ParamRow::Kind kind,
              const juce::String& label, ak::Tone tone, const juce::String& tipText)
    {
        auto row = std::make_unique<ParamRow> (proc, id, kind, label, tipText, tone);
        row->setLookAndFeel (tone == ak::Tone::pink   ? (juce::LookAndFeel*) &lnfPink
                           : tone == ak::Tone::yellow ? (juce::LookAndFeel*) &lnfYellow
                                                      : (juce::LookAndFeel*) &lnfBlue);
        // a lock toggled here has to reach the MAIN page's rows too, and the
        // editor's poll is what does that -- so tell it something changed
        row->onLockChanged = [this] { if (onLockChanged) onLockChanged(); };
        card.add (*row, kind == ParamRow::Kind::toggle ? ak::kToggleH : ak::kRowH);
        rows.push_back (std::move (row));
    }

    VoxMorphProcessor& proc;
    // The look-and-feels are declared BEFORE the rows that point at them:
    // members die in reverse declaration order, so this is what guarantees
    // every row is gone before the LookAndFeel it references. (Same ordering
    // as VoxMorphEditor's own lnf* / owned pair, for the same reason.)
    ak::ToneLookAndFeel lnfBlue   { ak::Tone::blue };
    ak::ToneLookAndFeel lnfPink   { ak::Tone::pink };
    ak::ToneLookAndFeel lnfYellow { ak::Tone::yellow };
    juce::Label heading, help, sceneLbl;
    SonarPad pad;
    juce::OwnedArray<juce::TextButton> scenes;
    ak::Card cardPos, cardBin, cardRoom, cardMove;
    std::vector<std::unique_ptr<ParamRow>> rows;
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
            // Never store what the LIST says: an offline device is shown with
            // a "(未接続 / offline)" suffix, and storing that decorated label
            // as a device name is what used to knock out the input and output
            // selection when MONITOR was pressed (see vmSetOutputDevice).
            const int sel = monBox.getSelectedId();
            proc.monitorDeviceName = sel <= 1 ? juce::String()
                                              : monNames[sel - 2];
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
        // monNames[id - 2] is the REAL device name behind each item; the item
        // text may carry an "offline" suffix and must never be used as one.
        monNames.clear();
        int id = 2, selId = 1;
        for (const auto& n : names)
        {
            monBox.addItem (n, id);
            monNames.add (n);
            if (n == proc.monitorDeviceName) selId = id;
            ++id;
        }
        // a device saved earlier but not currently connected must not be lost
        if (selId == 1 && proc.monitorDeviceName.isNotEmpty())
        {
            monBox.addItem (proc.monitorDeviceName
                              + juce::String::fromUTF8 ("  (未接続 / offline)"), id);
            monNames.add (proc.monitorDeviceName);
            selId = id;
        }
        monBox.setSelectedId (selId, juce::dontSendNotification);
    }

    VoxMorphProcessor& proc;
    juce::LookAndFeel_V4 lnf { juce::LookAndFeel_V4::getLightColourScheme() };
    juce::TooltipWindow  tips { this, 400 };
    juce::StringArray monNames;        // real device name per monBox item
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
        ak::styleSectionHeading (heading);
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

        holdLongRow = std::make_unique<ParamRow> (proc, "onsetholdlong", ParamRow::Kind::toggle,
            "Onset Hold Long",
            vmTip ("EXPERIMENTAL. Lets Onset Hold (MAIN tab) hold for longer - about 58 ms "
                   "instead of 35 - and keeps following the pitch while it holds instead of "
                   "freezing it. It only affects dropouts in the MIDDLE of a phrase, not the "
                   "start; measured on a real take it removed two of twenty-two of them over "
                   "110 seconds, so the difference is small. Off = the standard behaviour.",
                   "MAINタブのOnset Holdの保持時間を約35ms→58msに延ばし、保持中も音程の追従を"
                   "続けます。効くのは句の「途中」で起きる脱落だけで、句の頭には効きません。"
                   "実声での測定では110秒中22回が20回になる程度なので、差はわずかです。"
                   "オフ=標準の動作。"));
        addAndMakeVisible (*holdLongRow);

        relRow = std::make_unique<ParamRow> (proc, "relshelf", ParamRow::Kind::slider,
            "Release Suppression",
            vmTip ("EXPERIMENTAL. The other end of the phrase. Onset Repair works on the START of "
                   "a phrase; until now the engine could not tell a phrase ENDING apart from one, "
                   "so the same treatment ran there and took 4-6 dB out of the voice band as the "
                   "sound died away. The engine now knows the difference, and this is what it "
                   "uses on endings instead: a much gentler version, aimed only at the low "
                   "remains of your untransposed voice. Start around 0.5. 0 = off, and endings "
                   "then get no treatment at all.",
                   "句の「終わり」側です。Onset Repairは句の頭を直す機能ですが、これまでエンジンは"
                   "句の終わりと頭を区別できず、語尾にも同じ処理がかかって、消えていく声の帯域を"
                   "4〜6dB削っていました。エンジンが両者を区別できるようになったので、語尾には"
                   "こちらを使います。ずっと浅い処理で、狙うのは変換前の地声の低い残りだけです。"
                   "0.5あたりから試してください。0=オフ(語尾には何もしません)。"));
        addAndMakeVisible (*relRow);

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

        setSize (600, 348);
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
        holdLongRow->setBounds (r.removeFromTop (28));
        r.removeFromTop (4);
        relRow->setBounds (r.removeFromTop (30));
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
    std::unique_ptr<ParamRow> gciRow, breathRow, holdLongRow, relRow, lowLatRow;
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
// ---------------------------------------------------------------------------
// The character badge in the middle of the band (v0.36.13).
//
// Just the portrait now. The spectrum arc that used to ride along the bottom
// of it was dropped: at this size it was too small to read anything from, and
// keeping it alive forced the FFT and the audio thread's taps to run on every
// page. The analysis is back to running only while the Visualizer page is up.
//
// The picture can be the user's own: click the badge for "Choose image..." /
// "Use the default". The chosen path lives in the processor's state, so it
// comes back with the session. Any aspect ratio works — the image is scaled
// to FILL the circle and centre-cropped.
//
// The rim still brightens with the output level, which is the one live thing
// worth keeping here: it says "sound is leaving the plugin" at a glance.
class HeroCircle : public juce::Component, public juce::SettableTooltipClient,
                   private juce::Timer
{
public:
    explicit HeroCircle (VoxMorphProcessor& p) : proc (p)
    {
        setTooltip (vmTip (
            "MATCH sets this to the character you matched to. You can also "
            "choose your own from the preset dropdown (\"Choose character "
            "image...\"). It is stored in the preset, so each preset carries "
            "its own picture.",
            "MATCHを実行すると、合わせたキャラクターの画像がここに入ります。"
            "プリセット選択プルダウンの「キャラクター画像を選択...」から自分で"
            "選ぶこともできます。プリセットに保存されるので、プリセットごとに"
            "別の画像を持たせられます。"));
        startTimerHz (4);            // only watches for the picture changing
        reloadImage();
    }

    // Re-reads proc.characterImagePath. Called on construction and after the
    // host restores a session, so a saved picture comes back on its own.
    //
    // v0.42.0: the string is either a FILE path (chosen by the user) or
    // "builtin:<BinaryData name>" for one of the shipped character portraits,
    // which is what MATCH writes. One field rather than two, so persistence,
    // preset round-trip and the change-watching timer all keep working
    // unchanged -- and a built-in picture survives being moved between
    // machines, which a file path deliberately does not.
    void reloadImage()
    {
        custom = juce::Image();
        const auto path = proc.characterImagePath;
        if (path.startsWith (kBuiltinImagePrefix))
            custom = ak::image (path.fromFirstOccurrenceOf (kBuiltinImagePrefix, false, false)
                                    .toRawUTF8());
        else if (path.isNotEmpty())
        {
            const juce::File f (path);
            if (f.existsAsFile())
                custom = juce::ImageFileFormat::loadFrom (f);
        }
        loadedFrom = path;
        repaint();
    }

    // The bounds are a SQUARE around the badge, but only the disc is ours:
    // without this the corners would swallow clicks over the band.
    bool hitTest (int x, int y) override
    {
        auto b = getLocalBounds().toFloat();
        const float r = juce::jmin (b.getWidth(), b.getHeight()) * 0.5f;
        return b.getCentre().getDistanceFrom ({ (float) x, (float) y }) <= r;
    }

private:
    void timerCallback() override
    {
        if (! isShowing()) return;
        // the host may have restored a different picture under us
        if (loadedFrom != proc.characterImagePath) reloadImage();

    }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat();
        const float side   = juce::jmin (b.getWidth(), b.getHeight());
        const auto  c      = b.getCentre();
        const float outerR = side * 0.5f;
        // the component spans the portrait AND the collar the band bulges out
        const float portR  = outerR * (float) ak::kHeroR
                                    / (float) (ak::kHeroR + ak::kHeroRim);
        const auto  port   = juce::Rectangle<float> (portR * 2.0f, portR * 2.0f).withCentre (c);

        {
            juce::Graphics::ScopedSaveState ss (g);
            juce::Path clip;
            clip.addEllipse (port);
            g.reduceClipRegion (clip);
            g.setColour (juce::Colours::white);
            g.fillEllipse (port);
            // fillDestination scales to cover and centre-crops, so any shape
            // of picture fills the circle without distortion
            const auto& art = custom.isValid() ? custom : ak::image ("ui_hero_character_png");
            if (art.isValid())
                g.drawImage (art, port, juce::RectanglePlacement::fillDestination, false);
        }

    }

    VoxMorphProcessor& proc;
    juce::Image  custom;
    juce::String loadedFrom;
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
//  - The spectrum, the AEIOU vowel mix and the four level meters live on
//    the VISUALIZER page.
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
        asmrPanel.onLockChanged = [this] { syncLockUI(); };

        footer.setFont (ak::font (11.0f));
        footer.setColour (juce::Label::textColourId, ak::heading.withAlpha (0.85f));
        footer.setJustificationType (juce::Justification::centredLeft);
        // No standing text (v0.36.8). The label stays: flashFooter() still
        // uses it for transient confirmations, and clears back to empty.
        footer.setText ({}, juce::dontSendNotification);
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
            proc.uiWantsMeters.store (levels.isShowing() || outLamps.isShowing(),
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
        // spans the portrait AND the collar: the mini visualiser's ring is
        // drawn in the collar, so the component has to reach heroOuter
        hero.setBounds (juce::Rectangle<int> ((int) (heroOuter * 2.0f), (int) (heroOuter * 2.0f))
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
        // MAIN pushes only its middle column past the bulge; the other pages
        // are single blocks, so they start below it altogether. Without this
        // their top row sits on top of the dark collar.
        const auto sidePages = pageArea.withTop (
            juce::jmax (pageArea.getY(), bulgeBottom + 12));
        for (auto* pg : pages)
        {
            // MATCHING is the third case: it gets the FULL content area like
            // MAIN, but unlike MAIN it is not scrolled, so it is handed the
            // circle's footprint and tucks its description in beside it.
            // Starting it below the bulge like the other pages left a dead
            // band the width of the window.
            // MATCHING is the third case again, and for a second reason: its
            // character strip is full-bleed, and a component cannot paint
            // outside its own bounds, so it is given the WHOLE window width
            // and re-applies the page inset internally (MatchingPanel::kEdge).
            if (pg == &matchingPanel)
                pg->setBounds (pageArea.withX (0).withWidth (getWidth()));
            else
                pg->setBounds (pg == &mainScroll ? pageArea : sidePages);
        }
        matchingPanel.setBulge ({ (int) heroC.x - matchingPanel.getX(),
                                  (int) heroC.y - matchingPanel.getY() },
                                ak::kHeroR + ak::kHeroRim,
                                bulgeBottom - matchingPanel.getY() + 10);
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
        rowPitch = &knob (*cardPitch, "pitch", "Pitch (st)",
            tip ("Shifts the pitch in semitones. The timbre (formants) stays unchanged. "
                 "Male-to-female: +5 to +7. Female-to-male: around -5.",
                 "声の高さを半音単位で変えます。声色(フォルマント)は変わりません。"
                 "女声化は+5〜+7、男声化は-5前後が目安。"), ak::Tone::blue);
        rowPitch->setSignedValue();
        // Robotize / Robot Pitch were dropped from the UI in v0.35.0 (unused).
        // The parameters stay registered, so saved sessions and presets that
        // carry them still load and behave exactly as before.

        cardInton = &newCard ("INTONATION", "ui_mark_M_Intonation_png", ak::headBlue);
        slider (*cardInton, "range", "Intonation (%)",
            tip ("Exaggerates or flattens the pitch movement (intonation). 100% = unchanged. "
                 "Unlike 'Pitch', which moves the whole voice up or down, this scales only the movement. "
                 "110-140% recommended for male-to-female.",
                 "声の抑揚(音程の上がり下がり)を強調/抑制します。100%=変化なし。"
                 "Pitchが声全体を平行移動するのに対し、こちらは動きの幅だけを変えます。"
                 "女声化では110〜140%が目安です。"));
        slider (*cardInton, "center", "Center (Hz)",
            tip ("The pitch that intonation scaling expands around. Set it near the average pitch "
                 "of the converted voice (200-250 Hz for a female voice). No effect at 100% Amount.",
                 "抑揚を拡大/縮小するときの中心になる音程。変換後の声の平均的な高さに"
                 "合わせてください(女声なら200〜250Hz)。Amountが100%のときは無効。"));

        // The ADVANCED mark shares its art with the flow-line markers, which
        // draw it in treeLine — tinting it here pins it to the heading blue
        // no matter what else asks for a recoloured copy.
        cardAdvanced = &newCard ("ADVANCED", "ui_mark_M_Advanced_png",
                                 ak::headBlue, ak::markBlue);
        toggle (*cardAdvanced, "pulsesmooth", "Pulse Smoothing",
            tip ("Removes the low growl that a large upward shift can add. Voices are rarely "
                 "perfectly regular - most have a slight alternation between one glottal pulse "
                 "and the next - and when the pitch is raised a lot, that alternation stays "
                 "behind at the ORIGINAL pitch and is heard as a rumble underneath the new "
                 "voice. This averages each pulse with the one before it, which cancels the "
                 "alternating part and leaves the voice's own body untouched. It only does "
                 "anything above about +4 semitones, so smaller shifts sound exactly as before. "
                 "Turn it off if you want the raw grain behaviour back.",
                 "大きく上げたときに乗る低い唸り(ゴロゴロ音)を取り除きます。声は完全に規則的では"
                 "なく、多くの場合で声門パルスが1つおきにわずかに強弱します。ピッチを大きく上げると"
                 "その強弱だけが元のピッチの位置に取り残され、新しい声の下に唸りとして聞こえます。"
                 "各パルスを1つ前のパルスと平均することで、この交互成分だけを打ち消し、声の芯は"
                 "そのまま残します。おおよそ+4半音より上でしか動作しないので、小さい変換の音は"
                 "従来と完全に同じです。元のグレイン動作に戻したい場合はオフにしてください。"));
        toggle (*cardAdvanced, "onsethold", "Onset Hold",
            tip ("Stops a thump at the start of phrases. The engine has to decide, many times a "
                 "second, whether what it is hearing is a pitched voice or not - and the test it "
                 "uses compares a slice of sound with a slightly delayed copy of itself. During "
                 "the swell at the start of a phrase the two slices differ in loudness even "
                 "though the voice is perfectly steady, so the test fails for a few hundredths "
                 "of a second and the engine treats your voice as unpitched. Unpitched sound is "
                 "passed through WITHOUT the pitch change, so a burst of your own untransposed "
                 "voice escapes at the loudest moment of the attack - which is heard as a knock "
                 "or thump. This keeps the previous pitch through those few frames, but only "
                 "while the sound still looks like a voice, so consonants like S and SH are "
                 "unaffected, and the pitch keeps being tracked through the hold so it does not go stale. Leave it on unless you want the old behaviour back.",
                 "句の頭で鳴る「ボコっ」という打撃音のような音を止めます。エンジンは1秒に何度も"
                 "「今聞こえているのは音程のある声かどうか」を判定していますが、その判定は音の"
                 "一部と少しずらした自分自身を比べる方式です。発声の立ち上がりでは音量が急に"
                 "大きくなるため、声そのものは安定していても比較する2つの音量が食い違い、"
                 "数十ミリ秒だけ判定に失敗して「音程の無い音」と見なされます。音程の無い音は"
                 "ピッチ変換をせずにそのまま通すので、立ち上がりのいちばん大きいところで"
                 "変換前の低い地声が一瞬漏れ、それが打撃音のように聞こえます。この機能は"
                 "その数フレームだけ直前の音程を保持します。ただし「まだ声に見える」間だけ"
                 "なので、サ行などの子音には影響しません。保持中も音程の追従は続けるため、古い音程に貼り付いたままにはなりません。従来の動作に戻したいとき以外は"
                 "オンのままにしてください。"));
        toggle (*cardAdvanced, "onsetbackfill", "Onset Repair",
            tip ("Fixes the low growl at the start of phrases. The engine prepares sound slightly "
                 "ahead of what you hear, so at the moment it works out the pitch of a new "
                 "phrase, roughly 27 ms of the opening has been prepared but not played - "
                 "prepared without the pitch change, because nothing knew it yet. This throws "
                 "that away and redoes it with the pitch in hand, and takes the bottom out of "
                 "whatever was already gone (see Repair Strength below). It adds no delay. Turn "
                 "it off to get the behaviour from before v0.57.0 back exactly.",
                 "句の頭で鳴る低い唸りを直します。エンジンは聞こえている音より少し先まで音を"
                 "用意しているので、新しい句の音程を掴んだ時点で、出だしの約27ms分が"
                 "「まだ再生されていないが、音程が分からないまま用意された」状態で残っています。"
                 "この機能はそれを捨てて、掴んだ音程で作り直し、間に合わなかった分は低域を"
                 "削って処理します(下のRepair Strength)。遅延は増えません。"
                 "オフにすると v0.57.0 以前の動作に完全に戻ります。"));
        slider (*cardAdvanced, "prelowcut", "Onset Repair Strength",
            tip ("How hard to cut the bottom out of the part of the phrase opening that could not "
                 "be re-rendered - the part already sent on before the pitch was known. 0 leaves "
                 "it alone and relies on the re-render only; 1 removes about 24 dB below the "
                 "speaker's own pitch. The default 0.75 is the setting chosen by ear. The airy "
                 "top of the attack always passes, so consonants keep their bite, and anything "
                 "that looks like S or SH is left alone. Only active while Onset Repair is on, "
                 "and only on upward shifts.",
                 "句の出だしのうち、作り直しが間に合わなかった部分(音程が分かる前に既に送り出して"
                 "しまった部分)の低域をどれだけ削るか。0=削らず作り直しだけに任せる、"
                 "1=話者自身の音程より下を約24dB削ります。既定の0.75は試聴で選ばれた値です。"
                 "立ち上がりの空気感は常に通すので子音の勢いは保たれ、サ行のように見える音には"
                 "手を付けません。Onset Repairがオンのとき、かつ上げ方向のシフトのときだけ"
                 "動作します。"));
        slider (*cardAdvanced, "pulsebody", "Pulse Body",
            tip ("How much of each glottal pulse survives a large upward shift. To stop a raised "
                 "voice sounding like two voices at once, the engine cuts a shorter slice out of "
                 "your voice the further up you go - and past about +7 semitones that slice ends "
                 "before the pulse has finished closing, so the output waveform is a sharper, "
                 "more one-sided spike than your own voice ever was. Raising this hands the "
                 "missing tail back; measured on a real take, 0.75 (the default) puts the "
                 "waveform's shape exactly where the original recording was. It costs about 2 dB "
                 "of low end, so higher settings sound brighter and thinner. It does nothing on "
                 "downward shifts. 0 = the behaviour before v0.52.0.",
                 "大きく上げたときに、声門パルスをどれだけ残すか。ピッチを上げるほど「二重声」を"
                 "避けるためにグレインを短く切る必要があり、+7半音あたりから声門が閉じ切る前で"
                 "パルスが切れます。その結果、出力波形は元の声より鋭く上下非対称なスパイクに"
                 "なります。この値を上げると切れていた後半が戻ります。実声で測ったところ、"
                 "既定の0.75で波形の非対称性が原音とほぼ同じになりました。低域が約2dB減るため、"
                 "上げるほど明るく細い音になります。下げ方向のシフトでは何もしません。"
                 "0=v0.52.0より前の動作。"));
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
        rowFormant = &knob (*cardFormant, "formant", "Formant (st)",
            tip ("Changes the vocal-tract size = the timbre, without changing pitch. "
                 "+ sounds younger/feminine, - sounds deeper/masculine. +3 to +4 for male-to-female.",
                 "声道の長さ=声の響き・声色を変えます。ピッチは変わりません。"
                 "+で若く/女性的に、-で太く/男性的に。女声化は+3〜+4が目安。"), ak::Tone::pink);
        rowFormant->setSignedValue();
        addFmtCharacterRow();
        auto& rConst = slider (*cardFormant, "consonant", "Co Shift (st)",
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
        auto& rF2S = slider (*cardFormant, "f2shift", "F2 Shift (st)",
            tip ("Moves only the second formant (tongue position). The strongest single cue for "
                 "perceived gender/age of the vowels: +2 to +4 sounds younger and more feminine.",
                 "第2フォルマント(舌の位置)だけを動かします。母音の性別・年齢感に最も効く帯域で、"
                 "+2〜+4で若く女性的に聞こえます。"), ak::Tone::pink);
        auto& rF3S = slider (*cardFormant, "f3shift", "F3 Shift (st)",
            tip ("Moves only the third formant (front cavity / lip area). Small shifts (+1 to +2) "
                 "refine the impression of a shorter vocal tract.",
                 "第3フォルマント(声道前部・唇まわり)だけを動かします。+1〜+2の小さめの操作で"
                 "「声道が短い」印象を仕上げます。"), ak::Tone::pink);
        auto& rF1G = slider (*cardFormant, "f1gain", "F1 Gain (dB)",
            tip ("Boost or cut around the first formant. Cutting a few dB thins out a 'boomy' "
                 "chest resonance.",
                 "第1フォルマント付近の強さ。数dB下げると胸に響く「太さ」が抜けます。"), ak::Tone::pink);
        auto& rF2G = slider (*cardFormant, "f2gain", "F2 Gain (dB)",
            tip ("Boost or cut around the second formant. A few dB of boost adds clarity and "
                 "'presence' to the vowels.",
                 "第2フォルマント付近の強さ。数dB上げると母音の明瞭さ・華やかさが出ます。"), ak::Tone::pink);
        auto& rF3G = slider (*cardFormant, "f3gain", "F3 Gain (dB)",
            tip ("Boost or cut around the third formant. Boosting adds sheen and 'sparkle' - "
                 "this region carries much of a voice's charm.",
                 "第3フォルマント付近の強さ。上げると艶・張りが出ます。声の「華」が乗る帯域です。"), ak::Tone::pink);
        // v0.44.0: Definition became the eighth component of a character, so
        // it joins the bracket and the back-to-Custom set. Both halves matter
        // -- a row a preset WRITES but that does not knock the dropdown back
        // to Custom would let the label keep claiming a character the user
        // has already edited away from.
        auto& rDef = slider (*cardFormant, "resonance", "Formant Definition (%)",
            tip ("Sharpens or rounds the formant peaks without moving them. + deepens the dips "
                 "between the resonances so the vowels read as more defined and characterful; "
                 "- fills them in for a softer, more distant voice. Different from Softness/Tilt "
                 "(which tips low against high) and from F1-F3 Gain (which move one region). "
                 "Try +/-20 to 40 first.",
                 "フォルマントの中心位置は動かさず、山の輪郭だけを調整します。"
                 "+で共鳴の谷が深くなり母音がくっきり・キャラクター的に、"
                 "-で丸く柔らかく、距離感のある声になります。"
                 "Softness/Tilt(低域と高域の傾き)やF1〜F3 Gain(特定帯域の強さ)とは別の軸です。"
                 "まずは±20〜40程度から。"), ak::Tone::pink);
        rDef.setSignedValue();
        bracket ({ &rConst, &rF1S, &rF2S, &rF3S, &rF1G, &rF2G, &rF3G, &rDef });
        // picking a character writes these eight; moving any of them by hand
        // puts the dropdown back to Custom
        for (auto* r : { &rConst, &rF1S, &rF2S, &rF3S, &rF1G, &rF2G, &rF3G, &rDef })
            r->onUserEdit = [this] { setFmtCharacterCustom(); };
        cardFormant->addGap (14);          // blank line before AEIOU

        toggle (*cardFormant, "vadapt", "AEIOU",
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
        slider (*cardQuality, "gate", "Noise Gate (dB)",
            tip ("Mutes the input while it stays below this level - removes fan / room noise "
                 "between phrases. -80 = off. Set it just above your noise floor (try -55 to -45).",
                 "入力がこのレベルを下回っている間ミュートし、話していない間のファンノイズや"
                 "環境音を消します。-80=オフ。ノイズの音量より少し上に設定してください"
                 "(目安 -55〜-45)。"));


        // -- bottom row ------------------------------------------------
        cardHigh = &newCard ("HIGH RANGE / LOW LIMIT", "ui_mark_M_HighRange_png", ak::headBlue);
        toggle (*cardHigh, "hienable", "High Range",
            tip ("Switches the high-range guard on and off in one go - the three settings "
                 "bracketed under it. Off leaves loud, high notes converted by the normal "
                 "Pitch and Formant amounts.",
                 "高音域ガード(下にぶら下がる3項目)をまとめてオン/オフします。"
                 "オフのときは、高い声もPitch/Formantの通常の変化量のまま変換されます。"));
        auto& rHiF = slider (*cardHigh, "hifreq", "High Pitch Roof (Hz)",
            tip ("When your INPUT pitch (before conversion) rises above this - laughing, squealing, "
                 "exclamations - the Pitch/Formant shifts blend smoothly toward the High amounts "
                 "below, reaching them fully one octave up. Stops laughs from being shifted into "
                 "unnaturally high tones. 0 = off. Try 250-350 Hz.",
                 "入力(変換前)のピッチがこの値を超えると(笑い声・叫び・感嘆など)、ピッチ/"
                 "フォルマントの変化量が下のHigh設定へ滑らかに移行し、1オクターブ上で完全に"
                 "切り替わります。笑い声が不自然な高音まで上がるのを防ぎます。0=オフ。"
                 "250〜350Hzが目安。"));
        auto& rHiP = slider (*cardHigh, "hipitch", "High Pitch Keep (%)",
            tip ("How much of the Pitch shift remains in the high range. 100% = same as normal, "
                 "0% = no shift there (laughs keep their natural pitch). Try 30-60%.",
                 "高音域で残すPitchシフトの割合。100%=通常と同じ、0%=シフトなし(笑い声は"
                 "地声の高さのまま)。30〜60%が目安。"));
        auto& rHiA = slider (*cardHigh, "hiformant", "High Fmt Keep (%)",
            tip ("How much of the Formant shift remains in the high range. Usually leave at 100% "
                 "so the voice keeps its character while only the pitch settles down.",
                 "高音域で残すFormantシフトの割合。通常は100%のまま(声色は保ちつつピッチだけ"
                 "落ち着かせる)が自然です。"));
        bracket ({ &rHiF, &rHiP, &rHiA });
        cardHigh->addGap (8);              // blank line before the next group

        toggle (*cardHigh, "lowlimit", "Low Limit",
            tip ("Switches the low-pitch floor below on and off. Off lets the converted "
                 "pitch fall as low as it likes.",
                 "下のLow Pitch Floorをオン/オフします。オフのときは変換後のピッチが"
                 "どこまで低くなっても引き上げません。"));
        slider (*cardHigh, "pitchfloor", "Low Pitch Floor (Hz)",
            tip ("If the converted pitch falls below this, it is lifted softly toward the floor. "
                 "Useful when your voice drifts too low while speaking. 0 = off. "
                 "Try 140-180 with a female target voice.",
                 "変換後のピッチがこの値を下回ったとき、滑らかに引き上げます。"
                 "話しているうちに声が低くなりすぎる場合の補正用。0=オフ。"
                 "女声化なら140〜180が目安です。"))
            .setTree (ParamRow::Tree::last);

        cardOutput = &newCard ("OUTPUT", "ui_mark_M_Output_png", ak::headBlue);
        slider (*cardOutput, "gain", "Gain (dB)",
            tip ("Output level of the plugin, to compensate loudness changes from the conversion.",
                 "プラグインの出力レベル。変換で音量感が変わったときの補正用。"));
        slider (*cardOutput, "mix", "Mix",
            tip ("Balance between the converted voice (1.0) and the original (0.0). Usually 1.0.",
                 "変換した声(1.0)と元の声(0.0)の割合。通常は1.0のままにします。"));
        cardOutput->add (outLamps, 24);
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

    // Fmt Character: a preset for the eight global formant rows below it.
    // Nothing is hidden or disabled while one is selected — the rows stay
    // live, and touching any of them drops the dropdown back to Custom.
    void addFmtCharacterRow()
    {
        fmtCombo = std::make_unique<ParamRow> (proc, "fcharacter", ParamRow::Kind::combo,
            "Fmt Character",
            tip ("Presets the eight rows below (Co Shift, F1-F3 shift / gain and "
                 "Formant Definition) in one "
                 "move. The rows stay editable: change any of them and this goes back to "
                 "Custom. Same character names as AEIOU Character, but this shifts the "
                 "whole vocal tract while that one shapes the five vowels apart.",
                 "下の8項目(Co Shift、F1〜F3のShift/Gain、Formant Definition)を"
                 "まとめて設定します。"
                 "各項目はそのまま操作でき、どれか1つでも動かすとCustomに戻ります。"
                 "名前はAEIOU Characterと共通ですが、あちらが母音ごとの差を"
                 "作るのに対し、こちらは声道全体を動かします。"),
            ak::Tone::pink);
        fmtCombo->setLookAndFeel (&lnfPink);
        fmtCombo->onLockChanged = [this] { syncLockUI(); };
        rows.push_back (fmtCombo.get());
        cardFormant->add (*fmtCombo, 32);

        // The ComboBoxAttachment listens as a ComboBox::Listener, and JUCE
        // calls listeners BEFORE onChange — so by the time this runs the
        // parameter already holds the new choice.
        fmtCombo->getCombo().onChange = [this]
        {
            applyFmtCharacter (fmtCombo->getCombo().getSelectedItemIndex());
        };
    }

    void applyFmtCharacter (int index)
    {
        if (index < 0 || index >= kFmtCustom) return;      // Custom writes nothing
        const auto& m = getFmtCharacterMap (index);

        // One undo step for the whole character, and locked rows are left
        // alone — same contract as the AEIOU detail window's Copy to Custom.
        proc.history.group ([&]
        {
            auto put = [this] (const char* id, float v)
            {
                if (proc.isParamLocked (id)) return;
                if (auto* rp = proc.apvts.getParameter (id))
                {
                    rp->beginChangeGesture();
                    rp->setValueNotifyingHost (rp->convertTo0to1 (v));
                    rp->endChangeGesture();
                }
            };
            put ("consonant", m.consonantSt);
            put ("f1shift", m.shiftSt[0]);  put ("f1gain", m.gainDb[0]);
            put ("f2shift", m.shiftSt[1]);  put ("f2gain", m.gainDb[1]);
            put ("f3shift", m.shiftSt[2]);  put ("f3gain", m.gainDb[2]);
            put ("resonance", m.definitionPct);
        });
    }

    void setFmtCharacterCustom()
    {
        if (proc.isParamLocked ("fcharacter")) return;
        if (auto* cp = proc.apvts.getParameter ("fcharacter"))
            if (cp->convertFrom0to1 (cp->getValue()) < (float) kFmtCustom - 0.5f)
            {
                cp->beginChangeGesture();
                cp->setValueNotifyingHost (cp->convertTo0to1 ((float) kFmtCustom));
                cp->endChangeGesture();
            }
    }

    // AEIOU character dropdown, then DETAIL... on its own line beneath it
    void addAeiouRow()
    {
        aeiouCombo = std::make_unique<ParamRow> (proc, "vcharacter", ParamRow::Kind::combo,
            "AEIOU Character",
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
    // v0.46.0: the spectrum, and under it one short strip carrying the vowel
    // mix and the four levels. The radial ("donut") spectrum that used to sit
    // bottom-left is gone — it drew the same numbers as the graph above it,
    // and a third of the page is a lot to pay for the same reading twice.
    // Heading and help text are laid out like the ASMR page's, so the two
    // non-MAIN pages open the same way.
    void buildVisualizerPage()
    {
        specData.addView (&spectrum);
        vizPage.addAndMakeVisible (spectrum);
        vizPage.addAndMakeVisible (detect);
        vizPage.addAndMakeVisible (vowel);
        vizPage.addAndMakeVisible (levels);

        vizHead.setText ("VISUALIZER", juce::dontSendNotification);
        ak::styleSectionHeading (vizHead);
        vizPage.addAndMakeVisible (vizHead);

        vizNote.setFont (ak::font (11.5f));
        vizNote.setColour (juce::Label::textColourId, ak::heading.withAlpha (0.9f));
        vizNote.setJustificationType (juce::Justification::topLeft);
        vizNote.setText (juce::String::fromUTF8 (
            "入力(青)と変換後(ピンク)を重ねたスペクトラムです。表示だけの機能で、"
            "音には一切影響しません。\n"
            "すぐ下の細い帯は同じ周波数軸で、検出したピッチ(ひし形)とフォルマント(丸)の"
            "入力→出力の位置を示します。最下段は母音の割合と入出力レベルのL/Rです。"),
            juce::dontSendNotification);
        vizPage.addAndMakeVisible (vizNote);

        vizPage.fn = [this]
        {
            auto r = vizPage.getLocalBounds().reduced (ak::kPageMarginX + 8, 12);
            vizHead.setBounds (r.removeFromTop (24));
            vizNote.setBounds (r.removeFromTop (36));
            r.removeFromTop (6);

            // The strip is a fixed short band off the BOTTOM and the spectrum
            // takes everything left over: growing the window should make the
            // graph bigger, not stretch two rows of bars.
            auto strip = r.removeFromBottom (juce::jlimit (124, 152, r.getHeight() / 4));
            r.removeFromBottom (ak::kGap);
            // The lane must keep the spectrum's EXACT x and width — that is
            // what makes vmAxis line the two up — so it is taken off the
            // bottom of the same rectangle and nothing insets it sideways.
            detect.setBounds (r.removeFromBottom (DetectionLane::kHeight));
            r.removeFromBottom (4);
            spectrum.setBounds (r);

            const int vw = juce::jlimit (230, 430, strip.getWidth() * 38 / 100);
            vowel .setBounds (strip.removeFromLeft (vw));
            strip.removeFromLeft (ak::kGap);
            levels.setBounds (strip);
        };
    }

    // ---- MAIN page grid --------------------------------------------------
    // Three columns, no bottom row (v0.35.0):
    //   1  PITCH / INTONATION / HIGH RANGE
    //   2  FORMANT              (pushed down past the character's bulge)
    //   3  AIR / ADVANCED / VOICE QUALITY / OUTPUT
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
        const int col1 = stack ({ cardPitch, cardInton, cardHigh });
        const int col2 = c2Top + cardFormant->preferredHeight();
        const int col3 = stack ({ cardAir, cardAdvanced, cardQuality, cardOutput });

        const int footerH = 22;
        mainPage.setSize (vw, juce::jmax (juce::jmax (col1, juce::jmax (col2, col3)) + footerH + 10,
                                          pageArea.getHeight()));

        auto r = mainPage.getLocalBounds();
        footer.setBounds (r.removeFromBottom (footerH + 4).withTrimmedTop (4));

        r.reduce (ak::kPageMarginX, 0);          // breathing room at both edges

        const int usable = r.getWidth() - 2 * ak::kGap;
        // The side columns take kMidTrim/2 each, so the FORMANT column in the
        // middle ends up kMidTrim narrower without changing the total.
        const int w1 = juce::jmax (250, (int) ((float) usable * 0.92f / 3.16f))
                         + ak::kMidTrim / 2;
        const int w2 = usable - w1 * 2;

        auto c1 = r.removeFromLeft (w1);   r.removeFromLeft (ak::kGap);
        auto c2 = r.removeFromLeft (w2);   r.removeFromLeft (ak::kGap);
        auto c3 = r;

        auto place = [] (juce::Rectangle<int>& col, ak::Card* card)
        {
            card->setBounds (col.removeFromTop (card->preferredHeight()));
            col.removeFromTop (ak::kGap);
        };
        place (c1, cardPitch);  place (c1, cardInton);  place (c1, cardHigh);

        c2.removeFromTop (c2Top);            // clear the character's bulge
        cardFormant->setBounds (c2.removeFromTop (cardFormant->preferredHeight()));

        place (c3, cardAir);      place (c3, cardAdvanced);
        place (c3, cardQuality); place (c3, cardOutput);
    }

    // ---- misc ------------------------------------------------------------
    void syncLockUI()
    {
        for (auto* r : rows) r->refreshLock();
        asmrPanel.refreshLocks();       // its rows are owned by the panel
        lastLockState = proc.lockedIds.joinIntoString (",");
    }

    void flashFooter (const juce::String& msg)
    {
        footer.setText (msg, juce::dontSendNotification);
        // v0.47.0: was a confirmation green, the last green left in the
        // palette after the input series moved to blue
        footer.setColour (juce::Label::textColourId, ak::seriesIn.darker (0.35f));
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
    std::unique_ptr<ParamRow> fmtCombo;
    juce::TextButton detailBtn;
    std::unique_ptr<PresetBar> presetBar;
    StatusView  status   { proc };
    OutputLamps outLamps { proc };
    juce::Label footer;
    juce::String defaultFooterText;

    // VISUALIZER page
    FnComponent  vizPage;
    juce::Label  vizHead, vizNote;
    SpectrumData  specData { proc };
    SpectrumView  spectrum { specData };
    DetectionLane detect   { proc };
    VowelMeter    vowel    { proc };
    LevelMeters   levels   { proc };

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
