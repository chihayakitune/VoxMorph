#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "PsolaEngine.h"
#include "VoiceAnalyzer.h"
#include "SpatialEngine.h"

class VoxMorphProcessor : public juce::AudioProcessor
{
public:
    VoxMorphProcessor();
    ~VoxMorphProcessor() override;   // saves the FX chain setup (standalone)

    // -- AudioProcessor --
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;   // forwards to the hosted FX plugins
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

    // ---- Per-parameter locks + snapshot undo/redo (MESSAGE THREAD ONLY) --
    // The audio thread never reads any of this: locks are enforced purely on
    // the parameter-writing (UI) side and the history only stores/sets
    // parameter values, so the realtime path is unaffected.

    // Ids the user pinned with the row 🔒 buttons. Locked params are skipped
    // by preset load, Reset All and Analyze Auto-Set / Refine, and the UI
    // greys their controls out. Persisted in the plugin state, deliberately
    // NOT saved into preset files (locks are the user's intent).
    juce::StringArray lockedIds;
    bool isParamLocked (const juce::String& id) const { return lockedIds.contains (id); }
    void setParamLocked (const juce::String& id, bool on)
    {
        if (on) lockedIds.addIfNotAlreadyThere (id);
        else    lockedIds.removeString (id);
    }

    // Snapshot-based undo/redo for sound-changing operations. Manual knob
    // movements are coalesced by poll() (driven by the editor's timer) into
    // one step per burst once the values settle; grouped operations (preset
    // load, Reset All, Auto-Set, Refine) wrap their writes in group() so
    // ten parameter changes undo as a single step. Lock/unlock itself is
    // deliberately NOT undoable (spec). Host automation while the editor is
    // open also lands in the history; that is accepted for now.
    struct ParamHistory
    {
        void init (juce::AudioProcessor& p) { proc = &p; committed = snap(); }

        std::vector<float> snap() const
        {
            std::vector<float> v;
            for (auto* p : proc->getParameters())
                v.push_back (p->getValue());
            return v;
        }

        // editor timer (~3 Hz): commit a settled burst of manual edits
        void poll()
        {
            auto cur = snap();
            if (cur == committed) { pendingActive = false; return; }
            if (pendingActive && cur == pending)
            {
                push (undoStack, committed);
                committed = std::move (cur);
                redoStack.clear();
                pendingActive = false;
            }
            else { pending = std::move (cur); pendingActive = true; }
        }

        template <typename Fn>
        void group (Fn&& applyChanges)
        {
            commitPending();       // manual edits first, as their own step
            applyChanges();
            commitPending();       // then the whole group as ONE step
        }

        bool canUndo() const { return ! undoStack.empty(); }
        bool canRedo() const { return ! redoStack.empty(); }

        bool undo()
        {
            commitPending();
            if (undoStack.empty()) return false;
            redoStack.push_back (committed);
            committed = undoStack.back();  undoStack.pop_back();
            restore (committed);
            return true;
        }

        bool redo()
        {
            commitPending();
            if (redoStack.empty()) return false;
            undoStack.push_back (committed);
            committed = redoStack.back();  redoStack.pop_back();
            restore (committed);
            return true;
        }

    private:
        void commitPending()
        {
            auto cur = snap();
            if (cur != committed)
            {
                push (undoStack, committed);
                committed = std::move (cur);
                redoStack.clear();
            }
            pendingActive = false;
        }

        void restore (const std::vector<float>& s)
        {
            const auto& ps = proc->getParameters();
            for (int i = 0; i < ps.size() && i < (int) s.size(); ++i)
                if (ps[i]->getValue() != s[(size_t) i])
                {
                    ps[i]->beginChangeGesture();
                    ps[i]->setValueNotifyingHost (s[(size_t) i]);
                    ps[i]->endChangeGesture();
                }
        }

        static void push (std::vector<std::vector<float>>& st, std::vector<float> s)
        {
            st.push_back (std::move (s));
            if (st.size() > 64) st.erase (st.begin());   // history depth cap
        }

        juce::AudioProcessor* proc = nullptr;
        std::vector<float> committed, pending;
        std::vector<std::vector<float>> undoStack, redoStack;
        bool pendingActive = false;
    };
    ParamHistory history;

    // ---- Performance Mode (v0.31.0, parameter id "perfmode") --------------
    // A switch for running comfortably at small device buffers (64 / 128 /
    // 256 samples) WITHOUT giving up any conversion quality.
    //
    // What it must never do — and does not do anywhere in this codebase:
    // change the pitch detector, the formant path, Natural Air (including
    // its FFT cleanup), the minimum tracked f0, the analysis ranges, grain
    // width / placement / guard, the windows, the smoothing, or the engine
    // lookahead. PsolaEngine does not read this parameter at all; the audio
    // it produces is identical with the mode on and off.
    //
    // What it does: lowers the refresh rate of purely visual work (spectrum
    // analysis, vowel meter, level meters) so the message thread competes
    // less with the audio callback, and — standalone only — unlocks the device buffer
    // helper in Audio Settings. Everything else in this pass (the zero-value
    // DSP bypasses) is unconditional and applies in every mode, because a
    // bypass that produces identical samples has no reason to be optional.
    //
    // Mirrors the parameter for the UI timers (message thread writes it).
    std::atomic<bool> uiPerfMode { false };

    // Display-only tap gates, written by the editor (message thread) and read
    // by processBlock. Each tap is an extra pass over the block, so nothing
    // on screen means no work at all: the plugin with its window closed pays
    // nothing for meters or the visualizer. Set false whenever the editor
    // goes away, otherwise the audio thread would keep filling buffers that
    // nobody reads.
    std::atomic<bool> uiWantsMeters { false }, uiWantsViz { false };

    // STATUS row (UI): estimated internal latency in samples, updated on the
    // audio thread. uiLatencySamples = engine lookahead + enabled hosted FX;
    // uiFxLatSamples = the hosted-FX share of that (for the breakdown text).
    // Device/host buffers are added on the editor side (standalone only).
    std::atomic<int> uiLatencySamples { 0 }, uiFxLatSamples { 0 };

    // ---- MUTE / MONITOR (v0.30.0, driven by the standalone options bar) ----
    // muted      = the user pressed MUTE; the output is silenced so nothing
    //              reaches the stream / virtual cable.
    // monitoring = MONITOR is on, i.e. the app's OUTPUT DEVICE was temporarily
    //              switched to the monitor device chosen in Audio Settings.
    //              While monitoring, MUTE does NOT silence the signal — the
    //              point of monitoring is to hear yourself, and the normal
    //              output device is not receiving anything anyway. MUTE stays
    //              lit so that leaving monitoring drops you back to silence
    //              instead of accidentally going live.
    // Both are deliberately NOT parameters: presets and undo must never be
    // able to un-mute you behind your back.
    std::atomic<bool> muted      { false };
    std::atomic<bool> monitoring { false };

    // Monitor output device (message thread only). monitorDeviceName is the
    // device chosen in Audio Settings; preMonitorDeviceName remembers the
    // device that was active when monitoring started, so it can be restored.
    // The name is saved with the plugin state; the live monitoring state is not.
    juce::String monitorDeviceName, preMonitorDeviceName;

    // The picture in the character badge (message thread only). Empty = the
    // built-in art. Saved with the plugin state like monitorDeviceName, and
    // for the same reason it is a PATH, not the pixels: the state travels
    // between sessions, not between machines.
    juce::String characterImagePath;

    // AEIOU vowel readout (v0.30.2) for the vowel meter in the VISUALIZER.
    // These are the engine's OWN vowel coordinate — the same numbers the
    // AEIOU Character warp acts on — republished through atomics because the
    // engine's getters are not safe to call from the message thread.
    // uiVowelActive is false whenever the feature is off or at 0 %, in which
    // case the engine does not track vowels at all and the values are stale:
    // the UI must show an inactive state rather than draw them.
    std::atomic<float> uiVowelH { 0.5f }, uiVowelF { 0.5f }, uiVowelConf { 0.0f };
    std::atomic<bool>  uiVowelActive { false };

    // Detection readout (v0.47.0) for the VISUALIZER's detection lane, in Hz
    // on the same axis as the spectrum above it. Republished from the
    // engine's analysis taps for the same reason as the vowel coordinate:
    // the getters are plain floats written on the audio thread. 0 = nothing
    // detected (unvoiced or silent).
    //
    // uiFmtValid is false while the SPECTRAL LAYER is skipped, which it is
    // unless a formant feature is on -- the formant numbers are then stale
    // and the lane must say so rather than draw them. Do NOT "fix" this by
    // forcing the layer on: it is a per-grain FFT, and it also rewrites the
    // grain, so the plugin would cost more AND sound different purely
    // because a display was open.
    std::atomic<float> uiF0In { 0.0f }, uiF0Out { 0.0f };
    std::atomic<float> uiFmtIn[3]  { { 0.0f }, { 0.0f }, { 0.0f } };
    std::atomic<float> uiFmtOut[3] { { 0.0f }, { 0.0f }, { 0.0f } };
    std::atomic<bool>  uiFmtValid { false };

    // ASMR readout (v0.45.0): where the source ACTUALLY is, i.e. the pad
    // position after the auto-orbit has rotated it. The pad draws from here
    // rather than from asmrx/asmry, so a running orbit is visible; with the
    // orbit off these simply mirror the two parameters.
    std::atomic<float> uiSpaceX { 0.0f }, uiSpaceY { 0.0f };

    // ---- UI level meters (v0.30.0, per channel since v0.30.3) -------------
    // Written on the audio thread, read by the editor at ~30 Hz. Fast attack
    // / slow release on the RMS and a decaying peak hold, both driven by TIME
    // constants so the ballistics never depend on the host buffer size.
    struct LevelMeter
    {
        std::atomic<float> rms { 0.0f }, peak { 0.0f };

        void reset() { rms.store (0.0f); peak.store (0.0f); }

        void push (const float* d, int n, float dt)
        {
            if (d == nullptr || n <= 0) return;
            float pk = 0.0f;
            double sum = 0.0;
            for (int i = 0; i < n; ++i)
            {
                const float a = std::abs (d[i]);
                if (a > pk) pk = a;
                sum += (double) d[i] * d[i];
            }
            if (! std::isfinite (pk)) pk = 0.0f;
            push ((float) std::sqrt (sum / (double) n), pk, dt);
        }

        void push (float rmsNow, float peakNow, float dt)
        {
            float r = rms.load (std::memory_order_relaxed);
            r += (1.0f - std::exp (-dt / (rmsNow > r ? 0.02f : 0.25f))) * (rmsNow - r);
            rms.store (std::isfinite (r) ? r : 0.0f, std::memory_order_relaxed);

            float p = peak.load (std::memory_order_relaxed);
            p = peakNow > p ? peakNow : p * std::exp (-dt / 0.45f);
            peak.store (std::isfinite (p) ? p : 0.0f, std::memory_order_relaxed);
        }
    };

    // uiIn*  = the raw device/host input, measured BEFORE the noise gate and
    //          the Pre FX so the meter still shows your mic while gated.
    // uiOut* = what actually leaves the plugin: after mute, output gain, the
    //          ASMR position and the Post FX chain.
    // With a mono bus both channels of a pair carry the same signal.
    LevelMeter uiInL, uiInR, uiOutL, uiOutR;

    // Visualizer taps: mono input (pre-conversion) and output (as heard),
    // written on the audio thread, read by the editor's SpectrumData.
    //
    // The UI reads a 4096-sample window out of these rings while the audio
    // thread keeps writing, so the samples themselves are atomics accessed
    // with relaxed ordering: that makes the overlap a well-defined race-free
    // read (and keeps Thread Sanitizer quiet) without any lock on the audio
    // thread. A relaxed float load/store is a plain move on every platform we
    // build for -- the static_assert below is what keeps that true.
    //
    // vizPos is the running write position and doubles as the generation
    // counter: the reader takes it before and after copying a window and
    // drops the frame if the writer advanced far enough to have overwritten
    // any of it (see SpectrumData::timerCallback). It is unsigned so the wrap
    // after 2^32 samples (~25 h at 48 kHz) is defined and differences stay
    // exact across it.
    static constexpr int kVizLen = 16384;              // power of two
    static_assert (std::atomic<float>::is_always_lock_free,
                   "the visualizer taps must never lock on the audio thread");
    std::vector<std::atomic<float>> vizIn  = std::vector<std::atomic<float>> ((size_t) kVizLen);
    std::vector<std::atomic<float>> vizOut = std::vector<std::atomic<float>> ((size_t) kVizLen);
    std::atomic<unsigned>           vizPos { 0 };

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
    // v0.39.0: the same thing for MyVoice, so the MyVoiceFile menu can play
    // back what it just loaded. It has to be a SECOND buffer rather than a
    // reuse of prevBuf: the Target preview must survive a MyVoice load
    // untouched (spec 5.6), and "With target play" plays the target while
    // recording, so both can be armed in the same session. Only one is ever
    // started at a time -- the UI stops the other first.
    std::vector<float> myBuf;
    std::atomic<int>   myLen { 0 };
    std::atomic<int>   myPos { -1 };                   // -1 = stopped

    // latest measured profiles (message thread only; the PRESETS tab saves
    // these to .vmprofile files, ANALYZE can load such files as targets)
    VoiceProfile lastMyVoice, lastTarget;

    // External FX hosting (standalone): CHAINS of VST3s, processed in list
    // order. pre = on the mic input before the conversion, post = on the
    // converted output. Message-thread API; the audio thread takes fxLock
    // with a try-lock and skips the FX for one block while a chain is edited.
    struct FxSlot
    {
        std::unique_ptr<juce::AudioPluginInstance> plugin;
        std::atomic<bool> enabled { true };
        juce::String path;                       // .vst3 location, for persistence
    };
    juce::String addFx (bool post, const juce::File& vst3);   // "" = success
    void removeFx (bool post, int index);
    void setFxEnabled (bool post, int i, bool on);
    int  getNumFx (bool post) const { return (post ? postChain : preChain).size(); }
    FxSlot* getFxSlot (bool post, int i)
    {
        auto& c = post ? postChain : preChain;
        return juce::isPositiveAndBelow (i, c.size()) ? c[i] : nullptr;
    }
    // persistence: chains (paths + enabled + each plugin's internal state)
    // save to <userAppData>/VoxMorph/fxchains.xml on edits and on quit,
    // and reload automatically at startup (standalone only)
    void saveFxChains();
    void loadFxChains();

    // Total FX slots in both chains, republished (under fxLock) whenever the
    // message thread edits a chain. processBlock reads it to decide whether
    // taking the try-lock is worth it at all — with no slots, which is every
    // plugin build, the FX stages cost nothing.
    std::atomic<int> fxCount { 0 };

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    PsolaEngine engine;              // mono path / LEFT channel in stereo mode
    PsolaEngine engineR;             // RIGHT channel (stereo input mode only)
    // ASMR positioning, applied to the FINISHED output (see SpatialEngine.h).
    // It is downstream of everything above and of the visualizer tap, so it
    // cannot influence the conversion or what the graphs show.
    SpatialEngine spatial;
    std::vector<float> monoScratch, scratchL, scratchR;

    std::atomic<float>* pPitch     = nullptr;
    std::atomic<float>* pFormant   = nullptr;
    std::atomic<float>* pConsonant = nullptr;
    std::atomic<float>* pF1S = nullptr; std::atomic<float>* pF1G = nullptr;
    std::atomic<float>* pF2S = nullptr; std::atomic<float>* pF2G = nullptr;
    std::atomic<float>* pF3S = nullptr; std::atomic<float>* pF3G = nullptr;
    std::atomic<float>* pReso = nullptr;   // Formant Definition
    std::atomic<float>* pVAdapt  = nullptr;
    std::atomic<float>* pVAmount = nullptr;
    std::atomic<float>* pVChar   = nullptr;
    std::atomic<float>* pVCustom[5][3]  = {};   // [a,i,u,e,o][F1,F2,F3] shift
    std::atomic<float>* pVCustomG[5][3] = {};   // [a,i,u,e,o][F1,F2,F3] gain
    std::atomic<float>* pBreath2   = nullptr;
    std::atomic<float>* pAir       = nullptr;
    std::atomic<float>* pAirShine  = nullptr;
    std::atomic<float>* pGci       = nullptr;
    std::atomic<float>* pHiOn      = nullptr;   // High Range guard on/off
    std::atomic<float>* pLowOn     = nullptr;   // Low Limit on/off
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
    std::atomic<float>* pLowLat    = nullptr;   // Legacy Low Latency (Beta)
    std::atomic<float>* pPerf      = nullptr;   // Performance Mode

    // call with fxLock held: republish the slot total for the audio thread
    void publishFxCount() { fxCount.store (preChain.size() + postChain.size(),
                                           std::memory_order_relaxed); }

    // latency bookkeeping: last hosted-FX latency sum (audio thread) and the
    // debounce state for host PDC notifications (frequent setLatencySamples
    // calls can make hosts rebuild their graph = audible interruption)
    int   fxLatSamples  = 0;
    int   pendingLat    = -1;
    float pendingLatSec = 0.0f;

    // feedback-runaway protection state
    float rmsSm = 0.0f, loudSec = 0.0f, muteSec = 0.0f, muteGain = 1.0f;
    // noise gate + output gain smoothing state. The ASMR pan smoothers used
    // to live here too; they moved into SpatialEngine with the rest of the
    // positioning stage in v0.45.0.
    float gateEnv = 0.0f, gateGain = 1.0f;
    float gainSm = 1.0f;
    std::atomic<float>* pGate  = nullptr;
    std::atomic<float>* pAsmrX = nullptr;
    std::atomic<float>* pAsmrY = nullptr;
    std::atomic<float>* pAsmrBin   = nullptr;
    std::atomic<float>* pAsmrDist  = nullptr;
    std::atomic<float>* pAsmrAir   = nullptr;
    std::atomic<float>* pAsmrRoom  = nullptr;
    std::atomic<float>* pAsmrSize  = nullptr;
    std::atomic<float>* pAsmrWidth = nullptr;
    std::atomic<float>* pAsmrOrbit = nullptr;
    std::atomic<float>* pAsmrDepth = nullptr;
    std::atomic<float>* pStereo = nullptr;

    // external FX hosting state
    void applyFxMono (juce::AudioPluginInstance&, float* m, int n);
    juce::AudioPluginFormatManager fxFormats;
    juce::OwnedArray<FxSlot> preChain, postChain;
    juce::CriticalSection fxLock;
    juce::AudioBuffer<float> fxScratch;
    double fxSr = 48000.0;
    int    fxBlk = 512;
    bool   fxLoading = false;   // suppress saves while restoring at startup
    std::atomic<float>* pRobotHz   = nullptr;
    std::atomic<float>* pMix       = nullptr;
    std::atomic<float>* pGain      = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VoxMorphProcessor)
};
