// LIMIT panel (Ozone-style twin slider), ported from SPASynth 1.0.33's fxHeaderTests
// (limiterPanelControlsTest / limiterMetersTest). Every control is hit-tested at its
// centre and driven through the real mouse path (a control that does not respond in a
// test is a bug report, never something to route around). Included from SPAStripTests.cpp.

namespace limiterPanelTests
{
    namespace id = spa::params::id;

    inline void pump (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); }

    inline juce::MouseEvent eventFor (juce::Component& target, juce::Point<float> local, juce::Point<float> downLocal,
                                      bool down, int clicks, bool dragged)
    {
        auto src = juce::Desktop::getInstance().getMainMouseSource();
        const auto now = juce::Time::getCurrentTime();
        const auto mods = down ? juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier) : juce::ModifierKeys();
        return juce::MouseEvent (src, local, mods, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &target, &target, now, downLocal, now, clicks, dragged);
    }

    inline void realClick (juce::Component& editor, juce::Point<float> pos)
    {
        auto* target = editor.getComponentAt (pos.roundToInt());
        if (target == nullptr) return;
        const auto local = target->getLocalPoint (&editor, pos);
        target->mouseEnter (eventFor (*target, local, local, false, 0, false));
        target->mouseDown (eventFor (*target, local, local, true, 1, false));
        target->mouseUp (eventFor (*target, local, local, false, 1, false));
        pump (15);
    }

    // Hit-test at `from`, press there, move through the path in steps (each event delivered
    // to the component the hit test returned, like the peer does for a captured drag),
    // release at `to`.
    inline void realDrag (juce::Component& editor, juce::Point<float> from, juce::Point<float> to)
    {
        auto* target = editor.getComponentAt (from.roundToInt());
        if (target == nullptr) return;
        const auto start = target->getLocalPoint (&editor, from);
        target->mouseEnter (eventFor (*target, start, start, false, 0, false));
        target->mouseDown (eventFor (*target, start, start, true, 1, false));
        for (int i = 1; i <= 6; ++i)
        {
            const auto p = from + (to - from) * ((float) i / 6.0f);
            target->mouseDrag (eventFor (*target, target->getLocalPoint (&editor, p), start, true, 1, true));
        }
        target->mouseUp (eventFor (*target, target->getLocalPoint (&editor, to), start, false, 1, true));
        pump (15);
    }

    inline void realDoubleClick (juce::Component& editor, juce::Point<float> pos)
    {
        auto* target = editor.getComponentAt (pos.roundToInt());
        if (target == nullptr) return;
        const auto local = target->getLocalPoint (&editor, pos);
        target->mouseDown (eventFor (*target, local, local, true, 1, false));
        target->mouseUp (eventFor (*target, local, local, false, 1, false));
        target->mouseDown (eventFor (*target, local, local, true, 2, false));
        target->mouseDoubleClick (eventFor (*target, local, local, true, 2, false));
        target->mouseUp (eventFor (*target, local, local, false, 2, false));
        pump (15);
    }

    inline juce::Point<float> centreIn (juce::Component& editor, juce::Component& c)
    {
        return editor.getLocalArea (&c, c.getLocalBounds()).getCentre().toFloat();
    }

    inline juce::TabbedComponent* findFxTabs (juce::Component& root)
    {
        if (auto* t = dynamic_cast<juce::TabbedComponent*> (&root))
            if (t->getTabNames().contains ("LIMIT")) return t;
        for (auto* c : root.getChildren())
            if (auto* f = findFxTabs (*c)) return f;
        return nullptr;
    }

    void limiterPanelControlsTest()
    {
        std::cout << "limiterPanelControlsTest\n";
        phase3Tests::ForceLive live;
        spa::SPAStripProcessor proc;
        proc.prepareToPlay (48000.0, 512);
        std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
        editor->setSize (spa::ui::metrics::baseWidth, spa::ui::metrics::baseHeight);
        editor->addToDesktop (0);
        editor->setVisible (true);
        pump (150);
        auto* tabs = findFxTabs (*editor);
        expect (tabs != nullptr, "fxTabs found");
        if (tabs == nullptr) return;
        const int index = tabs->getTabNames().indexOf ("LIMIT");
        tabs->setCurrentTabIndex (index);
        pump (80);
        auto* panel = dynamic_cast<spa::ui::LimiterPanel*> (tabs->getTabContentComponent (index));
        expect (panel != nullptr, "LIMIT panel found");
        if (panel == nullptr) return;

        auto& apvts = proc.getAPVTS();
        const auto val = [&] (const char* pid) { return apvts.getRawParameterValue (pid)->load(); };
        auto& mx = panel->getMaximizer();
        auto& thrLane = mx.getThresholdLane();
        auto& ceilLane = mx.getCeilingLane();
        const auto yIn = [&] (juce::Component& lane, float db)
        {
            return editor->getLocalArea (&lane, juce::Rectangle<int> (0, (int) std::round (spa::ui::LimiterTwinSlider::handleY (lane, db)),
                                                                      lane.getWidth(), 1)).getCentre().toFloat();
        };
        const auto hitIs = [&] (juce::Component& c, const juce::String& what)
        {
            const auto pt = centreIn (*editor, c);
            auto* hit = editor->getComponentAt (pt.roundToInt());
            expect (hit != nullptr && (hit == &c || c.isParentOf (hit)), what + " is the hit target at its centre");
        };
        hitIs (thrLane, "THRESHOLD lane"); hitIs (ceilLane, "CEILING lane"); hitIs (mx.getLinkToggle(), "LINK icon");
        hitIs (panel->getReleaseKnob(), "RELEASE knob"); hitIs (panel->getLinkKnob(), "STEREO LINK knob");
        hitIs (panel->getCharacter(), "character combo");
        hitIs (panel->getAutoRel(), "AUTO REL toggle"); hitIs (panel->getTruePeak(), "TRUE PK toggle");
        hitIs (panel->getLookahead(), "LOOK AHEAD toggle"); hitIs (panel->getAutoGain(), "AUTO GAIN toggle");

        expect (std::abs (mx.getThresholdDb()) < 0.01f && std::abs (mx.getCeilingDb() + 0.3f) < 0.01f, "defaults read 0.0 / -0.3 dB");
        expect (! mx.isLinked(), "LINK is off by default");

        // Unlinked: dragging the THRESHOLD handle DOWN raises DRIVE; ceiling untouched.
        realDrag (*editor, yIn (thrLane, 0.0f), yIn (thrLane, -12.0f));
        expect (std::abs (val (id::fx::limDrive) - 12.0f) < 0.3f, "dragging THRESHOLD to -12 dB sets drive to ~12 dB (got " + juce::String (val (id::fx::limDrive), 2) + ")");
        expect (std::abs (mx.getThresholdDb() + val (id::fx::limDrive)) < 0.01f, "threshold == -drive exactly");
        expect (std::abs (val (id::fx::limCeiling) + 0.3f) < 0.01f, "ceiling untouched by a threshold drag when unlinked");
        realDrag (*editor, yIn (thrLane, -12.0f), yIn (thrLane, 0.0f) - juce::Point<float> (0.0f, 30.0f));
        expect (val (id::fx::limDrive) < 0.01f, "THRESHOLD clamps at 0 dB (drive 0)");

        realDrag (*editor, yIn (ceilLane, -0.3f), yIn (ceilLane, -6.0f));
        expect (std::abs (val (id::fx::limCeiling) + 6.0f) < 0.3f, "dragging CEILING to -6 dB sets the ceiling (got " + juce::String (val (id::fx::limCeiling), 2) + ")");
        expect (val (id::fx::limDrive) < 0.01f, "drive untouched by a ceiling drag when unlinked");
        realDrag (*editor, yIn (ceilLane, -6.0f), yIn (ceilLane, -24.0f));
        expect (val (id::fx::limCeiling) >= -12.01f && val (id::fx::limCeiling) < -11.7f, "CEILING clamps at its -12 dB floor");

        realDoubleClick (*editor, yIn (ceilLane, -12.0f));
        expect (std::abs (val (id::fx::limCeiling) + 0.3f) < 0.01f, "double-click resets CEILING to -0.3 dB");

        auto& lk = mx.getLinkToggle();
        realClick (*editor, centreIn (*editor, lk));
        expect (mx.isLinked() && mx.getLinkIsOn(), "real click turns LINK on");
        expect ((bool) apvts.state.getProperty ("uiLimiterLink", false), "LINK is kept as a state property");

        realDrag (*editor, yIn (thrLane, 0.0f), yIn (thrLane, -6.0f));
        expect (std::abs (val (id::fx::limDrive) - 6.0f) < 0.3f, "linked: threshold drag sets drive ~6");
        expect (std::abs (val (id::fx::limCeiling) - (-0.3f - val (id::fx::limDrive))) < 0.15f, "linked: ceiling moved by the same dB (ceiling " + juce::String (val (id::fx::limCeiling), 2) + ")");
        const float gap = mx.getCeilingDb() - mx.getThresholdDb();
        realDrag (*editor, yIn (thrLane, -6.0f), yIn (thrLane, -24.0f));
        expect (std::abs ((mx.getCeilingDb() - mx.getThresholdDb()) - gap) < 0.15f, "linked: the threshold-ceiling gap is preserved at the clamp");
        expect (val (id::fx::limCeiling) >= -12.01f, "linked: ceiling stays inside its range");
        const float t0 = mx.getThresholdDb();
        realDrag (*editor, yIn (ceilLane, mx.getCeilingDb()), yIn (ceilLane, mx.getCeilingDb() + 3.0f));
        expect (mx.getThresholdDb() > t0 + 2.0f, "linked: CEILING drag also moves THRESHOLD");
        realClick (*editor, centreIn (*editor, lk));
        expect (! mx.isLinked(), "second click turns LINK off");

        struct T { spa::ui::Toggle& tog; const char* pid; };
        for (auto t : { T { panel->getAutoRel(), id::fx::limAutoRelease }, T { panel->getTruePeak(), id::fx::limTruePeak },
                        T { panel->getLookahead(), id::fx::limLookahead }, T { panel->getAutoGain(), id::fx::limAutoGain } })
        {
            const auto c = centreIn (*editor, t.tog);
            realClick (*editor, c);
            expect (val (t.pid) > 0.5f, juce::String (t.pid) + ": real click turns it on");
            realClick (*editor, c);
            expect (val (t.pid) < 0.5f, juce::String (t.pid) + ": second click turns it off");
        }
        editor->removeFromDesktop();
    }

    void limiterMetersTest()
    {
        std::cout << "limiterMetersTest\n";
        phase3Tests::ForceLive live;
        spa::SPAStripProcessor proc;
        proc.prepareToPlay (48000.0, 512);
        setParam (proc, id::fx::limEnable, 1.0f);
        setParam (proc, id::fx::limDrive, 12.0f);
        setParam (proc, id::fx::limCeiling, -6.0f);
        std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
        editor->setSize (spa::ui::metrics::baseWidth, spa::ui::metrics::baseHeight);
        editor->addToDesktop (0);
        editor->setVisible (true);
        pump (150);
        auto* tabs = findFxTabs (*editor);
        if (tabs == nullptr) { expect (false, "tabs"); return; }
        const int index = tabs->getTabNames().indexOf ("LIMIT");
        tabs->setCurrentTabIndex (index);
        pump (80);
        auto* panel = dynamic_cast<spa::ui::LimiterPanel*> (tabs->getTabContentComponent (index));
        if (panel == nullptr) { expect (false, "LIMIT panel"); return; }

        auto& tel = proc.getTelemetry();
        const auto feed = [&] (float level)
        {
            juce::AudioBuffer<float> buf (2, 512);
            juce::MidiBuffer midi;
            for (int i = 0; i < 512; ++i)
            {
                const float v = level * (float) std::sin (6.2831853 * 220.0 * (double) i / 48000.0);
                buf.setSample (0, i, v); buf.setSample (1, i, v);
            }
            proc.processBlock (buf, midi);
        };
        for (int i = 0; i < 40; ++i) feed (0.5f);
        const float inPk = tel.limInPeak.load(), outPk = tel.limOutPeak.load();
        expect (inPk > 0.0f && outPk > 0.0f, "limiter input/output peak telemetry is live (" + juce::String (inPk, 3) + " / " + juce::String (outPk, 3) + ")");
        expect (outPk <= juce::Decibels::decibelsToGain (-6.0f) * 1.001f, "limiter output peak respects the -6 dB ceiling");
        expect (inPk > 0.4f && inPk < 0.6f, "IN peak is the signal before DRIVE (" + juce::String (inPk, 3) + ")");
        pump (200);
        expect (panel->getMaximizer().getOutMeter().getShownForTest() > 0.0f, "OUT meter follows the telemetry");

        setParam (proc, id::fx::limEnable, 0.0f);
        for (int i = 0; i < 4; ++i) feed (0.5f);
        expect (juce::exactlyEqual (tel.limInPeak.load(), 0.0f) && juce::exactlyEqual (tel.limOutPeak.load(), 0.0f),
                "meters read 0 while the limiter is off");

        // Idle: a flat, silent ring must not repaint the scrolling display.
        for (int i = 0; i < 600; ++i) feed (0.0f);
        pump (200);
        const int before = panel->getDisplay().getRepaintRequestsForTest();
        for (int i = 0; i < 20; ++i) feed (0.0f);
        pump (300);
        expect (panel->getDisplay().getRepaintRequestsForTest() == before, "flat silent limiter display does not repaint while idle");
        editor->removeFromDesktop();
    }

    // The limiter's DRIVE/CEILING parameters are still reachable by MIDI Learn / paramID lookups
    // (the lanes carry the property), and the old knobs are gone from the panel.
    void limiterLanesCarryParamIdsTest()
    {
        std::cout << "limiterLanesCarryParamIdsTest\n";
        phase3Tests::ForceLive live;
        spa::SPAStripProcessor proc;
        proc.prepareToPlay (48000.0, 512);
        std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
        editor->setSize (spa::ui::metrics::baseWidth, spa::ui::metrics::baseHeight);
        editor->addToDesktop (0);
        editor->setVisible (true);
        pump (150);
        auto* tabs = findFxTabs (*editor);
        if (tabs == nullptr) { expect (false, "tabs"); return; }
        const int index = tabs->getTabNames().indexOf ("LIMIT");
        tabs->setCurrentTabIndex (index);
        pump (80);
        auto* panel = dynamic_cast<spa::ui::LimiterPanel*> (tabs->getTabContentComponent (index));
        if (panel == nullptr) { expect (false, "LIMIT panel"); return; }
        auto& mx = panel->getMaximizer();
        expect (mx.getThresholdLane().getProperties()["paramID"].toString() == id::fx::limDrive, "THRESHOLD lane carries the DRIVE paramID");
        expect (mx.getCeilingLane().getProperties()["paramID"].toString() == id::fx::limCeiling, "CEILING lane carries the CEILING paramID");
        editor->removeFromDesktop();
    }
}
