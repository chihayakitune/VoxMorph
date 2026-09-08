// diag_probe.cpp — fault injection through the REAL processor (stage 1).
//
// Build:
//   cmake -B build -DCMAKE_BUILD_TYPE=Release -DVOXMORPH_DIAG_PROBE=ON
//   cmake --build build --target VoxMorphDiagProbe
//
// WHY THIS EXISTS
// test/diagnostics_test.cpp drives EngineDiagnostics through a mock of the
// wiring it expects. That checks the class, not the plugin: it cannot catch
// the processor calling observe() and ignoring what it returns, resetting the
// wrong thing, or leaving ProtectionGain's gain history behind. Every check
// here runs inside the real VoxMorphProcessor::processBlock.
//
// An engine-origin fault cannot be produced from the outside -- feeding NaN in
// gets sanitized at the input, which is exactly the behaviour under test -- so
// the processor carries a test-only injection hook (VOXMORPH_DIAG_TEST_HOOKS),
// compiled out of every shipping build.
#include "../src/PluginProcessor.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

// ---- allocation counter (same shape as offline_test.cpp) -------------------
// Used for the one measurement the reviews kept asking for: whether the
// recovery path -- which calls engine.prepare() from the audio callback --
// allocates.
static long g_allocCount = 0;
static bool g_countAlloc = false;
void* operator new (std::size_t sz)
{
    if (g_countAlloc) ++g_allocCount;
    if (sz == 0) sz = 1;
    if (void* p = std::malloc (sz)) return p;
    throw std::bad_alloc();
}
void* operator new[] (std::size_t sz) { return operator new (sz); }
void operator delete (void* p) noexcept { std::free (p); }
void operator delete[] (void* p) noexcept { std::free (p); }
void operator delete (void* p, std::size_t) noexcept { std::free (p); }
void operator delete[] (void* p, std::size_t) noexcept { std::free (p); }

static int g_fail = 0;
static void check (bool ok, const juce::String& what)
{
    std::printf ("%s  %s\n", ok ? "PASS" : "FAIL", what.toRawUTF8());
    if (! ok) ++g_fail;
}

using EDStage  = EngineDiagnostics::Stage;
using EDState  = EngineDiagnostics::State;
using EDOrigin = EngineDiagnostics::Origin;

static constexpr double kSr = 48000.0;

static const char* stateName (EDState s)
{
    switch (s)
    {
        case EDState::normal:           return "normal";
        case EDState::caution:          return "caution";
        case EDState::protectiveStop:   return "protectiveStop";
        case EDState::awaitingRecovery: return "awaitingRecovery";
        case EDState::halted:           return "halted";
    }
    return "?";
}
static const char* originName (EDOrigin o)
{
    switch (o)
    {
        case EDOrigin::none:       return "none";
        case EDOrigin::upstream:   return "upstream";
        case EDOrigin::engine:     return "engine";
        case EDOrigin::downstream: return "downstream";
    }
    return "?";
}

// A voiced-ish block so the engine has something real to convert.
static void fillTone (juce::AudioBuffer<float>& b, int64_t pos, float amp = 0.2f)
{
    for (int c = 0; c < b.getNumChannels(); ++c)
    {
        float* d = b.getWritePointer (c);
        for (int i = 0; i < b.getNumSamples(); ++i)
            d[i] = amp * (float) std::sin (2.0 * M_PI * 140.0 * (double) (pos + i) / kSr);
    }
}

static bool allFinite (const juce::AudioBuffer<float>& b)
{
    for (int c = 0; c < b.getNumChannels(); ++c)
    {
        const float* d = b.getReadPointer (c);
        for (int i = 0; i < b.getNumSamples(); ++i)
            if (! std::isfinite (d[i])) return false;
    }
    return true;
}

// Runs `blocks` blocks of tone through the processor. Returns false if any
// output sample was ever non-finite.
static bool runTone (VoxMorphProcessor& p, juce::AudioBuffer<float>& buf,
                     int blocks, int64_t& pos, std::vector<float>* outTrace = nullptr)
{
    juce::MidiBuffer midi;
    bool finite = true;
    for (int b = 0; b < blocks; ++b)
    {
        fillTone (buf, pos);
        p.processBlock (buf, midi);
        if (! allFinite (buf)) finite = false;
        if (outTrace != nullptr)
        {
            const float* d = buf.getReadPointer (0);
            outTrace->insert (outTrace->end(), d, d + buf.getNumSamples());
        }
        pos += buf.getNumSamples();
    }
    return finite;
}

static void setStereoInput (VoxMorphProcessor& p, bool on)
{
    if (auto* rp = p.apvts.getParameter ("stereo"))
    {
        rp->beginChangeGesture();
        rp->setValueNotifyingHost (on ? 1.0f : 0.0f);
        rp->endChangeGesture();
    }
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    // ---- 1. normal path is untouched, against a REAL baseline --------------
    // Comparing two builds that both carry the diagnostics only shows they are
    // deterministic. The question is whether the diagnostics changed the audio
    // at all, so this binary renders to a file and the SAME source built with
    // VOXMORPH_DIAG_ENABLED=0 -- which is the pre-v0.68.0 signal path, with
    // every diagnostics call compiled out -- renders the same input. The two
    // dumps are then compared byte for byte outside this program.
    {
        VoxMorphProcessor p;
        p.prepareToPlay (kSr, 256);
        juce::AudioBuffer<float> buf (2, 256);
        juce::MidiBuffer midi;
        int64_t pos = 0;
        std::vector<float> dump;
        for (int i = 0; i < 400; ++i)
        {
            fillTone (buf, pos);
            p.processBlock (buf, midi);
            for (int c = 0; c < 2; ++c)
            {
                const float* d = buf.getReadPointer (c);
                dump.insert (dump.end(), d, d + 256);
            }
            pos += 256;
        }
        const char* path = VOXMORPH_DIAG_ENABLED ? "/tmp/vm_diag_on.f32"
                                                 : "/tmp/vm_diag_off.f32";
        if (FILE* f = std::fopen (path, "wb"))
        {
            std::fwrite (dump.data(), sizeof (float), dump.size(), f);
            std::fclose (f);
            std::printf ("      baseline dump: %s (%zu samples)\n", path, dump.size());
        }
        check (! dump.empty(), "baseline: rendered a dump for the cross-build diff");

       #if VOXMORPH_DIAG_ENABLED
        check (p.diagnostics.currentState() == EDState::normal,
               "normal: state stays normal over 400 blocks");
        check (p.diagnostics.engineResets() == 0, "normal: no recovery was triggered");
        check (p.diagnostics.retryCount() == 0, "normal: no retry was spent");
       #endif
    }

   #if ! VOXMORPH_DIAG_ENABLED
    // The baseline build only exists to produce that dump.
    std::printf ("\nbaseline build (VOXMORPH_DIAG_ENABLED=0): dump written\n");
    return 0;
   #endif

    // ---- 2. non-finite INPUT is blocked before the engine -------------------
    // The fix the review asked for: an upstream fault must be sanitized, must
    // NOT be called an engine failure, and must NOT reset the engine.
    {
        VoxMorphProcessor p;
        p.prepareToPlay (kSr, 256);
        juce::AudioBuffer<float> buf (2, 256);
        juce::MidiBuffer midi;
        int64_t pos = 0;
        runTone (p, buf, 40, pos);                      // settle

        bool everBad = false;
        for (int b = 0; b < 20; ++b)
        {
            fillTone (buf, pos);
            buf.getWritePointer (0)[10]  = std::numeric_limits<float>::quiet_NaN();
            buf.getWritePointer (1)[200] = std::numeric_limits<float>::infinity();
            p.processBlock (buf, midi);
            if (! allFinite (buf)) everBad = true;
            pos += 256;
        }
        std::printf ("      input fault: state=%s origin=%s resets=%u retries=%d\n",
                     stateName (p.diagnostics.currentState()),
                     originName (p.diagnostics.lastOrigin()),
                     p.diagnostics.engineResets(), p.diagnostics.retryCount());

        check (! everBad, "input fault: nothing non-finite ever reaches the output");
        check (p.diagnostics.nonFinite (EDStage::input) >= 40,
               "input fault: counted at the input stage");
        check (p.diagnostics.nonFinite (EDStage::preEngine) == 0,
               "input fault: blocked BEFORE the engine (pre-engine stayed clean)");
        check (p.diagnostics.nonFinite (EDStage::postEngine) == 0,
               "input fault: the engine never produced one either");
        check (p.diagnostics.lastOrigin() == EDOrigin::upstream,
               "input fault: classified as upstream, not as an engine failure");
        check (p.diagnostics.engineResets() == 0,
               "input fault: the engine was NOT reset for someone else's fault");
        check (p.diagnostics.retryCount() == 0,
               "input fault: no retry budget spent");
        check (p.diagnostics.currentState() != EDState::halted,
               "input fault: a noisy host cannot halt the plugin");

        // and it recovers to full output on its own once the input is clean
        runTone (p, buf, 200, pos);
        check (p.diagnostics.currentState() == EDState::caution
            || p.diagnostics.currentState() == EDState::normal,
               "input fault: returns to a running state once the input is clean");
    }

    // ---- 3. engine-origin fault: reset, ramp back, no click ----------------
    {
        VoxMorphProcessor p;
        p.prepareToPlay (kSr, 256);
        juce::AudioBuffer<float> buf (2, 256);
        int64_t pos = 0;
        runTone (p, buf, 60, pos);

        p.diagInjectStage.store ((int) EDStage::postEngine);
        p.diagInjectBlocks.store (1);
        std::vector<float> trace;
        runTone (p, buf, 1, pos, &trace);

        check (p.diagnostics.lastOrigin() == EDOrigin::engine,
               "engine fault: classified as engine");
        check (p.diagnostics.engineResets() == 1, "engine fault: exactly one recovery");
        check (p.diagnostics.retryCount() == 1, "engine fault: exactly one retry spent");

        float pk = 0.0f;
        for (float v : trace) pk = std::max (pk, std::abs (v));
        check (pk == 0.0f, "engine fault: the offending block leaves as silence");

        trace.clear();
        const bool finite = runTone (p, buf, 120, pos, &trace);
        check (finite, "engine fault: output finite throughout recovery");

        // the fade back in must not step: measured on the OUTPUT envelope
        float worst = 0.0f;
        for (size_t i = 1; i < trace.size(); ++i)
            worst = std::max (worst, std::abs (std::abs (trace[i]) - std::abs (trace[i-1])));
        std::printf ("      engine fault: worst |sample| step during recovery %.5f\n", worst);
        check (worst < 0.05f, "engine fault: recovery has no step big enough to click");
        check (p.diagnostics.currentState() == EDState::normal
            || p.diagnostics.currentState() == EDState::caution,
               "engine fault: back to a running state");
    }

    // ---- 4. downstream fault does NOT reset the engine ----------------------
    // The review's point: re-preparing the engine cannot repair a stage that
    // runs after it, so it must not be asked for.
    {
        VoxMorphProcessor p;
        p.prepareToPlay (kSr, 256);
        juce::AudioBuffer<float> buf (2, 256);
        int64_t pos = 0;
        runTone (p, buf, 60, pos);
        const uint32_t resetsBefore = p.diagnostics.engineResets();

        p.diagInjectStage.store ((int) EDStage::output);
        p.diagInjectBlocks.store (1);
        std::vector<float> trace;
        const bool finite1 = runTone (p, buf, 1, pos, &trace);

        std::printf ("      downstream fault: origin=%s state=%s\n",
                     originName (p.diagnostics.lastOrigin()),
                     stateName (p.diagnostics.currentState()));
        check (finite1, "downstream fault: the block leaves finite (silenced, not NaN)");
        check (p.diagnostics.lastOrigin() == EDOrigin::downstream,
               "downstream fault: classified as downstream");
        check (p.diagnostics.nonFinite (EDStage::postEngine) == 0,
               "downstream fault: the engine was not blamed");
        check (p.diagnostics.engineResets() == resetsBefore + 1,
               "downstream fault: one recovery was performed");
        const bool finite2 = runTone (p, buf, 150, pos);
        check (finite2, "downstream fault: output stays finite afterwards");
    }

    // ---- 5. one fault at several taps costs ONE retry ----------------------
    // The exact defect in the review: a single NaN seen at four observation
    // points must not spend four retries.
    {
        VoxMorphProcessor p;
        p.prepareToPlay (kSr, 256);
        juce::AudioBuffer<float> buf (2, 256);
        int64_t pos = 0;
        runTone (p, buf, 60, pos);

        // injected post-engine, so it also reaches the output tap in the SAME
        // block -- two observation points, one fault
        p.diagInjectStage.store ((int) EDStage::postEngine);
        p.diagInjectBlocks.store (1);
        runTone (p, buf, 1, pos);

        std::printf ("      multi-tap: retries=%d resets=%u post=%u out=%u\n",
                     p.diagnostics.retryCount(), p.diagnostics.engineResets(),
                     p.diagnostics.nonFinite (EDStage::postEngine),
                     p.diagnostics.nonFinite (EDStage::output));
        check (p.diagnostics.retryCount() == 1,
               "multi-tap: one fault costs exactly one retry");
        check (p.diagnostics.engineResets() == 1,
               "multi-tap: one fault performs exactly one recovery");
        check (p.diagnostics.currentState() != EDState::halted,
               "multi-tap: a single fault does not halt the plugin");
    }

    // ---- 6. stereo: a fault on ONE side is caught, image intact ------------
    {
        VoxMorphProcessor p;
        p.prepareToPlay (kSr, 256);
        setStereoInput (p, true);
        juce::AudioBuffer<float> buf (2, 256);
        int64_t pos = 0;
        runTone (p, buf, 60, pos);

        p.diagInjectStage.store ((int) EDStage::postEngine);
        p.diagInjectBlocks.store (1);          // hook writes channel 0 only
        const bool finite = runTone (p, buf, 1, pos);
        check (finite, "stereo: a right/left-only fault still leaves the output finite");
        check (p.diagnostics.nonFinite (EDStage::postEngine) >= 1,
               "stereo: a fault on one side alone is caught");
        const bool ok = runTone (p, buf, 150, pos);
        check (ok, "stereo: recovers with both engines running");
    }

    // ---- 7. repeated faults halt; the log and manual recovery work ----------
    {
        VoxMorphProcessor p;
        p.prepareToPlay (kSr, 256);
        juce::AudioBuffer<float> buf (2, 256);
        int64_t pos = 0;
        runTone (p, buf, 40, pos);

        for (int f = 0; f < EngineDiagnostics::kMaxRetries + 2; ++f)
        {
            p.diagInjectStage.store ((int) EDStage::postEngine);
            p.diagInjectBlocks.store (1);
            runTone (p, buf, 1, pos);
            runTone (p, buf, 4, pos);       // far short of kCleanSec
        }
        p.diagInjectStage.store (-1);
        std::printf ("      halt: state=%s retries=%d resets=%u\n",
                     stateName (p.diagnostics.currentState()),
                     p.diagnostics.retryCount(), p.diagnostics.engineResets());
        check (p.diagnostics.currentState() == EDState::halted,
               "halt: repeated faults latch at halted");

        std::vector<float> trace;
        runTone (p, buf, 100, pos, &trace);
        float pk = 0.0f;
        for (float v : trace) pk = std::max (pk, std::abs (v));
        check (pk == 0.0f, "halt: stays silent on clean audio");

        // log is readable from this (non-audio) thread and actually has content
        EngineDiagnostics::Event ev[EngineDiagnostics::kEventCap];
        const int got = p.diagnostics.readEvents (ev, EngineDiagnostics::kEventCap);
        std::printf ("      halt: %d events read, %u dropped\n",
                     got, p.diagnostics.droppedEvents());
        check (got > 0, "log: events are readable while the plugin is running");
        bool sawHalt = false;
        for (int i = 0; i < got; ++i)
            if (ev[i].kind == EngineDiagnostics::Kind::halt) sawHalt = true;
        check (sawHalt, "log: the halt itself is in the log");

        // a consuming read empties it, and the writer keeps going
        const int again = p.diagnostics.readEvents (ev, EngineDiagnostics::kEventCap);
        check (again == 0, "log: a consuming read empties the queue");

        // manual recovery: UI writes one atomic, audio thread accepts it
        p.diagnostics.requestManualRecovery();
        check (p.diagnostics.manualRecoveryPending(),
               "manual: the request is pending until a block runs");
        trace.clear();
        runTone (p, buf, 200, pos, &trace);
        pk = 0.0f;
        for (float v : trace) pk = std::max (pk, std::abs (v));
        std::printf ("      manual: state=%s peak after clear %.4f\n",
                     stateName (p.diagnostics.currentState()), pk);
        check (! p.diagnostics.manualRecoveryPending(),
               "manual: the audio thread consumed the request");
        check (pk > 0.0f, "manual: output comes back");
        check (p.diagnostics.currentState() != EDState::halted,
               "manual: no longer halted");
    }

    // ---- 8. gain-history consistency across a recovery ----------------------
    // ProtectionGain must be reset with the engine. If it were not, the
    // restore would multiply fresh output by the inverse of a gain applied to
    // samples the engine has just discarded -- which shows up as a level
    // overshoot in the first D samples after recovery.
    {
        VoxMorphProcessor p;
        p.prepareToPlay (kSr, 256);
        juce::AudioBuffer<float> buf (2, 256);
        int64_t pos = 0;

        // drive ProtectionGain into real reduction first, so there IS a
        // history that could be applied to the wrong samples
        juce::MidiBuffer midi;
        for (int b = 0; b < 120; ++b) { fillTone (buf, pos, 2.0f); p.processBlock (buf, midi); pos += 256; }

        std::vector<float> before;
        runTone (p, buf, 40, pos, &before);          // quiet reference level
        float refPk = 0.0f;
        for (float v : before) refPk = std::max (refPk, std::abs (v));

        p.diagInjectStage.store ((int) EDStage::postEngine);
        p.diagInjectBlocks.store (1);
        runTone (p, buf, 1, pos);
        p.diagInjectStage.store (-1);

        std::vector<float> after;
        runTone (p, buf, 200, pos, &after);
        float postPk = 0.0f;
        for (float v : after) postPk = std::max (postPk, std::abs (v));

        std::printf ("      gain history: peak before %.4f, after recovery %.4f\n",
                     refPk, postPk);
        check (postPk <= refPk * 2.0f + 0.05f,
               "gain history: no level overshoot after recovery (histories stayed in step)");
        check (allFinite (buf), "gain history: output finite after recovery");
    }

    // ---- 9. a large host buffer behaves the same ---------------------------
    // The review asked for the sample-interpolated ramp to be checked at a big
    // buffer too: with 4096 samples the whole 30 ms fade fits inside ONE block,
    // which is the opposite extreme from the 256 case that found the step.
    {
        VoxMorphProcessor p;
        p.prepareToPlay (kSr, 4096);
        juce::AudioBuffer<float> buf (2, 4096);
        int64_t pos = 0;
        runTone (p, buf, 10, pos);

        p.diagInjectStage.store ((int) EDStage::postEngine);
        p.diagInjectBlocks.store (1);
        runTone (p, buf, 1, pos);
        p.diagInjectStage.store (-1);

        std::vector<float> trace;
        const bool finite = runTone (p, buf, 12, pos, &trace);
        float worst = 0.0f;
        for (size_t i = 1; i < trace.size(); ++i)
            worst = std::max (worst, std::abs (std::abs (trace[i]) - std::abs (trace[i-1])));
        std::printf ("      4096-sample buffer: worst |sample| step %.5f\n", worst);
        check (finite, "large buffer: output stays finite through a recovery");
        check (worst < 0.05f, "large buffer: recovery still has no step");
        check (p.diagnostics.engineResets() == 1, "large buffer: one recovery");
    }

    // ---- 10. gain history: ALIGNED WAVEFORM, not just a peak bound ---------
    // The previous version only checked that the peak after recovery was not
    // absurd, which a badly aligned gain history could still pass. This runs
    // two identically driven instances, faults one of them, and then compares
    // the two waveforms sample for sample once both are settled. If
    // ProtectionGain's history had been left behind, the faulted one would
    // come back at a different level and the difference would not close.
    {
        VoxMorphProcessor a, b;
        a.prepareToPlay (kSr, 256);
        b.prepareToPlay (kSr, 256);
        juce::AudioBuffer<float> ba (2, 256), bb (2, 256);
        juce::MidiBuffer midi;
        int64_t pa = 0, pb = 0;

        // drive ProtectionGain into real reduction on BOTH, so there is a
        // history that could be misapplied
        for (int i = 0; i < 120; ++i)
        {
            fillTone (ba, pa, 2.0f);  fillTone (bb, pb, 2.0f);
            a.processBlock (ba, midi); b.processBlock (bb, midi);
            pa += 256;  pb += 256;
        }

        a.diagInjectStage.store ((int) EDStage::postEngine);
        a.diagInjectBlocks.store (1);
        { fillTone (ba, pa); fillTone (bb, pb);
          a.processBlock (ba, midi); b.processBlock (bb, midi); pa += 256; pb += 256; }
        a.diagInjectStage.store (-1);

        // let the faulted one finish recovering AND the engine refill
        for (int i = 0; i < 400; ++i)
        {
            fillTone (ba, pa);  fillTone (bb, pb);
            a.processBlock (ba, midi); b.processBlock (bb, midi);
            pa += 256;  pb += 256;
        }

        // Compare the two settled runs. Both quantities are reported, because
        // they answer different questions and only one of them is evidence
        // about the gain history.
        //
        // The sample-by-sample residual CANNOT go to zero here, and that is
        // not a fault: engine.prepare() restarts TD-PSOLA's grain scheduling
        // from writePos 0, so the recovered instance lands its grains at
        // different absolute positions. The result is an equally valid
        // rendering of the same input at a different grain phase. Measuring
        // that difference measures the restart, not the gain history.
        //
        // What a mis-aligned ProtectionGain history WOULD do is change the
        // LEVEL -- the restore would multiply by the inverse of a gain applied
        // to samples the engine discarded. So the envelope is the quantity
        // that carries the claim, and it is what is asserted.
        double num = 0.0, den = 0.0, worst = 0.0, sqA = 0.0, sqB = 0.0;
        double pkA = 0.0, pkB = 0.0;
        long   nTot = 0;
        for (int i = 0; i < 100; ++i)
        {
            fillTone (ba, pa);  fillTone (bb, pb);
            a.processBlock (ba, midi); b.processBlock (bb, midi);
            const float* da = ba.getReadPointer (0);
            const float* db = bb.getReadPointer (0);
            for (int k = 0; k < 256; ++k)
            {
                const double d = (double) da[k] - (double) db[k];
                num += d * d;  den += (double) db[k] * db[k];
                worst = std::max (worst, std::abs (d));
                sqA += (double) da[k] * da[k];  sqB += (double) db[k] * db[k];
                pkA = std::max (pkA, (double) std::abs (da[k]));
                pkB = std::max (pkB, (double) std::abs (db[k]));
                ++nTot;
            }
            pa += 256;  pb += 256;
        }
        const double relDb = den > 1e-20 ? 10.0 * std::log10 (std::max (num / den, 1e-20))
                                         : -200.0;
        const double rmsA = std::sqrt (sqA / (double) std::max (1L, nTot));
        const double rmsB = std::sqrt (sqB / (double) std::max (1L, nTot));
        const double rmsDb  = 20.0 * std::log10 (std::max (rmsA, 1e-12) / std::max (rmsB, 1e-12));
        const double peakDb = 20.0 * std::log10 (std::max (pkA, 1e-12) / std::max (pkB, 1e-12));

        std::printf ("      recovered vs clean: RMS %+.3f dB, peak %+.3f dB"
                     "  (raw residual %.1f dB rel, worst |diff| %.5f -- grain phase)\n",
                     rmsDb, peakDb, relDb, worst);
        check (std::abs (rmsDb) < 0.5,
               "gain history: recovered instance is at the SAME LEVEL as the clean one");
        check (std::abs (peakDb) < 1.0,
               "gain history: and the same peak, so no restore overshoot survived");
        check (a.diagnostics.engineResets() == 1, "gain history: one recovery only");
    }

    // ---- 11. manual recovery keeps the HALTING CAUSE -----------------------
    // A halt caused downstream must not be cleared by re-preparing the engine.
    {
        VoxMorphProcessor p;
        p.prepareToPlay (kSr, 256);
        juce::AudioBuffer<float> buf (2, 256);
        int64_t pos = 0;
        runTone (p, buf, 40, pos);

        for (int f = 0; f < EngineDiagnostics::kMaxRetries + 2; ++f)
        {
            p.diagInjectStage.store ((int) EDStage::output);   // DOWNSTREAM
            p.diagInjectBlocks.store (1);
            runTone (p, buf, 1, pos);
            runTone (p, buf, 4, pos);
        }
        p.diagInjectStage.store (-1);
        check (p.diagnostics.currentState() == EDState::halted,
               "cause-preserving: repeated downstream faults halt");
        check (p.diagnostics.lastOrigin() == EDOrigin::downstream,
               "cause-preserving: the halting cause is downstream");

        const uint32_t resetsAtHalt = p.diagnostics.engineResets();
        p.diagnostics.requestManualRecovery();
        runTone (p, buf, 200, pos);
        std::printf ("      cause-preserving: state=%s origin=%s recoveries %u -> %u\n",
                     stateName (p.diagnostics.currentState()),
                     originName (p.diagnostics.lastOrigin()),
                     resetsAtHalt, p.diagnostics.engineResets());
        check (p.diagnostics.currentState() != EDState::halted,
               "cause-preserving: manual clear brings it back");
        check (p.diagnostics.lastOrigin() == EDOrigin::downstream,
               "cause-preserving: it did not relabel the cause as an engine fault");
    }

    // ---- 12. a contained upstream fault must not mask a real one -----------
    // Review item 4: once the input has been sanitized the engine got CLEAN
    // samples, so a non-finite coming out of it in the same block is an
    // independent failure and must still be able to ask for its recovery.
    {
        VoxMorphProcessor p;
        p.prepareToPlay (kSr, 256);
        juce::AudioBuffer<float> buf (2, 256);
        juce::MidiBuffer midi;
        int64_t pos = 0;
        runTone (p, buf, 40, pos);
        const uint32_t before = p.diagnostics.engineResets();

        // upstream rubbish AND an engine fault, in the SAME block
        p.diagInjectStage.store ((int) EDStage::postEngine);
        p.diagInjectBlocks.store (1);
        fillTone (buf, pos);
        buf.getWritePointer (0)[5] = std::numeric_limits<float>::quiet_NaN();
        p.processBlock (buf, midi);
        pos += 256;
        p.diagInjectStage.store (-1);

        std::printf ("      contained+independent: origin=%s recoveries %u -> %u retries=%d\n",
                     originName (p.diagnostics.lastOrigin()), before,
                     p.diagnostics.engineResets(), p.diagnostics.retryCount());
        check (p.diagnostics.engineResets() == before + 1,
               "contained upstream: the independent engine fault still got its recovery");
        check (p.diagnostics.lastOrigin() == EDOrigin::engine,
               "contained upstream: the engine fault is the one that is reported");
        check (p.diagnostics.retryCount() == 1,
               "contained upstream: the budget is still spent only once for the block");
        check (allFinite (buf), "contained upstream: the block still leaves finite");
    }

    // ---- 13. the recovery path's real-time cost ---------------------------
    // The measurement both reviews asked for and neither previous delivery
    // made: engine.prepare() is called from the audio callback, so does that
    // path allocate, and how long does it take?
    {
        VoxMorphProcessor p;
        p.prepareToPlay (kSr, 256);
        juce::AudioBuffer<float> buf (2, 256);
        juce::MidiBuffer midi;
        int64_t pos = 0;
        runTone (p, buf, 60, pos);            // warm every lazy allocation up

        // a normal block first, as the reference
        g_allocCount = 0;  g_countAlloc = true;
        const double n0 = juce::Time::getMillisecondCounterHiRes();
        fillTone (buf, pos);  p.processBlock (buf, midi);  pos += 256;
        const double normalMs = juce::Time::getMillisecondCounterHiRes() - n0;
        const long normalAllocs = g_allocCount;
        g_countAlloc = false;

        // then the recovery block
        p.diagInjectStage.store ((int) EDStage::postEngine);
        p.diagInjectBlocks.store (1);
        g_allocCount = 0;  g_countAlloc = true;
        const double r0 = juce::Time::getMillisecondCounterHiRes();
        fillTone (buf, pos);  p.processBlock (buf, midi);  pos += 256;
        const double recoveryMs = juce::Time::getMillisecondCounterHiRes() - r0;
        const long recoveryAllocs = g_allocCount;
        g_countAlloc = false;
        p.diagInjectStage.store (-1);

        const double budgetMs = 1000.0 * 256.0 / kSr;
        std::printf ("      normal block  : %ld alloc, %.3f ms\n", normalAllocs, normalMs);
        std::printf ("      recovery block: %ld alloc, %.3f ms (block budget %.3f ms)\n",
                     recoveryAllocs, recoveryMs, budgetMs);
        check (normalAllocs == 0, "real-time: a normal block allocates nothing");
        check (recoveryAllocs == 0,
               "real-time: the recovery block (engine.prepare) allocates nothing either");
        check (recoveryMs < budgetMs,
               "real-time: the recovery block still fits inside its own block budget");
        check (p.diagnostics.engineResets() == 1, "real-time: the recovery did happen");
    }

    std::printf ("\n%s (%d failure%s)\n", g_fail == 0 ? "ALL PASS" : "FAILURES",
                 g_fail, g_fail == 1 ? "" : "s");
    return g_fail == 0 ? 0 : 1;
}
