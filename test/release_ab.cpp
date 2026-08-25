// release_ab.cpp - B0 / B1 for Release Pitch Continuation.
//
// Two configurations, one buffer, six endings. Deliberately the whole of it:
// the analysis is ChatGPT's job, so this produces the material and the event
// times and stops.
//
//   B0  the shipping default   Onset Repair ON, Cadence OFF, Release OFF
//   B1  B0 + Release Pitch Continuation ON, release shelf 0
//
// Conditions are fixed by the 2026-08-23 master note: 44.1 kHz, buffer 256,
// Pitch +9, Formant +2, Analysis Cadence OFF. Same gain, no normalising.
//
// Build:  g++ -O2 -std=c++17 -I dsp -o /tmp/rab test/release_ab.cpp
// Run:    /tmp/rab <voice.wav> <outdir>
// Recordings stay OUT of the repository (project policy) - pass a path.
#define PSOLA_DETECT_LOG 1
#include "PsolaEngine.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>

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
            std::memcpy (&fmt, &d[i + 8], 2);  std::memcpy (&c2, &d[i + 10], 2);
            std::memcpy (&sr,  &d[i + 12], 4); std::memcpy (&bps, &d[i + 22], 2);
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

static void writeWav (const std::string& path, const std::vector<float>& x, double fs)
{
    FILE* f = std::fopen (path.c_str(), "wb");
    if (! f) { std::fprintf (stderr, "cannot write %s\n", path.c_str()); return; }
    const unsigned nd = (unsigned) (x.size() * 2), sr = (unsigned) fs, br = sr * 2;
    unsigned char h[44] = { 'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',
                            16,0,0,0, 1,0, 1,0, 0,0,0,0, 0,0,0,0, 2,0, 16,0,
                            'd','a','t','a',0,0,0,0 };
    const unsigned rl = 36 + nd;
    std::memcpy (h + 4, &rl, 4);  std::memcpy (h + 24, &sr, 4);
    std::memcpy (h + 28, &br, 4); std::memcpy (h + 40, &nd, 4);
    std::fwrite (h, 1, 44, f);
    for (float v : x)
    { const short q = (short) std::lround (std::clamp (v, -1.0f, 1.0f) * 32767.0f);
      std::fwrite (&q, 2, 1, f); }
    std::fclose (f);
}

struct Phrase { int64_t on, off; };

static std::vector<Phrase> findPhrases (const std::vector<float>& in, double fs)
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
    std::vector<Phrase> out;
    bool voiced = false; int quiet = 0;
    for (size_t i = 0; i < e.size(); ++i)
    {
        if (! voiced && e[i] > on && quiet >= 8)
        { voiced = true; out.push_back ({ (int64_t) i * hop, -1 }); quiet = 0; }
        else if (voiced && e[i] < off)
        { voiced = false; quiet = 1; if (! out.empty()) out.back().off = (int64_t) i * hop; }
        else if (! voiced) ++quiet;
    }
    if (! out.empty() && out.back().off < 0) out.back().off = (int64_t) in.size();
    return out;
}

static constexpr int64_t kLookahead = 2048;

struct Result { std::vector<float> out; std::vector<PsolaEngine::StateLogRow> st;
                std::vector<PsolaEngine::ClampSpan> spans;
                std::vector<PsolaEngine::PermitSpan> permits;
                int late, dropped, overflow; };

static Result render (const std::vector<float>& in, double fs, bool continuation)
{
    PsolaEngine::Params p;
    p.pitchSemi   = 9.0f;      // fixed by the master note
    p.formantSemi = 2.0f;      // +2, NOT +4 -- the v0.62.3 generator used +4
    p.grainAvg = true;  p.pulseBody = 0.75f;  p.onsetHold = 3;
    p.onsetBackfill = true;  p.preLockLowCut = 0.75f;    // shipping Onset Repair
    p.deterministicCadence = false;                      // fixed OFF
    p.releaseRepair = false;  p.releaseShelf = 0.0f;     // shelf out of the way
    p.releasePitchContinuation = continuation;   // = the clamp from v0.63.4
    PsolaEngine e;  e.prepare (fs);  e.setParams (p);
    Result r;  r.out.assign (in.size(), 0.0f);
    for (size_t i = 0; i < in.size(); i += 256)
        e.process (in.data() + i, r.out.data() + i, (int) std::min ((size_t) 256, in.size() - i));
    r.st = e.stateLog;  r.spans = e.clampSpans;  r.permits = e.permitSpans;
    r.late = e.releaseLateEvents(); r.dropped = e.releaseDropped(); r.overflow = e.releaseOverflow();
    return r;
}

// Several bands from one transform.
static void bandsDb (const std::vector<float>& x, double fs, int64_t a, int n,
                     const double (*bands)[2], int nb, double* out)
{
    if (a < 0 || a + n > (int64_t) x.size())
    { for (int k = 0; k < nb; ++k) out[k] = -200.0; return; }
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

// only to RANK the endings, so the six passages are the ones worth looking at
static double lowResidue (const std::vector<float>& x, double fs, int64_t off, double f0out)
{
    const int64_t a = off + kLookahead;
    const int n = (int) (fs * 0.150);
    if (a < 0 || a + n > (int64_t) x.size()) return -200.0;
    const int N = 32768;
    std::vector<float> re ((size_t) N, 0.0f), im ((size_t) N, 0.0f);
    for (int i = 0; i < n && i < N; ++i)
        re[(size_t) i] = x[(size_t) (a + i)] * (0.5f - 0.5f * std::cos (2.0f * (float) M_PI * i / n));
    PsolaEngine::fftForViz (re.data(), im.data(), N);
    auto band = [&] (double lo, double hi)
    {
        double acc = 0.0;
        for (int k = (int) (lo * N / fs); k <= (int) (hi * N / fs) && k <= N / 2; ++k)
            acc += re[(size_t) k] * re[(size_t) k] + im[(size_t) k] * im[(size_t) k];
        return 10.0 * std::log10 (acc + 1e-20);
    };
    // how far the speaker's own band sits ABOVE the converted voice's body:
    // positive means the low end is on top, which is the complaint
    return band (60.0, 140.0) - band (150.0, 600.0);
}

int main (int argc, char** argv)
{
    if (argc < 3)
    { std::fprintf (stderr, "usage: %s <voice.wav> <outdir>\n", argv[0]); return 2; }
    std::vector<float> in;  double fs = 0.0;
    if (! loadWav (argv[1], in, fs))
    { std::fprintf (stderr, "cannot read %s\n", argv[1]); return 1; }
    const std::string dir = argv[2];

    const auto b0 = render (in, fs, false);
    const auto b1 = render (in, fs, true);

    // sanity: nothing broken
    double peak0 = 0.0, peak1 = 0.0;  int bad = 0;
    for (size_t i = 0; i < in.size(); ++i)
    {
        if (! std::isfinite (b0.out[i]) || ! std::isfinite (b1.out[i])) ++bad;
        peak0 = std::max (peak0, (double) std::fabs (b0.out[i]));
        peak1 = std::max (peak1, (double) std::fabs (b1.out[i]));
    }
    std::printf ("non-finite %d, peak B0 %.3f, peak B1 %.3f\n", bad, peak0, peak1);
    // Both, because a counter that is non-zero in the BASELINE is a standing
    // property of the shipping path, not something this change introduced.
    std::printf ("queue B0: late %d dropped %d overflow %d\n",
                 b0.late, b0.dropped, b0.overflow);
    std::printf ("queue B1: late %d dropped %d overflow %d\n",
                 b1.late, b1.dropped, b1.overflow);

    std::printf ("symptom gate opened %zu times, %zu permission grants\n",
                 b1.spans.size(), b1.permits.size());
    {
        // the two structural conditions, checked here rather than left to the
        // reader: at most one grant per re-arm, none longer than the cap
        int    worstIdx = 0;  double worstLen = 0.0;
        for (const auto& q : b1.permits)
        {
            worstIdx = std::max (worstIdx, q.indexSinceRearm);
            worstLen = std::max (worstLen, q.to - q.from);
        }
        std::printf ("permission: max %d grant(s) per VOICE recovery, longest %.1f ms  %s\n",
                     worstIdx, worstLen * 1000.0,
                     (worstIdx <= 1 && worstLen <= 0.2505) ? "PASS" : "FAIL");
    }

    const auto phrases = findPhrases (in, fs);
    double speakerF0 = 150.0;
    {
        std::vector<float> f0s;
        for (const auto& r : b0.st) if (r.relF0 > 40.0f) f0s.push_back (r.relF0);
        std::sort (f0s.begin(), f0s.end());
        if (! f0s.empty()) speakerF0 = f0s[f0s.size() / 2];
    }
    const double f0out = speakerF0 * std::pow (2.0, 9.0 / 12.0);

    // three worst endings by low-band dominance in B0, and three ordinary ones
    struct Scored { int64_t off; double r; };
    std::vector<Scored> ends;
    for (const auto& ph : phrases)
    {
        const double r = lowResidue (b0.out, fs, ph.off, f0out);
        if (r > -190.0) ends.push_back ({ ph.off, r });
    }
    std::sort (ends.begin(), ends.end(),
               [] (const Scored& a, const Scored& b) { return a.r > b.r; });
    std::vector<Scored> pick;
    for (size_t i = 0; i < ends.size() && i < 3; ++i) pick.push_back (ends[i]);
    for (size_t i = ends.size() / 2; i < ends.size() && pick.size() < 6; ++i)
        pick.push_back (ends[i]);

    // reels: the same absolute window from input, B0 and B1, so the three
    // files line up sample for sample. The output lags the input by the
    // engine's lookahead, exactly as it does in the full take.
    const int64_t pre = (int64_t) (fs * 0.35), post = (int64_t) (fs * 0.80);
    const int64_t gap = (int64_t) (fs * 0.50);
    std::vector<float> rIn, r0, r1;
    std::vector<std::pair<double,double>> reelTimes;   // reel start, event in reel
    for (const auto& pk : pick)
    {
        const int64_t a = std::max<int64_t> (0, pk.off - pre);
        const int64_t b = std::min<int64_t> ((int64_t) in.size(), pk.off + post);
        reelTimes.push_back ({ (double) rIn.size() / fs, (double) (pk.off - a) / fs });
        for (int64_t i = a; i < b; ++i)
        { rIn.push_back (in[(size_t) i]);  r0.push_back (b0.out[(size_t) i]);
          r1.push_back (b1.out[(size_t) i]); }
        rIn.insert (rIn.end(), (size_t) gap, 0.0f);
        r0.insert  (r0.end(),  (size_t) gap, 0.0f);
        r1.insert  (r1.end(),  (size_t) gap, 0.0f);
    }
    writeWav (dir + "/input/input_reel.wav", rIn, fs);
    writeWav (dir + "/output/B0_reel.wav",   r0,  fs);
    writeWav (dir + "/output/B1_reel.wav",   r1,  fs);
    std::printf ("wrote 3 reels of %zu endings\n", pick.size());

    if (FILE* cf = std::fopen ((dir + "/logs/events.csv").c_str(), "w"))
    {
        std::fprintf (cf, "index,kind,source_end_s,reel_start_s,reel_end_s,"
                          "output_reel_end_s,b0_low_minus_body_db\n");
        for (size_t i = 0; i < pick.size(); ++i)
            std::fprintf (cf, "%zu,%s,%.4f,%.4f,%.4f,%.4f,%.2f\n",
                          i + 1, i < 3 ? "worst" : "ordinary",
                          (double) pick[i].off / fs,
                          reelTimes[i].first,
                          reelTimes[i].first + reelTimes[i].second,
                          reelTimes[i].first + reelTimes[i].second + (double) kLookahead / fs,
                          pick[i].r);
        std::fclose (cf);
    }
    // the state machine around those endings only -- not the whole take
    if (FILE* lf = std::fopen ((dir + "/logs/state_B1.csv").c_str(), "w"))
    {
        std::fprintf (lf, "t,state,voicedRead,relActive,energyPerSample,zcr,relF0,"
                          "clampFrom,clampTo\n");
        for (const auto& r : b1.st)
        {
            bool near = false;
            for (const auto& pk : pick)
                if (std::fabs (r.t - (double) pk.off / fs) < 0.5) near = true;
            if (! near) continue;
            std::fprintf (lf, "%.4f,%d,%d,%d,%.6g,%.4f,%.1f,%.4f,%.4f\n",
                          r.t, r.state, (int) r.voicedRead, (int) r.relActive,
                          r.energyPerSample, r.zcr, r.relF0, r.clampFrom, r.clampTo);
        }
        std::fclose (lf);
    }
    // The measurements the review asked to be logged: three bands and RMS,
    // over 0-30 ms and 0-80 ms from the ending.
    //
    // The ending on the OUTPUT axis is source_end + D. Subtracting source_end
    // alone -- which the v0.63.3 report did -- puts every window 46.4 ms
    // early and mislabels which part of the tail was touched.
    if (FILE* bf2 = std::fopen ((dir + "/logs/bands.csv").c_str(), "w"))
    {
        std::fprintf (bf2, "index,kind,source_end_s,output_end_s,window_ms,"
                           "b0_60_140,b1_60_140,d_60_140,"
                           "b0_150_600,b1_150_600,d_150_600,"
                           "b0_600_5k,b1_600_5k,d_600_5k,"
                           "b0_lowMinusBody,b1_lowMinusBody,d_lowMinusBody,"
                           "b0_rms_dbfs,b1_rms_dbfs,d_rms,"
                           "gate_from_s,gate_to_s\n");
        for (size_t i = 0; i < pick.size(); ++i)
        {
            const int64_t e0 = pick[i].off + kLookahead;
            // the clamp window that belongs to this ending, from the log
            // the gate openings that touch this ending's 0-80 ms window
            double cf = 0.0, ct = 0.0;
            {
                const double w0 = (double) e0 / fs, w1 = w0 + 0.080;
                for (const auto& sp : b1.spans)
                    if (sp.to > w0 - 0.050 && sp.from < w1)
                    { if (cf == 0.0) cf = sp.from;  ct = sp.to; }
            }
            for (double win : { 0.030, 0.080 })
            {
                const int n = (int) (fs * win);
                const double bands[3][2] = { { 60.0, 140.0 }, { 150.0, 600.0 }, { 600.0, 5000.0 } };
                double a[3], b[3];
                bandsDb (b0.out, fs, e0, n, bands, 3, a);
                bandsDb (b1.out, fs, e0, n, bands, 3, b);
                auto rms = [&] (const std::vector<float>& x)
                {
                    double acc = 0.0;  int cnt = 0;
                    for (int k = 0; k < n && e0 + k < (int64_t) x.size(); ++k)
                    { acc += (double) x[(size_t) (e0 + k)] * x[(size_t) (e0 + k)]; ++cnt; }
                    return 20.0 * std::log10 (std::sqrt (acc / std::max (1, cnt)) + 1e-12);
                };
                const double r0 = rms (b0.out), r1 = rms (b1.out);
                std::fprintf (bf2, "%zu,%s,%.4f,%.4f,%.0f,"
                                   "%.3f,%.3f,%+.3f,%.3f,%.3f,%+.3f,%.3f,%.3f,%+.3f,"
                                   "%+.3f,%+.3f,%+.3f,%.3f,%.3f,%+.3f,%.4f,%.4f\n",
                              i + 1, i < 3 ? "worst" : "ordinary",
                              (double) pick[i].off / fs, (double) e0 / fs, win * 1000.0,
                              a[0], b[0], b[0] - a[0], a[1], b[1], b[1] - a[1],
                              a[2], b[2], b[2] - a[2],
                              a[0] - a[1], b[0] - b[1], (b[0] - b[1]) - (a[0] - a[1]),
                              r0, r1, r1 - r0, cf, ct);
            }
        }
        std::fclose (bf2);
    }
    // Where the symptom gate actually held open, on the output axis, and
    // which ending each opening belongs to.
    if (FILE* gf = std::fopen ((dir + "/logs/gate.csv").c_str(), "w"))
    {
        std::fprintf (gf, "from_s,to_s,length_ms,nearest_event,rel_to_output_end_ms\n");
        for (const auto& sp : b1.spans)
        {
            int best = -1;  double bestD = 1e9;
            for (size_t i = 0; i < pick.size(); ++i)
            {
                const double oe = (double) (pick[i].off + kLookahead) / fs;
                if (std::fabs (sp.from - oe) < bestD) { bestD = std::fabs (sp.from - oe); best = (int) i + 1; }
            }
            const double oe = best > 0 ? (double) (pick[(size_t) best - 1].off + kLookahead) / fs : 0.0;
            std::fprintf (gf, "%.4f,%.4f,%.1f,%d,%+.1f\n",
                          sp.from, sp.to, (sp.to - sp.from) * 1000.0, best,
                          (sp.from - oe) * 1000.0);
        }
        std::fclose (gf);
    }
    if (FILE* pf = std::fopen ((dir + "/logs/permits.csv").c_str(), "w"))
    {
        std::fprintf (pf, "from_s,to_s,length_ms,index_since_rearm,"
                          "nearest_event,rel_to_output_end_ms\n");
        for (const auto& q : b1.permits)
        {
            int best = -1;  double bestD = 1e9;
            for (size_t i = 0; i < pick.size(); ++i)
            {
                const double oe = (double) (pick[i].off + kLookahead) / fs;
                if (std::fabs (q.from - oe) < bestD) { bestD = std::fabs (q.from - oe); best = (int) i + 1; }
            }
            const double oe = best > 0 ? (double) (pick[(size_t) best - 1].off + kLookahead) / fs : 0.0;
            std::fprintf (pf, "%.4f,%.4f,%.1f,%d,%d,%+.1f\n",
                          q.from, q.to, (q.to - q.from) * 1000.0, q.indexSinceRearm,
                          best, (q.from - oe) * 1000.0);
        }
        std::fclose (pf);
    }
    // One steady stretch with the clamp ENABLED but the gate never open.
    // With the recombination interpolating inside the LR pair, that stretch
    // should differ from baseline only by the pair's allpass phase, so every
    // band and the RMS have to land within 0.1 dB.
    if (FILE* sf = std::fopen ((dir + "/logs/steady.csv").c_str(), "w"))
    {
        std::fprintf (sf, "from_s,length_ms,metric,b0,b1,abs_diff\n");
        // the middle of the longest phrase that no gate opening touches
        int64_t bestAt = -1;  int64_t bestLen = 0;
        const int64_t need = (int64_t) (fs * 0.150);
        for (const auto& ph : phrases)
        {
            const int64_t a = ph.on + kLookahead + (int64_t) (fs * 0.10);
            const int64_t b = ph.off + kLookahead - (int64_t) (fs * 0.05);
            if (b - a < need) continue;
            // Not merely "no opening inside the window": the envelope has a
            // 30 ms release and decays geometrically, so a window that starts
            // just after an opening still has env well above zero. Require
            // half a second of clearance ahead of it -- more than sixteen
            // time constants -- so env really has returned to 0.
            bool clear = true;
            for (const auto& sp : b1.spans)
                if (sp.to > (double) a / fs - 0.50 && sp.from < (double) b / fs)
                { clear = false; break; }
            if (clear && b - a > bestLen) { bestLen = b - a; bestAt = a; }
        }
        if (bestAt < 0)
            std::fprintf (sf, "none,0,,,,\n");
        else
        {
            const int n = (int) need;
            const double bands[3][2] = { { 60.0, 140.0 }, { 150.0, 600.0 }, { 600.0, 5000.0 } };
            double a3[3], b3[3];
            bandsDb (b0.out, fs, bestAt, n, bands, 3, a3);
            bandsDb (b1.out, fs, bestAt, n, bands, 3, b3);
            const char* nm[3] = { "60-140", "150-600", "600-5k" };
            double worst = 0.0;
            for (int k = 0; k < 3; ++k)
            {
                std::fprintf (sf, "%.4f,%.0f,%s,%.4f,%.4f,%.4f\n",
                              (double) bestAt / fs, 150.0, nm[k], a3[k], b3[k],
                              std::fabs (b3[k] - a3[k]));
                worst = std::max (worst, std::fabs (b3[k] - a3[k]));
            }
            auto rms = [&] (const std::vector<float>& x)
            {
                double acc = 0.0;
                for (int k = 0; k < n; ++k)
                    acc += (double) x[(size_t) (bestAt + k)] * x[(size_t) (bestAt + k)];
                return 20.0 * std::log10 (std::sqrt (acc / n) + 1e-12);
            };
            const double r0 = rms (b0.out), r1 = rms (b1.out);
            std::fprintf (sf, "%.4f,%.0f,RMS,%.4f,%.4f,%.4f\n",
                          (double) bestAt / fs, 150.0, r0, r1, std::fabs (r1 - r0));
            worst = std::max (worst, std::fabs (r1 - r0));
            // The sharper test now that env = 0 is meant to be exact: how far
            // apart are the SAMPLES, in the int16 the reels are written at.
            double maxAbs = 0.0;  int maxLsb = 0;
            for (int k = 0; k < n; ++k)
            {
                const double d = std::fabs ((double) b1.out[(size_t) (bestAt + k)]
                                          - (double) b0.out[(size_t) (bestAt + k)]);
                maxAbs = std::max (maxAbs, d);
                maxLsb = std::max (maxLsb, (int) std::llround (d * 32767.0));
            }
            std::fprintf (sf, "%.4f,%.0f,maxSampleDiff,%.9g,%d,\n",
                          (double) bestAt / fs, 150.0, maxAbs, maxLsb);
            std::printf ("gate-inactive steady stretch at %.3f s: worst band/RMS difference "
                         "%.4f dB (bar 0.01), max sample difference %.3g = %d LSB (bar 1)  %s\n",
                         (double) bestAt / fs, worst, maxAbs, maxLsb,
                         (worst <= 0.01 && maxLsb <= 1) ? "PASS" : "FAIL");
        }
        std::fclose (sf);
    }
    std::printf ("wrote logs/events.csv, state_B1.csv, bands.csv, gate.csv, permits.csv "
                 "and steady.csv\n");
    std::printf ("speaker f0 %.0f Hz -> %.0f Hz, lookahead %lld samples (%.1f ms)\n",
                 speakerF0, f0out, (long long) kLookahead, 1000.0 * kLookahead / fs);
    return bad ? 1 : 0;
}
