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
//   * Air preserve     = mixed harmonic+noise handling: the natural breath
//                        component bypasses the pitch shifter un-pitched

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
        // tracking down to 40 Hz, tracks the glottal pulse rate instead of
        // sub-octave pattern periods, and holds the last stable pitch through
        // irregular stretches so voiced/unvoiced doesn't flutter.
        bool  lowVoice      = false;

        // Pitch Floor: if the OUTPUT pitch falls below this, lift it softly
        // toward the floor (log-domain soft knee). 0 = off.
        float pitchFloorHz  = 0.0f;

        // Individual formant control (spectral envelope warping, per grain).
        // Shifts are ADDED on top of the global formant shift; gains boost or
        // cut the region around each tracked formant.
        float f1Shift = 0.0f, f2Shift = 0.0f, f3Shift = 0.0f;   // semitones
        float f1Gain  = 0.0f, f2Gain  = 0.0f, f3Gain  = 0.0f;   // dB

        // Low Latency Mode: halves the lookahead (43 -> ~21 ms). Pitch
        // tracking floor rises to ~90 Hz. Ignored while lowVoice is on.
        bool  lowLatency = false;

        // Air Preserve (mixed harmonic+noise processing for breathy vowels):
        // the aperiodic breath component above ~1.4 kHz is separated from the
        // harmonics BEFORE grain cutting and passed through to the output
        // un-pitched. Re-spacing grains imposes the new pitch period on any
        // noise inside them (worst when an upward shift reuses a grain and
        // repeats its noise verbatim), which is what makes breathy voices
        // turn metallic; routing the breath around the shifter keeps it a
        // continuous natural noise. 0 = off (identical to previous versions).
        // 0..1 scales the split up to its variance-optimal maximum (energy
        // neutral); 1..1.5 additionally boosts the bypassed breath (up to
        // ~+4 dB at 1.5) so the effect is clearly audible.
        float airPreserve = 0.0f;   // 0..1.5

        // Separation crossover for Air Preserve: everything above this is
        // treated as breath. Lower reaches further into the mids (stronger,
        // but pitch movement may leak a ghost of the original pitch);
        // higher keeps only the top "air".
        float airFreqHz   = 1000.0f;   // 300..4000

        // High Range guard (Babisei-style variable pitch): when the INPUT
        // pitch rises above hiRangeHz (laughing, squealing), the pitch and
        // formant shifts blend from their normal amounts toward
        // hiPitchAmt / hiFormantAmt (a fraction of the normal shift, 0..1),
        // reaching the full high-range amounts one octave above hiRangeHz.
        // Stops laughs from being shifted into unnaturally high tones.
        // hiRangeHz = 0 disables the feature entirely (previous behaviour).
        float hiRangeHz    = 0.0f;    // 0 = off, else ~150..600
        float hiPitchAmt   = 0.5f;    // 0..1: pitch shift kept in high range
        float hiFormantAmt = 1.0f;    // 0..1: formant shift kept in high range

        // GCI Grain Sync: align grain centres to the glottal closure
        // instants (sharpest flank of each period, found by short-time
        // derivative energy) and track them period-to-period, ESOLA-style,
        // instead of the plain energy-peak search. Keeps successive grains
        // phase-consistent, reducing roughness/shimmer. Falls back to the
        // classic alignment automatically where no clear epochs exist;
        // off = identical to previous versions.
        bool  gciSync     = false;
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
        harmBuf .assign (kRing, 0.0f);
        noiseBuf.assign (kRing, 0.0f);
        accBuf.assign (kRing, 0.0f);
        normBuf.assign (kRing, 0.0f);
        tmp.assign ((size_t) (kDetN + maxLagLow + 4), 0.0f);
        tmpD.assign ((size_t) ((kDetN + maxLagLow) / 2 + 4), 0.0f);
        grainScratch.assign ((size_t) (2 * maxHout + 2), 0.0f);
        holdCount = 0;

        fr.assign (kFFT, 0.0f);   fi.assign (kFFT, 0.0f);
        mag.assign (kFFT/2 + 1, 0.0f);
        env.assign (kFFT/2 + 1, 0.0f);
        envSm.assign (kFFT/2 + 1, 0.0f);
        prefix.assign (kFFT/2 + 3, 0.0);
        trackF[0] = trackF[1] = trackF[2] = -1.0f;

        writePos    = 0;
        nextMarkF   = (double) D;
        lastInMark  = 0.0;
        curP        = (float) (fs / 150.0);
        voiced      = false;
        sinceDetect = 0;
        tiltLp      = 0.0f;
        tiltK       = 1.0f - std::exp ((float) (-2.0 * M_PI * 1000.0 / fs));

        airG   = airB = 0.0f;
        airLp1 = airLp2 = 0.0f;
        lastGci = -1;
        gciHold = 0;
        gciE.reserve ((size_t) (maxLagLow / 3 + 8));
        airP   = curP;
        airK   = 1.0f - std::exp ((float) (-1.0 / (0.005 * fs)));          // ~5 ms gate
        hpAirK = 1.0f - std::exp ((float) (-2.0 * M_PI * 1400.0 / fs));    // breath band HP
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
        floorHz        = std::clamp (q.pitchFloorHz, 0.0f, 400.0f);
        airAmt         = std::clamp (q.airPreserve, 0.0f, 1.5f);
        hpAirK         = 1.0f - std::exp ((float) (-2.0 * M_PI
                             * std::clamp (q.airFreqHz, 300.0f, 4000.0f) / fs));
        // 0..1 -> split amount up to the variance-optimal 2/3 (beyond it,
        // subtracting more noise puts anti-correlated copies back into the
        // grains); 1..1.5 -> boost the bypassed breath instead, which both
        // masks what is left of the periodic noise and makes the knob move
        // clearly audible.
        airKSplit      = 0.6667f * std::min (1.0f, airAmt);
        airBoost       = 1.0f + 1.2f * std::max (0.0f, airAmt - 1.0f);
        gciOn          = q.gciSync;
        hiFreq         = (q.hiRangeHz > 20.0f) ? std::clamp (q.hiRangeHz, 100.0f, 600.0f) : 0.0f;
        hiPAmt         = std::clamp (q.hiPitchAmt,   0.0f, 1.0f);
        hiFAmt         = std::clamp (q.hiFormantAmt, 0.0f, 1.0f);

        fShiftRatio[0] = std::pow (2.0f, q.f1Shift / 12.0f);
        fShiftRatio[1] = std::pow (2.0f, q.f2Shift / 12.0f);
        fShiftRatio[2] = std::pow (2.0f, q.f3Shift / 12.0f);
        fGainDb[0] = q.f1Gain;  fGainDb[1] = q.f2Gain;  fGainDb[2] = q.f3Gain;
        perFmt = std::abs (q.f1Shift) > 0.01f || std::abs (q.f2Shift) > 0.01f
              || std::abs (q.f3Shift) > 0.01f || std::abs (q.f1Gain) > 0.05f
              || std::abs (q.f2Gain)  > 0.05f || std::abs (q.f3Gain) > 0.05f;

        const bool lowLat = q.lowLatency && ! q.lowVoice;   // lowVoice wins
        pendingD    = (int) (fs * (lowLat ? 0.0213 : 0.0427));
        effMaxLagCur = lowVoice ? maxLagLow
                                : (lowLat ? (int)(fs / 90.0) : maxLag);
        capHalfCur   = lowVoice ? 700.0f
                                : (lowLat ? (float)(fs / 150.0) : (float) maxLag);
        houtCapCur   = lowVoice ? std::min (maxHout, 900)
                                : (lowLat ? (int)(fs / 120.0) : maxHout);
        guardFrac    = lowLat ? 0.35f : 1.0f;   // snap-grid drift budget
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
        // Internally chop large host buffers into <=512-sample chunks so the
        // engine behaves identically at ANY buffer size: pitch-detection
        // cadence, grain scheduling and dezippering all stay uniform.
        while (n > 512)
        {
            processChunk (in, out, 512);
            in += 512; out += 512; n -= 512;
        }
        if (n > 0)
            processChunk (in, out, n);
    }

    void processChunk (const float* in, float* out, int n)
    {
        if (pendingD != D)     // latency mode changed: soft reset
        {
            D = pendingD;
            std::fill (accBuf.begin(),  accBuf.end(),  0.0f);
            std::fill (normBuf.begin(), normBuf.end(), 0.0f);
            nextMarkF  = (double) (writePos + D);
            lastInMark = (double) writePos;
            voiced = false;
            holdCount = 0;
            lastGci = -1;
        }
        const int64_t start = writePos;
        for (int i = 0; i < n; ++i)
            inBuf[(size_t) ((start + i) & kMask)] = in[i];
        writePos += n;

        // ---- harmonic/noise split (Air Preserve) ----
        // A causal 2-tap pitch comb predicts the periodic part; the high-
        // passed residual is the natural breath. Grains are cut from
        // harmBuf (= input minus breath) and noiseBuf is added back to the
        // output un-pitched, D samples later. At the optimal split (2/3 of
        // the residual) total noise energy is preserved exactly. airG gates
        // on voicing and dezippers knob moves; when it is fully off the
        // buffers are a plain copy and the comb is skipped (no CPU cost).
        {
            const bool  on      = voiced && airAmt > 0.001f;
            const float tSplit  = on ? airKSplit            : 0.0f;
            const float tBypass = on ? airKSplit * airBoost : 0.0f;
            if (tSplit > 0.0f || airG > 1.0e-4f)
            {
                for (int i = 0; i < n; ++i)
                {
                    const int64_t pos = start + i;
                    const size_t  idx = (size_t) (pos & kMask);
                    airG += airK * (tSplit  - airG);
                    airB += airK * (tBypass - airB);
                    airP += 0.002f * (curP - airP);        // ~10 ms period glide
                    const double P = (double) std::max (32.0f, airP);
                    const float per = 0.5f * (readCubic ((double) pos - P)
                                            + readCubic ((double) pos - 2.0 * P));
                    float res = inBuf[idx] - per;
                    airLp1 += hpAirK * (res - airLp1);  res -= airLp1;
                    airLp2 += hpAirK * (res - airLp2);  res -= airLp2;
                    noiseBuf[idx] = airB * res;
                    harmBuf[idx]  = inBuf[idx] - airG * res;
                }
            }
            else
            {
                for (int i = 0; i < n; ++i)
                {
                    const size_t idx = (size_t) ((start + i) & kMask);
                    harmBuf[idx]  = inBuf[idx];
                    noiseBuf[idx] = 0.0f;
                }
                airG = airB = 0.0f;  airLp1 = airLp2 = 0.0f;  airP = curP;
            }
        }

        // dezipper: glide the conversion ratios over ~40 ms so parameter
        // moves (or host automation) never step audibly between grains
        {
            const float a = std::min (1.0f, (float) (n / (0.04 * fs)));
            prSm += a * (pitchRatio     - prSm);
            frSm += a * (formantRatio   - frSm);
            crSm += a * (consonantRatio - crSm);
        }

        sinceDetect += n;
        if (sinceDetect >= 512 && writePos >= kDetN + maxLag)
        {
            detectPitch();
            sinceDetect = 0;
        }

        while (nextMarkF < (double) (writePos + houtCapCur))
            placeGrain();

        const bool doTilt = (gLow != 1.0f || gHigh != 1.0f);

        for (int i = 0; i < n; ++i)
        {
            const int64_t oi  = start + i;
            const size_t  idx = (size_t) (oi & kMask);
            const float   nrm = normBuf[idx];
            float wet = nrm > 1.0e-3f ? accBuf[idx] / std::max (nrm, 0.25f) : 0.0f;

            const int64_t di = oi - D;
            if (di >= 0)
                wet += noiseBuf[(size_t) (di & kMask)];   // un-pitched breath

            if (doTilt)   // source spectral tilt around 1 kHz (wet path only)
            {
                tiltLp += tiltK * (wet - tiltLp);
                wet = gLow * tiltLp + gHigh * (wet - tiltLp);
            }

            float dry = 0.0f;
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
        const int effMaxLag = effMaxLagCur;
        const int span = kDetN + effMaxLag;
        const int64_t s0 = writePos - span;
        for (int i = 0; i < span; ++i)
            tmp[(size_t) i] = inBuf[(size_t) ((s0 + i) & kMask)];

        double energy = 0.0;
        for (int i = 0; i < kDetN; ++i) energy += (double) tmp[(size_t)i] * tmp[(size_t)i];
        if (energy / kDetN < 1.0e-8) { voiced = false; holdCount = 0; lastGci = -1; return; }

        // ---- decimate x2: the CMND search runs on a half-rate copy, cutting
        // this burst (which lands on a SINGLE block every 512 samples) ~4x.
        // That periodic burst was the main cause of dropouts at small buffers.
        const int spanD = span / 2;
        const int ND    = kDetN / 2;
        const int minL  = std::max (2, minLag / 2);
        const int maxL  = effMaxLag / 2;
        for (int i = 0; i < spanD; ++i)
            tmpD[(size_t)i] = 0.5f * (tmp[(size_t)(2*i)] + tmp[(size_t)(2*i + 1)]);

        std::vector<double>& d = dbuf;
        d.assign ((size_t) maxL + 1, 1.0);

        double cum = 0.0;
        for (int tau = 1; tau <= maxL; ++tau)
        {
            double s = 0.0;
            const float* a = tmpD.data();
            const float* b = tmpD.data() + tau;
            for (int i = 0; i < ND; ++i)
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
        for (int tau = minL; tau <= maxL; ++tau)
        {
            const double v = d[(size_t) tau];
            if (v < bestVal) { bestVal = v; best = tau; }
            if (pick < 0 && v < 0.15 && tau > minL && tau < maxL
                && v <= d[(size_t)(tau-1)] && v <= d[(size_t)(tau+1)])
                pick = tau;
        }
        int lag = pick > 0 ? pick : best;

        // Prefer the pulse rate over sub-octaves. Creaky/fry voices repeat
        // most strongly at 2-3x the glottal pulse period, so the global
        // minimum sits an octave (or more) too low; respacing single pulses
        // both fixes the "pulled down an octave" tracking and regularises
        // the fry in the output. Repeatedly try half the current lag.
        while (lag > 0 && lag / 2 >= minL)
        {
            const int c0 = lag / 2;
            const int lo = std::max (minL, (int) (c0 * 0.85f));
            const int hi = std::min (maxL, (int) (c0 * 1.15f));
            int bl = -1; double bv = 1.0e9;
            for (int t = lo; t <= hi; ++t)
                if (d[(size_t) t] < bv) { bv = d[(size_t) t]; bl = t; }
            const double dl = d[(size_t) lag];
            const bool ok = bl > 0
                         && (bv < 0.18 || (lowVoice && bv < std::min (0.62, dl * 2.5)));
            if (! ok) break;
            lag = bl;
        }

        const double confThr = lowVoice ? 0.62 : 0.45;   // fry is never "clean"
        const bool confident = (lag > 0) && ! (pick < 0 && bestVal > confThr);
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
            lastGci = -1;
            return;
        }
        holdCount = 0;

        // octave guard: resist sudden DOWNWARD period jumps (subharmonics),
        // but always allow upward corrections back to the pulse rate —
        // otherwise a wrong low lock can never recover.
        if (voiced)
        {
            const float curPD = curP * 0.5f;              // decimated domain
            const int lo = std::max (minL, (int) (curPD * 0.72f));
            const int hi = std::min (maxL, (int) (curPD * 1.38f));
            if (lo < hi && lag > hi)                       // jumped lower
            {
                int    nl = -1; double nv = 1.0e9;
                for (int t = lo; t <= hi; ++t)
                    if (d[(size_t) t] < nv) { nv = d[(size_t) t]; nl = t; }
                if (nl > 0 && nv < (lowVoice ? 0.55 : 0.35))
                    lag = nl;   // stay on the established octave
            }
            else if (lo < hi && lag < lo                   // jumped higher
                     && d[(size_t) lag] > (lowVoice ? 0.68 : 0.30))
            {
                int    nl = -1; double nv = 1.0e9;
                for (int t = lo; t <= hi; ++t)
                    if (d[(size_t) t] < nv) { nv = d[(size_t) t]; nl = t; }
                if (nl > 0 && nv < 0.35)
                    lag = nl;   // higher dip was weak - keep current octave
            }
        }

        // ---- full-rate refinement around 2*lag: sub-sample precision from
        // the plain difference function (cheap: 5 lags x kDetN)
        const int Lc = std::clamp (2 * lag, minLag, effMaxLag);
        double df[5];
        for (int k = 0; k < 5; ++k)
        {
            const int L = std::clamp (Lc + k - 2, 1, effMaxLag);
            double s = 0.0;
            const float* a = tmp.data();
            const float* b = tmp.data() + L;
            for (int i = 0; i < kDetN; ++i)
            {
                const double diff = (double) a[i] - (double) b[i];
                s += diff * diff;
            }
            df[k] = s;
        }
        int mi5 = 1;
        for (int k = 2; k < 4; ++k) if (df[k] < df[mi5]) mi5 = k;
        if (df[0] < df[mi5]) mi5 = 0;
        if (df[4] < df[mi5]) mi5 = 4;
        double p = (double) std::clamp (Lc + mi5 - 2, 1, effMaxLag);
        if (mi5 > 0 && mi5 < 4)
        {
            const double y0 = df[mi5-1], y1 = df[mi5], y2 = df[mi5+1];
            const double den = y0 - 2.0*y1 + y2;
            if (std::abs (den) > 1.0e-12)
                p += 0.5 * (y0 - y2) / den;
        }

        const float newP = (float) std::clamp (p, (double) minLag, (double) effMaxLag);
        // Adaptive smoothing: heavy while steady (kills fry-pulse wobble),
        // light while the pitch is really moving (glides must not lag —
        // a lagging period estimate makes grains mismatch and sound doubled)
        const float smoothBase = lowVoice ? 0.85f : 0.65f;
        const float rel = std::abs (newP - curP) / std::max (curP, 1.0f);
        const float smooth = (voiced && rel > 0.06f) ? 0.5f : smoothBase;
        curP  = voiced ? smooth * curP + (1.0f - smooth) * newP : newP;
        voiced = true;

        // GCI epoch tracking is unreliable while the pitch is really moving:
        // the lastGci + k*P prediction grid shears against the true epochs
        // and the alignment judders audibly on gliding vowels. Revert to the
        // classic alignment until the glide settles. Low Voice Mode is
        // exempt — its alternating fry periods would trip this constantly,
        // and regularising fry is exactly what the epoch tracking is for.
        if (! lowVoice && rel > 0.04f) gciHold = 4;
        else if (gciHold > 0)         --gciHold;
    }

    // ---------------- grain placement ----------------
    void placeGrain()
    {
        const float P = curP;
        const bool  v = voiced;

        // High Range guard: blend the shift amounts toward the high-range
        // fractions as the input pitch rises past hiFreq (full one octave up).
        // Per-grain and driven by the smoothed period, so it is seamless.
        float wHi = 0.0f;
        if (v && hiFreq > 0.0f && ! robotize)
        {
            const float f0in = (float) (fs / P);
            if (f0in > hiFreq)
                wHi = std::min (1.0f, std::log2 (f0in / hiFreq));
        }

        float fRat = v ? frSm : crSm;
        if (wHi > 0.0f)
            fRat = std::pow (frSm, 1.0f - wHi * (1.0f - hiFAmt));
        const float f = std::max (0.25f, fRat);

        float Ts;
        if (robotize)
            Ts = (float) (fs / robotHz);
        else if (v)
        {
            double pr = (double) prSm;
            if (wHi > 0.0f)
                pr = std::pow (pr, (double) (1.0f - wHi * (1.0f - hiPAmt)));
            // intonation scaling: f_out = center * (f_in*ratio / center)^range
            double ft = (fs / (double) P) * pr;
            if (range != 1.0f)
                ft = (double) centerHz * std::pow (ft / (double) centerHz, (double) range);
            if (floorHz > 20.0f && ft < (double) floorHz)          // soft low lift
                ft = (double) floorHz * std::pow (ft / (double) floorHz, 0.4);
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
            if (std::abs (c - natural) > (double) (guardFrac * P))
                c = natural;
            c = gciOn ? alignToGci (c, P) : alignToPeak (c, P);
            lastInMark = c;
        }

        // grain half width: ~1 input period, but never much wider than the
        // OUTPUT spacing. With large upward shifts a 2-period grain carries a
        // second pulse that lands between the new marks and sounds like two
        // voices at once (doubling); narrowing to ~1.25x the output spacing
        // keeps exactly one pulse per grain. Also capped so very low pitches
        // (Low Voice Mode, down to 40 Hz) fit inside the lookahead window.
        float baseHalf = v ? std::min (P, capHalfCur) : 256.0f;
        if (v)
            baseHalf = std::min (baseHalf, std::max (48.0f, 1.25f * Ts));
        const int Hout = (int) std::clamp (baseHalf / f, 32.0f, (float) houtCapCur);

        // pass 1: resample grain into scratch (from the harmonic-only buffer;
        // identical to the input while Air Preserve is off)
        for (int j = -Hout; j <= Hout; ++j)
        {
            const double  ip = c + ((double) j + err) * (double) f;
            const int64_t i0 = (int64_t) std::floor (ip);
            float s = 0.0f;
            if (i0 >= 0 && i0 + 1 < writePos)
            {
                const float frac = (float) (ip - (double) i0);
                s = harmBuf[(size_t) (i0 & kMask)] * (1.0f - frac)
                  + harmBuf[(size_t) ((i0 + 1) & kMask)] * frac;
            }
            grainScratch[(size_t) (j + Hout)] = s;
        }

        // pass 2: optional spectral processing (per-formant warp + breath),
        // then overlap-add. The spectral path windows the grain itself.
        const bool spectral = v && (perFmt || breath > 0.001f);
        if (spectral)
            spectralProcess (2 * Hout + 1, f);

        for (int j = -Hout; j <= Hout; ++j)
        {
            const float x = grainScratch[(size_t) (j + Hout)];
            const float w = 0.5f * (1.0f + std::cos ((float) M_PI * (float) j / (float) Hout));
            const size_t idx = (size_t) ((mi + j) & kMask);
            accBuf[idx]  += spectral ? x : w * x;   // spectral grain is pre-windowed
            normBuf[idx] += w;
        }

        nextMarkF += (double) std::max (24.0f, Ts);
    }

    // ---------------- spectral layer (per-formant warp + breath) ----------
    // Runs on a single grain: window -> FFT -> envelope estimation -> formant
    // peak tracking -> piecewise-linear envelope warp + per-formant gain ->
    // noise-excited-envelope breath -> IFFT. The grain comes back windowed.
    void spectralProcess (int len, float f)
    {
        // adaptive FFT size: most grains fit in 2048, halving the burst cost
        const int N  = (len <= 2040) ? kFFT / 2 : kFFT;
        const int NB = N / 2;
        if (len > N) len = N;
        const int H = (len - 1) / 2;

        // window + zero-pad
        for (int i = 0; i < N; ++i) { fr[(size_t)i] = 0.0f; fi[(size_t)i] = 0.0f; }
        for (int i = 0; i < len; ++i)
        {
            const float w = 0.5f * (1.0f + std::cos ((float) M_PI * (float)(i - H) / (float) H));
            fr[(size_t)i] = grainScratch[(size_t)i] * w;
        }
        fftRadix2 (fr.data(), fi.data(), N, false);

        for (int k = 0; k <= NB; ++k)
            mag[(size_t)k] = std::sqrt (fr[(size_t)k]*fr[(size_t)k] + fi[(size_t)k]*fi[(size_t)k]);

        // spectral envelope: locate each harmonic's peak and connect them by
        // linear interpolation in the log domain ("true envelope" style).
        // Unlike max-dilation this keeps the valleys between formants deep,
        // which is what lets the warp actually REMOVE a formant's old peak.
        const double f0c = (fs / (double) curP) * (double) f;   // content harmonic spacing
        const double spacing = std::max (2.5, f0c * N / fs);    // bins per harmonic
        pkB.clear(); pkV.clear();
        for (double cb = spacing; cb < (double)(NB - 2); cb += spacing)
        {
            const int a = std::max (1, (int)(cb - 0.45 * spacing));
            const int b = std::min (NB - 1, (int)(cb + 0.45 * spacing));
            int bm = a; float vm = mag[(size_t)a];
            for (int t = a + 1; t <= b; ++t)
                if (mag[(size_t)t] > vm) { vm = mag[(size_t)t]; bm = t; }
            pkB.push_back (bm);
            pkV.push_back (std::log (vm + 1.0e-12f));
        }
        if (pkB.size() >= 2)
        {
            size_t seg = 0;
            for (int k = 0; k <= NB; ++k)
            {
                float lv;
                if (k <= pkB.front())       lv = pkV.front();
                else if (k >= pkB.back())   lv = pkV.back();
                else
                {
                    while (seg + 1 < pkB.size() && pkB[seg + 1] < k) ++seg;
                    const int   a = pkB[seg], b = pkB[seg + 1];
                    const float t = (float)(k - a) / (float)(b - a);
                    lv = pkV[seg] * (1.0f - t) + pkV[seg + 1] * t;
                }
                env[(size_t)k] = std::exp (lv);
            }
        }
        else
            for (int k = 0; k <= NB; ++k) env[(size_t)k] = mag[(size_t)k] + 1.0e-12f;

        // light smoothing to remove interpolation kinks
        for (int pass = 0; pass < 2; ++pass)
        {
            prefix[0] = 0.0;
            for (int k = 0; k <= NB; ++k)
                prefix[(size_t)k+1] = prefix[(size_t)k] + (double) env[(size_t)k];
            for (int k = 0; k <= NB; ++k)
            {
                const int a = std::max (0, k - 3), b = std::min (NB, k + 3);
                envSm[(size_t)k] = (float) ((prefix[(size_t)b+1] - prefix[(size_t)a]) / (double)(b - a + 1));
            }
            std::swap (env, envSm);
        }

        auto binOf = [&] (double hz) { return std::clamp ((int) std::lround (hz * N / fs), 1, NB - 2); };
        auto hzOf  = [&] (double bin) { return bin * fs / N; };

        // formant peaks in scaled search ranges (grain content is already
        // resampled by the global ratio f, so ranges scale with f)
        const double loR[3] = { 250, 850, 1900 }, hiR[3] = { 1000, 2600, 3800 };
        const double defR[3] = { 500, 1500, 2500 };
        int sBin[3];
        for (int i = 0; i < 3; ++i)
        {
            const int a = binOf (loR[i] * f), b = binOf (std::min (hiR[i] * f, fs * 0.45));
            int    pk = -1; float pv = 0.0f;
            for (int k = a + 1; k < b - 1; ++k)
                if (env[(size_t)k] >= env[(size_t)k-1] && env[(size_t)k] >= env[(size_t)k+1]
                    && env[(size_t)k] > pv)
                { pv = env[(size_t)k]; pk = k; }
            const float hz = pk > 0 ? (float) hzOf (pk)
                                    : (trackF[i] > 0 ? trackF[i] : (float)(defR[i] * f));
            trackF[i] = trackF[i] > 0 ? 0.7f * trackF[i] + 0.3f * hz : hz;
            sBin[i] = binOf (trackF[i]);
        }
        sBin[1] = std::max (sBin[1], sBin[0] + binOf (150.0 * f));
        sBin[2] = std::max (sBin[2], sBin[1] + binOf (200.0 * f));

        // warp anchors: 0, F1, F2, F3, (F3 + 900*f Hz), Nyquist
        const int tail = std::min (NB - 2, sBin[2] + binOf (900.0 * f));
        int dBin[3];
        for (int i = 0; i < 3; ++i)
            dBin[i] = std::clamp ((int) std::lround (sBin[i] * fShiftRatio[i]),
                                  binOf (120.0 * f), tail - 1);
        dBin[1] = std::max (dBin[1], dBin[0] + binOf (120.0 * f));
        dBin[2] = std::max (dBin[2], dBin[1] + binOf (150.0 * f));
        dBin[2] = std::min (dBin[2], tail - 1);
        dBin[1] = std::min (dBin[1], dBin[2] - binOf (150.0 * f));
        dBin[0] = std::min (dBin[0], dBin[1] - binOf (120.0 * f));

        const int srcA[5] = { 0, sBin[0], sBin[1], sBin[2], tail };
        const int dstA[5] = { 0, dBin[0], dBin[1], dBin[2], tail };

        const float bwInv[3] = { 1.0f / std::max (4.0f, 0.22f * (float) dBin[0]),
                                 1.0f / std::max (4.0f, 0.22f * (float) dBin[1]),
                                 1.0f / std::max (4.0f, 0.22f * (float) dBin[2]) };
        const bool doBreath = breath > 0.001f;
        const int bLo = binOf (1200.0), bHi = binOf (3000.0);
        float envMax = 1.0e-12f;
        if (doBreath)
            for (int k = 0; k <= NB; ++k) envMax = std::max (envMax, env[(size_t)k]);

        for (int seg = 0; seg < 5; ++seg)
        {
            const int d0 = dstA[seg];
            const int d1 = seg < 4 ? dstA[seg+1] : NB;
            const int s0 = srcA[seg];
            const int s1 = seg < 4 ? srcA[seg+1] : NB;
            if (d1 <= d0) continue;
            const double slope = (double)(s1 - s0) / (double)(d1 - d0);

            for (int k = d0; k < d1; ++k)
            {
                const double sp = s0 + (k - d0) * slope;
                const int    si = std::min ((int) sp, NB - 1);
                const float  fracb = (float)(sp - si);
                const float  eSrc = env[(size_t)si] * (1.0f - fracb)
                                  + env[(size_t)std::min (si + 1, NB)] * fracb;
                float R = eSrc / std::max (env[(size_t)k], 1.0e-9f);

                float gDb = 0.0f;
                for (int i = 0; i < 3; ++i)
                {
                    const float z = (float)(k - dBin[i]) * bwInv[i];
                    gDb += fGainDb[i] * std::exp (-0.5f * z * z);
                }
                R *= std::pow (10.0f, gDb / 20.0f);
                R = std::clamp (R, 0.08f, 12.0f);

                float nr = fr[(size_t)k] * R, ni = fi[(size_t)k] * R;

                if (doBreath && k > bLo / 2)
                {
                    // Breathy phonation REPLACES the upper harmonics with
                    // aspiration noise (harmonic+noise model); merely adding
                    // noise on top of intact harmonics sounds electronic.
                    // So: fade the deterministic part down as the shaped
                    // noise (envelope-following, tilt-compensated) fades in.
                    const float hw   = std::clamp ((float)(k - bLo) / (float)(bHi - bLo), 0.0f, 1.0f);
                    const float mixN = breath * hw;               // 0..1 noise share
                    const float att  = 1.0f - 0.75f * mixN;       // harmonic fade-out
                    nr *= att;  ni *= att;
                    const float lift = std::min (14.0f, (float) k / (float) bLo);
                    const float nEnv = std::max (lift * R * env[(size_t)k], 0.10f * envMax);
                    const float g    = 0.45f * mixN * nEnv;
                    nr += g * nextRand();
                    ni += g * nextRand();
                }

                fr[(size_t)k] = nr;  fi[(size_t)k] = ni;
                if (k > 0 && k < NB)                        // conjugate mirror
                {
                    fr[(size_t)(N - k)] =  nr;
                    fi[(size_t)(N - k)] = -ni;
                }
            }
        }
        fi[0] = 0.0f; fi[(size_t)NB] = 0.0f;

        fftRadix2 (fr.data(), fi.data(), N, true);
        for (int i = 0; i < len; ++i)
            grainScratch[(size_t)i] = fr[(size_t)i];
    }

    static void fftRadix2 (float* re, float* im, int n, bool inv)
    {
        for (int i = 1, j = 0; i < n; ++i)
        {
            int bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j |= bit;
            if (i < j) { std::swap (re[i], re[j]); std::swap (im[i], im[j]); }
        }
        for (int half = 2; half <= n; half <<= 1)
        {
            const double ang = (inv ? 2.0 : -2.0) * M_PI / half;
            const double wr = std::cos (ang), wi = std::sin (ang);
            for (int i = 0; i < n; i += half)
            {
                double cr = 1.0, ci = 0.0;
                for (int k = 0; k < half / 2; ++k)
                {
                    const int a = i + k, b = i + k + half / 2;
                    const float xr = (float)(re[b] * cr - im[b] * ci);
                    const float xi = (float)(re[b] * ci + im[b] * cr);
                    re[b] = re[a] - xr;  im[b] = im[a] - xi;
                    re[a] += xr;         im[a] += xi;
                    const double ncr = cr * wr - ci * wi;
                    ci = cr * wi + ci * wr;  cr = ncr;
                }
            }
        }
        if (inv)
        {
            const float s = 1.0f / (float) n;
            for (int i = 0; i < n; ++i) { re[i] *= s; im[i] *= s; }
        }
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

    // ---------------- GCI-synchronous alignment (v0.7) ----------------
    // The glottal closure instant is the sharpest flank of each period;
    // locate it by short-time derivative energy (central difference, which
    // also attenuates HF noise), then keep the epoch train consistent from
    // period to period: a candidate near lastGci + k*P wins unless the
    // free-search peak is at least twice as strong. That hysteresis stops
    // the alignment from hopping between double-pulse candidates, which is
    // what causes shimmer with the plain peak search.
    double alignToGci (double c, float P)
    {
        if (gciHold > 0) { lastGci = -1; return alignToPeak (c, P); }
        const int r = std::max (6, (int) (P / 6.0f));
        const int64_t c0 = (int64_t) std::llround (c);
        const int64_t lo = c0 - r;
        gciE.assign ((size_t) (2 * r + 1), 0.0);

        double best = -1.0, sum = 0.0;
        int    bi = -1, cnt = 0;
        for (int j = 0; j <= 2 * r; ++j)
        {
            const int64_t i = lo + j;
            if (i < 3 || i + 3 >= writePos) continue;
            double e = 0.0;
            for (int64_t k = i - 2; k <= i + 2; ++k)
            {
                const double d = (double) inBuf[(size_t) ((k + 1) & kMask)]
                               - (double) inBuf[(size_t) ((k - 1) & kMask)];
                e += d * d;
            }
            gciE[(size_t) j] = e;
            sum += e; ++cnt;
            if (e > best) { best = e; bi = j; }
        }
        if (cnt < 5 || best <= 0.0) { lastGci = -1; return alignToPeak (c, P); }

        if (best < 2.5 * (sum / cnt))   // flat: no clear epoch (breathy/weak)
        { lastGci = -1; return alignToPeak (c, P); }

        int choice = bi;
        if (lastGci >= 0)
        {
            const double kP  = std::round ((double) (c0 - lastGci) / (double) P);
            const int    pj  = (int) (lastGci + (int64_t) std::llround (kP * (double) P) - lo);
            const int    w   = std::max (2, (int) (P / 12.0f));
            int pb = -1; double pv = -1.0;
            for (int j = std::max (0, pj - w); j <= std::min (2 * r, pj + w); ++j)
                if (gciE[(size_t) j] > pv) { pv = gciE[(size_t) j]; pb = j; }
            if (pb >= 0 && 2.0 * pv >= best)
                choice = pb;            // stay on the tracked epoch train
        }
        lastGci = lo + choice;
        return (double) lastGci;
    }

    float nextRand()   // fast LCG, uniform -1..1
    {
        rng = rng * 1664525u + 1013904223u;
        return (float) ((int32_t) rng) * (1.0f / 2147483648.0f);
    }

    // Catmull-Rom read from inBuf at a fractional position. Linear
    // interpolation is not enough here: its HF droop at half-sample offsets
    // stops the comb from cancelling the upper harmonics, leaking them into
    // the "noise" path (audible as a ghost of the un-shifted pitch).
    float readCubic (double p) const
    {
        const int64_t i1 = (int64_t) std::floor (p);
        const float t   = (float) (p - (double) i1);
        const float xm1 = inBuf[(size_t) ((i1 - 1) & kMask)];
        const float x0  = inBuf[(size_t) ( i1      & kMask)];
        const float x1  = inBuf[(size_t) ((i1 + 1) & kMask)];
        const float x2  = inBuf[(size_t) ((i1 + 2) & kMask)];
        return x0 + 0.5f * t * (x1 - xm1
                    + t * (2.0f*xm1 - 5.0f*x0 + 4.0f*x1 - x2
                    + t * (3.0f*(x0 - x1) + x2 - xm1)));
    }

    // ---------------- state ----------------
    static constexpr int kHoldMax = 24;   // ~250 ms of pitch hold (detect every ~512 samples)
    static constexpr int kFFT = 4096;     // spectral-layer FFT size

    double fs = 48000.0;
    int    D = 2048, pendingD = 2048, maxLag = 800, maxLagLow = 1200, minLag = 96, maxHout = 1200;
    int    holdCount = 0;
    int    effMaxLagCur = 800, houtCapCur = 1200;
    float  capHalfCur = 800.0f, guardFrac = 1.0f;

    std::vector<float>  inBuf, harmBuf, noiseBuf, accBuf, normBuf, tmp, tmpD, grainScratch;
    std::vector<float>  fr, fi, mag, env, envSm, pkV;
    std::vector<int>    pkB;
    std::vector<double> dbuf, prefix;
    float trackF[3] = { -1.0f, -1.0f, -1.0f };
    float fShiftRatio[3] = { 1.0f, 1.0f, 1.0f };
    float fGainDb[3] = { 0.0f, 0.0f, 0.0f };
    bool  perFmt = false;

    int64_t writePos = 0;
    double  nextMarkF = 0.0, lastInMark = 0.0;
    float   curP = 320.0f;
    bool    voiced = false;
    int     sinceDetect = 0;
    uint32_t rng = 0x1234567u;

    float pitchRatio = 1.0f, formantRatio = 1.0f, consonantRatio = 1.0f;
    float prSm = 1.0f, frSm = 1.0f, crSm = 1.0f;   // dezippered ratios
    float range = 1.0f, centerHz = 220.0f, breath = 0.0f, jitterAmt = 0.0f;
    float gLow = 1.0f, gHigh = 1.0f, tiltLp = 0.0f, tiltK = 0.12f;
    float mix = 1.0f, robotHz = 120.0f, floorHz = 0.0f;
    bool  robotize = false, lowVoice = false;
    float hiFreq = 0.0f, hiPAmt = 0.5f, hiFAmt = 1.0f;   // High Range guard

    // Air Preserve (harmonic/noise split) state
    float airAmt    = 0.0f;               // knob value
    float airKSplit = 0.0f;               // split coefficient (<= 2/3)
    float airBoost  = 1.0f;               // bypass emphasis (knob > 0.7)
    float airG   = 0.0f, airB = 0.0f;     // smoothed, voicing-gated gains
    float airP   = 320.0f;                // per-sample smoothed comb period
    float airLp1 = 0.0f, airLp2 = 0.0f;   // HP filter states (2x one-pole)
    float airK   = 0.004f, hpAirK = 0.17f;

    // GCI Grain Sync state
    bool    gciOn   = false;
    int     gciHold = 0;                  // detections left in glide fallback
    int64_t lastGci = -1;                 // tracked epoch (input position)
    std::vector<double> gciE;             // scratch: epoch-detector energies
};
