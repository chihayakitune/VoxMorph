#pragma once

// VoxMorph -- Auto Voice Protection Gain / Auto Gain Restore (v0.65.0)
//
// A small, self-contained safety stage that sits either side of the voice
// conversion. Dependency-free single-header C++17, same convention as
// PsolaEngine.h / VoiceAnalyzer.h / SpatialEngine.h.
//
//     ... -> Pre FX -> noise gate -> [ processPre ] -> PsolaEngine ->
//                                    [ processPost ] -> mute -> Output Gain -> ...
//
// WHAT IT IS FOR
// A sudden shout, a mic bump or a Pre FX plugin with makeup gain can hand the
// conversion a signal far louder than the material it was tuned on. TD-PSOLA
// is scale-linear in most of its arithmetic, but not all of it: the overlap-add
// normalisation has a floor (accBuf / max(nrm, 0.25)), so wherever the grain
// overlap is thin the output is NOT normalised, and the residue there grows in
// direct proportion to the input amplitude. Holding the engine input inside a
// known window keeps that residue where the engine was voiced for.
//
// WHAT IT IS NOT
// Not a compressor, not an AGC, not a limiter on the output. At normal speaking
// level it is EXACTLY unity -- the per-sample multiply is skipped outright, so
// the samples are bit-identical to a build without this stage. It only engages
// above the threshold, and everything it takes off it puts back on the other
// side of the conversion.
//
// THE ONE THING THAT IS EASY TO GET WRONG
// The engine delays its output by D samples (PsolaEngine::latencySamples(),
// ~43 ms, or ~21 ms in Low Latency Mode). Output sample i came from input
// sample i - D. So the restore CANNOT read the gain that the protection is
// applying right now -- it has to read the gain that was applied D samples ago.
// That is what gainRing is: a delay line for the control signal only, which
// costs no audio latency. Undoing the wrong gain would boost exactly the
// transient the protection had just taken down.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

class ProtectionGain
{
public:
    //==============================================================================
    // Tuning. All times are seconds and are turned into per-sample one-pole
    // coefficients in prepare(), so behaviour is identical at any host buffer
    // size (32 / 64 / 128 / 256 / 512 ...) and scales with the sample rate.

    // Sustained level allowed into the conversion. The knee is continuous:
    // gain is exactly 1.0 at and below the threshold, and holds the short-time
    // envelope AT the threshold above it, so threshold and target peak are the
    // same number by construction (a target below the threshold would put a
    // step in the gain curve at the knee).
    // Set so that the resulting PEAK lands inside the -9..-6 dBFS target
    // window: the gain is driven by the short-time envelope, and a steady tone
    // sits roughly 0.5 dB above its own envelope, so an envelope threshold of
    // -7 dBFS holds the peak at about -6.5 dBFS.
    static constexpr float kThresholdDb   = -7.0f;   // short-time envelope, dBFS

    // Emergency ceiling for the instantaneous peak. The envelope alone would
    // let a very short transient through at full height; this catches it
    // without making the stage sensitive to a single stray sample, because it
    // only starts acting once a peak is actually about to hit full scale.
    static constexpr float kPeakCeilDb    =  0.0f;   // dBFS

    // Never take off more than this, however loud the input is. Bounds the
    // restore on the far side too: the biggest thing it can ever be asked to
    // put back is +24 dB, and the headroom clamp still sits on top of that.
    static constexpr float kMaxReductionDb = 24.0f;

    // Ceiling the restore is not allowed to push the converted signal past.
    // 1 dB under full scale: the stages after the restore (SpatialEngine's
    // Catmull-Rom ITD interpolation, the room reflections, hosted Post FX) can
    // each add a little inter-sample overshoot.
    static constexpr float kRestoreCeilDb = -1.0f;

    static constexpr float kEnvAttackSec  = 0.003f;  // short-time envelope
    static constexpr float kEnvReleaseSec = 0.100f;
    static constexpr float kPeakRelSec    = 0.020f;  // peak follower (instant attack)
    static constexpr float kGainAttackSec = 0.002f;  // protection gain smoothing
    static constexpr float kGainRelSec    = 0.150f;
    static constexpr float kOutPeakRelSec = 0.050f;  // converted-signal peak follower
    static constexpr float kTrimAttackSec = 0.002f;  // restore headroom trim
    static constexpr float kTrimRelSec    = 0.150f;

    // How close to 1.0 a smoother has to get before it is snapped there, which
    // is what puts the stage back on its zero-cost path (no multiply at all).
    //
    // This CANNOT be made arbitrarily small. A one-pole in float32 stalls: the
    // release coefficient is 1.5e-4, so once the remaining error is under about
    // 2e-4 the increment (1.5e-4 * 2e-4 = 3e-8) is below half an ULP at 1.0
    // (~6e-8) and the addition stops changing the value. A snap threshold below
    // that stall point is never reached, and the stage would keep multiplying
    // by 0.9998 forever after the first loud moment. 1e-3 sits safely above it
    // and is 0.0087 dB -- two orders of magnitude under audibility.
    static constexpr float kSnap = 1.0e-3f;

    //==============================================================================
    void prepare (double sampleRate, int maxBlockSamples)
    {
        fs = sampleRate > 0.0 ? sampleRate : 44100.0;

        envAtkK  = onePole (kEnvAttackSec);
        envRelK  = onePole (kEnvReleaseSec);
        pkRelK   = onePole (kPeakRelSec);
        gAtkK    = onePole (kGainAttackSec);
        gRelK    = onePole (kGainRelSec);
        outPkRelK= onePole (kOutPeakRelSec);
        trimAtkK = onePole (kTrimAttackSec);
        trimRelK = onePole (kTrimRelSec);

        thrLin     = dbToGain (kThresholdDb);
        peakCeilLin= dbToGain (kPeakCeilDb);
        minGain    = dbToGain (-kMaxReductionDb);
        restCeilLin= dbToGain (kRestoreCeilDb);

        // The control-signal delay line has to span the engine lookahead plus
        // one block, because processPre() has already advanced the write head
        // by n samples by the time processPost() reads. Sized for the longest
        // lookahead the engine can ask for (0.0427 s) with room to spare, and
        // rounded up to a power of two so the index wrap is a mask.
        const int maxD  = (int) std::ceil (fs * 0.05);
        const int block = std::max (1, maxBlockSamples);
        size_t want = (size_t) (maxD + 2 * block + 16);
        size_t cap  = 1;
        while (cap < want) cap <<= 1;
        gainRing.assign (cap, 1.0f);
        ringMask = cap - 1;

        reset();
    }

    // Every piece of running state, including the control-signal history.
    // Called from prepareToPlay and from the processor's reset().
    void reset()
    {
        env = 0.0f;  pk = 0.0f;  outPk = 0.0f;
        gain = 1.0f; trim = 1.0f;
        writePos = 0;
        lastD = -1;
        blockMinGain = 1.0f;
        std::fill (gainRing.begin(), gainRing.end(), 1.0f);
    }

    // While the conversion is bypassed (Mix at 0 the engine is a pure delay of
    // the dry signal) there is nothing to protect. The stage is not cut dead --
    // that would step the gain -- it is asked for unity and glides out through
    // its normal release, after which the idle path skips the multiply anyway.
    void setBypassed (bool b) noexcept { bypassed = b; }

    // Negative while the stage is holding the input down, 0.0 when idle.
    // For a future "AUTO -3.2 dB" readout; nothing in the DSP reads it.
    float currentGainReductionDb() const noexcept
    {
        return blockMinGain >= 1.0f ? 0.0f
                                    : 20.0f * std::log10 (std::max (blockMinGain, 1.0e-6f));
    }
    float currentProtectionGainDb() const noexcept { return currentGainReductionDb(); }

    //==============================================================================
    // BEFORE the conversion. chans[0..numChans-1] are the buffers about to be
    // handed to the engine(s): one in mono, two in Stereo Input mode. The
    // detector is stereo-linked (max of the absolute values) and a single
    // common gain goes on both, so the stereo image cannot move.
    void processPre (float* const* chans, int numChans, int n)
    {
        if (gainRing.empty() || n <= 0 || numChans <= 0) return;

        float minG = 1.0f;

        for (int i = 0; i < n; ++i)
        {
            // stereo-linked detector input
            float a = 0.0f;
            for (int c = 0; c < numChans; ++c)
            {
                const float v = std::abs (chans[c][i]);
                if (std::isfinite (v)) a = std::max (a, v);
            }

            // Peak: instant attack, short release. Emergency side only.
            pk = a > pk ? a : pk + pkRelK * (a - pk);

            // Short-time envelope: what actually decides the reduction, so a
            // single sample cannot pull the gain down.
            env += (a > env ? envAtkK : envRelK) * (a - env);

            float target = 1.0f;
            if (! bypassed)
            {
                if (env > thrLin)      target = thrLin / env;
                if (pk  > peakCeilLin) target = std::min (target, peakCeilLin / pk);
                target = std::clamp (target, minGain, 1.0f);
                if (! std::isfinite (target)) target = 1.0f;
            }

            gain += (target < gain ? gAtkK : gRelK) * (target - gain);
            if (! std::isfinite (gain)) gain = 1.0f;

            // Snap so that "not doing anything" really is exactly 1.0 and the
            // multiply below can be skipped. See kSnap for why the threshold
            // is 1e-3 and not something tighter.
            if (target >= 1.0f && std::abs (gain - 1.0f) < kSnap) gain = 1.0f;

            gainRing[(size_t) (writePos + (int64_t) i) & ringMask] = gain;

            if (gain != 1.0f)
                for (int c = 0; c < numChans; ++c)
                    chans[c][i] *= gain;

            minG = std::min (minG, gain);
        }

        writePos += n;
        blockMinGain = minG;
    }

    // AFTER the conversion, before anything else touches the signal. Puts back
    // what processPre() took off D samples earlier, never more, and never past
    // the ceiling.
    void processPost (float* const* chans, int numChans, int n, int latencySamples)
    {
        if (gainRing.empty() || n <= 0 || numChans <= 0) return;

        const int D = std::max (0, latencySamples);

        // Lookahead changed under us (Low Latency Mode, or a re-prepare). The
        // history no longer lines up with the output, so seed it with the gain
        // currently in force: no step, and anything it gets wrong is caught by
        // the headroom clamp below.
        if (D != lastD)
        {
            std::fill (gainRing.begin(), gainRing.end(), gain);
            lastD = D;
        }

        // Not enough history for this delay -- can only happen if the host
        // hands us a block far larger than it prepared for. Leave the signal
        // attenuated: under-restoring is the safe direction.
        if ((size_t) (n + D + 1) > gainRing.size()) return;

        const int64_t readBase = writePos - (int64_t) n - (int64_t) D;

        for (int i = 0; i < n; ++i)
        {
            float a = 0.0f;
            for (int c = 0; c < numChans; ++c)
            {
                const float v = std::abs (chans[c][i]);
                if (std::isfinite (v)) a = std::max (a, v);
            }
            outPk = a > outPk ? a : outPk + outPkRelK * (a - outPk);

            const float gDel = gainRing[(size_t) (readBase + (int64_t) i) & ringMask];
            float want = (gDel > 1.0e-6f && std::isfinite (gDel)) ? 1.0f / gDel : 1.0f;
            want = std::clamp (want, 1.0f, 1.0f / minGain);

            // Headroom trim. Only ever pulls the restore back towards unity --
            // it can cancel the compensation but can never attenuate below it,
            // so this is not a limiter on the output.
            const float predicted = outPk * want;
            float trimT = predicted > restCeilLin ? restCeilLin / predicted : 1.0f;
            trimT = std::clamp (trimT, 0.0f, 1.0f);
            if (! std::isfinite (trimT)) trimT = 1.0f;
            trim += (trimT < trim ? trimAtkK : trimRelK) * (trimT - trim);
            if (! std::isfinite (trim)) trim = 1.0f;
            if (trimT >= 1.0f && std::abs (trim - 1.0f) < kSnap) trim = 1.0f;

            float r = want * trim;
            if (! (r > 1.0f)) r = 1.0f;      // also catches NaN

            if (r != 1.0f)
                for (int c = 0; c < numChans; ++c)
                    chans[c][i] *= r;
        }
    }

private:
    float onePole (float tauSec) const
    {
        return 1.0f - std::exp (-1.0f / std::max (1.0f, (float) (tauSec * fs)));
    }
    static float dbToGain (float db) { return std::pow (10.0f, db * 0.05f); }

    double fs = 44100.0;

    float envAtkK = 0.0f, envRelK = 0.0f, pkRelK = 0.0f;
    float gAtkK = 0.0f, gRelK = 0.0f;
    float outPkRelK = 0.0f, trimAtkK = 0.0f, trimRelK = 0.0f;
    float thrLin = 1.0f, peakCeilLin = 1.0f, minGain = 1.0f, restCeilLin = 1.0f;

    float env = 0.0f, pk = 0.0f, outPk = 0.0f;
    float gain = 1.0f, trim = 1.0f;
    float blockMinGain = 1.0f;
    bool  bypassed = false;

    std::vector<float> gainRing;
    size_t  ringMask = 0;
    int64_t writePos = 0;
    int     lastD = -1;
};
