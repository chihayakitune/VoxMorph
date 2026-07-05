#pragma once
#include "PluginProcessor.h"

// VoxMorph custom editor:
//  - parameters grouped into sections, English UI
//  - hover tooltips with bilingual (EN/JP) explanations, readable typography
//  - editable value boxes (click the number, type, Enter)
//  - per-parameter reset buttons + double-click-to-default on sliders

class VoxMorphEditor : public juce::AudioProcessorEditor
{
public:
    explicit VoxMorphEditor (VoxMorphProcessor& p)
        : juce::AudioProcessorEditor (&p), proc (p)
    {
        tooltipWindow.setLookAndFeel (&tipLnf);

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
        addSliderRow ("breath2", "Breath",
            tip ("Adds aspiration noise shaped by your vocal tract (noise-excited envelope), "
                 "synchronised with the voice. Small amounts (0.1-0.3) give a soft, airy quality. "
                 "Female voices naturally carry more breath than male voices.",
                 "声道の響きで整形した気息ノイズを声に同期して加えます(ノイズ励振方式)。"
                 "少量(0.1〜0.3)で柔らかく空気感のある質感に。女声は男声より息成分が多いのが自然です。"));
        addSliderRow ("tilt", "Softness / Tilt (dB)",
            tip ("Spectral tilt of the voice. + is softer and warmer, - is brighter and more present. "
                 "Start around +/-2 dB.",
                 "音色の傾き。+で柔らかく暖かい声、-で明るく張りのある声。±2dB程度から。"));
        addSliderRow ("jitter", "Natural Jitter",
            tip ("Adds tiny natural pitch fluctuations to reduce the 'machine' feel. Try around 0.1.",
                 "ごく小さな音程の揺らぎを加え、変換の機械っぽさを和らげます。0.1前後から。"));

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

        int total = 20;
        for (auto& it : items) total += it.h;
        total += 52;
        setSize (560, total);
    }

    ~VoxMorphEditor() override
    {
        tooltipWindow.setLookAndFeel (nullptr);
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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VoxMorphEditor)
};
