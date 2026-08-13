#pragma once
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

// ===========================================================================
// SpatialEngine (v0.45.0) — the ASMR tab's output stage.
//
// Dependency-free single-header C++17, same rules as PsolaEngine.h: no JUCE,
// no allocation outside prepare(), no branch that depends on the host buffer
// size. It runs AFTER the conversion engine on the finished output, so it
// cannot affect pitch, formants or anything the MAIN tab does — the worst it
// can do is colour the last stage before the speakers.
//
// WHAT IT REPLACES
// Up to v0.44 the ASMR tab was a constant-power pan plus a distance
// attenuation, written inline in processBlock(). That code is still here,
// unchanged and in the same order, as stage 1-2 below; everything added
// around it is skipped outright while its own control is at the neutral
// value. That is a hard requirement, not an optimisation:
//
//   *** With binaural off, air 0, room 0, orbit 0 and width 100 %, the
//   *** output is BIT-IDENTICAL to v0.44. Sessions and presets that predate
//   *** this file therefore sound exactly as they did.
//
// test/spatial_test.cpp checks that with memcmp against a copy of the old
// inline maths, because a listening test cannot see a sample move.
//
// THE STAGES, in the order a sound reaches your ears
//   1. position     pad x/y, optionally rotated by the auto-orbit
//   2. pan + level  constant-power L/R, distance attenuation   (legacy maths)
//   3. ITD          the far ear hears you a fraction of a ms later
//   4. head shadow  the far ear also hears you duller
//   5. dulling      air absorption with distance + the behind-you cue
//   6. room         early reflections and a short diffuse tail
//   7. width        M/S, so the whole picture can be narrowed or widened
//
// WHY THESE AND NOT AN HRTF
// A measured HRTF needs per-listener data and a convolution per ear; what
// actually carries "she is whispering into my left ear" on headphones is the
// interaural TIME difference plus the head's shadow, both of which are a few
// lines of code and stay stable under a moving source. This is deliberately a
// cue model, not an ear model — it is tuned to sound right, and the numbers
// below say where each one came from.
// ===========================================================================
class SpatialEngine
{
public:
    // Everything the stage needs for one block. Values are in the same units
    // the parameters carry, so the caller does no conversion and the defaults
    // here are literally the neutral (= legacy) setting.
    struct Params
    {
        float x = 0.0f, y = 0.0f;   // pad position, -1..1 (y > 0 = in front)
        float distanceP  = 100.0f;  // % of the classic distance attenuation
        float airP       = 0.0f;    // % air absorption over distance
        bool  binaural   = false;   // ITD + head shadow + behind-you cue
        float roomP      = 0.0f;    // % room ambience (0 = no reverb at all)
        float sizeP      = 50.0f;   // % room size
        float widthP     = 100.0f;  // % stereo width (100 = untouched)
        float orbitHz    = 0.0f;    // auto-orbit rate, 0 = off
        float orbitDepthP = 60.0f;  // % orbit radius when the pad is centred
    };

    void prepare (double sampleRate, int /*maxBlock*/)
    {
        sr = sampleRate > 0.0 ? (float) sampleRate : 48000.0f;

        // ITD line: the longest delay we can ask for is kItdMs, plus the
        // three samples Catmull-Rom reads around the fractional position.
        itdLen = nextPow2 ((int) std::ceil (kItdMs * 0.001f * sr) + 8);
        for (auto* d : { &itdL, &itdR }) d->assign ((size_t) itdLen, 0.0f);

        // Room: every line is sized for the LARGEST size setting, and the
        // read offset moves inside it. Sizing per block would mean allocating
        // on the audio thread the first time somebody turns the knob.
        erLen = nextPow2 ((int) std::ceil (kErMaxMs * 0.001f * sr) + 4);
        erBuf.assign ((size_t) erLen, 0.0f);
        for (int c = 0; c < 2; ++c)
        {
            // + kSpreadSamples: the right side reads that much further back,
            // and a line that cannot hold its own longest read would be
            // silently shortened by clampDelay() into a different room.
            for (int i = 0; i < kCombs; ++i)
            {
                const int n = nextPow2 ((int) std::ceil (kCombMs[i] * kSizeMax * 0.001f * sr)
                                          + kSpreadSamples + 4);
                comb[c][i].buf.assign ((size_t) n, 0.0f);
                comb[c][i].mask = n - 1;
            }
            for (int i = 0; i < kAps; ++i)
            {
                const int n = nextPow2 ((int) std::ceil (kApMs[i] * kSizeMax * 0.001f * sr)
                                          + kSpreadSamples + 4);
                ap[c][i].buf.assign ((size_t) n, 0.0f);
                ap[c][i].mask = n - 1;
            }
        }
        reset();
    }

    // Clears every filter, delay line and smoother. Called from prepareToPlay
    // so a device restart never resumes from a stale tail or a half-faded pan.
    void reset()
    {
        panL = panR = 1.0f;
        smItdL = smItdR = 0.0f;
        smShadL = smShadR = 0.0f;
        smAir = smBack = 0.0f;
        smWet = 0.0f;
        orbitPhase = 0.0f;
        liveX = liveY = 0.0f;

        std::fill (itdL.begin(), itdL.end(), 0.0f);
        std::fill (itdR.begin(), itdR.end(), 0.0f);
        itdPos = 0;
        std::fill (erBuf.begin(), erBuf.end(), 0.0f);
        erPos = 0;
        for (int c = 0; c < 2; ++c)
        {
            shadZ[c] = airZ[c] = backZ[c] = 0.0f;
            for (int i = 0; i < kCombs; ++i)
            {
                std::fill (comb[c][i].buf.begin(), comb[c][i].buf.end(), 0.0f);
                comb[c][i].pos = 0;  comb[c][i].z = 0.0f;
            }
            for (int i = 0; i < kAps; ++i)
            {
                std::fill (ap[c][i].buf.begin(), ap[c][i].buf.end(), 0.0f);
                ap[c][i].pos = 0;
            }
        }
    }

    // True while nothing in the extended set is engaged. The caller does not
    // need this (process() skips each stage on its own), but the UI uses it
    // to say "this page is doing nothing right now".
    static bool isNeutral (const Params& p)
    {
        return ! p.binaural
            && p.airP  <= 0.0f
            && p.roomP <= 0.0f
            && p.orbitHz <= 0.0f
            && std::abs (p.widthP - 100.0f) < 1.0e-6f;
    }

    // Where the source actually is right now, orbit included, for the pad to
    // draw. Written every block; read by the UI thread through an atomic.
    float currentX() const { return liveX; }
    float currentY() const { return liveY; }

    // ---- the stage -------------------------------------------------------
    // srcL/srcR are the finished mono or stereo output; dst may alias them.
    // `stereo` false means the host gave us a mono bus: there is no L/R to
    // place a source between, so pan, ITD and width are skipped and only the
    // things that survive a single speaker (level, dulling, room) run.
    void process (const float* srcL, const float* srcR,
                  float* dstL, float* dstR, int n, const Params& pIn)
    {
        const bool stereo = dstR != nullptr && srcR != nullptr;
        const Params p = sanitize (pIn);

        // ---- 1. position, with the auto-orbit ---------------------------
        float x = p.x, y = p.y;
        if (p.orbitHz > 0.0f)
        {
            // The orbit ROTATES the pad position around your head. With the
            // pad centred there is no vector to rotate, so Depth stands in as
            // the radius — that is why a centred pad still circles, and why
            // pulling the dot further out than Depth wins.
            const float r0 = std::sqrt (p.x * p.x + p.y * p.y);
            const float r  = std::max (r0, p.orbitDepthP * 0.01f);
            // atan2(0,0) is 0, which puts a centred start straight in front.
            const float a0 = std::atan2 (p.x, p.y);
            const float a  = a0 + kTwoPi * orbitPhase;
            x = r * std::sin (a);
            y = r * std::cos (a);
            orbitPhase += p.orbitHz * (float) n / sr;
            orbitPhase -= std::floor (orbitPhase);      // keep it in [0,1)
        }
        liveX = x;  liveY = y;

        // ---- 2. pan + distance level (the v0.44 maths, untouched) -------
        const float dist = std::min (1.0f, std::sqrt (x * x + y * y));
        float dg = 1.0f - 0.6f * (p.distanceP * 0.01f) * dist;
        // Behind-you also loses a little level: a source at the back of the
        // head is shadowed by it from both sides, not just filtered.
        if (p.binaural && y < 0.0f) dg *= 1.0f - 0.16f * std::min (1.0f, -y);

        const float panPhase = (x + 1.0f) * 0.25f * kPi;
        const float tL = dg * std::cos (panPhase) * kSqrt2;
        const float tR = dg * std::sin (panPhase) * kSqrt2;

        // The legacy "everything is exactly unity, so this is a copy" test.
        // It still decides the fast path, and the extended stages below add
        // their own gates rather than weakening this one.
        const bool panNeutral = (x == 0.0f && y == 0.0f)
                             && std::abs (panL - 1.0f) < 1.0e-6f
                             && std::abs (panR - 1.0f) < 1.0e-6f;
        if (panNeutral) { panL = 1.0f; panR = 1.0f; }

        if (stereo)
        {
            if (panNeutral)
            {
                if (dstL != srcL) std::copy (srcL, srcL + n, dstL);
                if (dstR != srcR) std::copy (srcR, srcR + n, dstR);
            }
            else
                for (int i = 0; i < n; ++i)
                {
                    panL += kSmooth * (tL - panL);
                    panR += kSmooth * (tR - panR);
                    dstL[i] = panL * srcL[i];
                    dstR[i] = panR * srcR[i];
                }
        }
        else
        {
            if (panNeutral)
            {
                if (dstL != srcL) std::copy (srcL, srcL + n, dstL);
            }
            else
                for (int i = 0; i < n; ++i)
                {
                    panL += kSmooth * (dg - panL);
                    dstL[i] = panL * srcL[i];
                }
        }

        // ---- 3. ITD + 4. head shadow ------------------------------------
        // Both are lateral cues, so both are skipped on a mono bus and while
        // Binaural Cues is off. Targets are recomputed per block and reached
        // with the same one-pole the pan uses, so dragging the dot never
        // steps the delay (a 32-sample jump spread over ~10 ms is a 0.3 %
        // rate change — inaudible, and the reason this is not a hard set).
        {
            // Woodworth: itd = (r/c)(theta + sin theta), head radius 8.75 cm,
            // c = 343 m/s -> 0.66 ms at full lateral. theta is taken from the
            // pad's x alone: |x| = 1 is "at the ear", i.e. 90 degrees.
            const float th = std::asin (std::min (1.0f, std::abs (x))) ;
            const float itdS = p.binaural && stereo
                                 ? kHeadR / kSpeedC * (th + std::sin (th)) * sr : 0.0f;
            const float tItdL = x > 0.0f ? itdS : 0.0f;    // source right -> left ear late
            const float tItdR = x < 0.0f ? itdS : 0.0f;
            // The far ear is also in the head's acoustic shadow. One pole at
            // 2.2 kHz, blended in rather than replacing the signal, so the
            // near ear stays open and only the far one goes dull.
            const float shad  = p.binaural && stereo ? 0.72f * std::abs (x) : 0.0f;
            const float tShL  = x > 0.0f ? shad : 0.0f;
            const float tShR  = x < 0.0f ? shad : 0.0f;

            const bool run = p.binaural && stereo;
            const bool settled = nearZero (smItdL) && nearZero (smItdR)
                              && nearZero (smShadL) && nearZero (smShadR);
            if (run || ! settled)      // keep running while it fades back out
            {
                const float aSh = onePoleA (2200.0f);
                for (int i = 0; i < n; ++i)
                {
                    smItdL += kSmooth * (tItdL - smItdL);
                    smItdR += kSmooth * (tItdR - smItdR);
                    smShadL += kSmooth * (tShL - smShadL);
                    smShadR += kSmooth * (tShR - smShadR);

                    itdL[(size_t) itdPos] = dstL[i];
                    itdR[(size_t) itdPos] = dstR[i];
                    float l = readDelay (itdL, itdPos, smItdL);
                    float r = readDelay (itdR, itdPos, smItdR);
                    itdPos = (itdPos + 1) & (itdLen - 1);

                    shadZ[0] += aSh * (l - shadZ[0]);
                    shadZ[1] += aSh * (r - shadZ[1]);
                    dstL[i] = l + smShadL * (shadZ[0] - l);
                    dstR[i] = r + smShadR * (shadZ[1] - r);
                }
            }
        }

        // ---- 5. dulling: air absorption, then the behind-you cue ---------
        // Both roll the top end off, but they are NOT the same filter and
        // sharing one was the first version's mistake. Air absorption is a
        // gentle slope that starts high and grows with distance; the pinna's
        // front/back difference is a much lower, deeper shelf. Sharing 3.8 kHz
        // left "behind you" worth only 2.6 dB at 6 kHz (measured with the
        // tone probe in test/spatial_test.cpp) — inside the range one voice
        // varies by, so it read as a level change rather than a direction.
        //
        // Air runs on any bus: distance dulls a sound on one speaker as much
        // as on two. The back cue is a head/ear effect and follows Binaural.
        {
            const float tAir = std::min (0.95f, p.airP * 0.01f * dist * 0.9f);
            if (tAir > 0.0f || ! nearZero (smAir))
                dull (dstL, stereo ? dstR : nullptr, n, tAir, 3800.0f, smAir, airZ);

            const float tBack = p.binaural && y < 0.0f ? 0.70f * std::min (1.0f, -y) : 0.0f;
            if (tBack > 0.0f || ! nearZero (smBack))
                dull (dstL, stereo ? dstR : nullptr, n, tBack, 2200.0f, smBack, backZ);
        }

        // ---- 6. room ------------------------------------------------------
        {
            // Distance feeds the room as well as the level: stepping back in
            // a real room raises the reverberant share, and that ratio is
            // most of what "further away" sounds like indoors.
            const float tWet = p.roomP * 0.01f * (0.55f + 0.45f * dist) * kWetMax;
            if (tWet > 0.0f || ! nearZero (smWet))
                room (dstL, stereo ? dstR : nullptr, n, tWet, p.sizeP * 0.01f);
        }

        // ---- 7. width -----------------------------------------------------
        // Skipped at exactly 100 %: M/S there is algebraically the identity
        // but NOT bit-exact in float, and this stage must not disturb a
        // session that never touched it.
        if (stereo && std::abs (p.widthP - 100.0f) > 1.0e-6f)
        {
            const float w = p.widthP * 0.01f;
            for (int i = 0; i < n; ++i)
            {
                const float m = 0.5f * (dstL[i] + dstR[i]);
                const float s = 0.5f * (dstL[i] - dstR[i]) * w;
                dstL[i] = m + s;
                dstR[i] = m - s;
            }
        }
    }

private:
    // ---- tuning constants ------------------------------------------------
    static constexpr float kPi    = 3.14159265358979323846f;
    static constexpr float kTwoPi = 2.0f * kPi;
    static constexpr float kSqrt2 = 1.41421356237309504880f;
    // the smoother every target uses. 0.002 per sample is what the output
    // gain and the old pan already used; sharing it keeps the whole output
    // stage moving at one speed (~10 ms at 48 kHz).
    static constexpr float kSmooth = 0.002f;
    static constexpr float kHeadR  = 0.0875f;   // m, standard head radius
    static constexpr float kSpeedC = 343.0f;    // m/s
    static constexpr float kItdMs  = 1.0f;      // line length; 0.66 ms is used
    static constexpr int   kItdBase = 1;        // see readDelay()
    static constexpr float kWetMax = 0.42f;     // room at 100 % is still a room

    static constexpr int   kCombs = 4, kAps = 2;
    // Schroeder/Freeverb-style delays in ms at size 100 %, mutually prime so
    // the comb resonances do not pile up on the same frequencies.
    static constexpr float kCombMs[kCombs] = { 29.7f, 37.1f, 41.1f, 43.7f };
    static constexpr float kApMs[kAps]     = { 5.0f, 1.7f };
    static constexpr float kSizeMax = 1.0f;     // sizeP 100 % = the values above
    static constexpr float kSizeMin = 0.32f;    // sizeP 0 %  = a small booth
    static constexpr float kErMaxMs = 60.0f;

    struct Line
    {
        std::vector<float> buf;
        int   pos = 0, mask = 0;
        float z = 0.0f;              // comb damping state (unused for allpass)
    };

    static int nextPow2 (int v)
    {
        int n = 1;
        while (n < v) n <<= 1;
        return n;
    }
    static bool nearZero (float v) { return std::abs (v) < 1.0e-6f; }

    float onePoleA (float hz) const
    {
        return 1.0f - std::exp (-kTwoPi * hz / sr);
    }

    static Params sanitize (const Params& in)
    {
        Params p = in;
        auto fin = [] (float v, float lo, float hi)
        {
            if (! std::isfinite (v)) return lo;
            return std::min (hi, std::max (lo, v));
        };
        p.x = fin (p.x, -1.0f, 1.0f);
        p.y = fin (p.y, -1.0f, 1.0f);
        p.distanceP   = fin (p.distanceP,   0.0f, 200.0f);
        p.airP        = fin (p.airP,        0.0f, 100.0f);
        p.roomP       = fin (p.roomP,       0.0f, 100.0f);
        p.sizeP       = fin (p.sizeP,       0.0f, 100.0f);
        p.widthP      = fin (p.widthP,      0.0f, 200.0f);
        p.orbitHz     = fin (p.orbitHz,     0.0f, 2.0f);
        p.orbitDepthP = fin (p.orbitDepthP, 0.0f, 100.0f);
        return p;
    }

    // Fractional read, `kItdBase + d` samples behind the sample just written
    // at `pos`. Catmull-Rom, for the same reason the Air comb uses it: linear
    // interpolation loses enough high end that a moving delay audibly sweeps
    // its own tone alongside the position.
    //
    // kItdBase is why the whole line is offset by one sample: the curve needs
    // the point BEFORE the segment it interpolates, and at delay 0 that point
    // is one sample into the future. Both ears carry the same base offset, so
    // it shifts nothing between them — it just costs one sample of latency
    // while Binaural Cues is on.
    float readDelay (const std::vector<float>& b, int pos, float d) const
    {
        const int   mask = itdLen - 1;
        const float dd   = kItdBase + std::max (0.0f, std::min ((float) (itdLen - 4), d));
        const int   i0   = (int) dd;
        const float t    = dd - (float) i0;
        auto at = [&] (int k) { return b[(size_t) ((pos - k) & mask)]; };
        const float pm1 = at (i0 - 1), p0 = at (i0), p1 = at (i0 + 1), p2 = at (i0 + 2);
        return 0.5f * ((2.0f * p0)
                     + (-pm1 + p1) * t
                     + (2.0f * pm1 - 5.0f * p0 + 4.0f * p1 - p2) * t * t
                     + (-pm1 + 3.0f * p0 - 3.0f * p1 + p2) * t * t * t);
    }

    // One shelving-by-blend stage: y = x + m*(lowpass(x) - x). At m = 0 it is
    // the identity (which is why every caller can leave it running until its
    // smoother has actually reached zero rather than cutting it off mid-fade
    // and stepping the signal).
    void dull (float* dL, float* dR, int n, float target, float hz,
               float& sm, float (&z)[2]) const
    {
        const float a = onePoleA (hz);
        for (int i = 0; i < n; ++i)
        {
            sm += kSmooth * (target - sm);
            z[0] += a * (dL[i] - z[0]);
            dL[i] += sm * (z[0] - dL[i]);
            if (dR != nullptr)
            {
                z[1] += a * (dR[i] - z[1]);
                dR[i] += sm * (z[1] - dR[i]);
            }
        }
    }

    // Early reflections + a short diffuse tail, added on top of the dry
    // signal. Deliberately small: an ASMR room is a bedroom, not a hall, so
    // the tail is short and damped and the wet share tops out well under half.
    void room (float* dL, float* dR, int n, float tWet, float size01)
    {
        const float scale = kSizeMin + (kSizeMax - kSizeMin) * size01;
        const float rt60  = 0.28f + 0.9f * size01;
        const float damp  = onePoleA (4200.0f);

        // comb feedback for the wanted decay, capped short of instability
        float fb[kCombs];
        for (int i = 0; i < kCombs; ++i)
        {
            const float d = kCombMs[i] * scale * 0.001f;
            fb[i] = std::min (0.86f, std::exp (-6.9078f * d / rt60));
        }
        int combD[2][kCombs], apD[2][kAps];
        for (int c = 0; c < 2; ++c)
        {
            for (int i = 0; i < kCombs; ++i)
                combD[c][i] = clampDelay ((int) (kCombMs[i] * scale * 0.001f * sr)
                                            + (c == 1 ? kSpreadSamples : 0), comb[c][i].mask);
            for (int i = 0; i < kAps; ++i)
                apD[c][i] = clampDelay ((int) (kApMs[i] * scale * 0.001f * sr)
                                          + (c == 1 ? kSpreadSamples / 2 : 0), ap[c][i].mask);
        }
        // early taps, ms and gain. L and R read different taps: identical
        // early reflections on both sides collapse to a mono ghost in the
        // middle, which is the opposite of what this page is for.
        static constexpr float erMsL[kErTaps] = {  7.3f, 13.1f, 21.7f, 33.4f };
        static constexpr float erMsR[kErTaps] = {  9.9f, 16.5f, 26.3f, 37.9f };
        static constexpr float erG  [kErTaps] = { 0.72f, 0.55f, 0.41f, 0.29f };
        int erdL[kErTaps], erdR[kErTaps];
        for (int i = 0; i < kErTaps; ++i)
        {
            erdL[i] = clampDelay ((int) (erMsL[i] * scale * 0.001f * sr), erLen - 1);
            erdR[i] = clampDelay ((int) (erMsR[i] * scale * 0.001f * sr), erLen - 1);
        }

        for (int i = 0; i < n; ++i)
        {
            smWet += kSmooth * (tWet - smWet);
            const float dry0 = dL[i];
            const float dry1 = dR != nullptr ? dR[i] : dL[i];
            const float in = 0.5f * (dry0 + dry1);

            erBuf[(size_t) erPos] = in;
            float eL = 0.0f, eR = 0.0f;
            for (int t = 0; t < kErTaps; ++t)
            {
                eL += erG[t] * erBuf[(size_t) ((erPos - erdL[t]) & (erLen - 1))];
                eR += erG[t] * erBuf[(size_t) ((erPos - erdR[t]) & (erLen - 1))];
            }
            erPos = (erPos + 1) & (erLen - 1);

            float wet[2] = { 0.0f, 0.0f };
            const float feed[2] = { 0.34f * (in + 0.5f * eL), 0.34f * (in + 0.5f * eR) };
            for (int c = 0; c < 2; ++c)
            {
                if (c == 1 && dR == nullptr) break;
                float acc = 0.0f;
                for (int k = 0; k < kCombs; ++k)
                {
                    auto& ln = comb[c][k];
                    const int rd = (ln.pos - combD[c][k]) & ln.mask;
                    const float out = ln.buf[(size_t) rd];
                    ln.z += damp * (out - ln.z);                 // damped loop
                    ln.buf[(size_t) ln.pos] = feed[c] + fb[k] * ln.z;
                    ln.pos = (ln.pos + 1) & ln.mask;
                    acc += out;
                }
                acc *= 0.25f;
                for (int k = 0; k < kAps; ++k)                   // diffusion
                {
                    auto& ln = ap[c][k];
                    const int rd = (ln.pos - apD[c][k]) & ln.mask;
                    const float bufOut = ln.buf[(size_t) rd];
                    const float v = acc + 0.5f * bufOut;
                    ln.buf[(size_t) ln.pos] = v;
                    ln.pos = (ln.pos + 1) & ln.mask;
                    acc = bufOut - 0.5f * v;
                }
                wet[c] = 0.55f * (c == 0 ? eL : eR) + acc;
            }

            // Denormals in a decaying tail cost more than the tail is worth.
            wet[0] = flush (wet[0]);
            wet[1] = flush (wet[1]);
            dL[i] = dry0 + smWet * wet[0];
            if (dR != nullptr) dR[i] = dry1 + smWet * wet[1];
        }
    }

    static int clampDelay (int d, int mask) { return std::max (1, std::min (mask, d)); }
    static float flush (float v)
    {
        return (std::abs (v) < 1.0e-20f || ! std::isfinite (v)) ? 0.0f : v;
    }

    static constexpr int kErTaps = 4;
    static constexpr int kSpreadSamples = 23;   // Freeverb's stereo spread

    float sr = 48000.0f;

    // smoothed targets
    float panL = 1.0f, panR = 1.0f;
    float smItdL = 0.0f, smItdR = 0.0f;
    float smShadL = 0.0f, smShadR = 0.0f;
    float smAir = 0.0f, smBack = 0.0f, smWet = 0.0f;
    float orbitPhase = 0.0f;
    float liveX = 0.0f, liveY = 0.0f;

    // filter states
    float shadZ[2] = { 0.0f, 0.0f };
    float airZ[2]  = { 0.0f, 0.0f };
    float backZ[2] = { 0.0f, 0.0f };

    // delay memory
    std::vector<float> itdL, itdR;
    int itdLen = 0, itdPos = 0;
    std::vector<float> erBuf;
    int erLen = 0, erPos = 0;
    Line comb[2][kCombs], ap[2][kAps];
};
