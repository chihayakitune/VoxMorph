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
    layout.add (std::make_unique<juce::AudioParameterBool> (
                juce::ParameterID { "air2", 1 }, "Natural Air v2 (Beta)", false));
    layout.add (std::make_unique<P> (juce::ParameterID { "airshine", 1 }, "Air Shine (dB)",
                juce::NormalisableRange<float> (0.0f, 6.0f, 0.1f), 0.0f));
    layout.add (std::make_unique<juce::AudioParameterBool> (
                juce::ParameterID { "air2low", 1 }, "Natural Air Low Cleanup", true));
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
    layout.add (std::make_unique<juce::AudioParameterBool> (
                juce::ParameterID { "stereo", 1 }, "Stereo Input (Binaural)", false));
    layout.add (std::make_unique<P> (juce::ParameterID { "pitchfloor", 1 }, "Pitch Floor (Hz)",
                juce::NormalisableRange<float> (0.0f, 300.0f, 1.0f), 0.0f));
    layout.add (std::make_unique<P> (juce::ParameterID { "robotHz", 1 }, "Robot Pitch (Hz)",
                juce::NormalisableRange<float> (40.0f, 400.0f, 0.1f, 0.5f), 120.0f));
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
    pBreath2 = apvts.getRawParameterValue ("breath2");
    pAir     = apvts.getRawParameterValue ("air");
    pAirBand = apvts.getRawParameterValue ("airband");
    pAir2    = apvts.getRawParameterValue ("air2");
    pAirShine = apvts.getRawParameterValue ("airshine");
    pAir2Low  = apvts.getRawParameterValue ("air2low");
    pGci     = apvts.getRawParameterValue ("gci");
    pHiFreq  = apvts.getRawParameterValue ("hifreq");
    pHiPitch = apvts.getRawParameterValue ("hipitch");
    pHiFmt   = apvts.getRawParameterValue ("hiformant");
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
    pGate      = apvts.getRawParameterValue ("gate");
    pAsmrX     = apvts.getRawParameterValue ("asmrx");
    pAsmrY     = apvts.getRawParameterValue ("asmry");
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
    monoScratch.assign ((size_t) samplesPerBlock, 0.0f);
    scratchL.assign ((size_t) samplesPerBlock, 0.0f);
    scratchR.assign ((size_t) samplesPerBlock, 0.0f);

    capBuf.assign ((size_t) (sampleRate * 15.0), 0.0f);
    capLen = 0;  capTarget = 0;  capturing = false;
    prevBuf.assign ((size_t) (sampleRate * 60.0), 0.0f);
    prevLen = 0;  prevPos = -1;

    // reset the per-run smoothing states so a device restart (or host
    // transport re-prepare) never resumes from a stale mute/gate fade
    rmsSm = 0.0f;  loudSec = 0.0f;  muteSec = 0.0f;  muteGain = 1.0f;
    gateEnv = 0.0f;  gateGain = 1.0f;  panL = 1.0f;  panR = 1.0f;
    gainSm = juce::Decibels::decibelsToGain (pGain->load());

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

    PsolaEngine::Params p;
    p.pitchSemi     = pPitch->load();
    p.formantSemi   = pFormant->load();
    p.consonantSemi = pConsonant->load();
    p.pitchRange    = pRange->load() * 0.01f;   // % -> ratio
    p.pitchCenterHz = pCenter->load();
    p.breath        = pBreath2->load();      // spectral (noise-excited envelope)
    p.airPreserve   = pAir->load();          // mixed harmonic+noise split
    p.airFreqHz     = pAirBand->load();
    p.airV2         = pAir2->load() > 0.5f;  // band-adaptive comb (Beta)
    p.airShineDb    = pAirShine->load();     // top-band bypass gain (Beta)
    p.airLowClean   = pAir2Low->load() > 0.5f;
    p.gciSync       = pGci->load() > 0.5f;
    p.hiRangeHz     = pHiFreq->load();       // high-range guard (laughs)
    p.hiPitchAmt    = pHiPitch->load() * 0.01f;
    p.hiFormantAmt  = pHiFmt->load()   * 0.01f;
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

    // Pre FX chain (standalone external plugins, e.g. a de-noiser)
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
        }
        if (stereoMode)
            for (int i = 0; i < n; ++i) m[i] = 0.5f * (sL[i] + sR[i]);
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

    const int vp = vizPos.load (std::memory_order_relaxed);
    for (int i = 0; i < n; ++i)
        vizIn[(size_t) ((vp + i) & (kVizLen - 1))] = m[i];

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

    // Latency estimate = engine lookahead (changes with Low Latency Mode)
    // + enabled hosted FX plugins. Pitch/Formant/Voice Quality/Breath run
    // inside the same grain pipeline and add no delay of their own. The UI
    // reads uiLatencySamples; the host is only (re)notified after the value
    // has been stable for 0.5 s, because each setLatencySamples() call can
    // make a DAW rebuild its delay compensation (audible interruption).
    {
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
                if (stereoMode) { sL[i] *= muteGain; sR[i] *= muteGain; }
            }
    }

    // Output gain, smoothed per sample (no zipper noise when the slider
    // moves), baked into m / sL / sR so the visualizer and the Refine
    // capture below see the exact same level as the speakers
    {
        const float gT = juce::Decibels::decibelsToGain (pGain->load());
        for (int i = 0; i < n; ++i)
        {
            gainSm += 0.002f * (gT - gainSm);
            m[i] *= gainSm;
            if (stereoMode) { sL[i] *= gainSm; sR[i] *= gainSm; }
        }
    }

    // ASMR pseudo-position: constant-power pan (X) + distance attenuation,
    // normalised so the centre position is exactly unity (no effect)
    const float ax = juce::jlimit (-1.0f, 1.0f, pAsmrX->load());
    const float ay = juce::jlimit (-1.0f, 1.0f, pAsmrY->load());
    const float dist = std::min (1.0f, std::sqrt (ax * ax + ay * ay));
    const float dg = 1.0f - 0.6f * dist;
    const float panPhase = (ax + 1.0f) * 0.25f * juce::MathConstants<float>::pi;
    const float tL = dg * std::cos (panPhase) * juce::MathConstants<float>::sqrt2;
    const float tR = dg * std::sin (panPhase) * juce::MathConstants<float>::sqrt2;
    for (int c = 0; c < ch; ++c)
    {
        float* d = buffer.getWritePointer (c);
        const float t = ch == 1 ? dg : (c == 0 ? tL : tR);
        float& sm = c == 0 ? panL : panR;
        const float* src = stereoMode ? (c == 0 ? sL : sR) : m;
        for (int i = 0; i < n; ++i)
        {
            sm += 0.002f * (t - sm);
            d[i] = sm * src[i];
        }
    }

    // Post FX chain (standalone external plugins) on the converted output;
    // the target-file preview below stays unfiltered
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

    for (int i = 0; i < n; ++i)
        vizOut[(size_t) ((vp + i) & (kVizLen - 1))] = m[i];
    vizPos.store (vp + n, std::memory_order_release);

    if (capturing.load() && capFromOutput.load())     // ANALYZE: capture converted
    {                                                 // output (before file preview)
        const int cl   = capLen.load();
        const int room = std::min ((int) capBuf.size(), capTarget.load()) - cl;
        const int c    = std::max (0, std::min (n, room));
        for (int i = 0; i < c; ++i) capBuf[(size_t) (cl + i)] = m[i];
        capLen.store (cl + c);
        if (c >= room) capturing.store (false);
    }

    const int pp = prevPos.load();     // ANALYZE tab: target-file preview
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
}

void VoxMorphProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    if (auto xml = apvts.copyState().createXml())
    {
        xml->setAttribute ("lockedIds", lockedIds.joinIntoString (","));   // 🔒 params
        copyXmlToBinary (*xml, dest);
    }
}

void VoxMorphProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
    {
        lockedIds = juce::StringArray::fromTokens (xml->getStringAttribute ("lockedIds"), ",", "");
        lockedIds.removeEmptyStrings();
        apvts.replaceState (juce::ValueTree::fromXml (*xml));
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new VoxMorphProcessor();
}
