// MIDI Learn (CC -> parameter), from the parameter right-click menu. Ported
// from SPAGlitch's manager; offered on the VST3 and the "SPAStrip MIDI" AU only.
namespace midiLearnTests
{
    using phase3Tests::Editor;
    using phase3Tests::openEditor;
    using phase3Tests::ForceLive;
    namespace fx = spa::params::id::fx;

    juce::MidiBuffer ccBuffer (int cc, int value)
    {
        juce::MidiBuffer m;
        m.addEvent (juce::MidiMessage::controllerEvent (1, cc, value), 0);
        return m;
    }

    bool menuHas (const juce::PopupMenu& menu, int itemId)
    {
        for (juce::PopupMenu::MenuItemIterator it (menu, true); it.next();)
            if (it.getItem().itemID == itemId)
                return true;
        return false;
    }

    void midiLearnTest()
    {
        std::cout << "midiLearnTest\n";
        ForceLive live;

        // Where the build cannot receive MIDI (the plain AU, tests by default),
        // the menu offers modulation only.
        {
            auto proc = makeProc (48000.0, 256);
            auto ed = openEditor (*proc);
            expect (! proc->supportsMidiLearn(), "no MIDI Learn outside VST3 / the MIDI AU");
            const auto menu = ed->getContent().buildParameterMenu (fx::delayFeedback);
            expect (! menuHas (menu, 1) && menuHas (menu, 101), "plain AU menu: modulation items, no MIDI Learn");
            expect (ed->getContent().buildParameterMenu (fx::distType).getNumItems() == 0,
                    "a non-modulatable choice gets no menu at all there");
        }

        auto proc = makeProc (48000.0, 256);
        proc->enableMidiLearnForTest();
        auto ed = openEditor (*proc);
        auto& content = ed->getContent();
        auto& learn = proc->getMidiLearn();

        expect (menuHas (content.buildParameterMenu (fx::delayFeedback), 1)
                    && menuHas (content.buildParameterMenu (fx::delayFeedback), 101),
                "VST3 menu: MIDI Learn plus the modulation items");
        expect (menuHas (content.buildParameterMenu (fx::distType), 1), "every parameter (a choice too) offers MIDI Learn");

        // Learn through the menu's own result path, then a CC captures and moves it.
        content.applyParameterMenuResult (1, fx::delayFeedback);
        expect (learn.isArmed() && learn.getArmedParamID() == fx::delayFeedback, "MIDI Learn arms the parameter");
        phase2bTests::pump (60);   // let the change broadcast reach the editor
        expect (content.getMidiLearnStatusForTest().startsWith ("MIDI LEARN"), "footer says it is listening");

        juce::AudioBuffer<float> buf (2, 256);
        buf.clear();
        auto midi = ccBuffer (74, 127);
        proc->processBlock (buf, midi);
        expect (! learn.isArmed() && learn.getAssignedCC (fx::delayFeedback) == 74, "the first CC is captured (CC 74)");
        auto* raw = proc->getAPVTS().getRawParameterValue (fx::delayFeedback);
        auto* param = proc->getAPVTS().getParameter (fx::delayFeedback);
        expect (std::abs (raw->load() - param->convertFrom0to1 (1.0f)) < 1.0e-4f,
                "the CC sets the value the DSP reads in the same block");
        expect (midi.isEmpty(), "an effect passes no MIDI on");
        phase2bTests::pump (60);
        expect (content.getMidiLearnStatusForTest().startsWith ("CC 74"), "footer confirms the CC");

        midi = ccBuffer (74, 0);
        proc->processBlock (buf, midi);
        expect (std::abs (raw->load() - param->convertFrom0to1 (0.0f)) < 1.0e-4f, "the mapped CC keeps driving it");
        expect (menuHas (content.buildParameterMenu (fx::delayFeedback), 2), "assigned: the menu offers Remove");

        // Session state: survives a host save/restore and a preset load, never in a preset.
        {
            juce::MemoryBlock block;
            proc->getStateInformation (block);
            auto other = makeProc (48000.0, 256);
            other->setStateInformation (block.getData(), (int) block.getSize());
            expect (other->getMidiLearn().getAssignedCC (fx::delayFeedback) == 74, "the map survives a session save / restore");
            const auto preset = proc->capturePresetState();
            expect (! preset.getChildWithName (spa::MidiLearnManager::mapTreeType).isValid(), "a preset never carries the map");
            proc->applyPresetState (preset, "LOAD");
            expect (learn.getAssignedCC (fx::delayFeedback) == 74, "a preset load keeps the session's map");
            expect (! proc->getAPVTS().state.getChildWithName (spa::MidiLearnManager::mapTreeType).isValid(),
                    "the map never lands in the parameter tree");
        }

        content.applyParameterMenuResult (2, fx::delayFeedback);
        expect (learn.getAssignedCC (fx::delayFeedback) < 0, "Remove clears the CC");
        content.applyParameterMenuResult (101, fx::delayFeedback);
        expect (proc->getModSlotTarget (0) == fx::delayFeedback, "the modulation items still assign slots");

        // A right-click on a switch opens the menu instead of flipping it.
        {
            spa::ui::Toggle t (proc->getAPVTS(), fx::delayPingPong, "PING");
            const bool before = t.button.getToggleState();
            auto e = juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), { 5.0f, 5.0f },
                                       juce::ModifierKeys (juce::ModifierKeys::rightButtonModifier | juce::ModifierKeys::popupMenuClickModifier),
                                       1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &t.button, &t.button, juce::Time::getCurrentTime(),
                                       { 5.0f, 5.0f }, juce::Time::getCurrentTime(), 1, false);
            t.button.mouseDown (e);
            t.button.mouseUp (e);
            expect (t.button.getToggleState() == before, "a right-click does not toggle a switch");
        }
    }
}
