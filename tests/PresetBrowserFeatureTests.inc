// The two preset-drawer features built once in the shared spa-presets module:
//   (a) the search box's clear "x" (visible only while there is text; click clears,
//       re-filters and returns the focus through the hook; Esc clears a non-empty box),
//   (b) multi-select batch export (right-click a row inside a multi-selection -> Export
//       writes SEPARATE files, one clash question, one summary; single export unchanged),
// plus the export gate in demo mode (isExportBlocked).
// Every control is hit-tested at its centre and driven through the real mouse path (a
// control that does not respond in a test is a bug report, never something to route around).
// Included from SPAStripTests.cpp after PresetFormatFixtureTests.inc, inside the
// presetTests namespace so the UI-only filter runs it.

namespace presetTests
{
    // A mouse event at `local` with explicit modifiers (the same modifiers go down and up, as
    // the peer delivers them: a right-click's mouseUp still reports the popup button).
    inline juce::MouseEvent eventWith (juce::Component& target, juce::Point<float> local, juce::ModifierKeys mods, int clicks)
    {
        auto src = juce::Desktop::getInstance().getMainMouseSource();
        const auto now = juce::Time::getCurrentTime();
        return juce::MouseEvent (src, local, mods, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &target, &target, now, local, now, clicks, false);
    }

    // Hit-tests `pos` (in `root`'s space) and clicks whatever is there. Returns the component hit.
    inline juce::Component* clickWith (juce::Component& root, juce::Point<float> pos, juce::ModifierKeys mods)
    {
        auto* target = root.getComponentAt (pos.roundToInt());
        if (target == nullptr)
            return nullptr;
        const auto local = target->getLocalPoint (&root, pos);
        target->mouseEnter (eventWith (*target, local, {}, 0));
        target->mouseDown (eventWith (*target, local, mods, 1));
        target->mouseUp (eventWith (*target, local, mods, 1));
        pump (15);
        return target;
    }

    inline juce::ModifierKeys leftClick() { return juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier); }
    inline juce::ModifierKeys shiftClick() { return juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::shiftModifier); }
    inline juce::ModifierKeys rightClick() { return juce::ModifierKeys (juce::ModifierKeys::rightButtonModifier); }

    inline juce::Point<float> centreOfRow (juce::Component& root, Browser& b, int row)
    {
        auto& list = b.getListForTest();
        list.scrollToEnsureRowIsOnscreen (row);   // the drawer scrolls to the loaded preset when it opens
        return root.getLocalArea (&list, list.getRowPosition (row, true)).getCentre().toFloat();
    }

    inline bool hitIsInside (juce::Component& root, juce::Point<float> pos, juce::Component& expected)
    {
        auto* hit = root.getComponentAt (pos.roundToInt());
        return hit != nullptr && (hit == &expected || expected.isParentOf (hit));
    }

    // Everything the hooks were asked, so a test can answer them like the editor's dialogs would.
    struct HookLog
    {
        int fileRequests = 0, clashQuestions = 0, popups = 0, focusRequests = 0, closes = 0;
        Browser::FileRequest lastRequest;
        int lastClashing = -1, lastTotal = -1;
        juce::StringArray messages;
        juce::PopupMenu lastMenu;
        std::function<void (int)> lastMenuCallback;
        // Answers.
        juce::File folderAnswer, fileAnswer;
        Browser::BatchExportClash clashAnswer = Browser::BatchExportClash::cancel;
    };

    inline void wireHooks (Browser& b, HookLog& log)
    {
        auto& h = b.getHooksForTest();
        h.showPopup = [&log] (juce::PopupMenu& menu, const juce::PopupMenu::Options&, std::function<void (int)> cb)
        {
            ++log.popups;
            log.lastMenu = menu;
            log.lastMenuCallback = std::move (cb);
        };
        h.chooseFile = [&log] (const Browser::FileRequest& r, std::function<void (const juce::Array<juce::File>&)> done)
        {
            ++log.fileRequests;
            log.lastRequest = r;
            const auto answer = r.kind == Browser::FileRequest::Kind::chooseFolder ? log.folderAnswer : log.fileAnswer;
            done (answer == juce::File() ? juce::Array<juce::File>() : juce::Array<juce::File> (answer));
        };
        h.askBatchExportClash = [&log] (int clashing, int total, std::function<void (Browser::BatchExportClash)> decide)
        {
            ++log.clashQuestions;
            log.lastClashing = clashing;
            log.lastTotal = total;
            decide (log.clashAnswer);
        };
        h.showMessage = [&log] (const juce::String& title, const juce::String& body) { log.messages.add (title + ": " + body); };
        h.requestKeyboardFocus = [&log] { ++log.focusRequests; };
    }

    inline juce::String menuItemText (juce::PopupMenu& m, int id)
    {
        juce::PopupMenu::MenuItemIterator it (m, true);
        while (it.next())
            if (it.getItem().itemID == id)
                return it.getItem().text;
        return "<none>";
    }

    //==========================================================================
    void presetSearchClearTest()
    {
        std::cout << "presetSearchClearTest\n";
        ForceLive live;
        TempDir t ("searchclear");
        SettingsGuard sg;
        auto proc = makeIsolated (t);
        auto& pm = proc->getPresetManager();
        populate (*proc);
        pm.rescan();
        auto ed = openEditor (*proc);
        ed->addToDesktop (0);   // hit-testing needs a showing component tree
        ed->setVisible (true);
        auto& content = ed->getContent();
        auto& b = content.getPresetBrowser();
        content.setPresetBrowserOpen (true, false);
        b.setBounds (b.getOpenBounds());
        HookLog log;
        wireHooks (b, log);
        auto& box = b.getSearchBoxForTest();
        auto& clear = b.getSearchClearForTest();

        const auto clearCentre = [&] { return ed->getLocalArea (&clear, clear.getLocalBounds()).getCentre().toFloat(); };

        // --- hidden while the box is empty: a click there lands on the box, not on a button.
        expect (! clear.isVisible() && ! hitIsInside (*ed, clearCentre(), clear), "no clear button while the search box is empty");
        expect (clear.getComponentID() == "searchClear" && ! clear.getMouseClickGrabsKeyboardFocus() && clear.getTooltip().isNotEmpty(),
                "the clear button has an id and a tooltip and never takes the keyboard focus on click");

        // --- typing shows it, inside the box (never wider than the box's right end).
        typeInto (box, "room");
        expect (b.getCountText() == "1 of 13 presets", "typing filters the list (" + b.getCountText() + ")");
        expect (clear.isVisible() && b.getLocalArea (&clear, clear.getLocalBounds()).getRight() <= box.getRight() + 1
                    && box.getBounds().contains (clear.getBounds()),
                "the clear button appears inside the search box once there is text");
        expect (hitIsInside (*ed, clearCentre(), clear), "hit-test at the button's centre finds the button, not the text box beneath it");

        // --- a real click clears the text, restores the list, hides the button and hands the focus back.
        const auto focusBefore = log.focusRequests;
        const auto* hit = clickWith (*ed, clearCentre(), leftClick());
        expect (hit == &clear, "the click is delivered to the clear button");
        expect (box.getText().isEmpty() && b.getCountText() == "13 of 13 presets", "the click clears the box and the list is restored (" + b.getCountText() + ")");
        expect (! clear.isVisible(), "the button hides again");
        expect (log.focusRequests == focusBefore + 1, "the keyboard focus is handed back through the hook exactly once");

        // --- without the hook the search box takes the focus back (so typing can continue).
        b.getHooksForTest().requestKeyboardFocus = nullptr;
        typeInto (box, "kick");
        clickWith (*ed, clearCentre(), leftClick());
        expect (box.getText().isEmpty() && b.getCountText() == "13 of 13 presets", "clearing works with no focus hook set");

        // --- a type + search filter: the clear button clears the search only.
        expect (b.setTypeFilter ("Drums"), "pick the Drums type");
        typeInto (box, "snare");
        expect (b.getCountText() == "1 of 13 presets", "type + search (" + b.getCountText() + ")");
        clickWith (*ed, clearCentre(), leftClick());
        expect (b.getCountText() == "3 of 13 presets" && b.getTypeBoxForTest().getText() == "Drums", "clearing the search keeps the type filter (" + b.getCountText() + ")");
        b.setTypeFilter ({});

        // --- Esc in the search box: clears a non-empty box and stays in it; the drawer stays open.
        typeInto (box, "vocal");
        expect (b.getCountText() == "2 of 13 presets" && clear.isVisible(), "typed 'vocal'");
        const auto escape = juce::KeyPress (juce::KeyPress::escapeKey);
        expect (box.keyPressed (escape), "Esc is consumed by the search box");
        pump (40);   // JUCE posts the Esc as a command message
        expect (box.getText().isEmpty() && b.getCountText() == "13 of 13 presets" && ! clear.isVisible(), "Esc clears the box and restores the list");
        expect (content.isPresetBrowserOpen(), "...and the drawer stays open");

        // --- Esc in an EMPTY box keeps today's behaviour: it leaves the box (no clearing); the next Esc closes the drawer.
        const auto consumed = box.keyPressed (escape);
        pump (40);
        expect (consumed && content.isPresetBrowserOpen() && b.getCountText() == "13 of 13 presets",
                "Esc in an empty box leaves it and does not close the drawer");
        b.keyPressed (escape);
        pump (30);
        expect (! content.isPresetBrowserOpen(), "the next Esc (with the drawer focused) closes the drawer");
        content.setPresetBrowserOpen (true, false);

        // --- setSearchText (the programmatic path) keeps the button in step too.
        b.setSearchText ("polish");
        expect (clear.isVisible(), "setSearchText shows the clear button");
        b.setSearchText ({});
        expect (! clear.isVisible(), "...and an emptied box hides it");
    }

    //==========================================================================
    // A module-level adapter with nothing of SPAStrip in it: proves the manager works for another
    // product, and exercises the optional "missing content" summary.
    inline spa::presets::Adapter dummyAdapter (const juce::File& root)
    {
        spa::presets::Adapter a;
        a.productName = "Dummy";
        a.fileExtension = ".dummy";
        a.rootTag = "DummyPreset";
        a.types = juce::StringArray { "One", "Two" };
        a.defaultPresetsRoot = [root] { return root; };
        a.captureState = [] { juce::ValueTree v ("STATE"); v.setProperty ("x", 1, nullptr); return v; };
        a.applyState = [] (const juce::ValueTree&, const juce::String&) { return true; };
        a.applyInit = [] {};
        a.captureBaseline = [] { return std::shared_ptr<const void> (std::make_shared<int> (0)); };
        a.differsFromBaseline = [] (const void*) { return false; };
        a.missingContent = [] (const spa::presets::PresetInfo& p)
        {
            return p.name == "Needs Piano" ? juce::StringArray { "Piano.wav", "Strings.wav" } : juce::StringArray();
        };
        return a;
    }

    void presetBatchExportTest()
    {
        std::cout << "presetBatchExportTest\n";
        ForceLive live;
        TempDir t ("batchexport");
        TempDir out ("batchexport-out");
        SettingsGuard sg;
        auto proc = makeIsolated (t);
        auto& pm = proc->getPresetManager();
        populate (*proc);
        pm.save ("Dup", "", false, "FX");
        pm.save ("Dup", "Studio", false, "FX");
        pm.rescan();
        auto ed = openEditor (*proc);
        ed->addToDesktop (0);   // hit-testing needs a showing component tree
        ed->setVisible (true);
        auto& content = ed->getContent();
        auto& b = content.getPresetBrowser();
        content.setPresetBrowserOpen (true, false);
        b.setBounds (b.getOpenBounds());
        HookLog log;
        wireHooks (b, log);
        log.folderAnswer = out.dir;
        const auto exportsDir = out.dir;
        const auto fileCount = [&] { return exportsDir.findChildFiles (juce::File::findFiles, false, "*.spastrip").size(); };

        // User root, by name: Dup, Kick Punch, Lead Vox, Mystery, Snare Crack, Warm Bass.
        const auto kick = rowOf (b, "Kick Punch"), lead = rowOf (b, "Lead Vox"), mystery = rowOf (b, "Mystery");
        expect (kick > 0 && lead == kick + 1 && mystery == lead + 1, "the User root lists Kick Punch, Lead Vox, Mystery in a row");

        // --- select a range the way a user does: click a row (it loads), shift-click another.
        expect (hitIsInside (*ed, centreOfRow (*ed, b, kick), b), "the row is hit-testable inside the drawer");
        clickWith (*ed, centreOfRow (*ed, b, kick), leftClick());
        expect (pm.getCurrentName() == "Kick Punch" && b.selectedPresetRows().size() == 1, "a plain click loads the preset and selects its row");
        clickWith (*ed, centreOfRow (*ed, b, mystery), shiftClick());
        expect (b.selectedPresetRows().size() == 3 && pm.getCurrentName() == "Kick Punch",
                "shift-click selects the range (3 presets) and does not load anything (" + juce::String ((int) b.selectedPresetRows().size()) + ")");
        expect (b.isInMultiSelection (lead) && b.isInMultiSelection (kick) && ! b.isInMultiSelection (rowOf (b, "Snare Crack")),
                "rows inside the selection are 'in a multi-selection', others are not");

        // --- a refresh (a preset change elsewhere) must not wipe the selection the user is building.
        pm.sendChangeMessage();
        pump (60);
        expect (b.selectedPresetRows().size() == 3, "a refresh keeps a multi-selection");

        // --- right-click a SELECTED row: the menu says Export 3 presets..., selection untouched.
        const auto popupsBefore = log.popups;
        auto* hit = clickWith (*ed, centreOfRow (*ed, b, lead), rightClick());
        expect (hit != nullptr && b.isParentOf (hit), "the right-click lands on a list row");
        expect (log.popups == popupsBefore + 1 && b.selectedPresetRows().size() == 3, "the row menu opened and the multi-selection is intact");
        expect (menuItemText (log.lastMenu, Browser::exportMenuItemId) == "Export 3 presets...", "the menu's export item names the selection: " + menuItemText (log.lastMenu, Browser::exportMenuItemId));
        expect (menuItemText (log.lastMenu, Browser::renameMenuItemId) == "Rename...", "the other items are the single-row ones");

        // --- Export: N separate files into the chosen folder, no clash question, one summary.
        expect (fileCount() == 0, "the destination folder starts empty");
        log.lastMenuCallback (Browser::exportMenuItemId);
        pump (30);
        expect (log.fileRequests == 1 && log.lastRequest.kind == Browser::FileRequest::Kind::chooseFolder,
                "one FOLDER is asked for (not a file name, not a zip)");
        expect (fileCount() == 3 && ! out.dir.getChildFile ("x.zip").exists()
                    && out.dir.getChildFile ("Kick Punch.spastrip").existsAsFile() && out.dir.getChildFile ("Lead Vox.spastrip").existsAsFile()
                    && out.dir.getChildFile ("Mystery.spastrip").existsAsFile(),
                "three separate .spastrip files were written (" + juce::String (fileCount()) + ")");
        expect (out.dir.getChildFile ("Lead Vox.spastrip").loadFileAsString() == fileOf (pm, "Lead Vox").loadFileAsString()
                    && out.dir.getChildFile ("Kick Punch.spastrip").loadFileAsString() == fileOf (pm, "Kick Punch").loadFileAsString(),
                "each exported file is the preset's file verbatim");
        expect (log.clashQuestions == 0 && log.messages.size() == 1 && log.messages[0].contains ("3 presets exported"),
                "no clash question for an empty folder; one summary: " + (log.messages.isEmpty() ? juce::String ("<none>") : log.messages[0]));

        // --- exporting the same selection again: ONE clash question for the batch.
        log.messages.clear();
        log.clashAnswer = Browser::BatchExportClash::skipExisting;
        {
            // Make the existing file different so "skip" and "replace" can be told apart.
            out.dir.getChildFile ("Lead Vox.spastrip").replaceWithText ("stale");
        }
        clickWith (*ed, centreOfRow (*ed, b, lead), rightClick());
        log.lastMenuCallback (Browser::exportMenuItemId);
        pump (30);
        expect (log.clashQuestions == 1 && log.lastClashing == 3 && log.lastTotal == 3, "ONE question for the whole batch, naming how many clash (" + juce::String (log.lastClashing) + " of " + juce::String (log.lastTotal) + ")");
        expect (out.dir.getChildFile ("Lead Vox.spastrip").loadFileAsString() == "stale" && fileCount() == 3 && log.messages.size() == 1 && log.messages[0].contains ("0 presets exported")
                    && log.messages[0].contains ("Lead Vox"),
                "Skip existing leaves the files alone and the summary lists what was left");

        log.messages.clear();
        log.clashAnswer = Browser::BatchExportClash::replaceAll;
        clickWith (*ed, centreOfRow (*ed, b, lead), rightClick());
        log.lastMenuCallback (Browser::exportMenuItemId);
        pump (30);
        expect (log.clashQuestions == 2 && out.dir.getChildFile ("Lead Vox.spastrip").loadFileAsString() == fileOf (pm, "Lead Vox").loadFileAsString(),
                "Replace all overwrites (still ONE question per batch)");

        log.messages.clear();
        log.clashAnswer = Browser::BatchExportClash::cancel;
        out.dir.getChildFile ("Mystery.spastrip").deleteFile();
        out.dir.getChildFile ("Lead Vox.spastrip").replaceWithText ("keep me");
        clickWith (*ed, centreOfRow (*ed, b, lead), rightClick());
        log.lastMenuCallback (Browser::exportMenuItemId);
        pump (30);
        expect (log.clashQuestions == 3 && out.dir.getChildFile ("Lead Vox.spastrip").loadFileAsString() == "keep me"
                    && ! out.dir.getChildFile ("Mystery.spastrip").exists() && log.messages.isEmpty(),
                "Cancel writes nothing at all (not even the files that were free) and shows no summary");

        // --- right-clicking a row OUTSIDE the selection: the selection becomes that row; single export, unchanged.
        for (const auto& f : out.dir.findChildFiles (juce::File::findFiles, false, "*"))
            f.deleteFile();
        log.messages.clear();
        const auto snare = rowOf (b, "Snare Crack");
        clickWith (*ed, centreOfRow (*ed, b, snare), rightClick());
        expect (b.selectedPresetRows().size() == 1 && ! b.isInMultiSelection (snare), "right-click on an unselected row selects just that row");
        expect (menuItemText (log.lastMenu, Browser::exportMenuItemId) == "Export preset...", "...and the export item is the single-preset one");
        log.fileAnswer = out.dir.getChildFile ("Snare Crack.spastrip");
        const auto requestsBefore = log.fileRequests;
        log.lastMenuCallback (Browser::exportMenuItemId);
        pump (30);
        expect (log.fileRequests == requestsBefore + 1 && log.lastRequest.kind == Browser::FileRequest::Kind::saveFile && log.lastRequest.wildcard == "*.spastrip",
                "single export asks for a FILE as before");
        expect (fileCount() == 1 && out.dir.getChildFile ("Snare Crack.spastrip").loadFileAsString() == fileOf (pm, "Snare Crack").loadFileAsString() && log.messages.isEmpty(),
                "one file, verbatim, no summary (unchanged behaviour)");

        // --- two presets with the same name (different folders) in one batch: both are kept.
        for (const auto& f : out.dir.findChildFiles (juce::File::findFiles, false, "*"))
            f.deleteFile();
        {
            // The two Dup presets: the User root one and the Studio one further down the list.
            std::vector<int> rowsToExport { rowOf (b, "Dup") };
            for (int i = 0; i < b.getNumRows(); ++i)
                if (const auto* p = b.presetAtRow (i); p != nullptr && p->name == "Dup" && p->folder == "Studio")
                    rowsToExport.push_back (i);
            expect (rowsToExport.size() == 2, "both Dup presets are listed");
            b.exportRows (rowsToExport);
            pump (30);
            expect (out.dir.getChildFile ("Dup.spastrip").existsAsFile() && out.dir.getChildFile ("Dup 2.spastrip").existsAsFile() && fileCount() == 2,
                    "same-named presets get ' 2' instead of overwriting each other");
        }

        // --- a factory preset is exportable in a batch too.
        for (const auto& f : out.dir.findChildFiles (juce::File::findFiles, false, "*"))
            f.deleteFile();
        pm.setFactoryPresets ({ { "Fact Verb", "Dance", "<SPAStripPreset name=\"Fact Verb\" version=\"1\" type=\"FX\"><PARAMS/></SPAStripPreset>" } });
        b.refresh();
        b.exportRows ({ rowOf (b, "Fact Verb"), rowOf (b, "Warm Bass") });
        pump (30);
        expect (out.dir.getChildFile ("Fact Verb.spastrip").existsAsFile() && out.dir.getChildFile ("Warm Bass.spastrip").existsAsFile(),
                "factory + user presets export together");
        pm.setFactoryPresets ({});
        b.refresh();

        // --- DEMO MODE: export is blocked (isExportBlocked), the same UX as a blocked save.
        for (const auto& f : out.dir.findChildFiles (juce::File::findFiles, false, "*"))
            f.deleteFile();
        {
            int blockedCalls = 0, saveBlockedCalls = 0;
            const auto oldBlocked = pm.isExportBlocked;
            const auto oldCb = pm.onExportBlocked;
            const auto oldSaveCb = pm.onSaveBlocked;
            pm.isExportBlocked = [] { return true; };
            pm.onExportBlocked = [&blockedCalls] { ++blockedCalls; };
            pm.onSaveBlocked = [&saveBlockedCalls] { ++saveBlockedCalls; };

            b.selectRowForTest (rowOf (b, "Kick Punch"));
            clickWith (*ed, centreOfRow (*ed, b, rowOf (b, "Mystery")), shiftClick());
            expect (b.selectedPresetRows().size() >= 2, "a multi-selection exists");
            clickWith (*ed, centreOfRow (*ed, b, rowOf (b, "Lead Vox")), rightClick());
            const auto requests = log.fileRequests;
            log.lastMenuCallback (Browser::exportMenuItemId);
            pump (30);
            expect (blockedCalls == 1 && log.fileRequests == requests && fileCount() == 0 && saveBlockedCalls == 0,
                    "batch export in demo mode: the blocked prompt fires once, no chooser opens, nothing is written");

            clickWith (*ed, centreOfRow (*ed, b, rowOf (b, "Snare Crack")), rightClick());
            log.lastMenuCallback (Browser::exportMenuItemId);
            pump (30);
            expect (blockedCalls == 2 && log.fileRequests == requests && fileCount() == 0, "single export in demo mode: blocked the same way");

            juce::String err;
            expect (! pm.exportPreset (*infoOf (pm, "Warm Bass"), out.dir.getChildFile ("w.spastrip"), &err) && err == PM::exportBlockedMessage && ! out.dir.getChildFile ("w.spastrip").exists(),
                    "exportPreset refuses with the demo message");
            expect (! pm.exportFolder ("Studio", out.dir.getChildFile ("s.zip")) && ! out.dir.getChildFile ("s.zip").exists(), "exportFolder refuses");
            const auto res = pm.exportPresets ({ *infoOf (pm, "Warm Bass") }, out.dir, PM::ExistingFile::replace);
            expect (res.blocked && res.written == 0 && fileCount() == 0, "exportPresets refuses and reports 'blocked'");

            // With no export-specific callback the save callback is used (the editor's licence panel).
            pm.onExportBlocked = nullptr;
            const auto before = saveBlockedCalls;
            pm.exportPreset (*infoOf (pm, "Warm Bass"), out.dir.getChildFile ("w.spastrip"));
            expect (saveBlockedCalls == before + 1, "without an export callback the blocked-save callback runs (same UX as a blocked save)");

            // Saving is its own gate: blocking exports does not block saving, and loading is never affected.
            expect (pm.save ("Still Saves", "").ok && pm.loadPreset (0).ok, "isExportBlocked does not affect save or load");

            pm.isExportBlocked = oldBlocked;
            pm.onExportBlocked = oldCb;
            pm.onSaveBlocked = oldSaveCb;
            const auto allowed = pm.exportPreset (*infoOf (pm, "Warm Bass"), out.dir.getChildFile ("w.spastrip"));
            expect (allowed && out.dir.getChildFile ("w.spastrip").existsAsFile(), "with the gate off, export works again");
        }

        // --- the module with an adapter that is not SPAStrip's: batch export + combined missing-content summary.
        {
            TempDir dummyRoot ("dummy-root");
            TempDir dummyOut ("dummy-out");
            spa::presets::PresetManager dummy (dummyAdapter (dummyRoot.dir));
            dummy.setUseTrash (false);
            expect (dummy.save ("Needs Piano", "", false, "One").ok && dummy.save ("Plain", "", false, "Two").ok && dummy.save ("Needs Piano", "Bank").ok,
                    "a different product's manager saves presets");
            expect (dummyRoot.dir.getChildFile ("User").getChildFile ("Plain.dummy").loadFileAsString().contains ("<DummyPreset name=\"Plain\" version=\"1\" type=\"Two\">"),
                    "...in its own format (extension, root tag, type)");
            std::vector<spa::presets::PresetInfo> all (dummy.getPresets().begin(), dummy.getPresets().end());
            const auto r = dummy.exportPresets (all, dummyOut.dir, spa::presets::PresetManager::ExistingFile::replace);
            expect (r.written == 3 && ! r.blocked && r.failed.isEmpty() && r.skipped.isEmpty(), "three presets exported");
            expect (r.missingContent.size() == 2 && r.missingContent[0].startsWith ("Needs Piano: Piano.wav, Strings.wav"),
                    "one combined missing-content line per affected preset: " + r.missingContent.joinIntoString (" | "));
            expect (dummyOut.dir.getChildFile ("Needs Piano.dummy").existsAsFile() && dummyOut.dir.getChildFile ("Needs Piano 2.dummy").existsAsFile()
                        && dummyOut.dir.getChildFile ("Plain.dummy").existsAsFile(), "files carry that product's extension; duplicates are numbered");
            // The same file is refused by a SPAStrip manager (another product's format is never accepted).
            auto session = pm.beginImport ({ dummyOut.dir.getChildFile ("Plain.dummy") });
            const auto step = session->advance();
            const auto result = session->finish();
            expect (step.finished && result.imported == 0 && result.malformed.size() == 1, "another product's preset file is not importable");
        }
    }
}
