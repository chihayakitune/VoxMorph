// cadence_probe.cpp - does the analysis land on the same samples whatever the
// host buffer is?  (Deterministic Analysis Cadence, v0.62.0)
//
// The question this exists to answer is NOT "does the output sound the same".
// It is the one underneath that: the engine analyses a frame, decides a pitch
// and a voicing state, and books a release event against an output position.
// If the FRAME itself sits at a different absolute sample index for a buffer
// of 64 than for one of 512, every decision downstream inherits the offset and
// no amount of care in applying it can line the two up. So the primary report
// here is a diff of the detection log, row by row, field by field.
//
// Build:  g++ -O2 -std=c++17 -I dsp -o /tmp/cp test/cadence_probe.cpp
// Run:    /tmp/cp                 synthetic signal, all stages
//         /tmp/cp <voice.wav>     the same, on a real take (stays out of the
//                                 repo -- pass a path)
//
// Stages:
//   1  detection-grid identity: every buffer size and rate against the 512
//      reference, cadence OFF (shows the damage) and ON (must be clean).
//   2  Release Repair effect (ON - OFF) per buffer, in fixed ABSOLUTE
//      windows, so the spread across buffers is the number under test.
//   3  C0/C1/C2/C3 at one buffer: what the cadence alone does to the onset
//      leak and to the ending, with Repair off and on.
#define PSOLA_DETECT_LOG 1
#include "PsolaEngine.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include <limits>
#include <numeric>
#include <ctime>

// ---------------------------------------------------------------- wav in
static bool loadWav (const char* path, std::vector<float>& out, double& fs)
{
    FILE* f = std::fopen (path, "rb");
    if (! f) return false;
    std::vector<unsigned char> d;
    unsigned char buf[65536]; size_t n;
    while ((n = std::fread (buf, 1, sizeof buf, f)) > 0) d.insert (d.end(), buf, buf + n);
    std::fclose (f);
    if (d.size() < 44 || std::memcmp (d.data(), "RIFF", 4)) return false;
    size_t i = 12; int ch = 1, bits = 16; fs = 44100; bool isFloat = false;
    while (i + 8 <= d.size())
    {
        const char* id = (const char*) &d[i];
        unsigned sz; std::memcpy (&sz, &d[i + 4], 4);
        if (! std::memcmp (id, "fmt ", 4))
        {
            unsigned short fmt, c2, bps; unsigned sr;
            std::memcpy (&fmt, &d[i + 8],  2);
            std::memcpy (&c2,  &d[i + 10], 2);
            std::memcpy (&sr,  &d[i + 12], 4);
            std::memcpy (&bps, &d[i + 22], 2);
            ch = c2; bits = bps; fs = sr; isFloat = (fmt == 3);
        }
        else if (! std::memcmp (id, "data", 4))
        {
            const size_t bytes = std::min ((size_t) sz, d.size() - i - 8);
            const size_t step  = (size_t) (bits / 8) * (size_t) ch;
            const size_t frames = step ? bytes / step : 0;
            out.resize (frames);
            for (size_t k = 0; k < frames; ++k)
            {
                const unsigned char* s = &d[i + 8 + k * step];
                double acc = 0.0;
                for (int c3 = 0; c3 < ch; ++c3)
                {
                    const unsigned char* q = s + (size_t) c3 * (size_t) (bits / 8);
                    if (isFloat && bits == 32) { float v; std::memcpy (&v, q, 4); acc += v; }
                    else if (bits == 16) { short v; std::memcpy (&v, q, 2); acc += v / 32768.0; }
                    else if (bits == 24)
                    {
                        int v = (int) ((unsigned) q[0] | ((unsigned) q[1] << 8) | ((unsigned) q[2] << 16));
                        if (v & 0x800000) v -= 0x1000000;
                        acc += v / 8388608.0;
                    }
                    else if (bits == 32) { int v; std::memcpy (&v, q, 4); acc += v / 2147483648.0; }
                }
                out[k] = (float) (acc / ch);
            }
            return true;
        }
        i += 8 + sz + (sz & 1);
    }
    return false;
}

// ------------------------------------------------------ synthetic material
// Eight phrases with real attacks and decays, a glide, a vibrato stretch, an
// unvoiced burst and digital silence between them. The endings matter as much
// as the onsets here, so every phrase decays over 100 ms rather than cutting.
static std::vector<float> makeSpeech (double fs, double sec = 8.0)
{
    const int n = (int) (fs * sec);
    std::vector<float> x ((size_t) n, 0.0f);
    unsigned rng = 22222u;
    auto noise = [&] { rng = rng * 1664525u + 1013904223u;
                       return (float) ((int) (rng >> 9) - 4194304) / 4194304.0f; };
    double ph = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double t   = i / fs;
        const double loc = std::fmod (t, 1.0);       // one phrase per second
        const int    idx = (int) (t / 1.0);
        double f0 = 108.0 + 14.0 * (idx % 3);        // a different pitch per phrase
        if (idx == 4) f0 *= std::pow (2.0, 0.45 * loc);            // glide
        if (idx == 6) f0 *= 1.0 + 0.035 * std::sin (2.0 * M_PI * 5.5 * t);  // vibrato
        ph += f0 / fs;  if (ph >= 1.0) ph -= 1.0;
        double s = 0.0;
        for (int k = 1; k <= 24; ++k)
            s += std::exp (-0.17 * (k - 1)) * std::sin (2.0 * M_PI * k * ph);
        double env = 0.0;
        if (loc > 0.15 && loc < 0.62)      env = std::min (1.0, (loc - 0.15) / 0.030);
        else if (loc >= 0.62 && loc < 0.72) env = std::max (0.0, 1.0 - (loc - 0.62) / 0.10);
        double v = 0.26 * env * s;
        if (idx == 2 && loc > 0.75 && loc < 0.88) v = 0.09 * noise();   // unvoiced burst
        x[(size_t) i] = (float) (v + 0.0015 * env * noise());
    }
    return x;
}

// ---------------------------------------------------------------- running
struct Run
{
    std::vector<float> out;
    std::vector<PsolaEngine::DetectLogRow> log;
    int late = 0, dropped = 0, overflow = 0;
};

// blocks cycles through the given sizes, so a variable-buffer host (which is
// what a real DAW under load actually is) gets exercised too
static Run runEngine (const std::vector<float>& in, double fs,
                      const PsolaEngine::Params& p, const std::vector<int>& blocks)
{
    PsolaEngine e;
    e.prepare (fs);
    e.setParams (p);
    Run r;
    r.out.assign (in.size(), 0.0f);
    size_t i = 0, b = 0;
    while (i < in.size())
    {
        const int c = (int) std::min ((size_t) blocks[b % blocks.size()], in.size() - i);
        e.process (in.data() + i, r.out.data() + i, c);
        i += (size_t) c; ++b;
    }
    r.log = e.detectLog;
    r.late = e.releaseLateEvents(); r.dropped = e.releaseDropped(); r.overflow = e.releaseOverflow();
    return r;
}

// how far apart are two detection logs? returns the number of differing rows,
// and fills `first` with a description of the first difference found
static int logDiff (const std::vector<PsolaEngine::DetectLogRow>& a,
                    const std::vector<PsolaEngine::DetectLogRow>& b,
                    double fs, std::string& first)
{
    int bad = 0;
    const size_t m = std::min (a.size(), b.size());
    for (size_t i = 0; i < m; ++i)
    {
        const auto& x = a[i]; const auto& y = b[i];
        // the frame POSITION first: everything else is downstream of it
        const bool same = (int64_t) std::llround (x.t * fs) == (int64_t) std::llround (y.t * fs)
                       && x.lag == y.lag && x.pick == y.pick
                       && x.voicedAfter == y.voicedAfter && x.confident == y.confident
                       && x.unvoicedRun == y.unvoicedRun && x.holdCount == y.holdCount
                       && x.preHold == y.preHold && x.why == y.why
                       && std::fabs (x.curP - y.curP) < 1.0e-6f
                       && std::fabs (x.zcr  - y.zcr)  < 1.0e-9f
                       && std::fabs (x.energy - y.energy) <= 1.0e-12 * std::fabs (x.energy);
        if (! same)
        {
            if (! bad)
            {
                char buf[256];
                std::snprintf (buf, sizeof buf,
                               "row %zu: pos %lld vs %lld, lag %d vs %d, voiced %d vs %d, f0 %.2f vs %.2f",
                               i, (long long) std::llround (x.t * fs), (long long) std::llround (y.t * fs),
                               x.lag, y.lag, (int) x.voicedAfter, (int) y.voicedAfter,
                               x.f0After, y.f0After);
                first = buf;
            }
            ++bad;
        }
    }
    bad += (int) (std::max (a.size(), b.size()) - m);
    if (a.size() != b.size() && first.empty())
    {
        char buf[128];
        std::snprintf (buf, sizeof buf, "row count %zu vs %zu", a.size(), b.size());
        first = buf;
    }
    return bad;
}

// ------------------------------------------------------------- band energy
// One transform, several bands: the sweep below asks for five bands over the
// same window hundreds of times, and re-transforming per band is the whole
// runtime of the probe.
static void bandsDb (const std::vector<float>& x, double fs, int64_t a, int n,
                     const double (*bands)[2], int nb, double* out)
{
    if (a < 0 || a + n > (int64_t) x.size())
    {
        for (int k = 0; k < nb; ++k) out[k] = -200.0;
        return;
    }
    const int N = 32768;
    static std::vector<float> re, im;
    re.assign ((size_t) N, 0.0f);  im.assign ((size_t) N, 0.0f);
    for (int i = 0; i < n && i < N; ++i)
        re[(size_t) i] = x[(size_t) (a + i)] * (0.5f - 0.5f * std::cos (2.0f * (float) M_PI * i / n));
    PsolaEngine::fftForViz (re.data(), im.data(), N);
    for (int b = 0; b < nb; ++b)
    {
        double acc = 0.0;
        for (int k = (int) (bands[b][0] * N / fs); k <= (int) (bands[b][1] * N / fs) && k <= N / 2; ++k)
            acc += re[(size_t) k] * re[(size_t) k] + im[(size_t) k] * im[(size_t) k];
        out[b] = 10.0 * std::log10 (acc + 1e-20);
    }
}

static double bandDb (const std::vector<float>& x, double fs, int64_t a, int n,
                      double lo, double hi)
{
    const double b[1][2] = { { lo, hi } };
    double v[1];
    bandsDb (x, fs, a, n, b, 1, v);
    return v[0];
}

// ------------------------------------------------------------ onset metric
// The phrase-start leak, measured the way the ending is: how much energy sits
// BELOW the converted pitch in the first 60 ms of each phrase, relative to the
// same band once the phrase is running. A leak is untransposed voice, so it
// lands under the new f0 and above the room. Higher = worse.
//
// Onsets come from the INPUT (they are a property of the take, not of the
// setting), so every condition is scored on exactly the same windows.
static std::vector<int64_t> findOnsets (const std::vector<float>& in, double fs)
{
    const int hop = (int) (fs * 0.010);
    std::vector<double> e;
    for (size_t i = 0; i + (size_t) hop <= in.size(); i += (size_t) hop)
    {
        double a = 0.0;
        for (int k = 0; k < hop; ++k) a += (double) in[i + k] * in[i + k];
        e.push_back (a / hop);
    }
    double peak = 0.0;
    for (double v : e) peak = std::max (peak, v);
    const double on = peak * 1.0e-3, off = peak * 3.0e-4;
    std::vector<int64_t> out;
    bool voiced = false; int quiet = 0;
    for (size_t i = 0; i < e.size(); ++i)
    {
        if (! voiced && e[i] > on && quiet >= 8)          // >=80 ms of quiet before
        { voiced = true; out.push_back ((int64_t) i * hop); quiet = 0; }
        else if (voiced && e[i] < off) { voiced = false; quiet = 1; }
        else if (! voiced) ++quiet;
    }
    return out;
}

// The engine's lookahead: every output sample corresponds to an input sample
// D earlier, so an input-derived window has to be shifted by D before it is
// read off the OUTPUT. Without this the "onset" window is mostly the silence
// in front of the onset and the measurement says nothing.
static constexpr int64_t kLookahead = 2048;

// Per-onset leak, in onset order. A NaN marks an onset that cannot be
// scored (the utterance is already over by the time the reference window
// lands, so there is nothing to be loud against); the position is kept so
// two conditions stay aligned onset by onset.
//
// The value is the level in the sub-f0 band over the first 60 ms, against
// the RUNNING PHRASE full band. Not the same narrow band later on: once the
// voice has been transposed out of it that band is nearly empty, and the
// ratio becomes a quotient of two small numbers that swings 20 dB on
// nothing. Broadband is also the question being asked -- how loud is the
// leak against the voice. 0 dB means as loud as the whole voice.
static std::vector<double> onsetLeaks (const std::vector<float>& out, double fs,
                                       const std::vector<int64_t>& onsets, double f0out)
{
    const int w = (int) (fs * 0.060);
    const double hi = std::max (90.0, f0out * 0.80);
    const double bands[1][2] = { { 60.0, hi } };
    std::vector<double> v;
    v.reserve (onsets.size());
    for (int64_t o : onsets)
    {
        double head[1];
        bandsDb (out, fs, o + kLookahead, w, bands, 1, head);
        // strongest of three positions, so a consonant or a dip 200 ms in
        // does not become the yardstick
        double ref = -200.0;
        for (double at : { 0.15, 0.20, 0.25 })
            ref = std::max (ref, bandDb (out, fs, o + kLookahead + (int64_t) (fs * at),
                                         w, 60.0, 5000.0));
        v.push_back ((head[0] > -190.0 && ref > -60.0) ? head[0] - ref
                                                       : std::numeric_limits<double>::quiet_NaN());
    }
    return v;
}

static void leakStats (std::vector<double> v, double& worst, double& p95,
                       double& p75, double& overMs, int& used)
{
    v.erase (std::remove_if (v.begin(), v.end(),
                             [] (double d) { return std::isnan (d); }), v.end());
    used = (int) v.size();
    std::sort (v.begin(), v.end());
    worst = v.empty() ? 0.0 : v.back();
    p95   = v.empty() ? 0.0 : v[(size_t) ((v.size() - 1) * 0.95)];
    p75   = v.empty() ? 0.0 : v[(size_t) ((v.size() - 1) * 0.75)];
    int over = 0;
    for (double d : v) if (d > 0.0) ++over;
    overMs = over * 60.0;
}

// ------------------------------------------------------------------- main
int main (int argc, char** argv)
{
    std::vector<float> real; double realFs = 0.0;
    const bool haveReal = argc > 1 && loadWav (argv[1], real, realFs);
    if (argc > 1 && ! haveReal) { std::fprintf (stderr, "cannot read %s\n", argv[1]); return 1; }

    using P = PsolaEngine::Params;
    const std::vector<int> BUFS = { 1, 31, 32, 63, 64, 127, 128, 255, 256, 480, 511, 512, 513, 1024 };
    const std::vector<std::vector<int>> VARIABLE = {
        { 32, 512, 64, 255 }, { 480, 64 }, { 1024, 31 }
    };

    // ---- stage 1: does the analysis grid move with the host buffer? ----
    std::printf ("=== 1. detection-grid identity (reference = buffer 512) ===\n");
    std::printf ("%-8s %-8s %10s %10s   %s\n", "rate", "buffer", "OFF-diff", "ON-diff", "first ON difference");
    int failures = 0;
    const double RATES[] = { 44100.0, 48000.0, 88200.0, 96000.0 };
    for (double fs : RATES)
    {
        auto in = makeSpeech (fs, 8.0);
        P base;  base.pitchSemi = 9.0f;  base.formantSemi = 4.0f;
        base.grainAvg = true;  base.pulseBody = 0.75f;  base.onsetHold = 3;
        base.onsetBackfill = true;  base.preLockLowCut = 0.75f;
        P det = base;  det.deterministicCadence = true;

        const auto refOff = runEngine (in, fs, base, { 512 });
        const auto refOn  = runEngine (in, fs, det,  { 512 });

        for (int b : BUFS)
        {
            std::string f1, f2;
            const int dOff = logDiff (runEngine (in, fs, base, { b }).log, refOff.log, fs, f1);
            const int dOn  = logDiff (runEngine (in, fs, det,  { b }).log, refOn.log,  fs, f2);
            if (dOn) ++failures;
            std::printf ("%-8.0f %-8d %10d %10d   %s\n", fs, b, dOff, dOn, f2.c_str());
        }
        for (const auto& seq : VARIABLE)
        {
            std::string f1, f2;
            const int dOff = logDiff (runEngine (in, fs, base, seq).log, refOff.log, fs, f1);
            const int dOn  = logDiff (runEngine (in, fs, det,  seq).log, refOn.log,  fs, f2);
            if (dOn) ++failures;
            char name[32];
            std::snprintf (name, sizeof name, "var%zu", seq.size());
            std::printf ("%-8.0f %-8s %10d %10d   %s\n", fs, name, dOff, dOn, f2.c_str());
        }
    }
    std::printf ("criterion 1: %s (%d conditions differ from the 512 reference)\n\n",
                 failures ? "FAIL" : "PASS", failures);

    // ---- stage 1b: how far does the identity actually go? ----
    // The detection log is the requirement, but the sharper question is
    // whether the SAMPLES come out the same. They do -- with Onset Repair
    // off, at every buffer size and every rate. With it on they cannot: its
    // rewind reaches back to the start of the chunk it fires in, and the
    // furthest it may legally reach IS one host chunk, because output
    // already handed over cannot be recalled. That residue is a property of
    // the repair, not of the cadence, and it lives only at phrase onsets.
    std::printf ("=== 1b. output identity across buffers (reference = 512) ===\n");
    std::printf ("%-8s %-14s %10s %12s %12s\n", "rate", "repair", "worst buf", "diff samples", "max |diff|");
    for (double fs : RATES)
    {
        auto in = makeSpeech (fs, 8.0);
        for (int rep = 0; rep < 2; ++rep)
        {
            P p;  p.pitchSemi = 9.0f;  p.formantSemi = 4.0f;
            p.grainAvg = true;  p.pulseBody = 0.75f;  p.onsetHold = 3;
            p.deterministicCadence = true;
            p.onsetBackfill = rep != 0;  p.preLockLowCut = rep ? 0.75f : 0.0f;
            const auto ref = runEngine (in, fs, p, { 512 });
            size_t worstN = 0;  double worstMax = 0.0;  int worstBuf = 0;
            auto check = [&] (const std::vector<int>& blocks, int label)
            {
                const auto r = runEngine (in, fs, p, blocks);
                size_t nd = 0;  double mx = 0.0;
                for (size_t i = 0; i < r.out.size(); ++i)
                    if (r.out[i] != ref.out[i])
                    { ++nd; mx = std::max (mx, (double) std::fabs (r.out[i] - ref.out[i])); }
                if (nd > worstN) { worstN = nd; worstMax = mx; worstBuf = label; }
            };
            for (int b : BUFS) check ({ b }, b);
            for (size_t s = 0; s < VARIABLE.size(); ++s) check (VARIABLE[s], -(int) s - 1);
            std::printf ("%-8.0f %-14s %10d %12zu %12.3g\n", fs,
                         rep ? "Onset Repair" : "repair off", worstBuf, worstN, worstMax);
            if (! rep && worstN) ++failures;
        }
    }
    std::printf ("\n");

    // ---- stage 2: Release Repair effect spread across buffers ----
    // effect(buffer) = ON - OFF measured in FIXED absolute windows, so the
    // only thing that can move the number is the engine, not the window.
    std::printf ("=== 2. Release Repair effect (ON - OFF), fixed absolute windows ===\n");
    // The bands: the speaker's own f0 region, the converted one, and three
    // wider bands. The first is the one the treatment is aimed at.
    const double BANDS[5][2] = { { 95.0, 125.0 }, { 170.0, 210.0 },
                                 { 60.0, 140.0 }, { 150.0, 600.0 }, { 600.0, 5000.0 } };
    double worstSpread = 0.0;
    for (double fs : RATES)
    {
        auto in = makeSpeech (fs, 8.0);
        // The endings are known: each phrase decays from loc 0.62 and is
        // silent by 0.72. What Release Repair exists to remove lives in the
        // LATE decay and the silence just after it, so the window starts a
        // little way into the decay -- and is shifted by the lookahead,
        // because it is being read off the output.
        std::vector<int64_t> ends;
        for (int k = 0; k < 8; ++k)
            ends.push_back ((int64_t) (fs * (k + 0.67)) + kLookahead);
        const int w = (int) (fs * 0.150);

        for (int mode = 0; mode < 2; ++mode)
        {
            std::printf ("-- %.0f Hz, cadence %s --\n", fs, mode ? "ON " : "OFF");
            std::printf ("%-8s %8s %8s %8s %8s %8s\n", "buffer", "inF0", "outF0", "60-140", "150-600", "600-5k");
            struct Row { double v[5]; };
            std::vector<Row> rows;
            auto measure = [&] (const std::vector<int>& blocks, const char* label)
            {
                P off;  off.pitchSemi = 9.0f;  off.formantSemi = 4.0f;
                off.grainAvg = true;  off.pulseBody = 0.75f;  off.onsetHold = 3;
                off.onsetBackfill = true;  off.preLockLowCut = 0.75f;
                off.deterministicCadence = mode != 0;
                P on = off;  on.releaseRepair = true;  on.releaseShelf = 0.50f;

                const auto ro = runEngine (in, fs, off, blocks);
                const auto rn = runEngine (in, fs, on,  blocks);
                if (rn.late || rn.dropped || rn.overflow)
                    std::printf ("   [queue] late %d dropped %d overflow %d\n",
                                 rn.late, rn.dropped, rn.overflow);
                Row r {};
                int cnt = 0;
                for (int64_t e0 : ends)
                {
                    double a[5], c[5];
                    bandsDb (ro.out, fs, e0, w, BANDS, 5, a);
                    bandsDb (rn.out, fs, e0, w, BANDS, 5, c);
                    if (a[0] <= -190.0 || c[0] <= -190.0) continue;
                    for (int k = 0; k < 5; ++k) r.v[k] += c[k] - a[k];
                    ++cnt;
                }
                if (cnt) for (int k = 0; k < 5; ++k) r.v[k] /= cnt;
                rows.push_back (r);
                std::printf ("%-8s %8.2f %8.2f %8.2f %8.2f %8.2f\n",
                             label, r.v[0], r.v[1], r.v[2], r.v[3], r.v[4]);
            };
            for (int b : BUFS)
            {
                char label[16];  std::snprintf (label, sizeof label, "%d", b);
                measure ({ b }, label);
            }
            for (size_t s = 0; s < VARIABLE.size(); ++s)
            {
                char label[16];  std::snprintf (label, sizeof label, "var%zu", s);
                measure (VARIABLE[s], label);
            }
            std::printf ("%-8s", "spread");
            for (int k = 0; k < 5; ++k)
            {
                double lo = 1e9, hi = -1e9;
                for (const auto& r : rows) { lo = std::min (lo, r.v[k]); hi = std::max (hi, r.v[k]); }
                std::printf (" %8.2f", hi - lo);
                if (mode) worstSpread = std::max (worstSpread, hi - lo);
            }
            std::printf ("\n");
        }
    }
    std::printf ("criterion 2: %s (worst spread with the cadence on: %.2f dB, "
                 "target 0.25, limit 0.50)\n\n",
                 worstSpread <= 0.50 ? (worstSpread <= 0.25 ? "PASS" : "PASS (over target)") : "FAIL",
                 worstSpread);
    if (worstSpread > 0.50) ++failures;

    // ---- stage 3: C0..C3 on the real take (or the synthetic one) ----
    std::printf ("=== 3. C0/C1/C2/C3 -- onset leak and ending, one buffer ===\n");
    {
        const double fs = haveReal ? realFs : 48000.0;
        const std::vector<float> in = haveReal ? real : makeSpeech (fs, 8.0);
        const auto onsets = findOnsets (in, fs);
        // Calibrate the leak band on the take rather than on a guess: the
        // engine's own median voiced f0 IS the speaker, and the band that
        // matters runs from the floor up to just under where the converted
        // voice now sits. Measured once, on C0, and reused for all four
        // cases so the four are scored on identical windows.
        double speakerF0 = 0.0;
        {
            P p0;  p0.pitchSemi = 9.0f;  p0.formantSemi = 4.0f;
            p0.grainAvg = true;  p0.pulseBody = 0.75f;  p0.onsetHold = 3;
            p0.onsetBackfill = true;  p0.preLockLowCut = 0.75f;
            const auto r0 = runEngine (in, fs, p0, { 256 });
            std::vector<float> f0s;
            for (const auto& row : r0.log)
                if (row.voicedAfter && row.f0After > 40.0f) f0s.push_back (row.f0After);
            std::sort (f0s.begin(), f0s.end());
            speakerF0 = f0s.empty() ? 150.0 : f0s[f0s.size() / 2];
        }
        const double f0out = speakerF0 * std::pow (2.0, 9.0 / 12.0);
        std::printf ("%s, %.0f Hz, %.1f s, %zu onsets, speaker f0 %.0f Hz -> %.0f Hz, "
                     "leak band 60-%.0f Hz\n",
                     haveReal ? argv[1] : "synthetic", fs, in.size() / fs, onsets.size(),
                     speakerF0, f0out, std::max (90.0, f0out * 0.80));
        std::printf ("%-4s %-10s %-8s %8s %8s %8s %9s %6s\n",
                     "case", "cadence", "repair", "worst", "p95", "p75", "over(ms)", "n");
        const char* names[4] = { "C0", "C1", "C2", "C3" };
        std::vector<double> leaks[4];
        for (int c = 0; c < 4; ++c)
        {
            P p;  p.pitchSemi = 9.0f;  p.formantSemi = 4.0f;
            p.grainAvg = true;  p.pulseBody = 0.75f;  p.onsetHold = 3;
            p.onsetBackfill = true;  p.preLockLowCut = 0.75f;
            p.deterministicCadence = (c & 1) != 0;
            if (c & 2) { p.releaseRepair = true;  p.releaseShelf = 0.50f; }
            const auto r = runEngine (in, fs, p, { 256 });
            leaks[c] = onsetLeaks (r.out, fs, onsets, f0out);
            double worst, p95, p75, overMs;  int used = 0;
            leakStats (leaks[c], worst, p95, p75, overMs, used);
            std::printf ("%-4s %-10s %-8s %8.2f %8.2f %8.2f %9.0f %6d\n",
                         names[c], (c & 1) ? "determin." : "legacy", (c & 2) ? "on" : "off",
                         worst, p95, p75, overMs, used);
        }

        // criterion 3, PAIRED. Comparing the p95 of one sorted list against
        // the p95 of another compares two DIFFERENT onsets, and on a take
        // with 200 of them that alone moves a decibel. The question is
        // whether the SAME onset got worse, so difference first, statistics
        // afterwards.
        {
            std::vector<double> d;
            for (size_t i = 0; i < leaks[0].size(); ++i)
                if (! std::isnan (leaks[0][i]) && ! std::isnan (leaks[1][i]))
                    d.push_back (leaks[1][i] - leaks[0][i]);
            std::sort (d.begin(), d.end());
            const double med  = d.empty() ? 0.0 : d[d.size() / 2];
            const double mean = d.empty() ? 0.0
                              : std::accumulate (d.begin(), d.end(), 0.0) / (double) d.size();
            const double hi95 = d.empty() ? 0.0 : d[(size_t) ((d.size() - 1) * 0.95)];
            int worseCount = 0;
            for (double x : d) if (x > 0.0) ++worseCount;
            // a wash is the expected result: the grid moves by a few tens of
            // samples, so individual onsets shift either way. What would be a
            // regression is a MEDIAN that has moved up.
            const bool ok = med <= 0.25 && mean <= 0.25;
            std::printf ("criterion 3: %s (C1 - C0 per onset, n=%zu: median %+.2f dB, "
                         "mean %+.2f dB, p95 of the shift %+.2f dB, worse at %d/%zu onsets)\n",
                         ok ? "PASS" : "FAIL", d.size(), med, mean, hi95, worseCount, d.size());
            if (! ok) ++failures;
        }
    }

    // ---- stage 4: cost, latency, queue health, live switching ----
    std::printf ("\n=== 4. cost, latency, queue health, live switching ===\n");
    {
        const double fs = 48000.0;
        auto in = makeSpeech (fs, 8.0);
        P base;  base.pitchSemi = 9.0f;  base.formantSemi = 4.0f;
        base.grainAvg = true;  base.pulseBody = 0.75f;  base.onsetHold = 3;
        base.onsetBackfill = true;  base.preLockLowCut = 0.75f;
        base.releaseRepair = true;  base.releaseShelf = 0.50f;
        base.airPreserve = 1.0f;   // the heaviest path, so the cost is the worst case

        for (int mode = 0; mode < 2; ++mode)
        {
            P p = base;  p.deterministicCadence = mode != 0;
            // three passes, keep the fastest: the question is the cost of the
            // work, not of whatever else the machine was doing
            double best = 1e9;
            for (int rep = 0; rep < 3; ++rep)
            {
                const clock_t t0 = std::clock();
                runEngine (in, fs, p, { 256 });
                best = std::min (best, (double) (std::clock() - t0) / CLOCKS_PER_SEC);
            }
            PsolaEngine e;  e.prepare (fs);  e.setParams (p);
            std::printf ("cadence %s: %.1f%% of one core, engine latency %d samples (%.1f ms)\n",
                         mode ? "ON " : "OFF", 100.0 * best / (in.size() / fs),
                         e.latencySamples(), 1000.0 * e.latencySamples() / fs);
        }

        // the release queue must stay healthy at every buffer size, not just
        // the ones that divide 512
        int worstLate = 0, worstDrop = 0, worstOver = 0;
        for (int b : BUFS)
        {
            P p = base;  p.deterministicCadence = true;
            const auto r = runEngine (in, fs, p, { b });
            worstLate = std::max (worstLate, r.late);
            worstDrop = std::max (worstDrop, r.dropped);
            worstOver = std::max (worstOver, r.overflow);
        }
        for (const auto& seq : VARIABLE)
        {
            P p = base;  p.deterministicCadence = true;
            const auto r = runEngine (in, fs, p, seq);
            worstLate = std::max (worstLate, r.late);
            worstDrop = std::max (worstDrop, r.dropped);
            worstOver = std::max (worstOver, r.overflow);
        }
        std::printf ("release queue over every buffer: late %d, dropped %d, overflow %d  %s\n",
                     worstLate, worstDrop, worstOver,
                     (worstLate || worstDrop || worstOver) ? "FAIL" : "PASS");
        if (worstLate || worstDrop || worstOver) ++failures;

        // Live switching. The grid is counted from the start of the stream,
        // so turning the mode on halfway has to re-seat it rather than replay
        // every frame since. Toggle it repeatedly, mid-phrase and mid-silence,
        // and check the output stays finite and bounded.
        {
            P p = base;  p.deterministicCadence = false;
            PsolaEngine e;  e.prepare (fs);  e.setParams (p);
            std::vector<float> out (in.size(), 0.0f);
            size_t i = 0;  int blk = 0;  double peak = 0.0;  int bad = 0;
            while (i < in.size())
            {
                const int c = (int) std::min ((size_t) 256, in.size() - i);
                if ((blk % 17) == 0)            // ~90 ms, deliberately not a phrase period
                { p.deterministicCadence = ! p.deterministicCadence;  e.setParams (p); }
                e.process (in.data() + i, out.data() + i, c);
                for (int k = 0; k < c; ++k)
                {
                    const float v = out[i + (size_t) k];
                    if (! std::isfinite (v)) ++bad;
                    else peak = std::max (peak, (double) std::fabs (v));
                }
                i += (size_t) c;  ++blk;
            }
            std::printf ("live toggling every 17 blocks: %d non-finite samples, peak %.3f  %s\n",
                         bad, peak, (bad == 0 && peak < 4.0) ? "PASS" : "FAIL");
            if (bad != 0 || peak >= 4.0) ++failures;
        }
    }

    std::printf ("\n%s\n", failures ? "SOME CHECKS FAILED" : "ALL CHECKS PASS");
    return failures ? 1 : 0;
}
