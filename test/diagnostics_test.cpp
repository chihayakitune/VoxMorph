// diagnostics_test.cpp — checks for dsp/EngineDiagnostics.h (stage 1, v0.68.0).
//
// Build (no JUCE, no numpy):
//   g++ -O2 -std=c++17 test/diagnostics_test.cpp -o /tmp/diag && /tmp/diag
//
// The checks follow the stage-1 brief, in the order the failures would hurt:
//
//  1. NORMAL IS UNCHANGED. While nothing is wrong the gate is exactly 1.0 and
//     no event is logged, so the audio path is sample-identical. A watchdog
//     that costs you the signal it is guarding is not worth having.
//  2. NON-FINITE IS CAUGHT AND RATIONED. A NaN gates the audio, asks for one
//     reset, and after kMaxRetries latches at halted rather than restarting
//     the engine forever.
//  3. RECOVERY DOES NOT CLICK. The fade back in is monotonic and starts at 0.
//  4. FALSE POSITIVES. Silence, mute, a shut gate, Mix at 0 and the warm-up
//     window must NOT be reported as a stalled engine.
//  5. NO ALLOCATION after prepare(), which is what makes it safe on the
//     audio callback.
#include "../dsp/EngineDiagnostics.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <vector>

// ---- allocation counter (same shape as offline_test.cpp) -------------------
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
static void check (bool ok, const char* what)
{
    std::printf ("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (! ok) ++g_fail;
}

using D  = EngineDiagnostics;
using St = EngineDiagnostics::State;
using Sg = EngineDiagnostics::Stage;

static constexpr double kFs = 48000.0;
static constexpr int    kN  = 256;

// One block of a steady tone, so "live input" is unambiguous.
static void fill (std::vector<float>& v, float amp, int phase = 0)
{
    for (int i = 0; i < (int) v.size(); ++i)
        v[(size_t) i] = amp * (float) std::sin (2.0 * M_PI * 200.0 * (i + phase) / kFs);
}

// Drives one block through all four observation points, standing in for
// processBlock. Returns the gate the caller would apply to the output.
// gainTrace, when given, receives the gain actually applied to every sample --
// which is what a click is measured on. The per-block value alone cannot show
// a discontinuity at the buffer boundary.
static float runBlock (D& d, std::vector<float>& pre, std::vector<float>& post,
                       const D::Context& ctx, bool* resetHappened = nullptr,
                       std::vector<float>* gainTrace = nullptr)
{
    d.beginBlock (kN, ctx);

    float* inCh[1]   = { pre.data() };
    if (d.observe (Sg::input, inCh, 1, kN) == D::Action::sanitize)
        for (auto& v : pre) if (! std::isfinite (v)) v = 0.0f;
    if (d.observe (Sg::preEngine, inCh, 1, kN) == D::Action::sanitize)
        for (auto& v : pre) if (! std::isfinite (v)) v = 0.0f;

    float* postCh[1] = { post.data() };
    if (d.observe (Sg::postEngine, postCh, 1, kN) == D::Action::silenceBlock)
        std::fill (post.begin(), post.end(), 0.0f);

    if (d.pendingRecovery() != D::RecoveryPlan::none)
    {
        d.beginRecovery();
        if (resetHappened != nullptr) *resetHappened = true;
    }

    float* outCh[1] = { post.data() };
    d.observe (Sg::output, outCh, 1, kN);
    if (d.pendingRecovery() != D::RecoveryPlan::none) d.beginRecovery();
    d.checkStall();
    const auto ramp = d.advanceGain();
    if (gainTrace != nullptr)
    {
        const float step = (ramp.to - ramp.from) / (float) kN;
        float g = ramp.from;
        for (int i = 0; i < kN; ++i) { gainTrace->push_back (g); g += step; }
    }
    d.endBlock (0.001);
    return ramp.to;
}

int main()
{
    // ---- 1. normal operation is untouched ----------------------------------
    {
        D d; d.prepare (kFs, kN);
        std::vector<float> pre ((size_t) kN), post ((size_t) kN);
        D::Context ctx;

        bool allUnity = true;
        for (int b = 0; b < 200; ++b)
        {
            fill (pre, 0.2f, b * kN);
            fill (post, 0.25f, b * kN);
            const auto before = post;
            if (runBlock (d, pre, post, ctx) != 1.0f) allUnity = false;
            if (post != before) allUnity = false;      // not a sample was moved
        }
        check (allUnity, "normal: gate is exactly 1.0 and no sample is touched");
        check (d.currentState() == St::normal, "normal: state stays normal");
        check (d.nonFinite (Sg::postEngine) == 0 && d.clips (Sg::postEngine) == 0,
               "normal: nothing counted");
        D::Event ev[8];
        check (d.readEvents (ev, 8) == 0, "normal: no events logged");
    }

    // ---- 2. non-finite at post-engine gates the audio ----------------------
    {
        D d; d.prepare (kFs, kN);
        std::vector<float> pre ((size_t) kN), post ((size_t) kN);
        D::Context ctx;

        fill (pre, 0.2f);  fill (post, 0.25f);
        runBlock (d, pre, post, ctx);

        fill (post, 0.25f);
        post[100] = std::numeric_limits<float>::quiet_NaN();
        post[101] = std::numeric_limits<float>::infinity();
        bool didReset = false;
        const float g = runBlock (d, pre, post, ctx, &didReset);

        check (g == 0.0f, "fault: the block is gated to silence");
        bool zeroed = true;
        for (float v : post) if (v != 0.0f) zeroed = false;
        check (zeroed, "fault: the offending block is zeroed, no NaN downstream");
        check (didReset, "fault: an engine reset was requested");
        check (d.nonFinite (Sg::postEngine) == 2, "fault: both bad samples counted");
        check (d.currentState() == St::awaitingRecovery, "fault: state is awaitingRecovery");
        check (d.engineResets() == 1, "fault: exactly one reset, not a storm");
        check (d.retryCount() == 1, "fault: one fault costs exactly one retry");
        check (d.lastOrigin() == D::Origin::engine, "fault: classified as engine");

        // the pre-engine side was clean, which is what says it was the engine
        check (d.nonFinite (Sg::preEngine) == 0,
               "fault: pre-engine stayed clean, so the stage is identified");
    }

    // ---- 3. recovery ramps in without a step -------------------------------
    {
        D d; d.prepare (kFs, kN);
        std::vector<float> pre ((size_t) kN), post ((size_t) kN);
        D::Context ctx;

        fill (pre, 0.2f);  fill (post, 0.25f);
        runBlock (d, pre, post, ctx);
        fill (post, 0.25f);  post[0] = std::numeric_limits<float>::quiet_NaN();
        runBlock (d, pre, post, ctx);

        std::vector<float> trace;
        bool reachedUnity = false;
        for (int b = 0; b < 200 && ! reachedUnity; ++b)
        {
            fill (post, 0.25f);
            if (runBlock (d, pre, post, ctx, nullptr, &trace) == 1.0f) reachedUnity = true;
        }

        // Measured on the gain actually applied to each SAMPLE, so a step at a
        // buffer boundary shows up here even though the per-block value looks
        // smooth. 256 samples at 48 kHz over a 30 ms fade would be ~0.27 per
        // block; per sample it has to be about three orders of magnitude less.
        float worstStep = 0.0f;
        bool monotonic = true;
        for (size_t i = 1; i < trace.size(); ++i)
        {
            const float d1 = trace[i] - trace[i-1];
            if (d1 < -1.0e-6f) monotonic = false;
            worstStep = std::max (worstStep, std::abs (d1));
        }
        std::printf ("      recovery: first gain %.3f, worst PER-SAMPLE step %.5f over %d samples\n",
                     trace.empty() ? -1.0f : trace.front(), worstStep, (int) trace.size());
        check (! trace.empty() && trace.front() == 0.0f,
               "recovery: starts from silence, not from a jump to full");
        check (monotonic, "recovery: fade is monotonic (no gain bounce)");
        check (worstStep < 0.002f, "recovery: no per-sample step big enough to click");
        check (reachedUnity, "recovery: returns to unity");
        check (d.currentState() == St::normal, "recovery: state returns to normal");
    }

    // ---- 4. retries are rationed, then it halts ----------------------------
    {
        D d; d.prepare (kFs, kN);
        std::vector<float> pre ((size_t) kN), post ((size_t) kN);
        D::Context ctx;

        for (int f = 0; f < D::kMaxRetries + 2; ++f)
        {
            fill (post, 0.25f);
            post[0] = std::numeric_limits<float>::quiet_NaN();
            fill (pre, 0.2f);
            runBlock (d, pre, post, ctx);
        }
        check (d.currentState() == St::halted,
               "halt: repeated faults latch at halted instead of resetting forever");
        check (d.engineResets() <= (uint32_t) D::kMaxRetries + 1,
               "halt: recoveries were rationed, not unbounded");

        // and it STAYS muted with clean audio, until a person clears it
        bool stayedMuted = true;
        for (int b = 0; b < 50; ++b)
        {
            fill (pre, 0.2f);  fill (post, 0.25f);
            if (runBlock (d, pre, post, ctx) != 0.0f) stayedMuted = false;
        }
        check (stayedMuted, "halt: stays muted on clean audio (manual recovery required)");
        check (d.currentState() == St::halted, "halt: does not self-clear");

        // manual clear brings it back
        d.requestManualRecovery();
        check (d.manualRecoveryPending(),
               "manual: the request is pending until a block runs");
        bool back = false;
        for (int b = 0; b < 200 && ! back; ++b)
        {
            fill (pre, 0.2f);  fill (post, 0.25f);
            if (runBlock (d, pre, post, ctx) == 1.0f) back = true;
        }
        check (back, "halt: manual recovery restores the output");
    }

    // ---- 5. false positives: a silent engine that is CORRECT ---------------
    {
        struct Case { const char* name; D::Context ctx; bool liveIn; };
        const Case cases[] {
            { "silent input",   D::Context {},                              false },
            { "muted",          D::Context { true,  true,  true,  true  },  true  },
            { "gate shut",      D::Context { false, false, true,  true  },  true  },
            { "Mix at 0",       D::Context { false, true,  false, true  },  true  },
            { "still warming",  D::Context { false, true,  true,  false },  true  },
        };
        for (auto& c : cases)
        {
            D d; d.prepare (kFs, kN);
            std::vector<float> pre ((size_t) kN), post ((size_t) kN, 0.0f);
            for (int b = 0; b < (int) (kFs * 2.0 / kN); ++b)   // 2 s, well past kStallSec
            {
                fill (pre, c.liveIn ? 0.2f : 0.0f, b * kN);
                std::fill (post.begin(), post.end(), 0.0f);    // engine silent
                runBlock (d, pre, post, c.ctx);
            }
            char msg[128];
            std::snprintf (msg, sizeof msg,
                           "false positive: \"%s\" is not reported as a stall", c.name);
            check (d.currentState() == St::normal, msg);
        }

        // ...and the genuine case IS reported
        D d; d.prepare (kFs, kN);
        std::vector<float> pre ((size_t) kN), post ((size_t) kN, 0.0f);
        D::Context ctx;                                  // live, unmuted, warmed up
        for (int b = 0; b < (int) (kFs * 2.0 / kN); ++b)
        {
            fill (pre, 0.2f, b * kN);
            std::fill (post.begin(), post.end(), 0.0f);
            runBlock (d, pre, post, ctx);
        }
        check (d.currentState() == St::caution,
               "stall: live input with a silent engine IS reported (as caution)");
    }

    // ---- 6. clipping is recorded but never gates ---------------------------
    {
        D d; d.prepare (kFs, kN);
        std::vector<float> pre ((size_t) kN), post ((size_t) kN);
        D::Context ctx;
        bool everGated = false;
        for (int b = 0; b < 50; ++b)
        {
            fill (pre, 2.0f, b * kN);      // way over full scale
            fill (post, 2.0f, b * kN);
            if (runBlock (d, pre, post, ctx) != 1.0f) everGated = true;
        }
        check (d.clips (Sg::postEngine) > 0, "clip: counted at the stage it happened");
        check (! everGated, "clip: never silences the audio");
        check (d.currentState() == St::caution, "clip: raises caution only");
    }

    // ---- 7. event ring saturates safely ------------------------------------
    {
        D d; d.prepare (kFs, kN);
        std::vector<float> pre ((size_t) kN), post ((size_t) kN);
        D::Context ctx;
        for (int b = 0; b < D::kEventCap * 4; ++b)
        {
            fill (pre, 2.0f, b * kN);      // one clip event per block per stage
            fill (post, 2.0f, b * kN);
            runBlock (d, pre, post, ctx);
        }
        D::Event ev[D::kEventCap];
        const int got = d.readEvents (ev, D::kEventCap);
        check (got == D::kEventCap, "log: the ring holds exactly its capacity");
        check (d.droppedEvents() > 0, "log: what scrolled off is counted, not hidden");
        std::printf ("      log: %d held, %u dropped\n", got, d.droppedEvents());
        bool ordered = true;
        for (int i = 1; i < got; ++i) if (ev[i].block < ev[i-1].block) ordered = false;
        check (ordered, "log: snapshot is in order, oldest first");
        check (d.readEvents (ev, D::kEventCap) == 0,
               "log: a consuming read empties the queue");
    }

    // ---- 8. stereo: a fault on ONE side only is still caught ---------------
    {
        D d; d.prepare (kFs, kN);
        std::vector<float> L ((size_t) kN), R ((size_t) kN);
        fill (L, 0.25f);  fill (R, 0.25f);
        R[7] = std::numeric_limits<float>::quiet_NaN();     // right engine only
        D::Context ctx;
        d.beginBlock (kN, ctx);
        float* ch[2] = { L.data(), R.data() };
        const bool caught = d.observe (Sg::postEngine, ch, 2, kN) == D::Action::silenceBlock;
        check (caught, "stereo: a fault on the right side alone is caught");
        check (d.nonFinite (Sg::postEngine) == 1, "stereo: counted once, from the bad side");
    }

    // ---- 9. no allocation after prepare -------------------------------------
    {
        D d; d.prepare (kFs, kN);
        std::vector<float> pre ((size_t) kN), post ((size_t) kN);
        D::Context ctx;
        fill (pre, 0.2f);  fill (post, 0.25f);
        runBlock (d, pre, post, ctx);          // warm up

        g_allocCount = 0;  g_countAlloc = true;
        for (int b = 0; b < 100; ++b)
        {
            fill (pre, 0.2f, b * kN);
            fill (post, 0.25f, b * kN);
            if (b == 50) post[3] = std::numeric_limits<float>::quiet_NaN();
            runBlock (d, pre, post, ctx);
        }
        D::Event ev[D::kEventCap];
        d.readEvents (ev, D::kEventCap);
        g_countAlloc = false;
        std::printf ("      allocations across 100 blocks incl. a fault: %ld\n", g_allocCount);
        check (g_allocCount == 0, "audio thread: no allocation, fault path included");
    }

    std::printf ("\n%s (%d failure%s)\n", g_fail ? "FAILURES" : "all checks passed",
                 g_fail, g_fail == 1 ? "" : "s");
    return g_fail ? 1 : 0;
}
