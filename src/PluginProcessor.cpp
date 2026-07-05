#include "PluginProcessor.h"

juce::AudioProcessorValueTreeState::ParameterLayout VoxMorphProcessor::createLayout()
{
    using P = juce::AudioParameterFloat;
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<P> (juce::ParameterID { "pitch", 1 }, "Pitch (st)",
                juce::NormalisableRange<float> (-24.0f, 24.0f, 0.01f), 0.0f));
    layout.add (std::make_unique<P> (juce::ParameterID { "formant", 1 }, "Formant (st)",
                juce::NormalisableRange<float> (-24.0f, 24.0f, 0.01f), 0.0f));
    layout.add (std::make_unique<juce::AudioParameterBool> (
                juce::ParameterID { "robot", 1 }, "Robotize", false));
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
    pPitch   = apvts.getRawParameterValue ("pitch");
    pFormant = apvts.getRawParameterValue ("formant");
    pRobot   = apvts.getRawParameterValue ("robot");
    pRobotHz = apvts.getRawParameterValue ("robotHz");
    pMix     = apvts.getRawParameterValue ("mix");
    pGain    = apvts.getRawParameterValue ("gain");
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

    engine.setParams (pPitch->load(), pFormant->load(),
                      pRobot->load() > 0.5f, pRobotHz->load(), pMix->load());

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
