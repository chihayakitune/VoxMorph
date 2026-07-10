#pragma once
#include "PluginProcessor.h"

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

class VoxMorphEditor : public juce::AudioProcessorEditor,
                       private juce::Timer
{
public:
    explicit VoxMorphEditor (VoxMorphProcessor& p)
        : juce::AudioProcessorEditor (&p), proc (p)
    {
        tooltipWindow.setLookAndFeel (&tipLnf);
        setWantsKeyboardFocus (true);   // for the Cmd+S shortcut

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
                 "breathy or whispery vowels when shifting pitch. Up to 0.7 the separation increases "
                 "at natural loudness; above 0.7 the preserved breath is also emphasized for a "
                 "clearly audible airy character. 0 = off.",
                 "声に含まれる自然な息(気息)成分を倍音成分から分離し、息だけはピッチ変換せずに"
                 "そのまま通します。息混じりの声をピッチシフトしたときに出る金属的なジャリジャリ感を"
                 "軽減します。0.7までは自然な音量のまま分離が増え、0.7を超えると息成分を強調して"
                 "効果がはっきり聴こえるようになります。0=オフ(従来どおり)。"));
        addSliderRow ("airband", "Air Preserve Band (Hz)",
            tip ("The frequency above which the voice is treated as breath (used only while Air "
                 "Preserve is up). Lower = stronger, reaching into the mids: try 700-900 if the "
                 "effect feels too subtle. If you hear roughness or a ghost of your original pitch "
                 "during pitch slides, raise it.",
                 "この周波数より上を「息」として扱います(Air Preserve使用時のみ有効)。"
                 "下げるほど中音域まで効いて効果が強くなります。効きが弱いと感じたら700〜900に。"
                 "音程を動かしたときにザラつきや元の声の高さの残りが聴こえる場合は上げてください。"));

        addSection ("ADVANCED");
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
        addToggleRow ("automute", "Auto-Mute on Feedback",
            tip ("Standalone app: if the output stays extremely loud for over a second "
                 "(a runaway feedback loop between speakers and mic), the output is muted "
                 "for 3 seconds automatically. Has no effect in a DAW.",
                 "スタンドアロン用。スピーカー→マイクのハウリングが暴走して出力が1秒以上"
                 "大音量で鳴り続けた場合、自動で3秒間ミュートして回路を切ります。"
                 "DAWプラグインとして使用中は動作しません。"));
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
        addAndMakeVisible (footer);
        defaultFooterText = footer.getText();

        int total = 20;
        for (auto& it : items) total += it.h;
        total += 52;
        setSize (560, total);
    }

    ~VoxMorphEditor() override
    {
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
        g.fillAll (juce::Colour (0xff1d1e23));
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (14, 10);
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
        lbl->setColour (juce::Label::textColourId, juce::Colour (0xffe8a33d));
        addAndMakeVisible (*lbl);
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

        addAndMakeVisible (*row);
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
        addAndMakeVisible (*row);
        items.push_back ({ row.get(), 28 });
        owned.push_back (std::move (row));
    }

    // ---- members --------------------------------------------------------------
    VoxMorphProcessor& proc;
    TipLookAndFeel tipLnf;
    juce::TooltipWindow tooltipWindow { this, 350 };

    struct Item { juce::Component* comp; int h; };
    std::vector<Item> items;
    std::vector<std::unique_ptr<juce::Component>> owned;
    juce::Label footer;
    juce::String defaultFooterText;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VoxMorphEditor)
};
