// PsolaEngine.h — Real-time TD-PSOLA voice transformer core (v0.2)
// Pitch shift and formant shift are fully independent.
// Self-contained header (no dependencies). C++17.
//
// Algorithm family: pitch-synchronous granular resynthesis (TD-PSOLA),
// the same family Logic Pro's Vocal Transformer is based on.
//
//   * Pitch change     = re-spacing of pitch-synchronous grains
//   * Formant change   = resampling of grain contents
//   * Robotize         = constant grain spacing at a fixed frequency
//   * Pitch range      = expand/compress intonation around a center pitch
//   * Consonant shift  = separate formant ratio for unvoiced segments
//   * Breath           = pitch-synchronous aspiration noise (glottal source model)
//   * Tilt             = source spectral tilt (low/high balance around 1 kHz)

#pragma once
#include <vector>
#include <cmath>
#include <algorithm>
#include <cstdint>

#ifndef M_PI
#define M_PI 3.14159265358979323846   // MSVC (Windows) では未定義のため
#endif

class PsolaEngine
{
public:
    struct Params
    {
        float pitchSemi     = 0.0f;   // -24..+24 st
        float formantSemi   = 0.0f;   // -24..+24 st
        float consonantSemi = 0.0f;   // -12..+12 st, ADDED to formantSemi for unvoiced
        float pitchRange    = 1.0f;   // 0.5..2.0  intonation scale (1 = unchanged)
        float pitchCenterHz = 220.0f; // pivot for pitchRange scaling
        float breath        = 0.0f;   // 0..1 aspiration amount
        float tiltDb        = 0.0f;   // -6..+6 dB  (+ = warmer/softer, - = brighter)
        float jitter        = 0.0f;   // 0..1 natural pitch micro-variation
        bool  robotize      = false;
        float robotHz       = 120.0f;
        float mix           = 1.0f;   // 0..1 dry/wet

        // Low Voice Mode: for deep/creaky voices (vocal fry). Extends pitch
        // tracking down to 40 Hz and holds the last stable pitch through
        // irregular stretches so voiced/unvoiced doesn't flutter.
        bool  lowVoice      = false;
    };

    void prepare (double sampleRate)
    {
        fs        = sampleRate;
        maxLag    = (int) (fs / 60.0);   // lowest tracked f0 = 60 Hz (normal)
        maxLagLow = (int) (fs / 40.0);   // lowest tracked f0 = 40 Hz (Low Voice Mode)
        minLag    = (int) (fs / 500.0);  // highest tracked f0 = 500 Hz
        D         = 2048;                // lookahead (latency), samples
        maxHout   = 1200;

        inBuf .assign (kRing, 0.0f);
        accBuf.assign (kRing, 0.0f);
        normBuf.assign (kRing, 0.0f);
        tmp.assign ((size_t) (kDetN + maxLagLow + 4), 0.0f);
        grainScratch.assign ((size_t) (2 * maxHout + 2), 0.0f);
        holdCount = 0;

        writePos    = 0;
        nextMarkF   = (double) D;
        lastInMark  = 0.0;
        curP        = (float) (fs / 150.0);
        voiced      = false;
        sinceDetect = 0;
        tiltLp      = 0.0f;
        tiltK       = 1.0f - std::exp ((float) (-2.0 * M_PI * 1000.0 / fs));
    }

    int latencySamples() const { return D; }

    void setParams (const Params& q)
    {
        pitchRatio     = std::pow (2.0f, q.pitchSemi / 12.0f);
        formantRatio   = std::pow (2.0f, q.formantSemi / 12.0f);
        consonantRatio = std::pow (2.0f, (q.formantSemi + q.consonantSemi) / 12.0f);
        range          = std::clamp (q.pitchRange, 0.25f, 4.0f);
        centerHz       = std::clamp (q.pitchCenterHz, 50.0f, 600.0f);
        breath         = std::clamp (q.breath, 0.0f, 1.0f);
        jitterAmt      = std::clamp (q.jitter, 0.0f, 1.0f);
        gLow           = std::pow (10.0f,  q.tiltDb / 20.0f);
        gHigh          = std::pow (10.0f, -q.tiltDb / 20.0f);
        robotize       = q.robotize;
        robotHz        = std::max (30.0f, q.robotHz);
        mix            = q.mix;
        lowVoice       = q.lowVoice;
    }

    // legacy convenience overload (kept for compatibility)
    void setParams (float pitchSemi, float formantSemi,
                    bool robot, float robotHz_, float mix_)
    {
        Params q;
        q.pitchSemi = pitchSemi;  q.formantSemi = formantSemi;
        q.robotize  = robot;      q.robotHz     = robotHz_;
        q.mix       = mix_;
        setParams (q);
    }

    // Mono in → mono out, n samples. out may alias in.
    void process (const float* in, float* out, int n)
    {
        const int64_t start = writePos;
        for (int i = 0; i < n; ++i)
            inBuf[(size_t) ((start + i) & kMask)] = in[i];
        writePos += n;

        sinceDetect += n;
        if (sinceDetect >= 512 && writePos >= kDetN + maxLag)
        {
            detectPitch();
            sinceDetect = 0;
        }

        while (nextMarkF < (double) (writePos + maxHout))
            placeGrain();

        const bool doTilt = (gLow != 1.0f || gHigh != 1.0f);

        for (int i = 0; i < n; ++i)
        {
            const int64_t oi  = start + i;
            const size_t  idx = (size_t) (oi & kMask);
            const float   nrm = normBuf[idx];
            float wet = nrm > 1.0e-3f ? accBuf[idx] / std::max (nrm, 0.25f) : 0.0f;

            if (doTilt)   // source spectral tilt around 1 kHz (wet path only)
            {
                tiltLp += tiltK * (wet - tiltLp);
                wet = gLow * tiltLp + gHigh * (wet - tiltLp);
            }

            float dry = 0.0f;
            const int64_t di = oi - D;
            if (di >= 0)
                dry = inBuf[(size_t) (di & kMask)];

            out[i] = mix * wet + (1.0f - mix) * dry;
            accBuf[idx]  = 0.0f;
            normBuf[idx] = 0.0f;
        }
    }

    bool  isVoiced()  const { return voiced; }
    float currentF0() const { return voiced ? (float) fs / curP : 0.0f; }

private:
    static constexpr int kRing = 1 << 15;
    static constexpr int kMask = kRing - 1;
    static constexpr int kDetN = 1024;

    // ---------------- pitch detection (YIN + octave guard) ----------------
    void detectPitch()
    {
        const int effMaxLag = lowVoice ? maxLagLow : maxLag;
        const int span = kDetN + effMaxLag;
        const int64_t s0 = writePos - span;
        for (int i = 0; i < span; ++i)
            tmp[(size_t) i] = inBuf[(size_t) ((s0 + i) & kMask)];

        double energy = 0.0;
        for (int i = 0; i < kDetN; ++i) energy += (double) tmp[(size_t)i] * tmp[(size_t)i];
        if (energy / kDetN < 1.0e-8) { voiced = false; holdCount = 0; return; }

        std::vector<double>& d = dbuf;
        d.assign ((size_t) effMaxLag + 1, 1.0);

        double cum = 0.0;
        for (int tau = 1; tau <= effMaxLag; ++tau)
        {
            double s = 0.0;
            const float* a = tmp.data();
            const float* b = tmp.data() + tau;
            for (int i = 0; i < kDetN; ++i)
            {
                const double diff = (double) a[i] - (double) b[i];
                s += diff * diff;
            }
            cum += s;
            d[(size_t) tau] = (cum > 0.0) ? s * tau / cum : 1.0;
        }

        // first local minimum below threshold, else global minimum
        int pick = -1, best = -1;
        double bestVal = 1.0e9;
        for (int tau = minLag; tau <= effMaxLag; ++tau)
        {
            const double v = d[(size_t) tau];
            if (v < bestVal) { bestVal = v; best = tau; }
            if (pick < 0 && v < 0.15 && tau > minLag && tau < effMaxLag
                && v <= d[(size_t)(tau-1)] && v <= d[(size_t)(tau+1)])
                pick = tau;
        }
        int lag = pick > 0 ? pick : best;

        const bool confident = (lag > 0) && ! (pick < 0 && bestVal > 0.45);
        if (! confident)
        {
            // Low Voice Mode: creaky/fry phonation is irregular, so the
            // detector loses confidence for a few frames although the voice
            // continues. Hold the last stable pitch instead of dropping to
            // the unvoiced path (which would toggle conversion on and off).
            if (lowVoice && voiced && holdCount < kHoldMax)
            {
                ++holdCount;
                return;                      // keep previous curP, stay voiced
            }
            voiced = false;
            holdCount = 0;
            return;
        }
        holdCount = 0;

        // octave guard: when already voiced, prefer a dip near the previous period
        if (voiced)
        {
            const int lo = std::max (minLag, (int) (curP * 0.72f));
            const int hi = std::min (effMaxLag, (int) (curP * 1.38f));
            if (lo < hi && (lag < lo || lag > hi))
            {
                int    nl = -1; double nv = 1.0e9;
                for (int t = lo; t <= hi; ++t)
                    if (d[(size_t) t] < nv) { nv = d[(size_t) t]; nl = t; }
                if (nl > 0 && nv < 0.35)
                    lag = nl;   // stay on the established octave
            }
        }

        double p = (double) lag;
        if (lag > minLag && lag < effMaxLag)
        {
            const double y0 = d[(size_t)(lag-1)], y1 = d[(size_t)lag], y2 = d[(size_t)(lag+1)];
            const double den = y0 - 2.0*y1 + y2;
            if (std::abs (den) > 1.0e-12)
                p += 0.5 * (y0 - y2) / den;
        }

        const float newP = (float) std::clamp (p, (double) minLag, (double) effMaxLag);
        curP  = voiced ? 0.65f * curP + 0.35f * newP : newP;
        voiced = true;
    }

    // ---------------- grain placement ----------------
    void placeGrain()
    {
        const float P = curP;
        const bool  v = voiced;
        const float f = std::max (0.25f, v ? formantRatio : consonantRatio);

        float Ts;
        if (robotize)
            Ts = (float) (fs / robotHz);
        else if (v)
        {
            // intonation scaling: f_out = center * (f_in*ratio / center)^range
            double ft = (fs / (double) P) * (double) pitchRatio;
            if (range != 1.0f)
                ft = (double) centerHz * std::pow (ft / (double) centerHz, (double) range);
            ft = std::clamp (ft, 40.0, 1000.0);
            Ts = (float) (fs / ft);
        }
        else
            Ts = 256.0f;

        if (jitterAmt > 0.0f && v && !robotize)
            Ts *= 1.0f + 0.02f * jitterAmt * nextRand();

        const int64_t mi  = (int64_t) std::llround (nextMarkF);
        const double  err = nextMarkF - (double) mi;

        const double natural = nextMarkF - (double) D;
        double c = natural;

        if (v)
        {
            const double k = std::round ((natural - lastInMark) / (double) P);
            c = lastInMark + k * (double) P;
            if (std::abs (c - natural) > (double) P)
                c = natural;
            c = alignToPeak (c, P);
            lastInMark = c;
        }

        // grain half width: ~1 period, capped so very low pitches (Low Voice
        // Mode, down to 40 Hz) still fit inside the lookahead window
        const float capHalf  = lowVoice ? 700.0f : (float) maxLag;
        const float baseHalf = v ? std::min (P, capHalf) : 256.0f;
        const int   houtCap  = lowVoice ? std::min (maxHout, 900) : maxHout;
        const int Hout = (int) std::clamp (baseHalf / f, 32.0f, (float) houtCap);

        // pass 1: resample grain into scratch
        for (int j = -Hout; j <= Hout; ++j)
        {
            const double  ip = c + ((double) j + err) * (double) f;
            const int64_t i0 = (int64_t) std::floor (ip);
            float s = 0.0f;
            if (i0 >= 0 && i0 + 1 < writePos)
            {
                const float frac = (float) (ip - (double) i0);
                s = inBuf[(size_t) (i0 & kMask)] * (1.0f - frac)
                  + inBuf[(size_t) ((i0 + 1) & kMask)] * frac;
            }
            grainScratch[(size_t) (j + Hout)] = s;
        }

        // pass 2: window + optional aspiration, overlap-add.
        // Aspiration model: band-limited noise (~1.5-5 kHz, like real breath
        // turbulence) whose level follows a SMOOTHED envelope of the waveform.
        // (Full-band noise with instantaneous modulation sounds like digital
        // hiss / ring-mod — that was the v0.2 mistake.)
        const bool doBreath = (v && breath > 0.0f);
        float nPrev = 0.0f, nB1 = 0.0f, nB2 = 0.0f, envF = 0.0f;
        for (int j = -Hout; j <= Hout; ++j)
        {
            float x = grainScratch[(size_t) (j + Hout)];

            if (doBreath)
            {
                envF += 0.02f * (std::abs (x) - envF);       // slow envelope (~1 ms)
                const float nz = nextRand();
                const float hp = nz - nPrev;                 // cut lows
                nPrev = nz;
                nB1  += 0.15f * (hp  - nB1);                 // 2-pole lowpass ->
                nB2  += 0.15f * (nB1 - nB2);                 // band ~1.5-4 kHz
                x += breath * 14.0f * nB2 * envF;
            }

            const float w = 0.5f * (1.0f + std::cos ((float) M_PI * (float) j / (float) Hout));
            const size_t idx = (size_t) ((mi + j) & kMask);
            accBuf[idx]  += w * x;
            normBuf[idx] += w;
        }

        nextMarkF += (double) std::max (24.0f, Ts);
    }

    double alignToPeak (double c, float P)
    {
        const int r = std::max (4, (int) (P / 6.0f));
        const int64_t c0 = (int64_t) std::llround (c);
        double best = -1.0; int64_t bestI = c0;
        for (int64_t i = c0 - r; i <= c0 + r; ++i)
        {
            if (i < 2 || i + 2 >= writePos) continue;
            double e = 0.0;
            for (int64_t k = i - 2; k <= i + 2; ++k)
            {
                const float x = inBuf[(size_t) (k & kMask)];
                e += (double) x * x;
            }
            if (e > best) { best = e; bestI = i; }
        }
        return (double) bestI;
    }

    float nextRand()   // fast LCG, uniform -1..1
    {
        rng = rng * 1664525u + 1013904223u;
        return (float) ((int32_t) rng) * (1.0f / 2147483648.0f);
    }

    // ---------------- state ----------------
    static constexpr int kHoldMax = 16;   // ~170 ms of pitch hold (detect every ~512 samples)

    double fs = 48000.0;
    int    D = 2048, maxLag = 800, maxLagLow = 1200, minLag = 96, maxHout = 1200;
    int    holdCount = 0;

    std::vector<float>  inBuf, accBuf, normBuf, tmp, grainScratch;
    std::vector<double> dbuf;

    int64_t writePos = 0;
    double  nextMarkF = 0.0, lastInMark = 0.0;
    float   curP = 320.0f;
    bool    voiced = false;
    int     sinceDetect = 0;
    uint32_t rng = 0x1234567u;

    float pitchRatio = 1.0f, formantRatio = 1.0f, consonantRatio = 1.0f;
    float range = 1.0f, centerHz = 220.0f, breath = 0.0f, jitterAmt = 0.0f;
    float gLow = 1.0f, gHigh = 1.0f, tiltLp = 0.0f, tiltK = 0.12f;
    float mix = 1.0f, robotHz = 120.0f;
    bool  robotize = false, lowVoice = false;
};
