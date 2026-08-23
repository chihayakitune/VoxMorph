// release_state_probe.cpp - does the ONSET/RELEASE state machine call the
// right things endings?  (v0.62.2)
//
// Release Repair only helps if RELEASE means "the phrase is ending". Two ways
// that can be wrong, and they fail in opposite directions:
//
//   FALSE POSITIVE  something inside a phrase looks like a decay -- vibrato,
//                   tremolo, a dip between two vowels, a rumble under the
//                   voice -- and the ending shelf fires in the middle of the
//                   sound. This is the expensive one: it takes the bottom out
//                   of a voice that is still going.
//   MISS            a real ending does not look like a decay -- a fricative,
//                   a plosive stop, a nasal murmur, a breathy fade -- and the
//                   treatment never runs.
//
// So this generates one phrase per case with a KNOWN ending time, reads the
// state machine's own log, and scores the entries into RELEASE against it.
// It also measures what the shelf actually did to the phrase BODY, because a
// misclassification that costs nothing audible is a different problem from
// one that eats the voice.
//
// Analysis Cadence is pinned ON throughout: per the 2026-08-23 decision note
// that is the baseline condition for Release Repair work, and it is the only
// way these numbers mean the same thing at two buffer sizes.
//
// Build:  g++ -O2 -std=c++17 -I dsp -o /tmp/rsp test/release_state_probe.cpp
// Run:    /tmp/rsp              the misclassification grid
//         /tmp/rsp strength     the Safe Strength sweep as well
#define PSOLA_DETECT_LOG 1
#include "PsolaEngine.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>

// ------------------------------------------------------------- test signals
// Every case is the same skeleton -- silence, attack, 700 ms of phrase, an
// ending, silence -- so the only thing that varies is the thing under test.
// `endAt` is when the VOICE stops, which is the answer the state machine is
// being marked against.
struct Case
{
    const char*        name;
    std::vector<float> x;
    double             relFrom;    // when the phrase STARTS ending
    double             relTo;      // when the voice has finished
    double             bodyFrom, bodyTo;   // a stretch that is unambiguously
};                                         // mid-phrase, for damage measurement

static float frand (unsigned& r)
{
    r = r * 1664525u + 1013904223u;
    return (float) ((int) (r >> 9) - 4194304) / 4194304.0f;
}

// `depth` < 0 asks for the case's own default. For tremolo it is the
// modulation depth (0 = none, 1 = the level reaches zero); for the dip it is
// the level the vowel falls TO before recovering.
static Case makeCase (int which, double fs, double depth = -1.0)
{
    if (depth < 0.0) depth = (which == 2) ? 0.40 : 0.25;
    const double sec = 1.6;
    const int    n   = (int) (fs * sec);
    std::vector<float> x ((size_t) n, 0.0f);
    unsigned rng = 9001u;

    const double t0 = 0.25;          // attack
    const double t1 = 0.95;          // start of the ending
    double endAt = t1 + 0.12;

    // a one-pole state for the shaped-noise endings
    float lp = 0.0f, hp = 0.0f;
    double ph = 0.0;

    for (int i = 0; i < n; ++i)
    {
        const double t = i / fs;
        double f0 = 120.0;
        double amp = 0.0;
        double noiseMix = 0.0;
        double lpCut = 0.0;          // >0 = low-pass the harmonics (nasal)

        // ---- the phrase body, and what is odd about it -------------------
        if (t >= t0 && t < t1)
        {
            amp = std::min (1.0, (t - t0) / 0.030);
            switch (which)
            {
                case 0: break;                                   // plain
                case 1: f0 *= 1.0 + 0.045 * std::sin (2.0 * M_PI * 5.5 * t); break;   // vibrato
                case 2:                                          // tremolo
                    amp *= 1.0 - depth * (0.5 - 0.5 * std::cos (2.0 * M_PI * 6.0 * t));
                    break;
                case 3:                                          // dip and recovery
                    if (t > 0.60 && t < 0.66) amp *= depth;
                    else if (t >= 0.66 && t < 0.70) amp *= depth + (1.0 - depth) * (t - 0.66) / 0.04;
                    break;
                case 4: break;                                   // rumble, added below
                case 8:                                          // descending glide
                    f0 *= std::pow (2.0, -1.0 * (t - t0) / (t1 - t0));
                    break;
                case 9: amp *= 0.06; break;                      // quiet voice
                default: break;
            }
        }
        // ---- the ending ---------------------------------------------------
        else if (t >= t1)
        {
            switch (which)
            {
                case 5:                                          // breathy fade
                    amp = std::max (0.0, 1.0 - (t - t1) / 0.18);
                    noiseMix = std::min (1.0, (t - t1) / 0.09);
                    endAt = t1 + 0.18;
                    break;
                case 6:                                          // nasal murmur
                    amp = std::max (0.0, 1.0 - (t - t1) / 0.20) * 0.55;
                    lpCut = 700.0;
                    endAt = t1 + 0.20;
                    break;
                case 7:                                          // fricative
                    amp = 0.0;  noiseMix = (t < t1 + 0.15) ? 1.0 : 0.0;
                    endAt = t1;
                    break;
                case 10:                                         // plosive stop
                    amp = 0.0;
                    noiseMix = (t > t1 + 0.05 && t < t1 + 0.07) ? 1.0 : 0.0;
                    endAt = t1;
                    break;
                default:                                         // ordinary decay
                    amp = std::max (0.0, 1.0 - (t - t1) / 0.12);
                    break;
            }
            if (which == 9) amp *= 0.06;
        }

        ph += f0 / fs;  if (ph >= 1.0) ph -= 1.0;
        double s = 0.0;
        for (int k = 1; k <= 24; ++k)
        {
            double g = std::exp (-0.17 * (k - 1));
            if (lpCut > 0.0)                       // nasal: damp everything high
                g /= 1.0 + std::pow (k * f0 / lpCut, 2.0);
            s += g * std::sin (2.0 * M_PI * k * ph);
        }

        double v = 0.26 * amp * (1.0 - noiseMix) * s;

        if (noiseMix > 0.0)
        {
            const float w = frand (rng);
            if (which == 7 || which == 10)         // fricative / burst: high band
            { hp = 0.7f * hp + 0.3f * w;  v += 0.10 * noiseMix * (w - hp); }
            else                                    // breath: broad, soft
            { lp = 0.85f * lp + 0.15f * w;  v += 0.06 * noiseMix * lp; }
        }

        if (which == 4)                            // low-frequency rumble, throughout
            v += 0.030 * std::sin (2.0 * M_PI * 42.0 * t);

        x[(size_t) i] = (float) (v + 0.0008 * frand (rng));
    }

    static const char* names[] = {
        "plain ending (control)", "vibrato", "tremolo", "dip and recovery",
        "low-frequency rumble", "breathy ending", "nasal ending",
        "fricative ending", "descending glide", "quiet voice", "plosive stop"
    };
    return { names[which], std::move (x), t1, endAt, 0.45, 0.58 };
}
static constexpr int kCases = 11;

// ------------------------------------------------------------------ running
struct Run
{
    std::vector<float> out;
    std::vector<PsolaEngine::StateLogRow> st;
    int late = 0, dropped = 0, overflow = 0;
};

// Set from main: the master note fixes Cadence OFF and Formant +2 for the
// Release Pitch Continuation work, while the v0.62.2 misclassification grid
// was taken with Cadence ON and +4. Both conditions have to stay reachable,
// so they are parameters of the run rather than constants.
static bool  gCadence = true;
static float gFormant = 4.0f;
static bool  gContinuation = false;

static Run runEngine (const std::vector<float>& in, double fs, bool repair,
                      float strength, int blk)
{
    PsolaEngine::Params p;
    p.pitchSemi = 9.0f;  p.formantSemi = gFormant;
    p.grainAvg = true;   p.pulseBody = 0.75f;  p.onsetHold = 3;
    p.onsetBackfill = true;  p.preLockLowCut = 0.75f;
    p.releasePitchContinuation = gContinuation;
    p.deterministicCadence = gCadence;
    p.releaseRepair = repair;  p.releaseShelf = strength;

    PsolaEngine e;
    e.prepare (fs);
    e.setParams (p);
    Run r;
    r.out.assign (in.size(), 0.0f);
    for (size_t i = 0; i < in.size(); i += (size_t) blk)
        e.process (in.data() + i, r.out.data() + i,
                   (int) std::min ((size_t) blk, in.size() - i));
    r.st = e.stateLog;
    r.late = e.releaseLateEvents(); r.dropped = e.releaseDropped(); r.overflow = e.releaseOverflow();
    return r;
}

// ------------------------------------------------------------- band measure
static double bandDb (const std::vector<float>& x, double fs, int64_t a, int n,
                      double lo, double hi)
{
    if (a < 0 || a + n > (int64_t) x.size()) return -200.0;
    const int N = 32768;
    std::vector<float> re ((size_t) N, 0.0f), im ((size_t) N, 0.0f);
    for (int i = 0; i < n && i < N; ++i)
        re[(size_t) i] = x[(size_t) (a + i)] * (0.5f - 0.5f * std::cos (2.0f * (float) M_PI * i / n));
    PsolaEngine::fftForViz (re.data(), im.data(), N);
    double acc = 0.0;
    for (int k = (int) (lo * N / fs); k <= (int) (hi * N / fs) && k <= N / 2; ++k)
        acc += re[(size_t) k] * re[(size_t) k] + im[(size_t) k] * im[(size_t) k];
    return 10.0 * std::log10 (acc + 1e-20);
}

// ------------------------------------------------ RELEASE entries from a log
struct Entry { double at; int frames; };

static std::vector<Entry> releaseEntries (const std::vector<PsolaEngine::StateLogRow>& st)
{
    std::vector<Entry> out;
    bool in = false;
    for (size_t i = 0; i < st.size(); ++i)
    {
        const bool rel = st[i].state == 1;
        if (rel && ! in) { out.push_back ({ st[i].t, 1 }); in = true; }
        else if (rel)    { ++out.back().frames; }
        else             { in = false; }
    }
    return out;
}

static constexpr int64_t kLookahead = 2048;

int main (int argc, char** argv)
{
    const double fs = 48000.0;
    bool doSweep = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if      (a == "strength") doSweep = true;
        else if (a == "contin")   { gContinuation = true; gCadence = false; gFormant = 2.0f; }
    }
    int failures = 0;

    // ------------------------------------------------------------ part 1
    std::printf ("=== 1. does RELEASE mean \"this phrase is ending\"? ===\n");
    std::printf ("Cadence %s, Formant %+.0f, Pitch Continuation %s. Repair ON 0.50, buffer 256.\n",
                 gCadence ? "ON" : "OFF", gFormant, gContinuation ? "ON" : "OFF");
    std::printf ("The phrase starts ending at `from` and the voice is finished by `to`. An\n");
    std::printf ("entry before from-30 ms is a false positive; [from-30 ms, to+150 ms] is right.\n\n");
    std::printf ("%-24s %6s %5s %5s %6s %8s %8s %9s\n", "case", "end", "entr", "false",
                 "missed", "1-frame", ">=2-fr", "body dmg");

    int totalFalse = 0, totalMissed = 0;
    double worstBodyDamage = 0.0;
    for (int c = 0; c < kCases; ++c)
    {
        const Case cs = makeCase (c, fs);
        const auto on  = runEngine (cs.x, fs, true,  0.50f, 256);
        const auto off = runEngine (cs.x, fs, false, 0.0f,  256);

        const auto entries = releaseEntries (on.st);
        int nFalse = 0, one = 0, many = 0;
        bool hit = false;
        for (const auto& e : entries)
        {
            if (e.frames == 1) ++one; else ++many;
            if (e.at < cs.relFrom - 0.030) ++nFalse;
            else if (e.at <= cs.relTo + 0.150) hit = true;
        }
        const bool missed = ! hit;

        // What did it cost the phrase body? The voice band, mid-phrase,
        // shifted by the lookahead because this is read off the output.
        const int64_t a = (int64_t) (fs * cs.bodyFrom) + kLookahead;
        const int     w = (int) (fs * (cs.bodyTo - cs.bodyFrom));
        const double dmg = bandDb (on.out, fs, a, w, 150.0, 600.0)
                         - bandDb (off.out, fs, a, w, 150.0, 600.0);

        totalFalse  += nFalse;
        totalMissed += missed ? 1 : 0;
        worstBodyDamage = std::min (worstBodyDamage, dmg);

        std::printf ("%-24s %5.2f %5.2f %5zu %5d %6s %8d %8d %+8.2f dB%s\n",
                     cs.name, cs.relFrom, cs.relTo, entries.size(), nFalse,
                     missed ? "YES" : "-", one, many, dmg,
                     (nFalse || missed) ? "  <--" : "");
        if (on.late || on.dropped || on.overflow)
        { std::printf ("    queue: late %d dropped %d overflow %d\n",
                       on.late, on.dropped, on.overflow); ++failures; }
    }
    std::printf ("\nfalse positives %d, missed endings %d, worst body damage %+.2f dB\n",
                 totalFalse, totalMissed, worstBodyDamage);

    // ------------------------------------------------------------ part 1b
    // Nothing misfires at ordinary depths, which is the answer but not the
    // whole answer: a bar that nothing touches says nothing about how close
    // the nearest thing is. So push the two amplitude cases until the state
    // machine does break, and report where.
    std::printf ("\n=== 1b. how far is the nearest false positive? ===\n");
    std::printf ("%-10s %8s %6s %10s   %s\n", "case", "depth", "false", "body dmg", "");
    double firstBadTrem = -1.0, firstBadDip = -1.0;
    for (int which : { 2, 3 })
        for (double depth : (which == 2
                ? std::vector<double> { 0.30, 0.40, 0.55, 0.70, 0.85, 0.95, 1.00 }
                : std::vector<double> { 0.35, 0.25, 0.15, 0.08, 0.04, 0.02, 0.00 }))
        {
            const Case cs = makeCase (which, fs, depth);
            const auto on  = runEngine (cs.x, fs, true,  0.50f, 256);
            const auto off = runEngine (cs.x, fs, false, 0.0f,  256);
            int nFalse = 0;
            for (const auto& e : releaseEntries (on.st))
                if (e.at < cs.relFrom - 0.030) ++nFalse;
            const int64_t a = (int64_t) (fs * cs.bodyFrom) + kLookahead;
            const int     w = (int) (fs * (cs.bodyTo - cs.bodyFrom));
            const double dmg = bandDb (on.out,  fs, a, w, 150.0, 600.0)
                             - bandDb (off.out, fs, a, w, 150.0, 600.0);
            if (nFalse)
            {
                if (which == 2 && firstBadTrem < 0.0) firstBadTrem = depth;
                if (which == 3 && firstBadDip  < 0.0) firstBadDip  = depth;
            }
            std::printf ("%-10s %8.2f %6d %+9.2f dB   %s\n",
                         which == 2 ? "tremolo" : "dip", depth, nFalse, dmg,
                         nFalse ? "<-- misfires" : "");
        }
    std::printf ("nearest false positive: tremolo depth %s, dip level %s\n",
                 firstBadTrem < 0.0 ? "none up to 1.00"
                                    : (std::to_string (firstBadTrem).substr (0, 4)).c_str(),
                 firstBadDip  < 0.0 ? "none down to 0.00"
                                    : (std::to_string (firstBadDip).substr (0, 4)).c_str());

    // ------------------------------------------------------------ part 2
    // One frame is 512 samples ~ 10.7 ms. The state machine acts on the FIRST
    // frame that stops looking voiced, so a single-frame wobble is enough to
    // enter RELEASE. Whether that matters depends on whether it also lasts
    // long enough for the shelf's attack to reach anything.
    std::printf ("\n=== 2. one frame versus two ===\n");
    std::printf ("%-24s %10s %10s %10s\n", "case", "shortest", "longest", "median");
    for (int c = 0; c < kCases; ++c)
    {
        const Case cs = makeCase (c, fs);
        const auto on = runEngine (cs.x, fs, true, 0.50f, 256);
        auto entries = releaseEntries (on.st);
        if (entries.empty()) { std::printf ("%-24s %10s\n", cs.name, "(none)"); continue; }
        std::vector<int> len;
        for (const auto& e : entries) len.push_back (e.frames);
        std::sort (len.begin(), len.end());
        std::printf ("%-24s %10d %10d %10d\n", cs.name, len.front(), len.back(),
                     len[len.size() / 2]);
    }

    // ------------------------------------------------------------ part 3
    if (doSweep)
    {
        // Safe Strength. Two columns from the same runs:
        //
        //   benefit  what the shelf removes at a REAL ending, averaged over
        //            all eleven cases at their ordinary settings.
        //   damage   what it costs when the state machine is wrong. Part 1b
        //            shows misclassification only happens once the sound
        //            actually reaches zero, so the damage column is taken
        //            from those two cases -- the worst realistic mistake,
        //            which is what a value called "safe" has to survive.
        //
        // Benefit saturates and damage does not, so the number to pick is the
        // knee, not the maximum.
        std::printf ("\n=== 3. Safe Strength sweep ===\n");
        std::printf ("benefit = ending band change over the 11 cases (more negative = more removed)\n");
        std::printf ("damage  = worst mid-phrase voice band change over the misfiring cases\n\n");
        std::printf ("%8s %13s %15s %12s %9s\n", "strength", "benefit inF0",
                     "benefit 60-140", "damage", "benefit/dmg");
        double bestRatioAt = 0.0, bestScore = -1.0;
        for (float sv : { 0.20f, 0.25f, 0.30f, 0.35f, 0.40f, 0.50f, 0.60f, 0.75f, 1.00f })
        {
            double benA = 0.0, benB = 0.0;  int nb = 0;
            for (int c = 0; c < kCases; ++c)
            {
                const Case cs = makeCase (c, fs);
                const auto on  = runEngine (cs.x, fs, true,  sv,   256);
                const auto off = runEngine (cs.x, fs, false, 0.0f, 256);
                const int64_t e0 = (int64_t) (fs * (cs.relFrom - 0.02)) + kLookahead;
                const int     we = (int) (fs * 0.150);
                const double a1 = bandDb (on.out,  fs, e0, we, 95.0, 125.0)
                                - bandDb (off.out, fs, e0, we, 95.0, 125.0);
                const double a2 = bandDb (on.out,  fs, e0, we, 60.0, 140.0)
                                - bandDb (off.out, fs, e0, we, 60.0, 140.0);
                if (a1 > -190.0) { benA += a1; benB += a2; ++nb; }
            }
            double dmg = 0.0;
            const std::pair<int,double> stress[2] = { { 2, 1.00 }, { 3, 0.00 } };
            for (const auto& sp : stress)
            {
                const Case cs = makeCase (sp.first, fs, sp.second);
                const auto on  = runEngine (cs.x, fs, true,  sv,   256);
                const auto off = runEngine (cs.x, fs, false, 0.0f, 256);
                const int64_t b0 = (int64_t) (fs * cs.bodyFrom) + kLookahead;
                const int     wb = (int) (fs * (cs.bodyTo - cs.bodyFrom));
                dmg = std::min (dmg, bandDb (on.out,  fs, b0, wb, 150.0, 600.0)
                                   - bandDb (off.out, fs, b0, wb, 150.0, 600.0));
            }
            const double ben = nb ? -benA / nb : 0.0;
            const double ratio = dmg < -1.0e-6 ? ben / -dmg : (ben > 0.0 ? 1e9 : 0.0);
            // the knee: how much benefit is still being bought per dB of
            // worst-case damage, weighted by how much benefit there is at all
            const double score = ben * ratio;
            if (score > bestScore) { bestScore = score; bestRatioAt = sv; }
            std::printf ("%8.2f %13.2f %15.2f %12.2f %9.2f\n", sv,
                         nb ? benA / nb : 0.0, nb ? benB / nb : 0.0, dmg, ratio);
        }
        std::printf ("\nknee (benefit x benefit-per-damage) falls at strength %.2f\n", bestRatioAt);
    }

    // ------------------------------------------------------------ part 4
    // Release Strength is read on every sample inside releaseStage, so before
    // v0.62.2 moving it stepped the shelf. Worst case is moving it DURING a
    // decay, which is exactly where the ear is listening for the thing the
    // feature exists to remove. Measure the first difference around the move
    // against a run that does not move it.
    std::printf ("\n=== 4. changing Release Strength mid-decay ===\n");
    std::printf ("%-22s %10s %11s %11s %12s %13s\n", "move", "at", "step (1 ms)",
                 "at 100 ms", "step vs sig", "step abs");
    {
        const Case cs = makeCase (0, fs);          // plain ending, decay 0.95-1.07
        auto runMove = [&] (float from, float to, double at)
        {
            PsolaEngine::Params p;
            p.pitchSemi = 9.0f;  p.formantSemi = 4.0f;
            p.grainAvg = true;   p.pulseBody = 0.75f;  p.onsetHold = 3;
            p.onsetBackfill = true;  p.preLockLowCut = 0.75f;
            p.deterministicCadence = true;
            p.releaseRepair = true;  p.releaseShelf = from;
            PsolaEngine e;  e.prepare (fs);  e.setParams (p);
            std::vector<float> out (cs.x.size(), 0.0f);
            const int64_t sw = (int64_t) (fs * at);
            size_t i = 0;
            while (i < cs.x.size())
            {
                const int c = (int) std::min ((size_t) 256, cs.x.size() - i);
                if ((int64_t) i <= sw && (int64_t) (i + c) > sw && from != to)
                { p.releaseShelf = to;  e.setParams (p); }
                e.process (cs.x.data() + i, out.data() + i, c);
                i += (size_t) c;
            }
            return out;
        };
        auto maxD1 = [&] (const std::vector<float>& x, double at)
        {
            const int64_t a = std::max<int64_t> (1, (int64_t) (fs * at) - (int64_t) (fs * 0.010));
            const int64_t b = std::min<int64_t> ((int64_t) x.size(),
                                                 (int64_t) (fs * at) + (int64_t) (fs * 0.010));
            double m = 0.0;
            for (int64_t i = a; i < b; ++i)
                m = std::max (m, (double) std::fabs (x[(size_t) i] - x[(size_t) (i - 1)]));
            return m;
        };
        // Find where the shelf is actually doing something first: a ratio of
        // 1.00 taken at a moment when relGain is 0 proves nothing, because
        // the depth is multiplied by it. The active window is wherever
        // strength 0.5 and strength 0 part company.
        double bestAt = cs.relFrom, bestMag = 0.0;
        {
            const auto a = runMove (0.50f, 0.50f, 0.0);
            const auto b = runMove (0.00f, 0.00f, 0.0);
            const int hop = (int) (fs * 0.005);
            for (int64_t i = 0; i + hop < (int64_t) a.size(); i += hop)
            {
                double m = 0.0;
                for (int k = 0; k < hop; ++k)
                    m = std::max (m, (double) std::fabs (a[(size_t) (i + k)] - b[(size_t) (i + k)]));
                if (m > bestMag) { bestMag = m; bestAt = (double) i / fs; }
            }
            std::printf ("(the shelf is most active at %.3f s, |0.50 - 0.00| = %.5f)\n",
                         bestAt, bestMag);
        }

        // The first difference of the OUTPUT cannot see this: the shelf's
        // whole contribution over the decay is 0.003 while the signal moves
        // 0.006 between neighbouring samples, so a step in the depth hides
        // under the audio. Measure the depth change itself -- moved minus
        // held -- and ask how fast it arrives. An instant jump reaches its
        // full size inside a millisecond; a 40 ms glide does not.
        struct Move { const char* name; float from, to; };
        const Move moves[] = { { "0.50 -> 0.00", 0.50f, 0.00f },
                               { "0.50 -> 0.40", 0.50f, 0.40f },
                               { "0.40 -> 0.50", 0.40f, 0.50f },
                               { "0.00 -> 0.50", 0.00f, 0.50f } };
        const double at = bestAt;
        double worst = -200.0;
        auto span = [&] (const std::vector<float>& u, const std::vector<float>& v,
                         double from, double to)
        {
            const int64_t a = std::max<int64_t> (0, (int64_t) (fs * from));
            const int64_t b = std::min<int64_t> ((int64_t) u.size(), (int64_t) (fs * to));
            double m = 0.0;
            for (int64_t i = a; i < b; ++i)
                m = std::max (m, (double) std::fabs (u[(size_t) i] - v[(size_t) i]));
            return m;
        };
        for (const auto& mv : moves)
        {
            const auto moved = runMove (mv.from, mv.to, at);
            const auto held  = runMove (mv.from, mv.from, at);
            const double d1ms   = span (moved, held, at, at + 0.001);
            const double d100ms = span (moved, held, at, at + 0.100);
            // against the signal that is playing there, because that is what
            // decides whether a discontinuity of this size can be heard
            double acc = 0.0;  int64_t nn = 0;
            for (int64_t i = (int64_t) (fs * at);
                 i < std::min<int64_t> ((int64_t) held.size(), (int64_t) (fs * (at + 0.010))); ++i)
            { acc += (double) held[(size_t) i] * held[(size_t) i]; ++nn; }
            const double rms = nn ? std::sqrt (acc / (double) nn) : 0.0;
            const double db  = 20.0 * std::log10 ((d1ms + 1e-12) / (rms + 1e-12));
            worst = std::max (worst, db);
            std::printf ("%-22s %10.3f %11.6f %11.6f %9.1f dB %9.1f dBFS\n",
                         mv.name, at, d1ms, d100ms, db,
                         20.0 * std::log10 (d1ms + 1e-12));
        }
        // The bar is masking: an artifact 20 dB under the audio playing at
        // that instant is buried, one 5 dB under is not. Both numbers are
        // reported because the absolute size matters too -- these steps sit
        // around -77 dBFS, in the tail of a decay.
        //
        // For scale: the same four moves through an engine WITHOUT the glide
        // (releaseStage reading relShelfAmt directly, as before v0.62.2) give
        // -5.0 / -21.0 / -21.3 / -6.9 dB. The glide is worth about 17 dB on
        // the moves that span the whole range.
        std::printf ("strength move: %s (worst step %.1f dB under the local signal; "
                     "without the glide the same move gives -5.0 dB)\n",
                     worst < -20.0 ? "PASS" : "FAIL", worst);
        if (! (worst < -20.0)) ++failures;
    }

    std::printf ("\n%s\n", failures ? "SOME CHECKS FAILED" : "queue healthy in every case");
    return failures ? 1 : 0;
}
