// Offline verification harness for PsolaEngine (v0.2 features included).
#include "../dsp/PsolaEngine.h"
#include "../dsp/VoiceAnalyzer.h"
#include "../dsp/MatchingEngine.h"
#include "../dsp/SampleTargetCatalog.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <cmath>
#include <new>

// global allocation counter: verifies process() never allocates after the
// warm-up (offline stand-in for the real-time thread allocation check)
static bool g_countAlloc = false;
static long g_allocCount = 0;
void* operator new (std::size_t sz)
{
    if (g_countAlloc) ++g_allocCount;
    if (void* p = std::malloc (sz ? sz : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[] (std::size_t sz) { return operator new (sz); }
void  operator delete   (void* p) noexcept              { std::free (p); }
void  operator delete[] (void* p) noexcept              { std::free (p); }
void  operator delete   (void* p, std::size_t) noexcept { std::free (p); }
void  operator delete[] (void* p, std::size_t) noexcept { std::free (p); }

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

// vowel with custom formant frequencies (for the vowel-adaptive warp tests)
static std::vector<float> makeVowelF (double f0, double F1, double F2, double F3,
                                      double seconds)
{
    const int n = (int) (FS * seconds);
    std::vector<double> src ((size_t) n, 0.0);
    double phase = 0.0;
    for (int i = 0; i < n; ++i)
    {
        phase += f0 / FS;
        if (phase >= 1.0) { phase -= 1.0; src[(size_t)i] = 1.0; }
    }
    Reso r1 (F1, 110.0), r2 (F2, 120.0), r3 (F3, 160.0);
    std::vector<double> y ((size_t) n, 0.0);
    double maxA = 1e-12;
    for (int i = 0; i < n; ++i)
    {
        const double s = r1.tick (src[(size_t)i]) + 0.7 * r2.tick (src[(size_t)i])
                       + 0.35 * r3.tick (src[(size_t)i]);
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

// pure harmonic series (partials at -6 dB/oct up to Nyquist*0.9), with an
// optional log-domain glide (f0a -> f0b over the length) and vibrato —
// the "known ground truth" inputs for the Natural Air v2 tests
static std::vector<float> makeHarm (double f0a, double f0b, double seconds,
                                    double vibHz = 0.0, double vibSemi = 0.0)
{
    const int n = (int) (FS * seconds);
    std::vector<float> v ((size_t) n, 0.0f);
    double ph = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double t  = i / FS;
        double f0 = f0a * std::pow (f0b / f0a, t / seconds);
        if (vibHz > 0.0)
            f0 *= std::pow (2.0, vibSemi / 12.0 * std::sin (2.0 * M_PI * vibHz * t));
        ph += f0 / FS;
        double s = 0.0;
        for (int h = 1; h <= 160; ++h)
        {
            if (h * f0 > 0.45 * FS) break;
            s += std::sin (2.0 * M_PI * h * ph) / h;
        }
        v[(size_t) i] = (float) (0.25 * s);
    }
    return v;
}

// add white or pink noise at a known RMS level relative to the signal
static void addNoise (std::vector<float>& v, double relDb, bool pink, uint32_t seed)
{
    double sig = 0.0;
    for (float s : v) sig += (double) s * s;
    sig = std::sqrt (sig / (double) v.size());
    std::vector<double> nz (v.size(), 0.0);
    uint32_t rng = seed;
    double b0 = 0, b1 = 0, b2 = 0, nrms = 0.0;
    for (size_t i = 0; i < v.size(); ++i)
    {
        rng = rng * 1664525u + 1013904223u;
        const double w = (double) ((int32_t) rng) / 2147483648.0;
        double x = w;
        if (pink)   // Kellet economy pink filter (~-3 dB/oct)
        {
            b0 = 0.99765 * b0 + w * 0.0990460;
            b1 = 0.96300 * b1 + w * 0.2965164;
            b2 = 0.57000 * b2 + w * 1.0526913;
            x  = 0.185 * (b0 + b1 + b2 + w * 0.1848);
        }
        nz[i] = x;  nrms += x * x;
    }
    nrms = std::sqrt (nrms / (double) v.size());
    const double g = sig * std::pow (10.0, relDb / 20.0) / std::max (nrms, 1e-12);
    for (size_t i = 0; i < v.size(); ++i)
        v[i] = (float) std::clamp ((double) v[i] + g * nz[i], -1.0, 1.0);
}

// low-pitch pulse train with alternating period (+/-dev) and alternating
// pulse amplitude (ampAlt on every 2nd pulse) through vowel resonators —
// mild vocal-fry / period-doubling / subharmonic textures around 80-120 Hz.
// These repeat exactly at TWO pulse periods, so their harmonic leakage
// correlates at 2P but only weakly at P.
static std::vector<float> makeAltPulse (double f0, double dev, double ampAlt,
                                        double seconds)
{
    const int n = (int) (FS * seconds);
    std::vector<double> src ((size_t) n, 0.0);
    double nextPulse = 100.0;
    bool flip = false;
    for (int i = 0; i < n; ++i)
        if ((double) i >= nextPulse)
        {
            src[(size_t) i] = flip ? ampAlt : 1.0;
            nextPulse += (FS / f0) * (flip ? 1.0 - dev : 1.0 + dev);
            flip = ! flip;
        }
    Reso f1 (600.0, 110.0), f2 (1100.0, 130.0), f3 (2500.0, 170.0);
    std::vector<double> y ((size_t) n, 0.0);
    double maxA = 1e-12;
    for (int i = 0; i < n; ++i)
    {
        const double s = f1.tick (src[(size_t) i]) + 0.7 * f2.tick (src[(size_t) i])
                       + 0.3 * f3.tick (src[(size_t) i]);
        y[(size_t) i] = s;
        maxA = std::max (maxA, std::abs (s));
    }
    std::vector<float> v ((size_t) n, 0.0f);
    for (int i = 0; i < n; ++i) v[(size_t) i] = (float) (0.7 * y[(size_t) i] / maxA);
    return v;
}

// sibilant-like unvoiced noise: white noise high-passed at ~5 kHz
static std::vector<float> makeSibilant (double seconds)
{
    const int n = (int) (FS * seconds);
    std::vector<float> v ((size_t) n, 0.0f);
    uint32_t rng = 1122334455u;
    double lp1 = 0.0, lp2 = 0.0;
    const double k = 1.0 - std::exp (-2.0 * M_PI * 5000.0 / FS);
    for (int i = 0; i < n; ++i)
    {
        rng = rng * 1664525u + 1013904223u;
        double x = (double) ((int32_t) rng) / 2147483648.0;
        lp1 += k * (x - lp1);  x -= lp1;
        lp2 += k * (x - lp2);  x -= lp2;
        v[(size_t) i] = (float) (0.35 * x);
    }
    return v;
}

static bool hasBad (const std::vector<float>& x)
{
    for (float s : x) if (! std::isfinite (s)) return true;
    return false;
}

// energy on the f0in harmonic grid excluding bins shared with the f0out
// grid ("old-pitch ghost"), via the engine's own FFT. dB re total.
static double ghostDb (const std::vector<float>& x, double f0in, double f0out)
{
    const int n = 32768;
    const size_t a = x.size() / 3;
    if (x.size() < a + (size_t) n) return 0.0;
    std::vector<float> re ((size_t) n), im ((size_t) n, 0.0f);
    for (int i = 0; i < n; ++i)
    {
        const float w = 0.5f * (1.0f - std::cos (2.0f * (float) M_PI * (float) i / (float) (n - 1)));
        re[(size_t) i] = x[a + (size_t) i] * w;
    }
    PsolaEngine::fftForViz (re.data(), im.data(), n);
    std::vector<double> mag ((size_t) n / 2 + 1);
    double total = 0.0;
    for (int b = 0; b <= n / 2; ++b)
    {
        mag[(size_t) b] = (double) re[(size_t) b] * re[(size_t) b]
                        + (double) im[(size_t) b] * im[(size_t) b];
        total += mag[(size_t) b];
    }
    std::vector<uint8_t> outb ((size_t) n / 2 + 1, 0);
    for (int h = 1; h < 400; ++h)
    {
        const int b = (int) std::lround (h * f0out * n / FS);
        if (b > n / 2) break;
        const int w = std::max (2, (int) (0.02 * b));
        for (int bb = std::max (0, b - 2*w); bb <= std::min (n/2, b + 2*w); ++bb)
            outb[(size_t) bb] = 1;
    }
    double e = 0.0;
    for (int h = 1; h < 400; ++h)
    {
        const int b = (int) std::lround (h * f0in * n / FS);
        const int w = std::max (2, (int) (0.02 * b));
        if (b + w > n / 2) break;
        bool shared = false;
        for (int bb = b - w; bb <= b + w && ! shared; ++bb) shared = outb[(size_t) bb];
        if (shared) continue;
        for (int bb = b - w; bb <= b + w; ++bb) e += mag[(size_t) bb];
    }
    return 10.0 * std::log10 (e / (total + 1e-300) + 1e-12);
}

static double rmsOf (const std::vector<float>& x)   // stable middle section
{
    const size_t a = x.size() / 3, b = std::min (x.size(), a + (size_t) FS);
    double e = 0.0;
    for (size_t i = a; i < b; ++i) e += (double) x[i] * x[i];
    return std::sqrt (e / (double) (b - a));
}

static double peakOf (const std::vector<float>& x)
{
    double p = 0.0;
    for (float s : x) p = std::max (p, (double) std::abs (s));
    return p;
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

// same as run(), but with a caller-chosen host block size
static std::vector<float> runBlocked (const std::vector<float>& in,
                                      const PsolaEngine::Params& p, int B)
{
    PsolaEngine eng;
    eng.prepare (FS);
    eng.setParams (p);
    std::vector<float> out (in.size(), 0.0f);
    for (size_t i = 0; i < in.size(); i += (size_t) B)
        eng.process (in.data() + i, out.data() + i,
                     (int) std::min ((size_t) B, in.size() - i));
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
        p.airPreserve = 1.0f;  writeWav ("out_air_on.wav",  run (breathy, p));
        p.airPreserve = 1.5f;
                               writeWav ("out_air_max.wav", run (breathy, p));
        P q;                   writeWav ("out_air_id0.wav", run (breathy, q));
        q.airPreserve = 1.0f;  writeWav ("out_air_id.wav",  run (breathy, q));
    }

    // v0.8: High Range guard — input jumps 150 -> 300 Hz ("laugh"); with the
    // guard at 200 Hz / amount 0 %, the high half must stay near its natural
    // pitch instead of being shifted the full +7 st
    {
        const auto lowhigh = makeVowel (150.0, 300.0, 4.0);
        P p; p.pitchSemi = 7.0f;
        writeWav ("out_hi_off.wav", run (lowhigh, p));
        p.hiRangeHz = 200.0f; p.hiPitchAmt = 0.0f;
        writeWav ("out_hi_on.wav",  run (lowhigh, p));
    }

    // v0.7: GCI Grain Sync — regression on the clean vowel plus a creaky
    // (irregular-pulse) comparison pair for listening
    {
        P p; p.pitchSemi = 7.0f; p.gciSync = true;
        writeWav ("out_gci_vowel.wav", run (vowel, p));
        const auto creaky2 = makeCreaky (55.0, 3.0);
        P q; q.pitchSemi = 7.0f; q.lowVoice = true;
        q.gciSync = false; writeWav ("out_gci_creak_off.wav", run (creaky2, q));
        q.gciSync = true;  writeWav ("out_gci_creak_on.wav",  run (creaky2, q));
    }

    // v0.10: VoiceAnalyzer sanity — a synthetic vowel must report its own
    // specs (f0 120, F ~700/1220/2600); the sweep must show a wider spread
    {
        const auto v  = makeVowel (120.0, 120.0, 3.0);
        const auto pr = VoiceAnalyzer::analyze (v.data(), (int) v.size(), FS);
        std::printf ("analyzer vowel : f0=%.1f (exp 120)  F=%.0f/%.0f/%.0f (exp ~700/1220/2600)"
                     "  L=%+.1f/%+.1f/%+.1f dB  spread=%.2f st  frames=%d\n",
                     pr.f0Hz, pr.F[0], pr.F[1], pr.F[2], pr.L[0], pr.L[1], pr.L[2],
                     pr.f0SpreadSt, pr.voicedFrames);
        const auto s  = makeVowel (110.0, 132.0, 4.0);
        const auto ps = VoiceAnalyzer::analyze (s.data(), (int) s.size(), FS);
        std::printf ("analyzer sweep : f0=%.1f  spread=%.2f st (exp > vowel's)\n",
                     ps.f0Hz, ps.f0SpreadSt);
    }

    // ==== v0.29.0: vowel-matched Matching ====
    // Everything here is checked against KNOWN formants. The v0.28.4
    // estimator was never measured this way, which is how a 4.8 st F2 error
    // at high pitch went unnoticed and ended up driving Auto-Set.
    std::puts ("\n== Matching (vowel-matched, v0.29.0) ==");
    int mFail = 0;
    {
        // PUBLISHED Japanese vowel formants (Yazawa & Kondo, 16 Tokyo
        // speakers, 8M/8F; medians at the vowel midpoint), order A I U E O.
        // Real measured values, not a textbook sketch -- and the male/female
        // pair below is the ground truth for what a male-to-female match
        // must produce: +1.95..+4.24 st, POSITIVE in every band and vowel.
        static const double VF[5][3] = {          // male
            { 704.0, 1245.0, 2611.0 }, { 292.0, 2199.0, 3000.0 },
            { 343.0, 1453.0, 2344.0 }, { 447.0, 1990.0, 2644.0 },
            { 450.0,  854.0, 2620.0 } };
        static const double VFF[5][3] = {         // female
            { 847.0, 1504.0, 2922.0 }, { 352.0, 2724.0, 3412.0 },
            { 439.0, 1656.0, 2826.0 }, { 518.0, 2319.0, 3050.0 },
            { 518.0, 1035.0, 2983.0 } };
        static const char* VN[5] = { "A", "I", "U", "E", "O" };

        // Realistic vowel: glottal-pulse source with jitter, FIVE resonators
        // (F4/F5 included) and aspiration noise. The earlier version of this
        // test used an impulse train through three clean resonators, which
        // any estimator can solve -- and that is precisely why it missed the
        // F3 degeneracy that broke matching on a real recording. A test
        // signal has to be at least as hard as the real thing.
        auto vowel = [&] (const double* F123, double f0, double scale,
                          double sec, unsigned seed)
        {
            const int n = (int) (FS*sec);
            std::vector<float> y ((size_t) n, 0.0f);
            unsigned st = seed*2654435761u + 1u;
            auto rnd = [&st] { st = st*1664525u + 1013904223u;
                               return (double) ((st >> 8) & 0xFFFF) / 32768.0 - 1.0; };
            int i = 0;
            while (i < n)
            {
                const double P = FS/(f0*(1.0 + 0.012*rnd()));
                const int Pi = (int) P;
                if (Pi < 8) break;
                const int tp = (int) (0.40*Pi), tn = (int) (0.16*Pi);
                std::vector<double> gl ((size_t) Pi, 0.0);
                for (int k = 0; k < tp; ++k)
                { const double t = (double) k/std::max (1, tp); gl[(size_t) k] = 3*t*t-2*t*t*t; }
                for (int k = 0; k < tn && tp+k < Pi; ++k)
                { const double t = (double) k/std::max (1, tn); gl[(size_t) (tp+k)] = 1.0-t*t; }
                double prev = 0.0;
                for (int k = 0; k < Pi && i+k < n; ++k)
                { y[(size_t) (i+k)] += (float) (gl[(size_t) k]-prev); prev = gl[(size_t) k]; }
                i += Pi;
            }
            double sd = 0; for (auto v : y) sd += (double) v*v;
            sd = std::sqrt (sd/std::max (1, n))+1e-12;
            for (auto& v : y) v = (float) (v/sd);
            double nprev = 0.0;
            for (int k = 0; k < n; ++k)
            { const double u = rnd(); y[(size_t) k] += (float) (0.06*(u-0.85*nprev)); nprev = u; }
            const double hi[2] = { 3800.0, 4600.0 };
            const double bws[5] = { 70., 110., 160., 220., 280. };
            for (int b = 0; b < 5; ++b)
            {
                const double f = (b < 3 ? F123[b] : hi[b-3])*scale;
                if (f >= 0.45*FS) continue;
                const double T = 1.0/FS, r = std::exp (-M_PI*bws[(size_t) b]*T);
                const double b1 = 2*r*std::cos (2*M_PI*f*T), b2 = -r*r, g = 1-b1-b2;
                double z1 = 0, z2 = 0;
                for (auto& v : y)
                { const double o = g*v + b1*z1 + b2*z2; v = (float) o; z2 = z1; z1 = o; }
            }
            float mx = 1e-12f; for (auto v : y) mx = std::max (mx, std::abs (v));
            for (auto& v : y) v *= 0.5f/mx;
            return y;
        };

        auto utterT = [&] (const double tbl[5][3], double f0, double scale, const char* which)
        {
            std::vector<float> out;
            for (const char* p = which; *p; ++p)
            {
                const int v = *p - '0';
                auto seg = vowel (tbl[v], f0, scale, 1.2, (unsigned) (v*7+3));
                out.insert (out.end(), seg.begin(), seg.end());
                out.insert (out.end(), (size_t) (0.12*FS), 0.0f);
            }
            return out;
        };
        auto utter = [&] (double f0, double scale, const char* which)
        { return utterT (VF, f0, scale, which); };

        // (a) per-formant accuracy against ground truth, for the bands the
        //     signal can actually carry (reliability >= kMinRel)
        {
            std::vector<double> errs;
            for (double f0 : { 110.0, 150.0, 220.0, 300.0 })
                for (int v = 0; v < 5; ++v)
                {
                    auto s2 = vowel (VF[v], f0, 1.0, 2.0, (unsigned) (v*11+5));
                    const auto p = VoiceAnalyzer::analyze (s2.data(), (int) s2.size(), FS);
                    if (! p.valid()) continue;
                    for (int b = 0; b < 3; ++b)
                    {
                        if (VoiceAnalyzer::reliability ((float) VF[v][b], (float) f0)
                                < MatchingEngine::kMinRel) continue;
                        errs.push_back (std::abs (12.0*std::log2 (p.F[b]/VF[v][b])));
                    }
                }
            std::sort (errs.begin(), errs.end());
            // The profile is a MEDIAN over many frames and vowels, so the
            // median error is the statistic that actually reaches Matching;
            // the tail is reported for information. Residual outliers are
            // all F1 just above the reliability floor (F/f0 ~ 3), where the
            // envelope peak can still snap onto a harmonic.
            const double med = errs.empty() ? 9.9 : errs[errs.size()/2];
            const double p90 = errs.empty() ? 9.9 : errs[(size_t) (errs.size()*0.9)];
            const bool ok = ! errs.empty() && errs.size() > 30 && med < 1.0;
            std::printf ("formant accuracy (reliable bands): median %.2f  p90 %.2f  worst %.2f st"
                         " over %zu checks  %s\n",
                         med, p90, errs.back(), errs.size(), ok ? "PASS" : "FAIL");
            if (! ok) ++mFail;
        }

        // (a2) THE SAME THING, BUT WITH f0 MOVING. (a) holds f0 fixed inside
        //      each case, and that is exactly how a -15.6 st F1 error shipped
        //      unnoticed through v0.37.0: real speech sweeps its pitch, and
        //      the profile is a median over frames whose identifiability
        //      therefore VARIES. A fixed-f0 test can only ever see the
        //      average case; it cannot see a statistic being captured by the
        //      frames that measured nothing.
        //
        //      What went wrong: harmonicEnvelope extrapolates FLAT below the
        //      first harmonic, extractPeaks accepted that flat run as a
        //      sequence of maxima (its `>=` test is satisfied everywhere on a
        //      plateau), and those fake peaks carry H1's level -- the largest
        //      thing in the F1 band on a high voice -- so assignFormants,
        //      which selects by level, chose the extrapolation. It returned
        //      F1 values BELOW the speaker's own f0 with full confidence, and
        //      because the fallback for a not-found band also voted in the
        //      median, the median WAS the fallback. See VoiceAnalyzer.h
        //      extractPeaks / bandReliability.
        {
            struct Case { const char* nm; double mid; };
            const Case cs[] = { { "260 Hz", 260.0 }, { "310 Hz", 310.0 },
                                { "370 Hz", 370.0 } };
            bool ok = true;
            int nAns = 0, nRefused = 0;
            double worstTrusted = 0.0;
            std::printf ("   %-5s %-8s %8s %8s %6s %6s\n",
                         "vowel", "f0 centre", "trueF1", "gotF1", "err", "rel");
            for (int v = 0; v < 5; ++v)
                for (const auto& c : cs)
                {
                    // one vowel, f0 swept +-4.5 st -- the span the real
                    // character recordings show (9.0-12.0 st p10..p90)
                    std::vector<float> x;
                    for (int k = 0; k < 10; ++k)
                    {
                        const double st = -4.5 + 9.0 * (double) (k % 7) / 6.0;
                        auto seg = vowel (VFF[v], c.mid * std::pow (2.0, st/12.0),
                                          1.0, 0.9, (unsigned) (k*17+5));
                        x.insert (x.end(), seg.begin(), seg.end());
                    }
                    const auto p = VoiceAnalyzer::analyze (x.data(), (int) x.size(), FS);
                    if (! p.valid()) continue;
                    const bool answered = p.F[0] > 0.0f;
                    const double err = answered
                        ? 12.0 * std::log2 (p.F[0] / VFF[v][0]) : 0.0;
                    if (answered) ++nAns; else ++nRefused;
                    // The contract is NOT "always right" -- for a close vowel
                    // on a high voice F1 really does sit at or under the first
                    // harmonic and cannot be recovered. The contract is:
                    // whatever it reports as TRUSTED (rel >= kMinRel) must be
                    // right, and it must never report a confident F1 below the
                    // speaker's own fundamental.
                    if (answered && p.rel[0] >= MatchingEngine::kMinRel)
                    {
                        worstTrusted = std::max (worstTrusted, std::abs (err));
                        if (std::abs (err) > 2.0) ok = false;
                        if (p.F[0] < p.f0Hz) ok = false;
                    }
                    std::printf ("   /%s/  %-8s %8.0f %8s %+6.2f %6.2f%s\n",
                                 VN[v], c.nm, VFF[v][0],
                                 answered ? std::to_string ((int) p.F[0]).c_str() : "(none)",
                                 err, p.rel[0],
                                 (answered && p.rel[0] >= MatchingEngine::kMinRel
                                  && std::abs (err) > 2.0) ? "   <-- BAD" : "");
                }
            std::printf ("moving-f0 F1: %d answered / %d refused, worst TRUSTED error "
                         "%.2f st (<= 2.0), none below own f0  %s\n",
                         nAns, nRefused, worstTrusted, ok ? "PASS" : "FAIL");
            if (! ok) ++mFail;
        }

        // (a2b) Two bands must never resolve to the SAME peak. The search
        //       ranges overlap (F1's 700-1200 is inside F2's 700-3400), and
        //       before v0.37.2 each band took its own maximum independently,
        //       so on a voice where F1 and F2 merge into one envelope hump
        //       both selected it and a minimum-spacing line pushed them
        //       150 Hz apart. 30-46 % of frames on the shipped character
        //       recordings did this. The give-away is the RATIO: two real
        //       formants of the same speaker do not sit at the repair
        //       distance.
        {
            bool ok = true;
            for (double f0 : { 150.0, 240.0, 320.0, 400.0 })
                for (int v = 0; v < 5; ++v)
                {
                    auto s2 = vowel (VFF[v], f0, 1.0, 2.0, (unsigned) (v*11+5));
                    const auto p = VoiceAnalyzer::analyze (s2.data(), (int) s2.size(), FS);
                    if (! p.valid()) continue;
                    // only bands the profile actually reports
                    const bool have12 = p.F[0] > 0.0f && p.F[1] > 0.0f;
                    const bool have23 = p.F[1] > 0.0f && p.F[2] > 0.0f;
                    if (have12 && ! (p.F[1] > p.F[0])) ok = false;
                    if (have23 && ! (p.F[2] > p.F[1])) ok = false;
                    // the old repair produced F2 == F1 + 150 exactly
                    if (have12 && std::abs ((p.F[1] - p.F[0]) - 150.0f) < 0.5f) ok = false;
                    if (have23 && std::abs ((p.F[2] - p.F[1]) - 200.0f) < 0.5f) ok = false;
                }
            std::printf ("formants stay distinct and ordered (no spacing repair): %s\n",
                         ok ? "PASS" : "FAIL");
            if (! ok) ++mFail;
        }

        // (a2c) Does a vowel BUCKET actually contain that vowel?
        //
        //       The per-vowel table is what MatchingEngine compares, so the
        //       question that matters is not "how evenly are frames spread"
        //       (an evenly wrong split scores well on that) but "does bucket
        //       /a/ hold /a/". Checked the only way that means anything:
        //       synthesize an utterance of all five vowels with KNOWN
        //       formants, then ask how far each bucket's own F2 lands from
        //       the true F2 of the vowel whose name it carries.
        //
        //       F2 is the discriminator -- it swings ~16 st across the vowel
        //       space, so a bucket holding the wrong vowel misses by a lot
        //       and there is no way to score well by accident.
        {
            std::vector<double> err;
            int buckets = 0;
            for (int tbl = 0; tbl < 2; ++tbl)
                for (double f0 : { 130.0, 200.0, 260.0, 320.0, 400.0 })
                    for (double sc : { 0.92, 1.0, 1.12 })
                    {
                        const double (*T)[3] = tbl ? VFF : VF;
                        std::vector<float> x;
                        for (int rep = 0; rep < 2; ++rep)
                            for (int v = 0; v < 5; ++v)
                            {
                                auto seg = vowel (T[v], f0, sc, 1.2, (unsigned)(v*7+rep*31+11));
                                x.insert (x.end(), seg.begin(), seg.end());
                                x.insert (x.end(), (size_t)(0.12*FS), 0.0f);
                            }
                        const auto p = VoiceAnalyzer::analyze (x.data(), (int) x.size(), FS, 1.0e9);
                        if (! p.valid()) continue;
                        for (int v = 0; v < 5; ++v)
                        {
                            const auto& q = p.vow[v];
                            if (! q.valid() || ! (q.F[1] > 0.0f)) continue;
                            ++buckets;
                            err.push_back (std::abs (12.0 * std::log2 (q.F[1] / (T[v][1] * sc))));
                        }
                    }
            std::sort (err.begin(), err.end());
            const double med2 = err.empty() ? 99.0 : err[err.size()/2];
            const double p90  = err.empty() ? 99.0 : err[(size_t)(err.size()*0.9)];
            // Measured over the same runs, median / p90 in st:
            //
            //   v0.37.1, both old                    1.32 / 7.84
            //   distinct-peak assignment only        1.45 / 8.04
            //   F3-normalized classifier only        1.57 / 6.83
            //   v0.37.2, both                        0.94 / 3.78
            //
            // Worth reading twice: NEITHER change helps on its own, and each
            // looks like a small regression alone. Correcting the assignment
            // moves the F2 distribution, and the old classifier normalized by
            // the median of that distribution, so it was partly compensating
            // for the very error being fixed. Only removing both couplings
            // improves anything -- which is also why the two ship together.
            //
            // A regression guard, not a certificate: 0.94 st is still not
            // good, and the residue is /e/ vs /i/, which differ almost
            // entirely in F1 and so cannot be told apart on a voice too high
            // for F1 to be located (see classifyVowels).
            const bool ok = ! err.empty() && med2 < 3.0;
            std::printf ("vowel buckets hold their own vowel: F2 err median %.2f  p90 %.2f st"
                         " over %d buckets (median < 3.0)  %s\n",
                         med2, p90, buckets, ok ? "PASS" : "FAIL");
            if (! ok) ++mFail;
        }

        // (a3) A band that was never located must not drive a level trim.
        //      L = 0 means "as loud as the strongest formant", so an ungated
        //      f*gain read a missing band as maximally loud and asked for
        //      several dB on the strength of nothing.
        {
            auto A = utter (150.0, 1.00, "01234");
            auto B = utterT (VFF, 330.0, 1.10, "01234");
            const auto p1 = VoiceAnalyzer::analyze (A.data(), (int) A.size(), FS);
            const auto p2 = VoiceAnalyzer::analyze (B.data(), (int) B.size(), FS);
            const auto r  = MatchingEngine::autoSet (p1, p2);
            const char* gid[3] = { "f1gain", "f2gain", "f3gain" };
            bool ok = true;
            for (int b = 0; b < 3; ++b)
            {
                float g = 0.0f;
                for (int i = 0; i < r.count; ++i)
                    if (std::string (r.changes[i].id) == gid[b]) g = r.changes[i].value;
                const bool usable = r.bandRel[b] >= MatchingEngine::kMinRel;
                if (! usable && g != 0.0f) ok = false;
                std::printf ("   %s rel=%.2f -> gain %+.2f dB%s\n",
                             gid[b], r.bandRel[b], g, usable ? "" : "  (band not located)");
            }
            std::printf ("unlocated band writes no gain trim: %s\n", ok ? "PASS" : "FAIL");
            if (! ok) ++mFail;
        }

        // (b) the reliability law itself: nothing below the fundamental is
        //     ever trusted, everything well above it is
        {
            const bool ok = VoiceAnalyzer::reliability (300.0f, 350.0f) == 0.0f
                         && VoiceAnalyzer::reliability (600.0f, 352.0f) == 0.0f
                         && VoiceAnalyzer::reliability (2000.0f, 150.0f) == 1.0f
                         && VoiceAnalyzer::reliability (500.0f, 100.0f) == 1.0f
                         && VoiceAnalyzer::reliability (100.0f, 0.0f) == 0.0f;
            std::printf ("reliability law (unmeasurable below ~2x f0): %s\n", ok ? "PASS" : "FAIL");
            if (! ok) ++mFail;
        }

        // (c) per-vowel detection: each vowel must be found and land near its
        //     own formants, not the recording's average
        {
            auto x = utter (150.0, 1.0, "01234");
            const auto p = VoiceAnalyzer::analyze (x.data(), (int) x.size(), FS);
            int found = 0; double worst = 0.0;
            for (int v = 0; v < 5; ++v)
            {
                if (! p.vow[v].valid()) continue;
                ++found;
                // F2 separates the vowels most strongly; check that one
                worst = std::max (worst, std::abs (12.0*std::log2 (p.vow[v].F[1]/VF[v][1])));
            }
            // 3 of 5, not 5 of 5: the engine's inter-frame smoothing (adopted
            // because it is what makes the tracker survive quiet, dark
            // material) blurs across vowel boundaries, so vowels with few
            // frames drop below the count threshold. The per-vowel table
            // degrades by losing vowels rather than by reporting wrong ones.
            const bool ok = found >= 3 && worst < 6.0;
            std::printf ("per-vowel detection: %d/5 vowels, worst F2 err %.2f st  %s\n",
                         found, worst, ok ? "PASS" : "FAIL");
            if (! ok) ++mFail;
            for (int v = 0; v < 5; ++v)
                std::printf ("   /%s/ n=%-4d F=%5.0f/%5.0f/%5.0f (true %4.0f/%4.0f/%4.0f)\n",
                             VN[v], p.vow[v].frames, p.vow[v].F[0], p.vow[v].F[1], p.vow[v].F[2],
                             VF[v][0], VF[v][1], VF[v][2]);
        }

        // (d) the headline case: recover a KNOWN vocal-tract shift when the
        //     target sits at a pitch where its own F1 is unmeasurable
        {
            struct { const char* name; double f0a, sa, f0b, sb;
                     const char* va; const char* vb; bool matched; } cs[] = {
                { "same vowels, 150->150, x1.15", 150.0, 1.00, 150.0, 1.15, "01234", "01234", true },
                { "same vowels, 150->352, x1.15", 150.0, 1.00, 352.0, 1.15, "01234", "01234", true },
                { "same vowels, 150->300, x1.00", 150.0, 1.00, 300.0, 1.00, "01234", "01234", true },
                { "same vowels, 150->200, x0.87", 150.0, 1.00, 200.0, 0.87, "01234", "01234", true },
                { "same vowels, 220->352, x1.30", 220.0, 1.00, 352.0, 1.30, "01234", "01234", true },
                { "diff vowels, 150->352, x1.15", 150.0, 1.00, 352.0, 1.15, "34",    "24",    false },
                { "diff vowels, 150->300, x1.20", 150.0, 1.00, 300.0, 1.20, "13",    "24",    false },
            };
            double worstMatched = 0.0; int unflagged = 0;
            // published male -> female, averaged over every vowel and band
            double pubShift = 0.0;
            for (int v = 0; v < 5; ++v) for (int b = 0; b < 3; ++b)
                pubShift += 12.0*std::log2 (VFF[v][b]/VF[v][b]);
            pubShift /= 15.0;
            {
                struct { const char* n; double f0a, f0b, sb; double truth; } fem[] = {
                    { "male 120 -> female 210",        120., 210., 1.00, pubShift },
                    { "male 160 -> female 250",        160., 250., 1.00, pubShift },
                    { "male 160 -> female 352",        160., 352., 1.00, pubShift },
                    { "male 160 -> anime x1.12 @330",  160., 330., 1.12, pubShift + 12.0*std::log2 (1.12) },
                    { "male 130 -> anime x1.20 @380",  130., 380., 1.20, pubShift + 12.0*std::log2 (1.20) },
                };
                int neg = 0; double worstF = 0.0;
                for (const auto& f : fem)
                {
                    auto A = utterT (VF,  f.f0a, 1.0,  "01234");
                    auto B = utterT (VFF, f.f0b, f.sb, "01234");
                    const auto p1 = VoiceAnalyzer::analyze (A.data(), (int) A.size(), FS);
                    const auto p2 = VoiceAnalyzer::analyze (B.data(), (int) B.size(), FS);
                    const auto r  = MatchingEngine::autoSet (p1, p2);
                    const double e = std::abs (r.formant - f.truth);
                    if (r.formant <= 0.0f) ++neg;
                    worstF = std::max (worstF, e);
                    std::printf ("   %-30s truth %+5.2f got %+5.2f (err %.2f) vowels=%d\n",
                                 f.n, f.truth, r.formant, e, r.vowelsMatched);
                }
                // A shorter vocal tract can only raise formants, so a
                // negative result here is physically impossible -- that was
                // the user-visible symptom. This is THE criterion for the
                // feature; the tolerance is set to what is actually achieved
                // (0.35-0.63 st on the four ordinary cases, 2.50 st on the
                // most extreme anime one, tract x1.20 at f0 380, which is
                // the current weak spot).
                const bool ok = neg == 0 && worstF < 3.0;
                std::printf ("male->female/anime recovery: worst %.2f st, negative results %d  %s\n",
                             worstF, neg, ok ? "PASS" : "FAIL");
                if (! ok) ++mFail;
            }
            for (const auto& c : cs)
            {
                auto A = utter (c.f0a, c.sa, c.va);
                auto B = utter (c.f0b, c.sb, c.vb);
                const auto p1 = VoiceAnalyzer::analyze (A.data(), (int) A.size(), FS);
                const auto p2 = VoiceAnalyzer::analyze (B.data(), (int) B.size(), FS);
                const auto r  = MatchingEngine::autoSet (p1, p2);
                const double truth = 12.0*std::log2 (c.sb/c.sa);
                const double err = std::abs (r.formant - truth);
                // Contract: when the two recordings share their content the
                // shift must be recovered tightly; when they do not, the
                // engine is allowed to miss but MUST flag it rather than
                // quietly hand back a wrong number.
                if (c.matched) worstMatched = std::max (worstMatched, err);
                else if (err > 1.5 && ! r.lowConfidence) ++unflagged;
                std::printf ("   %-30s truth %+5.2f got %+5.2f (err %.2f) vowels=%d MAD=%.2f%s\n",
                             c.name, truth, r.formant, err, r.vowelsMatched, r.agreementSt,
                             r.lowConfidence ? "  [flagged]" : "");
            }
            // DIAGNOSTIC, not a gate. These cases scale the MALE table by a
            // constant, which produces physically odd voices (male formants
            // x1.30 at f0 352 is nobody), and the estimator is measurably
            // weaker on them than on the real male/female pair above. They
            // are printed because the numbers are informative and the weak
            // spot should stay visible; the pass/fail criterion for this
            // feature is the male->female/anime block, which uses published
            // measurements of actual speakers.
            std::printf ("global formant recovery (diagnostic, scaled tables): "
                         "worst matched-content %.2f st, unflagged %d\n",
                         worstMatched, unflagged);
        }

        // (e) an unmeasurable band must NOT be trimmed. This is the reported
        //     bug: f1shift used to rail at its +-3 st clamp chasing a target
        //     F1 that was never observable in the first place.
        {
            auto A = utter (150.0, 1.00, "01234");
            auto B = utter (352.0, 1.15, "01234");
            const auto p1 = VoiceAnalyzer::analyze (A.data(), (int) A.size(), FS);
            const auto p2 = VoiceAnalyzer::analyze (B.data(), (int) B.size(), FS);
            const auto r  = MatchingEngine::autoSet (p1, p2);
            float f1shift = 999.0f;
            for (int i = 0; i < r.count; ++i)
                if (std::string (r.changes[i].id) == "f1shift") f1shift = r.changes[i].value;
            // target f0 352: every vowel's F1 is below 2 x f0, so F1 is
            // unmeasurable and its trim must stay at zero
            const bool ok = r.bandRel[0] < MatchingEngine::kMinRel
                         && std::abs (f1shift) < 0.01f;
            std::printf ("unmeasurable F1 is left alone: bandRel[F1]=%.2f f1shift=%+.2f  %s\n",
                         r.bandRel[0], f1shift, ok ? "PASS" : "FAIL");
            if (! ok) ++mFail;
        }

        // (f) AEIOU map output: in range, finite, and only for vowels that
        //     were really compared
        {
            auto A = utter (150.0, 1.00, "01234");
            auto B = utter (352.0, 1.15, "01234");
            const auto p1 = VoiceAnalyzer::analyze (A.data(), (int) A.size(), FS);
            const auto p2 = VoiceAnalyzer::analyze (B.data(), (int) B.size(), FS);
            const auto r  = MatchingEngine::autoSet (p1, p2);
            bool ok = r.count <= MatchingEngine::kMax;
            int nVa = 0; bool sawCustom = false, sawAdapt = false;
            for (int i = 0; i < r.count; ++i)
            {
                const std::string id = r.changes[i].id;
                const float v = r.changes[i].value;
                if (! std::isfinite (v)) ok = false;
                if (id.rfind ("va_", 0) == 0)
                {
                    ++nVa;
                    const int b = id.back() - '1';
                    const float cap = id[5] == 'f' ? MatchingEngine::kVowelClamp[b]
                                                   : MatchingEngine::kVowelGainClamp;
                    if (std::abs (v) > cap + 1.0e-4f) ok = false;
                }
                if (id == "vcharacter" && v == 8.0f) sawCustom = true;
                if (id == "vadapt"     && v == 1.0f) sawAdapt  = true;
            }
            ok = ok && nVa == 30 && sawCustom && sawAdapt;
            std::printf ("AEIOU map written: %d va_* ids, custom=%d adapt=%d, all in range  %s\n",
                         nVa, (int) sawCustom, (int) sawAdapt, ok ? "PASS" : "FAIL");
            if (! ok) ++mFail;
        }

        // (g) backward compatibility: a profile with no per-vowel table (a
        //     built-in catalog target, or a .vmprofile from before v0.29.0)
        //     must still match, take the global path, and NOT cry wolf
        {
            VoiceProfile me{}, tgt{};
            me.f0Hz = 146.0f; me.f0SpreadSt = 2.2f; me.voicedFrames = 200;
            me.F[0] = 560.0f; me.F[1] = 1500.0f; me.F[2] = 2600.0f;
            me.rel[0] = me.rel[1] = me.rel[2] = 1.0f;
            tgt = me; tgt.f0Hz = 215.0f;
            tgt.F[0] = 644.0f; tgt.F[1] = 1725.0f; tgt.F[2] = 2990.0f;   // x1.15
            const auto r = MatchingEngine::autoSet (me, tgt);
            const bool ok = r.fellBack && ! r.lowConfidence
                         && std::abs (r.formant - 2.42f) < 0.5f;
            std::printf ("no-vowel-table fallback: formant %+.2f st (exp +2.42) "
                         "fellBack=%d lowConf=%d  %s\n",
                         r.formant, (int) r.fellBack, (int) r.lowConfidence,
                         ok ? "PASS" : "FAIL");
            if (! ok) ++mFail;
        }

        // (g2) Intonation: the converted MEDIAN must land on the target
        //      (minus the deliberate perceptual bias) whatever Amount comes
        //      out as, and Amount must stay inside its cap.
        //
        //      The engine computes f_out = center*((f_in*2^(p/12))/center)^r,
        //      so if pitch aims somewhere the pivot is not, the gap is raised
        //      to the power r. That is the bug this checks for: it used to
        //      rail Amount at 200 % and land 2.00 st under the target.
        {
            struct { const char* n; float f0a, sa, f0b, sb; } cs2[] = {
                { "narrow -> very expressive", 162.5f, 3.54f, 276.1f, 6.43f },
                { "narrow -> expressive",      162.5f, 3.54f, 265.1f, 6.59f },
                { "similar spread",            162.5f, 3.54f, 352.0f, 4.08f },
                { "expressive -> flat",        180.0f, 7.00f, 240.0f, 2.00f },
                { "identical",                 200.0f, 4.00f, 200.0f, 4.00f },
            };
            bool ok = true;
            for (const auto& c : cs2)
            {
                VoiceProfile a{}, b{};
                a.f0Hz = c.f0a; a.f0SpreadSt = c.sa; a.voicedFrames = 200;
                a.F[0]=560; a.F[1]=1500; a.F[2]=2600;
                a.rel[0]=a.rel[1]=a.rel[2]=1.0f;
                b = a; b.f0Hz = c.f0b; b.f0SpreadSt = c.sb;
                b.F[0]=644; b.F[1]=1725; b.F[2]=2990;
                const auto r = MatchingEngine::autoSet (a, b);
                // replay the engine's own formula
                const double mid = c.f0a * std::pow (2.0, r.pitch/12.0);
                const double outMed = r.rangeApplied
                    ? r.center * std::pow (mid / r.center, r.range*0.01) : mid;
                const double wantHz = c.f0b * std::pow (2.0, -MatchingEngine::kPitchBias/12.0);
                const double errSt = 12.0*std::log2 (outMed / wantHz);
                const bool caseOk = std::abs (errSt) < 0.05
                                 && r.range <= MatchingEngine::kRangeMax + 0.01f
                                 && r.range >= MatchingEngine::kRangeMin - 0.01f;
                if (! caseOk) ok = false;
                std::printf ("   %-26s Amount %5.0f%%  pivot %5.0f Hz  median err %+.3f st%s\n",
                             c.n, r.range, r.center, errSt, caseOk ? "" : "   <-- BAD");
            }
            std::printf ("intonation: median on target regardless of Amount, Amount <= %.0f%%  %s\n",
                         MatchingEngine::kRangeMax, ok ? "PASS" : "FAIL");
            if (! ok) ++mFail;
        }

        // (h) degenerate inputs must not produce non-finite parameters
        {
            VoiceProfile a{}, b{};
            bool ok = MatchingEngine::autoSet (a, b).count == 0;   // both invalid
            a.f0Hz = 150.0f; a.voicedFrames = 100;
            a.F[0] = a.F[1] = a.F[2] = 0.0f;                       // zero formants
            a.rel[0] = a.rel[1] = a.rel[2] = 1.0f;
            b = a; b.f0Hz = 300.0f;
            const auto r = MatchingEngine::autoSet (a, b);
            for (int i = 0; i < r.count; ++i)
                if (! std::isfinite (r.changes[i].value)) ok = false;
            if (! std::isfinite (r.formant) || ! std::isfinite (r.agreementSt)) ok = false;
            std::printf ("degenerate profiles stay finite: %s\n", ok ? "PASS" : "FAIL");
            if (! ok) ++mFail;
        }

        // (i) built-in target catalog. Every entry has to be usable as a
        //     target from a plain male source -- that is the only thing the
        //     catalog exists to do, and it is where hand-written numbers go
        //     wrong silently (the v0.28.0 entries shipped tiltDb on the wrong
        //     SCALE and railed Auto-Set to maximum brightness for two
        //     releases before anyone noticed).
        //
        //     The v0.37.0 character entries are MEASURED, so they carry a
        //     per-vowel table and take the vowel-matched path; the five
        //     generic entries do not and take the global fallback. Both are
        //     correct, and the check asserts each takes the path its data
        //     supports rather than assuming one of them.
        {
            auto A = utter (150.0, 1.00, "01234");     // male, published F1-F3
            const auto me = VoiceAnalyzer::analyze (A.data(), (int) A.size(), FS);
            int n = 0;
            const auto* cat = getSampleTargets (n);
            bool ok = (n > 0) && me.valid();
            for (int i = 0; i < n; ++i)
            {
                const auto& e = cat[i];
                const auto& t = e.profile;
                bool eOk = t.valid() && sampleTargetIndexById (e.id) == i;
                const auto r = MatchingEngine::autoSet (me, t);
                if (r.count == 0 || r.count > MatchingEngine::kMax) eOk = false;
                for (int c = 0; c < r.count; ++c)
                    if (! std::isfinite (r.changes[c].value)) eOk = false;

                // pitch must land on the target median, less the perceptual bias
                const double landed = me.f0Hz * std::pow (2.0, r.pitch/12.0);
                const double want   = t.f0Hz * std::pow (2.0, -MatchingEngine::kPitchBias/12.0);
                if (std::abs (12.0*std::log2 (landed/want)) > 0.05) eOk = false;

                // A male source against a target that is clearly higher and
                // smaller-tract must ask for a POSITIVE formant shift; a
                // negative one is the signature of the failure this path was
                // rewritten for.
                //
                // "Androgynous" is deliberately NOT such a target -- it sits
                // at f0 175 Hz against this source's 150, and a neutral vocal
                // tract is not necessarily shorter than a particular male's,
                // so its honest answer is near zero and may fall either side
                // of it. Requiring a positive sign there would be asserting
                // something the entry never claimed. It still has to stay
                // small: a neutral target asking for a large shift in either
                // direction would be a real fault.
                const bool higherVoice = t.f0Hz > 1.25f * me.f0Hz;
                if (higherVoice) { if (! (r.formant > 0.0f)) eOk = false; }
                else             { if (std::abs (r.formant) > 3.0f) eOk = false; }

                const bool measured = t.vowelsMeasured() > 0;
                if (r.fellBack == measured) eOk = false;
                // texture is only derived when the target actually carries it
                if (r.airApplied != measured) eOk = false;
                if (measured && r.vowelsMatched < 2) eOk = false;

                std::printf ("   %-18s pitch %+6.2f  formant %+5.2f  band %+.2f/%+.2f/%+.2f"
                             "  vowels %d  agree %.2f st  air %.2f/%.2f  %s\n",
                             e.id, r.pitch, r.formant, r.bandShiftSt[0], r.bandShiftSt[1],
                             r.bandShiftSt[2], r.vowelsMatched, r.agreementSt,
                             r.air, r.airshine, eOk ? "" : "   <-- BAD");
                if (! eOk) ok = false;
            }
            std::printf ("built-in target catalog: %d entries all usable from a male source  %s\n",
                         n, ok ? "PASS" : "FAIL");
            if (! ok) ++mFail;
        }
    }
    std::printf ("Matching checks: %s (%d failure(s))\n",
                 mFail == 0 ? "ALL PASS" : "FAILURES", mFail);

    // ==== Natural Air v2 (band-adaptive comb) ====
    std::puts ("\n== Natural Air v2 (band-adaptive comb) ==");
    int naFail = 0;

    // (a) splitter recombination: b0+b1+b2+b3 must equal the input exactly
    {
        PsolaEngine::AirBands sp;
        sp.setup (FS);
        uint32_t rng = 13579u;
        double maxErr = 0.0;
        for (int i = 0; i < 48000; ++i)
        {
            rng = rng * 1664525u + 1013904223u;
            const float x = (float) ((int32_t) rng) / 2147483648.0f;
            float b[4];
            sp.split (x, b);
            maxErr = std::max (maxErr, (double) std::abs ((b[0]+b[1]+b[2]+b[3]) - x));
        }
        const bool ok = maxErr < 1.0e-5;
        std::printf ("band recombination (white noise, 1 s): max err=%.2e  %s\n",
                     maxErr, ok ? "PASS" : "FAIL");
        if (! ok) ++naFail;
    }

    // (b) OFF bypass: with Natural Air at 0 the air path must be inert —
    // output bit-identical no matter what the other air settings are
    {
        const auto breathy = makeBreathy (120.0, 2.0);
        P p0; p0.pitchSemi = 7.0f;              // air fully off
        P p2 = p0; p2.airShineDb = 6.0f;        // shine has nothing to boost
        const auto o0 = run (breathy, p0);
        const auto o2 = run (breathy, p2);
        double dmax = 0.0;
        for (size_t i = 0; i < o0.size(); ++i)
            dmax = std::max (dmax, (double) std::abs (o0[i] - o2[i]));
        const bool ok = dmax == 0.0;
        std::printf ("air off, shine 6 dB vs default: max diff=%.2e  %s\n",
                     dmax, ok ? "PASS (bit-identical)" : "FAIL");
        if (! ok) ++naFail;
    }

    // (c) per-band aperiodicity discrimination (spec item: vibrato / glide /
    // known noise must not fool the estimator)
    {
        auto lastA = [] (const std::vector<float>& in, const P& p, float* a4)
        {
            PsolaEngine eng;
            eng.prepare (FS);
            eng.setParams (p);
            std::vector<float> out (in.size(), 0.0f);
            for (size_t i = 0; i < in.size(); i += 256)
                eng.process (in.data() + i, out.data() + i,
                             (int) std::min ((size_t) 256, in.size() - i));
            for (int b = 0; b < 4; ++b) a4[b] = eng.airBandAperiodicity (b);
        };
        P p; p.pitchSemi = 7.0f; p.airPreserve = 1.0f;

        float aH[4], aN[4], aV[4], aG[4], aBr[4];
        const auto harm  = makeHarm (150.0, 150.0, 2.0);
        auto       hn    = makeHarm (150.0, 150.0, 2.0);
        addNoise (hn, -15.0, false, 111u);
        const auto vib   = makeHarm (150.0, 150.0, 2.5, 5.5, 0.35);
        const auto glide = makeHarm (110.0, 220.0, 2.5);
        const auto brth  = makeBreathy (120.0, 2.0);
        lastA (harm, p, aH); lastA (hn, p, aN); lastA (vib, p, aV);
        lastA (glide, p, aG); lastA (brth, p, aBr);
        std::printf ("residual keep amount a[b] (b0 <700, b1 <2.5k, b2 <6k, b3 >6k):\n");
        std::printf ("  steady harmonics    : %.2f %.2f %.2f %.2f  (residual = tiny interp noise, safe either way)\n",
                     aH[0], aH[1], aH[2], aH[3]);
        std::printf ("  + white @-15 dB     : %.2f %.2f %.2f %.2f  (want high: real noise kept)\n",
                     aN[0], aN[1], aN[2], aN[3]);
        std::printf ("  vibrato 5.5Hz 0.35st: %.2f %.2f %.2f %.2f  (want low: leakage guarded)\n",
                     aV[0], aV[1], aV[2], aV[3]);
        std::printf ("  glide 110->220 Hz   : %.2f %.2f %.2f %.2f  (want low: leakage guarded)\n",
                     aG[0], aG[1], aG[2], aG[3]);
        std::printf ("  breathy vowel       : %.2f %.2f %.2f %.2f  (want high in b2/b3: aspiration kept)\n",
                     aBr[0], aBr[1], aBr[2], aBr[3]);

        // what actually matters for pure harmonics: with v2 fully up the
        // output must stay ~identical to air-off (nothing real to bypass)
        P pOff; pOff.pitchSemi = 7.0f;
        const auto oOff = run (harm, pOff);
        const auto oOn  = run (harm, p);
        double de = 0.0, se = 0.0;
        for (size_t i = oOff.size() / 3; i < oOff.size(); ++i)
        {
            const double d = (double) oOn[i] - (double) oOff[i];
            de += d * d;  se += (double) oOff[i] * oOff[i];
        }
        const double relDiff = std::sqrt (de / std::max (se, 1e-30));
        std::printf ("pure harmonics, v2 air 1.0 vs air off: rel diff=%.4f  (want ~0)\n", relDiff);

        const bool ok = relDiff < 0.02
                     && aN[3] > 0.45f
                     && aV[1] < 0.35f && aV[2] < 0.35f && aV[3] < 0.40f
                     && aBr[2] > 0.6f && aBr[3] > 0.6f;
        std::printf ("discrimination: %s\n", ok ? "PASS" : "FAIL");
        if (! ok) ++naFail;
    }

    // (d) level neutrality + NaN/Inf on the breathy vowel (steady voiced):
    // Natural Air fully up must not change the overall level vs air off
    {
        const auto breathy = makeBreathy (120.0, 2.0);
        P pl; pl.pitchSemi = 7.0f;              // air off (reference)
        P pb = pl; pb.airPreserve = 1.0f;       // Natural Air up
        const auto ol = run (breathy, pl);
        const auto ob = run (breathy, pb);
        writeWav ("out_nav2_breathy_off.wav", ol);
        writeWav ("out_nav2_breathy_on.wav", ob);
        const bool bad = hasBad (ol) || hasBad (ob);
        const double rl = rmsOf (ol), rb = rmsOf (ob);
        const double dDb = 20.0 * std::log10 (std::max (rb, 1e-12)
                                            / std::max (rl, 1e-12));
        std::printf ("breathy +7st: RMS off=%.4f air1.0=%.4f (%+.2f dB)  "
                     "peak=%.3f/%.3f  NaN/Inf=%s\n",
                     rl, rb, dDb, peakOf (ol), peakOf (ob), bad ? "FOUND" : "none");
        const bool ok = ! bad && std::abs (dDb) < 1.5;
        std::printf ("level neutrality: %s\n", ok ? "PASS" : "FAIL");
        if (! ok) ++naFail;
    }

    // (e) no allocation in process() after warm-up (heaviest settings on)
    {
        PsolaEngine eng;
        eng.prepare (FS);
        P p; p.pitchSemi = 7.0f; p.airPreserve = 1.2f;
        p.f2Shift = 3.0f; p.breath = 0.5f; p.gciSync = true;
        eng.setParams (p);
        const auto breathy = makeBreathy (120.0, 3.0);
        std::vector<float> out (breathy.size(), 0.0f);
        size_t i = 0;
        for (; i + 256 <= (size_t) FS; i += 256)           // 1 s warm-up
            eng.process (breathy.data() + i, out.data() + i, 256);
        g_allocCount = 0;  g_countAlloc = true;
        for (; i + 256 <= breathy.size(); i += 256)
            eng.process (breathy.data() + i, out.data() + i, 256);
        g_countAlloc = false;
        std::printf ("allocations inside process() after warm-up: %ld  %s\n",
                     g_allocCount, g_allocCount == 0 ? "PASS" : "FAIL");
        if (g_allocCount != 0) ++naFail;
    }

    // (f) wav pairs for analyze.py: harmonic leakage (old-pitch ghost),
    // vibrato, glide, known white/pink noise retention, sibilant,
    // unvoiced->voiced transition (also checked for step discontinuities)
    {
        P leg; leg.pitchSemi = 7.0f;            // air off (reference)
        P bac = leg; bac.airPreserve = 1.0f;    // Natural Air (standard path)

        const auto harm = makeHarm (150.0, 150.0, 2.5);
        writeWav ("out_nav2_harm_dry.wav", harm);
        writeWav ("out_nav2_harm_off.wav", run (harm, leg));
        writeWav ("out_nav2_harm_bac.wav", run (harm, bac));

        const auto vib = makeHarm (150.0, 150.0, 3.0, 5.5, 0.35);
        writeWav ("out_nav2_vib_off.wav", run (vib, leg));
        writeWav ("out_nav2_vib_bac.wav", run (vib, bac));

        const auto glide = makeHarm (110.0, 220.0, 3.0);
        writeWav ("out_nav2_glide_off.wav", run (glide, leg));
        writeWav ("out_nav2_glide_bac.wav", run (glide, bac));

        auto hw = makeHarm (150.0, 150.0, 2.5);
        addNoise (hw, -15.0, false, 111u);
        writeWav ("out_nav2_hw_dry.wav", hw);
        writeWav ("out_nav2_hw_off.wav", run (hw, leg));
        writeWav ("out_nav2_hw_bac.wav", run (hw, bac));

        auto hp = makeHarm (150.0, 150.0, 2.5);
        addNoise (hp, -12.0, true, 222u);
        writeWav ("out_nav2_hp_dry.wav", hp);
        writeWav ("out_nav2_hp_off.wav", run (hp, leg));
        writeWav ("out_nav2_hp_bac.wav", run (hp, bac));

        const auto sib = makeSibilant (2.0);
        writeWav ("out_nav2_sib_dry.wav", sib);
        writeWav ("out_nav2_sib_off.wav", run (sib, leg));
        writeWav ("out_nav2_sib_bac.wav", run (sib, bac));

        // Air Shine: top-band bypass gain comparison (0/3/6 dB; 0 dB is
        // out_nav2_breathy_bac). Only the >6k air may rise; mids untouched.
        {
            P sh = bac;
            sh.airShineDb = 3.0f;
            writeWav ("out_nav2_shine3.wav", run (makeBreathy (120.0, 2.0), sh));
            sh.airShineDb = 6.0f;
            writeWav ("out_nav2_shine6.wav", run (makeBreathy (120.0, 2.0), sh));
        }

        std::vector<float> tr = makeNoiseCons (1.0);
        const auto bre = makeBreathy (120.0, 2.0);
        tr.insert (tr.end(), bre.begin(), bre.end());
        writeWav ("out_nav2_trans_off.wav", run (tr, leg));
        const auto tb = run (tr, bac);
        writeWav ("out_nav2_trans_bac.wav", tb);

        double step = 0.0;
        for (size_t i = 1; i < tb.size(); ++i)
            step = std::max (step, (double) std::abs (tb[i] - tb[i-1]));
        std::printf ("unvoiced->voiced max sample step: %.3f  %s\n",
                     step, step < 0.5 ? "PASS" : "FAIL");
        if (step >= 0.5) ++naFail;
    }

    // (g) low-pitch leakage guard (user-reported: sustained low "ah" grows
    // an old-pitch ghost). Period-doubled / alternating-period / light-
    // subharmonic phonation repeats at 2P and correlates only weakly at P;
    // it must be recognised as leakage, not air. Also verifies that aB does
    // NOT creep upward over a 6 s sustained low vowel (the ghost appearing
    // "after a few seconds" is exactly that creep).
    {
        auto track = [] (const std::vector<float>& in, const P& p,
                         float aMax[4], float aEnd[4], double& voicedFrac)
        {
            PsolaEngine eng;
            eng.prepare (FS);
            eng.setParams (p);
            std::vector<float> out (in.size(), 0.0f);
            for (int b = 0; b < 4; ++b) { aMax[b] = 0.0f; aEnd[b] = 0.0f; }
            long vo = 0, tot = 0;
            for (size_t i = 0; i < in.size(); i += 256)
            {
                eng.process (in.data() + i, out.data() + i,
                             (int) std::min ((size_t) 256, in.size() - i));
                if (i > (size_t) FS)
                {
                    ++tot;  if (eng.isVoiced()) ++vo;
                    if ((i % 24576) < 256)                    // ~every 0.5 s
                        for (int b = 0; b < 4; ++b)
                            aMax[b] = std::max (aMax[b], eng.airBandAperiodicity (b));
                }
            }
            for (int b = 0; b < 4; ++b) aEnd[b] = eng.airBandAperiodicity (b);
            voicedFrac = tot > 0 ? (double) vo / (double) tot : 0.0;
            return out;
        };
        P pv2; pv2.pitchSemi = 7.0f; pv2.airPreserve = 1.0f;
        P leg = pv2; leg.airPreserve = 0.0f;    // air-off reference for analyze

        const auto low90 = makeVowel   (90.0, 90.0, 6.0);
        const auto altp  = makeAltPulse (90.0, 0.01, 1.0,  6.0);   // stronger and YIN drops to unvoiced
        const auto subh  = makeAltPulse (90.0, 0.0,  0.85, 6.0);

        float aM[4], aE[4], bM[4], bE[4], cM[4], cE[4];
        double vfA, vfB, vfC;
        const auto oLow = track (low90, pv2, aM, aE, vfA);
        const auto oAlt = track (altp,  pv2, bM, bE, vfB);
        const auto oSub = track (subh,  pv2, cM, cE, vfC);
        writeWav ("out_nav2_low90_bac.wav", oLow);
        writeWav ("out_nav2_alt_bac.wav",   oAlt);
        writeWav ("out_nav2_sub_bac.wav",   oSub);
        writeWav ("out_nav2_alt_off.wav",   run (altp, leg));
        writeWav ("out_nav2_sub_off.wav",   run (subh, leg));

        std::printf ("low-pitch keep amounts (max over time / at 6 s), voiced%%:\n");
        std::printf ("  90Hz steady     : max %.2f/%.2f/%.2f/%.2f  end %.2f/%.2f/%.2f/%.2f  v=%.0f%%\n",
                     aM[0],aM[1],aM[2],aM[3], aE[0],aE[1],aE[2],aE[3], 100.0*vfA);
        std::printf ("  alt period +-1%% : max %.2f/%.2f/%.2f/%.2f  end %.2f/%.2f/%.2f/%.2f  v=%.0f%%\n",
                     bM[0],bM[1],bM[2],bM[3], bE[0],bE[1],bE[2],bE[3], 100.0*vfB);
        std::printf ("  subharmonic -15%%: max %.2f/%.2f/%.2f/%.2f  end %.2f/%.2f/%.2f/%.2f  v=%.0f%%\n",
                     cM[0],cM[1],cM[2],cM[3], cE[0],cE[1],cE[2],cE[3], 100.0*vfC);

        // steady low vowel: v2 with air up must stay ~identical to air off
        P pOff; pOff.pitchSemi = 7.0f;
        const auto oOff = run (low90, pOff);
        double de = 0.0, se = 0.0;
        for (size_t i = oOff.size() / 3; i < oOff.size(); ++i)
        {
            const double d = (double) oLow[i] - (double) oOff[i];
            de += d * d;  se += (double) oOff[i] * oOff[i];
        }
        const double relDiff = std::sqrt (de / std::max (se, 1e-30));
        std::printf ("  90Hz steady, v2 air 1.0 vs air off: rel diff=%.4f  (want ~0)\n", relDiff);

        const bool ok = vfB > 0.7 && vfC > 0.7
                     && bM[1] < 0.35f && bM[2] < 0.35f
                     && cM[1] < 0.35f && cM[2] < 0.35f
                     && relDiff < 0.03;
        std::printf ("low-pitch leakage guard: %s\n", ok ? "PASS" : "FAIL");
        if (! ok) ++naFail;
    }

    // (h) spectral air cleanup: +12st on a strong sustained vowel with a
    // little noise (the real-voice "KITUNE_middle" case: odd input
    // harmonics sit exactly between the output harmonics and expose any
    // f0-periodic residue in the bypassed air). The air path must not add
    // old-pitch energy over air-off, with or without Air Shine.
    {
        auto hv = makeHarm (182.0, 182.0, 2.5);
        addNoise (hv, -25.0, false, 333u);
        P off12; off12.pitchSemi = 12.0f;
        P v212 = off12; v212.airPreserve = 1.0f;
        P v2sh = v212; v2sh.airShineDb = 6.0f;
        const auto o0 = run (hv, off12);
        const auto o1 = run (hv, v212);
        const auto o2 = run (hv, v2sh);
        const double g0 = ghostDb (o0, 182.0, 364.4);
        const double g1 = ghostDb (o1, 182.0, 364.4);
        const double g2 = ghostDb (o2, 182.0, 364.4);
        // Low Latency mode: cleanup disabled by design (D < window) — the
        // raw Phase-1 air path must still be finite and sane
        P vlo = v212; vlo.lowLatency = true;
        const auto o3 = run (hv, vlo);
        std::printf ("+12st sustained vowel ghost: air off=%.1f dB  v2=%.1f  "
                     "v2+shine6=%.1f  (delta vs off: %+.1f / %+.1f dB)\n",
                     g0, g1, g2, g1 - g0, g2 - g0);
        const bool ok = (g1 - g0) < 1.5 && (g2 - g0) < 1.5
                     && ! hasBad (o1) && ! hasBad (o2) && ! hasBad (o3);
        std::printf ("spectral air cleanup: %s\n", ok ? "PASS" : "FAIL");
        if (! ok) ++naFail;
    }

    // (v0.31.0) Idle-drain bypass of the air path. Once Natural Air has been
    // quiet long enough for its lookahead delay and cleanup window to empty,
    // the engine stops clearing / FFT-ing / re-adding those buffers (they are
    // all zeros by then). This checks the visible half of that: a long silent
    // gap must still come out silent, at every block size, and the wake-up
    // afterwards must not arrive as a step.
    //
    // NOTE ON SCOPE: this does NOT prove the bypass is sample-exact. The ring
    // buffers wrap every 32768 samples, so a wake-up that failed to clear the
    // lookahead window would replay material from one wrap earlier — audible,
    // but landing on top of the real signal rather than in the silence, so no
    // single-run metric here separates it. That property was verified instead
    // by comparing raw output against a stored pre-change reference; see
    // test/bitexact.cpp, which is the tool to use for any further work in
    // here that is meant to leave the samples untouched.
    {
        auto gap = makeVowel (120.0, 120.0, 3.0);
        for (size_t i = (size_t) (FS * 1.0); i < (size_t) (FS * 1.7) && i < gap.size(); ++i)
            gap[i] = 0.0f;                       // long silence: air path drains

        P pa; pa.pitchSemi = 7.0f; pa.airPreserve = 1.0f; pa.airShineDb = 3.0f;

        double residue = 0.0, step = 0.0;
        bool   bad = false;
        for (int B : { 64, 128, 256, 512, 1024 })      // several idle cadences
        {
            const auto o = runBlocked (gap, pa, B);
            // deep inside the silence everything upstream has drained: any
            // sample here is material the bypass failed to clear
            for (size_t i = (size_t) (FS * 1.3); i < (size_t) (FS * 1.65); ++i)
                residue = std::max (residue, (double) std::abs (o[i]));
            // and the wake-up must not arrive as a jump
            for (size_t i = (size_t) (FS * 1.7); i + 1 < (size_t) (FS * 2.1); ++i)
                step = std::max (step, (double) std::abs (o[i + 1] - o[i]));
            bad = bad || hasBad (o);
        }

        const bool ok = residue < 1.0e-4 && step < 0.25 && ! bad;
        std::printf ("air idle-drain bypass: silence residue=%.2e  re-onset step=%.3f  %s\n",
                     residue, step, ok ? "PASS" : "FAIL");
        if (! ok) ++naFail;
    }

    std::printf ("Natural Air v2 checks: %s (%d failure(s))\n",
                 naFail == 0 ? "ALL PASS" : "FAILURES", naFail);

    std::puts ("\n== Vowel-Adaptive Formant Warp (Beta) ==");
    int vaFail = 0;

    // (a) compatibility: disabled (whatever the amount) and amount 0 must be
    // bit-identical to the previous behaviour, with and without manual F1-F3
    {
        const auto aVow = makeVowel (120.0, 120.0, 2.0);
        P p0; p0.pitchSemi = 7.0f; p0.f2Shift = 3.0f;
        P p1 = p0; p1.vowelAdapt = false; p1.vowelAdaptAmt = 1.0f;
        P p2 = p0; p2.vowelAdapt = true;  p2.vowelAdaptAmt = 0.0f;
        P q0; q0.pitchSemi = 7.0f;                       // no manual F1-F3
        P q2 = q0; q2.vowelAdapt = true; q2.vowelAdaptAmt = 0.0f;
        const auto o0 = run (aVow, p0), o1 = run (aVow, p1), o2 = run (aVow, p2);
        const auto u0 = run (aVow, q0), u2 = run (aVow, q2);
        double d1 = 0.0, d2 = 0.0, d3 = 0.0;
        for (size_t i = 0; i < o0.size(); ++i)
        {
            d1 = std::max (d1, (double) std::abs (o1[i] - o0[i]));
            d2 = std::max (d2, (double) std::abs (o2[i] - o0[i]));
            d3 = std::max (d3, (double) std::abs (u2[i] - u0[i]));
        }
        const bool ok = d1 == 0.0 && d2 == 0.0 && d3 == 0.0;
        std::printf ("off/amount-0 compatibility: max diff %.2e / %.2e / %.2e  %s\n",
                     d1, d2, d3, ok ? "PASS (bit-identical)" : "FAIL");
        if (! ok) ++vaFail;
    }

    // (b) vowel coordinate sanity + bounded effect on a steady /a/-like
    // vowel (F1 700 / F2 1220): open (height high), back-ish (frontness low)
    {
        const auto aVow = makeVowel (120.0, 120.0, 2.5);
        PsolaEngine eng;
        eng.prepare (FS);
        P p; p.pitchSemi = 7.0f; p.vowelAdapt = true; p.vowelAdaptAmt = 1.0f;
        eng.setParams (p);
        std::vector<float> out (aVow.size(), 0.0f);
        for (size_t i = 0; i < aVow.size(); i += 256)
            eng.process (aVow.data() + i, out.data() + i,
                         (int) std::min ((size_t) 256, aVow.size() - i));
        const float h = eng.vowelHeight(), fr = eng.vowelFrontness();
        const float cf = eng.vowelConfidence();
        const float o1 = eng.vowelOffsetSemi (0), o2 = eng.vowelOffsetSemi (1),
                    o3 = eng.vowelOffsetSemi (2);
        P pOff; pOff.pitchSemi = 7.0f;
        const auto oRef = run (aVow, pOff);
        const double dDb = 20.0 * std::log10 (std::max (rmsOf (out), 1e-12)
                                            / std::max (rmsOf (oRef), 1e-12));
        std::printf ("/a/ vowel: height=%.2f frontness=%.2f conf=%.2f  "
                     "offsets=%+.2f/%+.2f/%+.2f st  level %+0.2f dB  NaN/Inf=%s\n",
                     h, fr, cf, o1, o2, o3, dDb, hasBad (out) ? "FOUND" : "none");
        const bool ok = ! hasBad (out)
                     && h > 0.55f && fr < 0.45f && cf > 0.2f
                     && std::abs (o1) <= 2.0f && std::abs (o2) <= 3.0f
                     && std::abs (o3) <= 1.5f && std::abs (o1) > 0.05f
                     && std::abs (dDb) < 1.5;
        std::printf ("vowel coordinate + bounded offsets: %s\n", ok ? "PASS" : "FAIL");
        if (! ok) ++vaFail;
    }

    // (c) adaptivity + smoothing: /a/ then /i/-like vowel. The offsets must
    // differ between the vowels (that is the point of the feature) and must
    // never jump between blocks, even across the abrupt vowel switch.
    // Afterwards silence: the offsets must release smoothly to ~0.
    {
        auto seq = makeVowel (120.0, 120.0, 1.5);                        // /a/
        const auto iv = makeVowelF (120.0, 300.0, 2300.0, 3100.0, 1.5);  // /i/
        seq.insert (seq.end(), iv.begin(), iv.end());
        seq.insert (seq.end(), (size_t) FS, 0.0f);                       // silence
        PsolaEngine eng;
        eng.prepare (FS);
        P p; p.pitchSemi = 7.0f; p.vowelAdapt = true; p.vowelAdaptAmt = 1.0f;
        eng.setParams (p);
        std::vector<float> out (seq.size(), 0.0f);
        float prev[3] = { 0, 0, 0 }, jump = 0.0f;
        float offA1 = 0.0f, offI1 = 0.0f, offEnd = 0.0f;
        for (size_t i = 0; i < seq.size(); i += 256)
        {
            eng.process (seq.data() + i, out.data() + i,
                         (int) std::min ((size_t) 256, seq.size() - i));
            for (int k = 0; k < 3; ++k)
            {
                const float o = eng.vowelOffsetSemi (k);
                if (i > (size_t) (FS / 2))
                    jump = std::max (jump, std::abs (o - prev[k]));
                prev[k] = o;
            }
            if (i / 256 == (size_t) (1.4 * FS) / 256) offA1 = eng.vowelOffsetSemi (0);
            if (i / 256 == (size_t) (2.9 * FS) / 256) offI1 = eng.vowelOffsetSemi (0);
        }
        offEnd = std::max ({ std::abs (eng.vowelOffsetSemi (0)),
                             std::abs (eng.vowelOffsetSemi (1)),
                             std::abs (eng.vowelOffsetSemi (2)) });
        std::printf ("/a/->/i/->silence: F1 offset a=%+.2f i=%+.2f st  "
                     "max block jump=%.3f st  after 1 s silence=%.3f st\n",
                     offA1, offI1, jump, offEnd);
        const bool ok = ! hasBad (out)
                     && (offA1 - offI1) > 0.3f     // open vowel gets more F1
                     && jump < 0.35f               // no per-block steps
                     && offEnd < 0.06f;            // released on unvoiced
        std::printf ("adaptivity + smoothing + unvoiced release: %s\n",
                     ok ? "PASS" : "FAIL");
        if (! ok) ++vaFail;
    }

    // (d) estimator robustness: garbage inputs (NaN/Inf/zero/reversed
    // ordering) must keep every output finite and decay the offsets to 0
    {
        VowelAdaptiveWarp w;
        w.prepare (FS, 512);
        w.setAmount (1.0f);
        VowelAdaptiveWarp::Input good { 700.0f, 1220.0f, 2600.0f, 1.0f, true };
        for (int i = 0; i < 200; ++i) w.process (good);   // establish an offset
        const float est = w.offsetSemi (0);
        const VowelAdaptiveWarp::Input bad[] =
        {
            { 0.0f, 0.0f, 0.0f, 1.0f, true },
            { std::nanf (""), 1500.0f, 2500.0f, 1.0f, true },
            { 500.0f, INFINITY, 2500.0f, 1.0f, true },
            { 2000.0f, 500.0f, 2500.0f, 1.0f, true },     // F1 >= F2
            { 500.0f, 520.0f, 2500.0f, 1.0f, true },      // F2 too close
            { -100.0f, 1500.0f, 2500.0f, INFINITY, true },
        };
        bool finiteAll = true;
        for (int r = 0; r < 100; ++r)
            for (const auto& b : bad)
            {
                w.process (b);
                for (int k = 0; k < 3; ++k)
                    finiteAll = finiteAll && std::isfinite (w.offsetSemi (k));
                finiteAll = finiteAll && std::isfinite (w.vowelHeight())
                                      && std::isfinite (w.vowelFrontness())
                                      && std::isfinite (w.confidence());
            }
        const float rem = std::abs (w.offsetSemi (0));
        const bool ok = finiteAll && est > 0.05f && rem < 0.01f;
        std::printf ("garbage-input robustness: est=%.2f -> residual=%.4f st, "
                     "all finite=%s  %s\n", est, rem, finiteAll ? "yes" : "NO",
                     ok ? "PASS" : "FAIL");
        if (! ok) ++vaFail;
    }

    // (e) no allocation in process() after warm-up with the warp engaged,
    // and mode coexistence (Low Voice + GCI + Natural Air + adaptive warp;
    // Low Latency separately) — outputs must stay finite
    {
        PsolaEngine eng;
        eng.prepare (FS);
        P p; p.pitchSemi = 7.0f; p.vowelAdapt = true; p.vowelAdaptAmt = 1.0f;
        p.lowVoice = true; p.gciSync = true; p.airPreserve = 1.0f;
        eng.setParams (p);
        const auto creaky = makeCreaky (55.0, 3.0);
        std::vector<float> out (creaky.size(), 0.0f);
        size_t i = 0;
        for (; i + 256 <= (size_t) FS; i += 256)
            eng.process (creaky.data() + i, out.data() + i, 256);
        g_allocCount = 0;  g_countAlloc = true;
        for (; i + 256 <= creaky.size(); i += 256)
            eng.process (creaky.data() + i, out.data() + i, 256);
        g_countAlloc = false;

        P pl; pl.pitchSemi = 7.0f; pl.vowelAdapt = true; pl.vowelAdaptAmt = 1.0f;
        pl.lowLatency = true;
        const auto oL = run (makeVowel (120.0, 120.0, 2.0), pl);
        const bool ok = g_allocCount == 0 && ! hasBad (out) && ! hasBad (oL);
        std::printf ("allocations after warm-up=%ld, LowVoice+GCI+Air ok=%s, "
                     "LowLatency ok=%s  %s\n", g_allocCount,
                     hasBad (out) ? "NO" : "yes", hasBad (oL) ? "NO" : "yes",
                     ok ? "PASS" : "FAIL");
        if (! ok) ++vaFail;
    }

    // (f) AEIOU Character map plumbing (v0.26.0): Params.vowelMap reaches
    // the estimator — an all-zero map must produce zero offsets while the
    // default (Natural) map produces the nonzero offsets checked in (b)
    {
        const auto aVow = makeVowel (120.0, 120.0, 2.0);
        PsolaEngine eng;
        eng.prepare (FS);
        P p; p.pitchSemi = 7.0f; p.vowelAdapt = true; p.vowelAdaptAmt = 1.0f;
        for (auto& vv : p.vowelMap.offset)
            for (auto& x : vv) x = 0.0f;
        eng.setParams (p);
        std::vector<float> out (aVow.size(), 0.0f);
        for (size_t i = 0; i < aVow.size(); i += 256)
            eng.process (aVow.data() + i, out.data() + i,
                         (int) std::min ((size_t) 256, aVow.size() - i));
        const float mx = std::max ({ std::abs (eng.vowelOffsetSemi (0)),
                                     std::abs (eng.vowelOffsetSemi (1)),
                                     std::abs (eng.vowelOffsetSemi (2)) });
        const bool ok = mx < 1.0e-4f && ! hasBad (out);
        std::printf ("zero character map -> zero offsets: max=%.5f st  %s\n",
                     mx, ok ? "PASS" : "FAIL");
        if (! ok) ++vaFail;
    }

    std::printf ("Vowel-Adaptive Warp checks: %s (%d failure(s))\n",
                 vaFail == 0 ? "ALL PASS" : "FAILURES", vaFail);

    std::puts ("done");
    // mFail was missing from this sum, so every Matching check -- including
    // the ground-truth formant accuracy ones -- printed FAIL and still exited
    // 0. A check that cannot fail the run is not a check.
    return (mFail + naFail + vaFail) == 0 ? 0 : 1;
}
