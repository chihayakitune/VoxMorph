#pragma once
#include "PluginProcessor.h"
#include "VoiceAnalyzer.h"

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
//  - Tabs: MAIN = scrolling parameter list, ANALYZE = AnalyzePanel,
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
    const VoiceProfile* you    = nullptr;   // mint   (same as visualizer input)
    const VoiceProfile* target = nullptr;   // pastel yellow
    const VoiceProfile* conv   = nullptr;   // pink   (same as visualizer output)
    std::function<float (const char*)> param;   // reads current parameter values

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

        const juce::Colour cy (0xff54bda1), ct (0xffdfb545), cc (0xfff08ba5);
        if (you    != nullptr && you->valid())    drawProfile (g, r, *you,    cy);
        if (target != nullptr && target->valid()) drawProfile (g, r, *target, ct);
        if (conv   != nullptr && conv->valid())   drawProfile (g, r, *conv,   cc);

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

        g.setFont (juce::Font (juce::FontOptions (11.0f)));
        g.setColour (cy); g.drawText ("You",       (int) r.getRight() - 190, (int) r.getY() + 2, 44, 14, juce::Justification::left);
        g.setColour (ct); g.drawText ("Target",    (int) r.getRight() - 140, (int) r.getY() + 2, 54, 14, juce::Justification::left);
        g.setColour (cc); g.drawText ("Converted", (int) r.getRight() - 80,  (int) r.getY() + 2, 78, 14, juce::Justification::left);
        if ((you == nullptr || ! you->valid()) && (target == nullptr || ! target->valid())
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

    static void drawProfile (juce::Graphics& g, juce::Rectangle<float> r,
                             const VoiceProfile& p, juce::Colour col)
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
        g.strokePath (path, juce::PathStrokeType (1.8f));
        auto dot = [&] (double fc, float h)
        {
            if (fc <= 0.0) return;
            const float y = r.getY() + r.getHeight() * (kTop - h) / (kTop - kFloor);
            g.fillEllipse (xOf (r, fc) - 2.5f, y - 2.5f, 5.0f, 5.0f);
        };
        dot (p.f0Hz, 0.0f);
        for (int fi = 0; fi < 3; ++fi) dot (p.F[fi], p.L[fi]);

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
};

// ANALYZE tab: 1) record your own voice -> Profile 1, 2) load a target
// voice file -> Profile 2 (and preview it through the plugin output),
// 3) Auto-Set derives conversion parameters from the two profiles:
//   pitch      = F0 difference (st)
//   formant    = mean formant difference (st); F1-F3 shifts = per-formant trim
//   f1-f3 gain = relative-level difference (x0.7, conservative)
//   range/center = intonation-spread ratio and target median F0
//   tilt       = quarter of the texture (tilt) difference
class AnalyzePanel : public juce::Component, private juce::Timer
{
public:
    explicit AnalyzePanel (VoxMorphProcessor& p) : proc (p)
    {
        for (auto* b : { &recBtn, &loadBtn, &playBtn, &applyBtn, &refineBtn })
            addAndMakeVisible (*b);
        for (auto* c : { &recPlayChk, &refPlayChk })
        {
            c->setTooltip (juce::String::fromUTF8 (
                "When checked, the target file plays while you record - speak along with it. "
                "Headphones recommended.\nチェックすると録音と同時にターゲットを再生します。"
                "再生に合わせて喋ってください(ヘッドホン推奨)。"));
            addAndMakeVisible (*c);
        }
        auto initHeading = [this] (juce::Label& l, const char* t)
        {
            l.setText (t, juce::dontSendNotification);
            l.setFont (juce::Font (juce::FontOptions (13.5f, juce::Font::bold)));
            l.setColour (juce::Label::textColourId, juce::Colour (0xff45bda5));
            addAndMakeVisible (l);
        };
        initHeading (hTarget, "Target");
        initHeading (hVoice,  "Analyze MyVoice");
        initHeading (hConv,   "Re-Analyze MyVoice [Converted]");
        initHeading (hApply,  "Apply Analyzed Settings");
        graph.you = &prof1;  graph.target = &prof2;  graph.conv = &profC;
        graph.param = [this] (const char* id)
        {
            auto* v = proc.apvts.getRawParameterValue (id);
            return v != nullptr ? v->load() : 0.0f;
        };
        addAndMakeVisible (graph);
        durBox.addItem ("5 s", 5);  durBox.addItem ("10 s", 10);  durBox.addItem ("15 s", 15);
        durBox.setSelectedId (10, juce::dontSendNotification);
        durBox.setTooltip (juce::String::fromUTF8 ("Recording length. Longer = more frames = a steadier profile.\n録音時間。長いほど分析フレームが増え、プロファイルが安定します。"));
        addAndMakeVisible (durBox);
        for (auto* l : { &help, &p1Lbl, &p2Lbl, &pCLbl, &outLbl })
        {
            l->setJustificationType (juce::Justification::topLeft);
            l->setFont (juce::Font (juce::FontOptions (12.5f)));
            addAndMakeVisible (*l);
        }
        help.setText (juce::String::fromUTF8 (
            "1) Targetの音声ファイルを読み込む → 2) 自分の声を録音(with Play=再生と同時録音・ヘッドホン\n"
            "推奨) → 3) Auto-Setで自動設定 → 4) 変換後の声をRecord Converted+Refineで録音すると\n"
            "目標との残差で自動再調整。4)を繰り返すほど目標の声に近づきます。"),
            juce::dontSendNotification);
        p1Lbl.setText (juce::String::fromUTF8 ("MyVoiceプロファイル: --"),   juce::dontSendNotification);
        p2Lbl.setText (juce::String::fromUTF8 ("ターゲット プロファイル: --"), juce::dontSendNotification);
        pCLbl.setText (juce::String::fromUTF8 ("Convertedプロファイル: --"),  juce::dontSendNotification);

        recBtn.setTooltip (juce::String::fromUTF8 ("Records the microphone input and analyzes it.\nマイク入力を録音して分析します。録音中は普段の調子で喋り続けてください(ヘッドホン推奨)。"));
        loadBtn.setTooltip (juce::String::fromUTF8 ("Load a voice file (wav/aiff/mp3/m4a/flac, first 60 s used).\n目標の声の音声ファイルを読み込みます(先頭60秒まで)。"));
        playBtn.setTooltip (juce::String::fromUTF8 ("Preview the loaded file through the plugin output.\n読み込んだファイルを出力から再生します。"));
        applyBtn.setTooltip (juce::String::fromUTF8 ("Writes the derived settings into the MAIN tab parameters.\n分析結果から求めた設定をMAINタブのパラメータに書き込みます。"));

        refineBtn.setTooltip (juce::String::fromUTF8 (
            "Records the CONVERTED output voice, compares it with the target and nudges the "
            "parameters to close the remaining gap. Repeatable - each pass gets closer.\n"
            "変換後の出力音声を録音し、目標と比較して残差分だけパラメータを再調整します。"
            "何度でも繰り返せて、繰り返すほど目標に近づきます。現在の設定のまま喋ってください。"));

        recBtn.onClick = [this]
        {
            if (recPlayChk.getToggleState() && ! startPlayForCapture()) return;
            proc.capFromOutput = false;
            startCapture (recBtn, waitingCapture);
        };
        refineBtn.onClick = [this]
        {
            if (! requireTarget()) return;
            if (refPlayChk.getToggleState() && ! startPlayForCapture()) return;
            proc.capFromOutput = true;
            startCapture (refineBtn, waitingRefine);
        };
        loadBtn.onClick  = [this] { loadFile(); };
        playBtn.onClick  = [this]
        {
            if (proc.prevPos.load() >= 0) proc.prevPos = -1;
            else if (proc.prevLen.load() > 0) proc.prevPos = 0;
        };
        applyBtn.onClick = [this] { apply(); };
        startTimerHz (10);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (16, 10);
        help.setBounds (r.removeFromTop (56));

        hTarget.setBounds (r.removeFromTop (20));             // -- Target --
        auto r1 = r.removeFromTop (48);
        loadBtn.setBounds (r1.removeFromLeft (170).withHeight (26));
        playBtn.setBounds (r1.removeFromLeft (90).withHeight (26).translated (8, 0));
        p2Lbl.setBounds (r1.withTrimmedLeft (18));

        hVoice.setBounds (r.removeFromTop (20));              // -- Analyze MyVoice --
        auto r2 = r.removeFromTop (50);
        recBtn.setBounds (r2.removeFromLeft (150).withHeight (26));
        recPlayChk.setBounds (r2.removeFromLeft (140).withHeight (26).translated (8, 0));
        durBox.setBounds (r2.removeFromLeft (72).withHeight (26).translated (14, 0));
        p1Lbl.setBounds (r2.withTrimmedLeft (22));

        hConv.setBounds (r.removeFromTop (20));               // -- Re-Analyze [Converted] --
        auto r3 = r.removeFromTop (50);
        refineBtn.setBounds (r3.removeFromLeft (210).withHeight (26));
        refPlayChk.setBounds (r3.removeFromLeft (140).withHeight (26).translated (8, 0));
        pCLbl.setBounds (r3.withTrimmedLeft (22));

        r.removeFromTop (12);                                 // blank line
        hApply.setBounds (r.removeFromTop (20));              // -- Apply Analyzed Settings --
        applyBtn.setBounds (r.removeFromTop (32).withWidth (210).withHeight (26));

        graph.setBounds (r.removeFromTop (juce::jmax (110, r.getHeight() - 92)));
        outLbl.setBounds (r.withTrimmedTop (4));
    }

private:
    void startCapture (juce::TextButton& b, bool& waitFlag)
    {
        const double sr = proc.getSampleRate() > 0 ? proc.getSampleRate() : 48000.0;
        proc.capTarget = (int) (sr * durBox.getSelectedId());
        proc.capLen = 0;
        proc.capturing = true;
        waitFlag = true;
        b.setButtonText ("Recording... speak!");
    }

    bool requireTarget()
    {
        if (prof2.valid()) return true;
        outLbl.setText (juce::String::fromUTF8 ("先に目標ファイルを読み込んでください。"),
                        juce::dontSendNotification);
        return false;
    }

    bool startPlayForCapture()      // start target playback for a "with Play" recording
    {
        if (proc.prevLen.load() <= 0)
        {
            outLbl.setText (juce::String::fromUTF8 ("先に目標ファイルを読み込んでください。"),
                            juce::dontSendNotification);
            return false;
        }
        proc.prevPos = 0;
        playStartedByCapture = true;
        return true;
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
            recBtn.setButtonText ("Record My Voice");
            prof1 = analyzeCapture();
            proc.lastMyVoice = prof1;
            p1Lbl.setText (juce::String::fromUTF8 ("MyVoiceプロファイル:\n") + fmt (prof1),
                           juce::dontSendNotification);
            graph.repaint();
        }
        if (waitingRefine && ! proc.capturing.load())
        {
            waitingRefine = false;
            stopPlayIfStartedByCapture();
            refineBtn.setButtonText ("Record Converted + Refine");
            profC = analyzeCapture();
            pCLbl.setText (juce::String::fromUTF8 ("Convertedプロファイル:\n") + fmt (profC),
                           juce::dontSendNotification);
            refine (profC);
            graph.repaint();
        }
        playBtn.setButtonText (proc.prevPos.load() >= 0 ? "Stop" : "Play");
    }

    void stopPlayIfStartedByCapture()
    {
        if (playStartedByCapture) { proc.prevPos = -1; playStartedByCapture = false; }
    }

    static juce::String fmt (const VoiceProfile& pr)
    {
        if (! pr.valid())
            return juce::String::fromUTF8 ("分析できませんでした(有声区間が不足)。もう一度、声を出し続けて試してください。");
        return juce::String::formatted ("F0 %.0f Hz (intonation %.1f st)   tilt %+.1f dB\n"
                                        "F1 %.0f / F2 %.0f / F3 %.0f Hz   levels %+.0f / %+.0f / %+.0f dB",
                                        pr.f0Hz, pr.f0SpreadSt, pr.tiltDb,
                                        pr.F[0], pr.F[1], pr.F[2], pr.L[0], pr.L[1], pr.L[2]);
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
            proc.prevPos = -1;                              // stop before writing

            if (file.hasFileExtension ("vmprofile"))        // saved profile: no audio
            {
                VoiceProfile p;
                if (auto xml = juce::XmlDocument::parse (file); xml != nullptr
                    && profileFromXml (*xml, p))
                {
                    prof2 = p;
                    proc.lastTarget = p;
                    proc.prevLen = 0;                        // nothing to Play
                    p2Lbl.setText (juce::String::fromUTF8 ("ターゲット(")
                                   + file.getFileNameWithoutExtension()
                                   + juce::String::fromUTF8 (")プロファイル [Play不可]:\n") + fmt (prof2),
                                   juce::dontSendNotification);
                    graph.repaint();
                }
                else
                    p2Lbl.setText (juce::String::fromUTF8 ("プロファイルを読み込めませんでした: ")
                                   + file.getFileName(), juce::dontSendNotification);
                return;
            }
            juce::AudioFormatManager fm;
            fm.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> rd (fm.createReaderFor (file));
            if (rd == nullptr || rd->sampleRate <= 0)
            {
                p2Lbl.setText ("Could not read: " + file.getFileName(), juce::dontSendNotification);
                return;
            }
            const int nIn = (int) std::min<juce::int64> (rd->lengthInSamples,
                                                         (juce::int64) (rd->sampleRate * 60.0));
            juce::AudioBuffer<float> tb ((int) rd->numChannels, nIn);
            rd->read (&tb, 0, nIn, 0, true, true);

            const double sr    = proc.getSampleRate() > 0 ? proc.getSampleRate() : 48000.0;
            const double ratio = rd->sampleRate / sr;       // linear resample -> engine rate
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
            p2Lbl.setText (juce::String::fromUTF8 ("ターゲット(") + file.getFileName()
                           + juce::String::fromUTF8 (")プロファイル:\n") + fmt (prof2),
                           juce::dontSendNotification);
            graph.repaint();
        });
    }

    void setP (const char* id, float v)
    {
        if (auto* p = proc.apvts.getParameter (id))
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost (p->convertTo0to1 (v));
            p->endChangeGesture();
        }
    }

    void apply()
    {
        if (! prof1.valid() || ! prof2.valid())
        {
            outLbl.setText (juce::String::fromUTF8 ("両方のプロファイルを先に作成してください。"),
                            juce::dontSendNotification);
            return;
        }
        auto st = [] (float a, float b) { return 12.0f * std::log2 (a / b); };
        // Pitch deliberately sits kPitchBias below the plain F0 difference and
        // the intonation gets boosted instead: speech carries its perceived
        // height in the peaks, so a full median match sounds overshot (user
        // feedback). The Refine loop applies the same bias so they agree.
        const float pitch = juce::jlimit (-24.0f, 24.0f, st (prof2.f0Hz, prof1.f0Hz) - kPitchBias);
        float sh[3];
        for (int i = 0; i < 3; ++i) sh[i] = st (prof2.F[i], prof1.F[i]);
        const float formant = juce::jlimit (-24.0f, 24.0f, (sh[0] + sh[1] + sh[2]) / 3.0f);
        const float tilt = juce::jlimit (-4.0f, 4.0f, 0.25f * (prof2.tiltDb - prof1.tiltDb));

        setP ("pitch",   pitch);
        setP ("formant", formant);
        // Per-formant trims stay deliberately small: the global Formant knob
        // carries the bulk of the shift, and large individual warps sound
        // dry / electronic. Half the measured difference, tightly clamped.
        const char* sid[3] = { "f1shift", "f2shift", "f3shift" };
        const char* gid[3] = { "f1gain",  "f2gain",  "f3gain"  };
        for (int i = 0; i < 3; ++i)
        {
            setP (sid[i], juce::jlimit (-3.0f, 3.0f, 0.5f * (sh[i] - formant)));
            setP (gid[i], juce::jlimit (-8.0f, 8.0f, 0.5f * (prof2.L[i] - prof1.L[i])));
        }
        setP ("tilt", tilt);
        juce::String extra;
        if (prof1.f0SpreadSt > 0.3f && prof2.f0SpreadSt > 0.3f)
        {
            const float range = juce::jlimit (50.0f, 200.0f,
                                              kRangeBoost * 100.0f * prof2.f0SpreadSt / prof1.f0SpreadSt);
            setP ("range",  range);
            setP ("center", juce::jlimit (80.0f, 400.0f, prof2.f0Hz));
            extra = juce::String::formatted ("  range %.0f%%  center %.0f Hz", range, prof2.f0Hz);
        }

        // High Range guard: engage just above YOUR normal speaking range so
        // laughs get tamed but plain talking is unaffected. Pitch Floor: the
        // lower edge of the TARGET's range keeps the converted voice from
        // dropping out of character.
        const float hifreq = juce::jlimit (150.0f, 600.0f,
                                 prof1.f0Hz * std::pow (2.0f, (prof1.f0SpreadSt + 2.0f) / 12.0f));
        setP ("hifreq",    hifreq);
        setP ("hipitch",   50.0f);
        setP ("hiformant", 100.0f);
        const float pfloor = juce::jlimit (0.0f, 300.0f,
                                 prof2.f0Hz * std::pow (2.0f, -(prof2.f0SpreadSt + 1.0f) / 12.0f));
        setP ("pitchfloor", pfloor);

        outLbl.setText (juce::String::fromUTF8 ("設定しました → MAINタブで確認・微調整してください。\n")
                        + juce::String::formatted ("pitch %+.1f st   formant %+.1f st   tilt %+.1f dB", pitch, formant, tilt)
                        + extra
                        + juce::String::formatted ("\nhigh-range start %.0f Hz   pitch floor %.0f Hz", hifreq, pfloor),
                        juce::dontSendNotification);
        graph.repaint();
    }

    // Refine loop (steps 4/5): record the CONVERTED output, compare with the
    // target, and nudge the parameters by the damped residual. Damping keeps
    // repeated passes converging instead of oscillating.
    void refine (const VoiceProfile& pc)
    {
        if (! pc.valid())
        {
            outLbl.setText (juce::String::fromUTF8 ("変換後の声を分析できませんでした。もう一度、喋り続けて録音してください。"),
                            juce::dontSendNotification);
            return;
        }
        auto cur = [this] (const char* id) { return proc.apvts.getRawParameterValue (id)->load(); };
        auto st  = [] (float a, float b)   { return 12.0f * std::log2 (a / b); };

        const float dPitch = juce::jlimit (-6.0f, 6.0f, st (prof2.f0Hz, pc.f0Hz) - kPitchBias);
        setP ("pitch", juce::jlimit (-24.0f, 24.0f, cur ("pitch") + 0.8f * dPitch));

        float sh[3];
        for (int i = 0; i < 3; ++i) sh[i] = st (prof2.F[i], pc.F[i]);
        const float dFmt = juce::jlimit (-6.0f, 6.0f, (sh[0] + sh[1] + sh[2]) / 3.0f);
        setP ("formant", juce::jlimit (-24.0f, 24.0f, cur ("formant") + 0.7f * dFmt));

        const char* sid[3] = { "f1shift", "f2shift", "f3shift" };
        const char* gid[3] = { "f1gain",  "f2gain",  "f3gain"  };
        for (int i = 0; i < 3; ++i)
        {
            setP (sid[i], juce::jlimit (-3.0f, 3.0f, cur (sid[i]) + 0.4f * (sh[i] - dFmt)));
            setP (gid[i], juce::jlimit (-8.0f, 8.0f, cur (gid[i]) + 0.4f * (prof2.L[i] - pc.L[i])));
        }
        const float dTilt = 0.25f * (prof2.tiltDb - pc.tiltDb);
        setP ("tilt", juce::jlimit (-6.0f, 6.0f, cur ("tilt") + juce::jlimit (-1.5f, 1.5f, dTilt)));
        if (prof2.f0SpreadSt > 0.3f && pc.f0SpreadSt > 0.3f)
            setP ("range", juce::jlimit (50.0f, 200.0f,
                     cur ("range") * juce::jlimit (0.75f, 1.35f,
                                                   kRangeBoost * prof2.f0SpreadSt / pc.f0SpreadSt)));

        outLbl.setText (juce::String::fromUTF8 ("再調整しました(残差が小さくなるまで繰り返せます)。\n")
                        + juce::String::formatted ("residual: pitch %+.1f st  formant %+.1f st  tilt %+.1f dB",
                                                   dPitch, dFmt, prof2.tiltDb - pc.tiltDb),
                        juce::dontSendNotification);
    }

    static constexpr float kPitchBias  = 1.0f;    // st below the plain F0 match
    static constexpr float kRangeBoost = 1.15f;   // intonation compensation

    VoxMorphProcessor& proc;
    VoiceProfile prof1, prof2, profC;
    ProfileGraph graph;
    bool waitingCapture = false, waitingRefine = false, playStartedByCapture = false;
    juce::TextButton recBtn { "Record My Voice" }, loadBtn { "Load Target File..." },
                     playBtn { "Play" }, applyBtn { "Auto-Set Parameters" },
                     refineBtn { "Record Converted + Refine" };
    juce::ToggleButton recPlayChk { "With target play" }, refPlayChk { "With target play" };
    juce::ComboBox durBox;
    juce::Label help, p1Lbl, p2Lbl, pCLbl, outLbl, hTarget, hVoice, hConv, hApply;
    std::unique_ptr<juce::FileChooser> chooser;
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
        if (auto xml = juce::XmlDocument::parse (files.getReference (idx)))
        {
            if (xml->hasTagName (proc.apvts.state.getType()))
            {
                proc.apvts.replaceState (juce::ValueTree::fromXml (*xml));
                setStatus (juce::String::fromUTF8 ("読み込みました: ") + presetBox.getText());
                return;
            }
        }
        setStatus (juce::String::fromUTF8 ("読み込みに失敗しました(壊れたファイル?)"));
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
        for (auto* p : proc.getParameters())
            if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p))
            {
                rp->beginChangeGesture();
                rp->setValueNotifyingHost (rp->getDefaultValue());
                rp->endChangeGesture();
            }
        setStatus (juce::String::fromUTF8 ("全パラメータを初期値に戻しました。"));
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
        tabs.addTab ("ANALYZE", juce::Colour (0xfffcf9f9), &analyzePanel, false);
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
        addSliderRow ("air", "Air Preserve",
            tip ("Separates the natural breath (air) in your voice from the harmonics and passes it "
                 "through WITHOUT pitch shifting. Fixes the metallic 'buzzy' texture that appears on "
                 "breathy or whispery vowels when shifting pitch. Up to 1.0 the separation increases "
                 "at natural loudness; from 1.0 to 1.5 the preserved breath is also emphasized for a "
                 "clearly audible airy character. 0 = off.",
                 "声に含まれる自然な息(気息)成分を倍音成分から分離し、息だけはピッチ変換せずに"
                 "そのまま通します。息混じりの声をピッチシフトしたときに出る金属的なジャリジャリ感を"
                 "軽減します。1.0までは自然な音量のまま分離が増え(1.0=分離最大)、1.0〜1.5では"
                 "息成分を強調して効果がはっきり聴こえるようになります。0=オフ(従来どおり)。"));
        addSliderRow ("airband", "Air Preserve Band (Hz)",
            tip ("The frequency above which the voice is treated as breath (used only while Air "
                 "Preserve is up). Lower = stronger, reaching into the mids: try 700-900 if the "
                 "effect feels too subtle. If you hear roughness or a ghost of your original pitch "
                 "during pitch slides, raise it.",
                 "この周波数より上を「息」として扱います(Air Preserve使用時のみ有効)。"
                 "下げるほど中音域まで効いて効果が強くなります。効きが弱いと感じたら700〜900に。"
                 "音程を動かしたときにザラつきや元の声の高さの残りが聴こえる場合は上げてください。"));

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
            tip ("For deep or creaky voices (vocal fry). Extends pitch tracking down to 40 Hz and "
                 "holds the last stable pitch through irregular, gravelly stretches - this prevents "
                 "the converted/unconverted flutter on low 'mm' or 'eh' sounds. "
                 "Leave off unless you need it.",
                 "低くガラガラした声(ボーカルフライ)向けのモード。ピッチ検出を40Hzまで拡張し、"
                 "不規則な区間では直前の安定ピッチを保持します。低い「んー」「えー」で"
                 "変換/未変換が交互に切り替わってロボット的になる現象を防ぎます。"
                 "必要な場合のみオンにしてください。"));

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
    struct SliderRow : public juce::Component
    {
        juce::Label      name;
        juce::Slider     slider;
        juce::TextButton reset;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> att;

        SliderRow()
        {
            addAndMakeVisible (name);
            addAndMakeVisible (slider);
            addAndMakeVisible (reset);
        }
        void resized() override
        {
            auto r = getLocalBounds();
            name.setBounds (r.removeFromLeft (168));
            reset.setBounds (r.removeFromRight (30).reduced (3));
            slider.setBounds (r);
        }
    };

    struct ToggleRow : public juce::Component
    {
        juce::ToggleButton toggle;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> att;
        ToggleRow() { addAndMakeVisible (toggle); }
        void resized() override { toggle.setBounds (getLocalBounds().withTrimmedLeft (4)); }
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
    AnalyzePanel    analyzePanel { proc };
    PresetPanel     presetPanel { proc };
    AsmrPanel       asmrPanel { proc };
    FxBar           fxBar { proc };
    FnComponent     mainPage;    // MAIN tab page (declared before tabs!)
    juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };
    int contentHeight = 0;

    struct Item { juce::Component* comp; int h; };
    std::vector<Item> items;
    std::vector<std::unique_ptr<juce::Component>> owned;
    juce::Label footer;
    juce::String defaultFooterText;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VoxMorphEditor)
};
