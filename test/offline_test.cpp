// Offline verification harness for PsolaEngine (v0.2 features included).
#include "../dsp/PsolaEngine.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <cmath>

static const double FS = 48000.0;

struct Reso
{
    double b0=1, a1=0, a2=0, z1=0, z2=0;
    Reso (double f, double bw)
    {
        const double r = std::exp (-M_PI * bw / FS);
        a1 = -2.0 * r * std::cos (2.0 * M_PI * f / FS);
        a2 = r * r;
        b0 = (1.0 - r) * std::sqrt (1.0 + r*r - 2.0*r*std::cos(4.0*M_PI*f/FS));
    }
    double tick (double x)
    {
        const double y = b0*x - a1*z1 - a2*z2;
        z2 = z1; z1 = y;
        return y;
    }
};

// vowel with piecewise-constant f0 (two halves) for intonation tests
static std::vector<float> makeVowel (double f0a, double f0b, double seconds)
{
    const int n = (int) (FS * seconds);
    std::vector<double> src ((size_t) n, 0.0);
    double phase = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double f0 = (i < n/2) ? f0a : f0b;
        phase += f0 / FS;
        if (phase >= 1.0) { phase -= 1.0; src[(size_t)i] = 1.0; }
    }
    Reso f1 (700.0, 110.0), f2 (1220.0, 120.0), f3 (2600.0, 160.0);
    std::vector<double> y ((size_t) n, 0.0);
    double maxA = 1e-12;
    for (int i = 0; i < n; ++i)
    {
        const double s = f1.tick (src[(size_t)i]) + 0.7 * f2.tick (src[(size_t)i])
                       + 0.35 * f3.tick (src[(size_t)i]);
        y[(size_t)i] = s;
        maxA = std::max (maxA, std::abs (s));
    }
    std::vector<float> v ((size_t) n, 0.0f);
    for (int i = 0; i < n; ++i)
        v[(size_t)i] = (float) (0.7 * y[(size_t)i] / maxA);
    return v;
}

// unvoiced "consonant": white noise through a 3 kHz resonator
static std::vector<float> makeNoiseCons (double seconds)
{
    const int n = (int) (FS * seconds);
    Reso rz (3000.0, 900.0);
    std::vector<float> v ((size_t) n, 0.0f);
    uint32_t rng = 987654321u;
    double maxA = 1e-12;
    std::vector<double> y ((size_t) n, 0.0);
    for (int i = 0; i < n; ++i)
    {
        rng = rng * 1664525u + 1013904223u;
        const double nz = (double) ((int32_t) rng) / 2147483648.0;
        y[(size_t)i] = rz.tick (nz);
        maxA = std::max (maxA, std::abs (y[(size_t)i]));
    }
    for (int i = 0; i < n; ++i) v[(size_t)i] = (float) (0.5 * y[(size_t)i] / maxA);
    return v;
}

static void writeWav (const std::string& path, const std::vector<float>& x)
{
    FILE* f = std::fopen (path.c_str(), "wb");
    if (!f) { std::perror (path.c_str()); return; }
    const uint32_t sr = (uint32_t) FS, n = (uint32_t) x.size();
    const uint32_t dataBytes = n * 2, riff = 36 + dataBytes;
    const uint16_t fmt = 1, ch = 1, bits = 16, block = 2;
    const uint32_t byteRate = sr * block;
    std::fwrite ("RIFF", 1, 4, f); std::fwrite (&riff, 4, 1, f);
    std::fwrite ("WAVEfmt ", 1, 8, f);
    uint32_t sz = 16; std::fwrite (&sz, 4, 1, f);
    std::fwrite (&fmt, 2, 1, f);  std::fwrite (&ch, 2, 1, f);
    std::fwrite (&sr, 4, 1, f);   std::fwrite (&byteRate, 4, 1, f);
    std::fwrite (&block, 2, 1, f); std::fwrite (&bits, 2, 1, f);
    std::fwrite ("data", 1, 4, f); std::fwrite (&dataBytes, 4, 1, f);
    for (float s : x)
    {
        const int16_t q = (int16_t) std::lround (std::clamp (s, -1.0f, 1.0f) * 32767.0f);
        std::fwrite (&q, 2, 1, f);
    }
    std::fclose (f);
}

static std::vector<float> run (const std::vector<float>& in, const PsolaEngine::Params& p)
{
    PsolaEngine eng;
    eng.prepare (FS);
    eng.setParams (p);
    std::vector<float> out (in.size(), 0.0f);
    const int B = 256;
    for (size_t i = 0; i < in.size(); i += B)
    {
        const int n = (int) std::min ((size_t) B, in.size() - i);
        eng.process (in.data() + i, out.data() + i, n);
    }
    return out;
}

int main()
{
    using P = PsolaEngine::Params;
    const auto vowel  = makeVowel (120.0, 120.0, 2.0);
    const auto sweep  = makeVowel (110.0, 132.0, 4.0);   // "intonation": low then high
    const auto conson = makeNoiseCons (2.0);

    writeWav ("out_dry.wav", vowel);

    { P p;                                         writeWav ("out_p0_f0.wav",  run (vowel,  p)); }
    { P p; p.pitchSemi = 7;                        writeWav ("out_p7_f0.wav",  run (vowel,  p)); }
    { P p; p.formantSemi = 7;                      writeWav ("out_p0_f7.wav",  run (vowel,  p)); }
    { P p; p.pitchSemi = 12; p.formantSemi = -4;   writeWav ("out_p12_fm4.wav",run (vowel,  p)); }
    { P p; p.robotize = true; p.robotHz = 100;     writeWav ("out_robot.wav",  run (vowel,  p)); }

    // v0.2 features
    { P p; p.pitchRange = 2.0f; p.pitchCenterHz = 120.0f;
                                                   writeWav ("out_range2.wav", run (sweep,  p)); }
    { P p;                                         writeWav ("out_range1.wav", run (sweep,  p)); }
    { P p; p.breath = 0.9f;                        writeWav ("out_breath.wav", run (vowel,  p)); }
    { P p; p.consonantSemi = 7;                    writeWav ("out_cons7.wav",  run (conson, p)); }
    { P p;                                         writeWav ("out_cons0.wav",  run (conson, p)); }
    { P p; p.tiltDb = 6.0f;                        writeWav ("out_tilt6.wav",  run (vowel,  p)); }

    std::puts ("done");
    return 0;
}
