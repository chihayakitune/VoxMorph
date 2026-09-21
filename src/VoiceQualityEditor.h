#pragma once
// Included after SpectrumData and ParamRow in PluginEditor.h.
// Integrated Voice Quality editor: all parameter writes use APVTS and existing locks.
class VoiceQualityPanel : public juce::Component, private juce::Timer
{
  public:
    VoiceQualityPanel(VoxMorphProcessor &p, SpectrumData &spectrum)
        : proc(p), data(spectrum), graph(*this)
    {
        setName("Voice Quality EQ / Dynamics");
        graph.setName("EQ graph: drag a band to change frequency and gain");
        addAndMakeVisible(graph);
        data.addView(&graph);
        tabs.addItemList(juce::StringArray{"LOW", "BODY", "MID", "PRESENCE"}, 1);
        tabs.setSelectedId(1);
        tabs.onChange = [this]
        {
            selected = tabs.getSelectedId() - 1;
            buildRows();
            graph.repaint();
        };
        addAndMakeVisible(tabs);
        reset.setButtonText("Reset Band");
        reset.onClick = [this]
        {
            proc.history.group(
                [this]
                {
                    for (auto *suffix : keys)
                    {
                        const auto id = bandId() + suffix;
                        auto *p = proc.apvts.getParameter(id);
                        if (!proc.isParamLocked(id))
                        {
                            p->beginChangeGesture();
                            p->setValueNotifyingHost(p->getDefaultValue());
                            p->endChangeGesture();
                        }
                    }
                });
        };
        addAndMakeVisible(reset);
        addAndMakeVisible(readout);
        readout.setFont(ak::font(11));
        buildRows();
        startTimerHz(20);
    }
    ~VoiceQualityPanel() override { graph.finish(); }
    void resized() override
    {
        graph.setBounds(0, 0, getWidth(), 224);
        tabs.setBounds(0, 228, getWidth() - 100, 28);
        reset.setBounds(getWidth() - 96, 228, 96, 28);
        readout.setBounds(0, 258, getWidth(), 36);
        int y = 300;
        for (size_t i = 0; i < rows.size(); ++i)
        {
            if (i == 12)
                y += 30;
            rows[i]->setBounds(0, y, getWidth(), 34);
            y += 34;
        }
    }
    void paint(juce::Graphics &g) override
    {
        g.fillAll(ak::bodyFill);
        g.setColour(ak::ink);
        g.setFont(ak::font(12));
        g.drawText("DYNAMIC PITCH", 0, 710, getWidth(), 24, juce::Justification::centredLeft);
    }

  private:
    VoxMorphProcessor &proc;
    SpectrumData &data;
    int selected = 0;
    const std::array<const char *, 12> keys{"on",  "type",  "freq", "gain", "q",    "dyn",
                                            "thr", "ratio", "atk",  "rel",  "knee", "maxgr"};
    juce::ComboBox tabs;
    juce::TextButton reset;
    juce::Label readout;
    std::vector<std::unique_ptr<ParamRow>> rows;
    juce::String bandId(int b = -1) const
    {
        return juce::String("vqb") + juce::String((b < 0 ? selected : b) + 1) + "_";
    }
    float value(const juce::String &id) const
    {
        return vq::finite(proc.apvts.getRawParameterValue(id)->load());
    }
    void buildRows()
    {
        rows.clear();
        const char *labels[] = {"Band Enabled",     "Filter Type", "Frequency (Hz)",
                                "Gain (dB)",        "Q",           "Dynamics Enabled",
                                "Threshold (dBFS)", "Ratio",       "Attack (ms)",
                                "Release (ms)",     "Knee (dB)",   "Max Reduction (dB)"};
        auto add = [this](const juce::String &id, ParamRow::Kind kind, const char *label)
        {
            auto row = std::make_unique<ParamRow>(
                proc, id, kind, label,
                "Voice Quality EQ / Dynamics. Detection uses the gated input, before "
                "the conversion."
                "\n入力レベルは変換前で検出します。ThresholdはdBFS、GainはEQのdBです。");
            if (id.endsWith("thr"))
                row->setReadoutWidth(82);
            row->onLockChanged = [this]
            {
                for (auto &r : rows)
                    r->refreshLock();
            };
            addAndMakeVisible(*row);
            rows.push_back(std::move(row));
        };
        for (int i = 0; i < 12; ++i)
            add(bandId() + keys[i],
                i == 0 || i == 5 ? ParamRow::Kind::toggle
                : i == 1         ? ParamRow::Kind::combo
                                 : ParamRow::Kind::slider,
                labels[i]);
        const char *ids[] = {"on", "thr", "range", "amt", "atk", "rel"};
        const char *names[] = {"Enabled",     "Threshold (dBFS)", "Range (dB)",
                               "Amount (st)", "Attack (ms)",      "Release (ms)"};
        for (int i = 0; i < 6; ++i)
            add(juce::String("vqdp_") + ids[i], i == 0 ? ParamRow::Kind::toggle : ParamRow::Kind::slider,
                names[i]);
        resized();
    }
    void timerCallback() override
    {
        startTimerHz(vmDrawHz(proc, 20));
        for (auto &r : rows)
            r->refreshLock();
        readout.setText("Level " + juce::String(proc.uiVqLevel[selected].load(), 1) + " dBFS   GR -" +
                            juce::String(proc.uiVqGR[selected].load(), 1) + " dB\nPitch " +
                            juce::String(proc.uiVqPitch.load(), 2) + " st   Full " +
                            juce::String(proc.uiVqFullLevel.load(), 1) + " dBFS",
                        juce::dontSendNotification);
        graph.repaint();
    }
    class Graph : public juce::Component
    {
      public:
        explicit Graph(VoiceQualityPanel &p) : owner(p) {}
        void finish()
        {
            if (!drag)
                return;
            for (auto *p : gestureParams)
                p->endChangeGesture();
            gestureParams.clear();
            owner.proc.history.endGesture();
            drag = false;
        }
        void paint(juce::Graphics &g) override
        {
            auto r = plot();
            g.setColour(juce::Colour(0xff182232));
            g.fillRoundedRectangle(getLocalBounds().toFloat(), 5);
            g.setFont(ak::font(10));
            for (int db = -18; db <= 18; db += 6)
            {
                auto y = yFor((float)db);
                g.setColour(juce::Colours::white.withAlpha(.13f));
                g.drawHorizontalLine((int)y, r.getX(), r.getRight());
                g.setColour(juce::Colours::lightgrey);
                g.drawText(juce::String(db), 0, (int)y - 6, 27, 12, juce::Justification::centredRight);
            }
            for (float f : {100.f, 1000.f, 10000.f})
            {
                float x = xFor(f);
                g.setColour(juce::Colours::white.withAlpha(.12f));
                g.drawVerticalLine((int)x, r.getY(), r.getBottom());
                g.setColour(juce::Colours::lightgrey);
                g.drawText(f < 1000 ? "100" : "" + juce::String((int)f / 1000) + "k", (int)x - 18,
                           (int)r.getBottom() + 2, 36, 12, juce::Justification::centred);
            }
            // Spectrum uses its own dBFS scale (-66..+6), not the EQ gain axis.
            for (int out = 0; out < 2; ++out)
            {
                const auto &values = out ? owner.data.out() : owner.data.in();
                juce::Path p;
                for (size_t i = 0; i < values.size(); ++i)
                {
                    float x = r.getX() + r.getWidth() * float(i) / float(values.size());
                    float y =
                        r.getBottom() - r.getHeight() * juce::jlimit(0.f, 1.f, (values[i] + 66) / 72);
                    if (i == 0)
                        p.startNewSubPath(x, y);
                    else
                        p.lineTo(x, y);
                }
                g.setColour((out ? ak::seriesOut : ak::seriesIn).withAlpha(.28f));
                g.strokePath(p, juce::PathStrokeType(1));
            }
            std::array<std::array<vq::Coeff, 4>, 3> c;
            const double sr = owner.proc.getSampleRate() > 0 ? owner.proc.getSampleRate() : 48000;
            for (int b = 0; b < 4; ++b)
            {
                auto id = owner.bandId(b);
                bool on = owner.value(id + "on") > .5f, dyn = owner.value(id + "dyn") > .5f;
                float gain = owner.value(id + "gain"), max = owner.value(id + "maxgr");
                for (int mode = 0; mode < 3; ++mode)
                    c[mode][b] =
                        mode == 1 ? owner.proc.uiVqResponse[b].load()
                                  : vq::coefficients((int)owner.value(id + "type"),
                                                     owner.value(id + "freq"), owner.value(id + "q"),
                                                     on ? gain - (mode == 2 && dyn ? max : 0) : 0, sr);
            }
            std::array<juce::Path, 3> paths;
            std::array<std::vector<juce::Point<float>>, 3> points;
            for (int mode = 0; mode < 3; ++mode)
                for (int i = 0; i <= 160; ++i)
                {
                    double f = 20 * std::pow(1000., i / 160.);
                    double db = 0;
                    for (auto &coeff : c[mode])
                        db += coeff.db(std::min(f, sr * .499), sr);
                    float x = r.getX() + r.getWidth() * i / 160.f, y = yFor((float)db);
                    points[mode].push_back({x, y});
                    if (i == 0)
                        paths[mode].startNewSubPath(x, y);
                    else
                        paths[mode].lineTo(x, y);
                }
            juce::Path area = paths[0];
            for (auto it = points[2].rbegin(); it != points[2].rend(); ++it)
                area.lineTo(*it);
            area.closeSubPath();
            g.setColour(ak::seriesOut.withAlpha(.12f));
            g.fillPath(area);
            g.setColour(juce::Colours::white);
            g.strokePath(paths[0], juce::PathStrokeType(1.6f));
            g.setColour(ak::seriesOut);
            g.strokePath(paths[1], juce::PathStrokeType(1.8f));
            for (int b = 0; b < 4; ++b)
            {
                auto id = owner.bandId(b);
                float x = xFor(owner.value(id + "freq")), y = yFor(owner.value(id + "gain"));
                g.setColour(b == owner.selected ? ak::seriesOut : ak::seriesIn);
                g.fillEllipse(x - 5, y - 5, 10, 10);
                g.drawEllipse(x - 8, y - 8, 16, 16, 1 + std::min(3.f, owner.proc.uiVqGR[b].load() / 3));
                g.setColour(juce::Colours::white);
                g.drawText(juce::String(b + 1), (int)x - 6, (int)y - 6, 12, 12,
                           juce::Justification::centred);
            }
            g.setColour(juce::Colours::lightgrey);
            g.drawText("EQ dB | white: static  pink: current", 4, 0, getWidth() - 8, 16,
                       juce::Justification::centredLeft);
            g.drawText("Spectrum: input blue / output pink (-66..+6 dBFS)", 4, getHeight() - 14,
                       getWidth() - 8, 14, juce::Justification::centredLeft);
        }
        void mouseDown(const juce::MouseEvent &e) override
        {
            if (!plot().expanded(8).contains(e.position))
                return;
            int best = -1;
            float distance = 22;
            for (int b = 0; b < 4; ++b)
            {
                auto id = owner.bandId(b);
                float d = e.position.getDistanceFrom(
                    {xFor(owner.value(id + "freq")), yFor(owner.value(id + "gain"))});
                if (d < distance)
                {
                    best = b;
                    distance = d;
                }
            }
            if (best < 0)
                return;
            owner.selected = best;
            owner.tabs.setSelectedId(best + 1, juce::dontSendNotification);
            owner.buildRows();
            owner.proc.history.beginGesture();
            drag = true;
            for (auto *suffix : {"freq", "gain"})
            {
                auto id = owner.bandId() + suffix;
                if (!owner.proc.isParamLocked(id))
                {
                    auto *p = owner.proc.apvts.getParameter(id);
                    gestureParams.push_back(p);
                    p->beginChangeGesture();
                }
            }
            repaint();
        }
        void mouseDrag(const juce::MouseEvent &e) override
        {
            if (!drag)
                return;
            auto r = plot();
            for (auto *p : gestureParams)
            {
                float v =
                    p->paramID.endsWith("freq")
                        ? 20 * std::pow(1000.f,
                                        juce::jlimit(0.f, 1.f, (e.position.x - r.getX()) / r.getWidth()))
                        : juce::jlimit(-18.f, 18.f, 18 - 36 * (e.position.y - r.getY()) / r.getHeight());
                p->setValueNotifyingHost(p->convertTo0to1(v));
            }
            repaint();
        }
        void mouseUp(const juce::MouseEvent &) override { finish(); }

      private:
        VoiceQualityPanel &owner;
        bool drag = false;
        std::vector<juce::RangedAudioParameter *> gestureParams;
        juce::Rectangle<float> plot() const
        {
            return {30, 20, float(std::max(20, getWidth() - 38)), float(getHeight() - 52)};
        }
        float xFor(float f) const
        {
            auto r = plot();
            return r.getX() +
                   r.getWidth() * std::log(std::clamp(f, 20.f, 20000.f) / 20) / std::log(1000.f);
        }
        float yFor(float db) const
        {
            auto r = plot();
            return r.getY() + r.getHeight() * (18 - std::clamp(db, -18.f, 18.f)) / 36;
        }
    } graph;
};
