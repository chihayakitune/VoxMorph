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
//   postEngine what the conversion produced
//   output     what actually leaves the plugin, past every later stage
//
// WHERE THE FAULT IS DECIDES WHAT CAN FIX IT
// This is the whole reason the four points are separate, and getting it wrong
// was the main defect in the first version of this file:
//
//   upstream   non-finite already at input or preEngine. The engine has not
//              been contaminated -- provided we SANITIZE before it gets in,
//              which observe() now asks the caller to do. Resetting the
//              engine here would be treating it for a fault it never saw.
//   engine     clean going in, non-finite coming out: the conversion's own
//              state has gone bad. A re-prepare is the fix.
//   downstream clean out of the engine, non-finite at the output: the spatial
//              stage, the hosted Post FX or the preview mix. Re-preparing the
//              ENGINE cannot fix this, so it does not ask for it; the
//              downstream stages are reset instead, and repeated failure halts.
//
// ONE FAULT IS ONE FAULT
// A single bad sample shows up at every observation point after the one that
// produced it. Counting each sighting as a separate failure spends the whole
// retry budget on one event -- the first version of this file halted the
// plugin outright on a single bad block (retries=4 from one NaN). So only the
// FIRST bad stage in a block sets the origin and may ask for a recovery; later
// stages in the same block are recorded as evidence and nothing more.
// `retries` is incremented in beginRecovery(), i.e. once per recovery actually
// performed, never per observation.
//
// WHAT GETS GATED
// Only non-finite samples. Clipping, an output stall and a processing-time
// overrun are recorded and raise `caution`, but never silence anything:
// whisper, fry and deliberate effects all look like a stall from outside, and
// muting a working plugin is worse than the symptom. The overrun is a ratio of
// the block's own duration and is deliberately NOT called a dropout -- only
// the host knows whether it actually missed a deadline.
//
// It never falls back to passing the dry input through: silence is an obvious
// failure, while unconverted voice going out to a stream sounds like it is
// working and is not.
//
// THREADING
// Everything that mutates state runs on the AUDIO thread. The UI only ever
// stores one atomic request flag (requestManualRecovery) and reads published
// atomics; the audio thread accepts the request at a block boundary. The event
// log is a true single-producer / single-consumer ring: the writer only
// touches slots the reader cannot reach and drops (counting) when full, so no
// slot is ever accessed by both threads at once.
//
// THE ONE THING THAT IS EASY TO GET WRONG
// The engine delays its output by D samples, and ProtectionGain holds a
// D-sample history of the gain it applied. Resetting the engine without
// resetting that history leaves the restore multiplying converted samples by
// the inverse of a gain applied to material the engine has just thrown away --
// so the caller MUST reset both together. RecoveryPlan::resetEngine says so at
// the call site rather than in a comment.

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
        caution,             // clip / stall / overrun / sanitized input; audio NOT gated
        protectiveStop,      // non-finite downstream of the input: block silenced
        awaitingRecovery,    // reset done, pipeline refilling, ramping in
        halted               // retries spent; muted until a person clears it
    };

    // What the fault can possibly be fixed by. See the header note.
    enum class Origin : uint8_t { none = 0, upstream, engine, downstream };

    // What observe() is telling the caller to DO about this stage.
    enum class Action : uint8_t
    {
        none = 0,
        sanitize,        // replace the non-finite samples here, before they travel
        silenceBlock     // zero the whole block: something past the input broke
    };

    // What the caller must reset, if anything, before the next block.
    enum class RecoveryPlan : uint8_t
    {
        none = 0,
        resetEngine,     // engine(s) AND ProtectionGain's gain history, together
        resetDownstream  // the stages after the conversion (spatial, FX state)
    };

    enum class Kind : uint8_t
    {
        nonFinite = 0, clip, stall, overrun,
        sanitized,          // upstream non-finite replaced; the engine was protected
        recovery, halt, manualClear
    };

    struct Event
    {
        uint32_t block  = 0;
        Stage    stage  = Stage::input;
        Kind     kind   = Kind::nonFinite;
        Origin   origin = Origin::none;
        float    value  = 0.0f;   // kind-dependent: count, seconds, ratio
    };

    static constexpr int kEventCap = 64;

    static constexpr float kClipCeil      = 1.0f;
    static constexpr float kLiveInputRms  = 1.0e-4f;
    static constexpr float kSilentOutRms  = 1.0e-6f;
    static constexpr float kStallSec      = 0.5f;
    static constexpr float kSettleSec     = 0.10f;
    static constexpr float kRampSec       = 0.03f;
    static constexpr float kCleanSec      = 2.0f;
    static constexpr int   kMaxRetries    = 3;

    //==============================================================================
    void prepare (double sampleRate, int /*maxBlockSamples*/)
    {
        fs = sampleRate > 0.0 ? sampleRate : 44100.0;
        reset();
    }

    // Clear the running state.
    //
    // THE LOG INDICES ARE NOT TOUCHED, and that is deliberate. Rewinding head
    // and tail to 0 was a real race: a reader that had already loaded the old
    // tail would go on copying slots the writer was free to reuse, and a
    // generation number compared AFTER the copy cannot un-read memory that was
    // being rewritten while it was read. The indices are monotonic for the
    // life of the object, so the writer's range [head, tail + cap) and the
    // reader's [tail, head) can never overlap, whatever else is reset.
    //
    // A reset is therefore an EVENT in the log rather than an erasure of it,
    // which is also the more useful record: "everything before here belongs to
    // a previous run" is information, and throwing the entries away is not.
    //
    // Concurrency: this touches audio-thread-only scalars, so it may be called
    // only when the audio thread is not running (JUCE guarantees that for
    // prepareToPlay). The runtime path -- AudioProcessor::reset() -- goes
    // through requestReset() instead, which is serviced on the audio thread.
    void reset()
    {
        state.store (State::normal, std::memory_order_relaxed);
        gain = 1.0f;  settleLeft = 0.0f;  rampLeft = 0.0f;
        retries = 0;  cleanSec = 0.0f;  stallSec = 0.0f;
        blockIdx = 0;
        plan = RecoveryPlan::none;
        blockOrigin = Origin::none;
        blockFaulted = false;
        blockRecoveryAsked = false;
        for (int i = 0; i < (int) Stage::count; ++i)
        {
            nonFiniteCount[i].store (0, std::memory_order_relaxed);
            clipCount[i].store (0, std::memory_order_relaxed);
            peak[i].store (0.0f, std::memory_order_relaxed);
            lastRms[i] = 0.0f;
        }
        // evHead / evTail are deliberately left alone -- see the note above.
        evDropped.store (0, std::memory_order_relaxed);
        resetCount.store (0, std::memory_order_relaxed);
        lastOverrun.store (0.0f, std::memory_order_relaxed);
        pubRetries.store (0, std::memory_order_relaxed);
        pubOrigin.store (Origin::none, std::memory_order_relaxed);
        haltOrigin = Origin::none;
        manualReq.store (false, std::memory_order_relaxed);
        resetReq.store (false, std::memory_order_relaxed);
    }

    // Safe from ANY thread. The clear itself happens on the audio thread at
    // the next block boundary, for the same reason the manual-recovery request
    // does: everything it touches is audio-thread state.
    void requestReset() noexcept { resetReq.store (true, std::memory_order_release); }

    //==============================================================================
    // Per-block context. Every field is a reason a silent engine is CORRECT
    // rather than broken, so the stall check is told instead of guessing.
    struct Context
    {
        bool muted       = false;
        bool gateOpen    = true;
        bool converting  = true;
        bool warmedUp    = true;
    };

    // AUDIO THREAD. Opens the block and accepts any pending UI request.
    void beginBlock (int n, const Context& c)
    {
        ctx = c;
        blockSec = (float) n / (float) fs;
        ++blockIdx;
        blockOrigin  = Origin::none;
        blockFaulted = false;
        blockRecoveryAsked = false;

        // The UI's only write is the atomic flag; everything it used to touch
        // directly is mutated here, on the audio thread, at a block boundary.
        if (resetReq.exchange (false, std::memory_order_acquire))
            reset();

        if (manualReq.exchange (false, std::memory_order_acquire))
        {
            retries  = 0;
            cleanSec = 0.0f;
            pubRetries.store (0, std::memory_order_relaxed);
            if (state.load (std::memory_order_relaxed) == State::halted)
            {
                // Back through the normal recovery path, so the output still
                // fades in rather than snapping to full level -- and with the
                // plan the HALTING CAUSE calls for. Always asking for
                // resetEngine here would re-prepare the conversion to clear a
                // fault that came from a stage after it, which is exactly the
                // misdirection the cause-specific recovery exists to avoid.
                state.store (State::awaitingRecovery, std::memory_order_relaxed);
                settleLeft = kSettleSec;
                rampLeft   = kRampSec;
                plan = planFor (haltOrigin);
            }
            push ({ blockIdx, Stage::postEngine, Kind::manualClear, haltOrigin, 0.0f });
        }
    }

    //==============================================================================
    // AUDIO THREAD. Scan one observation point and say what to do about it.
    //
    // Both channels are always scanned in Stereo Input mode: a fault on one
    // engine only is exactly what a mono-only check would miss.
    Action observe (Stage s, const float* const* chans, int numChans, int n)
    {
        if (chans == nullptr || numChans <= 0 || n <= 0) return Action::none;

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
            push ({ blockIdx, s, Kind::clip, Origin::none, (float) clipped });
            raiseCaution();
        }

        if (bad == 0) return Action::none;

        // Evidence is recorded at every stage it is seen; only the FIRST bad
        // stage in this block decides what happened and may cost a retry.
        nonFiniteCount[si].fetch_add ((uint32_t) bad, std::memory_order_relaxed);

        blockFaulted = true;

        // Upstream: the host or the Pre FX handed us rubbish. Replaced here, so
        // the engine never sees it -- that is a repair, not an engine failure,
        // and it costs no retry and triggers no reset.
        //
        // Crucially it also does NOT count as "this block already faulted" for
        // what follows. A fault that was CONTAINED cannot be the thing a later
        // stage is reporting, so if the engine goes non-finite after we handed
        // it clean samples, that is an independent failure and must still be
        // able to ask for its recovery. Treating the two as one event let a
        // noisy host mask a genuinely broken engine.
        if (s == Stage::input || s == Stage::preEngine)
        {
            push ({ blockIdx, s, Kind::sanitized, Origin::upstream, (float) bad });
            if (! blockRecoveryAsked)
                pubOrigin.store (Origin::upstream, std::memory_order_relaxed);
            raiseCaution();
            return Action::sanitize;
        }

        // Past the input the fault was not contained. The FIRST uncontained
        // one in the block decides the origin and may ask for a recovery; a
        // later stage seeing the same samples travel is evidence only, so the
        // budget is still spent once per block.
        const bool independent = ! blockRecoveryAsked;
        if (independent)
        {
            blockRecoveryAsked = true;
            blockOrigin = originOf (s);
            pubOrigin.store (blockOrigin, std::memory_order_relaxed);
        }

        push ({ blockIdx, s, Kind::nonFinite, independent ? blockOrigin : Origin::none,
                (float) bad });
        if (independent)
            enterProtectiveStop (blockOrigin);
        return Action::silenceBlock;
    }

    // AUDIO THREAD. Observation only; never touches a sample.
    void checkStall()
    {
        const bool couldSound = ctx.converting && ctx.warmedUp && ! ctx.muted && ctx.gateOpen
                             && lastRms[(int) Stage::preEngine] > kLiveInputRms;
        if (! couldSound || gain < 1.0f)
        {
            stallSec = 0.0f;
            return;
        }
        if (lastRms[(int) Stage::postEngine] < kSilentOutRms)
        {
            stallSec += blockSec;
            if (stallSec >= kStallSec)
            {
                push ({ blockIdx, Stage::postEngine, Kind::stall, Origin::none, stallSec });
                raiseCaution();
                stallSec = 0.0f;
            }
        }
        else
            stallSec = 0.0f;
    }

    // AUDIO THREAD. Observed processing time as a fraction of the block's own
    // duration. NOT a dropout; recorded, never acted on.
    void endBlock (double elapsedSec)
    {
        const float ratio = blockSec > 0.0f ? (float) (elapsedSec / (double) blockSec) : 0.0f;
        lastOverrun.store (ratio, std::memory_order_relaxed);
        if (ratio > 1.0f)
        {
            push ({ blockIdx, Stage::output, Kind::overrun, Origin::none, ratio });
            raiseCaution();
        }

        // Retries are forgiven only after a genuinely quiet stretch, so a fault
        // every few seconds still walks up to `halted`.
        if (! blockFaulted && state.load (std::memory_order_relaxed) == State::normal)
        {
            cleanSec += blockSec;
            if (cleanSec >= kCleanSec)
            {
                retries = 0;  cleanSec = 0.0f;
                pubRetries.store (0, std::memory_order_relaxed);
            }
        }
        else if (blockFaulted)
            cleanSec = 0.0f;
    }

    //==============================================================================
    // Recovery handshake. AUDIO THREAD.
    //
    // resetEngine means engine(s) AND ProtectionGain together: the restore
    // undoes the gain from D samples ago, and after an engine reset those
    // samples are gone.
    // resetDownstream means the stages AFTER the conversion. Re-preparing the
    // engine cannot fix a fault introduced past it, so it is not asked for.
    RecoveryPlan pendingRecovery() const noexcept { return plan; }

    void beginRecovery() noexcept
    {
        const RecoveryPlan p = plan;
        plan = RecoveryPlan::none;
        if (p == RecoveryPlan::none) return;

        resetCount.fetch_add (1, std::memory_order_relaxed);

        // ONE retry per recovery actually performed -- not per observation.
        ++retries;
        pubRetries.store ((uint32_t) retries, std::memory_order_relaxed);
        push ({ blockIdx, Stage::postEngine, Kind::recovery,
                pubOrigin.load (std::memory_order_relaxed), (float) retries });

        if (retries > kMaxRetries)
        {
            // Remember WHY we stopped, so a manual clear later asks for the
            // recovery this cause actually needs.
            haltOrigin = pubOrigin.load (std::memory_order_relaxed);
            state.store (State::halted, std::memory_order_relaxed);
            push ({ blockIdx, Stage::postEngine, Kind::halt, Origin::none, (float) retries });
            return;
        }

        state.store (State::awaitingRecovery, std::memory_order_relaxed);
        settleLeft = kSettleSec;
        rampLeft   = kRampSec;
    }

    // The output gain for this block, as a ramp rather than one value.
    //
    // A single value per block is NOT enough, and the test measures why: at
    // 256 samples the 30 ms fade spans about six blocks, so a per-block step
    // reaches 0.27 -- a discontinuity at the buffer boundary, which is exactly
    // the click this fade exists to avoid. The caller interpolates across the
    // block. In normal use both ends are exactly 1.0 and isUnity() lets the
    // caller skip the multiply, leaving the audio path sample-identical.
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
                const float t = 1.0f - rampLeft / kRampSec;
                gain = 0.5f - 0.5f * std::cos (3.14159265f * std::clamp (t, 0.0f, 1.0f));
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

    //==============================================================================
    // UI THREAD. The ONLY thing the UI writes. The audio thread picks it up at
    // the next block boundary; nothing here touches the state machine, the
    // retry counter or the log.
    void requestManualRecovery() noexcept
    {
        manualReq.store (true, std::memory_order_release);
    }

    //==============================================================================
    // Readouts. All published atomics -- safe from any thread.
    State    currentState()   const noexcept { return state.load (std::memory_order_relaxed); }
    Origin   lastOrigin()     const noexcept { return pubOrigin.load (std::memory_order_relaxed); }
    int      retryCount()     const noexcept { return (int) pubRetries.load (std::memory_order_relaxed); }
    uint32_t nonFinite (Stage s) const noexcept { return nonFiniteCount[(int) s].load (std::memory_order_relaxed); }
    uint32_t clips     (Stage s) const noexcept { return clipCount[(int) s].load (std::memory_order_relaxed); }
    float    stagePeak (Stage s) const noexcept { return peak[(int) s].load (std::memory_order_relaxed); }
    float    lastOverrunRatio()  const noexcept { return lastOverrun.load (std::memory_order_relaxed); }
    uint32_t engineResets()      const noexcept { return resetCount.load (std::memory_order_relaxed); }
    uint32_t droppedEvents()     const noexcept { return evDropped.load (std::memory_order_relaxed); }
    bool     manualRecoveryPending() const noexcept { return manualReq.load (std::memory_order_relaxed); }

    // CONSUMING read, single consumer. Slots handed back here can be reused by
    // the writer, which is what keeps the two threads off the same memory: the
    // writer only ever touches [head, tail + kEventCap), the reader only
    // [tail, head). When the ring is full the writer drops the NEWEST event and
    // counts it, rather than overwriting something the reader may be copying.
    int readEvents (Event* dst, int cap)
    {
        if (dst == nullptr || cap <= 0) return 0;

        // head only ever grows and tail is ours, so [tail, head) is a range no
        // writer can be inside. A reset cannot move either index, so there is
        // nothing here that a concurrent reset can invalidate -- which is why
        // this no longer needs a generation check to paper over one.
        const uint32_t h = evHead.load (std::memory_order_acquire);
        uint32_t t = evTail.load (std::memory_order_relaxed);
        int out = 0;
        while (t != h && out < cap)
        {
            dst[out++] = events[(size_t) (t % (uint32_t) kEventCap)];
            ++t;
        }
        evTail.store (t, std::memory_order_release);
        return out;
    }

private:
    static Origin originOf (Stage s) noexcept
    {
        switch (s)
        {
            case Stage::input:
            case Stage::preEngine:  return Origin::upstream;
            case Stage::postEngine: return Origin::engine;
            case Stage::output:     return Origin::downstream;
            default:                return Origin::none;
        }
    }

    void raiseCaution()
    {
        State expected = State::normal;
        state.compare_exchange_strong (expected, State::caution, std::memory_order_relaxed);
    }

    // Cause-specific: an engine re-prepare cannot repair a stage that runs
    // after it, so a downstream fault does not ask for one.
    static RecoveryPlan planFor (Origin o) noexcept
    {
        return o == Origin::downstream ? RecoveryPlan::resetDownstream
                                       : RecoveryPlan::resetEngine;
    }

    void enterProtectiveStop (Origin o)
    {
        if (state.load (std::memory_order_relaxed) == State::halted) return;
        state.store (State::protectiveStop, std::memory_order_relaxed);
        plan = planFor (o);
    }

    // AUDIO THREAD ONLY. Single producer.
    void push (const Event& e)
    {
        const uint32_t h = evHead.load (std::memory_order_relaxed);
        const uint32_t t = evTail.load (std::memory_order_acquire);
        if (h - t >= (uint32_t) kEventCap)
        {
            // Full. Dropping the newest keeps the writer off memory the reader
            // owns; the count is what stops the log quietly understating how
            // bad it got.
            evDropped.fetch_add (1, std::memory_order_relaxed);
            return;
        }
        events[(size_t) (h % (uint32_t) kEventCap)] = e;
        evHead.store (h + 1, std::memory_order_release);
    }

    double fs = 44100.0;
    Context ctx {};
    float blockSec = 0.0f;

    // audio-thread-only state
    float gain = 1.0f, settleLeft = 0.0f, rampLeft = 0.0f;
    int   retries = 0;
    float cleanSec = 0.0f, stallSec = 0.0f;
    uint32_t blockIdx = 0;
    RecoveryPlan plan = RecoveryPlan::none;
    Origin blockOrigin = Origin::none;
    bool   blockFaulted = false;        // anything non-finite seen this block
    bool   blockRecoveryAsked = false;  // an UNCONTAINED fault already claimed the budget
    float  lastRms[(int) Stage::count] {};

    // published
    std::atomic<State>    state { State::normal };
    std::atomic<Origin>   pubOrigin { Origin::none };
    std::atomic<uint32_t> pubRetries { 0 };
    std::atomic<uint32_t> nonFiniteCount[(int) Stage::count] {};
    std::atomic<uint32_t> clipCount[(int) Stage::count] {};
    std::atomic<float>    peak[(int) Stage::count] {};
    std::atomic<float>    lastOverrun { 0.0f };
    std::atomic<uint32_t> resetCount { 0 };

    // UI/host -> audio requests, serviced at a block boundary
    std::atomic<bool> manualReq { false };
    std::atomic<bool> resetReq  { false };
    Origin haltOrigin = Origin::none;

    // SPSC event log
    Event events[kEventCap] {};
    // Monotonic for the life of the object. reset() never rewinds them.
    std::atomic<uint32_t> evHead { 0 }, evTail { 0 }, evDropped { 0 };
};
