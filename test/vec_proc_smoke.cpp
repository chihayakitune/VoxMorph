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
        // A writable path we choose, and every step asserted: a fixture that
        // silently failed to write would make the two checks below pass
        // against a preset that was never applied at all.
        auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                       .getChildFile ("VoxMorphVecSmoke");
        dir.createDirectory();
        auto f = dir.getChildFile ("old_preset.vmpreset");
        f.deleteFile();
        check (xml->writeTo (f) && f.existsAsFile() && f.getSize() > 0,
               "preset fixture: XML written to a writable test path");
        auto reparsed = juce::XmlDocument::parse (f);
        check (reparsed != nullptr && reparsed->getChildByAttribute ("id", "pitch") != nullptr,
               "preset fixture: the file parses back as a preset");
        check (reparsed != nullptr && reparsed->getChildByAttribute ("id", "vecenabled") == nullptr,
               "preset fixture: it really has no vec keys");

        int applied = 0, locked = 0;
        setP (p, "vecenabled", 1.0f);
        check (voxMorphApplyPreset (p, f, applied, locked) && applied > 0,
               "preset fixture: apply succeeded and changed parameters");
        check (getP (p, "vecenabled") < 0.5f, "preset without vec keys loads OFF");
        check (std::abs (getP (p, "vecamount") - 50.0f) < 1.0e-3f,
               "preset without vec keys loads Amount 50 %");

        setP (p, "vecenabled", 1.0f);
        p.setParamLocked ("vecenabled", true);
        applied = 0; locked = 0;
        check (voxMorphApplyPreset (p, f, applied, locked),
               "preset fixture: second apply succeeded");
        check (getP (p, "vecenabled") > 0.5f && locked >= 1,
               "locked vecenabled keeps its current value (lock policy)");
        p.setParamLocked ("vecenabled", false);
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
        bool didStereo = false, didReset = false, didLowLat = false;
        auto y = render (p, v, [&] (int b)
        {
            const int s = b * 256;
            // Fire once, on the first block at or past each point. The old
            // form compared s (always a multiple of 256) for EQUALITY with
            // n0 + n1 + k*256, and n0 + n1 = 144000 is not a multiple of 256 --
            // so none of these three ever happened and the check below was
            // asserting nothing about stereo, reset or Low Latency at all.
            if (! didStereo && s >= n0 + n1 + 256 * 40)
                { setP (p, "stereo", 1.0f);  didStereo = true; }
            if (! didReset && s >= n0 + n1 + 256 * 80)
                { p.reset();                 didReset  = true; }
            if (! didLowLat && s >= n0 + n1 + 256 * 120)
                { setP (p, "lowlat", 1.0f);  didLowLat = true; }
            maxEff = std::max (maxEff, p.uiVecEffort.load());
        });
        bool finite = true;
        for (float s : y) finite = finite && std::isfinite (s);
        std::printf ("   max effort readout %.1f, warm %.2f\n", maxEff, p.uiVecWarm.load());
        check (finite, "ON: silence -> normal -> loud + stereo / reset / low latency: finite");
        check (didStereo, "the Stereo Input switch actually fired");
        check (didReset,  "the host reset actually fired");
        check (didLowLat, "the Low Latency switch actually fired");
        check (maxEff > 10.0f, "effort readout rises on the loud part");
    }

    // ---- 5. session restore breaks the VEC time line ------------------------
    // Two instances warmed up identically on a loud passage, so both carry a
    // non-zero correction and a filter tail. One then has a session restored
    // (same parameter values, so nothing else can differ). Its first samples
    // must differ from the one that kept running: the restored instance starts
    // from neutral control instead of replaying the previous voice's history.
    {
        auto warm = voice (fs, (int) (fs * 3.0), 11);
        for (int i = 0; i < (int) warm.size(); ++i)
            warm[(size_t) i] *= i < (int) (fs * 2.0) ? 1.0f : 6.0f;   // normal, then loud
        auto probe = voice (fs, (int) (fs * 0.25), 12);

        VoxMorphProcessor a, b;
        juce::MemoryBlock state;
        for (auto* p : { &a, &b })
        {
            setP (*p, "pitch", 5.0f);
            setP (*p, "vecenabled", 1.0f);  setP (*p, "vecamount", 100.0f);
            p->prepareToPlay (fs, 256);
            render (*p, warm);
        }
        a.getStateInformation (state);                       // same values as b
        a.setStateInformation (state.getData(), (int) state.getSize());
        auto ya = render (a, probe), yb = render (b, probe);
        check (std::memcmp (ya.data(), yb.data(), ya.size() * sizeof (float)) != 0,
               "session restore resets the VEC control/filter state (differs from a running instance)");
    }

    // ---- 6. Amount 0 : analysis runs, audio is untouched --------------------
    // The point of separating Enable from Amount. Enable ON / Amount 0 has to
    // be sample-identical to OFF while still learning the baseline, so the UI
    // can show warmup progress before the user has chosen an amount.
    {
        const int n = (int) (fs * 4.0);
        auto v = voice (fs, n, 9);

        VoxMorphProcessor off, zero;
        for (auto* p : { &off, &zero }) setP (*p, "pitch", 5.0f);
        setP (zero, "vecenabled", 1.0f);  setP (zero, "vecamount", 0.0f);
        setP (off,  "vecenabled", 0.0f);
        for (auto* p : { &off, &zero }) p->prepareToPlay (fs, 256);

        const auto ya = render (off,  v);
        const auto yb = render (zero, v);
        check (ya.size() == yb.size()
               && std::memcmp (ya.data(), yb.data(), ya.size() * sizeof (float)) == 0,
               "Enable ON / Amount 0 is sample-identical to OFF");
        std::printf ("   Amount 0: warm %.2f, running %d\n",
                     zero.uiVecWarm.load(), (int) zero.uiVecRunning.load());
        check (zero.uiVecWarm.load() >= 1.0f,
               "Enable ON / Amount 0 still completes baseline warmup");
        check (off.uiVecWarm.load() <= 0.0f,
               "OFF does not analyse at all");
    }

    // ---- 7. a full reset returns the estimator to warming up ----------------
    {
        const int n = (int) (fs * 4.0);
        auto v = voice (fs, n, 9);

        VoxMorphProcessor p;
        setP (p, "pitch", 5.0f);
        setP (p, "vecenabled", 1.0f);  setP (p, "vecamount", 60.0f);
        p.prepareToPlay (fs, 256);
        render (p, v);
        const float warmBefore = p.uiVecWarm.load();

        // host reset -> serviced on the next block
        p.reset();
        auto one = std::vector<float> (v.begin(), v.begin() + 256 * 4);
        render (p, one);
        const float warmAfter = p.uiVecWarm.load();
        std::printf ("   warm before reset %.2f, just after %.2f\n", warmBefore, warmAfter);
        check (warmBefore >= 1.0f, "warmed up before the reset");
        check (warmAfter < 1.0f, "host reset sends the baseline back to warming up");

        // and it warms up again from fresh speech
        render (p, v);
        check (p.uiVecWarm.load() >= 1.0f, "it re-learns from the next normal speech");
    }

    std::printf (fails == 0 ? "ALL PASS\n" : "%d FAIL\n", fails);
    return fails == 0 ? 0 : 1;
}
