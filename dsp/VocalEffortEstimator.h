#pragma once

// VoxMorph -- Vocal Effort Estimator (Adaptive Voice Dynamics, Phase 0-2)
//
// Dependency-free single-header C++17, same convention as ProtectionGain.h.
//
// WHAT IT ESTIMATES
// A baseline-relative approximation of "how much harder than usual is this
// speaker pushing right now", as a continuous 0..1 value, plus how far the
// Body (~180-700 Hz) and Presence (~2.5-6 kHz) bands stand above the
// speaker's own normal balance. It is NOT a measurement of physiological
// vocal effort: it compares the current level / spectral tilt / crest factor
// with a baseline learned from the speaker's normal voiced speech. If the
// first seconds after enabling are a shout, the shout becomes the baseline --
// there is no way to tell from the signal alone, so the UI asks for a few
// seconds of normal speech first.
//
// COST
// No FFT. Per channel and sample: 8 biquads (HP+LP per band) and a few
// one-pole smoothers. Everything above that (baseline, effort, excess) runs
// at a fixed 32-sample control cadence counted from the stream, so the
// result does not depend on the host buffer size.
//
// STEREO
// Band energies are computed PER CHANNEL and the energies are averaged (the
// peak is the max). Detecting on the signed L+R sum would cancel an
// out-of-phase pair and read a loud voice as silence.
//
// TIMING
// It reads the signal after the noise gate and BEFORE Auto Protection, so the
// level it sees is never the protected one. Its own peak follower also decides
// when the input is in Protection's territory or clipping, from the SAME
// samples -- so "do not learn the baseline while Protection is acting" needs
// no gain information from another stage and cannot be misaligned in time.
//
// That gate used to be a flat peak >= -8 dBFS, which was wrong in both
// directions and stopped baseline warmup on ordinary loud-ish speech.
// ProtectionGain does not act on the peak at all: it acts when its short-time
// ENVELOPE passes kThresholdDb (-7 dBFS), which for voiced material means a
// peak comfortably above that. The gate is now derived from Protection's own
// constant with a small margin, so a -7.5 dBFS peak -- well inside normal
// speech -- can still complete warmup, and retuning Protection moves this with
// it instead of leaving the two silently disagreeing.

#include "ProtectionGain.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#ifndef M_PI
#define M_PI 3.14159265358979323846   // MSVC (Windows) では未定義のため
#endif

class VocalEffortEstimator
{
public:
    static constexpr int kCadence = 32;           // control-rate step (samples)

    struct Output
    {
        float effort   = 0.0f;   // 0..1, attack/release smoothed, gated by confidence & warmup
        float bodyEx   = 0.0f;   // 0..1, positive Body excess over baseline (0 = none)
        float presEx   = 0.0f;   // 0..1, positive Presence excess over baseline
        float warm     = 0.0f;   // 0..1, how much of the baseline has been collected
        float conf     = 0.0f;   // 0..1, voicing-likeness of the current frame
    };

    void prepare (double sampleRate)
    {
        fs = sampleRate > 1000.0 ? sampleRate : 48000.0;
        auto opK = [this] (double tau) { return (float) (1.0 - std::exp (-1.0 / (tau * fs))); };
        envK    = opK (0.025);                       // 25 ms band energy smoothing
        peakRel = opK (0.025);
        const double cfs = fs / kCadence;            // control-rate coefficients
        auto cK = [cfs] (double tau) { return (float) (1.0 - std::exp (-1.0 / (tau * cfs))); };
        atkK     = cK (0.025);                       // effort attack  ~25 ms
        relK     = cK (0.220);                       // effort release ~220 ms
        baseK    = cK (10.0);                        // baseline tracking ~10 s
        exK      = cK (0.050);                       // band-excess smoothing
        warmNeed = (float) (2.0 * fs);               // ~2 s of accepted voiced input

        for (int c = 0; c < 2; ++c)
        {
            lo[c].hp.hp (150.0, fs);   lo[c].lp.lp (1000.0, fs);
            hi[c].hp.hp (2000.0, fs);  hi[c].lp.lp (6000.0, fs);
            bo[c].hp.hp (180.0, fs);   bo[c].lp.lp (700.0, fs);
            pr[c].hp.hp (2500.0, fs);  pr[c].lp.lp (6000.0, fs);
        }
        resetAll();
    }

    // Everything, baseline included (prepare / sample-rate change).
    void resetAll()
    {
        // Baseline first: resetSignalState() republishes `warm` from it, so
        // clearing it afterwards would leave the readout claiming a warmup
        // that no longer exists.
        bValid = false;  warmAcc = 0.0f;
        bLevel = bTilt = bCrest = bBody = bPres = 0.0f;
        learning = true;
        resetSignalState();
    }

    // Envelopes, filters, effort and cadence -- the baseline is kept. Used on
    // host reset, stereo/latency changes and re-enable: the speaker and the
    // microphone are the same, only the time line has broken.
    void resetSignalState()
    {
        for (int c = 0; c < 2; ++c)
        {
            lo[c].clear(); hi[c].clear(); bo[c].clear(); pr[c].clear();
            eFull[c] = eLo[c] = eHi[c] = eBody[c] = ePres[c] = 0.0f;
            peak[c] = 0.0f;  zc[c] = 0;  prevSign[c] = 0.0f;
        }
        zcr = 0.0f;
        clipHold = 0;
        phase = 0;
        out = {};
        // The baseline survives this call, so the warmup readout has to as
        // well: zeroing it would make the UI announce "learning your voice"
        // after something as small as a Low Latency switch, and the user would
        // be told to speak normally again for a baseline that is already there.
        out.warm = bValid ? 1.0f : std::min (1.0f, warmNeed > 0.0f ? warmAcc / warmNeed : 0.0f);
    }

    // Feed n samples of 1 or 2 channels (read only). `perSample` is called
    // once per sample with the current (control-rate held) Output, so the
    // caller can store it at that sample's stream time.
    template <typename Fn>
    void process (const float* const* ch, int numCh, int n, Fn&& perSample)
    {
        numCh = std::clamp (numCh, 1, 2);
        for (int i = 0; i < n; ++i)
        {
            for (int c = 0; c < numCh; ++c)
            {
                float x = ch[c][i];
                // NaN/Inf is not the only way to wreck this. A FINITE 1e20
                // squares to +inf, and one such sample sticks in eFull for
                // good -- every later frame then reads level = inf and the
                // estimator never recovers. Clamp before anything squares or
                // enters a filter.
                if (! std::isfinite (x)) x = 0.0f;
                x = std::clamp (x, -kMaxIn, kMaxIn);
                const float x2 = x * x;
                eFull[c] += envK * (x2 - eFull[c]);
                const float yl = lo[c].run (x), yh = hi[c].run (x);
                const float yb = bo[c].run (x), yp = pr[c].run (x);
                eLo[c]   += envK * (yl * yl - eLo[c]);
                eHi[c]   += envK * (yh * yh - eHi[c]);
                eBody[c] += envK * (yb * yb - eBody[c]);
                ePres[c] += envK * (yp * yp - ePres[c]);
                const float a = std::abs (x);
                peak[c] = a > peak[c] ? a : peak[c] + peakRel * (a - peak[c]);
                if (a >= kClipLin) clipHold = (int) (0.2 * fs);
                const float s = x > 0.0f ? 1.0f : (x < 0.0f ? -1.0f : prevSign[c]);
                if (s != prevSign[c]) ++zc[c];
                prevSign[c] = s;
            }
            if (clipHold > 0) --clipHold;

            if (++phase >= kCadence)
            {
                phase = 0;
                controlStep (numCh);
            }
            perSample (out);
        }
    }

    const Output& current() const { return out; }

private:
    static constexpr float kEps     = 1.0e-10f;
    static constexpr float kClipLin = 0.99f;

    // Peak at/above which Protection is taken to be acting, so the baseline
    // stops learning. Derived from Protection's own envelope threshold plus a
    // margin: the peak of voiced material sits above its envelope, so gating
    // at the bare threshold would stop learning before Protection does
    // anything. +1 dB keeps -7.5 dBFS speech learnable and still stops before
    // a steady tone at this peak would push Protection's envelope over.
    static constexpr float kProtPeakMarginDb = 1.0f;
    static constexpr float kProtDb = ProtectionGain::kThresholdDb + kProtPeakMarginDb;

    // Anything louder than this is not a voice; it is a broken host, a bad
    // file or a feedback squeal. Squaring +24 dBFS already reaches 256, and a
    // genuinely wild finite value (1e20) would square to +inf and poison every
    // envelope permanently. Clamping costs one min/max per sample and keeps
    // the whole estimator in a range its filters are stable over.
    static constexpr float kMaxIn = 16.0f;       // +24 dBFS

    struct Biquad
    {
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        void set (double w, bool high)
        {
            const double c = std::cos (w), al = std::sin (w) / (2.0 * 0.7071);
            const double a0 = 1.0 + al;
            const double bb = high ? (1.0 + c) * 0.5 : (1.0 - c) * 0.5;
            b0 = (float) (bb / a0);  b1 = (float) ((high ? -(1.0 + c) : (1.0 - c)) / a0);  b2 = b0;
            a1 = (float) (-2.0 * c / a0);  a2 = (float) ((1.0 - al) / a0);
        }
        void hp (double f, double sr) { set (2.0 * M_PI * std::min (f, 0.45 * sr) / sr, true); }
        void lp (double f, double sr) { set (2.0 * M_PI * std::min (f, 0.45 * sr) / sr, false); }
        float run (float x)
        {
            const float y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
        void clear() { z1 = z2 = 0; }
    };
    struct Band
    {
        Biquad hp, lp;
        float run (float x) { return lp.run (hp.run (x)); }
        void clear() { hp.clear(); lp.clear(); }
    };

    static float db (float e) { return 10.0f * std::log10 (e + kEps); }
    static float ramp (float x, float a, float b) { return std::clamp ((x - a) / (b - a), 0.0f, 1.0f); }

    // Belt and braces: the clamp above should make this impossible, but a
    // filter state that has gone non-finite would otherwise never come back,
    // and "the estimator is silently dead" is the worst failure mode here.
    // Checked once per control step, not per sample.
    bool signalStateFinite() const
    {
        for (int c = 0; c < 2; ++c)
            if (! std::isfinite (eFull[c]) || ! std::isfinite (eLo[c])
             || ! std::isfinite (eHi[c])   || ! std::isfinite (eBody[c])
             || ! std::isfinite (ePres[c]) || ! std::isfinite (peak[c]))
                return false;
        return true;
    }

    void controlStep (int numCh)
    {
        if (! signalStateFinite())
        {
            // Drop the poisoned envelopes and the filter memories. The
            // baseline is a property of the speaker, not of this accident, so
            // it survives -- the estimator resumes from the next good frame.
            resetSignalState();
            return;
        }
        float Ef = 0, El = 0, Eh = 0, Eb = 0, Ep = 0, Pk = 0;
        int   zcs = 0;
        for (int c = 0; c < numCh; ++c)
        {
            Ef += eFull[c];  El += eLo[c];  Eh += eHi[c];  Eb += eBody[c];  Ep += ePres[c];
            Pk = std::max (Pk, peak[c]);
            zcs += zc[c];  zc[c] = 0;
        }
        const float inv = 1.0f / (float) numCh;
        Ef *= inv; El *= inv; Eh *= inv; Eb *= inv; Ep *= inv;
        zcr += 0.2f * ((float) zcs / (float) (kCadence * numCh) - zcr);   // ~5 steps

        const float level = db (Ef);
        const float tilt  = db (El) - db (Eh);                // + = darker
        const float peakDb = 20.0f * std::log10 (Pk + 1.0e-9f);
        const float crest = peakDb - level;                   // peak over RMS, dB
        const float body  = db (Eb) - level;                  // band share, gain-invariant
        const float pres  = db (Ep) - level;

        // Voicing-likeness proxy (the engine's own detector runs per channel,
        // a block late and after Protection, so it is not used here). Voiced
        // speech: audible, most energy below 1 kHz, few zero crossings, not an
        // impulse. Breath / fricatives fail the low-band share or the ZCR,
        // clicks and bumps fail the crest test; silence fails the level.
        const float loShare = El / (Ef + kEps);
        const float zcrMax  = (float) (3000.0 / fs);          // ~1.5 kHz "frequency"
        const float conf = ramp (level, -62.0f, -52.0f)
                         * ramp (loShare, 0.15f, 0.35f)
                         * (1.0f - ramp (zcr, zcrMax, 2.0f * zcrMax))
                         * (1.0f - ramp (crest, 18.0f, 26.0f));
        out.conf = conf;

        const bool clip    = clipHold > 0;
        const bool protect = peakDb >= kProtDb;

        // ---- baseline ---------------------------------------------------
        const bool goodFrame = conf > 0.6f && ! clip && ! protect;
        if (! bValid)
        {
            // warmup: running mean over accepted voiced frames
            if (goodFrame)
            {
                const float w = 1.0f / (1.0f + warmAcc / kCadence);
                bLevel += w * (level - bLevel);  bTilt += w * (tilt - bTilt);
                bCrest += w * (crest - bCrest);  bBody += w * (body - bBody);
                bPres  += w * (pres - bPres);
                warmAcc += (float) kCadence;
                if (warmAcc >= warmNeed) bValid = true;
            }
        }
        else if (goodFrame)
        {
            // Slow tracking only inside the normal range and only while the
            // effort is low. Hysteresis: stop learning above 0.30, resume
            // below 0.15, so a sustained loud passage never becomes "normal".
            if (learning && out.effort > 0.30f) learning = false;
            if (! learning && out.effort < 0.15f) learning = true;
            if (learning && std::abs (level - bLevel) < 6.0f)
            {
                bLevel += baseK * (level - bLevel);  bTilt += baseK * (tilt - bTilt);
                bCrest += baseK * (crest - bCrest);  bBody += baseK * (body - bBody);
                bPres  += baseK * (pres - bPres);
            }
        }
        out.warm = bValid ? 1.0f : std::min (1.0f, warmAcc / warmNeed);

        // ---- effort -----------------------------------------------------
        float target = 0.0f;
        if (warmAcc > 0.0f)
        {
            const float lv = ramp (level - bLevel, 4.0f, 16.0f);
            // Tilt and crest only count alongside a level rise: a darker or
            // brighter vowel at normal level is not more effort.
            const float withLv = std::min (1.0f, lv * 4.0f);
            const float tl = ramp (bTilt - tilt, 0.0f, 6.0f) * withLv;
            const float cr = ramp (bCrest - crest, 0.0f, 6.0f) * withLv;
            target = (0.6f * lv + 0.3f * tl + 0.1f * cr) * conf * out.warm;
            target = std::clamp (target, 0.0f, 1.0f);
        }
        out.effort += (target > out.effort ? atkK : relK) * (target - out.effort);
        if (out.effort < 1.0e-5f) out.effort = 0.0f;

        const float bx = warmAcc > 0.0f ? ramp (body - bBody, 0.0f, 3.0f) * conf : 0.0f;
        const float px = warmAcc > 0.0f ? ramp (pres - bPres, 0.0f, 3.0f) * conf : 0.0f;
        out.bodyEx += exK * (bx - out.bodyEx);
        out.presEx += exK * (px - out.presEx);
    }

    double fs = 48000.0;
    float envK = 0, peakRel = 0, atkK = 0, relK = 0, baseK = 0, exK = 0, warmNeed = 1;
    Band  lo[2], hi[2], bo[2], pr[2];
    float eFull[2] {}, eLo[2] {}, eHi[2] {}, eBody[2] {}, ePres[2] {}, peak[2] {};
    int   zc[2] {};
    float prevSign[2] {};
    float zcr = 0.0f;
    int   clipHold = 0, phase = 0;
    bool  bValid = false, learning = true;
    float warmAcc = 0.0f;
    float bLevel = 0, bTilt = 0, bCrest = 0, bBody = 0, bPres = 0;
    Output out;
};
