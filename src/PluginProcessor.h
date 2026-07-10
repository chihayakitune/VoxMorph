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

    // Visualizer taps: mono input (pre-conversion) and output (as heard),
    // written on the audio thread, read by the editor's SpectrumView.
    static constexpr int kVizLen = 16384;              // power of two
    std::vector<float> vizIn  = std::vector<float> ((size_t) kVizLen, 0.0f);
    std::vector<float> vizOut = std::vector<float> ((size_t) kVizLen, 0.0f);
    std::atomic<int>   vizPos { 0 };

    // ANALYZE tab support. capBuf: mic capture, up to 15 s (raw input,
    // pre-engine); capTarget = requested length in samples.
    // prevBuf: target-file preview, pre-allocated in prepareToPlay and only
    // written while prevPos == -1 (stopped), so no locking is needed.
    std::vector<float> capBuf;
    std::atomic<int>   capLen { 0 }, capTarget { 0 };
    std::atomic<bool>  capturing { false };
    std::atomic<bool>  capFromOutput { false };   // false = mic in, true = converted out
    std::vector<float> prevBuf;
    std::atomic<int>   prevLen { 0 };
    std::atomic<int>   prevPos { -1 };                 // -1 = stopped

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    PsolaEngine engine;
    std::vector<float> monoScratch;

    std::atomic<float>* pPitch     = nullptr;
    std::atomic<float>* pFormant   = nullptr;
    std::atomic<float>* pConsonant = nullptr;
    std::atomic<float>* pF1S = nullptr; std::atomic<float>* pF1G = nullptr;
    std::atomic<float>* pF2S = nullptr; std::atomic<float>* pF2G = nullptr;
    std::atomic<float>* pF3S = nullptr; std::atomic<float>* pF3G = nullptr;
    std::atomic<float>* pBreath2   = nullptr;
    std::atomic<float>* pAir       = nullptr;
    std::atomic<float>* pAirBand   = nullptr;
    std::atomic<float>* pGci       = nullptr;
    std::atomic<float>* pHiFreq    = nullptr;
    std::atomic<float>* pHiPitch   = nullptr;
    std::atomic<float>* pHiFmt     = nullptr;
    std::atomic<float>* pRange     = nullptr;
    std::atomic<float>* pCenter    = nullptr;
    std::atomic<float>* pTilt      = nullptr;
    std::atomic<float>* pJitter    = nullptr;
    std::atomic<float>* pRobot     = nullptr;
    std::atomic<float>* pLowVoice  = nullptr;
    std::atomic<float>* pFloor     = nullptr;
    std::atomic<float>* pAutoMute  = nullptr;
    std::atomic<float>* pLowLat    = nullptr;

    // feedback-runaway protection state
    float rmsSm = 0.0f, loudSec = 0.0f, muteSec = 0.0f, muteGain = 1.0f;
    std::atomic<float>* pRobotHz   = nullptr;
    std::atomic<float>* pMix       = nullptr;
    std::atomic<float>* pGain      = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VoxMorphProcessor)
};
