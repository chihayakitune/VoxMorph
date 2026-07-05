// PsolaEngine.h — Real-time TD-PSOLA voice transformer core
// Pitch shift and formant shift are fully independent.
// Self-contained header (no dependencies). C++17.
//
// Algorithm family: pitch-synchronous granular resynthesis (TD-PSOLA),
// the same family Logic Pro's Vocal Transformer is based on
// ("The Vocal Transformer effect algorithm is based on granular synthesis"
//  — Logic Pro 9 manual).
//
//   * Pitch change   = re-spacing of pitch-synchronous grains (envelope kept)
//   * Formant change = resampling of grain contents (envelope scaled)
//   * Robotize       = constant grain spacing at a fixed frequency

#pragma once
#include <vector>
#include <cmath>
#include <algorithm>
#include <cstdint>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

class PsolaEngine
{
public:
    void prepare (double sampleRate)
    {
        fs      = sampleRate;
        maxLag  = (int) (fs / 60.0);   // lowest tracked f0 = 60 Hz
        minLag  = (int) (fs / 500.0);  // highest tracked f0 = 500 Hz
        D       = 2048;                // lookahead (latency), samples
        maxHout = 1200;

        inBuf .assign (kRing, 0.0f);
        accBuf.assign (kRing, 0.0f);
        normBuf.assign (kRing, 0.0f);
        tmp.assign ((size_t) (kDetN + maxLag + 4), 0.0f);

        writePos    = 0;
        nextMarkF   = (double) D;
        lastInMark  = 0.0;
        curP        = (float) (fs / 150.0);
        voiced      = false;
        sinceDetect = 0;
    }

    int latencySamples() const { return D; }

    // pitchSemi/formantSemi in semitones. mix 0..1.
    void setParams (float pitchSemi, float formantSemi,
                    bool robot, float robotHz_, float mix_)
    {
        pitchRatio   = std::pow (2.0f, pitchSemi   / 12.0f);
        formantRatio = std::pow (2.0f, formantSemi / 12.0f);
        robotize     = robot;
        robotHz      = std::max (30.0f, robotHz_);
        mix          = mix_;
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

        // Place all grains whose windows can reach into future emitted blocks.
        // A grain at mark m needs input up to m - D + Hin (Hin <= maxLag <= 800),
        // available while m <= writePos + D - maxLag. We place while
        // m < writePos + maxHout so every grain overlapping [start, writePos) exists.
        while (nextMarkF < (double) (writePos + maxHout))
            placeGrain();

        for (int i = 0; i < n; ++i)
        {
            const int64_t oi  = start + i;
            const size_t  idx = (size_t) (oi & kMask);
            const float   nrm = normBuf[idx];
            const float   wet = nrm > 1.0e-3f ? accBuf[idx] / std::max (nrm, 0.25f) : 0.0f;

            float dry = 0.0f;
            const int64_t di = oi - D;
            if (di >= 0)
                dry = inBuf[(size_t) (di & kMask)];

            out[i] = mix * wet + (1.0f - mix) * dry;
            accBuf[idx]  = 0.0f;   // consumed — recycle ring cell
            normBuf[idx] = 0.0f;
        }
    }

    // Introspection (for tests / UI)
    bool  isVoiced()  const { return voiced; }
    float currentF0() const { return voiced ? (float) fs / curP : 0.0f; }

private:
    static constexpr int kRing = 1 << 15;          // 32768-sample rings
    static constexpr int kMask = kRing - 1;
    static constexpr int kDetN = 1024;             // YIN window

    // ---------------- pitch detection (YIN) ----------------
    void detectPitch()
    {
        const int span = kDetN + maxLag;
        const int64_t s0 = writePos - span;
        for (int i = 0; i < span; ++i)
            tmp[(size_t) i] = inBuf[(size_t) ((s0 + i) & kMask)];

        double energy = 0.0;
        for (int i = 0; i < kDetN; ++i) energy += (double) tmp[(size_t)i] * tmp[(size_t)i];
        if (energy / kDetN < 1.0e-8) { voiced = false; return; }

        // difference function + cumulative-mean normalisation
        double cum = 0.0;
        int    bestLag = -1;
        double bestVal = 1.0e9;
        int    pickLag = -1;
        std::vector<double>& d = dbuf;
        d.assign ((size_t) maxLag + 1, 0.0);

        for (int tau = 1; tau <= maxLag; ++tau)
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
            const double nd = (cum > 0.0) ? s * tau / cum : 1.0;
            d[(size_t) tau] = nd;

            if (tau >= minLag)
            {
                if (nd < bestVal) { bestVal = nd; bestLag = tau; }
                if (pickLag < 0 && nd < 0.15
                    && tau + 1 <= maxLag)                      // first clear dip
                {
                    // wait until local minimum passes
                    if (d[(size_t) tau] <= d[(size_t) (tau - 1)]) continue;
                    pickLag = tau - 1;
                    break;
                }
            }
        }

        int lag = pickLag > 0 ? pickLag : bestLag;
        if (lag <= 0 || (pickLag < 0 && bestVal > 0.45)) { voiced = false; return; }

        // parabolic refinement
        double p = (double) lag;
        if (lag > minLag && lag < maxLag)
        {
            const double y0 = d[(size_t)(lag-1)], y1 = d[(size_t)lag], y2 = d[(size_t)(lag+1)];
            const double den = y0 - 2.0*y1 + y2;
            if (std::abs (den) > 1.0e-12)
                p += 0.5 * (y0 - y2) / den;
        }

        const float newP = (float) std::clamp (p, (double) minLag, (double) maxLag);
        curP  = voiced ? 0.65f * curP + 0.35f * newP : newP;
        voiced = true;
    }

    // ---------------- grain placement ----------------
    void placeGrain()
    {
        const float P  = curP;
        const float f  = std::max (0.25f, formantRatio);
        const bool  v  = voiced;

        float Ts;
        if      (robotize) Ts = (float) (fs / robotHz);
        else if (v)        Ts = P / pitchRatio;
        else               Ts = 256.0f;                 // unvoiced: identity spacing

        const int64_t mi  = (int64_t) std::llround (nextMarkF);
        const double  err = nextMarkF - (double) mi;    // sub-sample mark position

        // natural (time-aligned) input position for this output mark
        const double natural = nextMarkF - (double) D;
        double c = natural;

        if (v)
        {
            // snap to the pitch-synchronous mark grid: each grain then contains
            // one glottal event, so output periodicity is set by grain spacing
            // (this is what makes both pitch shift and robotize work)
            const double k = std::round ((natural - lastInMark) / (double) P);
            c = lastInMark + k * (double) P;
            // keep the grid from drifting too far from real time
            if (std::abs (c - natural) > (double) P)
                c = natural;
            c = alignToPeak (c, P);
            lastInMark = c;
        }

        // output half-width: ~2 input periods mapped through the resampler
        float baseHalf = v ? P : 256.0f;
        int Hout = (int) std::clamp (baseHalf / f, 32.0f, (float) maxHout);

        for (int j = -Hout; j <= Hout; ++j)
        {
            const double  ip = c + ((double) j + err) * (double) f;  // resample read
            const int64_t i0 = (int64_t) std::floor (ip);
            if (i0 < 0 || i0 + 1 >= writePos) continue;

            const float frac = (float) (ip - (double) i0);
            const float s = inBuf[(size_t) (i0 & kMask)] * (1.0f - frac)
                          + inBuf[(size_t) ((i0 + 1) & kMask)] * frac;

            const float w = 0.5f * (1.0f + std::cos ((float) M_PI * (float) j / (float) Hout));
            const size_t idx = (size_t) ((mi + j) & kMask);
            accBuf[idx]  += w * s;
            normBuf[idx] += w;
        }

        nextMarkF += (double) std::max (24.0f, Ts);
    }

    // crude glottal-pulse alignment: snap to local energy peak
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

    // ---------------- state ----------------
    double fs = 48000.0;
    int    D = 2048, maxLag = 800, minLag = 96, maxHout = 1200;

    std::vector<float>  inBuf, accBuf, normBuf, tmp;
    std::vector<double> dbuf;

    int64_t writePos = 0;
    double  nextMarkF = 0.0, lastInMark = 0.0;
    float   curP = 320.0f;
    bool    voiced = false;
    int     sinceDetect = 0;

    float pitchRatio = 1.0f, formantRatio = 1.0f, mix = 1.0f, robotHz = 120.0f;
    bool  robotize = false;
};
