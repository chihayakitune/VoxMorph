// onset_ab.cpp - the listening material for the Analysis Cadence decision.
//
// The measurement says the cadence costs about +0.2 dB at the median onset
// and buys buffer independence outright. Whether that trade is acceptable is
// not a question numbers can answer, so this builds the four configurations
// from the 2026-08-23 note and lays out the passages that decide it.
//
//   A0  cadence OFF, Onset Repair ON, Release OFF   the shipping product
//   A1  cadence ON,  Onset Repair ON, Release OFF   the cadence on its own
//   A2  cadence ON,  Onset Repair ON, Release ON at Safe Strength (0.35)
//   A3  cadence ON,  Onset Repair ON, Release ON at 0.40
//
// Two kinds of output:
//   full     the whole take, per configuration, at host buffers 32/64/256/512
//   reel     a curated sequence at buffer 256 -- the onsets the cadence hurt
//            most, then ordinary / weak / breath-initial / low-vowel onsets,
//            then representative and worst-residue endings. Same passages in
//            the same order in every reel, so A0 and A1 can be compared
//            passage by passage rather than by hunting through 110 seconds.
//
// Nothing is normalised: every file goes through at the same gain, because a
// per-file normalise would hide exactly the low-end differences under test.
//
// A0 and A1 are also written to a blind/ subfolder under opaque names, with
// the key in a separate file, so the comparison can be made without knowing
// which is which.
//
// Build:  g++ -O2 -std=c++17 -I dsp -o /tmp/oab test/onset_ab.cpp
// Run:    /tmp/oab <voice.wav> <outdir>
// Recordings stay OUT of the repository (project policy) - pass a path.
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

// ---------------------------------------------------------------- wav i/o
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
    const unsigned nd = (unsigned) (x.size() * 2);
    const unsigned sr = (unsigned) fs;
    const unsigned br = sr * 2;
    unsigned char h[44] = { 'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',
                            16,0,0,0, 1,0, 1,0, 0,0,0,0, 0,0,0,0, 2,0, 16,0,
                            'd','a','t','a',0,0,0,0 };
    const unsigned rl = 36 + nd;
    std::memcpy (h + 4, &rl, 4);  std::memcpy (h + 24, &sr, 4);
    std::memcpy (h + 28, &br, 4); std::memcpy (h + 40, &nd, 4);
    std::fwrite (h, 1, 44, f);
    for (float v : x)
    {
        const int s = (int) std::lround (std::clamp (v, -1.0f, 1.0f) * 32767.0f);
        const short q = (short) s;
        std::fwrite (&q, 2, 1, f);
    }
    std::fclose (f);
}

// ------------------------------------------------------------- phrase find
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

// ------------------------------------------------------------------ render
static constexpr int64_t kLookahead = 2048;

struct Config { const char* tag; bool cadence, release; float strength; const char* what; };

static std::vector<float> render (const std::vector<float>& in, double fs,
                                  const Config& cfg, int blk)
{
    PsolaEngine::Params p;
    p.pitchSemi = 9.0f;  p.formantSemi = 4.0f;
    p.grainAvg = true;   p.pulseBody = 0.75f;  p.onsetHold = 3;
    p.onsetBackfill = true;  p.preLockLowCut = 0.75f;     // Onset Repair, shipping defaults
    p.deterministicCadence = cfg.cadence;
    p.releaseRepair = cfg.release;  p.releaseShelf = cfg.strength;
    PsolaEngine e;  e.prepare (fs);  e.setParams (p);
    std::vector<float> out (in.size(), 0.0f);
    for (size_t i = 0; i < in.size(); i += (size_t) blk)
        e.process (in.data() + i, out.data() + i, (int) std::min ((size_t) blk, in.size() - i));
    return out;
}

// ------------------------------------------------------------ band measure
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

static double bandDb (const std::vector<float>& x, double fs, int64_t a, int n,
                      double lo, double hi)
{
    const double b[1][2] = { { lo, hi } };  double v[1];
    bandsDb (x, fs, a, n, b, 1, v);  return v[0];
}

// leak at one onset, against the running phrase (see cadence_probe for why
// the reference is broadband)
static double onsetLeak (const std::vector<float>& out, double fs, int64_t o, double f0out)
{
    const int w = (int) (fs * 0.060);
    const double hi = std::max (90.0, f0out * 0.80);
    const double head = bandDb (out, fs, o + kLookahead, w, 60.0, hi);
    double ref = -200.0;
    for (double at : { 0.15, 0.20, 0.25 })
        ref = std::max (ref, bandDb (out, fs, o + kLookahead + (int64_t) (fs * at), w, 60.0, 5000.0));
    if (head <= -190.0 || ref <= -60.0) return std::numeric_limits<double>::quiet_NaN();
    return head - ref;
}

// --------------------------------------------------------- onset character
// Enough to sort the passages into the four kinds the note asks for. These
// are descriptions of the INPUT, so every configuration gets the same list.
enum class Kind { normal, weak, breath, lowVowel };

static Kind classify (const std::vector<float>& in, double fs, int64_t o, double medPeak)
{
    const int w = (int) (fs * 0.060);
    if (o + w > (int64_t) in.size()) return Kind::normal;
    double pk = 0.0;
    for (int i = 0; i < (int) (fs * 0.150) && o + i < (int64_t) in.size(); ++i)
        pk = std::max (pk, (double) std::fabs (in[(size_t) (o + i)]));
    if (pk < 0.45 * medPeak) return Kind::weak;

    const double bands[2][2] = { { 60.0, 500.0 }, { 2000.0, 8000.0 } };
    double v[2];
    bandsDb (in, fs, o, w, bands, 2, v);
    if (v[1] - v[0] > -12.0) return Kind::breath;      // unusually bright start
    if (v[0] - v[1] > 26.0)  return Kind::lowVowel;    // almost nothing above 2k
    return Kind::normal;
}

static const char* kindName (Kind k)
{
    switch (k) { case Kind::weak: return "weak"; case Kind::breath: return "breath";
                 case Kind::lowVowel: return "lowvowel"; default: return "normal"; }
}

int main (int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf (stderr, "usage: %s <voice.wav> <outdir> [reels]\n", argv[0]);
        std::fprintf (stderr, "  reels = skip the 16 full renders (they are ~10 MB each);\n");
        std::fprintf (stderr, "          use it for the secondary takes, where the curated\n");
        std::fprintf (stderr, "          passages are the point and the whole file is not.\n");
        return 2;
    }
    const bool reelsOnly = argc > 3 && std::string (argv[3]) == "reels";
    std::vector<float> in;  double fs = 0.0;
    if (! loadWav (argv[1], in, fs))
    { std::fprintf (stderr, "cannot read %s\n", argv[1]); return 1; }
    const std::string dir = argv[2];

    const Config cfgs[4] = {
        { "A0", false, false, 0.00f, "cadence OFF, Onset Repair ON, Release OFF (shipping)" },
        { "A1", true,  false, 0.00f, "cadence ON,  Onset Repair ON, Release OFF" },
        { "A2", true,  true,  0.35f, "cadence ON,  Onset Repair ON, Release ON 0.35 (Safe)" },
        { "A3", true,  true,  0.40f, "cadence ON,  Onset Repair ON, Release ON 0.40" },
    };
    const int bufs[4] = { 32, 64, 256, 512 };

    std::printf ("%s  %.0f Hz  %.1f s\n", argv[1], fs, in.size() / fs);

    // ---- full renders, every configuration at every buffer ----
    std::vector<std::vector<float>> at256 (4);
    for (int c = 0; c < 4; ++c)
        for (int b = 0; b < 4; ++b)
        {
            if (reelsOnly && bufs[b] != 256) continue;
            const auto out = render (in, fs, cfgs[c], bufs[b]);
            if (! reelsOnly)
            {
                char name[256];
                std::snprintf (name, sizeof name, "%s/full_%s_buf%d.wav", dir.c_str(),
                               cfgs[c].tag, bufs[b]);
                writeWav (name, out, fs);
            }
            if (bufs[b] == 256) at256[(size_t) c] = out;
        }
    std::printf (reelsOnly ? "skipped the full renders (reels only)\n"
                           : "wrote 16 full renders (4 configurations x buffers 32/64/256/512)\n");

    // ---- pick the passages ----
    const auto phrases = findPhrases (in, fs);
    double speakerF0 = 150.0;
    {
        PsolaEngine::Params p;  p.pitchSemi = 9.0f;  p.deterministicCadence = true;
        PsolaEngine e;  e.prepare (fs);  e.setParams (p);
        std::vector<float> o (in.size(), 0.0f);
        for (size_t i = 0; i < in.size(); i += 256)
            e.process (in.data() + i, o.data() + i, (int) std::min ((size_t) 256, in.size() - i));
        std::vector<float> f0s;
        for (const auto& r : e.detectLog)
            if (r.voicedAfter && r.f0After > 40.0f) f0s.push_back (r.f0After);
        std::sort (f0s.begin(), f0s.end());
        if (! f0s.empty()) speakerF0 = f0s[f0s.size() / 2];
    }
    const double f0out = speakerF0 * std::pow (2.0, 9.0 / 12.0);

    double medPeak = 0.0;
    {
        std::vector<double> pk;
        for (const auto& ph : phrases)
        {
            double m = 0.0;
            for (int i = 0; i < (int) (fs * 0.150) && ph.on + i < (int64_t) in.size(); ++i)
                m = std::max (m, (double) std::fabs (in[(size_t) (ph.on + i)]));
            pk.push_back (m);
        }
        std::sort (pk.begin(), pk.end());
        if (! pk.empty()) medPeak = pk[pk.size() / 2];
    }

    // the onsets the cadence hurt most, by the paired difference A1 - A0
    struct Pick { int64_t at; double score; std::string label; };
    std::vector<Pick> picks;
    {
        struct Scored { int64_t at; double d; Kind k; };
        std::vector<Scored> all;
        for (const auto& ph : phrases)
        {
            const double a = onsetLeak (at256[0], fs, ph.on, f0out);
            const double b = onsetLeak (at256[1], fs, ph.on, f0out);
            if (std::isnan (a) || std::isnan (b)) continue;
            all.push_back ({ ph.on, b - a, classify (in, fs, ph.on, medPeak) });
        }
        std::sort (all.begin(), all.end(),
                   [] (const Scored& x, const Scored& y) { return x.d > y.d; });
        for (size_t i = 0; i < all.size() && i < 10; ++i)
        {
            char lb[64];
            std::snprintf (lb, sizeof lb, "worst%02zu_%+.1fdB_%s", i + 1, all[i].d,
                           kindName (all[i].k));
            picks.push_back ({ all[i].at, all[i].d, lb });
        }
        // three of each kind, taken from the middle of the degradation range
        // so they are ordinary examples rather than more outliers
        for (Kind want : { Kind::normal, Kind::weak, Kind::breath, Kind::lowVowel })
        {
            int taken = 0;
            for (size_t i = all.size() / 3; i < all.size() && taken < 3; ++i)
            {
                if (all[i].k != want) continue;
                bool dup = false;
                for (const auto& p2 : picks) if (p2.at == all[i].at) dup = true;
                if (dup) continue;
                char lb[64];
                std::snprintf (lb, sizeof lb, "%s%d", kindName (want), taken + 1);
                picks.push_back ({ all[i].at, all[i].d, lb });
                ++taken;
            }
        }
    }
    // endings: three ordinary, three with the largest low residue after the
    // voice has gone (which is what Release Repair is aimed at)
    {
        struct Scored { int64_t at; double r; };
        std::vector<Scored> ends;
        const int w = (int) (fs * 0.150);
        for (const auto& ph : phrases)
        {
            const double r = bandDb (at256[0], fs, ph.off + kLookahead, w, 60.0,
                                     std::max (90.0, f0out * 0.80));
            if (r > -190.0) ends.push_back ({ ph.off, r });
        }
        std::sort (ends.begin(), ends.end(),
                   [] (const Scored& x, const Scored& y) { return x.r > y.r; });
        for (size_t i = 0; i < ends.size() && i < 3; ++i)
        {
            char lb[64];  std::snprintf (lb, sizeof lb, "endresidue%zu", i + 1);
            picks.push_back ({ std::max<int64_t> (0, ends[i].at - (int64_t) (fs * 0.35)),
                               ends[i].r, lb });
        }
        for (size_t i = ends.size() / 2; i < ends.size() && i < ends.size() / 2 + 3; ++i)
        {
            char lb[64];  std::snprintf (lb, sizeof lb, "ending%zu", i - ends.size() / 2 + 1);
            picks.push_back ({ std::max<int64_t> (0, ends[i].at - (int64_t) (fs * 0.35)),
                               ends[i].r, lb });
        }
    }

    // ---- the reels ----
    // Same passages, same order, same lengths in every configuration.
    const int64_t pre  = (int64_t) (fs * 0.30);
    const int64_t post = (int64_t) (fs * 0.80);
    const int64_t gap  = (int64_t) (fs * 0.50);
    for (int c = 0; c < 4; ++c)
    {
        std::vector<float> reel;
        for (const auto& pk : picks)
        {
            const int64_t a = std::max<int64_t> (0, pk.at + kLookahead - pre);
            const int64_t b = std::min<int64_t> ((int64_t) at256[(size_t) c].size(),
                                                 pk.at + kLookahead + post);
            for (int64_t i = a; i < b; ++i) reel.push_back (at256[(size_t) c][(size_t) i]);
            reel.insert (reel.end(), (size_t) gap, 0.0f);
        }
        char name[256];
        std::snprintf (name, sizeof name, "%s/reel_%s.wav", dir.c_str(), cfgs[c].tag);
        writeWav (name, reel, fs);
    }
    std::printf ("wrote 4 reels of %zu passages each (buffer 256)\n", picks.size());

    // ---- blind pair for A0 / A1 ----
    // Named so the file gives nothing away, and the key kept apart.
    {
        // deterministic but not guessable from the name alone
        const unsigned h = (unsigned) (in.size() * 2654435761u);
        const bool swap = (h >> 16) & 1u;
        const int first  = swap ? 1 : 0;
        const int second = swap ? 0 : 1;
        for (int pass = 0; pass < 2; ++pass)
        {
            const int c = pass == 0 ? first : second;
            std::vector<float> reel;
            for (const auto& pk : picks)
            {
                const int64_t a = std::max<int64_t> (0, pk.at + kLookahead - pre);
                const int64_t b = std::min<int64_t> ((int64_t) at256[(size_t) c].size(),
                                                     pk.at + kLookahead + post);
                for (int64_t i = a; i < b; ++i) reel.push_back (at256[(size_t) c][(size_t) i]);
                reel.insert (reel.end(), (size_t) gap, 0.0f);
            }
            char name[256];
            std::snprintf (name, sizeof name, "%s/blind/reel_%c.wav", dir.c_str(),
                           pass == 0 ? 'X' : 'Y');
            writeWav (name, reel, fs);
        }
        char keyName[256];
        std::snprintf (keyName, sizeof keyName, "%s/blind/KEY_do_not_open_first.txt", dir.c_str());
        if (FILE* kf = std::fopen (keyName, "w"))
        {
            std::fprintf (kf, "reel_X.wav = %s\nreel_Y.wav = %s\n",
                          cfgs[first].tag, cfgs[second].tag);
            std::fprintf (kf, "\nX = %s\nY = %s\n", cfgs[first].what, cfgs[second].what);
            std::fclose (kf);
        }
        std::printf ("wrote blind/reel_X.wav and blind/reel_Y.wav (A0 and A1, order hidden)\n");
    }

    // ---- the passage list ----
    char idxName[256];
    std::snprintf (idxName, sizeof idxName, "%s/PASSAGES.txt", dir.c_str());
    if (FILE* xf = std::fopen (idxName, "w"))
    {
        std::fprintf (xf, "source: %s  (%.0f Hz, %.1f s)\n", argv[1], fs, in.size() / fs);
        std::fprintf (xf, "speaker f0 %.0f Hz -> %.0f Hz at +9 st\n\n", speakerF0, f0out);
        for (int c = 0; c < 4; ++c)
            std::fprintf (xf, "%s  %s\n", cfgs[c].tag, cfgs[c].what);
        std::fprintf (xf, "\nEvery file is at the same gain. Reels are buffer 256; the full\n");
        std::fprintf (xf, "renders also cover 32 / 64 / 512.\n\n");
        std::fprintf (xf, "reel order (%.2f s before, %.2f s after, %.2f s gap):\n",
                      (double) pre / fs, (double) post / fs, (double) gap / fs);
        double t = 0.0;
        for (size_t i = 0; i < picks.size(); ++i)
        {
            const double len = ((double) (pre + post) / fs);
            std::fprintf (xf, "%3zu  reel %6.2f s   source %7.2f s   %s\n",
                          i + 1, t, (double) picks[i].at / fs, picks[i].label.c_str());
            t += len + (double) gap / fs;
        }
        std::fclose (xf);
        std::printf ("wrote PASSAGES.txt\n");
    }
    return 0;
}
