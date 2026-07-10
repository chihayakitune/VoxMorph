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
    layout.add (std::make_unique<P> (juce::ParameterID { "air", 1 }, "Air Preserve",
                juce::NormalisableRange<float> (0.0f, 1.5f, 0.001f), 0.0f));
    layout.add (std::make_unique<P> (juce::ParameterID { "airband", 1 }, "Air Preserve Band (Hz)",
                juce::NormalisableRange<float> (500.0f, 3000.0f, 1.0f, 0.5f), 1000.0f));
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
                juce::ParameterID { "gci", 1 }, "GCI Grain Sync", false));
    layout.add (std::make_unique<juce::AudioParameterBool> (
                juce::ParameterID { "lowvoice", 1 }, "Low Voice Mode", false));
    layout.add (std::make_unique<juce::AudioParameterBool> (
                juce::ParameterID { "automute", 1 }, "Auto-Mute on Feedback", true));
    layout.add (std::make_unique<juce::AudioParameterBool> (
                juce::ParameterID { "lowlat", 1 }, "Low Latency Mode", false));
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
    // Standalone: rename the confusing "Feedback Loop: / Mute audio input"
    // checkbox in the audio settings dialog (JUCE routes its UI strings
    // through the translation system, so we can remap them)
    if (wrapperType == wrapperType_Standalone)
    {
        juce::LocalisedStrings::setCurrentMappings (new juce::LocalisedStrings (
            juce::String ("language: en\n"
                          "countries: en\n"
                          "\"Feedback Loop:\" = \"Input Mute:\"\n"
                          "\"Mute audio input\" = \"Mute mic passthrough (prevents feedback)\"\n"),
            false));
    }

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
    pAir     = apvts.getRawParameterValue ("air");
    pAirBand = apvts.getRawParameterValue ("airband");
    pGci     = apvts.getRawParameterValue ("gci");
    pRange     = apvts.getRawParameterValue ("range");
    pCenter    = apvts.getRawParameterValue ("center");
    pTilt      = apvts.getRawParameterValue ("tilt");
    pJitter    = apvts.getRawParameterValue ("jitter");
    pRobot     = apvts.getRawParameterValue ("robot");
    pLowVoice  = apvts.getRawParameterValue ("lowvoice");
    pFloor     = apvts.getRawParameterValue ("pitchfloor");
    pAutoMute  = apvts.getRawParameterValue ("automute");
    pLowLat    = apvts.getRawParameterValue ("lowlat");
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
    p.airPreserve   = pAir->load();          // mixed harmonic+noise split
    p.airFreqHz     = pAirBand->load();
    p.gciSync       = pGci->load() > 0.5f;
    p.tiltDb        = pTilt->load();
    p.f1Shift = pF1S->load();  p.f1Gain = pF1G->load();
    p.f2Shift = pF2S->load();  p.f2Gain = pF2G->load();
    p.f3Shift = pF3S->load();  p.f3Gain = pF3G->load();
    p.jitter        = pJitter->load();
    p.robotize      = pRobot->load() > 0.5f;
    p.lowVoice      = pLowVoice->load() > 0.5f;
    p.pitchFloorHz  = pFloor->load();
    p.lowLatency    = pLowLat->load() > 0.5f;
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

    if (getLatencySamples() != engine.latencySamples())
        setLatencySamples (engine.latencySamples());

    // Feedback-runaway protection (standalone): if the output stays very
    // loud continuously (screaming feedback loop), mute for 3 s. The loop
    // then breaks, the input settles, and normal operation resumes.
    const bool fbActive = wrapperType == wrapperType_Standalone
                       && pAutoMute->load() > 0.5f;
    {
        double sum = 0.0;
        for (int i = 0; i < n; ++i) sum += (double) m[i] * m[i];
        const float rms = (float) std::sqrt (sum / std::max (1, n));
        const float dt = (float) n / (float) std::max (1.0, getSampleRate());
        // time-based smoothing (~50 ms) so behaviour does NOT depend on the
        // host buffer size; threshold high enough that loud singing can't
        // trigger it — genuine runaway feedback reaches near full scale
        const float aR = std::min (1.0f, dt / 0.05f);
        rmsSm += aR * (rms - rmsSm);

        if (fbActive)
        {
            if (rmsSm > 0.70f) loudSec += dt;
            else               loudSec = std::max (0.0f, loudSec - 2.0f * dt);
            if (loudSec > 1.5f) { muteSec = 3.0f; loudSec = 0.0f; }
        }
        if (muteSec > 0.0f) muteSec -= dt;

        const float target = (muteSec > 0.0f) ? 0.0f : 1.0f;
        if (muteGain < 0.999f || target < 1.0f)
            for (int i = 0; i < n; ++i)
            {
                muteGain += 0.002f * (target - muteGain);
                m[i] *= muteGain;
            }
    }

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
