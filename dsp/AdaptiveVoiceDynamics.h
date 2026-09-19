#pragma once

// VoxMorph -- Adaptive Voice Dynamics / Vocal Effort Compensation (Phase 0-2)
//
// Dependency-free single-header C++17.
//
// Two halves:
//
//   AvdControl  (processor, one instance)   estimator output -> correction
//               gains in dB, smoothed, stored per INPUT sample in a ring
//               indexed by the processor's own stream time.
//
//   AvdFilter   (inside each PsolaEngine)   reads the ring at the input time
//               its current OUTPUT sample came from (n - D, D = the engine's
//               actual lookahead) and applies Dynamic Tilt + Body/Presence
//               peaking cuts to the reconstructed wet signal.
//
// WHY A RING AND NOT "THE CURRENT VALUE"
// The engine's output is D samples (~43 ms, ~21 ms Low Latency) behind its
// input. Applying the effort measured on the current block to the output of
// the current block would correct the syllable BEFORE the one that was loud.
// Same reasoning as ProtectionGain's restore. No audio delay is added.
//
// STEREO
// Both engines receive the same AvdView and the same base time for the same
// block, so L and R read identical control for identical instants. Only the
// filter state is per channel.
//
// OFF
// When nothing but zeros could be read, the processor passes no view at all
// and the engine does not touch the sample. With a view, a sample whose three
// gains are exactly 0 is also left untouched, and the filter state is reset
// before the next non-zero sample so nothing stale is replayed.

#include <algorithm>
#include <cmath>
#include <cstdint>

#ifndef M_PI
#define M_PI 3.14159265358979323846   // MSVC (Windows) では未定義のため
#endif
#include <vector>

// Read-only view of the control ring handed to the engine for one call.
struct AvdView
{
    const float* tilt = nullptr;   // dB, >= 0 : high-frequency cut ("+ = softer")
    const float* body = nullptr;   // dB, <= 0 : ~400 Hz cut
    const float* pres = nullptr;   // dB, <= 0 : ~3.5 kHz cut
    int64_t mask      = 0;
    int64_t validFrom = 0;         // stream times outside [validFrom, validTo)
    int64_t validTo   = 0;         // read as neutral (0 dB)

    bool read (int64_t t, float& ti, float& bo, float& pr) const
    {
        if (tilt == nullptr || t < validFrom || t >= validTo) { ti = bo = pr = 0.0f; return false; }
        const size_t k = (size_t) (t & mask);
        ti = tilt[k];  bo = body[k];  pr = pres[k];
        return ti != 0.0f || bo != 0.0f || pr != 0.0f;
    }
};

class AvdControl
{
public:
    static constexpr float kTiltMaxDb = 2.0f;
    static constexpr float kBodyMaxDb = 2.5f;    // applied as a cut
    static constexpr float kPresMaxDb = 1.5f;    // applied as a cut

    // Allocates. `maxDelay` = the largest engine lookahead possible at this
    // rate; `segCap` = the most samples one processing segment may hold.
    void prepare (double sampleRate, int maxDelay, int segCap)
    {
        fs = sampleRate > 1000.0 ? sampleRate : 48000.0;
        size_t cap = 1;
        while (cap < (size_t) (maxDelay + segCap + 64)) cap <<= 1;
        tilt.assign (cap, 0.0f);  body.assign (cap, 0.0f);  pres.assign (cap, 0.0f);
        mask = (int64_t) cap - 1;
        dBound = maxDelay;
        smK   = (float) (1.0 - std::exp (-1.0 / (0.020 * fs)));   // 20 ms gain smoothing
        rampStep = (float) (1.0 / (0.030 * fs));                  // 30 ms on/off ramp
        stream = 0;
        resetTimeline();
        env = 0.0f;
    }

    // Break the time line: nothing written before now is read again, the
    // smoothers start from neutral. The on/off ramp position is kept.
    void resetTimeline()
    {
        validFrom = validTo = stream;
        sTilt = sBody = sPres = 0.0f;
        lastNonZero = INT64_MIN / 2;
    }

    int64_t now() const { return stream; }
    void advance (int n) { stream += n; }

    // true while the estimator has to run (target on, or ramping out)
    bool wantsRun (bool targetOn) const { return targetOn || env > 0.0f; }
    bool envelopeIdle() const { return env <= 0.0f; }

    // Called per input sample at stream time t (t increases by one each call
    // and continues the previous block). effort/bodyEx/presEx are 0..1.
    void push (int64_t t, bool targetOn, float amount,
               float effort, float bodyEx, float presEx)
    {
        if (t != validTo) { validFrom = t; }        // first write after a gap
        env = targetOn ? std::min (1.0f, env + rampStep) : std::max (0.0f, env - rampStep);

        const float a = std::clamp (amount, 0.0f, 1.0f) * std::clamp (effort, 0.0f, 1.0f);
        const float tT =  kTiltMaxDb * a;
        const float bT = -kBodyMaxDb * a * std::clamp (bodyEx, 0.0f, 1.0f);
        const float pT = -kPresMaxDb * a * std::clamp (presEx, 0.0f, 1.0f);
        sTilt += smK * (tT - sTilt);
        sBody += smK * (bT - sBody);
        sPres += smK * (pT - sPres);
        if (! std::isfinite (sTilt) || ! std::isfinite (sBody) || ! std::isfinite (sPres))
            sTilt = sBody = sPres = 0.0f;

        float ti = sTilt * env, bo = sBody * env, pr = sPres * env;
        if (env <= 0.0f) { ti = bo = pr = 0.0f; sTilt = sBody = sPres = 0.0f; }   // exact neutral
        const size_t k = (size_t) (t & mask);
        tilt[k] = ti;  body[k] = bo;  pres[k] = pr;
        validTo = t + 1;
        if (ti != 0.0f || bo != 0.0f || pr != 0.0f) lastNonZero = t;
    }

    // Whether an engine processing input starting at stream time `blockStart`
    // could still read a non-zero gain (it reads back up to dBound samples).
    // Call AFTER push()ing the segment, so its own writes are counted.
    bool engineNeedsView (int64_t blockStart) const
    {
        return lastNonZero >= blockStart - dBound - 1;
    }

    AvdView view() const
    {
        AvdView v;
        v.tilt = tilt.data();  v.body = body.data();  v.pres = pres.data();
        v.mask = mask;  v.validFrom = validFrom;  v.validTo = validTo;
        return v;
    }

    int capacity() const { return (int) tilt.size(); }

private:
    double fs = 48000.0;
    std::vector<float> tilt, body, pres;
    int64_t mask = 0, stream = 0, validFrom = 0, validTo = 0;
    int64_t lastNonZero = INT64_MIN / 2;
    int   dBound = 0;
    float smK = 0.0f, rampStep = 0.0f, env = 0.0f;
    float sTilt = 0.0f, sBody = 0.0f, sPres = 0.0f;
};

// Per-channel wet-path filter. Coefficients are recomputed every kCadence
// samples of STREAM time (so L and R update on the same instants) from the
// gains read at that instant; no allocation, no JUCE IIR factory.
class AvdFilter
{
public:
    static constexpr int kCadence = 32;

    void prepare (double sampleRate)
    {
        fs = sampleRate > 1000.0 ? sampleRate : 48000.0;
        tiltK = (float) (1.0 - std::exp (-2.0 * M_PI * 1500.0 / fs));   // shelf split 1.5 kHz
        auto pre = [this] (double f, float& c, float& al)
        {
            const double w = 2.0 * M_PI * std::min (f, 0.45 * fs) / fs;
            c  = (float) std::cos (w);
            al = (float) (std::sin (w) / (2.0 * 0.7));                 // Q ~0.7
        };
        pre (400.0,  bodyCos, bodyAl);
        pre (3500.0, presCos, presAl);
        reset();
    }

    void reset()
    {
        lp = 0.0f;  body = Peak {};  pres = Peak {};
        gH = 1.0f;  have = false;
    }

    void idle() { if (have) reset(); }

    // One wet sample at input stream time t. Returns x untouched when every
    // gain is exactly zero (and arranges a clean restart for the next one).
    float apply (float x, const AvdView& v, int64_t t)
    {
        float ti, bo, pr;
        if (! v.read (t, ti, bo, pr))
        {
            if (have) reset();
            return x;
        }
        if (! have || (t % kCadence) == 0)
        {
            gH = std::pow (10.0f, -ti / 20.0f);
            body.set (bo, bodyCos, bodyAl);
            pres.set (pr, presCos, presAl);
            have = true;
        }
        lp += tiltK * (x - lp);
        float y = lp + gH * (x - lp);
        y = body.run (y);
        y = pres.run (y);
        if (! std::isfinite (y)) { reset(); return x; }   // never propagate a blow-up
        return y;
    }

private:
    struct Peak
    {
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        void set (float gDb, float c, float al)
        {
            const float A  = std::pow (10.0f, gDb / 40.0f);
            const float a0 = 1.0f + al / A;
            b0 = (1.0f + al * A) / a0;  b1 = (-2.0f * c) / a0;  b2 = (1.0f - al * A) / a0;
            a1 = b1;                    a2 = (1.0f - al / A) / a0;
        }
        float run (float x)
        {
            const float y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };

    double fs = 48000.0;
    float tiltK = 0.0f, lp = 0.0f, gH = 1.0f;
    float bodyCos = 0, bodyAl = 0, presCos = 0, presAl = 0;
    Peak  body, pres;
    bool  have = false;
};
