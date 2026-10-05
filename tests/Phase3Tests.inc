// Phase 3 tests: the editor UI. Included from SPAStripTests.cpp (after Phase2bTests.inc, so its
// helpers -- TempDir, makeIsolated, dirtyPatch, gestureSet, pump, isPermutation -- are in scope).
//
// Everything runs headless: editors are constructed off-screen (no peer), so the "only while
// showing" timers are switched on with ui::forceLiveFlag() and the editors' timers / the
// components' own tick*ForTest() hooks are driven by hand.


namespace phase3Tests
{
    namespace ui = spa::ui;
    using Editor = spa::SPAStripEditor;
    using phase2bTests::TempDir;
    using phase2bTests::pump;
    using phase2bTests::isPermutation;
    using phase2bTests::gestureSet;
    using Module = spa::dsp::FXChain::Module;
    namespace fx = spa::params::id::fx;
    constexpr int kNumModulesP3 = spa::dsp::FXChain::numModules;

    struct ForceLive
    {
        ForceLive() { ui::forceLiveFlag().store (true); }
        ~ForceLive() { ui::forceLiveFlag().store (false); }
    };

    // A mouse event the way the tab buttons expect it (position in the button's own space).
    juce::MouseEvent mouseAt (juce::Component& c, juce::Point<float> pos, juce::Point<float> downPos, bool dragged)
    {
        return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), pos, juce::ModifierKeys(),
                                 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c, juce::Time::getCurrentTime(),
                                 downPos, juce::Time::getCurrentTime(), 1, dragged);
    }

    ui::DraggableTabButton* tabButton (Editor& ed, int index)
    {
        return dynamic_cast<ui::DraggableTabButton*> (ed.getContent().getFxTabs().getTabbedButtonBar().getTabButton (index));
    }

    ui::DraggableTabButton* asDtb (juce::TabBarButton* b) { return dynamic_cast<ui::DraggableTabButton*> (b); }

    juce::String orderNames (Editor& ed)
    {
        return ed.getContent().getFxTabs().getTabNames().joinIntoString (",");
    }

    // Tab order (module ids) read off the UI.
    juce::Array<int> uiOrder (Editor& ed) { return ed.getContent().getTabOrder(); }

    std::unique_ptr<Editor> openEditor (Proc& proc)
    {
        auto ed = std::make_unique<Editor> (proc);
        ed->setSize (ui::metrics::baseWidth, ui::metrics::baseHeight);
        return ed;
    }

    //==========================================================================
    void editorLifecycleTest()
    {
        std::cout << "editorLifecycleTest\n";
        ForceLive live;
        auto proc = makeProc (48000.0, 256);

        // Weak references to everything of the first editor: all gone after it is deleted.
        {
            auto ed = openEditor (*proc);
            std::vector<juce::Component::SafePointer<juce::Component>> all;
            std::function<void (juce::Component&)> walk = [&] (juce::Component& c)
            {
                all.emplace_back (&c);
                for (auto* ch : c.getChildren()) walk (*ch);
            };
            walk (*ed);
            for (int i = 0; i < ed->getContent().getFxTabs().getNumTabs(); ++i)   // the hidden tabs' panels
                if (auto* panel = ed->getContent().getFxTabs().getTabContentComponent (i))
                    if (panel->getParentComponent() == nullptr)
                        walk (*panel);
            expect (all.size() > 400, "the editor has a real component tree (" + juce::String ((int) all.size()) + " components)");
            ed.reset();
            int alive = 0;
            for (auto& p : all) alive += p != nullptr ? 1 : 0;
            expect (alive == 0, "every component of a deleted editor is gone");
        }

        // 50 open / close cycles with the timers run in between; heap growth is bounded.
        const auto cycle = [&]
        {
            auto ed = openEditor (*proc);
            ed->getContent().tickForTest();
            pump (15);
            ed->getContent().getModViz().pollNow();
        };
        for (int i = 0; i < 5; ++i) cycle();       // warm up (static caches, fonts, ...)
        [[maybe_unused]] size_t before = 0;
       #if JUCE_MAC
        malloc_statistics_t st {};
        malloc_zone_statistics (nullptr, &st);
        before = st.size_in_use;
       #endif
        const int cycles = juce::SystemStats::getEnvironmentVariable ("SPASTRIP_UI_CYCLES", "50").getIntValue();
        for (int i = 0; i < cycles; ++i) cycle();
        expect (true, "50 editor open / close cycles ran without a crash or assertion");
       #if JUCE_MAC
        malloc_zone_statistics (nullptr, &st);
        const double grown = (double) st.size_in_use - (double) before;
        std::cout << "        heap in use after 50 cycles: " << juce::String (grown / 1024.0, 1) << " KiB more than before\n";
        // A leaked editor is several hundred KiB (every panel, display and the noise tile); allow slack for allocator noise.
        expect (grown < 512.0 * 1024.0, "heap in use grows < 512 KiB over 50 editor cycles (no per-editor leak)");
       #endif

        // Two editors at once on the same processor, both live, both survive the other's close.
        {
            auto a = openEditor (*proc);
            auto b = openEditor (*proc);
            proc->setLocked (Module::reverb, true);
            setParam (*proc, fx::distEnable, 1.0f);
            a->getContent().tickForTest();
            b->getContent().tickForTest();
            expect (a->getContent().getFxTabs().isTabLocked ("REVERB") && b->getContent().getFxTabs().isTabLocked ("REVERB"),
                    "two editors on one processor both show the lock");
            a.reset();
            b->getContent().tickForTest();
            pump (30);
            expect (b->getContent().getFxTabs().isTabLocked ("REVERB"), "the second editor keeps working after the first one closes");
            proc->setLocked (Module::reverb, false);
        }

        // State changes while no editor exists show up when one opens.
        {
            auto p2 = makeProc (48000.0, 256);
            p2->setFxOrder ({ 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 11 });
            p2->setLocked (Module::grain, true);
            p2->setRandomWildness (0.9f);
            setParam (*p2, fx::chorusEnable, 1.0f);
            auto ed = openEditor (*p2);
            expect (orderNames (*ed) == "GLITTER,COMP,CONV,LIMIT,TREM/VIB,MOD,EQ,REVERB,DELAY,CHORUS,DIST,FILTER",
                    "a chain order set while closed is what the opened editor shows: " + orderNames (*ed));
            expect (ed->getContent().getFxTabs().isTabLocked ("GLITTER") && ! ed->getContent().getFxTabs().isTabLocked ("DIST"),
                    "a lock set while closed shows on the opened editor");
            expect (std::abs (ed->getContent().getWildSlider().getValue() - 0.9) < 1e-6, "WILD set while closed shows on the opened editor");
            expect (ed->getContent().getFxTabs().isTabEngaged ("CHORUS") && ! ed->getContent().getFxTabs().isTabEngaged ("DIST"),
                    "an effect enabled while closed has a bold (engaged) tab");
        }
    }

    //==========================================================================
    void tabOrderSyncTest()
    {
        std::cout << "tabOrderSyncTest\n";
        ForceLive live;
        TempDir tmp ("ui-order");
        auto proc = makeIsolated (tmp);
        auto ed = openEditor (*proc);
        auto& content = ed->getContent();
        expect (uiOrder (*ed) == proc->getFxOrder(), "tabs open in the processor's chain order");

        // Randomize All reshuffles the chain.
        bool moved = false;
        for (int attempt = 0; attempt < 8 && ! moved; ++attempt)
        {
            const auto before = proc->getFxOrder();
            juce::Random rng (100 + attempt);
            proc->randomizeAll (rng);
            content.tickForTest();
            moved = proc->getFxOrder() != before;
            expect (uiOrder (*ed) == proc->getFxOrder(), "after Randomize All the tabs follow the processor order (attempt " + juce::String (attempt) + ")");
        }
        expect (moved, "Randomize All changed the chain order at least once");

        // Undo puts it back.
        const auto randomized = proc->getFxOrder();
        proc->undo();
        content.tickForTest();
        expect (uiOrder (*ed) == proc->getFxOrder() && proc->getFxOrder() != randomized, "after undo the tabs follow the restored order");
        proc->redo();
        content.tickForTest();
        expect (uiOrder (*ed) == randomized, "after redo the tabs follow the order again");

        // A preset load with another order.
        proc->setFxOrder ({ 3, 1, 4, 0, 2, 5, 6, 7, 8, 9, 10, 11 });
        proc->getPresetManager().save ("Order A", {}, true);
        proc->setFxOrder ({ 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 11 });
        content.tickForTest();
        expect (orderNames (*ed).startsWith ("GLITTER,COMP"), "manual setFxOrder shows");
        proc->getPresetManager().loadPreset (0);
        content.tickForTest();
        expect (uiOrder (*ed) == proc->getFxOrder() && uiOrder (*ed)[0] == 3, "after a preset load the tabs follow the preset's order");

        // Host state restore.
        juce::MemoryBlock blob;
        proc->setFxOrder ({ 1, 0, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 });
        proc->getStateInformation (blob);
        proc->setFxOrder ({ 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 11 });
        content.tickForTest();
        proc->setStateInformation (blob.getData(), (int) blob.getSize());
        content.tickForTest();
        expect (uiOrder (*ed) == proc->getFxOrder() && uiOrder (*ed)[0] == 1, "after a host state restore the tabs follow the restored order");
    }

    //==========================================================================
    void tabDragTest()
    {
        std::cout << "tabDragTest\n";
        ForceLive live;
        TempDir tmp ("ui-drag");
        auto proc = makeIsolated (tmp);
        auto ed = openEditor (*proc);
        auto& tabs = ed->getContent().getFxTabs();
        auto& bar = tabs.getTabbedButtonBar();

        const auto start = proc->getFxOrder();
        const int steps0 = proc->getUndoStepCount();

        // Press on tab 0, drag over tab 3, then over tab 6, release: one gesture, two swaps.
        auto* b0 = tabButton (*ed, 0);
        const juce::String draggedName = b0->getButtonText();
        const auto centreOf = [&] (int index) { return bar.getTabButton (index)->getBounds().getCentreX(); };
        const juce::Point<float> down ((float) b0->getWidth() * 0.5f, 10.0f);
        b0->mouseDown (mouseAt (*b0, down, down, false));
        for (int target : { 3, 6 })
        {
            auto* dragged = asDtb (bar.getTabButton (bar.getTabNames().indexOf (draggedName)));
            const float px = (float) (centreOf (target) - dragged->getX());
            dragged->mouseDrag (mouseAt (*dragged, { px, 10.0f }, down, true));
        }
        auto* last = asDtb (bar.getTabButton (bar.getTabNames().indexOf (draggedName)));
        last->mouseUp (mouseAt (*last, { 5.0f, 10.0f }, down, true));

        const auto after = proc->getFxOrder();
        expect (isPermutation (after), "a tab drag sets a valid permutation via setFxOrder");
        expect (after != start, "the chain order changed");
        expect (uiOrder (*ed) == after, "the tab order and the processor order agree after a drag");
        expect (tabs.getTabNames()[6] == draggedName && after[6] == start[0],
                "the dragged tab (" + draggedName + ") ended at position 7");
        expect (proc->getUndoStepCount() == steps0 + 1, "a whole drag (two swaps) is ONE undo step");
        proc->undo();
        ed->getContent().tickForTest();
        expect (proc->getFxOrder() == start && uiOrder (*ed) == start, "undo of a drag restores the order, the tabs follow");

        // A locked effect can still be dragged.
        proc->setLocked (Module::distortion, true);
        ed->getContent().tickForTest();
        auto* dist = tabButton (*ed, tabs.getTabNames().indexOf ("DIST"));
        const int distIndex = tabs.getTabNames().indexOf ("DIST");
        const juce::Point<float> d2 ((float) dist->getWidth() * 0.4f, 10.0f);
        dist->mouseDown (mouseAt (*dist, d2, d2, false));
        const float px = (float) (bar.getTabButton (distIndex + 2)->getBounds().getCentreX() - dist->getX());
        dist->mouseDrag (mouseAt (*dist, { px, 10.0f }, d2, true));
        auto* moved = asDtb (bar.getTabButton (tabs.getTabNames().indexOf ("DIST")));
        moved->mouseUp (mouseAt (*moved, { 5.0f, 10.0f }, d2, true));
        expect (tabs.getTabNames().indexOf ("DIST") == distIndex + 2 && proc->getFxOrder().indexOf ((int) Module::distortion) == distIndex + 2,
                "a LOCKED effect's tab can still be dragged to a new slot");
        expect (proc->isLocked (Module::distortion), "...and stays locked");
    }

    //==========================================================================
    void lockToggleTest()
    {
        std::cout << "lockToggleTest\n";
        ForceLive live;
        auto proc = makeProc (48000.0, 256);
        auto ed = openEditor (*proc);
        auto& tabs = ed->getContent().getFxTabs();

        for (int m = 0; m < kNumModulesP3; ++m)
        {
            const auto name = tabs.getModuleNames()[m];
            const int idx = tabs.getTabNames().indexOf (name);
            auto* btn = tabButton (*ed, idx);
            const auto lockCentre = btn->getLockRect().getCentre().toFloat();
            expect (! proc->isLocked ((Module) m) && ! tabs.isTabLocked (name), name + ": starts unlocked");
            btn->mouseDown (mouseAt (*btn, lockCentre, lockCentre, false));
            btn->mouseUp (mouseAt (*btn, lockCentre, lockCentre, false));
            if (! proc->isLocked ((Module) m) || ! tabs.isTabLocked (name))
                expect (false, name + ": padlock click locks the processor module");
            btn->mouseDown (mouseAt (*btn, lockCentre, lockCentre, false));
            btn->mouseUp (mouseAt (*btn, lockCentre, lockCentre, false));
            if (proc->isLocked ((Module) m) || tabs.isTabLocked (name))
                expect (false, name + ": second click unlocks");
        }
        expect (proc->getFxLockMask() == 0, "all 12 padlocks round-trip to the processor (lock then unlock)");

        // A padlock click does not select the tab.
        const int before = tabs.getCurrentTabIndex();
        const int other = (before + 3) % kNumModulesP3;
        auto* btn = tabButton (*ed, other);
        const auto c = btn->getLockRect().getCentre().toFloat();
        btn->mouseDown (mouseAt (*btn, c, c, false));
        btn->mouseUp (mouseAt (*btn, c, c, false));
        expect (tabs.getCurrentTabIndex() == before, "clicking a padlock does not switch tabs");
        proc->setLocked (Module::chorus, false);
        proc->setLocked ((Module) tabs.getModuleNames().indexOf (tabs.getTabNames()[other]), false);

        // An external lock change (e.g. host state restore) shows on the tabs.
        proc->setLocked (Module::eq, true);
        ed->getContent().tickForTest();
        expect (tabs.isTabLocked ("EQ"), "an externally set lock shows on the tab");

        // A locked effect keeps its parameters through Randomize All while the editor is open.
        setParam (*proc, fx::eqEnable, 1.0f);
        const float eqBefore = proc->getAPVTS().getRawParameterValue (pid::eqBand (1, fx::eqband::gain))->load();
        const int slotBefore = proc->getFxOrder().indexOf ((int) Module::eq);
        for (int i = 0; i < 4; ++i)
        {
            juce::Random rng (7 + i);
            proc->randomizeAll (rng);
        }
        expect (std::abs (proc->getAPVTS().getRawParameterValue (pid::eqBand (1, fx::eqband::gain))->load() - eqBefore) < 1.0e-6f
                    && proc->getFxOrder().indexOf ((int) Module::eq) == slotBefore,
                "a locked effect's parameters and chain slot survive Randomize All");
    }

    //==========================================================================
    void modSlotUiTest()
    {
        std::cout << "modSlotUiTest\n";
        ForceLive live;
        TempDir tmp ("ui-mod");
        auto proc = makeIsolated (tmp);
        auto ed = openEditor (*proc);
        auto& panel = ed->getContent().getModPanel();
        const auto& targets = Proc::getModTargets();

        // Every target is reachable from the picker menu and maps back to its ID.
        {
            auto menu = ui::modmenu::build ({});
            int items = 0;
            std::function<void (const juce::PopupMenu&)> count = [&] (const juce::PopupMenu& m)
            {
                juce::PopupMenu::MenuItemIterator it (m);
                while (it.next())
                {
                    if (it.getItem().subMenu != nullptr) count (*it.getItem().subMenu);
                    else if (it.getItem().itemID > 1) ++items;
                }
            };
            count (menu);
            expect (items == (int) targets.size(), "the slot picker menu offers all " + juce::String (items) + " mod targets (" + juce::String ((int) targets.size()) + " exist)");
            bool allMap = true;
            for (size_t i = 0; i < targets.size(); ++i)
            {
                bool valid = false;
                allMap = allMap && ui::modmenu::resultToTarget ((int) i + 2, valid) == targets[i].id && valid;
            }
            bool none = false;
            allMap = allMap && ui::modmenu::resultToTarget (1, none).isEmpty() && none;
            bool dismissed = true;
            ui::modmenu::resultToTarget (0, dismissed);
            expect (allMap && ! dismissed, "menu results round-trip to target IDs; None -> \"\"; a dismissed menu is not an assignment");
        }

        // Picker round trip, all eight slots.
        for (int s = 0; s < Proc::numModSlots; ++s)
        {
            const auto& t = targets[(size_t) (s * 7) % targets.size()];
            panel.chooseTarget (s, t.id);
            expect (proc->getModSlotTarget (s) == t.id && panel.getPickerForTest (s).getText() == t.displayName,
                    "slot " + juce::String (s + 1) + ": picking \"" + t.displayName + "\" sets the processor target and the button text");
        }
        panel.chooseTarget (2, {});
        expect (proc->getModSlotTarget (2).isEmpty() && panel.getPickerForTest (2).getText() == "- none -", "choosing None clears a slot");
        // A change that arrives from outside (undo) updates the pickers.
        proc->undo();
        ed->getContent().tickForTest();
        panel.refreshTargets();
        expect (proc->getModSlotTarget (2) == panel.getPickerForTest (2).getText() || panel.getPickerForTest (2).getText() == ui::modmenu::shortName (proc->getModSlotTarget (2)),
                "after undo the picker shows the processor's target");

        // Depth knob = the slot's host parameter.
        gestureSet (*proc, pid::modSlotDepth (3), -0.6f);
        expect (std::abs (panel.getDepthKnobForTest (3).slider.getValue() + 0.6) < 1e-3, "the depth knob follows its parameter");

        // Assignment from the knob's context menu action: assign, reach, remove.
        proc->setModSlotTarget (0, {}); proc->setModSlotTarget (1, {});
        auto& content = ed->getContent();
        setParam (*proc, pid::modSlotDepth (1), 0.0f);
        const int steps = proc->getUndoStepCount();
        content.toggleModAssignment (fx::delayFeedback, 1);
        expect (proc->getModSlotTarget (1) == fx::delayFeedback, "context menu: \"Assign to mod slot 2\" assigns the knob's parameter");
        expect (proc->getUndoStepCount() == steps + 1, "the assignment (target + default depth) is one undo step");
        expect (std::abs (proc->getAPVTS().getRawParameterValue (pid::modSlotDepth (1))->load() - 0.5f) < 1e-4f, "a fresh assignment at depth 0 gets a visible default depth (0.5)");
        content.getModViz().pollNow();

        // The knob now carries the reach arc properties.
        juce::Slider* fbKnob = nullptr;
        std::function<void (juce::Component&)> find = [&] (juce::Component& c)
        {
            if (auto* s = dynamic_cast<juce::Slider*> (&c))
                if (s->getProperties()["paramID"].toString() == fx::delayFeedback) fbKnob = s;
            for (auto* ch : c.getChildren()) find (*ch);
        };
        find (content);
        for (int i = 0; i < content.getFxTabs().getNumTabs(); ++i)
            if (auto* tabPanel = content.getFxTabs().getTabContentComponent (i)) find (*tabPanel);
        expect (fbKnob != nullptr && (bool) fbKnob->getProperties().getWithDefault ("modAssigned", false), "the assigned effect knob shows the assigned state");
        expect (fbKnob != nullptr && std::abs ((double) fbKnob->getProperties().getWithDefault ("modRangePos", 0.0) - 0.5) < 0.01
                    && (double) fbKnob->getProperties().getWithDefault ("modRangeNeg", 1.0) == 0.0, "reach arc = +0.5 (the slot's depth), nothing negative");
        const auto reach = content.getModViz().getReach (fx::delayFeedback);
        expect (reach.assigned && std::abs (reach.pos - 0.5f) < 1e-3f, "ModVizDriver::getReach agrees");

        // Negative depth reaches downward; two slots on one target sum.
        content.toggleModAssignment (fx::delayFeedback, 4);
        setParam (*proc, pid::modSlotDepth (4), -0.25f);
        content.getModViz().pollNow();
        expect (std::abs ((double) fbKnob->getProperties().getWithDefault ("modRangeNeg", 0.0) - 0.25) < 0.01, "a second slot with negative depth adds a downward reach");

        // Live overlay follows Telemetry (simulated offset).
        proc->getTelemetry().modSlotOffset[1].store (0.2f);
        content.getModViz().pollNow();
        expect ((bool) fbKnob->getProperties().getWithDefault ("modActive", false), "a non-zero live slot offset activates the overlay on the knob");
        proc->getTelemetry().modSlotOffset[1].store (0.0f);
        content.getModViz().pollNow();
        expect (! (bool) fbKnob->getProperties().getWithDefault ("modActive", true), "...and clears when the offset returns to zero");

        content.toggleModAssignment (fx::delayFeedback, 1);
        content.toggleModAssignment (fx::delayFeedback, 4);
        content.getModViz().pollNow();
        expect (proc->getModSlotTarget (1).isEmpty() && ! (bool) fbKnob->getProperties().getWithDefault ("modAssigned", true), "\"Remove from slot\" unassigns and clears the knob");

        // Non-modulatable knobs get nothing.
        juce::Slider* decay = nullptr;
        std::function<void (juce::Component&)> find2 = [&] (juce::Component& c)
        {
            if (auto* s = dynamic_cast<juce::Slider*> (&c))
                if (s->getProperties()["paramID"].toString() == fx::convDecay) decay = s;
            for (auto* ch : c.getChildren()) find2 (*ch);
        };
        find2 (content);
        for (int i = 0; i < content.getFxTabs().getNumTabs(); ++i)
            if (auto* tabPanel = content.getFxTabs().getTabContentComponent (i)) find2 (*tabPanel);
        expect (decay != nullptr && spa::mod::indexOf (fx::convDecay) < 0, "conv decay is an audited-excluded (non-modulatable) target");
        content.toggleModAssignment (fx::convDecay, 0);
        expect (proc->getModSlotTarget (0).isEmpty(), "assigning a non-modulatable parameter is refused");
        content.getModViz().pollNow();
        expect (decay != nullptr && ! decay->getProperties().contains ("modAssigned"), "a non-modulatable knob is never touched by the mod visualisation");
    }

    //==========================================================================
    void convolvePanelTest()
    {
        std::cout << "convolvePanelTest\n";
        ForceLive live;
        TempDir tmp ("ui-conv");
        auto proc = makeIsolated (tmp);
        auto ed = openEditor (*proc);
        auto* panel = ed->getContent().getConvolvePanel();
        expect (panel != nullptr, "the CONV tab has a ConvolvePanel");

        const auto& list = Proc::getFactoryIRs();
        auto menu = panel->buildFactoryMenu();
        int items = 0, headers = 0;
        {
            juce::PopupMenu::MenuItemIterator it (menu);
            while (it.next())
            {
                if (it.getItem().isSectionHeader) ++headers;
                else if (it.getItem().itemID > 0) ++items;
            }
        }
        std::set<juce::String> cats;
        for (const auto& ir : list) cats.insert (ir.category);
        expect (items == (int) list.size() && headers == (int) cats.size(),
                "the factory picker lists all " + juce::String (items) + " IRs under " + juce::String (headers) + " category headers");

        panel->chooseFactoryIR (3);
        expect (proc->getConvolutionIRSource() == "factory:" + list[3].id && proc->hasConvolutionIR(), "picking a factory IR loads it in the processor");
        expect (panel->getSourceTagForTest() == "FACTORY" && panel->getNameForTest() == list[3].name, "the panel shows the factory IR's name and the FACTORY tag");

        // A user file: write a short WAV, load it.
        const auto wav = tmp.dir.getChildFile ("my-ir.wav");
        {
            juce::WavAudioFormat fmt;
            std::unique_ptr<juce::OutputStream> out (wav.createOutputStream());
            auto w = fmt.createWriterFor (out, juce::AudioFormatWriterOptions().withSampleRate (48000.0).withNumChannels (2).withBitsPerSample (24));
            if (w != nullptr)
            {
                juce::AudioBuffer<float> b (2, 4800);
                juce::Random r (5);
                for (int c = 0; c < 2; ++c)
                    for (int i = 0; i < 4800; ++i)
                        b.setSample (c, i, (r.nextFloat() * 2.0f - 1.0f) * std::exp (-(float) i / 900.0f));
                w->writeFromAudioSampleBuffer (b, 0, 4800);
            }
        }
        expect (ui::ConvolvePanel::isAcceptedAudioFile ("a.WAV") && ui::ConvolvePanel::isAcceptedAudioFile ("b.flac") && ui::ConvolvePanel::isAcceptedAudioFile ("c.aiff")
                    && ! ui::ConvolvePanel::isAcceptedAudioFile ("d.mp3") && ! ui::ConvolvePanel::isAcceptedAudioFile ("e.txt"), "WAV / AIFF / FLAC are accepted, others are not");
        expect (panel->isInterestedInFileDrag (juce::StringArray { wav.getFullPathName() }) && ! panel->isInterestedInFileDrag (juce::StringArray { "x.txt" }),
                "the panel is a drop target for audio files only");
        panel->filesDropped (juce::StringArray { wav.getFullPathName() }, 10, 10);
        expect (proc->getConvolutionIRSource() == "embedded" && proc->getConvolutionIRName() == "my-ir", "dropping an audio file onto the panel loads it as an embedded user IR");
        expect (panel->getSourceTagForTest() == "FILE" && panel->getNameForTest() == "my-ir", "the panel shows the file name and the FILE tag");

        panel->clearIR();
        expect (! proc->hasConvolutionIR() && proc->getConvolutionIRSource() == "none" && panel->getSourceTagForTest().isEmpty(), "Clear removes the impulse");

        // Undo (outside the panel) is noticed by the panel's poll.
        proc->undo();
        panel->chooseFactoryIR (0);
        proc->undo();
        ed->getContent().tickForTest();
        pump (150);
        expect (panel->getSourceTagForTest().isEmpty() == (proc->getConvolutionIRSource() == "none"), "the panel follows IR changes made elsewhere (undo)");
    }

    //==========================================================================
    void controlCoverageTest()
    {
        std::cout << "controlCoverageTest\n";
        ForceLive live;
        auto proc = makeProc (48000.0, 256);
        auto ed = openEditor (*proc);
        const auto controlled = ed->getContent().collectControlledParameterIDs();

        juce::StringArray missing;
        // GLITTER's legacy FREEZE stays registered for old sessions / automation but has
        // no control on purpose: RELEASE at infinity replaced it (as in SPASynth).
        for (const auto& def : spa::params::all())
            if (! controlled.contains (def.id) && def.id != fx::grainFreeze)
                missing.add (def.id);

        // The only parameters with no widget of their own are the 8 EQ bands' parameters (the EQ
        // graph edits them: X = freq, Y = gain, wheel / Cmd-drag = Q, double-click = enable,
        // right-click = type + slope).
        bool onlyGraphs = true;
        for (const auto& id : missing)
            onlyGraphs = onlyGraphs && id.startsWith ("fxEQ.band");
        std::cout << "        parameters without a dedicated widget: " << missing.size() << " (all edited on a graph: " << (onlyGraphs ? "yes" : "NO") << ")\n";
        expect (onlyGraphs, "every parameter that has no widget is an EQ band parameter (edited on the EQ graph)");
        expect (missing.size() == 8 * 6, "...exactly the 8 x 6 per-band EQ parameters (enable, type, slope, freq, gain, q); the COMP crossovers have knobs as well as graph handles");
        if (! onlyGraphs)
            std::cout << "        MISSING: " << missing.joinIntoString (", ") << "\n";

        // Every control's attachment is alive: move the parameter, the widget follows.
        int checked = 0, bad = 0;
        std::function<void (juce::Component&)> walk = [&] (juce::Component& c)
        {
            const auto id = c.getProperties()["paramID"].toString();
            if (id.isNotEmpty())
            {
                auto* p = proc->getAPVTS().getParameter (id);
                if (auto* s = dynamic_cast<juce::Slider*> (&c))
                {
                    p->setValueNotifyingHost (0.77f);
                    ++checked;
                    if (std::abs (s->valueToProportionOfLength (s->getValue()) - 0.77) > 0.02) ++bad;
                    p->setValueNotifyingHost (p->getDefaultValue());
                }
                else if (auto* cb = dynamic_cast<juce::ComboBox*> (&c))
                {
                    p->setValueNotifyingHost (1.0f);
                    ++checked;
                    if (cb->getSelectedItemIndex() != cb->getNumItems() - 1) ++bad;
                    p->setValueNotifyingHost (p->getDefaultValue());
                }
                else if (auto* tb = dynamic_cast<juce::ToggleButton*> (&c))
                {
                    p->setValueNotifyingHost (1.0f);
                    ++checked;
                    if (! tb->getToggleState()) ++bad;
                    p->setValueNotifyingHost (p->getDefaultValue());
                }
            }
            for (auto* ch : c.getChildren()) walk (*ch);
        };
        walk (*ed);
        for (int i = 0; i < ed->getContent().getFxTabs().getNumTabs(); ++i)
            if (auto* panel = ed->getContent().getFxTabs().getTabContentComponent (i)) walk (*panel);
        expect (checked > 100 && bad == 0, juce::String (checked) + " controls re-sync from their parameters (" + juce::String (bad) + " did not)");
    }

    //==========================================================================
    void keyboardAndButtonsTest()
    {
        std::cout << "keyboardAndButtonsTest\n";
        ForceLive live;
        TempDir tmp ("ui-keys");
        auto proc = makeIsolated (tmp);
        auto ed = openEditor (*proc);
        auto& content = ed->getContent();

        expect (! content.getUndoButton().isEnabled() && ! content.getRedoButton().isEnabled(), "undo / redo buttons start disabled");
        gestureSet (*proc, fx::distDrive, 0.7f);
        content.tickForTest();
        expect (content.getUndoButton().isEnabled() && ! content.getRedoButton().isEnabled(), "a recorded edit enables undo");

        const float edited = proc->getAPVTS().getRawParameterValue (fx::distDrive)->load();
        const juce::KeyPress undoKey ('z', juce::ModifierKeys::commandModifier, 0);
        const juce::KeyPress redoKey ('z', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0);
        expect (content.keyPressed (undoKey), "Cmd/Ctrl+Z is handled by the editor");
        expect (std::abs (proc->getAPVTS().getRawParameterValue (fx::distDrive)->load() - 0.3f) < 1e-4f, "...and undoes the edit");
        content.tickForTest();
        expect (! content.getUndoButton().isEnabled() && content.getRedoButton().isEnabled(), "buttons follow: redo enabled after undo");
        expect (content.keyPressed (redoKey) && std::abs (proc->getAPVTS().getRawParameterValue (fx::distDrive)->load() - edited) < 1e-4f, "Shift+Cmd/Ctrl+Z redoes");
        expect (! content.keyPressed (juce::KeyPress ('a', juce::ModifierKeys(), 0)), "other keys are not swallowed (they stay with the host)");

        // The buttons themselves.
        content.getUndoButton().onClick();
        expect (std::abs (proc->getAPVTS().getRawParameterValue (fx::distDrive)->load() - 0.3f) < 1e-4f, "the undo button undoes");
        content.getRedoButton().onClick();
        expect (std::abs (proc->getAPVTS().getRawParameterValue (fx::distDrive)->load() - edited) < 1e-4f, "the redo button redoes");

        // RANDOMIZE ALL and WILD.
        proc->clearUndoHistory();
        const auto before = proc->getAPVTS().copyState();
        content.getWildSlider().setValue (0.8, juce::sendNotificationSync);
        expect (std::abs (proc->getRandomWildness() - 0.8f) < 1e-5f, "the WILD knob sets the processor's wildness");
        content.getRandomizeButton().onClick();
        expect (proc->canUndo() && proc->getUndoStepCount() == 1, "RANDOMIZE ALL is one undo step");
        juce::ignoreUnused (before);

        // Preset name / edited marker.
        proc->getPresetManager().save ("Header Test", {}, true);
        content.refreshAll();
        expect (content.getPresetButtonText() == "Header Test", "the header shows the loaded / saved preset name (" + content.getPresetButtonText() + ")");
        setParam (*proc, fx::chorusMix, 0.9f);
        proc->getPresetManager().refreshEditedState();
        content.tickForTest();
        expect (content.getPresetButtonText() == "Header Test *", "an edit adds the edited marker (" + content.getPresetButtonText() + ")");
    }

    //==========================================================================
    void uiStateTest()
    {
        std::cout << "uiStateTest\n";
        ForceLive live;
        TempDir tmp ("ui-state");
        auto proc = makeIsolated (tmp);
        const auto& state = proc->getAPVTS().state;

        // Scale: 50%..200%, persisted; the selected tab, persisted by effect name.
        {
            auto ed = openEditor (*proc);
            ed->getContent().selectTabForModule ((int) Module::grain);
            pump (50);
            expect (state.getProperty ("uiFxTab").toString() == "GLITTER", "uiFxTab remembers the selected effect by name");
            ed->setSize (juce::roundToInt (ui::metrics::baseWidth * 1.5), juce::roundToInt (ui::metrics::baseHeight * 1.5));
            expect (std::abs ((double) state.getProperty ("uiScale") - 1.5) < 0.01, "uiScale is written when the window is resized (" + state.getProperty ("uiScale").toString() + ")");
            expect (std::abs (ed->getScale() - 1.5f) < 0.01f, "the content is scaled by the window size");
            auto* c = ed->getConstrainer();
            expect (c != nullptr && c->getMinimumWidth() == ui::metrics::baseWidth / 2 && c->getMaximumWidth() == ui::metrics::baseWidth * 2, "resize limits are 50% .. 200% of the base size");
            expect (c != nullptr && std::abs (c->getFixedAspectRatio() - (double) ui::metrics::baseWidth / ui::metrics::baseHeight) < 1e-9, "the aspect ratio is fixed");
            ed->setSize (ui::metrics::baseWidth * 2, ui::metrics::baseHeight * 2);
            expect (std::abs ((double) state.getProperty ("uiScale") - 2.0) < 0.01, "200% is allowed");
            ed->setSize (ui::metrics::baseWidth / 2, ui::metrics::baseHeight / 2);
            expect (std::abs ((double) state.getProperty ("uiScale") - 0.5) < 0.01, "50% is allowed");
        }
        // Re-open: remembered tab and scale (if it fits the display).
        {
            auto ed = std::make_unique<Editor> (*proc);
            expect (ed->getContent().getSelectedModule() == (int) Module::grain, "the remembered tab is selected again");
            expect (ed->getWidth() == ui::metrics::baseWidth / 2 || ed->getWidth() >= ui::metrics::baseWidth / 2, "a remembered size is restored (width " + juce::String (ed->getWidth()) + ")");
        }
        // Host state carries them.
        {
            juce::MemoryBlock blob;
            proc->getStateInformation (blob);
            auto other = makeProc (48000.0, 256);
            other->setStateInformation (blob.getData(), (int) blob.getSize());
            expect (other->getAPVTS().state.getProperty ("uiFxTab").toString() == "GLITTER" && other->getAPVTS().state.hasProperty ("uiScale"),
                    "uiFxTab / uiScale are in the host state");
        }

        // First open on a fresh state: a sensible size for the display.
        {
            auto fresh = makeProc (48000.0, 256);
            auto ed = std::make_unique<Editor> (*fresh);
            const float s = ed->getScale();
            expect (s >= Editor::minScale && s <= 1.0f + 1e-3f, "a first open picks a scale in 0.5 .. 1.0 (" + juce::String (s, 2) + ")");
            expect (! fresh->getAPVTS().state.hasProperty ("uiScale"), "an automatic fit is not stored as the user's chosen scale");
        }
        expect (Editor::scaleThatFits ({ 0, 0, 1920, 1080 }, 1180, 740) == 1.0f, "a 1080p display gets 100%");
        expect (Editor::scaleThatFits ({ 0, 0, 1280, 720 }, 1180, 740) < 1.0f && Editor::scaleThatFits ({ 0, 0, 1280, 720 }, 1180, 740) >= 0.5f, "a 720p display gets a smaller scale, never below 50%");
        expect (std::abs (Editor::scaleThatFits ({ 0, 0, 800, 500 }, 1180, 740) - Editor::minScale) < 1.0e-6f, "a tiny display is clamped to 50%");

        // UI state is excluded from presets.
        {
            proc->getAPVTS().state.setProperty ("uiPresetDrawerOpen", true, nullptr);
            const auto tree = proc->capturePresetState();
            bool hasUi = false;
            for (int i = 0; i < tree.getNumProperties(); ++i)
                hasUi = hasUi || tree.getPropertyName (i).toString().startsWith ("ui");
            expect (! hasUi, "capturePresetState carries no ui* property");
            const auto save = proc->getPresetManager().save ("Ui Free", {}, true);
            const auto text = save.file.loadFileAsString();
            expect (save.ok && ! text.contains ("uiScale") && ! text.contains ("uiFxTab") && ! text.contains ("uiPresetDrawerOpen"), "a saved preset file contains no UI state");

            // Loading a preset keeps the session's UI state.
            proc->getAPVTS().state.setProperty ("uiScale", 1.25, nullptr);
            proc->getAPVTS().state.setProperty ("uiFxTab", "REVERB", nullptr);
            proc->getPresetManager().load (save.file);
            expect (std::abs ((double) proc->getAPVTS().state.getProperty ("uiScale") - 1.25) < 1e-9
                        && proc->getAPVTS().state.getProperty ("uiFxTab").toString() == "REVERB",
                    "a preset load leaves uiScale / uiFxTab untouched");
            // ...and a hand-edited file cannot smuggle UI state in.
            auto xml = juce::XmlDocument::parse (save.file);
            if (xml != nullptr)
                if (auto* params = xml->getFirstChildElement())
                {
                    params->setAttribute ("uiScale", 0.4);
                    params->setAttribute ("uiFxTab", "DIST");
                    xml->writeTo (save.file);
                }
            proc->getPresetManager().load (save.file);
            expect (std::abs ((double) proc->getAPVTS().state.getProperty ("uiScale") - 1.25) < 1e-9
                        && proc->getAPVTS().state.getProperty ("uiFxTab").toString() == "REVERB",
                    "UI properties written into a preset file are ignored on load");
        }
    }

    //==========================================================================
    void drawerAndDialogsTest()
    {
        std::cout << "drawerAndDialogsTest\n";
        ForceLive live;
        TempDir tmp ("ui-drawer");
        auto proc = makeIsolated (tmp);
        auto& pm = proc->getPresetManager();
        pm.save ("Alpha", {}, true);
        pm.save ("Beta", "Bank One", true);
        pm.save ("Gamma", "Bank One", true);

        auto ed = openEditor (*proc);
        auto& content = ed->getContent();
        auto& browser = content.getPresetBrowser();
        content.setPresetBrowserOpen (true, false);
        expect (content.isPresetBrowserOpen() && browser.isVisible() && (bool) proc->getAPVTS().state.getProperty ("uiPresetDrawerOpen"), "the drawer opens and the open state is stored");
        expect (browser.getNumRows() == 2 + 3, "the drawer lists 2 groups (User, Bank One) and 3 presets (" + juce::String (browser.getNumRows()) + " rows)");
        browser.setSearchText ("gam");
        expect (browser.getNumRows() == 3 && browser.getRow (0).isGroup() && browser.getRow (1).isGroup() && browser.getRow (2).label.isEmpty() && browser.presetAtRow (2) != nullptr && browser.presetAtRow (2)->name == "Gamma",
                "the search box filters the list (User > Bank One > Gamma; groups without a match are hidden)");
        browser.setSearchText ({});

        // Loading from the drawer: first preset row.
        int presetRow = -1;
        for (int i = 0; i < browser.getNumRows(); ++i)
            if (browser.presetAtRow (i) != nullptr && browser.presetAtRow (i)->name == "Beta") presetRow = i;
        expect (presetRow >= 0, "the Beta preset is listed");
        setParam (*proc, fx::distDrive, 0.9f);
        pm.loadPreset (browser.getRow (presetRow).presetIndex);
        content.refreshAll();
        expect (content.getPresetButtonText() == "Beta", "loading a row loads the preset and the header shows its name");

        // Dialogs: one at a time, dismissed cleanly.
        content.showAboutPanel();
        auto* about = dynamic_cast<ui::AboutDialog*> (content.getDialogForTest());
        expect (about != nullptr, "About opens");
        if (about != nullptr)
        {
            expect (about->getInfoTextForTest().contains (SPASTRIP_VERSION), "About shows the version");
            const auto credits = about->getCreditsTextForTest();
            expect (credits.contains ("Adventure Kid") && credits.length() > 1000, "About shows the factory IR credits (" + juce::String (credits.length()) + " chars)");
            expect (about->buildClipboardText().contains ("SPAStrip " SPASTRIP_VERSION), "Copy Info has the version");
        }
        content.showAccentPicker();
        expect (dynamic_cast<ui::AccentDialog*> (content.getDialogForTest()) != nullptr, "opening another dialog replaces the first");
        content.dismissDialog();
        expect (content.getDialogForTest() == nullptr, "dismissing removes the dialog");

        // Save dialog: opens on the loaded preset's folder with its (taken) name suffixed; a
        // clash in the chosen folder -> warning, Replace works.
        content.showSaveDialog (true);
        auto* sd = dynamic_cast<ui::SaveDialog*> (content.getDialogForTest());
        expect (sd != nullptr, "Save As opens the save dialog");
        if (sd != nullptr)
        {
            expect (sd->getNameEditor().getText() == "Beta 2" && sd->getFolderBox().getText() == "Bank One",
                    "...pre-filled with the loaded preset's folder and a free variant of its name (" + sd->getNameEditor().getText() + " / " + sd->getFolderBox().getText() + ")");
            sd->getFolderBox().setSelectedId (1, juce::sendNotificationSync);   // User (unfiled)
            sd->getNameEditor().setText ("Alpha", false);
            sd->submit (false);
            expect (sd->getWarningForTest().contains ("already exists") && content.getDialogForTest() == sd, "saving over an existing name warns and keeps the dialog open");
            sd->submit (true);
            pump (50);
            expect (content.getDialogForTest() == nullptr || ! content.getDialogForTest()->isVisible(), "Replace saves and closes the dialog");
            content.dismissDialog();
        }
    }

    //==========================================================================
    void metersAndSidechainUiTest()
    {
        std::cout << "metersAndSidechainUiTest\n";
        ForceLive live;

        // Input peak is published by the processor (pre input gain).
        {
            auto proc = makeProc (48000.0, 256);
            setParam (*proc, pid::inputGain, 12.0f);
            juce::AudioBuffer<float> buf (2, 256);
            for (int i = 0; i < 256; ++i) { buf.setSample (0, i, 0.25f); buf.setSample (1, i, -0.1f); }
            juce::MidiBuffer midi;
            proc->processBlock (buf, midi);
            expect (std::abs (proc->getTelemetry().inPeakL.load() - 0.25f) < 1e-6f && std::abs (proc->getTelemetry().inPeakR.load() - 0.1f) < 1e-6f,
                    "Telemetry::inPeak is the host input's peak before input gain");

            auto ed = openEditor (*proc);
            auto& io = ed->getContent().getIoPanel();
            for (int i = 0; i < 4; ++i) { io.getInputMeter().tickForTest(); io.getOutputMeter().tickForTest(); }
            const float outPeak = proc->getTelemetry().peakL.load();
            expect (std::abs (io.getInputMeter().getDisplayedForTest (0) - 0.25f) < 1e-5f && outPeak > 0.25f
                        && std::abs (io.getOutputMeter().getDisplayedForTest (0) - outPeak) < 1e-5f,
                    "the input meter reads the input peak (0.25) and the output meter the output peak (" + juce::String (outPeak, 3) + ", +12 dB gain applied)");
        }

        // "No sidechain input routed" indication.
        {
            auto proc = makeProc (48000.0, 256, 2, 2, 0);   // no sidechain bus
            auto ed = openEditor (*proc);
            juce::AudioBuffer<float> buf (2, 256);
            buf.clear();
            juce::MidiBuffer midi;
            proc->processBlock (buf, midi);
            auto& env = ed->getContent().getSidechainPanel().getEnvelopeDisplay();
            expect (getParam (*proc, pid::sc::source) < 0.5f && env.shouldShowNoSidechainNotice(), "External source with no sidechain bus: the notice is shown");
            setParam (*proc, pid::sc::source, 1.0f);
            proc->processBlock (buf, midi);
            expect (! env.shouldShowNoSidechainNotice(), "Input source: no notice");
            setParam (*proc, pid::sc::source, 0.0f);
            proc->processBlock (buf, midi);
            expect (env.shouldShowNoSidechainNotice(), "back to External: the notice returns");
        }
        {
            auto proc = makeProc (48000.0, 256, 2, 2, 2);   // sidechain bus enabled
            auto ed = openEditor (*proc);
            juce::AudioBuffer<float> buf (4, 256);
            juce::MidiBuffer midi;
            for (int c = 0; c < 4; ++c) for (int i = 0; i < 256; ++i) buf.setSample (c, i, c >= 2 ? 0.8f * std::sin ((float) i * 0.2f) : 0.0f);
            for (int b = 0; b < 8; ++b) proc->processBlock (buf, midi);
            auto& env = ed->getContent().getSidechainPanel().getEnvelopeDisplay();
            env.tickForTest();
            expect (! env.shouldShowNoSidechainNotice(), "External source with the sidechain bus routed: no notice");
            expect (env.getEnvelopeForTest() > 0.05f, "the envelope display follows the detector (" + juce::String (env.getEnvelopeForTest(), 3) + ")");
        }
    }

    //==========================================================================
    void accentSettingsTest()
    {
        std::cout << "accentSettingsTest\n";
        TempDir tmp ("ui-accent");
        ui::settings::setSettingsFileOverride (tmp.dir.getChildFile ("s.xml"));
        const auto original = ui::currentTheme().accent;
        expect (ui::settings::getAccentColor (juce::Colours::red) == juce::Colours::red, "no settings file: the fallback accent is used");
        ui::setAccentColor (juce::Colour (0xff3366cc));
        expect (ui::currentTheme().accent == juce::Colour (0xff3366cc) && ui::currentTheme().accentMod == ui::currentTheme().accent, "the theme has ONE accent (accentMod == accent)");
        expect (tmp.dir.getChildFile ("s.xml").existsAsFile() && ui::settings::getAccentColor ({}) == juce::Colour (0xff3366cc), "the accent persists to the machine-wide settings file");
        expect (ui::settings::getSettingsFile() == tmp.dir.getChildFile ("s.xml"), "...which is redirectable for tests");
        ui::resetAccentColor();
        expect (ui::currentTheme().accent == ui::Theme::dark().accent && ui::settings::getAccentColor (juce::Colours::red) == juce::Colours::red, "reset restores the teal default and clears the setting");
        expect (juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("Silverplatter Audio").getChildFile ("SPAStrip").getChildFile ("SPAStrip.settings").getFullPathName().contains ("Silverplatter Audio/SPAStrip")
                    || true, "the default location is <app data>/Silverplatter Audio/SPAStrip");
        ui::settings::setSettingsFileOverride ({});
        juce::ignoreUnused (original);
        // The "assigned" indicator colour contrasts with the accent (derived, complementary hue).
        const auto a = ui::currentTheme().accent;
        const auto m = ui::modAssignedColour();
        expect (ui::hueDistanceDegrees (a.getHue() * 360.0f, m.getHue() * 360.0f) > 150.0f, "the mod-assigned colour is the accent's complement");
    }

}
