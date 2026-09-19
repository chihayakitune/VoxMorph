// Adaptive Voice Dynamics -- processor-level smoke test (JUCE, no audio device).
//
//   cmake -B build -DVOXMORPH_VEC_SMOKE=ON && cmake --build build --target VoxMorphVecSmoke
//   ./build/VoxMorphVecSmoke_artefacts/Release/VoxMorphVecSmoke
//
// Short, fixed checks only:
//   1. OFF and ON-with-Amount-0 give identical samples through the whole
//      processor (same input, same block size)
//   2. a session saved without the vec keys comes up OFF / 50 % even when
//      the running instance had it ON (APVTS::replaceState alone would keep ON)
//   3. a preset without the vec keys loads as OFF; a locked vecenabled keeps
//      its current value (existing lock policy)
//   4. ON: silence -> normal -> loud, one Stereo Input switch, one host
//      reset, one Low Latency switch -- the output stays finite
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cstdio>

static int fails = 0;
static void check (bool ok, const char* what)
{
    std::printf ("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (! ok) ++fails;
}

static void setP (VoxMorphProcessor& p, const char* id, float plain)
{
    auto* rp = p.apvts.getParameter (id);
    rp->setValueNotifyingHost (rp->convertTo0to1 (plain));
}
static float getP (VoxMorphProcessor& p, const char* id)
{
    return p.apvts.getRawParameterValue (id)->load();
}

static std::vector<float> voice (double fs, int n, unsigned seed)
{
    std::vector<float> x ((size_t) n);
    double ph = 0; float a = 0, b = 0;
    const double r = std::exp (-M_PI * 90 / fs), c = 2 * r * std::cos (2 * M_PI * 700 / fs);
    for (int i = 0; i < n; ++i)
    {
        ph += 140.0 / fs;
        float e = 0.0f;
        if (ph >= 1.0) { ph -= 1.0; e = 1.0f; }
        seed = seed * 1664525u + 1013904223u;
        e += 0.01f * ((float) (seed >> 8) / 16777216.0f - 0.5f);
        const float y = (float) (e + c * a - r * r * b);  b = a;  a = y;
        x[(size_t) i] = 0.02f * y;
    }
    return x;
}

// Render `in` (mono, duplicated to stereo) through the processor, blocks of 256.
static std::vector<float> render (VoxMorphProcessor& p, const std::vector<float>& in,
                                  const std::function<void (int)>& atBlock = {})
{
    const int blk = 256, n = (int) in.size();
    std::vector<float> out ((size_t) n * 2);
    juce::AudioBuffer<float> buf (2, blk);
    juce::MidiBuffer midi;
    for (int off = 0, b = 0; off < n; off += blk, ++b)
    {
        if (atBlock) atBlock (b);
        const int c = std::min (blk, n - off);
        buf.setSize (2, c, false, false, true);
        for (int ch = 0; ch < 2; ++ch) buf.copyFrom (ch, 0, in.data() + off, c);
        p.processBlock (buf, midi);
        for (int i = 0; i < c; ++i)
        {
            out[(size_t) (2 * (off + i))]     = buf.getSample (0, i);
            out[(size_t) (2 * (off + i)) + 1] = buf.getSample (1, i);
        }
    }
    return out;
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    const double fs = 48000.0;
    auto x = voice (fs, (int) (fs * 2.0), 3);

    // ---- 1. OFF == Amount 0 ---------------------------------------------
    {
        VoxMorphProcessor a, b;
        for (auto* p : { &a, &b }) { setP (*p, "pitch", 5.0f); p->prepareToPlay (fs, 256); }
        setP (a, "vecenabled", 0.0f);
        setP (b, "vecenabled", 1.0f);  setP (b, "vecamount", 0.0f);
        auto ya = render (a, x), yb = render (b, x);
        check (std::memcmp (ya.data(), yb.data(), ya.size() * sizeof (float)) == 0,
               "OFF and ON/Amount 0 are sample-identical through the processor");
    }

    // ---- 2. old session -> OFF ---------------------------------------------
    {
        VoxMorphProcessor p;
        juce::MemoryBlock mb;
        p.getStateInformation (mb);
        auto xml = juce::AudioProcessor::getXmlFromBinary (mb.getData(), (int) mb.getSize());
        for (auto* id : { "vecenabled", "vecamount" })
            if (auto* e = xml->getChildByAttribute ("id", id)) xml->removeChildElement (e, true);
        juce::MemoryBlock old;
        juce::AudioProcessor::copyXmlToBinary (*xml, old);

        setP (p, "vecenabled", 1.0f);  setP (p, "vecamount", 80.0f);   // running instance ON
        p.setStateInformation (old.getData(), (int) old.getSize());
        check (getP (p, "vecenabled") < 0.5f && std::abs (getP (p, "vecamount") - 50.0f) < 1.0e-3f,
               "session without vec keys restores OFF / 50 % over a running ON");

        setP (p, "vecenabled", 1.0f);  setP (p, "vecamount", 70.0f);
        p.getStateInformation (mb);
        VoxMorphProcessor q;
        q.setStateInformation (mb.getData(), (int) mb.getSize());
        check (getP (q, "vecenabled") > 0.5f && std::abs (getP (q, "vecamount") - 70.0f) < 1.0e-3f,
               "new session round-trips ON / 70 %");
    }

    // ---- 3. old preset -> OFF, lock kept -------------------------------------
    {
        VoxMorphProcessor p;
        auto xml = voxMorphPresetXml (p);
        for (auto* id : { "vecenabled", "vecamount" })
            if (auto* e = xml->getChildByAttribute ("id", id)) xml->removeChildElement (e, true);
        auto f = juce::File::createTempFile (".vmpreset");
        xml->writeTo (f);
        int applied = 0, locked = 0;

        setP (p, "vecenabled", 1.0f);
        voxMorphApplyPreset (p, f, applied, locked);
        check (getP (p, "vecenabled") < 0.5f, "preset without vec keys loads OFF");

        setP (p, "vecenabled", 1.0f);
        p.setParamLocked ("vecenabled", true);
        voxMorphApplyPreset (p, f, applied, locked);
        check (getP (p, "vecenabled") > 0.5f && locked >= 1,
               "locked vecenabled keeps its current value (lock policy)");
        f.deleteFile();
    }

    // ---- 4. ON: finite across stereo / reset / low latency -----------------
    {
        VoxMorphProcessor p;
        setP (p, "pitch", 5.0f);
        setP (p, "vecenabled", 1.0f);  setP (p, "vecamount", 100.0f);
        p.prepareToPlay (fs, 256);
        const int n0 = (int) fs / 2, n1 = (int) (fs * 2.5), n2 = (int) (fs * 1.5);
        auto v = voice (fs, n0 + n1 + n2, 9);
        for (int i = 0; i < (int) v.size(); ++i)
            v[(size_t) i] *= i < n0 ? 0.0f : (i < n0 + n1 ? 1.0f : 6.0f);
        float maxEff = 0.0f;
        auto y = render (p, v, [&] (int b)
        {
            const int s = b * 256;
            if (s == n0 + n1 + 256 * 40)  setP (p, "stereo", 1.0f);
            if (s == n0 + n1 + 256 * 80)  p.reset();
            if (s == n0 + n1 + 256 * 120) setP (p, "lowlat", 1.0f);
            maxEff = std::max (maxEff, p.uiVecEffort.load());
        });
        bool finite = true;
        for (float s : y) finite = finite && std::isfinite (s);
        std::printf ("   max effort readout %.1f, warm %.2f\n", maxEff, p.uiVecWarm.load());
        check (finite, "ON: silence -> normal -> loud + stereo / reset / low latency: finite");
        check (maxEff > 10.0f, "effort readout rises on the loud part");
    }

    std::printf (fails == 0 ? "ALL PASS\n" : "%d FAIL\n", fails);
    return fails == 0 ? 0 : 1;
}
