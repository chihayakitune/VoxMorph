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

// "creaky" low vowel: irregular glottal pulses (vocal fry style) —
// alternating period +/-12% and alternating pulse amplitude, f0 ~= 55 Hz
static std::vector<float> makeCreaky (double f0, double seconds)
{
    const int n = (int) (FS * seconds);
    std::vector<double> src ((size_t) n, 0.0);
    double nextPulse = 100.0;
    bool flip = false;
    for (int i = 0; i < n; ++i)
    {
        if ((double) i >= nextPulse)
        {
            src[(size_t)i] = flip ? 0.6 : 1.0;
            const double per = FS / f0;
            nextPulse += per * (flip ? 0.88 : 1.12);
            flip = ! flip;
        }
    }
    Reso f1 (600.0, 110.0), f2 (1100.0, 130.0), f3 (2500.0, 170.0);
    std::vector<double> y ((size_t) n, 0.0);
    double maxA = 1e-12;
    for (int i = 0; i < n; ++i)
    {
        const double s = f1.tick (src[(size_t)i]) + 0.7 * f2.tick (src[(size_t)i])
                       + 0.3 * f3.tick (src[(size_t)i]);
        y[(size_t)i] = s;
        maxA = std::max (maxA, std::abs (s));
    }
    std::vector<float> v ((size_t) n, 0.0f);
    for (int i = 0; i < n; ++i) v[(size_t)i] = (float) (0.7 * y[(size_t)i] / maxA);
    return v;
}

// breathy vowel: vowel with realistic HF rolloff (-12 dB/oct above ~1.8 kHz,
// real voices carry little harmonic energy up there) plus continuous
// high-passed aspiration noise — the input type Air Preserve targets.
// Above ~3 kHz the aspiration dominates, as in a real breathy voice.
static std::vector<float> makeBreathy (double f0, double seconds)
{
    auto v = makeVowel (f0, f0, seconds);
    uint32_t rng = 24681357u;
    double lp1 = 0.0, lp2 = 0.0, vl1 = 0.0, vl2 = 0.0;
    const double k  = 1.0 - std::exp (-2.0 * M_PI * 2000.0 / FS);
    const double kv = 1.0 - std::exp (-2.0 * M_PI * 1800.0 / FS);
    for (auto& s : v)
    {
        vl1 += kv * ((double) s - vl1);          // 2x one-pole LP @1.8k
        vl2 += kv * (vl1 - vl2);
        rng = rng * 1664525u + 1013904223u;
        double nz = (double) ((int32_t) rng) / 2147483648.0;
        lp1 += k * (nz - lp1);  nz -= lp1;       // 2x one-pole HP @2k
        lp2 += k * (nz - lp2);  nz -= lp2;
        s = (float) (0.9 * vl2 + 0.08 * nz);
    }
    return v;
}

// run and also count voiced/unvoiced transitions (the "flutter" artifact)
static std::vector<float> runToggles (const std::vector<float>& in,
                                      const PsolaEngine::Params& p, int& toggles)
{
    PsolaEngine eng;
    eng.prepare (FS);
    eng.setParams (p);
    std::vector<float> out (in.size(), 0.0f);
    const int B = 256;
    bool last = false;
    toggles = 0;
    for (size_t i = 0; i < in.size(); i += B)
    {
        const int n = (int) std::min ((size_t) B, in.size() - i);
        eng.process (in.data() + i, out.data() + i, n);
        if (i > (size_t)(FS/4) && eng.isVoiced() != last) { ++toggles; last = eng.isVoiced(); }
    }
    return out;
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

    // Low Voice Mode: creaky 55 Hz vowel, pitch +7st
    {
        const auto creaky = makeCreaky (55.0, 3.0);
        int togOff = 0, togOn = 0;
        P p; p.pitchSemi = 7.0f;
        p.lowVoice = false; writeWav ("out_creaky_off.wav", runToggles (creaky, p, togOff));
        p.lowVoice = true;  writeWav ("out_creaky_on.wav",  runToggles (creaky, p, togOn));
        std::printf ("creaky 55Hz +7st: voiced/unvoiced toggles  off=%d  on=%d (lower is better)\n",
                     togOff, togOn);

        // recovery test: modal 120Hz -> creaky 55Hz -> modal 120Hz
        const auto modal = makeVowel (120.0, 120.0, 1.5);
        std::vector<float> seq;
        seq.insert (seq.end(), modal.begin(),  modal.end());
        seq.insert (seq.end(), creaky.begin(), creaky.begin() + (int) FS * 3 / 2);
        seq.insert (seq.end(), modal.begin(),  modal.end());
        int tog = 0;
        writeWav ("out_seq_on.wav", runToggles (seq, p, tog));

        // pitch floor: lift the converted creaky voice toward 160 Hz
        p.pitchFloorHz = 160.0f;
        writeWav ("out_creaky_floor.wav", runToggles (creaky, p, tog));
    }

    // Phase 2: per-formant control + spectral breath
    { P p; p.f2Shift = 4.0f;                       writeWav ("out_pf_f2s4.wav",  run (vowel, p)); }
    { P p; p.f1Shift = -3.0f;                      writeWav ("out_pf_f1sm3.wav", run (vowel, p)); }
    { P p; p.f1Gain = -9.0f;                       writeWav ("out_pf_f1gm9.wav", run (vowel, p)); }
    { P p; p.f3Gain = 9.0f;                        writeWav ("out_pf_f3g9.wav",  run (vowel, p)); }
    { P p; p.breath = 0.6f;                        writeWav ("out_pf_breath.wav",run (vowel, p)); }
    { P p; p.pitchSemi = 7.0f; p.f2Shift = 3.0f;   writeWav ("out_pf_mix.wav",   run (vowel, p)); }

    // v0.6: Air Preserve (mixed harmonic+noise) on a breathy vowel.
    // With air ON the aspiration must stay continuous (less periodic HF)
    // while f0/formants and total HF energy stay the same.
    {
        const auto breathy = makeBreathy (120.0, 2.0);
        writeWav ("out_air_dry.wav", breathy);
        P p; p.pitchSemi = 7.0f;
        p.airPreserve = 0.0f;  writeWav ("out_air_off.wav", run (breathy, p));
        p.airPreserve = 0.8f;  writeWav ("out_air_on.wav",  run (breathy, p));
        p.airPreserve = 1.0f;  p.airFreqHz = 700.0f;
                               writeWav ("out_air_max.wav", run (breathy, p));
        P q;                   writeWav ("out_air_id0.wav", run (breathy, q));
        q.airPreserve = 0.8f;  writeWav ("out_air_id.wav",  run (breathy, q));
    }

    std::puts ("done");
    return 0;
}
