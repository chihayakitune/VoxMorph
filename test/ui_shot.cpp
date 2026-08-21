// ui_shot.cpp — offscreen render + layout audit for the editor (v0.48.0).
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
// escape their parent, buttons that should or should not exist, and -- for views
// driven by audio -- what the rendered pixels actually say once signal has
// been pushed through processBlock.
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

static juce::Label* findLabelStartingWith (juce::Component* root, const juce::String& t)
{
    juce::Label* hit = nullptr;
    walk (root, [&] (juce::Component* c)
    {
        if (hit != nullptr) return;
        if (auto* l = dynamic_cast<juce::Label*> (c))
            if (l->getText().startsWith (t)) hit = l;
    });
    return hit;
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

        // ---- v0.43.0: Formant Definition ----
        // The row is what the listening test is run from, so check that the
        // parameter exists with the intended range and that a control is
        // actually bound to it -- an APVTS entry with no slider would still
        // save, automate and reset perfectly while being unreachable.
        {
            auto* rp = proc.apvts.getParameter ("resonance");
            check (rp != nullptr, "resonance parameter exists");
            if (rp != nullptr)
            {
                const auto& rng = dynamic_cast<juce::RangedAudioParameter*> (rp)->getNormalisableRange();
                check (std::abs (rng.start + 100.0f) < 1.0e-4f
                    && std::abs (rng.end  - 100.0f) < 1.0e-4f, "resonance range is -100..+100");
                check (std::abs (rp->getDefaultValue() - 0.5f) < 1.0e-4f,
                       "resonance defaults to 0 % (centre of a signed range)");
                // rows are identified by their label text: the sliders do not
                // carry the parameter id, and the label is what the user reads.
                // ParamRow splits a trailing "(unit)" off into its own field,
                // so the label reads "Formant Definition" and the % shows up
                // beside the value.
                int rows = 0, sliders = 0;
                juce::Slider* defSlider = nullptr;
                walk (ed.get(), [&] (juce::Component* c)
                {
                    if (auto* l = dynamic_cast<juce::Label*> (c))
                        if (l->getText() == "Formant Definition") ++rows;
                    if (auto* s = dynamic_cast<juce::Slider*> (c))
                        if (std::abs (s->getMinimum() + 100.0) < 1.0e-6
                         && std::abs (s->getMaximum() - 100.0) < 1.0e-6) { ++sliders; defSlider = s; }
                });
                std::printf ("  Formant Definition rows=%d  (-100..100 sliders=%d)\n",
                             rows, sliders);
                check (rows >= 1, "Formant Definition row is present in the editor");
                check (sliders >= 1, "a -100..+100 slider exists for it");

                // v0.44.0: Definition is the eighth component of a Fmt
                // Character. Driven end to end -- move the choice parameter,
                // let the dropdown's onChange run, and read back what landed
                // in resonance -- because the write happens in the editor's
                // combo callback, not anywhere a map lookup would reach.
                if (auto* fc = proc.apvts.getParameter ("fcharacter"))
                {
                    int wrong = 0;
                    for (int ci = 0; ci < kFmtCustom; ++ci)
                    {
                        fc->beginChangeGesture();
                        fc->setValueNotifyingHost (fc->convertTo0to1 ((float) ci));
                        fc->endChangeGesture();
                        juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
                        const float got = rp->convertFrom0to1 (rp->getValue());
                        const float want = getFmtCharacterMap (ci).definitionPct;
                        if (std::abs (got - want) > 0.51f)      // step is 1 %
                        {
                            std::fprintf (stderr, "  character %d: resonance %.1f, expected %.1f\n",
                                          ci, got, want);
                            ++wrong;
                        }
                    }
                    check (wrong == 0, "every Fmt Character writes its Definition value");
                    // Uni is the one that has to leave the spectral path off
                    fc->beginChangeGesture();
                    fc->setValueNotifyingHost (fc->convertTo0to1 ((float) (kFmtCustom - 1)));
                    fc->endChangeGesture();
                    juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
                    check (std::abs (rp->convertFrom0to1 (rp->getValue())) < 0.01f,
                           "Uni leaves Definition at 0 (no spectral path for it)");
                    // Automation and presets must NOT drop the dropdown to
                    // Custom -- only a hand edit does, and ParamRow draws that
                    // line at Slider::onDragStart. So this drives the callback
                    // the row actually listens to; moving the parameter here
                    // would test the opposite contract and pass for the wrong
                    // reason.
                    fc->beginChangeGesture();
                    fc->setValueNotifyingHost (fc->convertTo0to1 (0.0f));   // Natural
                    fc->endChangeGesture();
                    juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
                    const bool stillChar = fc->convertFrom0to1 (fc->getValue())
                                            < (float) kFmtCustom - 0.5f;
                    check (stillChar, "selecting a character does not immediately self-cancel");
                    if (defSlider != nullptr && defSlider->onDragStart != nullptr)
                    {
                        defSlider->onDragStart();
                        juce::MessageManager::getInstance()->runDispatchLoopUntil (30);
                        check (fc->convertFrom0to1 (fc->getValue()) > (float) kFmtCustom - 0.5f,
                               "hand-editing Definition puts Fmt Character back to Custom");
                    }
                    else
                        check (false, "Definition row exposes the hand-edit hook");
                }
            }
        }

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
            // v0.42.0: the two picture items moved here off the SAVE button
            if (presetBox != nullptr)
            {
                bool choose = false, revert = false;
                for (int i = 0; i < presetBox->getNumItems(); ++i)
                {
                    const auto t = presetBox->getItemText (i);
                    if (t.contains ("Choose character image")) choose = true;
                    if (t.contains ("Use the default image"))  revert = true;
                }
                check (choose, "preset dropdown offers Choose character image");
                check (revert, "preset dropdown offers Use the default image");
            }
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

        // ---- the approach to AUTO MATCHING is a DOUBLE line ----
        // Two feeds (the chosen character, and your own voice) arrive along
        // the same run and must stay legible as two. At a 1 px stroke the
        // difference between "two lines" and "one thick line" is a few
        // pixels of gap, which is exactly the kind of thing that looks fine
        // in a scaled-down screenshot and is wrong on screen -- so it is
        // counted in the pixels instead.
        if (auto* h = findLabelStartingWith (page, "AUTO MATCHING"))
        {
            const auto img = render (*ed);
            // NOT getRight(): the heading Label is laid out across the whole
            // row for alignment, so its right edge is the page edge, not the
            // end of the word. 260 px in from its left is inside the run at
            // both window widths.
            const int x = page->getX() + h->getBounds().getX() + 260;
            const int y0 = page->getY() + h->getBounds().getCentreY();
            int runs = 0; bool inRun = false;
            for (int y = y0 - 20; y <= y0 + 20; ++y)
            {
                // 0.97, not the line colour's own 0.91: a 1 px stroke is
                // ANTIALIASED over the page, so the pure colour never appears
                // as a pixel -- measured, the strokes land at 0.945 against
                // the page's 0.984.
                const bool ink = img.getPixelAt (x, y).getBrightness() < 0.97f;
                if (ink && ! inRun) ++runs;
                inRun = ink;
            }
            std::printf ("  strokes crossing x=%d near AUTO MATCHING: %d\n", x, runs);
            check (runs == 2, "AUTO MATCHING is approached by exactly two lines");
        }

        const int over = auditOverflow (ed.get(), tag);
        std::printf ("  children outside their parent: %d\n", over);
        check (over == 0, "no component escapes its parent");

        shoot (*ed, outDir.getChildFile ("matching_" + tag + ".png"));
    }

    // ---- v0.45.0: the ASMR page ------------------------------------------
    // Checked on the PIXELS as well as the components. The point of this page
    // is a picture -- where the dot sits and what colour things are -- and
    // neither of those is observable from bounds. A scaled-down screenshot is
    // not evidence either: the old pad's mint disc (0xffeef7f4) and the theme
    // dish (0xfffafcfd) look identical at thumbnail size and differ by a hue
    // the eye cannot name, so the tones are compared numerically.
    {
        std::printf ("\n== ASMR page ==\n");
        ed->setSize (1400, 1080);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (200);
        auto* asmrTab = findButton (ed.get(), "ASMR");
        check (asmrTab != nullptr, "ASMR tab button exists");
        if (asmrTab != nullptr) asmrTab->triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (250);

        // -- the parameters, with the ranges and NEUTRAL defaults that make
        //    an old session load unchanged (see dsp/SpatialEngine.h)
        struct Want { const char* id; float lo, hi, def; };
        static const Want wants[] = {
            { "asmrbin",   0.0f,   1.0f,   0.0f   },
            { "asmrdist",  0.0f, 200.0f, 100.0f   },
            { "asmrair",   0.0f, 100.0f,   0.0f   },
            { "asmrroom",  0.0f, 100.0f,   0.0f   },
            { "asmrsize",  0.0f, 100.0f,  50.0f   },
            { "asmrwidth", 0.0f, 200.0f, 100.0f   },
            { "asmrorbit", 0.0f,   2.0f,   0.0f   },
            { "asmrdepth", 0.0f, 100.0f,  60.0f   },
        };
        for (auto& w : wants)
        {
            auto* rp = dynamic_cast<juce::RangedAudioParameter*> (proc.apvts.getParameter (w.id));
            check (rp != nullptr, juce::String (w.id) + " exists");
            if (rp == nullptr) continue;
            const auto& rng = rp->getNormalisableRange();
            check (std::abs (rng.start - w.lo) < 1.0e-4f && std::abs (rng.end - w.hi) < 1.0e-4f,
                   juce::String (w.id) + " range is correct");
            check (std::abs (rp->convertFrom0to1 (rp->getDefaultValue()) - w.def) < 1.0e-3f,
                   juce::String (w.id) + " default is the neutral value");
        }

        juce::Component* page = asmrTab != nullptr ? nullptr : nullptr;
        SonarPad* pad = nullptr;
        walk (ed.get(), [&] (juce::Component* c)
        {
            if (auto* sp = dynamic_cast<SonarPad*> (c)) if (sp->isShowing()) pad = sp;
        });
        check (pad != nullptr && pad->isShowing(), "the sonar pad is on screen");

        // -- every control the page promises has a row bound to it. Rows are
        //    found by label text, the same convention the Definition check
        //    above uses: a parameter with no control would still save and
        //    automate perfectly while being unreachable.
        for (const char* label : { "Distance Amount", "Air Absorption", "Binaural Cues",
                                   "Stereo Width", "Ambience", "Room Size",
                                   "Orbit Rate", "Orbit Radius" })
        {
            int hits = 0;
            walk (ed.get(), [&] (juce::Component* c)
            {
                if (auto* l = dynamic_cast<juce::Label*> (c))
                    if (l->getText() == label && l->isShowing()) ++hits;
                if (auto* b = dynamic_cast<juce::Button*> (c))
                    if (b->getButtonText() == label && b->isShowing()) ++hits;
            });
            check (hits >= 1, juce::String ("a visible control for ") + label);
        }

        // -- scenes are a bulk write, so they are driven, not just counted
        if (auto* behind = findButton (ed.get(), "Behind"))
        {
            behind->triggerClick();
            juce::MessageManager::getInstance()->runDispatchLoopUntil (120);
            const float by  = proc.apvts.getRawParameterValue ("asmry")->load();
            const float bin = proc.apvts.getRawParameterValue ("asmrbin")->load();
            const float air = proc.apvts.getRawParameterValue ("asmrair")->load();
            std::printf ("  Behind scene -> y=%.2f bin=%.0f air=%.0f\n", by, bin, air);
            check (by < -0.5f && bin > 0.5f && air > 0.0f, "the Behind scene writes its whole set");
        }
        else
            check (false, "Behind scene button exists");

        if (auto* offBtn = findButton (ed.get(), "Off"))
        {
            offBtn->triggerClick();
            juce::MessageManager::getInstance()->runDispatchLoopUntil (120);
            bool allNeutral = true;
            for (auto& w : wants)
                if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (proc.apvts.getParameter (w.id)))
                    if (std::abs (rp->convertFrom0to1 (rp->getValue()) - w.def) > 1.0e-3f)
                        allNeutral = false;
            check (allNeutral, "the Off scene puts every control back to neutral");
        }
        else
            check (false, "Off scene button exists");

        // -- the dot is drawn, in the theme's OUTPUT pink, in the quadrant
        //    the two position parameters point at
        if (pad != nullptr)
        {
            auto setP = [&] (const char* id, float v)
            {
                if (auto* rp = proc.apvts.getParameter (id))
                {
                    rp->beginChangeGesture();
                    rp->setValueNotifyingHost (rp->convertTo0to1 (v));
                    rp->endChangeGesture();
                }
            };
            setP ("asmrx", -0.6f);      // front-left
            setP ("asmry",  0.6f);
            juce::MessageManager::getInstance()->runDispatchLoopUntil (150);

            const auto img  = render (*ed);
            const auto area = ed->getLocalArea (pad, pad->getLocalBounds());
            const auto want = ak::seriesOut;                 // 0xfff08ba5
            double sx = 0.0, sy = 0.0;
            int hits = 0;
            for (int y = area.getY(); y < area.getBottom(); ++y)
                for (int x = area.getX(); x < area.getRight(); ++x)
                {
                    const auto px = img.getPixelAt (x, y);
                    const int d = std::abs (px.getRed()   - want.getRed())
                                + std::abs (px.getGreen() - want.getGreen())
                                + std::abs (px.getBlue()  - want.getBlue());
                    if (d < 12) { sx += x; sy += y; ++hits; }
                }
            std::printf ("  pad %s: %d pink px, centroid (%.0f, %.0f), centre (%d, %d)\n",
                         area.toString().toRawUTF8(), hits,
                         hits ? sx / hits : 0.0, hits ? sy / hits : 0.0,
                         area.getCentreX(), area.getCentreY());
            check (hits > 40, "the source dot is drawn in the OUTPUT pink");
            if (hits > 0)
            {
                check (sx / hits < area.getCentreX() - 20, "x = -0.6 puts the dot LEFT of centre");
                check (sy / hits < area.getCentreY() - 20, "y = +0.6 puts the dot ABOVE centre");
            }

            // The dish must be the VISUALIZER donuts' vertical gradient
            // (0xfffafcfd at the top -> 0xffeceff3 at the bottom), not the
            // pad's own mint disc from before v0.45.
            //
            // Asserted POSITIVELY, on two samples of empty dish. The obvious
            // test -- "the old colour 0xffeef7f4 appears nowhere" -- is not
            // sound: it also matches the antialiased edge of the new mint
            // head against the new dish, which lands on that exact tone at
            // one particular alpha, so it reports ~15 hits on a pad that has
            // no disc at all. Absence of a colour is the wrong question when
            // blending can synthesise it.
            //
            // Both samples are taken along the 45-degree ray into the BACK-
            // RIGHT quadrant, which is the one part of the dish with nothing
            // drawn on it: the axes are exactly horizontal/vertical, the
            // rings sit at 1/3, 2/3 and 1 of the radius, the dB labels are
            // only ever above the centre, the ear arcs are at 0.30, and the
            // line of sight runs to the dot in the FRONT-LEFT.
            {
                const float rr = (float) area.getWidth() * 0.5f - 24.0f;
                const float k  = 0.70710678f;
                auto sample = [&] (float f)
                {
                    return img.getPixelAt ((int) (area.getCentreX() + rr * f * k),
                                           (int) (area.getCentreY() + rr * f * k));
                };
                const auto up = sample (0.48f), down = sample (0.86f);
                std::printf ("  dish gradient: upper %s  lower %s\n",
                             up.toDisplayString (false).toRawUTF8(),
                             down.toDisplayString (false).toRawUTF8());
                auto inRange = [] (juce::Colour c)
                {
                    return c.getRed()   >= 232 && c.getRed()   <= 254
                        && c.getGreen() >= 235 && c.getGreen() <= 254
                        && c.getBlue()  >= 239 && c.getBlue()  <= 255;
                };
                check (inRange (up) && inRange (down),
                       "the dish tone is the theme's (neutral, not mint)");
                check (down.getGreen() < up.getGreen(),
                       "and it is the same top-lighter gradient the donuts use");
                // the old disc was a FLAT mint fill: a saturated green cast
                check (up.getGreen() - up.getRed() < 8 && down.getGreen() - down.getRed() < 8,
                       "no green cast left over from the old mint disc");
            }

            // Behind-you shading. The back cue is a FILTER: it has no slider,
            // so the shaded lower half is the only thing on screen that says
            // it is armed. Measured as the same pixel with the switch off and
            // on, which cancels the dish gradient out of the comparison.
            {
                const int px = area.getCentreX() - 40;
                const int py = area.getCentreY() + (int) ((area.getWidth() * 0.5f - 24.0f) * 0.55f);
                setP ("asmrbin", 0.0f);
                juce::MessageManager::getInstance()->runDispatchLoopUntil (120);
                const auto offPx = render (*ed).getPixelAt (px, py);
                setP ("asmrbin", 1.0f);
                juce::MessageManager::getInstance()->runDispatchLoopUntil (120);
                const auto onPx = render (*ed).getPixelAt (px, py);
                std::printf ("  back shading at (%d,%d): off %s  on %s\n", px, py,
                             offPx.toDisplayString (false).toRawUTF8(),
                             onPx.toDisplayString (false).toRawUTF8());
                check (onPx.getBrightness() < offPx.getBrightness() - 0.005f,
                       "Binaural Cues shades the behind-you half of the pad");
                shoot (*ed, outDir.getChildFile ("asmr_active.png"));
                setP ("asmrbin", 0.0f);
            }

            setP ("asmrx", 0.0f);
            setP ("asmry", 0.0f);
        }

        const int over = auditOverflow (ed.get(), "asmr");
        std::printf ("  children outside their parent: %d\n", over);
        check (over == 0, "no component escapes its parent on the ASMR page");
        shoot (*ed, outDir.getChildFile ("asmr_1400x1080.png"));

        // and at the smallest window the editor allows
        ed->setSize (1180, 920);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (250);
        const int overMin = auditOverflow (ed.get(), "asmr-min");
        check (overMin == 0, "ASMR page still fits at the minimum window size");
        shoot (*ed, outDir.getChildFile ("asmr_1180x920.png"));
        juce::ignoreUnused (page);
    }

    // ---- VISUALIZER page (v0.46.0) ----------------------------------------
    // The page lost the radial spectrum donut and turned the vowel mix and the
    // level rings into bars. Two claims here cannot be checked by looking at a
    // screenshot, so both are measured from rendered pixels with signal
    // actually pushed through processBlock:
    //   1. the bars MOVE with the audio (the v0.36.9 trap: a view that has
    //      quietly stopped analysing still draws a perfectly plausible shape)
    //   2. when the voice stops the vowel mix RETURNS TO NEUTRAL instead of
    //      freezing on its last reading, which is the whole point of the
    //      change -- a frozen shape at low alpha still reads as a reading
    {
        std::printf ("\n== VISUALIZER page ==\n");
        ed->setSize (1400, 1080);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (200);
        auto* vizTab = findButton (ed.get(), "Visualizer");
        check (vizTab != nullptr, "Visualizer tab button exists");
        if (vizTab != nullptr) vizTab->triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (250);

        VowelMeter*  vowel = nullptr;
        LevelMeters* level = nullptr;
        SpectrumView*  spec = nullptr;
        DetectionLane* lane = nullptr;
        walk (ed.get(), [&] (juce::Component* c)
        {
            if (auto* v = dynamic_cast<VowelMeter*>  (c)) if (v->isShowing()) vowel = v;
            if (auto* l = dynamic_cast<LevelMeters*> (c)) if (l->isShowing()) level = l;
            if (auto* s = dynamic_cast<SpectrumView*> (c)) if (s->isShowing()) spec = s;
            if (auto* d = dynamic_cast<DetectionLane*> (c)) if (d->isShowing()) lane = d;
        });
        check (vowel != nullptr, "the vowel meter is on screen");
        check (level != nullptr, "the level meters are on screen");
        check (spec  != nullptr, "the spectrum is on screen");
        check (lane  != nullptr, "the detection lane is on screen");
        if (vowel == nullptr || level == nullptr || spec == nullptr || lane == nullptr) return 1;

        // The donut row used to take 55 % of the page. The point of replacing
        // it was to hand that back to the graph, so this is asserted rather
        // than admired: the strip must be short and the spectrum must be the
        // thing that grew.
        std::printf ("  spectrum %s  vowel %s  levels %s\n",
                     spec->getBounds().toString().toRawUTF8(),
                     vowel->getBounds().toString().toRawUTF8(),
                     level->getBounds().toString().toRawUTF8());
        check (vowel->getHeight() <= 160 && level->getHeight() <= 160,
               "the readout strip is short (<= 160 px)");
        check (spec->getHeight() > 2 * vowel->getHeight(),
               "the spectrum is more than twice the strip's height");
        check (vowel->getBounds().getRight() <= level->getX(),
               "vowel and levels do not overlap");

        auto setP = [&] (const char* id, float v)
        {
            if (auto* rp = proc.apvts.getParameter (id))
            {
                rp->beginChangeGesture();
                rp->setValueNotifyingHost (rp->convertTo0to1 (v));
                rp->endChangeGesture();
            }
        };

        // ---- the audio the bars are supposed to be reacting to ------------
        // A voiced /a/: a 140 Hz harmonic series shaped by three resonances,
        // plus a little noise. Parameters alone move nothing -- the taps are
        // filled on the audio thread, so the signal has to go through
        // processBlock, interleaved with the dispatch loop that ticks the
        // 30 Hz timers.
        juce::AudioBuffer<float> buf (2, 512);
        juce::MidiBuffer midi;
        double ph = 0.0;
        juce::Random rnd (7);
        double vf0 = 140.0, vF1 = 730.0, vF2 = 1090.0, vF3 = 2440.0;
        auto feed = [&] (int blocks)
        {
            for (int blk = 0; blk < blocks; ++blk)
            {
                for (int i = 0; i < 512; ++i)
                {
                    double s = 0.0;
                    for (int k = 1; k <= 30; ++k)
                    {
                        const double f = vf0 * k;
                        s += (1.0 / (1.0 + std::pow ((f - vF1) /  90.0, 2.0))
                            + 0.5 / (1.0 + std::pow ((f - vF2) / 110.0, 2.0))
                            + 0.3 / (1.0 + std::pow ((f - vF3) / 160.0, 2.0)))
                             * std::sin (juce::MathConstants<double>::twoPi * f * ph);
                    }
                    const float v = 0.28f * (float) s + 0.004f * (rnd.nextFloat() - 0.5f);
                    buf.setSample (0, i, v);
                    buf.setSample (1, i, v * 0.8f);
                    ph += 1.0 / 48000.0;
                }
                proc.processBlock (buf, midi);
                juce::MessageManager::getInstance()->runDispatchLoopUntil (12);
            }
        };

        // The vowel coordinate is a by-product of AEIOU Character: with the
        // feature off the engine does not track vowels at all, so the panel
        // has nothing to draw and says so instead.
        setP ("vadapt",  1.0f);
        setP ("vamount", 100.0f);
        feed (120);

        std::printf ("  gates: uiWantsViz=%d uiWantsMeters=%d  vowelActive=%d conf=%.2f\n",
                     (int) proc.uiWantsViz.load(), (int) proc.uiWantsMeters.load(),
                     (int) proc.uiVowelActive.load(), proc.uiVowelConf.load());
        check (proc.uiWantsViz.load() && proc.uiWantsMeters.load(),
               "the page asks the audio thread for both taps");
        check (proc.uiInL.rms.load() > 0.01f && proc.uiOutL.rms.load() > 0.001f,
               "signal reached the meters");

        const auto vowelArea = ed->getLocalArea (vowel, vowel->getLocalBounds());
        const auto levelArea = ed->getLocalArea (level, level->getLocalBounds());

        auto isColour = [] (juce::Colour px, juce::Colour want, int tol)
        {
            return std::abs (px.getRed()   - want.getRed())
                 + std::abs (px.getGreen() - want.getGreen())
                 + std::abs (px.getBlue()  - want.getBlue()) < tol;
        };

        // Bar heights, read off the picture: split the panel into five equal
        // columns and find the topmost FILLED pixel in each. Returns 0 for
        // "no bar", 1 for "full". The heading row is skipped because the
        // dominant-vowel chip is drawn in the same pink up there.
        //
        // "Filled" is saturation, not an exact colour match: the bars are
        // drawn at an alpha that drops while nothing is being said, so a
        // resting mint bar lands on (117, 201, 180) rather than on seriesIn
        // itself and an exact-match scan reports the panel as empty. Anything
        // this panel paints is far more colourful than the card (spread 5)
        // or the track outline (spread 19); the exact-colour checks below are
        // where the theme colours themselves get asserted.
        auto saturated = [] (juce::Colour c)
        {
            const int hi = juce::jmax (c.getRed(), juce::jmax (c.getGreen(), c.getBlue()));
            const int lo = juce::jmin (c.getRed(), juce::jmin (c.getGreen(), c.getBlue()));
            return hi - lo > 35;
        };
        auto barHeights = [&] (const juce::Image& img, float* out)
        {
            const int x0 = vowelArea.getX() + 16, x1 = vowelArea.getRight() - 16;
            const int y0 = vowelArea.getY() + 40, y1 = vowelArea.getBottom() - 22;
            for (int c = 0; c < 5; ++c)
            {
                const int cx0 = x0 + (x1 - x0) * c / 5, cx1 = x0 + (x1 - x0) * (c + 1) / 5;
                int topY = y1;
                for (int y = y0; y < y1; ++y)
                    for (int x = cx0; x < cx1; ++x)
                        if (saturated (img.getPixelAt (x, y)))
                        { topY = juce::jmin (topY, y); y = y1; break; }
                out[c] = (float) (y1 - topY) / (float) juce::jmax (1, y1 - y0);
            }
        };

        float speaking[5] = {};
        {
            const auto img = render (*ed);
            barHeights (img, speaking);
            std::printf ("  bars while speaking: A %.2f  I %.2f  U %.2f  E %.2f  O %.2f\n",
                         speaking[0], speaking[1], speaking[2], speaking[3], speaking[4]);
            float lo = 1.0f, hi = 0.0f;
            for (float v : speaking) { lo = juce::jmin (lo, v); hi = juce::jmax (hi, v); }
            check (hi > 0.6f, "a vowel is being read (one bar is well up)");
            check (hi - lo > 0.3f, "the bars differ, i.e. they carry a measurement");

            // The winning bar and its chip are the theme's OUTPUT pink and the
            // rest are the INPUT mint -- no colour of this panel's own. Both
            // are counted POSITIVELY: "the old colour is absent" is the wrong
            // question when antialiasing can synthesise any tone (v0.45.0).
            int pink = 0, mint = 0;
            for (int y = vowelArea.getY(); y < vowelArea.getBottom(); ++y)
                for (int x = vowelArea.getX(); x < vowelArea.getRight(); ++x)
                {
                    const auto px = img.getPixelAt (x, y);
                    if (isColour (px, ak::seriesOut, 12)) ++pink;
                    if (isColour (px, ak::seriesIn,  12)) ++mint;
                }
            std::printf ("  vowel panel: %d px seriesOut, %d px seriesIn\n", pink, mint);
            check (pink > 200, "the dominant vowel is drawn in the theme's output pink");
            check (mint > 50,  "the other vowels are drawn in the theme's input mint");
        }

        // ---- the levels are the theme's colours, per row ------------------
        // Checked row by row: the two INPUT rows must carry mint and the two
        // OUTPUT rows pink. The dial this replaced drew its input lane in the
        // sidebar blue, which is what "unify the colours" was about.
        {
            const auto img = render (*ed);
            const int top = levelArea.getY() + 32, bot = levelArea.getBottom() - 24;
            const int rowH = juce::jmax (1, (bot - top) / 4);
            for (int row = 0; row < 4; ++row)
            {
                const auto want = row < 2 ? ak::seriesIn : ak::seriesOut;
                int hits = 0;
                for (int y = top + row * rowH; y < top + (row + 1) * rowH; ++y)
                    for (int x = levelArea.getX(); x < levelArea.getRight(); ++x)
                        if (isColour (img.getPixelAt (x, y), want, 12)) ++hits;
                std::printf ("  level row %d: %d px of %s\n", row, hits,
                             want.toDisplayString (false).toRawUTF8());
                check (hits > 100, juce::String ("level row ") + juce::String (row)
                                     + " is filled in its theme colour");
            }
        }
        shoot (*ed, outDir.getChildFile ("viz_speaking.png"));

        // ---- detection lane (v0.47.0) -------------------------------------
        // The whole promise of this strip is that a marker sits at the x of
        // the thing it names in the curve above. That is a geometric claim,
        // so it is measured, not admired: the marker positions are read out
        // of the rendered image, converted back to Hz through the axis, and
        // compared with the numbers the engine actually published.
        {
            check (lane->getX() == spec->getX() && lane->getWidth() == spec->getWidth(),
                   "the lane has the spectrum's exact x and width (this is what aligns them)");

            const auto laneArea = ed->getLocalArea (lane, lane->getLocalBounds());
            const auto sp = vmAxis::span (lane->getLocalBounds());
            auto hzAt = [&] (double xLocal)
            {
                const double t = (xLocal - sp.getStart()) / sp.getLength();
                return vmAxis::kLoHz * std::pow (1000.0, t);
            };
            // centroid of one row's markers, split into clusters: markers are
            // 7 px wide and the four features are far apart on a log axis
            auto rowMarkers = [&] (const juce::Image& img, float yFrac, juce::Colour want)
            {
                std::vector<double> hits, out;
                const int y0 = laneArea.getY() + (int) (laneArea.getHeight() * yFrac) - 5;
                const int y1 = y0 + 10;
                for (int x = laneArea.getX(); x < laneArea.getRight(); ++x)
                {
                    int n = 0;
                    for (int y = y0; y < y1; ++y)
                    {
                        const auto px = img.getPixelAt (x, y);
                        if (std::abs (px.getRed()   - want.getRed())
                          + std::abs (px.getGreen() - want.getGreen())
                          + std::abs (px.getBlue()  - want.getBlue()) < 30) ++n;
                    }
                    if (n >= 3) hits.push_back (x);      // a marker body, not a hairline
                }
                for (size_t i = 0; i < hits.size();)
                {
                    size_t j = i;
                    double sum = 0.0;
                    while (j < hits.size() && hits[j] - hits[i == j ? j : j - 1] <= 2.0)
                    { sum += hits[j]; ++j; }
                    out.push_back (sum / (double) (j - i));
                    i = j;
                }
                return out;
            };

            const auto img = render (*ed);
            const auto inM = rowMarkers (img, 0.34f, ak::seriesIn);
            std::printf ("  lane IN markers: %d\n", (int) inM.size());
            for (auto x : inM)
                std::printf ("    x=%.0f -> %.0f Hz\n", x, hzAt (x - laneArea.getX()));

            // engine truth: f0 and F1-F3, as published
            const float truth[4] = { proc.uiF0In.load(),  proc.uiFmtIn[0].load(),
                                     proc.uiFmtIn[1].load(), proc.uiFmtIn[2].load() };
            std::printf ("  engine says f0=%.0f F1=%.0f F2=%.0f F3=%.0f Hz\n",
                         truth[0], truth[1], truth[2], truth[3]);
            check (proc.uiFmtValid.load(), "formants are being tracked (AEIOU is on)");
            check (inM.size() >= 4, "all four input markers are drawn");

            // Every published feature must have a marker within 3 px of where
            // the shared axis puts it. 3 px is about 1.7 % in frequency here,
            // i.e. tighter than the marker is wide.
            for (int i = 0; i < 4 && inM.size() >= 4; ++i)
            {
                // span() is in the lane's LOCAL coordinates (getLocalBounds()
                // starts at 0), so the only conversion is the lane's origin
                // in the editor -- laneArea.getX(). Adding the lane's own x
                // within its page on top of that is a second offset for the
                // same thing, and it showed up as every marker being a
                // constant 15 px "wrong" while the frequencies read back off
                // those same pixels were exact.
                const float wantX = vmAxis::xFor (sp, truth[i]) + laneArea.getX();
                double best = 1.0e9;
                for (auto x : inM) best = std::min (best, std::abs (x - (double) wantX));
                std::printf ("    %s: want x=%.0f, nearest marker %.1f px away\n",
                             i == 0 ? "f0" : i == 1 ? "F1" : i == 2 ? "F2" : "F3", wantX, best);
                check (best <= 3.0, juce::String ("a marker sits on the published ")
                                      + (i == 0 ? "f0" : i == 1 ? "F1" : i == 2 ? "F2" : "F3"));
            }

            // -- and it MOVES with the conversion: +7 semitones has to push
            //    the output f0 marker to the right of the input one, by the
            //    ratio the axis says (a marker that never moves would pass
            //    every static check above)
            const float f0Before = proc.uiF0Out.load();
            setP ("pitch", 7.0f);
            feed (40);
            const float f0After = proc.uiF0Out.load();
            std::printf ("  +7 st: output f0 %.1f -> %.1f Hz (x1.498 expected)\n",
                         f0Before, f0After);
            check (f0After > f0Before * 1.42f && f0After < f0Before * 1.58f,
                   "the published output f0 follows the pitch parameter");
            // Recorded, not asserted: a +7 st shift narrows the grain (the
            // width is capped at 1.25x the OUTPUT spacing), so the engine's
            // envelope has fewer bins to work with and its own F2/F3 start
            // to merge. That is the engine's reading and the lane shows it
            // faithfully -- pinning a number on it here would be asserting
            // the tracker, not the display.
            std::printf ("  at +7 st the engine reads F1=%.0f F2=%.0f F3=%.0f Hz\n",
                         proc.uiFmtIn[0].load(), proc.uiFmtIn[1].load(),
                         proc.uiFmtIn[2].load());
            {
                const auto img2 = render (*ed);
                const auto outM = rowMarkers (img2, 0.72f, ak::seriesOut);
                const auto inM2 = rowMarkers (img2, 0.34f, ak::seriesIn);
                check (! outM.empty() && ! inM2.empty(), "both rows still have markers");
                if (! outM.empty() && ! inM2.empty())
                {
                    std::printf ("  leftmost marker: IN x=%.0f  OUT x=%.0f\n", inM2[0], outM[0]);
                    check (outM[0] > inM2[0] + 8.0,
                           "the OUTPUT f0 marker has moved right of the INPUT one");
                }
                shoot (*ed, outDir.getChildFile ("viz_lane_shifted.png"));
            }
            // ---- a short unvoiced gap must not wipe the formant markers ----
            // v0.48.1. The spectral layer only runs on voiced grains, so a
            // consonant or a breath drops formantsValid() for a moment. The
            // markers used to blink out there while f0 sat still, which made
            // them unreadable in normal speech.
            {
                // Count by SATURATION, not by an exact colour match: a fading
                // marker is seriesIn at partial alpha over the card, which is
                // nowhere near seriesIn itself. Counting exact matches
                // reported "0 markers" for a lane that was visibly still
                // showing all three -- the same trap the vowel bars hit.
                auto markerRuns = [&] (const juce::Image& img)
                {
                    const int y0 = laneArea.getY() + (int) (laneArea.getHeight() * 0.34f) - 5;
                    int runs = 0; bool in = false;
                    for (int x = laneArea.getX(); x < laneArea.getRight() - 60; ++x)
                    {
                        int n = 0;
                        for (int y = y0; y < y0 + 10; ++y)
                            if (saturated (img.getPixelAt (x, y))) ++n;
                        if (n >= 3) { if (! in) ++runs; in = true; } else in = false;
                    }
                    return runs;
                };
                const int before = markerRuns (render (*ed));
                for (int blk = 0; blk < 12; ++blk)     // ~0.15 s of silence
                {
                    buf.clear();
                    proc.processBlock (buf, midi);
                    juce::MessageManager::getInstance()->runDispatchLoopUntil (12);
                }
                const int during = markerRuns (render (*ed));
                std::printf ("  markers before a 0.15 s gap: %d, during: %d\n", before, during);
                check (before >= 3, "markers were up before the gap");
                check (during >= before - 1,
                       "a short unvoiced gap does not wipe the formant markers");
                feed (40);                              // back to the vowel
            }

            // ---- a formant that cannot be measured is not drawn on a default --
            // v0.48.0. On a high voice F1 is the first casualty: once f0 is up
            // near where F1 sits there is no separate peak left, the tracker
            // falls back to its band default, and the old lane drew a confident
            // dot on that constant. Measured on a real 251-317 Hz voice, F1 read
            // 495 Hz (= defR[0]) on every vowel. Here: an /i/-like vowel at
            // f0 300 with F1 also at 300, which is exactly that situation.
            {
                vf0 = 300.0; vF1 = 300.0; vF2 = 2300.0; vF3 = 3100.0;
                feed (150);
                const float c1 = proc.uiFmtConf[0].load(), c3 = proc.uiFmtConf[2].load();
                std::printf ("  high voice (f0 300, F1 300): conf F1=%.2f F2=%.2f F3=%.2f, "
                             "F1 published as %.0f Hz\n",
                             c1, proc.uiFmtConf[1].load(), c3, proc.uiFmtIn[0].load());
                check (c1 < 0.15f, "F1 reports itself as not measured on a high voice");
                check (c3 > 0.5f,  "F3 is still measured (the flag is per formant)");

                const auto img = render (*ed);
                const auto inM = rowMarkers (img, 0.34f, ak::seriesIn);
                std::printf ("  markers drawn: %d\n", (int) inM.size());
                for (auto x : inM) std::printf ("    x=%.0f -> %.0f Hz\n", x, hzAt (x - laneArea.getX()));
                // f0 and the upper formants stay; F1 must NOT be drawn on 495 Hz
                const float f1x = vmAxis::xFor (sp, proc.uiFmtIn[0].load()) + laneArea.getX();
                bool drawnOnDefault = false;
                for (auto x : inM) if (std::abs (x - (double) f1x) < 4.0) drawnOnDefault = true;
                check (! drawnOnDefault,
                       "no marker is drawn at the F1 fallback constant");
                shoot (*ed, outDir.getChildFile ("viz_lane_highvoice.png"));
                vf0 = 140.0; vF1 = 730.0; vF2 = 1090.0; vF3 = 2440.0;
                feed (60);
            }

            setP ("pitch", 0.0f);
            feed (30);
        }

        // ---- silence: the mix settles back to an even five ----------------
        // "No input" means SILENT BLOCKS STILL ARRIVING, which is what a host
        // does when you stop talking -- not the host stopping. Simply ceasing
        // to call processBlock freezes uiVowelConf at its last value and the
        // meter would be right to hold, so that version of the test proved
        // nothing about the release. ~2.4 s is a little over two of the
        // glide's time constants. The release is deliberately slow (about a
        // 1.1 s time constant), so this runs for ~3.5 s of message thread:
        // at 2.4 s the held vowel was still 0.14 of the lane above the rest,
        // which is the glide working, not the glide failing.
        for (int blk = 0; blk < 300; ++blk)
        {
            buf.clear();          // INSIDE the loop: processBlock works in
                                  // place, so reusing the buffer without
                                  // clearing feeds the output back in as the
                                  // next input and the "silence" is a
                                  // feedback loop that never goes quiet
            proc.processBlock (buf, midi);
            juce::MessageManager::getInstance()->runDispatchLoopUntil (12);
        }
        std::printf ("  after silence: vowelConf=%.3f  inL=%.5f outL=%.5f\n",
                     proc.uiVowelConf.load(), proc.uiInL.rms.load(), proc.uiOutL.rms.load());
        check (proc.uiInL.rms.load() < 0.002f, "the input meter fell back on silence");
        {
            const auto img = render (*ed);
            float quiet[5] = {};
            barHeights (img, quiet);
            std::printf ("  bars after silence:  A %.2f  I %.2f  U %.2f  E %.2f  O %.2f\n",
                         quiet[0], quiet[1], quiet[2], quiet[3], quiet[4]);
            float lo = 1.0f, hi = 0.0f;
            for (float v : quiet) { lo = juce::jmin (lo, v); hi = juce::jmax (hi, v); }
            check (hi - lo < 0.10f, "silence brings the five bars back level with each other");
            check (lo > 0.05f && hi < 0.7f,
                   "and to a NEUTRAL height, not to zero and not stuck at the last shape");
            check (quiet[0] < speaking[0] - 0.2f,
                   "the vowel that was being held has actually come down");
        }
        shoot (*ed, outDir.getChildFile ("viz_neutral.png"));

        // ---- AEIOU Character off: there is no measurement to show ---------
        setP ("vadapt", 0.0f);
        // Long enough for the engine to re-acquire pitch after the silence
        // above -- the lane fades a marker out over ~0.3 s and only brings it
        // back once f0 is being detected again, so a short burst here tests
        // the recovery timing rather than the thing this block is about.
        feed (60);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (300);
        {
            check (! proc.uiVowelActive.load(), "the engine stops tracking vowels");
            const auto img = render (*ed);
            int pink = 0;
            for (int y = vowelArea.getY(); y < vowelArea.getBottom(); ++y)
                for (int x = vowelArea.getX(); x < vowelArea.getRight(); ++x)
                    if (isColour (img.getPixelAt (x, y), ak::seriesOut, 12)) ++pink;
            // Empty, not neutral: a filled bar is a reading, and with the
            // feature off nothing measured one. The panel says so in words
            // instead (drawn text, so it has no component to find).
            //
            // Read at the FOOT of each bar, not with barHeights(): every fill
            // grows up from the bottom, so an empty foot is an empty bar --
            // whereas a top-down scan finds the "AEIOU Character is off"
            // message itself, which is drawn in the heading blue across the
            // middle three columns and is just as saturated as a bar.
            const int x0 = vowelArea.getX() + 16, x1 = vowelArea.getRight() - 16;
            const int footTop = vowelArea.getBottom() - 40, footBot = vowelArea.getBottom() - 26;
            int foot[5] = {};
            for (int c = 0; c < 5; ++c)
                for (int y = footTop; y < footBot; ++y)
                    for (int x = x0 + (x1 - x0) * c / 5; x < x0 + (x1 - x0) * (c + 1) / 5; ++x)
                        if (saturated (img.getPixelAt (x, y))) ++foot[c];
            std::printf ("  feature off: %d px seriesOut, bar feet %d %d %d %d %d\n",
                         pink, foot[0], foot[1], foot[2], foot[3], foot[4]);
            check (pink < 40, "no vowel is named while nothing is measuring one");
            for (int i = 0; i < 5; ++i)
                check (foot[i] < 8, juce::String ("bar ") + juce::String (i)
                                      + " is empty with the feature off");

            // The lane has the same dependency: with no formant feature on,
            // the engine skips its spectral layer and trackF[] is stale. The
            // lane must drop to the f0 pair alone rather than keep drawing
            // the last formants it saw.
            check (! proc.uiFmtValid.load(),
                   "the engine reports its formant analysis as not running");
            const auto laneA = ed->getLocalArea (lane, lane->getLocalBounds());
            int rows = 0;
            {
                const int y0 = laneA.getY() + (int) (laneA.getHeight() * 0.34f) - 5;
                bool run = false;
                for (int x = laneA.getX(); x < laneA.getRight(); ++x)
                {
                    int n = 0;
                    for (int y = y0; y < y0 + 10; ++y)
                    {
                        const auto px = img.getPixelAt (x, y);
                        if (std::abs (px.getRed()   - ak::seriesIn.getRed())
                          + std::abs (px.getGreen() - ak::seriesIn.getGreen())
                          + std::abs (px.getBlue()  - ak::seriesIn.getBlue()) < 30) ++n;
                    }
                    if (n >= 3) { if (! run) ++rows; run = true; } else run = false;
                }
            }
            std::printf ("  lane with formants off: %d input marker(s)\n", rows);
            check (rows == 1, "only the f0 marker is left, not stale formants");
        }
        shoot (*ed, outDir.getChildFile ("viz_off.png"));
        setP ("vamount", 60.0f);

        const int over = auditOverflow (ed.get(), "visualizer");
        std::printf ("  children outside their parent: %d\n", over);
        check (over == 0, "no component escapes its parent on the VISUALIZER page");

        ed->setSize (1180, 920);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (250);
        check (auditOverflow (ed.get(), "visualizer-min") == 0,
               "VISUALIZER page still fits at the minimum window size");
        shoot (*ed, outDir.getChildFile ("viz_1180x920.png"));
    }

    // ---- Pulse Smoothing (v0.50.0) ----------------------------------------
    // A parameter that reaches the engine is the only kind worth having, so
    // this is driven end to end: build a slightly creaky voice (alternating
    // pulse amplitudes, which is what a real voice does and what an upshift
    // turns into a growl at the old pitch), push it through processBlock at
    // +9 st with the switch on and off, and compare the half-integer
    // harmonic content of the two outputs.
    {
        std::printf ("\n== Pulse Smoothing ==\n");
        auto* ps = proc.apvts.getParameter ("pulsesmooth");
        check (ps != nullptr, "pulsesmooth parameter exists");
        if (ps != nullptr)
        {
            check (ps->getDefaultValue() > 0.5f, "Pulse Smoothing defaults to ON");
            // A toggle row carries its name on the BUTTON, not on a Label --
            // matching only Labels reported the row missing when it was
            // there. (The ASMR audit above already matches both; this one
            // did not.)
            int rows = 0;
            walk (ed.get(), [&] (juce::Component* c)
            {
                if (auto* l = dynamic_cast<juce::Label*> (c))
                    if (l->getText() == "Pulse Smoothing") ++rows;
                if (auto* b = dynamic_cast<juce::Button*> (c))
                    if (b->getButtonText() == "Pulse Smoothing") ++rows;
            });
            std::printf ("  controls named \"Pulse Smoothing\": %d\n", rows);
            check (rows >= 1, "a row is bound to it");

            auto run = [&] (bool on)
            {
                proc.reset();
                auto* pp = proc.apvts.getParameter ("pitch");
                pp->beginChangeGesture(); pp->setValueNotifyingHost (pp->convertTo0to1 (9.0f)); pp->endChangeGesture();
                ps->beginChangeGesture(); ps->setValueNotifyingHost (on ? 1.0f : 0.0f); ps->endChangeGesture();
                juce::AudioBuffer<float> b (2, 512);
                juce::MidiBuffer m;
                std::vector<float> out;
                double ph = 0.0; int pulse = 0;
                for (int blk = 0; blk < 120; ++blk)
                {
                    for (int i = 0; i < 512; ++i)
                    {
                        // 120 Hz pulse train whose pulses alternate in
                        // amplitude by 25 % -- a mild creak
                        ph += 120.0 / 48000.0;
                        if (ph >= 1.0) { ph -= 1.0; ++pulse; }
                        const double env = std::exp (-ph * 26.0);
                        const float amp = (pulse & 1) ? 0.75f : 1.0f;
                        const float v = (float) (0.35 * amp * env * std::sin (juce::MathConstants<double>::twoPi * 3.0 * ph));
                        b.setSample (0, i, v);  b.setSample (1, i, v);
                    }
                    proc.processBlock (b, m);
                    if (blk > 40)
                        out.insert (out.end(), b.getReadPointer (0), b.getReadPointer (0) + 512);
                }
                return out;
            };
            const auto off = run (false), on = run (true);
            // half-integer harmonics of the OUTPUT pitch (120 * 2^(9/12) = 202 Hz)
            auto subIndex = [] (const std::vector<float>& x)
            {
                const int N = 8192;
                if ((int) x.size() < N) return 0.0;
                std::vector<float> re (x.end() - N, x.end()), im ((size_t) N, 0.0f);
                for (int i = 0; i < N; ++i)
                    re[(size_t) i] *= 0.5f - 0.5f * std::cos (2.0f * (float) M_PI * i / N);
                PsolaEngine::fftForViz (re.data(), im.data(), N);
                const double f0 = 120.0 * std::pow (2.0, 9.0 / 12.0), fs = 48000.0;
                double h = 0.0, sub = 0.0;
                for (int m = 2; m <= 12; ++m)
                    for (int half = 0; half < 2; ++half)
                    {
                        const double hz = f0 * (m + 0.5 * half);
                        const int b0 = (int) ((hz - 25) * N / fs), b1 = (int) ((hz + 25) * N / fs);
                        double pk = 0;
                        for (int k = std::max (1, b0); k <= std::min (N / 2, b1); ++k)
                            pk = std::max (pk, (double) (re[(size_t) k] * re[(size_t) k]
                                                       + im[(size_t) k] * im[(size_t) k]));
                        (half ? sub : h) += pk;
                    }
                return 10.0 * std::log10 ((sub + 1e-20) / (h + 1e-20));
            };
            const double sOff = subIndex (off), sOn = subIndex (on);
            std::printf ("  subharmonic index: off %.2f dB, on %.2f dB (change %+.2f)\n",
                         sOff, sOn, sOn - sOff);
            check (sOn < sOff - 1.0,
                   "Pulse Smoothing actually reduces the half-integer harmonics");
            auto* pp = proc.apvts.getParameter ("pitch");
            pp->beginChangeGesture(); pp->setValueNotifyingHost (pp->convertTo0to1 (0.0f)); pp->endChangeGesture();
        }
    }

    // ---- Pulse Body (v0.52.0) ---------------------------------------------
    // Two things have to hold: 0 is the OLD sound to the sample (so nobody's
    // session changes when they update), and raising it actually rounds the
    // waveform out. The second is measured the way the real investigation
    // was -- per-period positive/negative peak ratio -- because that is the
    // quantity the control exists to move.
    //
    // The source here is a deliberately ONE-SIDED pulse train (fast attack,
    // slow decay), because a symmetric test tone has no pulse shape to lose
    // and would report success no matter what the engine did.
    {
        std::printf ("\n== Pulse Body ==\n");
        auto* pb = proc.apvts.getParameter ("pulsebody");
        check (pb != nullptr, "pulsebody parameter exists");
        if (pb != nullptr)
        {
            check (std::abs (pb->getDefaultValue() - 0.75f) < 0.01f,
                   "Pulse Body defaults to 0.75 (the value the user chose by ear)");
            int rows = 0;
            walk (ed.get(), [&] (juce::Component* c)
            {
                if (auto* l = dynamic_cast<juce::Label*> (c))
                    if (l->getText() == "Pulse Body") ++rows;
                if (auto* b = dynamic_cast<juce::Button*> (c))
                    if (b->getButtonText() == "Pulse Body") ++rows;
            });
            std::printf ("  controls named \"Pulse Body\": %d\n", rows);
            check (rows >= 1, "a row is bound to it on the MAIN tab");

            auto run = [&] (float body)
            {
                // NOT proc.reset() -- the processor does not override it, so
                // it is JUCE's no-op and the engine's ring buffers survive
                // into the next run. prepareToPlay is what actually clears
                // them, and without it "same parameters -> same samples" is
                // not even true of the unchanged engine.
                proc.prepareToPlay (48000.0, 512);
                auto* pp = proc.apvts.getParameter ("pitch");
                pp->beginChangeGesture(); pp->setValueNotifyingHost (pp->convertTo0to1 (9.0f)); pp->endChangeGesture();
                pb->beginChangeGesture(); pb->setValueNotifyingHost (body); pb->endChangeGesture();
                juce::AudioBuffer<float> b (2, 512);
                juce::MidiBuffer m;
                std::vector<float> out;
                double ph = 0.0;
                for (int blk = 0; blk < 120; ++blk)
                {
                    for (int i = 0; i < 512; ++i)
                    {
                        ph += 130.0 / 48000.0;
                        if (ph >= 1.0) ph -= 1.0;
                        // sharp rise, long decay: a caricature of a glottal
                        // pulse, one-sided on purpose
                        const double v = std::exp (-ph * 14.0) - 0.35 * std::exp (-ph * 3.0);
                        const float x = (float) (0.35 * v);
                        b.setSample (0, i, x);  b.setSample (1, i, x);
                    }
                    proc.processBlock (b, m);
                    if (blk > 40)
                        out.insert (out.end(), b.getReadPointer (0), b.getReadPointer (0) + 512);
                }
                return out;
            };
            // ratio of the largest positive to the largest negative excursion
            // inside each output period, median over periods
            auto asym = [] (const std::vector<float>& x)
            {
                const double f0 = 130.0 * std::pow (2.0, 9.0 / 12.0);
                const int P = (int) std::lround (48000.0 / f0);
                std::vector<double> r;
                for (size_t a = 0; a + (size_t) P < x.size(); a += (size_t) P)
                {
                    float hi = -1e9f, lo = 1e9f;
                    for (int k = 0; k < P; ++k) { hi = std::max (hi, x[a + (size_t) k]); lo = std::min (lo, x[a + (size_t) k]); }
                    if (hi > 1e-5f && lo < -1e-5f) r.push_back ((double) hi / (double) -lo);
                }
                if (r.empty()) return 1.0;
                std::sort (r.begin(), r.end());
                return r[r.size() / 2];
            };
            const auto zero = run (0.0f), zero2 = run (0.0f), wide = run (0.75f);
            size_t diff = 0;
            for (size_t i = 0; i < std::min (zero.size(), zero2.size()); ++i)
                if (zero[i] != zero2[i]) ++diff;
            check (diff == 0 && ! zero.empty(), "Pulse Body 0 is repeatable to the sample");
            size_t moved = 0;
            for (size_t i = 0; i < std::min (zero.size(), wide.size()); ++i)
                if (zero[i] != wide[i]) ++moved;
            check (moved > zero.size() / 10, "Pulse Body 0.75 actually changes the output");
            const double a0 = asym (zero), a1 = asym (wide);
            std::printf ("  peak ratio: body 0 = %.3f, body 0.75 = %.3f\n", a0, a1);
            // Deliberately loose: the exact numbers belong to THIS generator,
            // and the real measurement lives on the user's recording (see
            // HANDOVER v0.52.0). All this has to catch is the lever being
            // wired backwards or not at all.
            check (std::abs (a1 - 1.0) < std::abs (a0 - 1.0),
                   "Pulse Body moves the waveform toward symmetry");
            auto* pp = proc.apvts.getParameter ("pitch");
            pp->beginChangeGesture(); pp->setValueNotifyingHost (pp->convertTo0to1 (0.0f)); pp->endChangeGesture();
            pb->beginChangeGesture(); pb->setValueNotifyingHost (0.0f); pb->endChangeGesture();
        }
    }

    // ---- Onset Hold (v0.54.0) ---------------------------------------------
    // The defect: voicing dropped out for a few frames in the middle of a
    // phrase attack, and the unvoiced path does not pitch-shift, so a burst
    // of the UNTRANSPOSED input escaped at full level. The test drives a
    // crescendo on purpose -- a steady tone never fails the confidence test,
    // so a steady-tone test would pass either way and prove nothing.
    {
        std::printf ("\n== Onset Hold ==\n");
        auto* oh = proc.apvts.getParameter ("onsethold");
        check (oh != nullptr, "onsethold parameter exists");
        if (oh != nullptr)
        {
            check (oh->getDefaultValue() > 0.5f, "Onset Hold defaults to ON");
            int rows = 0;
            walk (ed.get(), [&] (juce::Component* c)
            {
                if (auto* l = dynamic_cast<juce::Label*> (c))
                    if (l->getText() == "Onset Hold") ++rows;
                if (auto* b = dynamic_cast<juce::Button*> (c))
                    if (b->getButtonText() == "Onset Hold") ++rows;
            });
            std::printf ("  controls named \"Onset Hold\": %d\n", rows);
            check (rows >= 1, "a row is bound to it");

            auto run = [&] (bool on)
            {
                proc.prepareToPlay (48000.0, 512);
                auto* pp = proc.apvts.getParameter ("pitch");
                pp->beginChangeGesture(); pp->setValueNotifyingHost (pp->convertTo0to1 (9.0f)); pp->endChangeGesture();
                oh->beginChangeGesture(); oh->setValueNotifyingHost (on ? 1.0f : 0.0f); oh->endChangeGesture();
                juce::AudioBuffer<float> b (2, 512);
                juce::MidiBuffer m;
                std::vector<float> out;
                // A CLEAN harmonic stack never fails the confidence test,
                // whatever envelope it is given -- measured, 0.048 stuck
                // either way at every ramp from 12 to 90 ms. The defect needs
                // a crescendo AND the period-to-period irregularity a real
                // voice has, so the source here is a pulse train with 15 %
                // jitter and shimmer. Freeze those numbers: they belong to
                // this generator, which is why the check below compares the
                // two runs instead of testing an absolute level.
                juce::Random rng (20260821);
                double ph = 0.0, amp = 1.0, per = 48000.0 / 110.0;
                for (int blk = 0; blk < 90; ++blk)
                {
                    for (int i = 0; i < 512; ++i)
                    {
                        const double t = (blk * 512 + i) / 48000.0;
                        ph += 1.0 / per;
                        if (ph >= 1.0)
                        {
                            ph -= 1.0;
                            per = 48000.0 / 110.0 * (1.0 + 0.15 * (rng.nextDouble() * 2.0 - 1.0));
                            amp = 1.0 + 0.45 * (rng.nextDouble() * 2.0 - 1.0);
                        }
                        double s2 = 0.0;
                        for (int k = 1; k <= 24; ++k)
                            s2 += std::exp (-0.16 * (k - 1)) * std::sin (2.0 * M_PI * k * ph);
                        double env = 0.0;
                        if (t > 0.40) env = std::min (1.0, (t - 0.40) / 0.09);
                        b.setSample (0, i, (float) (0.3 * env * amp * s2));
                        b.setSample (1, i, (float) (0.3 * env * amp * s2));
                    }
                    proc.processBlock (b, m);
                    out.insert (out.end(), b.getReadPointer (0), b.getReadPointer (0) + 512);
                }
                return out;
            };
            // how much of the attack still sits at the INPUT pitch (110 Hz)
            // rather than the shifted one (185 Hz)
            auto stuck = [] (const std::vector<float>& x)
            {
                const int fs = 48000, N = 2048;
                int bad = 0, tot = 0;
                for (int s = (int) (0.42 * fs); s + N < (int) (0.62 * fs); s += N / 4)
                {
                    std::vector<float> re (x.begin() + s, x.begin() + s + N), im ((size_t) N, 0.0f);
                    double e = 0.0;
                    for (int i = 0; i < N; ++i) e += re[(size_t) i] * re[(size_t) i];
                    if (e / N < 1.0e-6) continue;
                    for (int i = 0; i < N; ++i)
                        re[(size_t) i] *= 0.5f - 0.5f * std::cos (2.0f * (float) M_PI * i / N);
                    PsolaEngine::fftForViz (re.data(), im.data(), N);
                    auto band = [&] (double lo, double hi)
                    {
                        double a = 0.0;
                        for (int k = (int) (lo * N / fs); k <= (int) (hi * N / fs); ++k)
                            a += re[(size_t) k] * re[(size_t) k] + im[(size_t) k] * im[(size_t) k];
                        return a;
                    };
                    ++tot;
                    if (band (95, 130) > band (165, 205)) ++bad;   // input pitch wins
                }
                return tot > 0 ? (double) bad / tot : 0.0;
            };
            const double off = stuck (run (false)), on = stuck (run (true));
            std::printf ("  attack frames dominated by the INPUT pitch: off %.2f, on %.2f\n", off, on);
            check (off > 0.05, "the generator actually reproduces the defect");
            check (on < off * 0.5,
                   "Onset Hold at least halves the untransposed burst in the attack");
            auto* pp = proc.apvts.getParameter ("pitch");
            pp->beginChangeGesture(); pp->setValueNotifyingHost (pp->convertTo0to1 (0.0f)); pp->endChangeGesture();
            oh->beginChangeGesture(); oh->setValueNotifyingHost (1.0f); oh->endChangeGesture();
        }
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

    // ---- built-in badge art: the naming contract (v0.42.0) ----
    // MATCH writes "builtin:<BinaryData name>" into characterImagePath and
    // the badge resolves it through ak::image. A typo in a catalog `image`
    // field would leave the badge blank with nothing else going wrong, so the
    // resolution is checked directly, for every entry that claims art.
    {
        std::printf ("\n== built-in badge art ==\n");
        int nCat = 0;
        const auto* cat = getSampleTargets (nCat);
        int withArt = 0;
        for (int i = 0; i < nCat; ++i)
        {
            const juce::String res (cat[i].image != nullptr ? cat[i].image : "");
            if (res.isEmpty()) continue;
            ++withArt;
            const auto img = ak::image (res.toRawUTF8());
            std::printf ("  %-6s %-16s %s (%d x %d)\n", cat[i].id, res.toRawUTF8(),
                         img.isValid() ? "ok" : "MISSING", img.getWidth(), img.getHeight());
            check (img.isValid(), juce::String (cat[i].id) + ": image resource resolves");

            // and the exact string MATCH would store must round-trip
            proc.characterImagePath = juce::String (kBuiltinImagePrefix) + res;
            check (proc.characterImagePath.startsWith (kBuiltinImagePrefix),
                   "stored path carries the builtin prefix");
            const auto viaPrefix = ak::image (proc.characterImagePath
                                                  .fromFirstOccurrenceOf (kBuiltinImagePrefix,
                                                                          false, false)
                                                  .toRawUTF8());
            check (viaPrefix.isValid(), "badge resolves the stored builtin path");
        }
        std::printf ("  %d of %d catalog entries ship art\n", withArt, nCat);
        check (withArt > 0, "at least one catalog entry ships art");
        proc.characterImagePath.clear();
    }

    ed->removeFromDesktop();
    std::printf ("\n%s (%d failure%s)\n", g_fail == 0 ? "ALL PASS" : "FAILURES",
                 g_fail, g_fail == 1 ? "" : "s");
    return g_fail == 0 ? 0 : 1;
}
