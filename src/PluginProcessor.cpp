#include "PluginProcessor.h"
#include "PluginEditor.h"

juce::AudioProcessorEditor* VoxMorphProcessor::createEditor()
{
    return new VoxMorphEditor (*this);
}

juce::AudioProcessorValueTreeState::ParameterLayout VoxMorphProcessor::createLayout()
{
    using P = juce::AudioParameterFloat;
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<P> (juce::ParameterID { "pitch", 1 }, "Pitch (st)",
                juce::NormalisableRange<float> (-24.0f, 24.0f, 0.01f), 0.0f));
    layout.add (std::make_unique<P> (juce::ParameterID { "formant", 1 }, "Formant (st)",
                juce::NormalisableRange<float> (-24.0f, 24.0f, 0.01f), 0.0f));
    layout.add (std::make_unique<P> (juce::ParameterID { "consonant", 1 }, "Consonant Shift (st)",
                juce::NormalisableRange<float> (-12.0f, 12.0f, 0.01f), 0.0f));
    layout.add (std::make_unique<P> (juce::ParameterID { "f1shift", 1 }, "F1 Shift (st)",
                juce::NormalisableRange<float> (-6.0f, 6.0f, 0.01f), 0.0f));
    layout.add (std::make_unique<P> (juce::ParameterID { "f1gain", 1 }, "F1 Gain (dB)",
                juce::NormalisableRange<float> (-12.0f, 12.0f, 0.1f), 0.0f));
    layout.add (std::make_unique<P> (juce::ParameterID { "f2shift", 1 }, "F2 Shift (st)",
                juce::NormalisableRange<float> (-6.0f, 6.0f, 0.01f), 0.0f));
    layout.add (std::make_unique<P> (juce::ParameterID { "f2gain", 1 }, "F2 Gain (dB)",
                juce::NormalisableRange<float> (-12.0f, 12.0f, 0.1f), 0.0f));
    layout.add (std::make_unique<P> (juce::ParameterID { "f3shift", 1 }, "F3 Shift (st)",
                juce::NormalisableRange<float> (-6.0f, 6.0f, 0.01f), 0.0f));
    layout.add (std::make_unique<P> (juce::ParameterID { "f3gain", 1 }, "F3 Gain (dB)",
                juce::NormalisableRange<float> (-12.0f, 12.0f, 0.1f), 0.0f));
    layout.add (std::make_unique<P> (juce::ParameterID { "breath2", 1 }, "Breath",
                juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.0f));
    layout.add (std::make_unique<P> (juce::ParameterID { "range", 1 }, "Intonation Amount (%)",
                juce::NormalisableRange<float> (50.0f, 200.0f, 1.0f), 100.0f));
    layout.add (std::make_unique<P> (juce::ParameterID { "center", 1 }, "Intonation Pivot (Hz)",
                juce::NormalisableRange<float> (80.0f, 400.0f, 1.0f, 0.5f), 220.0f));
    layout.add (std::make_unique<P> (juce::ParameterID { "tilt", 1 }, "Softness / Tilt (dB)",
                juce::NormalisableRange<float> (-6.0f, 6.0f, 0.1f), 0.0f));
    layout.add (std::make_unique<P> (juce::ParameterID { "jitter", 1 }, "Natural Jitter",
                juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.0f));
    layout.add (std::make_unique<juce::AudioParameterBool> (
                juce::ParameterID { "robot", 1 }, "Robotize", false));
    layout.add (std::make_unique<juce::AudioParameterBool> (
                juce::ParameterID { "lowvoice", 1 }, "Low Voice Mode", false));
    layout.add (std::make_unique<P> (juce::ParameterID { "pitchfloor", 1 }, "Pitch Floor (Hz)",
                juce::NormalisableRange<float> (0.0f, 300.0f, 1.0f), 0.0f));
    layout.add (std::make_unique<P> (juce::ParameterID { "robotHz", 1 }, "Robot Pitch (Hz)",
                juce::NormalisableRange<float> (40.0f, 400.0f, 0.1f, 0.5f), 120.0f));
    layout.add (std::make_unique<P> (juce::ParameterID { "mix", 1 }, "Mix",
                juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 1.0f));
    layout.add (std::make_unique<P> (juce::ParameterID { "gain", 1 }, "Output Gain (dB)",
                juce::NormalisableRange<float> (-24.0f, 12.0f, 0.1f), 0.0f));
    return layout;
}

VoxMorphProcessor::VoxMorphProcessor()
    : AudioProcessor (BusesProperties()
          .withInput ("Input",  juce::AudioChannelSet::stereo(), true)
          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "params", createLayout())
{
    pPitch     = apvts.getRawParameterValue ("pitch");
    pFormant   = apvts.getRawParameterValue ("formant");
    pConsonant = apvts.getRawParameterValue ("consonant");
    pF1S = apvts.getRawParameterValue ("f1shift");
    pF1G = apvts.getRawParameterValue ("f1gain");
    pF2S = apvts.getRawParameterValue ("f2shift");
    pF2G = apvts.getRawParameterValue ("f2gain");
    pF3S = apvts.getRawParameterValue ("f3shift");
    pF3G = apvts.getRawParameterValue ("f3gain");
    pBreath2 = apvts.getRawParameterValue ("breath2");
    pRange     = apvts.getRawParameterValue ("range");
    pCenter    = apvts.getRawParameterValue ("center");
    pTilt      = apvts.getRawParameterValue ("tilt");
    pJitter    = apvts.getRawParameterValue ("jitter");
    pRobot     = apvts.getRawParameterValue ("robot");
    pLowVoice  = apvts.getRawParameterValue ("lowvoice");
    pFloor     = apvts.getRawParameterValue ("pitchfloor");
    pRobotHz   = apvts.getRawParameterValue ("robotHz");
    pMix       = apvts.getRawParameterValue ("mix");
    pGain      = apvts.getRawParameterValue ("gain");
}

bool VoxMorphProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    const auto& in  = l.getMainInputChannelSet();
    const auto& out = l.getMainOutputChannelSet();
    return (in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo())
        && in == out;
}

void VoxMorphProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.prepare (sampleRate);
    monoScratch.assign ((size_t) samplesPerBlock, 0.0f);
    setLatencySamples (engine.latencySamples());
}

void VoxMorphProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int n  = buffer.getNumSamples();
    const int ch = buffer.getNumChannels();
    if ((int) monoScratch.size() < n)
        monoScratch.assign ((size_t) n, 0.0f);

    PsolaEngine::Params p;
    p.pitchSemi     = pPitch->load();
    p.formantSemi   = pFormant->load();
    p.consonantSemi = pConsonant->load();
    p.pitchRange    = pRange->load() * 0.01f;   // % -> ratio
    p.pitchCenterHz = pCenter->load();
    p.breath        = pBreath2->load();      // spectral (noise-excited envelope)
    p.tiltDb        = pTilt->load();
    p.f1Shift = pF1S->load();  p.f1Gain = pF1G->load();
    p.f2Shift = pF2S->load();  p.f2Gain = pF2G->load();
    p.f3Shift = pF3S->load();  p.f3Gain = pF3G->load();
    p.jitter        = pJitter->load();
    p.robotize      = pRobot->load() > 0.5f;
    p.lowVoice      = pLowVoice->load() > 0.5f;
    p.pitchFloorHz  = pFloor->load();
    p.robotHz       = pRobotHz->load();
    p.mix           = pMix->load();
    engine.setParams (p);

    // mono-sum the input (voice sources are mono; stereo inputs are averaged)
    float* m = monoScratch.data();
    if (ch == 1)
        std::copy (buffer.getReadPointer (0), buffer.getReadPointer (0) + n, m);
    else
    {
        const float* L = buffer.getReadPointer (0);
        const float* R = buffer.getReadPointer (1);
        for (int i = 0; i < n; ++i) m[i] = 0.5f * (L[i] + R[i]);
    }

    engine.process (m, m, n);

    const float g = juce::Decibels::decibelsToGain (pGain->load());
    for (int c = 0; c < ch; ++c)
    {
        float* d = buffer.getWritePointer (c);
        for (int i = 0; i < n; ++i) d[i] = g * m[i];
    }
}

void VoxMorphProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, dest);
}

void VoxMorphProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new VoxMorphProcessor();
}
