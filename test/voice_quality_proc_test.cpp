#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cstdio>
#include <cstring>
static int failures = 0;
static void check(bool ok, const char *name)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok)
        ++failures;
}
static void set(VoxMorphProcessor &p, const char *id, float v)
{
    auto *a = p.apvts.getParameter(id);
    a->setValueNotifyingHost(a->convertTo0to1(v));
}
static float get(VoxMorphProcessor &p, const char *id)
{
    return p.apvts.getRawParameterValue(id)->load();
}
int main(int argc, char **argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    VoxMorphProcessor p;
    p.prepareToPlay(48000, 128);
    // Old state must clear newly-added controls even when replacing a running instance.
    juce::MemoryBlock state;
    p.getStateInformation(state);
    auto xml = juce::AudioProcessor::getXmlFromBinary(state.getData(), (int)state.getSize());
    for (int i = xml->getNumChildElements() - 1; i >= 0; --i)
        if (xml->getChildElement(i)->getStringAttribute("id").startsWith("vq"))
            xml->removeChildElement(xml->getChildElement(i), true);
    juce::AudioProcessor::copyXmlToBinary(*xml, state);
    set(p, "vqb1_gain", 6);
    set(p, "vqdp_on", 1);
    p.setStateInformation(state.getData(), (int)state.getSize());
    check(std::abs(get(p, "vqb1_gain")) < 1e-5f && get(p, "vqdp_on") == 0,
          "old session restores new controls to neutral");
    set(p, "tilt", 3);
    set(p, "vecenabled", 1);
    set(p, "vecamount", 100);
    set(p, "vqb2_gain", 4);
    set(p, "vqdp_amt", .7f);
    p.getStateInformation(state);
    VoxMorphProcessor q;
    q.setStateInformation(state.getData(), (int)state.getSize());
    check(std::abs(get(q, "tilt") - 3) < 1e-5f && std::abs(get(q, "vqb2_gain") - 4) < .001 &&
              std::abs(get(q, "vqdp_amt") - .7f) < .001,
          "legacy tilt + new parameters state round trip");
    auto preset = voxMorphPresetXml(p);
    auto file = juce::File::getCurrentWorkingDirectory().getNonexistentChildFile(
        "vq-test-roundtrip", ".vmpreset", false);
    check(preset->writeTo(file), "preset written to local test file");
    p.setParamLocked("vqb2_gain", true);
    set(p, "vqb2_gain", 2);
    set(p, "vqdp_amt", 0);
    int applied = 0, locked = 0;
    check(voxMorphApplyPreset(p, file, applied, locked), "preset parsed and applied");
    file.deleteFile();
    check(std::abs(get(p, "vqb2_gain") - 2) < .001 && std::abs(get(p, "vqdp_amt") - .7f) < .001 &&
              locked > 0,
          "preset round trip respects new parameter locks");
    p.history.init(p);
    p.history.beginGesture();
    set(p, "vqb1_freq", 600);
    p.history.poll();
    p.history.poll();
    set(p, "vqb1_gain", 5);
    p.history.endGesture();
    p.history.undo();
    check(std::abs(get(p, "vqb1_gain")) < 1e-5f && std::abs(get(p, "vqb1_freq") - 120) < .01,
          "one graph gesture undoes frequency and gain together");
    VoxMorphProcessor a, b;
    for (auto *proc : {&a, &b})
    {
        set(*proc, "pitch", 5);
        proc->prepareToPlay(48000, 32);
    }
    set(b, "vecenabled", 1);
    set(b, "vecamount", 100);
    juce::MidiBuffer midi;
    bool exact = true, finite = true;
    for (int block = 0; block < 40; ++block)
    {
        int n = block == 20 ? 8192 : 128;
        juce::AudioBuffer<float> left(2, n), right(2, n);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < n; ++i)
            {
                float x = .1f * std::sin(float(block * 128 + i) * .023f);
                left.setSample(ch, i, x);
                right.setSample(ch, i, x);
            }
        a.processBlock(left, midi);
        b.processBlock(right, midi);
        for (int ch = 0; ch < 2; ++ch)
            exact = exact && std::memcmp(left.getReadPointer(ch), right.getReadPointer(ch),
                                         sizeof(float) * n) == 0;
    }
    check(exact, "legacy vec ON/100 has no effect, including oversized blocks");
    set(a, "vqb1_dyn", 1);
    set(a, "vqb1_thr", -60);
    set(a, "vqdp_on", 1);
    set(a, "vqdp_thr", -60);
    for (int block = 0; block < 80; ++block)
    {
        if (block == 20)
            set(a, "stereo", 1);
        if (block == 40)
            a.reset();
        if (block == 60)
            set(a, "lowlat", 1);
        juce::AudioBuffer<float> buf(2, 128);
        for (int i = 0; i < 128; ++i)
        {
            float x = .2f * std::sin(float(block * 128 + i) * .023f);
            buf.setSample(0, i, x);
            buf.setSample(1, i, x * .2f);
        }
        a.processBlock(buf, midi);
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 128; ++i)
                finite = finite && std::isfinite(buf.getSample(ch, i));
    }
    check(finite, "active EQ/Pitch across stereo, reset, latency changes stays finite");
    if (argc > 1)
    {
        ak::ToneLookAndFeel look(ak::Tone::blue);
        SpectrumData spectrum(a);
        VoiceQualityPanel panel(a, spectrum);
        panel.setLookAndFeel(&look);
        panel.setSize(320, 944);
        panel.addToDesktop(juce::ComponentPeer::windowIsTemporary);
        panel.setVisible(true);
        juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
        auto image = panel.createComponentSnapshot(panel.getLocalBounds(), true, 2);
        juce::File output(argv[1]);
        auto stream = output.createOutputStream();
        juce::PNGImageFormat png;
        if (stream)
        {
            stream->setPosition(0);
            stream->truncate();
            png.writeImageToStream(image, *stream);
        }
        check(panel.isShowing(), "Voice Quality panel rendered with desktop peer");
        panel.removeFromDesktop();
        panel.setLookAndFeel(nullptr);
    }
    return failures ? 1 : 0;
}
