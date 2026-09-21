#pragma once
// Explicit user-controlled Dynamic EQ. No tracker/protection feedback and no audio delay.
#include <array>
#include <vector>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <complex>

namespace vq
{
constexpr double pi = 3.14159265358979323846;
inline float finite(float x, float fallback = 0)
{
    return std::isfinite(x) ? x : fallback;
}
inline float limit(float x, float lo, float hi, float fallback = 0)
{
    return std::clamp(finite(x, fallback), lo, hi);
}
struct Band
{
    bool on = true, dyn = false;
    int type = 0;
    float freq = 400, gain = 0, q = 0.8f, thr = -18, ratio = 2, atk = 25, rel = 180, knee = 6, maxgr = 6;
};
struct Pitch
{
    bool on = false;
    float thr = -18, range = 12, amt = 0.3f, atk = 40, rel = 200;
};
struct Settings
{
    std::array<Band, 4> bands;
    Pitch pitch;
};
struct Coeff
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double db(double f, double sr) const
    {
        auto z = std::polar(1.0, -2 * pi * f / sr);
        return 20 * std::log10(std::max(
                        1e-12, std::abs((b0 + b1 * z + b2 * z * z) / (1.0 + a1 * z + a2 * z * z))));
    }
};
// RBJ cookbook, shelf Q uses the same pole-Q convention as the bell.
inline Coeff coefficients(int type, double f, double q, double gain, double sr, bool detector = false)
{
    f = std::clamp(f, 20.0, sr * 0.45);
    q = std::clamp(q, 0.2, 10.0);
    const double w = 2 * pi * f / sr, c = std::cos(w), s = std::sin(w), alpha = s / (2 * q),
                 A = std::pow(10.0, gain / 40);
    double b0, b1, b2, a0, a1, a2;
    if (detector)
    {
        a0 = 1 + alpha;
        a1 = -2 * c;
        a2 = 1 - alpha;
        if (type == 1)
        {
            b0 = (1 - c) / 2;
            b1 = 1 - c;
            b2 = b0;
        }
        else if (type == 2)
        {
            b0 = (1 + c) / 2;
            b1 = -(1 + c);
            b2 = b0;
        }
        else
        {
            b0 = alpha;
            b1 = 0;
            b2 = -alpha;
        }
    }
    else if (type == 1)
    {
        double t = 2 * std::sqrt(A) * alpha;
        b0 = A * ((A + 1) - (A - 1) * c + t);
        b1 = 2 * A * ((A - 1) - (A + 1) * c);
        b2 = A * ((A + 1) - (A - 1) * c - t);
        a0 = (A + 1) + (A - 1) * c + t;
        a1 = -2 * ((A - 1) + (A + 1) * c);
        a2 = (A + 1) + (A - 1) * c - t;
    }
    else if (type == 2)
    {
        double t = 2 * std::sqrt(A) * alpha;
        b0 = A * ((A + 1) + (A - 1) * c + t);
        b1 = -2 * A * ((A - 1) + (A + 1) * c);
        b2 = A * ((A + 1) + (A - 1) * c - t);
        a0 = (A + 1) - (A - 1) * c + t;
        a1 = 2 * ((A - 1) - (A + 1) * c);
        a2 = (A + 1) - (A - 1) * c - t;
    }
    else
    {
        b0 = 1 + alpha * A;
        b1 = -2 * c;
        b2 = 1 - alpha * A;
        a0 = 1 + alpha / A;
        a1 = -2 * c;
        a2 = 1 - alpha / A;
    }
    return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}
struct Filter
{
    double z1 = 0, z2 = 0;
    void reset() { z1 = z2 = 0; }
    float run(float x, const Coeff &c)
    {
        x = finite(x);
        double y = c.b0 * x + z1;
        z1 = c.b1 * x - c.a1 * y + z2;
        z2 = c.b2 * x - c.a2 * y;
        if (!std::isfinite(y) || !std::isfinite(z1) || !std::isfinite(z2))
        {
            reset();
            return x;
        }
        return finite(static_cast<float>(y));
    }
};
struct Control
{
    std::array<float, 4> gr{};
    float pitch = 0;
};
struct View
{
    const Control *data = nullptr;
    int64_t end = 0;
    size_t size = 0;
    Control read(int64_t t) const
    {
        if (!data || size == 0 || t < 0 || t >= end || t < end - static_cast<int64_t>(size))
            return {};
        return data[static_cast<size_t>(t) % size];
    }
};
class Processor
{
  public:
    void prepare(double rate)
    {
        sr = std::isfinite(rate) && rate >= 8000 ? rate : 48000;
        ring.assign(static_cast<size_t>(sr * 0.15) + 4096, {});
        reset();
    }
    void reset()
    {
        time = 0;
        std::fill(ring.begin(), ring.end(), Control{});
        energy = {};
        gr = {};
        pitchEnv = pitchValue = 0;
        for (auto &b : det)
            for (auto &f : b)
                f.reset();
        resetOutput();
        initialized = false;
    }
    void resetOutput()
    {
        for (auto &b : audio)
            for (auto &f : b)
                f.reset();
        gain = {};
        outputCoefficients = {};
    }
    int64_t now() const { return time; }
    View view() const { return {ring.data(), time, ring.size()}; }
    std::array<float, 4> levels{}, applied{};
    std::array<Coeff, 4> outputCoefficients{};
    float level = -120, appliedPitch = 0;
    void set(Settings s)
    {
        for (auto &b : s.bands)
        {
            b.type = std::clamp(b.type, 0, 2);
            b.freq = limit(b.freq, 20, float(sr * .45), 400);
            b.q = limit(b.q, .2f, 10, .8f);
            b.gain = limit(b.gain, -18, 18);
            if (std::abs(b.gain) < 1e-5f)
                b.gain = 0;
            b.thr = limit(b.thr, -60, 0, -18);
            b.ratio = limit(b.ratio, 1, 10, 2);
            b.atk = limit(b.atk, 1, 200, 25);
            b.rel = limit(b.rel, 20, 1000, 180);
            b.knee = limit(b.knee, 0, 12, 6);
            b.maxgr = limit(b.maxgr, 0, 18, 6);
        }
        s.pitch.thr = limit(s.pitch.thr, -60, 0, -18);
        s.pitch.range = limit(s.pitch.range, 1, 30, 12);
        s.pitch.amt = limit(s.pitch.amt, -2, 2);
        if (std::abs(s.pitch.amt) < 1e-5f)
            s.pitch.amt = 0;
        s.pitch.atk = limit(s.pitch.atk, 5, 300, 40);
        s.pitch.rel = limit(s.pitch.rel, 20, 1000, 200);
        settings = s;
        if (!initialized)
        {
            for (int b = 0; b < 4; ++b)
            {
                freq[b] = s.bands[b].freq;
                qs[b] = s.bands[b].q;
                types[b] = s.bands[b].type;
            }
            initialized = true;
        }
        for (int b = 0; b < 4; ++b)
        {
            dc[b] = coefficients(s.bands[b].type, s.bands[b].freq, s.bands[b].q, 0, sr, true);
            attack[b] = coef(s.bands[b].atk);
            release[b] = coef(s.bands[b].rel);
        }
        rms = coef(10);
        glide = coef(20);
        pa = coef(s.pitch.atk);
        pr = coef(s.pitch.rel);
        off = coef(10);
    }
    static float reduction(float db, const Band &b)
    {
        float x = db - b.thr, k = b.knee, slope = 1 - 1 / b.ratio;
        float y = k > 0 && x > -k / 2 && x < k / 2 ? slope * (x + k / 2) * (x + k / 2) / (2 * k)
                                                   : slope * std::max(0.0f, x);
        return std::min(b.maxgr, y);
    }
    void detect(float *const *channels, int nc, int n)
    {
        for (int i = 0; i < n; ++i)
        {
            Control control;
            float full = 0;
            for (int ch = 0; ch < nc; ++ch)
            {
                float x = limit(channels[ch][i], -32, 32);
                full += x * x / nc;
            }
            energy[4] += rms * (full - energy[4]);
            level = 10 * std::log10(std::max(energy[4], 1e-12f));
            for (int b = 0; b < 4; ++b)
            {
                float power = 0;
                for (int ch = 0; ch < nc; ++ch)
                {
                    float y = det[b][ch].run(limit(channels[ch][i], -32, 32), dc[b]);
                    power += y * y / nc;
                }
                energy[b] += rms * (power - energy[b]);
                energy[b] = finite(energy[b]);
                levels[b] = 10 * std::log10(std::max(energy[b], 1e-12f));
                const auto &p = settings.bands[b];
                float target = p.on && p.dyn ? reduction(levels[b], p) : 0;
                gr[b] += (target > gr[b] ? attack[b] : release[b]) * (target - gr[b]);
                if (gr[b] < 1e-6f)
                    gr[b] = 0;
                control.gr[b] = gr[b];
            }
            const auto &p = settings.pitch;
            float x = p.on ? std::clamp((level - p.thr) / p.range, 0.0f, 1.0f) : 0;
            x = x * x * (3 - 2 * x);
            pitchEnv += (p.on ? (x > pitchEnv ? pa : pr) : off) * (x - pitchEnv);
            pitchValue += off * (pitchEnv * p.amt - pitchValue);
            if (std::abs(pitchValue) < 1e-7f)
                pitchValue = 0;
            control.pitch = pitchValue;
            ring[static_cast<size_t>(time++) % ring.size()] = control;
        }
    }
    void output(float *const *channels, int nc, int n, int64_t base, int delay)
    {
        auto v = view();
        for (int i = 0; i < n; ++i)
        {
            auto control = v.read(base + i - delay);
            appliedPitch = control.pitch;
            for (int b = 0; b < 4; ++b)
            {
                const auto &p = settings.bands[b];
                applied[b] = control.gr[b];
                freq[b] += glide * (p.freq - freq[b]);
                qs[b] += glide * (p.q - qs[b]);
                // Type changes fade through unity, then reset the old topology.
                float target = p.on && types[b] == p.type ? p.gain - control.gr[b] : 0;
                gain[b] += glide * (target - gain[b]);
                if (std::abs(gain[b] - target) < 1e-5f)
                    gain[b] = target;
                if (types[b] != p.type && std::abs(gain[b]) < 1e-4f)
                {
                    types[b] = p.type;
                    gain[b] = 0;
                    for (auto &f : audio[b])
                        f.reset();
                }
                if (gain[b] == 0)
                {
                    outputCoefficients[b] = {};
                    for (auto &f : audio[b])
                        f.reset();
                    continue;
                }
                auto c = coefficients(types[b], freq[b], qs[b], gain[b], sr);
                outputCoefficients[b] = c;
                for (int ch = 0; ch < nc; ++ch)
                    channels[ch][i] = audio[b][ch].run(channels[ch][i], c);
            }
            for (int ch = 0; ch < nc; ++ch)
                if (!std::isfinite(channels[ch][i]))
                    channels[ch][i] = 0;
        }
    }

  private:
    float coef(float ms) const { return float(1 - std::exp(-1 / (sr * ms * .001))); }
    double sr = 48000;
    int64_t time = 0;
    bool initialized = false;
    Settings settings;
    std::vector<Control> ring;
    std::array<std::array<Filter, 2>, 4> det{}, audio{};
    std::array<Coeff, 4> dc{};
    std::array<float, 5> energy{};
    std::array<float, 4> gr{}, gain{}, freq{}, qs{}, attack{}, release{};
    std::array<int, 4> types{};
    float rms = 0, glide = 0, pa = 0, pr = 0, off = 0, pitchEnv = 0, pitchValue = 0;
};
} // namespace vq
