// realvoice_probe.cpp - formant tracking on a REAL recording (v0.47.1).
//
// Build:  g++ -O2 -std=c++17 -I dsp -o /tmp/rv test/realvoice_probe.cpp
// Run:    /tmp/rv <a-sustained-vowel.wav>
//
// The invariance test: the vocal tract does not move when the plugin shifts
// pitch, so the engine's F1-F3 reading must not move either. Any drift down
// the shift column is error, and this needs NO labelled ground truth -- which
// matters, because at a high f0 even Praat cannot label the file (measured on
// a 316 Hz /a/: Praat's F3 ranged 1739-3833 Hz depending purely on its own
// ceiling / n-formants settings).
//
// Also prints how often two formants collapsed onto one peak, which is the
// defect this was written to chase.
//
// Recordings themselves stay OUT of the repository (project policy) -- pass a
// path. See HANDOVER.md v0.47.1 for what Kitune_A.wav produced.
#include "PsolaEngine.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>
static bool loadWav (const char* path, std::vector<float>& out, double& fs)
{
    FILE* f = std::fopen (path, "rb");
    if (! f) return false;
    std::vector<unsigned char> d;
    unsigned char buf[65536]; size_t n;
    while ((n = std::fread (buf, 1, sizeof buf, f)) > 0) d.insert (d.end(), buf, buf + n);
    std::fclose (f);
    if (d.size() < 44 || std::memcmp (d.data(), "RIFF", 4)) return false;
    size_t i = 12; int ch = 1, bits = 16; fs = 44100;
    while (i + 8 <= d.size())
    {
        const char* id = (const char*) &d[i];
        unsigned sz; std::memcpy (&sz, &d[i+4], 4);
        if (! std::memcmp (id, "fmt ", 4))
        {
            unsigned short c; std::memcpy (&c, &d[i+10], 2); ch = c;
            unsigned r; std::memcpy (&r, &d[i+12], 4); fs = r;
            unsigned short b; std::memcpy (&b, &d[i+22], 2); bits = b;
        }
        else if (! std::memcmp (id, "data", 4))
        {
            const size_t ns = sz / (size_t)(bits / 8);
            out.resize (ns / (size_t) ch);
            for (size_t k = 0; k < out.size(); ++k)
            {
                short s; std::memcpy (&s, &d[i + 8 + k * (size_t)(bits/8) * (size_t) ch], 2);
                out[k] = (float) s / 32768.0f;
            }
            return true;
        }
        i += 8 + sz + (sz & 1);
    }
    return false;
}
static float med (std::vector<float> v)
{ if (v.empty()) return 0; std::sort (v.begin(), v.end()); return v[v.size()/2]; }
int main (int argc, char** argv)
{
    std::vector<float> sig; double fs = 0;
    if (argc < 2 || ! loadWav (argv[1], sig, fs)) { std::printf ("load failed\n"); return 1; }
    std::printf ("%s: %.0f Hz, %.2f s\n\n", argv[1], fs, sig.size() / fs);
    std::printf ("%6s | %-12s %-12s %-12s | merged | notes\n",
                 "shift", "F1 (conf)", "F2 (conf)", "F3 (conf)");
    for (double st : { 0.0, 3.0, 5.0, 7.0, 9.0, 12.0 })
    {
        PsolaEngine e; e.prepare (fs);
        PsolaEngine::Params p;
        p.pitchSemi = (float) st;
        p.vowelAdapt = true; p.vowelAdaptAmt = 1.0f;
        e.setParams (p);
        std::vector<float> m[3], c[3], buf (512);
        int nMerged = 0, nFrames = 0;
        for (size_t q = 0; q + 512 <= sig.size(); q += 512)
        {
            std::copy (sig.begin() + (long) q, sig.begin() + (long) q + 512, buf.begin());
            e.process (buf.data(), buf.data(), 512);
            if ((double) q / fs < 0.7 || ! e.formantsValid()) continue;
            ++nFrames;
            if (e.formantMerged (2) || e.formantMerged (1)) ++nMerged;
            for (int k = 0; k < 3; ++k)
            {
                if (e.analysisFormantIn (k) > 20.0f) m[k].push_back (e.analysisFormantIn (k));
                c[k].push_back (e.formantConfidence (k));
            }
        }
        std::printf ("%5.0fst | %6.0f(%.2f) %6.0f(%.2f) %6.0f(%.2f) | %4.0f %% | %d frames\n",
                     st, med (m[0]), med (c[0]), med (m[1]), med (c[1]),
                     med (m[2]), med (c[2]),
                     nFrames ? 100.0 * nMerged / nFrames : 0.0, nFrames);
    }
    return 0;
}
