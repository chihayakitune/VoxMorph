// Bounded DSP regression: clang++ -O2 -std=c++17 -I dsp test/voice_quality_test.cpp -o /tmp/vq-test
#define PSOLA_GRAIN_LOG
#include "PsolaEngine.h"
#include <cstdio>
#include <cstring>
#include <limits>
static int failures = 0;
static void check(bool ok, const char *name)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok)
        ++failures;
}
static std::vector<float> run(double sr, int block, bool active)
{
    vq::Processor p;
    p.prepare(sr);
    vq::Settings s;
    s.bands[0].freq = 400;
    s.bands[0].gain = active ? 0.f : 8.55e-7f;
    s.bands[0].dyn = active;
    s.bands[0].thr = -30;
    s.pitch.on = active;
    s.pitch.thr = -30;
    p.set(s);
    int count = (int)(sr * .3);
    std::vector<float> result;
    result.reserve(count * 4);
    for (int pos = 0; pos < count;)
    {
        int n = std::min({block, 512, count - pos});
        std::vector<float> l(n), r(n), original(n);
        for (int i = 0; i < n; ++i)
        {
            l[i] = float(.4 * std::sin(2 * vq::pi * 400 * (pos + i) / sr));
            r[i] = l[i] * .25f;
            original[i] = l[i];
        }
        float *ch[] = {l.data(), r.data()};
        auto base = p.now();
        p.detect(ch, 2, n);
        auto v = p.view();
        p.output(ch, 2, n, base, 128);
        for (int i = 0; i < n; ++i)
        {
            auto c = v.read(base + i);
            result.insert(result.end(), {c.gr[0], c.pitch, l[i], r[i]});
            if (!active && l[i] != original[i])
                ++failures;
        }
        pos += n;
    }
    return result;
}
int main()
{
    vq::Band b;
    b.knee = 0;
    b.thr = -18;
    b.ratio = 2;
    check(vq::Processor::reduction(-24, b) == 0 && vq::Processor::reduction(-12, b) == 3,
          "hard knee / ratio / below threshold");
    b.knee = 6;
    check(std::abs(vq::Processor::reduction(-18, b) - .375f) < 1e-6, "soft knee");
    for (int type = 0; type < 3; ++type)
    {
        auto c = vq::coefficients(type, 1000, .707, 6, 48000);
        double f = type == 1 ? 1 : type == 2 ? 23999 : 1000;
        check(std::abs(c.db(f, 48000) - 6) < .01, "bell/shelf response +6 dB");
    }
    for (double sr : {44100., 48000., 88200., 96000.})
    {
        auto ref = run(sr, 32, true);
        bool same = true, linked = true;
        for (int block : {64, 128, 256, 512, 1024, 8192})
            same = same && ref == run(sr, block, true);
        for (size_t i = 0; i < ref.size(); i += 4)
            linked = linked && std::abs(ref[i + 3] - .25f * ref[i + 2]) < 1e-6;
        check(same, "sample-based detector / EQ invariant to segment size");
        check(linked, "stereo linked GR preserves scaled L/R");
        run(sr, 128, false);
    }
    check(failures == 0, "flat EQ exact bypass");
    for (bool low : {false, true})
    {
        const double sr = 48000;
        vq::Processor control;
        control.prepare(sr);
        vq::Settings settings;
        control.set(settings);
        PsolaEngine zero, plain;
        zero.prepare(sr);
        plain.prepare(sr);
        PsolaEngine::Params params;
        params.pitchSemi = 5;
        params.lowLatency = low;
        zero.setParams(params);
        plain.setParams(params);
        bool exact = true;
        float in[128], a[128], b[128];
        for (int pos = 0; pos < 24000; pos += 128)
        {
            for (int i = 0; i < 128; ++i)
                in[i] = float(.2 * std::sin(2 * vq::pi * 140 * (pos + i) / sr));
            float *ch[] = {in};
            auto base = control.now();
            control.detect(ch, 1, 128);
            auto view = control.view();
            zero.process(in, a, 128, &view, base);
            plain.process(in, b, 128);
            exact = exact && std::memcmp(a, b, sizeof(a)) == 0;
        }
        check(exact, "zero control view equals unchanged engine (Normal/Low Latency)");
        // Actual source-grain timestamp must select the offset after intonation.
        std::vector<vq::Control> ring(48000);
        for (size_t i = 12000; i < ring.size(); ++i)
            ring[i].pitch = .5f;
        vq::View view{ring.data(), 48000, ring.size()};
        PsolaEngine e;
        e.prepare(sr);
        params.pitchSemi = 0;
        params.pitchRange = 1.5f;
        params.pitchCenterHz = 150;
        e.setParams(params);
        for (int pos = 0; pos < 32000; pos += 128)
        {
            for (int i = 0; i < 128; ++i)
                in[i] = float(.3 * std::sin(2 * vq::pi * 140 * (pos + i) / sr));
            e.process(in, a, 128, &view, pos);
        }
        bool aligned = true;
        int checked = 0;
        for (const auto &g : e.grainLog)
            if (g.voiced)
            {
                double hz = 150 * std::pow((sr / g.P) / 150, 1.5);
                float semitone = view.read((int64_t)std::llround(g.inMark)).pitch;
                hz *= std::pow(2., semitone / 12.);
                double ts = sr / std::clamp(hz, 40., 1000.);
                aligned = aligned && std::abs(ts - g.Ts) < .001;
                ++checked;
            }
        check(aligned && checked > 20, "Dynamic Pitch reads source c after intonation, both latencies");
        PsolaEngine ra, rb;
        ra.prepare(sr);
        rb.prepare(sr);
        params.robotize = true;
        ra.setParams(params);
        rb.setParams(params);
        exact = true;
        for (int pos = 0; pos < 16000; pos += 128)
        {
            for (int i = 0; i < 128; ++i)
                in[i] = float(.3 * std::sin(2 * vq::pi * 140 * (pos + i) / sr));
            ra.process(in, a, 128, &view, pos);
            rb.process(in, b, 128);
            exact = exact && std::memcmp(a, b, sizeof(a)) == 0;
        }
        check(exact, "Robotize ignores Dynamic Pitch");
    }
    // A detector burst must alter the converted signal only D samples later.
    for (int delay : {1024, 2048})
    {
        vq::Processor eq;
        eq.prepare(48000);
        vq::Settings cfg;
        cfg.bands[0].dyn = true;
        cfg.bands[0].thr = -40;
        cfg.bands[0].knee = 0;
        cfg.bands[0].atk = 1;
        eq.set(cfg);
        int onset = -1, outputOnset = -1;
        bool synced = true;
        for (int t = 0; t < delay + 2048; ++t)
        {
            float detector = t < 512 ? 0.0f : float(.5 * std::sin(2 * vq::pi * 400 * t / 48000));
            float *in[] = {&detector};
            eq.detect(in, 1, 1);
            auto view = eq.view();
            if (onset < 0 && view.read(t).gr[0] > 0)
                onset = t;
            float converted = 1;
            float *out[] = {&converted};
            eq.output(out, 1, 1, t, delay);
            if (outputOnset < 0 && converted != 1)
                outputOnset = t;
            synced = synced && eq.applied[0] == view.read(t - delay).gr[0];
        }
        std::printf("EQ delay %d input onset %d output onset %d synced %d\n", delay, onset, outputOnset,
                    (int)synced);
        check(synced && onset >= 512 && outputOnset >= onset + delay && outputOnset <= onset + delay + 1,
              "Dynamic EQ control is sample-exact; float output onset within one sample");
    }
    float firstPitch = -1;
    bool sameTime = true;
    for (double sr : {44100., 48000., 88200., 96000.})
    {
        vq::Processor pitch;
        pitch.prepare(sr);
        vq::Settings cfg;
        cfg.pitch.on = true;
        pitch.set(cfg);
        for (int i = 0; i < int(sr * .3); ++i)
        {
            float x = .5f;
            float *ch[] = {&x};
            pitch.detect(ch, 1, 1);
        }
        float end = pitch.view().read(pitch.now() - 1).pitch;
        if (firstPitch < 0)
            firstPitch = end;
        else
            sameTime = sameTime && std::abs(firstPitch - end) < .0001;
    }
    check(sameTime, "Pitch time constants agree in seconds across 44.1/48/88.2/96 kHz");
    vq::Processor p;
    p.prepare(48000);
    vq::Settings s;
    s.bands[0].gain = 6;
    s.bands[0].dyn = true;
    s.pitch.on = true;
    p.set(s);
    float x[512] = {};
    x[0] = std::numeric_limits<float>::quiet_NaN();
    x[1] = std::numeric_limits<float>::infinity();
    float *ch[] = {x};
    p.detect(ch, 1, 512);
    p.output(ch, 1, 512, 0, 0);
    bool finite = true;
    for (float y : x)
        finite = finite && std::isfinite(y);
    check(finite, "NaN/Inf quarantined");
    return failures ? 1 : 0;
}
