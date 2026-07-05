// Offline verification harness for PsolaEngine.
// Generates a synthetic vowel (glottal pulse train through 3 formant
// resonators), processes it with several settings, writes WAV files.
#include "../dsp/PsolaEngine.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <cmath>

static const double FS = 48000.0;

// simple resonator biquad
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

static std::vector<float> makeVowel (double f0, double seconds)
{
    const int n = (int) (FS * seconds);
    std::vector<float> v ((size_t) n, 0.0f);

    // Rosenberg-ish glottal pulse train (decaying impulses are fine for tests)
    double phase = 0.0;
    std::vector<double> src ((size_t) n, 0.0);
    for (int i = 0; i < n; ++i)
    {
        phase += f0 / FS;
        if (phase >= 1.0) { phase -= 1.0; src[(size_t)i] = 1.0; }
    }
    Reso f1 (700.0, 110.0), f2 (1220.0, 120.0), f3 (2600.0, 160.0);
    double maxA = 1e-12;
    std::vector<double> y ((size_t) n, 0.0);
    for (int i = 0; i < n; ++i)
    {
        const double s = f1.tick (src[(size_t)i]) + 0.7 * f2.tick (src[(size_t)i])
                       + 0.35 * f3.tick (src[(size_t)i]);
        y[(size_t)i] = s;
        maxA = std::max (maxA, std::abs (s));
    }
    for (int i = 0; i < n; ++i)
        v[(size_t)i] = (float) (0.7 * y[(size_t)i] / maxA);
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

static std::vector<float> run (const std::vector<float>& in,
                               float pitch, float formant,
                               bool robot = false, float robotHz = 120.0f)
{
    PsolaEngine eng;
    eng.prepare (FS);
    eng.setParams (pitch, formant, robot, robotHz, 1.0f);
    std::vector<float> out (in.size(), 0.0f);
    const int B = 256;
    for (size_t i = 0; i < in.size(); i += B)
    {
        const int n = (int) std::min ((size_t) B, in.size() - i);
        eng.process (in.data() + i, out.data() + i, n);
    }
    std::printf ("pitch=%+5.1f formant=%+5.1f robot=%d  -> detected f0 (last) = %.1f Hz\n",
                 pitch, formant, (int) robot, eng.currentF0());
    return out;
}

int main()
{
    const auto vowel = makeVowel (120.0, 2.0);
    writeWav ("out_dry.wav", vowel);
    writeWav ("out_p0_f0.wav",  run (vowel,  0.0f,  0.0f));
    writeWav ("out_p7_f0.wav",  run (vowel, +7.0f,  0.0f));
    writeWav ("out_p0_f7.wav",  run (vowel,  0.0f, +7.0f));
    writeWav ("out_p0_fm5.wav", run (vowel,  0.0f, -5.0f));
    writeWav ("out_p12_fm4.wav",run (vowel,+12.0f, -4.0f));
    writeWav ("out_robot.wav",  run (vowel,  0.0f,  0.0f, true, 100.0f));
    std::puts ("done");
    return 0;
}
