// ui_shot.cpp — offscreen render + layout audit for the editor (v0.39.0).
//
// Build:  cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DVOXMORPH_UI_HARNESS=ON
//         cmake --build build --target VoxMorphUiShot
//
// Why this exists, and the trap it is built around: an editor rendered
// WITHOUT a desktop peer reports isShowing() == false, and everything that
// gates on that (SpectrumData, meters, level-driven art) then does nothing
// while still drawing something plausible. A v0.36.9 change was "verified"
// that way and the description of what it did turned out to be wrong. So the
// harness calls addToDesktop() and prints the gates it can observe -- if
// those read 0 the screenshot is not evidence about anything they drive.
//
// It also audits the layout numerically rather than by eye: children that
// escape their parent, and buttons that should or should not exist.
#include "../src/PluginProcessor.h"
#include "../src/PluginEditor.h"
#include <juce_gui_basics/juce_gui_basics.h>

static int g_fail = 0;
static void check (bool ok, const juce::String& what)
{
    if (! ok) { std::fprintf (stderr, "FAIL: %s\n", what.toRawUTF8()); ++g_fail; }
}

static void walk (juce::Component* c, const std::function<void (juce::Component*)>& fn)
{
    if (c == nullptr) return;
    fn (c);
    for (int i = 0; i < c->getNumChildComponents(); ++i) walk (c->getChildComponent (i), fn);
}

static juce::Button* findButton (juce::Component* root, const juce::String& text)
{
    juce::Button* hit = nullptr;
    walk (root, [&] (juce::Component* c)
    {
        if (hit != nullptr) return;
        if (auto* b = dynamic_cast<juce::Button*> (c))
            if (b->getButtonText() == text) hit = b;
    });
    return hit;
}

static int countButtons (juce::Component* root, const juce::String& text)
{
    int n = 0;
    walk (root, [&] (juce::Component* c)
    {
        if (auto* b = dynamic_cast<juce::Button*> (c))
            if (b->getButtonText() == text) ++n;
    });
    return n;
}

// A child sticking out of its parent is the failure mode that adding tiles
// causes, and it is invisible in a screenshot when the parent clips.
//
// Scrolled content is exempt: a Viewport's viewed component is SUPPOSED to be
// taller than the Viewport -- that is the entire point of one -- and the MAIN
// page is built that way, so without this the audit reports a false positive
// at small window heights and stops being worth reading.
static bool isScrolledContent (juce::Component* c)
{
    for (auto* p = c->getParentComponent(); p != nullptr; p = p->getParentComponent())
        if (auto* vp = dynamic_cast<juce::Viewport*> (p))
            return vp->getViewedComponent() == c
                || (c->getParentComponent() != nullptr
                    && vp->getViewedComponent() == c->getParentComponent());
    return false;
}

static int auditOverflow (juce::Component* root, const juce::String& label)
{
    int bad = 0;
    walk (root, [&] (juce::Component* c)
    {
        auto* p = c->getParentComponent();
        if (p == nullptr || ! c->isVisible() || c->getBounds().isEmpty()) return;
        if (isScrolledContent (c)) return;
        if (! p->getLocalBounds().contains (c->getBounds()))
        {
            std::fprintf (stderr, "  overflow in %s: child %s %s outside parent %s\n",
                          label.toRawUTF8(),
                          c->getName().isEmpty() ? "(unnamed)" : c->getName().toRawUTF8(),
                          c->getBounds().toString().toRawUTF8(),
                          p->getLocalBounds().toString().toRawUTF8());
            ++bad;
        }
    });
    return bad;
}

static juce::Image render (juce::AudioProcessorEditor& ed)
{
    juce::Image img (juce::Image::ARGB, ed.getWidth(), ed.getHeight(), true);
    { juce::Graphics g (img); ed.paintEntireComponent (g, true); }
    return img;
}

static void shoot (juce::AudioProcessorEditor& ed, const juce::File& out)
{
    const auto img = render (ed);
    juce::PNGImageFormat png;
    if (auto os = std::unique_ptr<juce::FileOutputStream> (out.createOutputStream()))
    { os->setPosition (0); os->truncate(); png.writeImageToStream (img, *os); }
    std::printf ("  wrote %s (%d x %d)\n", out.getFullPathName().toRawUTF8(),
                 ed.getWidth(), ed.getHeight());
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    const juce::File outDir = argc > 1 ? juce::File (juce::String (argv[1]))
                                       : juce::File::getCurrentWorkingDirectory();
    outDir.createDirectory();

    VoxMorphProcessor proc;
    proc.prepareToPlay (48000.0, 512);

    std::unique_ptr<juce::AudioProcessorEditor> ed (proc.createEditor());
    check (ed != nullptr, "editor created");
    if (ed == nullptr) return 1;

    // REAL PEER. Without this isShowing() is false everywhere and the
    // screenshot silently stops being evidence -- see the header note.
    ed->addToDesktop (juce::ComponentPeer::windowIsTemporary);
    ed->setVisible (true);

    for (auto size : { juce::Point<int> (1180, 920),      // minimum allowed
                       juce::Point<int> (1400, 1080) })   // default
    {
        ed->setSize (size.x, size.y);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (250);

        if (auto* b = findButton (ed.get(), "Matching")) b->triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (250);

        const juce::String tag (juce::String (size.x) + "x" + juce::String (size.y));
        std::printf ("\n== %s ==\n", tag.toRawUTF8());

        // ---- the gates that decide whether analysis is even running ----
        std::printf ("  isShowing=%d  uiWantsViz=%d  uiWantsMeters=%d\n",
                     (int) ed->isShowing(), (int) proc.uiWantsViz.load(),
                     (int) proc.uiWantsMeters.load());
        check (ed->isShowing(), "editor reports isShowing() with a peer attached");

        // ---- v0.39.0 audit: what must and must not be on the page ----
        // Scoped to the MATCHING page, reached via its MATCH button. Searching
        // the whole editor would also see the PRESETS tab, which keeps its own
        // "Reset All to Defaults" button on purpose -- only the Matching copy
        // was folded into the header dropdown.
        auto* matchBtn = findButton (ed.get(), "MATCH");
        check (matchBtn != nullptr, "MATCH exists");
        juce::Component* page = matchBtn != nullptr ? matchBtn->getParentComponent() : nullptr;
        check (page != nullptr, "found the Matching page");
        if (page == nullptr) return 1;

        check (findButton (page, "NEW CHARACTER") != nullptr, "NEW CHARACTER exists");
        check (countButtons (page, "SAVE PRESET") == 0,
               "SAVE PRESET is gone (the header dropdown saves presets)");
        check (countButtons (page, "Play") == 0,            "no stray Play button");
        check (countButtons (page, "Save Profile...") == 0, "no Save Profile buttons");
        check (countButtons (page, "Reset All to Defaults") == 0,
               "Reset All is gone from MATCHING (the PRESETS tab keeps its own)");
        check (findButton (page, "TargetFile")  != nullptr, "TargetFile tile exists");
        check (findButton (page, "MyVoiceFile") != nullptr, "MyVoiceFile tile exists");
        check (findButton (page, "RECORD")     != nullptr, "RECORD button exists");
        // the per-profile readouts were removed in v0.41.0
        int readouts = 0;
        walk (page, [&] (juce::Component* c)
        {
            if (auto* l = dynamic_cast<juce::Label*> (c))
                if (l->getText().startsWith ("Target:") || l->getText().startsWith ("MyVoice:"))
                    ++readouts;
        });
        check (readouts == 0, "no Target:/MyVoice: profile readout labels");

        // NEW CHARACTER must start disabled: no MyVoice has been measured.
        if (auto* nc = findButton (page, "NEW CHARACTER"))
            check (! nc->isEnabled(), "NEW CHARACTER disabled without MyVoice");

        // ---- character tiles: count, size and that the row fits ----
        int nCat = 0;
        const auto* cat = getSampleTargets (nCat);
        juce::Rectangle<int> row;
        int tiles = 0, tileW = 0;
        for (int i = 0; i < nCat; ++i)
            if (auto* b = findButton (page, juce::String (cat[i].displayEn)))
            { row = row.getUnion (b->getBounds()); tileW = b->getWidth(); ++tiles; }
        if (auto* tf = findButton (page, "TargetFile"))
        { row = row.getUnion (tf->getBounds()); ++tiles; }
        std::printf ("  catalog=%d tiles=%d tile=%dpx row=%s\n",
                     nCat, tiles, tileW, row.toString().toRawUTF8());
        check (tiles == nCat + 1, "every catalog entry plus TargetFile has a tile");
        check (tileW >= 72, "tiles did not shrink below the 72 px floor");

        // ---- Reset All now lives in the header preset dropdown ----
        // Checked as BEHAVIOUR, not just presence: move a parameter off its
        // default, pick the item, and confirm the parameter came back AND the
        // dropdown did not stay showing the action as if it were a preset.
        {
            juce::ComboBox* presetBox = nullptr;
            walk (ed.get(), [&] (juce::Component* c)
            {
                if (presetBox != nullptr) return;
                if (auto* cb = dynamic_cast<juce::ComboBox*> (c))
                    for (int i = 0; i < cb->getNumItems(); ++i)
                        if (cb->getItemText (i).startsWith ("Reset All to Defaults"))
                        { presetBox = cb; return; }
            });
            check (presetBox != nullptr, "preset dropdown carries a Reset All item");
            if (presetBox != nullptr)
            {
                auto* pitch = proc.apvts.getParameter ("pitch");
                check (pitch != nullptr, "pitch parameter found");
                if (pitch != nullptr)
                {
                    const int idBefore = presetBox->getSelectedId();
                    pitch->beginChangeGesture();
                    pitch->setValueNotifyingHost (0.75f);
                    pitch->endChangeGesture();
                    check (std::abs (pitch->getValue() - pitch->getDefaultValue()) > 1.0e-4f,
                           "pitch actually moved off its default first");

                    presetBox->setSelectedId (90001);     // the Reset All item
                    juce::MessageManager::getInstance()->runDispatchLoopUntil (100);

                    check (std::abs (pitch->getValue() - pitch->getDefaultValue()) < 1.0e-4f,
                           "Reset All from the dropdown restored the default");
                    check (presetBox->getSelectedId() == idBefore,
                           "dropdown selection restored (the action is not left showing)");
                }
            }
        }

        // ---- full-bleed check, on the PIXELS ----
        // The strip is painted, not a component, so its bounds cannot be
        // queried -- and bounds would not prove it anyway, since a component
        // cannot paint outside itself and would simply be clipped. Sampling
        // the rendered image at both window edges is the only thing that
        // actually answers "does the dark band reach the edge".
        {
            juce::Rectangle<int> cards;
            for (int i = 0; i < nCat; ++i)
                if (auto* b = findButton (page, juce::String (cat[i].displayEn)))
                    cards = cards.getUnion (b->getBounds());
            check (! cards.isEmpty(), "found the character cards");
            if (! cards.isEmpty())
            {
                const auto img = render (*ed);
                const int y = page->getY() + cards.getCentreY();
                const auto l = img.getPixelAt (0, y);
                const auto rr = img.getPixelAt (img.getWidth() - 1, y);
                std::printf ("  strip edge pixels at y=%d: left %s  right %s\n", y,
                             l.toDisplayString (false).toRawUTF8(),
                             rr.toDisplayString (false).toRawUTF8());
                // band is 0xff262b45 (brightness ~0.17); the page is
                // 0xfff3f5fb (~0.96). Anything light means a white margin.
                check (l.getBrightness()  < 0.35f, "strip reaches the LEFT window edge");
                check (rr.getBrightness() < 0.35f, "strip reaches the RIGHT window edge");
            }
        }

        const int over = auditOverflow (ed.get(), tag);
        std::printf ("  children outside their parent: %d\n", over);
        check (over == 0, "no component escapes its parent");

        shoot (*ed, outDir.getChildFile ("matching_" + tag + ".png"));
    }

    // ---- .vmprofile round-trip, including the v0.40.0 texture fields ----
    // profileToXml / profileFromXml live in PluginEditor.h and need JUCE, so
    // offline_test cannot reach them; this is the only place they get tested.
    {
        std::printf ("\n== .vmprofile round-trip ==\n");
        VoiceProfile a{};
        a.f0Hz = 291.9f; a.f0SpreadSt = 4.08f; a.tiltDb = 20.45f; a.voicedFrames = 3018;
        a.F[0]=879.9f; a.F[1]=2027.4f; a.F[2]=3875.6f;
        a.L[0]=0.0f;   a.L[1]=-13.31f; a.L[2]=-21.38f;
        a.rel[0]=0.25f; a.rel[1]=1.0f; a.rel[2]=0.66f;
        a.hnr[0]=22.18f; a.hnr[1]=9.54f; a.hnr[2]=4.06f;
        a.hfDb = -28.68f; a.tractScale = 1.21f;
        a.vow[0].frames = 1539; a.vow[0].f0Hz = 281.0f;
        a.vow[0].F[0]=934.1f; a.vow[0].F[1]=1887.6f; a.vow[0].F[2]=3835.9f;
        a.vow[0].rel[0]=0.54f; a.vow[0].rel[1]=1.0f; a.vow[0].rel[2]=0.60f;

        auto xml = profileToXml (a);
        VoiceProfile b{};
        check (xml != nullptr && profileFromXml (*xml, b), "profile parses back");
        auto near = [] (float x, float y) { return std::abs (x - y) < 0.01f; };
        check (near (a.f0Hz, b.f0Hz) && near (a.tiltDb, b.tiltDb), "scalars survive");
        for (int i = 0; i < 3; ++i)
        {
            check (near (a.F[i],   b.F[i]),   "F"   + juce::String (i+1) + " survives");
            check (near (a.rel[i], b.rel[i]), "rel" + juce::String (i+1) + " survives");
            check (near (a.hnr[i], b.hnr[i]), "hnr" + juce::String (i+1) + " survives");
        }
        check (near (a.hfDb, b.hfDb),             "hfDb survives");
        check (near (a.tractScale, b.tractScale), "tractScale survives");
        check (b.vow[0].frames == a.vow[0].frames
               && near (b.vow[0].F[1], a.vow[0].F[1]), "vowel table survives");
        std::printf ("  hnr %.2f/%.2f/%.2f  hfDb %.2f  tract %.3f\n",
                     b.hnr[0], b.hnr[1], b.hnr[2], b.hfDb, b.tractScale);

        // the point of persisting them: a saved profile can now drive Air
        VoiceProfile me{};
        me.f0Hz = 120.0f; me.voicedFrames = 300;
        me.F[0]=650; me.F[1]=1450; me.F[2]=2600;
        me.rel[0]=me.rel[1]=me.rel[2]=1.0f;
        me.hnr[0]=6.0f; me.hnr[1]=1.2f; me.hnr[2]=0.8f;
        const auto prop = MatchingEngine::autoSet (me, b);
        check (prop.airApplied && prop.air > 0.10f,
               "a round-tripped profile drives Air (this is what NEW CHARACTER needs)");
        std::printf ("  air from the reloaded profile: %.2f\n", prop.air);

        // a profile written before v0.40.0 has no texture and must stay silent
        auto old = profileToXml (a);
        for (int i = 0; i < 3; ++i) old->removeAttribute ("h" + juce::String (i + 1));
        old->removeAttribute ("hf");
        VoiceProfile c{};
        check (profileFromXml (*old, c), "pre-v0.40.0 profile still parses");
        check (c.hnr[0] == 0.0f && c.hnr[1] == 0.0f && c.hnr[2] == 0.0f,
               "missing texture stays 0 rather than being invented");
        check (! MatchingEngine::autoSet (me, c).airApplied,
               "and therefore writes no Air");
    }

    ed->removeFromDesktop();
    std::printf ("\n%s (%d failure%s)\n", g_fail == 0 ? "ALL PASS" : "FAILURES",
                 g_fail, g_fail == 1 ? "" : "s");
    return g_fail == 0 ? 0 : 1;
}
