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

static void shoot (juce::AudioProcessorEditor& ed, const juce::File& out)
{
    juce::Image img (juce::Image::ARGB, ed.getWidth(), ed.getHeight(), true);
    { juce::Graphics g (img); ed.paintEntireComponent (g, true); }
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
        check (findButton (page, "SAVE PRESET")   != nullptr, "SAVE PRESET exists");
        check (countButtons (page, "Play") == 0,            "no stray Play button");
        check (countButtons (page, "Save Profile...") == 0, "no Save Profile buttons");
        check (countButtons (page, "Reset All to Defaults") == 0,
               "Reset All is gone from MATCHING (the PRESETS tab keeps its own)");
        check (findButton (page, "TargetFile")  != nullptr, "TargetFile tile exists");
        check (findButton (page, "MyVoiceFile") != nullptr, "MyVoiceFile tile exists");

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

        const int over = auditOverflow (ed.get(), tag);
        std::printf ("  children outside their parent: %d\n", over);
        check (over == 0, "no component escapes its parent");

        shoot (*ed, outDir.getChildFile ("matching_" + tag + ".png"));
    }

    ed->removeFromDesktop();
    std::printf ("\n%s (%d failure%s)\n", g_fail == 0 ? "ALL PASS" : "FAILURES",
                 g_fail, g_fail == 1 ? "" : "s");
    return g_fail == 0 ? 0 : 1;
}
