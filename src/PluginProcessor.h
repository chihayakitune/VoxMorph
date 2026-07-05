#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "PsolaEngine.h"

class VoxMorphProcessor : public juce::AudioProcessor
{
public:
    VoxMorphProcessor();

    // -- AudioProcessor --
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override               { return true; }
    const juce::String getName() const override   { return "VoxMorph"; }
    bool acceptsMidi() const override             { return false; }
    bool producesMidi() const override            { return false; }
    double getTailLengthSeconds() const override  { return 0.1; }
    int getNumPrograms() override                 { return 1; }
    int getCurrentProgram() override              { return 0; }
    void setCurrentProgram (int) override         {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    PsolaEngine engine;
    std::vector<float> monoScratch;

    std::atomic<float>* pPitch     = nullptr;
    std::atomic<float>* pFormant   = nullptr;
    std::atomic<float>* pConsonant = nullptr;
    std::atomic<float>* pRange     = nullptr;
    std::atomic<float>* pCenter    = nullptr;
    std::atomic<float>* pBreath    = nullptr;
    std::atomic<float>* pTilt      = nullptr;
    std::atomic<float>* pJitter    = nullptr;
    std::atomic<float>* pRobot     = nullptr;
    std::atomic<float>* pRobotHz   = nullptr;
    std::atomic<float>* pMix       = nullptr;
    std::atomic<float>* pGain      = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VoxMorphProcessor)
};
