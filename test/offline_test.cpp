// Offline verification harness for PsolaEngine (v0.2 features included).
#include "../dsp/PsolaEngine.h"
#include "../dsp/VoiceAnalyzer.h"
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

    std::printf ("Natural Air v2 checks: %s (%d failure(s))\n",
                 naFail == 0 ? "ALL PASS" : "FAILURES", naFail);

    std::puts ("done");
    return naFail == 0 ? 0 : 1;
}
