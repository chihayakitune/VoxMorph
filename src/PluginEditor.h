#pragma once
#include "PluginProcessor.h"

// VoxMorph custom editor:
//  - parameters grouped into sections
//  - hover tooltips with Japanese explanations
//  - editable value boxes (click the number, type, Enter)
//  - per-parameter reset buttons + double-click-to-default on sliders

class VoxMorphEditor : public juce::AudioProcessorEditor
{
public:
    explicit VoxMorphEditor (VoxMorphProcessor& p)
        : juce::AudioProcessorEditor (&p), proc (p)
    {
        addSection ("PITCH \xE3\x83\x94\xE3\x83\x83\xE3\x83\x81");
        addSliderRow ("pitch",
            jp ("ピッチ (半音)"),
            jp ("声の高さを半音単位で変えます。声色(フォルマント)は変わりません。"
                "女声化は+5〜+7、男声化は-5前後が目安。"));
        addToggleRow ("robot",
            jp ("ロボット化"),
            jp ("ピッチを一定の高さに固定し、抑揚のないロボット声にします。"
                "高さは下の「ロボット音程」で指定します。"));
        addSliderRow ("robotHz",
            jp ("ロボット音程 (Hz)"),
            jp ("ロボット化がオンのときに固定されるピッチ(Hz)。"));

        addSection ("INTONATION \xE6\x8A\x91\xE6\x8F\x9A");
        addSliderRow ("range",
            jp ("抑揚の強さ (%)"),
            jp ("声の抑揚(音程の上がり下がり)を強調/抑制します。100%=変化なし。"
                "「ピッチ」が声全体を平行移動するのに対し、こちらは動きの幅だけを変えます。"
                "女声化では110〜140%が目安(男声の平坦な抑揚が残ると不自然になるため)。"));
        addSliderRow ("center",
            jp ("抑揚の支点 (Hz)"),
            jp ("抑揚を拡大/縮小するときの中心になる音程。変換後の声の平均的な高さに"
                "合わせてください(女声なら200〜250Hz)。抑揚の強さが100%のときは無効。"));

        addSection ("FORMANT \xE3\x83\x95\xE3\x82\xA9\xE3\x83\xAB\xE3\x83\x9E\xE3\x83\xB3\xE3\x83\x88");
        addSliderRow ("formant",
            jp ("フォルマント (半音)"),
            jp ("声道の長さ=声の響き・声色を変えます。ピッチは変わりません。"
                "+で若く/女性的に、-で太く/男性的に。女声化は+3〜+4が目安。"));
        addSliderRow ("consonant",
            jp ("子音シフト (半音)"),
            jp ("無声子音(サ行・シャ行など)だけを追加でシフトします(フォルマント設定に加算)。"
                "女声の子音は明るいので+2〜+3が目安。上げすぎると舌足らずに聞こえます。"));

        addSection ("VOICE QUALITY \xE5\xA3\xB0\xE8\xB3\xAA");
        addSliderRow ("breath",
            jp ("息成分 (Breath)"),
            jp ("声帯の息漏れ成分を声に同期して加えます。少量(0.1〜0.3)で柔らかく"
                "艶のある質感になります。女声は男声より息成分が多いのが自然です。"));
        addSliderRow ("tilt",
            jp ("柔らかさ (dB)"),
            jp ("音色の傾き。+で柔らかく暖かい声、-で明るく張りのある声。±2dB程度から。"));
        addSliderRow ("jitter",
            jp ("自然な揺らぎ"),
            jp ("ごく小さな音程の揺らぎを加え、変換の機械っぽさを和らげます。0.1前後から。"));

        addSection ("OUTPUT \xE5\x87\xBA\xE5\x8A\x9B");
        addSliderRow ("mix",
            jp ("ミックス"),
            jp ("変換した声(1.0)と元の声(0.0)の割合。通常は1.0のままにします。"));
        addSliderRow ("gain",
            jp ("出力音量 (dB)"),
            jp ("プラグインの出力レベル。変換で音量感が変わったときの補正用。"));

        footer.setText (
            jp ("各項目にマウスを乗せると説明が表示されます。\n"
                "数値をクリックしてキーボード入力、\xE2\x86\xBA ボタンかスライダーのダブルクリックで初期値に戻ります。"),
            juce::dontSendNotification);
        footer.setFont (juce::Font (juce::FontOptions (11.5f)));
        footer.setColour (juce::Label::textColourId, juce::Colours::grey);
        footer.setJustificationType (juce::Justification::topLeft);
        addAndMakeVisible (footer);

        int total = 20;
        for (auto& it : items) total += it.h;
        total += 44;
        setSize (560, total);
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
        footer.setBounds (r.removeFromTop (40));
    }

private:
    static juce::String jp (const char* s) { return juce::String::fromUTF8 (s); }

    // ---- row components -------------------------------------------------
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

    // ---- builders --------------------------------------------------------
    void addSection (const char* text)
    {
        auto lbl = std::make_unique<juce::Label>();
        lbl->setText (jp (text), juce::dontSendNotification);
        lbl->setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
        lbl->setColour (juce::Label::textColourId, juce::Colour (0xffe8a33d));
        addAndMakeVisible (*lbl);
        items.push_back ({ lbl.get(), 30 });
        owned.push_back (std::move (lbl));
    }

    void addSliderRow (const juce::String& id, const juce::String& jpName, const juce::String& tip)
    {
        auto row = std::make_unique<SliderRow>();
        row->name.setText (jpName, juce::dontSendNotification);
        row->name.setTooltip (tip);
        row->name.setFont (juce::Font (juce::FontOptions (13.0f)));

        row->slider.setSliderStyle (juce::Slider::LinearHorizontal);
        row->slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 74, 22);
        row->slider.setTextBoxIsEditable (true);
        row->slider.setTooltip (tip);
        row->att = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
                        proc.apvts, id, row->slider);

        auto* rp = proc.apvts.getParameter (id);
        row->slider.setDoubleClickReturnValue (true, (double) rp->convertFrom0to1 (rp->getDefaultValue()));

        row->reset.setButtonText (jp ("\xE2\x86\xBA"));
        row->reset.setTooltip (jp ("初期値に戻す"));
        row->reset.onClick = [rp]
        {
            rp->beginChangeGesture();
            rp->setValueNotifyingHost (rp->getDefaultValue());
            rp->endChangeGesture();
        };

        addAndMakeVisible (*row);
        items.push_back ({ row.get(), 34 });
        owned.push_back (std::move (row));
    }

    void addToggleRow (const juce::String& id, const juce::String& jpName, const juce::String& tip)
    {
        auto row = std::make_unique<ToggleRow>();
        row->toggle.setButtonText (jpName);
        row->toggle.setTooltip (tip);
        row->att = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
                        proc.apvts, id, row->toggle);
        addAndMakeVisible (*row);
        items.push_back ({ row.get(), 30 });
        owned.push_back (std::move (row));
    }

    // ---- members ----------------------------------------------------------
    VoxMorphProcessor& proc;
    juce::TooltipWindow tooltipWindow { this, 350 };

    struct Item { juce::Component* comp; int h; };
    std::vector<Item> items;
    std::vector<std::unique_ptr<juce::Component>> owned;
    juce::Label footer;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VoxMorphEditor)
};
