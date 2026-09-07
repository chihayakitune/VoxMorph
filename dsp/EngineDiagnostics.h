#pragma once

// VoxMorph -- Engine Diagnostics, stage 1: detection and safe recovery.
//
// Dependency-free single-header C++17, same convention as PsolaEngine.h /
// ProtectionGain.h / SpatialEngine.h. Nothing here allocates, locks, writes a
// file or runs an analysis after prepare(): every buffer is a fixed array.
//
// WHAT THIS IS
// A watchdog around the conversion. It watches four points that fail for
// different reasons and must not be confused with one another:
//
//   input      what the host handed us, before anything
//   preEngine  what is about to enter the conversion (past gate + protection)
//   postEngine what the conversion produced -- THE point that matters
//   output     what actually leaves the plugin, past every later stage
//
// A NaN at postEngine but not at preEngine is the engine's own state going
// bad. A NaN at both is something upstream. A NaN only at output is one of
// the stages after the conversion. Collapsing these into one "something is
// wrong" flag would throw away the only information that says where to look.
//
// WHAT IT DOES ABOUT IT
// Only non-finite samples gate the audio. Clipping, an output stall and a
// processing-time overrun are RECORDED and raise the state to `caution`, but
// they never silence anything: a false positive that mutes a working plugin
// is worse than the symptom it was guarding against, and whisper, fry and
// deliberate effects all look like a stall from the outside.
//
// On a non-finite sample the block is zeroed, the engine is asked for a full
// reset, and the output is ramped back in. It never falls back to passing the
// dry input through: silence is an obvious failure, while unconverted voice
// going out to a stream sounds like it is working and is not.
//
// WHY THE RECOVERY IS RATIONED
// If the engine comes back bad, resetting it forever produces a stutter of
// silence-then-noise that is worse than staying quiet. After kMaxRetries the
// state latches at `halted`: the output is held muted until a person clears
// it. That is the honest end state -- something is broken that this class
// cannot fix by restarting it.
//
// THE ONE THING THAT IS EASY TO GET WRONG
// The engine delays its output by D samples, and ProtectionGain holds a
// D-sample history of the gain it applied. Resetting the engine without
// resetting that history leaves the restore multiplying converted samples by
// the inverse of a gain that was applied to material the engine has just
// thrown away -- so the caller MUST reset both together. beginRecovery()
// exists to say so at the call site rather than in a comment.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

class EngineDiagnostics
{
public:
    //==============================================================================
    enum class Stage : uint8_t { input = 0, preEngine = 1, postEngine = 2, output = 3, count = 4 };

    enum class State : uint8_t
    {
        normal = 0,          // nothing observed
        caution,             // clip / stall / overrun seen; audio NOT gated
        protectiveStop,      // non-finite found: output silenced, reset wanted
        awaitingRecovery,    // engine reset done, pipeline refilling, ramping in
        halted               // retries spent; muted until a person clears it
    };

    enum class Kind : uint8_t { nonFinite = 0, clip, stall, overrun, recovery, halt, manualClear };

    struct Event
    {
        uint32_t block = 0;      // block index since prepare()
        Stage    stage = Stage::input;
        Kind     kind  = Kind::nonFinite;
        float    value = 0.0f;   // kind-dependent: count, peak, ratio
    };

    // Fixed capacity. The audio thread never waits for a reader: once full it
    // drops the newest event and counts the drop, so a storm of faults cannot
    // turn into unbounded work or a stall on the callback.
    static constexpr int kEventCap = 64;

    // Tuning.
    static constexpr float kClipCeil      = 1.0f;    // |x| above this counts as clipped
    static constexpr float kLiveInputRms  = 1.0e-4f; // input counts as "live" above this
    static constexpr float kSilentOutRms  = 1.0e-6f; // engine counts as silent below this
    static constexpr float kStallSec      = 0.5f;    // live in, silent out, this long
    static constexpr float kSettleSec     = 0.10f;   // hold silence while the pipeline refills
    static constexpr float kRampSec       = 0.03f;   // fade the output back in over this
    static constexpr float kCleanSec      = 2.0f;    // clean for this long -> retries forgiven
    static constexpr int   kMaxRetries    = 3;

    //==============================================================================
    void prepare (double sampleRate, int /*maxBlockSamples*/)
    {
        fs = sampleRate > 0.0 ? sampleRate : 44100.0;
        reset();
    }

    void reset()
    {
        state.store (State::normal, std::memory_order_relaxed);
        gain = 1.0f;  settleLeft = 0.0f;  rampLeft = 0.0f;
        retries = 0;  cleanSec = 0.0f;  stallSec = 0.0f;
        blockIdx = 0; resetWanted = false;
        for (int i = 0; i < (int) Stage::count; ++i)
        {
            nonFiniteCount[i].store (0, std::memory_order_relaxed);
            clipCount[i].store (0, std::memory_order_relaxed);
            peak[i].store (0.0f, std::memory_order_relaxed);
        }
        evWrite.store (0, std::memory_order_relaxed);
        evDropped.store (0, std::memory_order_relaxed);
        resetCount.store (0, std::memory_order_relaxed);
        lastOverrun.store (0.0f, std::memory_order_relaxed);
    }

    //==============================================================================
    // Per-block context. Every one of these is a reason a silent engine is
    // CORRECT rather than broken, so the stall detector is told about them
    // instead of guessing: nothing to convert, the user asked for silence,
    // the gate is shut, Mix is at 0, or the lookahead has not filled yet.
    struct Context
    {
        bool muted       = false;   // user mute, auto-mute, or monitor mute
        bool gateOpen    = true;    // noise gate is passing
        bool converting  = true;    // Mix > 0, i.e. the engine output is in use
        bool warmedUp    = true;    // more than the engine lookahead has been fed
    };

    void beginBlock (int n, const Context& c)
    {
        ctx = c;
        blockSec = (float) n / (float) fs;
        ++blockIdx;
        blockHadNonFinite = false;
    }

    //==============================================================================
    // Scan one observation point. Returns true if a non-finite sample was
    // found, which is the only condition that gates audio.
    //
    // chans may be 1 (mono) or 2 (Stereo Input): both sides are always
    // scanned, because a fault on one engine only is exactly the case a
    // mono-only check would miss.
    bool observe (Stage s, const float* const* chans, int numChans, int n)
    {
        if (chans == nullptr || numChans <= 0 || n <= 0) return false;

        const int si = (int) s;
        int bad = 0, clipped = 0;
        float pk = 0.0f;
        double sumSq = 0.0;

        for (int c = 0; c < numChans; ++c)
        {
            const float* x = chans[c];
            if (x == nullptr) continue;
            for (int i = 0; i < n; ++i)
            {
                const float v = x[i];
                if (! std::isfinite (v)) { ++bad; continue; }
                const float a = std::abs (v);
                if (a > pk) pk = a;
                if (a > kClipCeil) ++clipped;
                sumSq += (double) v * v;
            }
        }

        peak[si].store (pk, std::memory_order_relaxed);
        lastRms[si] = (float) std::sqrt (sumSq / std::max (1, n * numChans));

        if (clipped > 0)
        {
            clipCount[si].fetch_add ((uint32_t) clipped, std::memory_order_relaxed);
            push ({ blockIdx, s, Kind::clip, (float) clipped });
            raiseCaution();
        }

        if (bad > 0)
        {
            nonFiniteCount[si].fetch_add ((uint32_t) bad, std::memory_order_relaxed);
            push ({ blockIdx, s, Kind::nonFinite, (float) bad });
            blockHadNonFinite = true;
            enterProtectiveStop();
            return true;
        }
        return false;
    }

    // Output stall: live input, converting, warmed up, not muted, gate open --
    // and still nothing coming out of the engine. Observation only; it raises
    // `caution` and logs, and never touches a sample. Call after observing
    // preEngine and postEngine.
    void checkStall()
    {
        const bool couldSound = ctx.converting && ctx.warmedUp && ! ctx.muted && ctx.gateOpen
                             && lastRms[(int) Stage::preEngine] > kLiveInputRms;
        if (! couldSound || gate() < 1.0f)
        {
            stallSec = 0.0f;
            return;
        }
        if (lastRms[(int) Stage::postEngine] < kSilentOutRms)
        {
            stallSec += blockSec;
            if (stallSec >= kStallSec)
            {
                push ({ blockIdx, Stage::postEngine, Kind::stall, stallSec });
                raiseCaution();
                stallSec = 0.0f;            // re-arm rather than log every block
            }
        }
        else
            stallSec = 0.0f;
    }

    // Observed processing time as a fraction of the block's own duration.
    // This is NOT a dropout: the host may have plenty of slack, and a real
    // dropout is something only the host can report. Recorded so a person can
    // see the trend, never acted on.
    void endBlock (double elapsedSec)
    {
        const float ratio = blockSec > 0.0f ? (float) (elapsedSec / (double) blockSec) : 0.0f;
        lastOverrun.store (ratio, std::memory_order_relaxed);
        if (ratio > 1.0f)
        {
            push ({ blockIdx, Stage::output, Kind::overrun, ratio });
            raiseCaution();
        }

        // Retries are forgiven only after a genuinely quiet stretch, so a
        // fault every few seconds still walks up to `halted` instead of
        // resetting the counter each time it recovers.
        if (! blockHadNonFinite && state.load (std::memory_order_relaxed) == State::normal)
        {
            cleanSec += blockSec;
            if (cleanSec >= kCleanSec) { retries = 0; cleanSec = 0.0f; }
        }
        else
            cleanSec = 0.0f;
    }

    //==============================================================================
    // Recovery handshake with the owner of the engine.
    //
    // The caller must reset the engine AND ProtectionGain's delayed-gain
    // history in the same breath: the restore undoes the gain from D samples
    // ago, and after an engine reset those samples no longer exist.
    bool wantsEngineReset() const noexcept { return resetWanted; }

    void beginRecovery() noexcept
    {
        resetWanted = false;
        resetCount.fetch_add (1, std::memory_order_relaxed);
        if (state.load (std::memory_order_relaxed) == State::halted) return;
        state.store (State::awaitingRecovery, std::memory_order_relaxed);
        settleLeft = kSettleSec;
        rampLeft   = kRampSec;
        push ({ blockIdx, Stage::postEngine, Kind::recovery, (float) retries });
    }

    // The output gain for this block, as a ramp rather than one value.
    //
    // A single value per block is NOT enough, and the test measures why: at
    // 256 samples the 30 ms fade spans about six blocks, so a per-block step
    // reaches 0.27 -- a discontinuity at the buffer boundary, which is exactly
    // the click this fade exists to avoid. The caller interpolates from `from`
    // to `to` across the block, so the fade is per sample and the block size
    // cannot be heard in it.
    //
    // In normal use both ends are exactly 1.0 and `isUnity()` lets the caller
    // skip the multiply entirely, leaving the audio path sample-identical.
    struct GainRamp
    {
        float from = 1.0f, to = 1.0f;
        bool isUnity() const noexcept { return from == 1.0f && to == 1.0f; }
    };

    GainRamp advanceGain()
    {
        const float prev = gain;
        const State s = state.load (std::memory_order_relaxed);

        if (s == State::protectiveStop || s == State::halted)
            gain = 0.0f;
        else if (s == State::awaitingRecovery)
        {
            if (settleLeft > 0.0f) { settleLeft -= blockSec; gain = 0.0f; }
            else
            {
                rampLeft = std::max (0.0f, rampLeft - blockSec);
                const float t = 1.0f - rampLeft / kRampSec;         // 0 -> 1
                gain = 0.5f - 0.5f * std::cos (3.14159265f * std::clamp (t, 0.0f, 1.0f));
                // Finish the state on the SAME block the gain reaches unity,
                // not one block later: otherwise the plugin reports itself as
                // still recovering while it is already at full level.
                if (rampLeft <= 0.0f)
                {
                    gain = 1.0f;
                    state.store (State::normal, std::memory_order_relaxed);
                }
            }
        }
        else
            gain = 1.0f;

        return { prev, gain };
    }

    float gate() const noexcept { return gain; }

    // From the UI (message thread) to leave `halted`. Deliberately manual:
    // reaching halted means restarting the engine did not help.
    void requestManualRecovery() noexcept
    {
        if (state.load (std::memory_order_relaxed) != State::halted) return;
        retries = 0;  cleanSec = 0.0f;
        state.store (State::protectiveStop, std::memory_order_relaxed);
        resetWanted = true;
        push ({ blockIdx, Stage::postEngine, Kind::manualClear, 0.0f });
    }

    //==============================================================================
    // Readouts (message thread).
    State    currentState()   const noexcept { return state.load (std::memory_order_relaxed); }
    uint32_t nonFinite (Stage s) const noexcept { return nonFiniteCount[(int) s].load (std::memory_order_relaxed); }
    uint32_t clips     (Stage s) const noexcept { return clipCount[(int) s].load (std::memory_order_relaxed); }
    float    stagePeak (Stage s) const noexcept { return peak[(int) s].load (std::memory_order_relaxed); }
    float    lastOverrunRatio()  const noexcept { return lastOverrun.load (std::memory_order_relaxed); }
    uint32_t engineResets()      const noexcept { return resetCount.load (std::memory_order_relaxed); }
    uint32_t droppedEvents()     const noexcept { return evDropped.load (std::memory_order_relaxed); }
    int      retryCount()        const noexcept { return retries; }

    // Snapshot of the event ring, newest last. Returns how many were written.
    int readEvents (Event* dst, int cap) const
    {
        if (dst == nullptr || cap <= 0) return 0;
        const uint32_t w = evWrite.load (std::memory_order_acquire);
        const int have = (int) std::min<uint32_t> (w, (uint32_t) kEventCap);
        const int take = std::min (have, cap);
        for (int i = 0; i < take; ++i)
            dst[i] = events[(size_t) ((w - (uint32_t) (take - i)) % (uint32_t) kEventCap)];
        return take;
    }

private:
    void raiseCaution()
    {
        State expected = State::normal;
        state.compare_exchange_strong (expected, State::caution, std::memory_order_relaxed);
    }

    void enterProtectiveStop()
    {
        const State s = state.load (std::memory_order_relaxed);
        if (s == State::halted) return;

        if (++retries > kMaxRetries)
        {
            state.store (State::halted, std::memory_order_relaxed);
            resetWanted = false;
            push ({ blockIdx, Stage::postEngine, Kind::halt, (float) retries });
            return;
        }
        state.store (State::protectiveStop, std::memory_order_relaxed);
        resetWanted = true;
    }

    void push (const Event& e)
    {
        const uint32_t w = evWrite.load (std::memory_order_relaxed);
        // Past kEventCap the oldest entry is overwritten. Keeping the newest
        // is the right trade for a watchdog -- what is happening now matters
        // more than what happened first -- but the number that scrolled off
        // has to survive, or the log quietly understates how bad it got.
        if (w >= (uint32_t) kEventCap)
            evDropped.fetch_add (1, std::memory_order_relaxed);
        events[(size_t) (w % (uint32_t) kEventCap)] = e;
        evWrite.store (w + 1, std::memory_order_release);
    }

    double fs = 44100.0;
    Context ctx {};
    float blockSec = 0.0f;
    bool  blockHadNonFinite = false;

    std::atomic<State> state { State::normal };
    float gain = 1.0f, settleLeft = 0.0f, rampLeft = 0.0f;
    int   retries = 0;
    float cleanSec = 0.0f, stallSec = 0.0f;
    bool  resetWanted = false;
    uint32_t blockIdx = 0;

    float lastRms[(int) Stage::count] {};
    std::atomic<uint32_t> nonFiniteCount[(int) Stage::count] {};
    std::atomic<uint32_t> clipCount[(int) Stage::count] {};
    std::atomic<float>    peak[(int) Stage::count] {};

    Event events[kEventCap] {};
    std::atomic<uint32_t> evWrite { 0 }, evDropped { 0 }, resetCount { 0 };
    std::atomic<float>    lastOverrun { 0.0f };
};
