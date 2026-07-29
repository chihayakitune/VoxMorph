// viz_race.cpp - the audio thread / UI thread hand-off of the visualizer taps.
//
// WHEN TO USE: any change to how vizIn / vizOut / vizPos are written in
// PluginProcessor::processBlock or read in SpectrumData (PluginEditor.h).
// The writer and the reader below are copies of that code with the JUCE
// parts stripped out, so this file has to be kept in step with them.
//
// It answers two questions the plugin itself cannot be asked directly:
// does Thread Sanitizer see a data race, and does a window that reaches the
// FFT ever contain samples from two different moments?
//
//   OLD=1  -> the pre-v0.31.1 version (plain float ring, int position).
//             Expected to FAIL: 4 TSan races, and 30 torn windows once the
//             reader is stalled. Kept so the fix can be re-demonstrated.
//   OLD=0  -> the current version (atomic<float> ring, unsigned position,
//             window copy validated against vizPos afterwards).
//
// HOW (from the repo root):
//     clang++ -std=c++17 -O1 -g -fsanitize=thread -DOLD=0 -o /tmp/vr test/viz_race.cpp
//     /tmp/vr                          # wanted: PASS, and no TSan output
//     clang++ -std=c++17 -O1 -g -fsanitize=thread -DOLD=1 -o /tmp/vr test/viz_race.cpp
//     /tmp/vr                          # the problem this replaced
#include <atomic>
#include <cassert>
#include <cstdio>
#include <thread>
#include <vector>

static constexpr int kVizLen = 16384;   // as in VoxMorphProcessor
static constexpr int kN      = 4096;    // as in SpectrumData

#if OLD
using Pos  = int;
using Cell = float;
static inline float ld (const Cell& c) { return c; }
static inline void  st (Cell& c, float v) { c = v; }
#else
using Pos  = unsigned;
using Cell = std::atomic<float>;
static inline float ld (const Cell& c) { return c.load (std::memory_order_relaxed); }
static inline void  st (Cell& c, float v) { c.store (v, std::memory_order_relaxed); }
#endif

static std::vector<Cell> vizIn  = std::vector<Cell> ((size_t) kVizLen);
static std::vector<Cell> vizOut = std::vector<Cell> ((size_t) kVizLen);
static std::atomic<Pos>  vizPos { 0 };
static std::atomic<bool> running { true };

// ---- writer: the audio thread ------------------------------------------
static void writeBlock (int n)
{
    const Pos vp = vizPos.load (std::memory_order_relaxed);
    for (int i = 0; i < n; ++i)
        st (vizIn[(size_t) ((vp + (Pos) i) & (Pos) (kVizLen - 1))], (float) (Pos) (vp + (Pos) i));
    for (int i = 0; i < n; ++i)
        st (vizOut[(size_t) ((vp + (Pos) i) & (Pos) (kVizLen - 1))], (float) (Pos) (vp + (Pos) i));
    vizPos.store (vp + (Pos) n, std::memory_order_release);
}

static void audioThread (int blockSize, int blocks, int stallEveryN, int stallUs)
{
    for (int b = 0; b < blocks && running.load(); ++b)
    {
        const Pos vp = vizPos.load (std::memory_order_relaxed);
        const int n  = blockSize;
        // the sample value IS its absolute position, so the reader can check
        // that the window it got is one contiguous run of the right samples
        for (int i = 0; i < n; ++i)
            st (vizIn[(size_t) ((vp + (Pos) i) & (Pos) (kVizLen - 1))], (float) (Pos) (vp + (Pos) i));
        for (int i = 0; i < n; ++i)
            st (vizOut[(size_t) ((vp + (Pos) i) & (Pos) (kVizLen - 1))], (float) (Pos) (vp + (Pos) i));
        vizPos.store (vp + (Pos) n, std::memory_order_release);

        if (stallEveryN > 0 && b % stallEveryN == 0)
            std::this_thread::sleep_for (std::chrono::microseconds (stallUs));
    }
    running.store (false);
}

// ---- reader: the message thread (SpectrumData::timerCallback) -----------
static void grabWindow (const std::vector<Cell>& src, Pos pos, std::vector<float>& dest)
{
    const Pos mask = (Pos) kVizLen - 1;
    for (int i = 0; i < kN; ++i)
        dest[(size_t) i] = ld (src[(size_t) ((pos - (Pos) kN + (Pos) i) & mask)]);
}

struct Stats { long frames = 0, dropped = 0, torn = 0; };

static void uiThread (Stats& s, int readDelayUs)
{
    std::vector<float> winIn ((size_t) kN), winOut ((size_t) kN);
    while (running.load())
    {
        const Pos pos = vizPos.load (std::memory_order_acquire);
        if (pos < (Pos) kN) continue;

        grabWindow (vizIn,  pos, winIn);
        if (readDelayUs > 0)     // stand-in for a stalled/descheduled UI thread
            std::this_thread::sleep_for (std::chrono::microseconds (readDelayUs));
        grabWindow (vizOut, pos, winOut);

#if ! OLD
        if (vizPos.load (std::memory_order_acquire) - pos > (Pos) (kVizLen - kN))
        { ++s.dropped; continue; }
#endif
        ++s.frames;
        // a good window is kN consecutive samples ending at pos
        for (int i = 0; i < kN; ++i)
        {
            const float want = (float) (Pos) (pos - (Pos) kN + (Pos) i);
            if (winIn[(size_t) i] != want || winOut[(size_t) i] != want) { ++s.torn; break; }
        }
    }
}

static Stats run (int blockSize, int blocks, int readDelayUs, int stallEveryN, int stallUs)
{
    vizPos.store (0);
    running.store (true);
    Stats s;
    std::thread a (audioThread, blockSize, blocks, stallEveryN, stallUs);
    std::thread u (uiThread, std::ref (s), readDelayUs);
    a.join(); u.join();
    return s;
}

int main()
{
    // 1. the rings must start silent (value-initialised atomics)
    for (int i = 0; i < kVizLen; ++i)
        assert (ld (vizIn[(size_t) i]) == 0.0f && ld (vizOut[(size_t) i]) == 0.0f);
    printf ("rings zero-initialised                                 OK\n");

    // 2. normal traffic: 512-sample blocks, reader never stalls
    Stats n = run (512, 4000, 0, 0, 0);
    printf ("normal   frames=%-7ld dropped=%-7ld torn=%ld\n", n.frames, n.dropped, n.torn);

    // 3. the reader is descheduled for ~1 ms in the middle of every frame
    Stats m = run (512, 4000, 1000, 0, 0);
    printf ("stalled  frames=%-7ld dropped=%-7ld torn=%ld\n", m.frames, m.dropped, m.torn);

    // 4. the position counter close to its 2^32 wrap. Prime the ring first:
    //    rewinding vizPos by hand is something only this harness does, and a
    //    reader that starts before a full ring has been written would (quite
    //    correctly) see samples left over from run 3.
    vizPos.store ((Pos) (0u - 100000u));
    for (int b = 0; b < kVizLen / 512; ++b) writeBlock (512);
    running.store (true);
    Stats w; { std::thread a (audioThread, 512, 1500, 0, 0);
               std::thread u (uiThread, std::ref (w), 0); a.join(); u.join(); }
    printf ("wrap     frames=%-7ld dropped=%-7ld torn=%ld\n", w.frames, w.dropped, w.torn);

    // 5. real rates: 512 samples every 10.7 ms against a 30 Hz reader, for 2 s.
    //    This is the case that must never drop a frame -- dropping is only for
    //    a message thread that stalled for a quarter of a second.
    vizPos.store (0);
    for (int b = 0; b < kVizLen / 512; ++b) writeBlock (512);
    running.store (true);
    Stats r;
    {
        std::thread a ([] {
            for (int b = 0; b < 187 && running.load(); ++b)
            {
                writeBlock (512);
                std::this_thread::sleep_for (std::chrono::microseconds (10666));
            }
            running.store (false);
        });
        std::thread u ([&r] {
            while (running.load())
            {
                std::vector<float> wi ((size_t) kN), wo ((size_t) kN);
                const Pos pos = vizPos.load (std::memory_order_acquire);
                grabWindow (vizIn, pos, wi);  grabWindow (vizOut, pos, wo);
#if ! OLD
                if (vizPos.load (std::memory_order_acquire) - pos > (Pos) (kVizLen - kN))
                { ++r.dropped; }
                else
#endif
                {
                    ++r.frames;
                    for (int i = 0; i < kN; ++i)
                    {
                        const float want = (float) (Pos) (pos - (Pos) kN + (Pos) i);
                        if (wi[(size_t) i] != want || wo[(size_t) i] != want) { ++r.torn; break; }
                    }
                }
                std::this_thread::sleep_for (std::chrono::milliseconds (33));
            }
        });
        a.join(); u.join();
    }
    printf ("realtime frames=%-7ld dropped=%-7ld torn=%ld\n", r.frames, r.dropped, r.torn);

    const bool ok = n.torn == 0 && m.torn == 0 && w.torn == 0 && r.torn == 0
                 && r.dropped == 0 && n.frames > 0 && w.frames > 0 && r.frames > 0;
    printf ("%s\n", ok ? "PASS: every analysed frame was one contiguous window"
                       : "FAIL: a torn window reached the FFT");
    return ok ? 0 : 1;
}
