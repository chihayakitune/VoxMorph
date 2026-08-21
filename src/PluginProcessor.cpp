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
    // Formant Definition: peak-to-valley sharpness of the resonance
    // contour, with the centres left alone. Signed, 0 = legacy behaviour
    // (the engine starts no spectral path for it), so sessions and presets
    // that predate the feature load as 0 and sound exactly as before.
    layout.add (std::make_unique<P> (juce::ParameterID { "resonance", 1 }, "Formant Definition (%)",
                juce::NormalisableRange<float> (-100.0f, 100.0f, 1.0f), 0.0f));
    // AEIOU Character (v0.26.0; ids "vadapt"/"vamount" kept from the
    // v0.25.0 Beta for session compatibility). vadapt defaults OFF, so old
    // sessions/presets that predate the feature stay bit-identical even
    // though vamount now defaults to 60 % (the feature is gated by vadapt;
    // v0.25.0 sessions carry their own saved vamount value and restore it).
    layout.add (std::make_unique<juce::AudioParameterBool> (
                juce::ParameterID { "vadapt", 1 }, "AEIOU Character", false));
    layout.add (std::make_unique<P> (juce::ParameterID { "vamount", 1 }, "AEIOU Amount (%)",
                juce::NormalisableRange<float> (0.0f, 200.0f, 1.0f), 60.0f));
    layout.add (std::make_unique<juce::AudioParameterChoice> (
                juce::ParameterID { "vcharacter", 1 }, "AEIOU Character Type",
                juce::StringArray { "Natural", "Soft", "Active", "Loli",
                                    "Anime", "Lily", "Elegant", "Uni", "Custom" }, 0));
    // Fmt Character (v0.36.7): a preset for the seven global formant rows
    // (Consonant + F1-F3 shift/gain). UI-only — the editor writes those
    // parameters, the engine never reads this. Defaults to Custom so an
    // existing session keeps whatever its sliders already hold instead of
    // claiming to be a character it is not.
    layout.add (std::make_unique<juce::AudioParameterChoice> (
                juce::ParameterID { "fcharacter", 1 }, "Fmt Character Type",
                juce::StringArray { "Natural", "Soft", "Active", "Loli",
                                    "Anime", "Lily", "Elegant", "Uni", "Custom" }, 8));
    // Custom map: 15 shift ids (vowel a/i/u/e/o x F1/F2/F3, semitones) +
    // 15 gain ids (dB, v0.27.0), defaults = the Natural preset. Kept in
    // the state even while a built-in Character is selected.
    {
        static const char* vw[5] = { "a", "i", "u", "e", "o" };
        static constexpr float rng[3] = { 2.0f, 3.0f, 1.5f };
        const auto& nat = getAEIOUCharacterMap (AEIOUCharacter::natural);
        for (int v = 0; v < 5; ++v)
            for (int f = 0; f < 3; ++f)
            {
                const auto id = juce::String ("va_") + vw[v] + "_f" + juce::String (f + 1);
                const auto nm = juce::String ("AEIOU ") + juce::String (vw[v]).toUpperCase()
                              + " F" + juce::String (f + 1) + " (st)";
                layout.add (std::make_unique<P> (juce::ParameterID { id, 1 }, nm,
                            juce::NormalisableRange<float> (-rng[f], rng[f], 0.01f),
                            nat.offset[v][f]));
            }
        for (int v = 0; v < 5; ++v)
            for (int f = 0; f < 3; ++f)
            {
                const auto id = juce::String ("va_") + vw[v] + "_g" + juce::String (f + 1);
                const auto nm = juce::String ("AEIOU ") + juce::String (vw[v]).toUpperCase()
                              + " F" + juce::String (f + 1) + " Gain (dB)";
                layout.add (std::make_unique<P> (juce::ParameterID { id, 1 }, nm,
                            juce::NormalisableRange<float> (-3.0f, 3.0f, 0.1f),
                            nat.gainDb[v][f]));
            }
    }
    layout.add (std::make_unique<P> (juce::ParameterID { "breath2", 1 }, "Breath",
                juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.0f));
    layout.add (std::make_unique<P> (juce::ParameterID { "air", 1 }, "Natural Air",
                juce::NormalisableRange<float> (0.0f, 1.5f, 0.001f), 0.0f));
    // DEPRECATED / legacy compatibility (since v0.24.0): "airband", "air2"
    // and "air2low" stay registered so old DAW sessions, presets and
    // automation lanes still load, but the DSP ignores them entirely —
    // Natural Air always uses the standard path (band-adaptive comb + both
    // cleanup stages, fixed band weights). Not shown in the UI. Remove only
    // after confirming hosts tolerate the id/order change.
    layout.add (std::make_unique<P> (juce::ParameterID { "airband", 1 }, "Air Preserve Band (deprecated)",
                juce::NormalisableRange<float> (500.0f, 3000.0f, 1.0f, 0.5f), 1000.0f));
    layout.add (std::make_unique<juce::AudioParameterBool> (
                juce::ParameterID { "air2", 1 }, "Natural Air v2 (deprecated)", false));
    layout.add (std::make_unique<P> (juce::ParameterID { "airshine", 1 }, "Air Shine (dB)",
                juce::NormalisableRange<float> (0.0f, 6.0f, 0.1f), 0.0f));
    layout.add (std::make_unique<juce::AudioParameterBool> (
                juce::ParameterID { "air2low", 1 }, "Air Low Cleanup (deprecated)", true));
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
    // Pulse Smoothing (v0.50.0). ON by default: it only engages on upward
    // shifts beyond ~3.9 st, and there it removes the period-2 pulse
    // alternation that a slightly creaky voice turns into a growl at the OLD
    // pitch. Measured on a real take at +9 st, the half-integer harmonic
    // content drops 4.3 dB overall and 7.7 dB in the worst passage.
    //
    // NOTE this default changes the sound of existing sessions that use a
    // large upshift. That is deliberate and was the user's call after an A/B;
    // the engine's own Params default stays false so offline_test, bitexact
    // and any direct user of PsolaEngine are unaffected.
    layout.add (std::make_unique<juce::AudioParameterBool> (
                juce::ParameterID { "pulsesmooth", 1 }, "Pulse Smoothing", true));
    // Onset Hold (v0.54.0). ON by default: it fixes a defect, not a taste.
    // Voicing was dropping out for two or three frames in the middle of a
    // phrase attack -- YIN's difference function reads a fast crescendo as
    // aperiodic -- and the unvoiced path does not pitch-shift, so a burst of
    // the speaker's own untransposed voice came through at the loudest point
    // of the attack. The user heard it as a thump, "like hitting a desk".
    // Measured on their take at +9 st: at the moment they pointed at, the
    // output's dominant peak was 106 Hz (their own pitch) and is now 178 Hz,
    // with the 60-140 Hz share down from -1.7 to -10.5 dB; over the whole
    // 110 s, dropouts inside voiced speech fall from 78 to 42 and the
    // periodicity imposed on fricatives does not move (0.222 -> 0.217).
    layout.add (std::make_unique<juce::AudioParameterBool> (
                juce::ParameterID { "onsethold", 1 }, "Onset Hold", true));
    // Pulse Body (v0.52.0). Default raised to 0.75 in v0.53.0 after the user
    // A/B'd the four settings: that is the value where the output waveform's
    // positive/negative peak ratio lands on the original recording's (1.007
    // against 1.046), and the user chose it knowing the low end costs about
    // 2 dB. See the Params comment in PsolaEngine.h.
    //
    // NOTE this changes the sound of existing sessions that use a large
    // upshift, exactly as the Pulse Smoothing default did in v0.50.0, and for
    // the same reason: it is the setting the user picked by ear. The engine's
    // own Params default stays 0 so offline_test, bitexact and any direct
    // user of PsolaEngine keep the old behaviour.
    layout.add (std::make_unique<P> (juce::ParameterID { "pulsebody", 1 }, "Pulse Body",
                juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f), 0.75f));
    layout.add (std::make_unique<juce::AudioParameterBool> (
                juce::ParameterID { "automute", 1 }, "Auto-Mute on Feedback", true));
    // Legacy Low Latency (v0.31.0: moved to the BETA window, id unchanged).
    // This is the OLD experimental mode: it buys latency by changing the
    // engine's analysis conditions (half the lookahead, pitch floor up to
    // ~90 Hz, narrower grain caps), i.e. it trades quality for delay. The id,
    // range and default are frozen so old sessions, presets and automation
    // lanes keep working exactly as before. Do NOT reuse it for anything.
    layout.add (std::make_unique<juce::AudioParameterBool> (
                juce::ParameterID { "lowlat", 1 }, "Legacy Low Latency (Beta)", false));
    // Performance Mode (v0.31.0). A NEW, independent switch that never
    // touches any DSP quality condition — see the Performance Mode notes in
    // PluginProcessor.h. It only reduces work that does not affect the
    // converted samples (display/analysis refresh), so small device buffers
    // become easier to sustain. Can be on at the same time as "lowlat".
    layout.add (std::make_unique<juce::AudioParameterBool> (
                juce::ParameterID { "perfmode", 1 }, "Performance Mode", false));
    layout.add (std::make_unique<juce::AudioParameterBool> (
                juce::ParameterID { "stereo", 1 }, "Stereo Input (Binaural)", false));
    layout.add (std::make_unique<P> (juce::ParameterID { "pitchfloor", 1 }, "Pitch Floor (Hz)",
                juce::NormalisableRange<float> (0.0f, 300.0f, 1.0f), 0.0f));
    layout.add (std::make_unique<P> (juce::ParameterID { "robotHz", 1 }, "Robot Pitch (Hz)",
                juce::NormalisableRange<float> (40.0f, 400.0f, 0.1f, 0.5f), 120.0f));
    // v0.36.6: on/off for the two guards. Both default to ON so that every
    // session and preset saved before they existed loads unchanged — with
    // their amount at 0 (hifreq / pitchfloor) the guard is inert anyway, so
    // "on with nothing set" is exactly the old behaviour.
    layout.add (std::make_unique<juce::AudioParameterBool> (
                juce::ParameterID { "hienable", 1 }, "High Range", true));
    layout.add (std::make_unique<juce::AudioParameterBool> (
                juce::ParameterID { "lowlimit", 1 }, "Low Limit", true));
    layout.add (std::make_unique<P> (juce::ParameterID { "hifreq", 1 }, "High Range Start (Hz)",
                juce::NormalisableRange<float> (0.0f, 600.0f, 1.0f, 0.5f), 0.0f));
    layout.add (std::make_unique<P> (juce::ParameterID { "hipitch", 1 }, "High Pitch Amount (%)",
                juce::NormalisableRange<float> (0.0f, 100.0f, 1.0f), 50.0f));
    layout.add (std::make_unique<P> (juce::ParameterID { "hiformant", 1 }, "High Formant Amount (%)",
                juce::NormalisableRange<float> (0.0f, 100.0f, 1.0f), 100.0f));
    layout.add (std::make_unique<P> (juce::ParameterID { "gate", 1 }, "Noise Gate (dB)",
                juce::NormalisableRange<float> (-80.0f, -20.0f, 1.0f), -80.0f));
    layout.add (std::make_unique<P> (juce::ParameterID { "asmrx", 1 }, "ASMR X",
                juce::NormalisableRange<float> (-1.0f, 1.0f, 0.001f), 0.0f));
    layout.add (std::make_unique<P> (juce::ParameterID { "asmry", 1 }, "ASMR Y",
                juce::NormalisableRange<float> (-1.0f, 1.0f, 0.001f), 0.0f));
    // ---- ASMR spatial stage (v0.45.0) -----------------------------------
    // EVERY default below is the value at which its stage is skipped, so a
    // session or preset written before this release loads with the whole
    // extension inert and comes out bit-identical to v0.44. That property is
    // checked in test/spatial_test.cpp; do not "improve" a default here
    // without re-running it.
    layout.add (std::make_unique<juce::AudioParameterBool> (
                juce::ParameterID { "asmrbin", 1 }, "ASMR Binaural Cues", false));
    layout.add (std::make_unique<P> (juce::ParameterID { "asmrdist", 1 }, "ASMR Distance Amount (%)",
                juce::NormalisableRange<float> (0.0f, 200.0f, 1.0f), 100.0f));
    layout.add (std::make_unique<P> (juce::ParameterID { "asmrair", 1 }, "ASMR Air Absorption (%)",
                juce::NormalisableRange<float> (0.0f, 100.0f, 1.0f), 0.0f));
    layout.add (std::make_unique<P> (juce::ParameterID { "asmrroom", 1 }, "ASMR Room Ambience (%)",
                juce::NormalisableRange<float> (0.0f, 100.0f, 1.0f), 0.0f));
    layout.add (std::make_unique<P> (juce::ParameterID { "asmrsize", 1 }, "ASMR Room Size (%)",
                juce::NormalisableRange<float> (0.0f, 100.0f, 1.0f), 50.0f));
    layout.add (std::make_unique<P> (juce::ParameterID { "asmrwidth", 1 }, "ASMR Stereo Width (%)",
                juce::NormalisableRange<float> (0.0f, 200.0f, 1.0f), 100.0f));
    layout.add (std::make_unique<P> (juce::ParameterID { "asmrorbit", 1 }, "ASMR Orbit Rate (Hz)",
                juce::NormalisableRange<float> (0.0f, 2.0f, 0.01f, 0.5f), 0.0f));
    layout.add (std::make_unique<P> (juce::ParameterID { "asmrdepth", 1 }, "ASMR Orbit Depth (%)",
                juce::NormalisableRange<float> (0.0f, 100.0f, 1.0f), 60.0f));
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

    fxFormats.addFormat (new juce::VST3PluginFormat());

    pPitch     = apvts.getRawParameterValue ("pitch");
    pFormant   = apvts.getRawParameterValue ("formant");
    pConsonant = apvts.getRawParameterValue ("consonant");
    pF1S = apvts.getRawParameterValue ("f1shift");
    pF1G = apvts.getRawParameterValue ("f1gain");
    pF2S = apvts.getRawParameterValue ("f2shift");
    pF2G = apvts.getRawParameterValue ("f2gain");
    pF3S = apvts.getRawParameterValue ("f3shift");
    pF3G = apvts.getRawParameterValue ("f3gain");
    pReso = apvts.getRawParameterValue ("resonance");
    pVAdapt  = apvts.getRawParameterValue ("vadapt");
    pVAmount = apvts.getRawParameterValue ("vamount");
    pVChar   = apvts.getRawParameterValue ("vcharacter");
    {
        static const char* vw[5] = { "a", "i", "u", "e", "o" };
        for (int v = 0; v < 5; ++v)
            for (int f = 0; f < 3; ++f)
            {
                pVCustom[v][f] = apvts.getRawParameterValue (
                    juce::String ("va_") + vw[v] + "_f" + juce::String (f + 1));
                pVCustomG[v][f] = apvts.getRawParameterValue (
                    juce::String ("va_") + vw[v] + "_g" + juce::String (f + 1));
            }
    }
    pBreath2 = apvts.getRawParameterValue ("breath2");
    pAir     = apvts.getRawParameterValue ("air");
    pAirShine = apvts.getRawParameterValue ("airshine");
    // (deprecated "airband"/"air2"/"air2low" are intentionally not read)
    pGci     = apvts.getRawParameterValue ("gci");
    pHiOn    = apvts.getRawParameterValue ("hienable");
    pLowOn   = apvts.getRawParameterValue ("lowlimit");
    pHiFreq  = apvts.getRawParameterValue ("hifreq");
    pHiPitch = apvts.getRawParameterValue ("hipitch");
    pHiFmt   = apvts.getRawParameterValue ("hiformant");
    pRange     = apvts.getRawParameterValue ("range");
    pCenter    = apvts.getRawParameterValue ("center");
    pTilt      = apvts.getRawParameterValue ("tilt");
    pJitter    = apvts.getRawParameterValue ("jitter");
    pRobot     = apvts.getRawParameterValue ("robot");
    pLowVoice  = apvts.getRawParameterValue ("lowvoice");
    pPulseSmooth = apvts.getRawParameterValue ("pulsesmooth");
    pPulseBody   = apvts.getRawParameterValue ("pulsebody");
    pOnsetHold   = apvts.getRawParameterValue ("onsethold");
    pFloor     = apvts.getRawParameterValue ("pitchfloor");
    pAutoMute  = apvts.getRawParameterValue ("automute");
    pLowLat    = apvts.getRawParameterValue ("lowlat");
    pPerf      = apvts.getRawParameterValue ("perfmode");
    pRobotHz   = apvts.getRawParameterValue ("robotHz");
    pMix       = apvts.getRawParameterValue ("mix");
    pGain      = apvts.getRawParameterValue ("gain");
    pGate      = apvts.getRawParameterValue ("gate");
    pAsmrX     = apvts.getRawParameterValue ("asmrx");
    pAsmrY     = apvts.getRawParameterValue ("asmry");
    pAsmrBin   = apvts.getRawParameterValue ("asmrbin");
    pAsmrDist  = apvts.getRawParameterValue ("asmrdist");
    pAsmrAir   = apvts.getRawParameterValue ("asmrair");
    pAsmrRoom  = apvts.getRawParameterValue ("asmrroom");
    pAsmrSize  = apvts.getRawParameterValue ("asmrsize");
    pAsmrWidth = apvts.getRawParameterValue ("asmrwidth");
    pAsmrOrbit = apvts.getRawParameterValue ("asmrorbit");
    pAsmrDepth = apvts.getRawParameterValue ("asmrdepth");
    pStereo    = apvts.getRawParameterValue ("stereo");

    loadFxChains();   // standalone: restore the saved Pre/Post FX setup
    history.init (*this);
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
    engineR.prepare (sampleRate);
    spatial.prepare (sampleRate, samplesPerBlock);   // allocates its delay lines
    monoScratch.assign ((size_t) samplesPerBlock, 0.0f);
    scratchL.assign ((size_t) samplesPerBlock, 0.0f);
    scratchR.assign ((size_t) samplesPerBlock, 0.0f);

    capBuf.assign ((size_t) (sampleRate * 15.0), 0.0f);
    capLen = 0;  capTarget = 0;  capturing = false;
    prevBuf.assign ((size_t) (sampleRate * 60.0), 0.0f);
    prevLen = 0;  prevPos = -1;
    myBuf.assign ((size_t) (sampleRate * 60.0), 0.0f);
    myLen = 0;  myPos = -1;

    // reset the per-run smoothing states so a device restart (or host
    // transport re-prepare) never resumes from a stale mute/gate fade
    rmsSm = 0.0f;  loudSec = 0.0f;  muteSec = 0.0f;  muteGain = 1.0f;
    gateEnv = 0.0f;  gateGain = 1.0f;
    spatial.reset();          // clears the pan smoothers and any room tail
    gainSm = juce::Decibels::decibelsToGain (pGain->load());
    uiInL.reset();  uiInR.reset();                    // UI meter ballistics
    uiOutL.reset(); uiOutR.reset();

    fxSr = sampleRate;  fxBlk = samplesPerBlock;
    fxScratch.setSize (2, samplesPerBlock);
    {
        const juce::ScopedLock sl (fxLock);
        int fx = 0;
        for (auto* chain : { &preChain, &postChain })
            for (auto* s : *chain)
                if (s->plugin != nullptr)
                {
                    s->plugin->setPlayConfigDetails (2, 2, fxSr, fxBlk);
                    s->plugin->prepareToPlay (fxSr, fxBlk);
                    if (s->enabled.load())
                        fx += s->plugin->getLatencySamples();
                }
        fxLatSamples = fx;
    }

    // initial latency report: engine lookahead + enabled hosted FX
    uiFxLatSamples.store (fxLatSamples, std::memory_order_relaxed);
    uiLatencySamples.store (engine.latencySamples() + fxLatSamples, std::memory_order_relaxed);
    setLatencySamples (engine.latencySamples() + fxLatSamples);
    pendingLat = -1;  pendingLatSec = 0.0f;
}

void VoxMorphProcessor::releaseResources()
{
    const juce::ScopedLock sl (fxLock);
    for (auto* chain : { &preChain, &postChain })
        for (auto* s : *chain)
            if (s->plugin != nullptr)
                s->plugin->releaseResources();
}

juce::String VoxMorphProcessor::addFx (bool post, const juce::File& vst3)
{
    juce::OwnedArray<juce::PluginDescription> types;
    for (auto* fmt : fxFormats.getFormats())
        fmt->findAllTypesForFile (types, vst3.getFullPathName());
    if (types.isEmpty())
        return "VST3として認識できませんでした";

    juce::String err;
    auto inst = fxFormats.createPluginInstance (*types[0], fxSr, fxBlk, err);
    if (inst == nullptr)
        return err.isNotEmpty() ? err : juce::String ("読み込みに失敗しました");

    inst->setPlayConfigDetails (2, 2, fxSr, fxBlk);
    inst->prepareToPlay (fxSr, fxBlk);
    auto slot = std::make_unique<FxSlot>();
    slot->plugin = std::move (inst);
    slot->path   = vst3.getFullPathName();
    {
        const juce::ScopedLock sl (fxLock);
        (post ? postChain : preChain).add (slot.release());
        publishFxCount();
    }
    saveFxChains();
    return {};
}

void VoxMorphProcessor::removeFx (bool post, int index)
{
    std::unique_ptr<FxSlot> old;
    {
        const juce::ScopedLock sl (fxLock);
        auto& c = post ? postChain : preChain;
        if (juce::isPositiveAndBelow (index, c.size()))
            old.reset (c.removeAndReturn (index));
        publishFxCount();
    }
    if (old != nullptr && old->plugin != nullptr)
        old->plugin->releaseResources();   // audio thread can no longer see it
    saveFxChains();
}

void VoxMorphProcessor::setFxEnabled (bool post, int i, bool on)
{
    if (auto* s = getFxSlot (post, i))
    {
        s->enabled = on;
        saveFxChains();
    }
}

static juce::File fxChainFile()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               .getChildFile ("VoxMorph").getChildFile ("fxchains.xml");
}

void VoxMorphProcessor::saveFxChains()
{
    if (wrapperType != wrapperType_Standalone || fxLoading)
        return;
    juce::XmlElement root ("VMFXCHAINS");
    for (int post = 0; post <= 1; ++post)
        for (auto* s : (post != 0 ? postChain : preChain))
            if (s->plugin != nullptr)
            {
                auto* e = root.createNewChildElement ("FX");
                e->setAttribute ("post", post);
                e->setAttribute ("path", s->path);
                e->setAttribute ("enabled", s->enabled.load() ? 1 : 0);
                juce::MemoryBlock mb;
                s->plugin->getStateInformation (mb);
                e->setAttribute ("state", mb.toBase64Encoding());
            }
    auto f = fxChainFile();
    f.getParentDirectory().createDirectory();
    root.writeTo (f);
}

void VoxMorphProcessor::loadFxChains()
{
    if (wrapperType != wrapperType_Standalone)
        return;
    auto xml = juce::XmlDocument::parse (fxChainFile());
    if (xml == nullptr || ! xml->hasTagName ("VMFXCHAINS"))
        return;
    fxLoading = true;
    for (auto* e : xml->getChildIterator())
    {
        const bool post = e->getIntAttribute ("post") != 0;
        const juce::File f (e->getStringAttribute ("path"));
        if (! f.exists() || addFx (post, f).isNotEmpty())
            continue;                                    // moved/uninstalled: skip
        auto& c = post ? postChain : preChain;
        if (auto* s = c.getLast())
        {
            s->enabled = e->getIntAttribute ("enabled", 1) != 0;
            juce::MemoryBlock mb;
            if (mb.fromBase64Encoding (e->getStringAttribute ("state")) && mb.getSize() > 0)
                s->plugin->setStateInformation (mb.getData(), (int) mb.getSize());
        }
    }
    fxLoading = false;
}

VoxMorphProcessor::~VoxMorphProcessor()
{
    saveFxChains();   // capture the plugins' latest internal states on quit
}

// external plugins can emit NaN/Inf; replace with silence so it never
// poisons the engine's filter states or reaches the speakers
static void sanitizeFx (float* d, int n)
{
    for (int i = 0; i < n; ++i)
        if (! std::isfinite (d[i])) d[i] = 0.0f;
}

// run a mono signal through a (2-in/2-out prepared) plugin: duplicate to
// stereo, process, average back. Processed in chunks no larger than the
// prepared block size, so an oversized host buffer is never fed to the FX
void VoxMorphProcessor::applyFxMono (juce::AudioPluginInstance& fx, float* m, int n)
{
    const int maxN = std::min (fxScratch.getNumSamples(), fxBlk);
    if (maxN <= 0) return;
    for (int off = 0; off < n; off += maxN)
    {
        const int c = std::min (maxN, n - off);
        fxScratch.copyFrom (0, 0, m + off, c);
        fxScratch.copyFrom (1, 0, m + off, c);
        juce::AudioBuffer<float> sub (fxScratch.getArrayOfWritePointers(), 2, 0, c);
        juce::MidiBuffer midi;
        fx.processBlock (sub, midi);
        const float* L = sub.getReadPointer (0);
        const float* R = sub.getReadPointer (1);
        for (int i = 0; i < c; ++i) m[off + i] = 0.5f * (L[i] + R[i]);
    }
}

void VoxMorphProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int n  = buffer.getNumSamples();
    const int ch = buffer.getNumChannels();
    if ((int) monoScratch.size() < n)
        monoScratch.assign ((size_t) n, 0.0f);

    const float meterDt = (float) n / (float) std::max (1.0, getSampleRate());

    // Display-only taps are skipped whenever nothing is on screen to read
    // them (editor closed, or the meters / visualizer scrolled out of view).
    // Each one is a full extra pass over the block; with a 64-sample device
    // buffer that overhead is a real share of the callback, and it changes
    // no audio whatsoever. See uiWantsMeters / uiWantsViz in the header.
    const bool wantMeters = uiWantsMeters.load (std::memory_order_relaxed);
    const bool wantViz    = uiWantsViz   .load (std::memory_order_relaxed);

    // Performance Mode is republished for the UI timers and NEVER given to
    // the engine: no DSP decision anywhere reads it. See PluginProcessor.h.
    uiPerfMode.store (pPerf->load() > 0.5f, std::memory_order_relaxed);

    // INPUT meters: the raw signal as it arrives, i.e. before the noise gate
    // and the Pre FX, so the meter still shows your mic while the gate has
    // it shut. buffer still holds the untouched input at this point (the
    // conversion works in monoScratch / scratchL / scratchR).
    if (wantMeters && ch > 0)
    {
        uiInL.push (buffer.getReadPointer (0), n, meterDt);
        uiInR.push (buffer.getReadPointer (ch > 1 ? 1 : 0), n, meterDt);
    }

    PsolaEngine::Params p;
    p.pitchSemi     = pPitch->load();
    p.formantSemi   = pFormant->load();
    p.consonantSemi = pConsonant->load();
    p.pitchRange    = pRange->load() * 0.01f;   // % -> ratio
    p.pitchCenterHz = pCenter->load();
    p.breath        = pBreath2->load();      // spectral (noise-excited envelope)
    p.airPreserve   = pAir->load();          // Natural Air (standard path)
    p.airShineDb    = pAirShine->load();     // Air Shine
    p.gciSync       = pGci->load() > 0.5f;
    // The toggles gate the guards here rather than in the engine: it already
    // reads "start = 0" and "floor = 0" as off, so switching them off is the
    // same code path it has always taken.
    p.hiRangeHz     = pHiOn->load() > 0.5f ? pHiFreq->load() : 0.0f;
    p.hiPitchAmt    = pHiPitch->load() * 0.01f;
    p.hiFormantAmt  = pHiFmt->load()   * 0.01f;
    p.tiltDb        = pTilt->load();
    p.f1Shift = pF1S->load();  p.f1Gain = pF1G->load();
    p.f2Shift = pF2S->load();  p.f2Gain = pF2G->load();
    p.f3Shift = pF3S->load();  p.f3Gain = pF3G->load();
    p.formantDefinition = pReso->load();
    p.vowelAdapt    = pVAdapt->load() > 0.5f;      // AEIOU Character
    p.vowelAdaptAmt = pVAmount->load() * 0.01f;    // % -> 0..1
    // The 30-float map is only read while the warp is running; at 0 % (or
    // off) the engine ignores it, so building it would be pure work. The
    // copy still happens on the very block that switches the feature on.
    if (p.vowelAdapt && p.vowelAdaptAmt > 1.0e-4f)
    {
        // Character selection -> per-vowel map. Custom reads the 15 APVTS
        // values; every other choice copies the immutable built-in preset
        // (plain float copies on the audio thread, no lookup, no strings).
        const auto ch = (AEIOUCharacter) (int) pVChar->load();
        if (ch == AEIOUCharacter::custom)
            for (int v = 0; v < 5; ++v)
                for (int f = 0; f < 3; ++f)
                {
                    p.vowelMap.offset[v][f] = pVCustom[v][f]->load();
                    p.vowelMap.gainDb[v][f] = pVCustomG[v][f]->load();
                }
        else
            p.vowelMap = getAEIOUCharacterMap (ch);
    }
    p.jitter        = pJitter->load();
    p.robotize      = pRobot->load() > 0.5f;
    p.lowVoice      = pLowVoice->load() > 0.5f;
    p.grainAvg      = pPulseSmooth->load() > 0.5f;
    p.pulseBody     = pPulseBody->load();
    p.onsetHold     = pOnsetHold->load() > 0.5f ? 3 : 0;   // 3 frames = ~35 ms
    p.pitchFloorHz  = pLowOn->load() > 0.5f ? pFloor->load() : 0.0f;
    p.lowLatency    = pLowLat->load() > 0.5f;
    p.robotHz       = pRobotHz->load();
    p.mix           = pMix->load();
    engine.setParams (p);

    // Stereo Input mode (binaural/ASMR mics): L and R run through two
    // independent conversion engines in PARALLEL, so the latency is the
    // same as mono. m stays the mono sum and keeps feeding every analysis
    // tap (visualizer, ANALYZE captures, auto-mute) exactly as before.
    const bool stereoMode = ch >= 2 && pStereo->load() > 0.5f;
    if ((int) scratchL.size() < n) { scratchL.assign ((size_t) n, 0.0f); scratchR.assign ((size_t) n, 0.0f); }
    float* sL = scratchL.data();
    float* sR = scratchR.data();
    if (stereoMode)
    {
        engineR.setParams (p);
        std::copy (buffer.getReadPointer (0), buffer.getReadPointer (0) + n, sL);
        std::copy (buffer.getReadPointer (1), buffer.getReadPointer (1) + n, sR);
    }

    // mono-sum the input (mono path input; analysis taps in stereo mode)
    float* m = monoScratch.data();
    if (ch == 1)
        std::copy (buffer.getReadPointer (0), buffer.getReadPointer (0) + n, m);
    else
    {
        const float* L = buffer.getReadPointer (0);
        const float* R = buffer.getReadPointer (1);
        for (int i = 0; i < n; ++i) m[i] = 0.5f * (L[i] + R[i]);
    }

    // Pre FX chain (standalone external plugins, e.g. a de-noiser).
    // fxCount is the message thread's published slot total; while it is 0
    // (every plugin build, and the app until FX are added) there is nothing
    // to run, so the lock is not even attempted.
    if (fxCount.load (std::memory_order_relaxed) > 0)
    {
        bool ranPre = false;
        const juce::ScopedTryLock tl (fxLock);
        if (tl.isLocked())
            for (auto* s : preChain)
                if (s->enabled.load() && s->plugin != nullptr)
                {
                    if (stereoMode)      // true stereo through the FX, chunked
                    {
                        const int maxN = std::min (fxScratch.getNumSamples(), fxBlk);
                        for (int off = 0; maxN > 0 && off < n; off += maxN)
                        {
                            const int c = std::min (maxN, n - off);
                            fxScratch.copyFrom (0, 0, sL + off, c);
                            fxScratch.copyFrom (1, 0, sR + off, c);
                            juce::AudioBuffer<float> sub (fxScratch.getArrayOfWritePointers(), 2, 0, c);
                            juce::MidiBuffer midi;
                            s->plugin->processBlock (sub, midi);
                            std::copy (sub.getReadPointer (0), sub.getReadPointer (0) + c, sL + off);
                            std::copy (sub.getReadPointer (1), sub.getReadPointer (1) + c, sR + off);
                        }
                    }
                    else
                        applyFxMono (*s->plugin, m, n);
                    ranPre = true;
                }
        if (ranPre)          // NaN/Inf guard before anything downstream
        {
            sanitizeFx (m, n);
            if (stereoMode) { sanitizeFx (sL, n); sanitizeFx (sR, n); }
            if (stereoMode)  // the FX changed sL/sR: re-derive the analysis sum
                for (int i = 0; i < n; ++i) m[i] = 0.5f * (sL[i] + sR[i]);
        }
    }

    // Noise gate: while the input stays below the threshold, fade it out.
    // Fast open (~1 ms), slow close (~25 ms) so word onsets are kept.
    // Stereo mode: one shared gain (driven by the louder channel) so the
    // image never lurches sideways.
    const float gateThr = pGate->load();
    if (gateThr > -79.5f)
    {
        const float lt = juce::Decibels::decibelsToGain (gateThr);
        for (int i = 0; i < n; ++i)
        {
            const float a = stereoMode ? std::max (std::abs (sL[i]), std::abs (sR[i]))
                                       : std::abs (m[i]);
            gateEnv  += (a > gateEnv ? 0.30f : 0.0015f) * (a - gateEnv);
            const float t = gateEnv > lt ? 1.0f : 0.0f;
            gateGain += (t > gateGain ? 0.02f : 0.0008f) * (t - gateGain);
            m[i] *= gateGain;
            if (stereoMode) { sL[i] *= gateGain; sR[i] *= gateGain; }
        }
    }

    const unsigned vp = vizPos.load (std::memory_order_relaxed);
    if (wantViz)
        for (int i = 0; i < n; ++i)
            vizIn[(size_t) ((vp + (unsigned) i) & (unsigned) (kVizLen - 1))]
                .store (m[i], std::memory_order_relaxed);

    if (capturing.load() && ! capFromOutput.load())   // ANALYZE: capture raw input
    {
        const int cl   = capLen.load();
        const int room = std::min ((int) capBuf.size(), capTarget.load()) - cl;
        const int c    = std::max (0, std::min (n, room));
        if (c > 0) std::copy (m, m + c, capBuf.data() + cl);
        capLen.store (cl + c);
        if (c >= room) capturing.store (false);
    }

    if (stereoMode)
    {
        engine.process  (sL, sL, n);
        engineR.process (sR, sR, n);
        for (int i = 0; i < n; ++i) m[i] = 0.5f * (sL[i] + sR[i]);   // analysis taps
    }
    else
        engine.process (m, m, n);

    // AEIOU vowel readout for the UI vowel meter. The engine only tracks vowels
    // while the AEIOU Character warp is on (it is what drives the tracking),
    // so publish that flag too and let the UI show an inactive state rather
    // than drawing values that stopped updating.
    {
        const bool vaLive = p.vowelAdapt && p.vowelAdaptAmt > 1.0e-4f;
        uiVowelActive.store (vaLive, std::memory_order_relaxed);
        if (vaLive)
        {
            uiVowelH   .store (engine.vowelHeight(),     std::memory_order_relaxed);
            uiVowelF   .store (engine.vowelFrontness(),  std::memory_order_relaxed);
            uiVowelConf.store (engine.vowelConfidence(), std::memory_order_relaxed);
        }
        else
            uiVowelConf.store (0.0f, std::memory_order_relaxed);
    }

    // Detection readout for the VISUALIZER lane (v0.47.0). Always the LEFT
    // engine: in Stereo Input mode both run the same parameters, and every
    // other analysis tap on this page is the mono/left one too.
    {
        uiF0In .store (engine.analysisF0In(),  std::memory_order_relaxed);
        uiF0Out.store (engine.analysisF0Out(), std::memory_order_relaxed);
        const bool fv = engine.formantsValid();
        for (int i = 0; i < 3; ++i)
        {
            uiFmtIn [i].store (fv ? engine.analysisFormantIn  (i) : 0.0f,
                               std::memory_order_relaxed);
            uiFmtOut[i].store (fv ? engine.analysisFormantOut (i) : 0.0f,
                               std::memory_order_relaxed);
            uiFmtMerged[i].store (fv && engine.formantMerged (i),
                                  std::memory_order_relaxed);
            uiFmtConf[i].store (fv ? engine.formantConfidence (i) : 0.0f,
                                std::memory_order_relaxed);
        }
        uiFmtValid.store (fv, std::memory_order_relaxed);
    }

    // Latency estimate = engine lookahead (changes with Low Latency Mode)
    // + enabled hosted FX plugins. Pitch/Formant/Voice Quality/Breath run
    // inside the same grain pipeline and add no delay of their own. The UI
    // reads uiLatencySamples; the host is only (re)notified after the value
    // has been stable for 0.5 s, because each setLatencySamples() call can
    // make a DAW rebuild its delay compensation (audible interruption).
    {
        if (fxCount.load (std::memory_order_relaxed) > 0)
        {
            const juce::ScopedTryLock tl (fxLock);
            if (tl.isLocked())
            {
                int fx = 0;
                for (auto* chain : { &preChain, &postChain })
                    for (auto* s : *chain)
                        if (s->enabled.load() && s->plugin != nullptr)
                            fx += s->plugin->getLatencySamples();
                fxLatSamples = fx;
            }
        }
        else
            fxLatSamples = 0;   // no slots: nothing can be contributing delay
        const int totalLat = engine.latencySamples() + fxLatSamples;
        uiFxLatSamples.store (fxLatSamples, std::memory_order_relaxed);
        uiLatencySamples.store (totalLat, std::memory_order_relaxed);

        if (totalLat != getLatencySamples())
        {
            if (totalLat != pendingLat) { pendingLat = totalLat; pendingLatSec = 0.0f; }
            pendingLatSec += (float) n / (float) std::max (1.0, getSampleRate());
            if (pendingLatSec >= 0.5f)
                setLatencySamples (totalLat);
        }
        else
        {
            pendingLat = -1;  pendingLatSec = 0.0f;
        }
    }

    // Feedback-runaway protection (standalone): if the output stays very
    // loud continuously (screaming feedback loop), mute for 3 s. The loop
    // then breaks, the input settles, and normal operation resumes.
    const bool fbActive = wrapperType == wrapperType_Standalone
                       && pAutoMute->load() > 0.5f;
    {
        const float dt = (float) n / (float) std::max (1.0, getSampleRate());

        if (fbActive)
        {
            double sum = 0.0;
            for (int i = 0; i < n; ++i) sum += (double) m[i] * m[i];
            const float rms = (float) std::sqrt (sum / std::max (1, n));
            // time-based smoothing (~50 ms) so behaviour does NOT depend on
            // the host buffer size; threshold high enough that loud singing
            // can't trigger it — genuine runaway feedback reaches full scale
            const float aR = std::min (1.0f, dt / 0.05f);
            rmsSm += aR * (rms - rmsSm);

            if (rmsSm > 0.70f) loudSec += dt;
            else               loudSec = std::max (0.0f, loudSec - 2.0f * dt);
            if (loudSec > 1.5f) { muteSec = 3.0f; loudSec = 0.0f; }
        }
        else
        {
            // Detector switched off (or a plugin build, where it never runs):
            // the level scan was its only consumer, so skip it entirely. The
            // state is cleared rather than frozen, so switching the feature
            // back on starts from silence and still needs a full 1.5 s of
            // runaway level before it can mute — no stale trigger.
            rmsSm = 0.0f;  loudSec = 0.0f;
        }
        if (muteSec > 0.0f) muteSec -= dt;

        // Manual MUTE (options bar). Suppressed while MONITOR is on: the
        // output device is the monitor device then, so silencing it would
        // defeat the purpose — see the comment in PluginProcessor.h.
        const bool userMute = muted.load (std::memory_order_relaxed)
                          && ! monitoring.load (std::memory_order_relaxed);

        const float target = (muteSec > 0.0f || userMute) ? 0.0f : 1.0f;
        if (muteGain < 0.999f || target < 1.0f)
            for (int i = 0; i < n; ++i)
            {
                muteGain += 0.002f * (target - muteGain);
                m[i] *= muteGain;
                if (stereoMode) { sL[i] *= muteGain; sR[i] *= muteGain; }
            }
    }

    // Output gain, smoothed per sample (no zipper noise when the slider
    // moves), baked into m / sL / sR so the visualizer and the Refine
    // capture below see the exact same level as the speakers
    // Zero-value bypass: at 0 dB, once the smoother has settled on unity, the
    // per-sample multiply is a no-op. The smoother is one-pole so it only
    // approaches 1.0 asymptotically; it is snapped the moment the remaining
    // error is below 1e-6 (= -120 dB, far under any audible step) and the
    // loop then stops until the slider moves again.
    {
        const float gT = juce::Decibels::decibelsToGain (pGain->load());
        if (gT == 1.0f && std::abs (gainSm - 1.0f) < 1.0e-6f)
            gainSm = 1.0f;
        else
            for (int i = 0; i < n; ++i)
            {
                gainSm += 0.002f * (gT - gainSm);
                m[i] *= gainSm;
                if (stereoMode) { sL[i] *= gainSm; sR[i] *= gainSm; }
            }
    }

    // ASMR spatial stage (v0.45.0): position, distance, binaural cues, room
    // and width. Up to v0.44 this was a constant-power pan plus a distance
    // attenuation written out here; that maths now lives in SpatialEngine
    // unchanged, with every addition gated on its own control so the defaults
    // still produce the identical samples (test/spatial_test.cpp proves it by
    // memcmp). Everything it does happens AFTER the visualizer tap and the
    // Refine capture below, exactly as the old pan did.
    if (ch > 0)
    {
        SpatialEngine::Params sp;
        sp.x           = pAsmrX->load();
        sp.y           = pAsmrY->load();
        sp.binaural    = pAsmrBin->load() > 0.5f;
        sp.distanceP   = pAsmrDist->load();
        sp.airP        = pAsmrAir->load();
        sp.roomP       = pAsmrRoom->load();
        sp.sizeP       = pAsmrSize->load();
        sp.widthP      = pAsmrWidth->load();
        sp.orbitHz     = pAsmrOrbit->load();
        sp.orbitDepthP = pAsmrDepth->load();

        const float* srcL = stereoMode ? sL : m;
        const float* srcR = stereoMode ? sR : m;
        spatial.process (srcL, ch > 1 ? srcR : nullptr,
                         buffer.getWritePointer (0),
                         ch > 1 ? buffer.getWritePointer (1) : nullptr, n, sp);

        // publish the orbited position for the pad to draw
        uiSpaceX.store (spatial.currentX(), std::memory_order_relaxed);
        uiSpaceY.store (spatial.currentY(), std::memory_order_relaxed);
    }

    // Post FX chain (standalone external plugins) on the converted output;
    // the target-file preview below stays unfiltered. Skipped without even
    // taking the lock while no slots exist (see the Pre FX chain above).
    if (fxCount.load (std::memory_order_relaxed) > 0)
    {
        bool ranPost = false;
        const juce::ScopedTryLock tl (fxLock);
        if (tl.isLocked())
            for (auto* s : postChain)
                if (s->enabled.load() && s->plugin != nullptr)
                {
                    if (ch >= 2)         // chunked to the prepared block size
                    {
                        for (int off = 0; fxBlk > 0 && off < n; off += fxBlk)
                        {
                            const int c = std::min (fxBlk, n - off);
                            juce::AudioBuffer<float> sub (buffer.getArrayOfWritePointers(), ch, off, c);
                            juce::MidiBuffer midi;
                            s->plugin->processBlock (sub, midi);
                        }
                    }
                    else
                        applyFxMono (*s->plugin, buffer.getWritePointer (0), n);
                    ranPost = true;
                }
        if (ranPost)         // NaN/Inf guard before the speakers
            for (int c = 0; c < ch; ++c)
                sanitizeFx (buffer.getWritePointer (c), n);
    }

    if (wantViz)
        for (int i = 0; i < n; ++i)
            vizOut[(size_t) ((vp + (unsigned) i) & (unsigned) (kVizLen - 1))]
                .store (m[i], std::memory_order_relaxed);
    // the write position advances either way, so the reader can tell how much
    // fresh material has arrived since it asked for the taps to be filled --
    // and, while reading, how close we have come to overwriting its window
    vizPos.store (vp + (unsigned) n, std::memory_order_release);

    if (capturing.load() && capFromOutput.load())     // ANALYZE: capture converted
    {                                                 // output (before file preview)
        const int cl   = capLen.load();
        const int room = std::min ((int) capBuf.size(), capTarget.load()) - cl;
        const int c    = std::max (0, std::min (n, room));
        for (int i = 0; i < c; ++i) capBuf[(size_t) (cl + i)] = m[i];
        capLen.store (cl + c);
        if (c >= room) capturing.store (false);
    }

    const int pp = prevPos.load();     // Matching tab: target-file preview
    if (pp >= 0)
    {
        const int pl = prevLen.load();
        const int c  = std::min (n, pl - pp);
        for (int c2 = 0; c2 < ch; ++c2)
        {
            float* d = buffer.getWritePointer (c2);
            for (int i = 0; i < c; ++i) d[i] += 0.9f * prevBuf[(size_t) (pp + i)];
        }
        prevPos.store (pp + n >= pl ? -1 : pp + n);
    }

    const int mp = myPos.load();       // Matching tab: MyVoice-file preview
    if (mp >= 0)
    {
        const int ml = myLen.load();
        const int c  = std::min (n, ml - mp);
        for (int c2 = 0; c2 < ch; ++c2)
        {
            float* d = buffer.getWritePointer (c2);
            for (int i = 0; i < c; ++i) d[i] += 0.9f * myBuf[(size_t) (mp + i)];
        }
        myPos.store (mp + n >= ml ? -1 : mp + n);
    }

    // OUTPUT meters: measured on the finished buffer, so they show exactly
    // what leaves the plugin (mute, gain, ASMR pan, Post FX and the Matching
    // target preview all included).
    if (wantMeters && ch > 0)
    {
        uiOutL.push (buffer.getReadPointer (0), n, meterDt);
        uiOutR.push (buffer.getReadPointer (ch > 1 ? 1 : 0), n, meterDt);
    }
}

void VoxMorphProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    if (auto xml = apvts.copyState().createXml())
    {
        xml->setAttribute ("lockedIds", lockedIds.joinIntoString (","));   // 🔒 params
        xml->setAttribute ("monitorDevice", monitorDeviceName);            // MONITOR target
        xml->setAttribute ("characterImage", characterImagePath);          // badge picture
        copyXmlToBinary (*xml, dest);
    }
}

void VoxMorphProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
    {
        lockedIds = juce::StringArray::fromTokens (xml->getStringAttribute ("lockedIds"), ",", "");
        lockedIds.removeEmptyStrings();
        // The monitor device is a machine-local preference, so a state saved
        // elsewhere simply leaves the existing choice alone (no attribute).
        if (xml->hasAttribute ("monitorDevice"))
            monitorDeviceName = xml->getStringAttribute ("monitorDevice");
        // Same reasoning for the badge picture: a path from another machine
        // will not resolve, and HeroCircle falls back to the built-in art.
        if (xml->hasAttribute ("characterImage"))
            characterImagePath = xml->getStringAttribute ("characterImage");
        apvts.replaceState (juce::ValueTree::fromXml (*xml));
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new VoxMorphProcessor();
}
