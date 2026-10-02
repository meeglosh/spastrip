// Preset management tests: the preset TYPES (Drums ... Creative, the "Other" bucket), the
// user folders, the browser's filtering / grouping / navigation, the save panel, export /
// import, favourites -- the way SPASynth's preset drawer manages them, adapted for an effect.
// Included from SPAStripTests.cpp after Phase3Tests.inc (its helpers are in scope).

namespace presetTests
{
    namespace ui = spa::ui;
    namespace fx = spa::params::id::fx;
    namespace params = spa::params;
    using PM = spa::preset::PresetManager;
    using Browser = ui::PresetBrowser;
    using phase2bTests::TempDir;
    using phase2bTests::makeIsolated;
    using phase2bTests::dirtyPatch;
    using phase2bTests::pump;
    using phase2bTests::allValues;
    using phase2bTests::identicalValuesExceptOversampling;
    using phase3Tests::ForceLive;
    using phase3Tests::openEditor;

    // A hermetic settings file (favourites, collapsed groups, group mode) for one test.
    struct SettingsGuard
    {
        juce::File previous = ui::settings::getSettingsFile();
        juce::File file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                              .getChildFile ("spastrip-presettests-" + juce::String (juce::Random::getSystemRandom().nextInt (1000000)) + ".xml");
        SettingsGuard() { ui::settings::setSettingsFileOverride (file); }
        ~SettingsGuard() { ui::settings::setSettingsFileOverride (previous); file.deleteFile(); }
    };

    PM::PresetInfo info (const juce::String& name, const juce::String& folder, const juce::String& type, bool factory = false)
    {
        PM::PresetInfo p;
        p.name = name;
        p.folder = factory ? juce::String() : folder;
        p.bank = factory ? folder : folder.upToFirstOccurrenceOf ("/", false, false);
        p.isFactory = factory;
        p.type = type;
        p.storedType = type;
        p.file = factory ? juce::File() : juce::File ("/virtual/User").getChildFile (folder).getChildFile (name + ".spastrip");
        return p;
    }

    // Typing into a field the way a user does: the editor's own text-change callback is async, so call it directly.
    void typeInto (juce::TextEditor& e, const juce::String& text)
    {
        e.setText (text, false);
        if (e.onTextChange)
            e.onTextChange();
    }

    // A copy of a preset file under another name (its name attribute changes; the patch is the same).
    void copyAs (const juce::File& src, const juce::File& dest, const juce::String& name)
    {
        auto xml = juce::XmlDocument::parse (src);
        xml->setAttribute ("name", name);
        xml->writeTo (dest);
    }

    juce::File fileOf (PM& pm, const juce::String& name)
    {
        for (const auto& p : pm.getPresets())
            if (p.name == name && ! p.isFactory)
                return p.file;
        return {};
    }

    const PM::PresetInfo* infoOf (PM& pm, const juce::String& name)
    {
        for (const auto& p : pm.getPresets())
            if (p.name == name)
                return &p;
        return nullptr;
    }

    juce::String rootAttr (const juce::File& f, const juce::String& attr)
    {
        auto xml = juce::XmlDocument::parse (f);
        return xml != nullptr ? xml->getStringAttribute (attr) : juce::String ("<unreadable>");
    }

    bool rootHasAttr (const juce::File& f, const juce::String& attr)
    {
        auto xml = juce::XmlDocument::parse (f);
        return xml != nullptr && xml->hasAttribute (attr);
    }

    juce::StringArray namesOf (const std::vector<PM::PresetInfo>& list, const std::vector<int>& idx)
    {
        juce::StringArray out;
        for (auto i : idx)
            out.add (list[(size_t) i].name);
        return out;
    }

    //==========================================================================
    void presetTypeRoundTripTest()
    {
        std::cout << "presetTypeRoundTripTest\n";
        TempDir t ("types");
        SettingsGuard sg;
        auto proc = makeIsolated (t);
        auto& pm = proc->getPresetManager();

        expect (PM::presetTypes() == juce::StringArray ({ "Drums", "Bass", "Vocals", "Guitar", "Keys", "Synth", "FX", "Mixbus", "Mastering", "Creative" }),
                "the types are Drums, Bass, Vocals, Guitar, Keys, Synth, FX, Mixbus, Mastering, Creative, in that order");
        expect (PM::canonicalType ("  mIXbus ") == "Mixbus" && PM::canonicalType ("fx") == "FX" && PM::canonicalType ("Dubstep").isEmpty()
                    && PM::canonicalType ("").isEmpty() && PM::canonicalType ("Other").isEmpty(),
                "canonicalType: case-insensitive and trimmed; empty / unknown / 'Other' -> untyped");

        // --- every type is stored as type="..." on <SPAStripPreset> and read back by the scan.
        dirtyPatch (*proc);
        for (const auto& type : PM::presetTypes())
        {
            const auto r = pm.save ("T " + type, "", false, type);
            expect (r.ok, "save as " + type);
            auto xml = juce::XmlDocument::parse (r.file);
            expect (xml != nullptr && xml->hasTagName ("SPAStripPreset") && xml->getStringAttribute ("type") == type
                        && xml->getStringAttribute ("name") == "T " + type && xml->getIntAttribute ("version") == 1,
                    type + ": <SPAStripPreset name=\"T " + type + "\" version=\"1\" type=\"" + type + "\">");
            const auto* state = xml != nullptr ? xml->getFirstChildElement() : nullptr;
            expect (state != nullptr && ! state->hasAttribute ("type") && state->hasAttribute ("fxOrder"),
                    type + ": the type is on the root, not inside the patch state");
        }
        bool scanned = true;
        for (const auto& type : PM::presetTypes())
        {
            const auto* p = infoOf (pm, "T " + type);
            scanned = scanned && p != nullptr && p->type == type && p->storedType == type;
        }
        expect (scanned, "the scan reports every preset's type");

        // --- the type does not touch the patch: a typed preset loads like an untyped one.
        {
            auto other = makeIsolated (t);
            expect (other->getPresetManager().load (fileOf (pm, "T Mixbus")).ok && identicalValuesExceptOversampling (*proc, *other),
                    "a typed preset loads in a fresh processor with every parameter equal");
        }

        // --- spelling: case-insensitive on the way in, canonical on disk; unknown refused; none = no attribute.
        {
            const auto r = pm.save ("lower", "", false, "dRuMs");
            expect (r.ok && rootAttr (r.file, "type") == "Drums", "a type typed in another case is stored in its canonical spelling");
            const auto bad = pm.save ("Bad Type", "", false, "Dubstep");
            expect (! bad.ok && bad.error.contains ("not a preset type") && ! t.dir.getChildFile ("User/Bad Type.spastrip").exists(),
                    "an unknown type is refused and nothing is written (" + bad.error + ")");
            const auto none = pm.save ("Plain", "");
            expect (none.ok && ! rootHasAttr (none.file, "type") && infoOf (pm, "Plain")->type.isEmpty(), "no type -> no attribute, untyped");
        }

        // --- rename keeps the type (and every other attribute).
        {
            const auto r = pm.rename (fileOf (pm, "T Bass"), "Sub Bass");
            expect (r.ok && rootAttr (r.newFile, "type") == "Bass" && rootAttr (r.newFile, "name") == "Sub Bass" && infoOf (pm, "Sub Bass")->type == "Bass",
                    "rename keeps the type attribute");
        }

        // --- Save (in place) keeps the name and the type, replaces the patch, lands clean.
        {
            pm.load (fileOf (pm, "T Guitar"));
            setParam (*proc, fx::reverbMix, 0.93f);
            expect (pm.isEdited(), "(edited before the save)");
            const auto file = fileOf (pm, "T Guitar");
            const auto r = pm.saveInPlace (file);
            expect (r.ok && r.file == file && rootAttr (file, "type") == "Guitar" && rootAttr (file, "name") == "T Guitar" && ! pm.isEdited(),
                    "saveInPlace keeps the name and type and clears the edited flag");
            auto other = makeIsolated (t);
            other->getPresetManager().load (file);
            expect (std::abs (getParam (*other, fx::reverbMix) - 0.93f) < 1.0e-5f, "...and wrote the current patch");
            expect (! pm.saveInPlace (t.dir.getChildFile ("User/none.spastrip")).ok, "saveInPlace refuses an unscanned file");
        }

        // --- setPresetType: change, clear, refuse; atomic; never touches the sound or the edited flag.
        {
            const auto file = fileOf (pm, "T Keys");
            pm.load (file);
            const auto before = file.loadFileAsString();
            expect (pm.setPresetType (file, "Synth") && rootAttr (file, "type") == "Synth" && infoOf (pm, "T Keys")->type == "Synth", "setPresetType changes the type");
            expect (pm.getCurrentName() == "T Keys" && ! pm.isEdited() && pm.getCurrentFile() == file, "...the loaded preset stays current and unedited");
            juce::String err;
            expect (! pm.setPresetType (file, "Dubstep", &err) && err.isNotEmpty() && rootAttr (file, "type") == "Synth", "an unknown type is refused, the file untouched (" + err + ")");
            expect (pm.setPresetType (file, "") && ! rootHasAttr (file, "type") && infoOf (pm, "T Keys")->type.isEmpty(), "an empty type clears the attribute");
            auto a = juce::XmlDocument::parse (before), b = juce::XmlDocument::parse (file);
            expect (a != nullptr && b != nullptr && a->getFirstChildElement()->isEquivalentTo (b->getFirstChildElement(), true),
                    "...and the patch inside is byte-for-byte what it was");
            expect (! t.dir.getChildFile ("User/T Keys.spastrip.tmp").exists(), "no temp file left behind");
            expect (! pm.setPresetType (t.dir.getChildFile ("User/none.spastrip"), "Bass"), "an unscanned file is refused");
            pm.setFactoryPresets ({ { "Embedded", "", file.loadFileAsString() } });
            expect (! pm.setPresetType (juce::File(), "Bass"), "a factory preset has no file to retype");
            pm.setFactoryPresets ({});
        }

        // --- hand-written files: no type, an unknown type, a long root tag, an embedded IR.
        {
            const auto src = fileOf (pm, "T Drums").loadFileAsString();
            const auto folder = t.dir.getChildFile ("User");
            const auto tagEnd = src.indexOfChar (src.indexOf ("<SPAStripPreset"), '>');
            const auto open = src.substring (src.indexOf ("<SPAStripPreset"), tagEnd + 1);
            const auto rest = src.substring (tagEnd + 1);
            const auto write = [&] (const juce::String& name, const juce::String& newOpen) { folder.getChildFile (name + ".spastrip").replaceWithText (newOpen + rest); };
            write ("Hand NoType", "<SPAStripPreset name=\"Hand NoType\" version=\"1\">");
            write ("Hand Unknown", "<SPAStripPreset name=\"Hand Unknown\" version=\"1\" type=\"Dubstep\">");
            write ("Hand Lower", "<SPAStripPreset name=\"Hand Lower\" version=\"1\" type=\"vocals\">");
            write ("Hand Long", "<SPAStripPreset name=\"Hand Long\" version=\"1\" note=\"" + juce::String::repeatedString ("x", 6000) + "\" type=\"Bass\">");
            pm.rescan();
            expect (infoOf (pm, "Hand NoType")->type.isEmpty() && infoOf (pm, "Hand NoType")->storedType.isEmpty(), "a file with no type attribute (older / hand-made) is untyped");
            expect (infoOf (pm, "Hand Unknown")->type.isEmpty() && infoOf (pm, "Hand Unknown")->storedType == "Dubstep",
                    "an unknown type is untyped (listed under Other) but its raw value is remembered");
            expect (infoOf (pm, "Hand Lower")->type == "Vocals", "a stored type in another case is read as the canonical type");
            expect (infoOf (pm, "Hand Long")->type == "Bass", "a root tag longer than the scan window is still read (fallback to a full parse)");
            expect (pm.load (fileOf (pm, "Hand Unknown")).ok, "an unknown-type preset loads normally");
            // Renaming it must not lose the unknown value; saving in place keeps it too; the UI never invents one.
            const auto renamed = pm.rename (fileOf (pm, "Hand Unknown"), "Hand Unknown 2");
            expect (renamed.ok && rootAttr (renamed.newFile, "type") == "Dubstep", "rename keeps an unknown stored type untouched");
            pm.load (renamed.newFile);
            expect (pm.saveInPlace (renamed.newFile).ok && rootAttr (renamed.newFile, "type") == "Dubstep", "saveInPlace keeps it too");
            for (const char* n : { "Hand NoType", "Hand Unknown 2", "Hand Lower", "Hand Long" })
                fileOf (pm, n).deleteFile();
            pm.rescan();
        }
        {
            const auto wav = writeTestIR ("types-ir", 48000.0, 1.0);
            proc->loadConvolutionIR (wav);
            const auto r = pm.save ("Big IR", "", false, "Creative");
            expect (r.ok && r.file.getSize() > 8192, "an embedded-IR preset is large (" + juce::String ((int) (r.file.getSize() / 1024)) + " KB)");
            expect (infoOf (pm, "Big IR")->type == "Creative", "...and its type is still read from the head of the file");
            wav.deleteFile();
        }
    }

    //==========================================================================
    void presetFolderLayoutTest()
    {
        std::cout << "presetFolderLayoutTest\n";
        TempDir t ("folders");
        SettingsGuard sg;
        auto proc = makeIsolated (t);
        auto& pm = proc->getPresetManager();
        const auto user = t.dir.getChildFile ("User");
        dirtyPatch (*proc);

        // --- folder creation: two levels, sanitised names, no clashes.
        expect (pm.createUserFolder ({}, "Kits").ok && pm.createUserFolder ("Kits", "Acoustic").ok, "a folder and a nested folder are created");
        expect (! pm.createUserFolder ("Kits/Acoustic", "Deep").ok && ! user.getChildFile ("Kits/Acoustic/Deep").exists(), "a third level is refused");
        expect (! pm.createUserFolder ({}, "Kits").ok && (juce::File::areFileNamesCaseSensitive() || ! pm.createUserFolder ({}, "kits").ok), "a folder that exists is refused (on this file system case does not matter)");
        expect (! pm.createUserFolder ({}, "").ok && ! pm.createUserFolder ({}, "..").ok && ! pm.createUserFolder ({}, "   ").ok, "empty / dot-only names are refused");
        expect (! pm.createUserFolder ("Nope", "x").ok, "a missing parent is refused");
        const auto weird = pm.createUserFolder ({}, "a/b:c");
        expect (weird.ok && weird.file.getParentDirectory() == user && ! weird.file.getFileName().containsAnyOf ("/\\:"), "a path in the name is sanitised, never followed (" + weird.file.getFileName() + ")");
        expect (pm.createUserFolder ({}, "CON").ok && user.getChildFile ("_CON").isDirectory(), "a Windows device name gets the '_' prefix");
        expect (pm.createUserFolder ({}, "Empty").ok, "(an empty folder)");
        expect (pm.getUserFolders().contains ("Empty") && pm.getUserFolders().contains ("Kits/Acoustic"), "empty and nested folders are listed");
        expect (pm.getBanks().contains ("Kits") && ! pm.getBanks().contains ("Kits/Acoustic"), "getBanks is the top-level folders only");

        // --- saving into folders, with a type: the preset knows its folder, bank and type.
        const auto snare = pm.save ("Snare 1", "Kits/Acoustic", false, "Drums");
        expect (snare.ok && snare.file == user.getChildFile ("Kits/Acoustic/Snare 1.spastrip"), "saved into User/Kits/Acoustic/");
        const auto* si = infoOf (pm, "Snare 1");
        expect (si != nullptr && si->folder == "Kits/Acoustic" && si->bank == "Kits" && si->type == "Drums" && ! si->isFactory, "folder 'Kits/Acoustic', bank 'Kits', type Drums");
        expect (! pm.save ("X", "Missing", false, "", false).ok && ! user.getChildFile ("Missing").exists(), "a missing folder is refused when createFolder is off, and not created");
        expect (! pm.save ("X", "A/B/C", false, "", true).ok && ! user.getChildFile ("A").exists(), "a new folder deeper than two levels is refused");
        const auto auto1 = pm.save ("Auto Drum", "Drums", false, "Drums", true);
        expect (auto1.ok && auto1.file == user.getChildFile ("Drums/Auto Drum.spastrip"), "createFolder makes the missing folder (the panel's 'Auto (by type)' files into User/<Type>/)");
        const auto traversal = pm.save ("../../up", "../../Evil/..", false, "", true);
        expect (traversal.ok && traversal.file.isAChildOf (user) && ! t.dir.getParentDirectory().getChildFile ("up.spastrip").exists(), "traversal in name or folder cannot leave User/ (" + traversal.file.getRelativePathFrom (t.dir) + ")");
        traversal.file.deleteFile();
        expect (pm.userPresetExists ("Snare 1", "Kits/Acoustic") && ! pm.userPresetExists ("Snare 1", "Kits") && ! pm.userPresetExists ("Snare 1", "Nope"), "userPresetExists checks the exact folder");
        expect (pm.suggestName ("Snare 1", "Kits/Acoustic", false) == "Snare 1 2" && pm.suggestName ("Fresh", "Kits/Acoustic", false) == "Fresh"
                    && pm.suggestName ("Snare", "Kits/Acoustic", true) == "Snare 2" && pm.suggestName ("Drums", "Drums", true) == "Drums 1",
                "suggestName: '<base>' if free else '<base> 2'; the type default counts from 1 and skips taken names");
        pm.save ("Snare", "Kits/Acoustic");
        expect (pm.suggestName ("Snare", "Kits/Acoustic", true) == "Snare 2" && pm.suggestName ("Snare 1", "Kits/Acoustic", true) == "Snare 1 1",
                "(the number counts from 1 whatever the base)");

        // --- a folder deeper than two levels already on disk is still scanned (as in SPASynth); hidden folders and temp files are not.
        {
            const auto deep = user.getChildFile ("Old/Deeper/Deepest");
            deep.createDirectory();
            snare.file.copyFileTo (deep.getChildFile ("Deep One.spastrip"));
            user.getChildFile (".hidden").createDirectory();
            snare.file.copyFileTo (user.getChildFile (".hidden/Ghost.spastrip"));
            snare.file.copyFileTo (user.getChildFile ("Half.spastrip.tmp"));
            pm.rescan();
            const auto* d = infoOf (pm, "Deep One");
            expect (d != nullptr && d->folder == "Old/Deeper/Deepest" && d->bank == "Old", "a pre-existing deeper folder is scanned and listed");
            expect (infoOf (pm, "Ghost") == nullptr && infoOf (pm, "Half") == nullptr, "hidden folders and *.tmp files are not presets");
            expect (pm.getUserFolders().contains ("Old/Deeper/Deepest") && ! pm.getUserFolders().contains (".hidden"), "...and the folder list agrees");
            user.getChildFile ("Old").deleteRecursively();
            user.getChildFile (".hidden").deleteRecursively();
            user.getChildFile ("Half.spastrip.tmp").deleteFile();
            pm.rescan();
        }

        // --- moving a preset: favourite follows, the loaded preset stays current, clashes and bad targets are refused.
        pm.save ("Mover", "");
        const auto mover = fileOf (pm, "Mover");
        pm.load (mover);
        ui::settings::setPresetFavorite ("User/Mover", true);
        const auto moved = pm.moveUserPreset (mover, "Kits");
        expect (moved.ok && moved.file == user.getChildFile ("Kits/Mover.spastrip") && ! mover.exists() && moved.file.existsAsFile(), "moveUserPreset moves the file");
        expect (ui::settings::getFavoritePresets().contains ("Kits/Mover") && ! ui::settings::getFavoritePresets().contains ("User/Mover"), "the favourite followed the move (its key's folder part changed)");
        expect (pm.getCurrentName() == "Mover" && pm.getCurrentFile() == moved.file && pm.getCurrentIndex() >= 0 && pm.getPresets()[(size_t) pm.getCurrentIndex()].folder == "Kits" && ! pm.isEdited(),
                "the loaded preset stays current (new folder), still unedited");
        pm.save ("Mover", "");
        expect (! pm.moveUserPreset (user.getChildFile ("Mover.spastrip"), "Kits").ok, "a name clash in the target is refused");
        expect (user.getChildFile ("Mover.spastrip").existsAsFile() && moved.file.existsAsFile(), "...with both files untouched");
        expect (! pm.moveUserPreset (user.getChildFile ("Mover.spastrip"), "Nope").ok && ! pm.moveUserPreset (user.getChildFile ("Mover.spastrip"), "../..").ok,
                "a missing or escaping target is refused");
        expect (pm.moveUserPreset (moved.file, "Kits").ok, "moving to where it already is succeeds as a no-op");
        expect (! pm.moveUserPreset (t.dir.getChildFile ("elsewhere.spastrip"), "Kits").ok, "an unscanned file is refused");
        user.getChildFile ("Mover.spastrip").deleteFile();
        pm.rescan();

        // --- renaming a folder: files, the loaded preset and favourites follow; clashes are refused.
        {
            pm.load (snare.file);
            ui::settings::setPresetFavorite ("Kits/Snare 1", true);
            ui::settings::setPresetFavorite ("Kits/Mover", true);
            expect (! pm.renameUserFolder ("Kits", "Empty").ok && ! pm.renameUserFolder ("Kits", "").ok && ! pm.renameUserFolder ("", "x").ok && ! pm.renameUserFolder ("Nope", "x").ok,
                    "renaming onto an existing name / to nothing / the root / a missing folder is refused");
            const auto r = pm.renameUserFolder ("Kits", "Drum Kits");
            expect (r.ok && ! user.getChildFile ("Kits").exists() && user.getChildFile ("Drum Kits/Acoustic/Snare 1.spastrip").existsAsFile(), "the folder (and its nested one) moved");
            expect (pm.getCurrentName() == "Snare 1" && pm.getCurrentFile() == user.getChildFile ("Drum Kits/Acoustic/Snare 1.spastrip") && pm.getCurrentIndex() >= 0, "the loaded preset follows into the renamed folder");
            const auto favs = ui::settings::getFavoritePresets();
            expect (favs.contains ("Drum Kits/Snare 1") && favs.contains ("Drum Kits/Mover") && ! favs.contains ("Kits/Snare 1") && ! favs.contains ("Kits/Mover"), "favourites inside follow the new top-level name");
            const auto caseOnly = pm.renameUserFolder ("Drum Kits", "DRUM KITS");
            expect (caseOnly.ok && user.getChildFile ("DRUM KITS").isDirectory() && pm.getBanks().contains ("DRUM KITS") && ! pm.getBanks().contains ("Drum Kits"), "a case-only rename works");
            pm.renameUserFolder ("DRUM KITS", "Kits");
        }

        // --- trashing a folder: the presets and their favourites go, the loaded SOUND stays.
        {
            pm.load (user.getChildFile ("Kits/Acoustic/Snare 1.spastrip"));
            phase2bTests::pump (1);
            const auto soundAtLoad = allValues (*proc);
            const auto n = pm.countPresetsInFolder ("Kits/Acoustic");
            expect (n == 2, "(Kits/Acoustic holds Snare 1 and Snare: " + juce::String (n) + ")");
            ui::settings::setPresetFavorite ("Kits/Snare 1", true);
            const auto r = pm.trashUserFolder ("Kits/Acoustic");
            expect (r.ok && r.count == 2 && ! user.getChildFile ("Kits/Acoustic").exists() && user.getChildFile ("Kits").isDirectory(), "the folder (not its parent) is removed; count = presets inside");
            expect (! ui::settings::getFavoritePresets().contains ("Kits/Snare 1"), "...and their favourites are dropped");
            expect (allValues (*proc) == soundAtLoad && pm.getCurrentName() == "Snare 1" && pm.getCurrentFile() == juce::File() && pm.getCurrentIndex() == -1,
                    "the loaded sound is untouched; the identity loses the file that went");
            expect (! pm.trashUserFolder ("").ok && ! pm.trashUserFolder ("Nope").ok, "the root and a missing folder cannot be trashed");
        }

        // --- factory entries are read-only for every folder / file operation.
        {
            pm.setFactoryPresets ({ { "Fact One", "", "<SPAStripPreset name=\"Fact One\" version=\"1\" type=\"Mastering\"><PARAMS/></SPAStripPreset>" },
                                    { "Fact Two", "Dance", "<SPAStripPreset name=\"Fact Two\" version=\"1\"><PARAMS/></SPAStripPreset>" } });
            const auto* f1 = infoOf (pm, "Fact One");
            const auto* f2 = infoOf (pm, "Fact Two");
            expect (f1 != nullptr && f1->isFactory && f1->type == "Mastering" && f1->folder.isEmpty() && f2 != nullptr && f2->bank == "Dance" && f2->type.isEmpty(),
                    "factory presets carry their type (from the embedded document) and bank, no folder");
            expect (! pm.moveUserPreset (juce::File(), "Kits").ok && ! pm.rename (juce::File(), "x").ok && ! pm.deletePreset (juce::File()), "factory presets cannot be moved, renamed or deleted");
            expect (PM::favouriteKey (*f1) != PM::favouriteKey (info ("Fact One", "", "")), "a factory preset's favourite key cannot collide with a user preset of the same name");
            pm.setFactoryPresets ({});
        }
    }

    //==========================================================================
    void presetBrowserModelTest()
    {
        std::cout << "presetBrowserModelTest\n";
        const std::vector<PM::PresetInfo> presets {
            info ("Kick Punch",    "",                  "Drums"),      // 0
            info ("Warm Bass",     "",                  "Bass"),       // 1
            info ("Lead Vox",      "",                  "Vocals"),     // 2
            info ("Mystery",       "",                  ""),           // 3 untyped
            info ("Room Drums",    "Studio",            "Drums"),      // 4
            info ("Bus Glue",      "Studio",            "Mixbus"),     // 5
            info ("Final Polish",  "Studio/Mastering",  "Mastering"),  // 6
            info ("Odd One",       "Sound Design",      ""),           // 7 untyped
            info ("Granular Wash", "Sound Design",      "Creative"),   // 8
            info ("Fact Verb",     "Dance",             "FX", true),   // 9 factory
        };
        const auto names = [&] (const Browser::Filter& f, const juce::StringArray& fav = {})
        {
            auto n = namesOf (presets, Browser::filterIndices (presets, f, fav));
            n.sort (true);
            return n.joinIntoString (",");
        };

        expect (Browser::typeLabelOf (presets[0]) == "Drums" && Browser::typeLabelOf (presets[3]) == "Other", "typeLabelOf: the type, or 'Other'");
        expect (Browser::categoryOf (presets[0]) == "User" && Browser::categoryOf (presets[4]) == "Studio" && Browser::categoryOf (presets[6]) == "Studio" && Browser::categoryOf (presets[9]) == "Dance",
                "categoryOf: User for unfiled, else the top-level folder (a nested folder counts under its top-level one)");

        // --- type filter.
        Browser::Filter drums;
        drums.type = "Drums";
        expect (names (drums) == "Kick Punch,Room Drums", "type filter: Drums (across folders)");
        Browser::Filter other;
        other.type = "Other";
        expect (names (other) == "Mystery,Odd One", "type filter 'Other' = the untyped presets (no type) wherever they live");
        Browser::Filter mastering;
        mastering.type = "Mastering";
        expect (names (mastering) == "Final Polish", "a nested folder's preset is found by type");
        Browser::Filter fxType;
        fxType.type = "FX";
        expect (names (fxType) == "Fact Verb", "factory presets are filtered by type too");
        Browser::Filter noSuchType;
        noSuchType.type = "Guitar";
        expect (names (noSuchType).isEmpty(), "a type nobody has -> an empty list");

        // --- combined with search, category and favourites (AND).
        Browser::Filter drumsRoom = drums;
        drumsRoom.search = "room";
        expect (names (drumsRoom) == "Room Drums", "type + search");
        Browser::Filter drumsKick = drums;
        drumsKick.search = "BASS";
        expect (names (drumsKick).isEmpty(), "type + a search that matches another type's preset -> nothing");
        Browser::Filter studioOnly;
        studioOnly.category = "Studio";
        expect (names (studioOnly) == "Bus Glue,Final Polish,Room Drums", "category filter = the top-level folder, nested included");
        Browser::Filter studioDrums = studioOnly;
        studioDrums.type = "Drums";
        expect (names (studioDrums) == "Room Drums", "type + category");
        Browser::Filter userCat;
        userCat.category = "User";
        expect (names (userCat) == "Kick Punch,Lead Vox,Mystery,Warm Bass", "the 'User' category is the unfiled presets");
        Browser::Filter favs;
        favs.favoritesOnly = true;
        expect (names (favs, { "User/Warm Bass", "Studio/Bus Glue" }) == "Bus Glue,Warm Bass" && names (favs).isEmpty(), "favourites only");
        Browser::Filter favType = favs;
        favType.type = "Bass";
        expect (names (favType, { "User/Warm Bass", "Studio/Bus Glue" }) == "Warm Bass", "favourites + type");
        Browser::Filter byTypeName;
        byTypeName.search = "mixbus";
        expect (names (byTypeName) == "Bus Glue", "the search also matches the type's name");
        Browser::Filter byFolder;
        byFolder.search = "mastering";
        expect (names (byFolder) == "Final Polish", "...and the (nested) folder name");
        Browser::Filter caseSearch;
        caseSearch.search = "kIcK";
        expect (names (caseSearch) == "Kick Punch", "search is case-insensitive");

        // --- the TYPE dropdown offers what is present under the OTHER filters, in the types' order, Other last.
        expect (Browser::availableTypes (presets, {}, {}).joinIntoString (",") == "Drums,Bass,Vocals,FX,Mixbus,Mastering,Creative,Other",
                "availableTypes: only types present, in the fixed order, Other last (" + Browser::availableTypes (presets, {}, {}).joinIntoString (",") + ")");
        Browser::Filter studioSearch;
        studioSearch.category = "Studio";
        expect (Browser::availableTypes (presets, studioSearch, {}).joinIntoString (",") == "Drums,Mixbus,Mastering", "...narrowed by the folder filter");
        Browser::Filter staleType;
        staleType.type = "Guitar";
        expect (Browser::availableTypes (presets, staleType, {}).joinIntoString (",") == "Drums,Bass,Vocals,FX,Mixbus,Mastering,Creative,Other",
                "...and independent of its own current pick");

        // --- grouping by TYPE: types in order, Other last, empty groups absent, presets sorted by name.
        {
            std::vector<int> all;
            for (size_t i = 0; i < presets.size(); ++i) all.push_back ((int) i);
            const auto rows = Browser::buildRows (presets, all, {}, Browser::GroupBy::type, {}, false, false);
            juce::StringArray labels;
            for (const auto& r : rows) if (r.isGroup()) labels.add (r.label + ":" + juce::String (r.count));
            expect (labels.joinIntoString (",") == "Drums:2,Bass:1,Vocals:1,FX:1,Mixbus:1,Mastering:1,Creative:1,Other:2",
                    "group by type: " + labels.joinIntoString (","));
            expect (rows.size() == 8 + 10 && rows[1].presetIndex == 0 && rows[2].presetIndex == 4 && rows[1].depth == 1, "...each group lists its presets (by name), indented one level");
            const auto collapsed = Browser::buildRows (presets, all, {}, Browser::GroupBy::type, { "T:Drums" }, false, false);
            expect (collapsed.size() == rows.size() - 2 && ! collapsed[0].expanded, "a collapsed type group hides its presets");
            const auto searching = Browser::buildRows (presets, all, {}, Browser::GroupBy::type, { "T:Drums" }, true, true);
            expect (searching.size() == rows.size() && searching[0].expanded, "...but a search forces every group open");
            // Filtered: only the matching groups exist.
            const auto some = Browser::buildRows (presets, { 0, 1 }, {}, Browser::GroupBy::type, {}, false, true);
            expect (some.size() == 4 && some[0].label == "Drums" && some[2].label == "Bass", "only groups with matches are shown while filtering");
        }

        // --- grouping by FOLDER: User (root presets, then folders and nested ones), Factory last.
        {
            std::vector<int> all;
            for (size_t i = 0; i < presets.size(); ++i) all.push_back ((int) i);
            const juce::StringArray folders { "Sound Design", "Studio", "Studio/Mastering", "Empty" };
            const auto rows = Browser::buildRows (presets, all, folders, Browser::GroupBy::folders, {}, false, false);
            juce::StringArray seq;
            for (const auto& r : rows) seq.add ((r.isGroup() ? "[" + r.label + "]" : presets[(size_t) r.presetIndex].name) + "@" + juce::String (r.depth));
            expect (seq.joinIntoString (",") == "[User]@0,Kick Punch@1,Lead Vox@1,Mystery@1,Warm Bass@1,[Empty]@1,[Sound Design]@1,Granular Wash@2,Odd One@2,[Studio]@1,Bus Glue@2,Room Drums@2,[Mastering]@2,Final Polish@3,[Factory]@0,[Dance]@1,Fact Verb@2",
                    "group by folder: " + seq.joinIntoString (","));
            expect (rows[0].count == 9 && rows[5].count == 0, "the User count includes nested folders; an empty folder shows with 0 (so it can be dropped into)");
            const auto filtered = Browser::buildRows (presets, { 4 }, folders, Browser::GroupBy::folders, {}, false, true);
            expect (filtered.size() == 3 && filtered[0].label == "User" && filtered[1].label == "Studio" && filtered[2].presetIndex == 4,
                    "while filtering, groups with no match (empty folders, Factory) are hidden");
        }
    }

    //==========================================================================
    // A dozen presets across the types, filed in folders; returns the processor's manager populated.
    void populate (Proc& proc)
    {
        auto& pm = proc.getPresetManager();
        const struct { const char* name; const char* folder; const char* type; } list[] = {
            { "Kick Punch", "", "Drums" },           { "Snare Crack", "", "Drums" },       { "Warm Bass", "", "Bass" },
            { "Lead Vox", "", "Vocals" },            { "Mystery", "", "" },
            { "Room Drums", "Studio", "Drums" },     { "Vocal Air", "Studio", "Vocals" },  { "Bus Glue", "Studio", "Mixbus" },
            { "Final Polish", "Studio/Mastering", "Mastering" }, { "Loud Master", "Studio/Mastering", "Mastering" },
            { "Granular Wash", "Sound Design", "Creative" },     { "FX Riser", "Sound Design", "FX" },
            { "Gtr Amp", "Sound Design", "Guitar" } };
        int i = 0;
        for (const auto& e : list)
        {
            setParam (proc, fx::distDrive, 0.05f + 0.07f * (float) i++);
            pm.save (e.name, e.folder, false, e.type);
        }
        pm.createUserFolder ({}, "Empty Folder");
    }

    int rowOf (Browser& b, const juce::String& name) { return b.findVisibleRow (name); }

    void presetBrowserUiTest()
    {
        std::cout << "presetBrowserUiTest\n";
        ForceLive live;
        TempDir t ("browser");
        SettingsGuard sg;
        auto proc = makeIsolated (t);
        auto& pm = proc->getPresetManager();
        populate (*proc);
        pm.rescan();
        auto ed = openEditor (*proc);
        auto& content = ed->getContent();
        auto& b = content.getPresetBrowser();
        content.setPresetBrowserOpen (true, false);
        b.setBounds (b.getOpenBounds());

        expect (b.getGroupBy() == Browser::GroupBy::folders, "the drawer opens grouped by folder");
        expect (b.getCountText() == "13 of 13 presets", "count: " + b.getCountText());
        expect (b.findGroupRow ("U:") == 0 && b.findGroupRow ("U:Studio") > 0 && b.findGroupRow ("U:Studio/Mastering") > b.findGroupRow ("U:Studio") && b.findGroupRow ("U:Empty Folder") > 0 && b.findGroupRow ("F:") < 0,
                "User, its folders, the nested folder and the empty folder are groups; no Factory group while there is no factory content");
        expect (b.getRow (b.findGroupRow ("U:Studio/Mastering")).depth == b.getRow (b.findGroupRow ("U:Studio")).depth + 1 && b.getRow (b.findGroupRow ("U:")).count == 13,
                "the nested folder is indented one level deeper; the User count includes everything");

        // --- type dropdown: the types present, in order, Other last.
        expect (b.getOfferedTypes().joinIntoString (",") == "Drums,Bass,Vocals,Guitar,FX,Mixbus,Mastering,Creative,Other",
                "TYPE dropdown: " + b.getOfferedTypes().joinIntoString (","));

        // --- filter by type.
        expect (b.setTypeFilter ("Drums"), "pick Drums");
        expect (b.getCountText() == "3 of 13 presets" && rowOf (b, "Kick Punch") >= 0 && rowOf (b, "Room Drums") >= 0 && rowOf (b, "Snare Crack") >= 0 && rowOf (b, "Warm Bass") < 0,
                "only the Drums presets show, from the root and from a folder (" + b.getCountText() + ")");
        expect (b.findGroupRow ("U:Studio/Mastering") < 0 && b.findGroupRow ("U:Empty Folder") < 0 && b.findGroupRow ("U:Studio") >= 0, "folders with no Drums preset are hidden while filtering");
        // combined with search
        b.setSearchText ("room");
        expect (b.getCountText() == "1 of 13 presets" && rowOf (b, "Room Drums") >= 0 && rowOf (b, "Kick Punch") < 0, "type + search combine");
        b.setSearchText ({});
        // the available types follow the other filters
        b.setSearchText ("studio");
        expect (b.getOfferedTypes().joinIntoString (",") == "Drums,Vocals,Mixbus,Mastering", "the TYPE dropdown re-offers only the types under the search (" + b.getOfferedTypes().joinIntoString (",") + ")");
        expect (b.getTypeBoxForTest().getText() == "Drums", "...and keeps the pick while it is still offered");
        b.setSearchText ("kick");
        expect (b.getOfferedTypes().joinIntoString (",") == "Drums", "a narrower search offers fewer types");
        b.setSearchText ("warm");
        expect (b.getOfferedTypes().joinIntoString (",") == "Bass" && b.getTypeBoxForTest().getSelectedId() == 1 && b.getCountText() == "1 of 13 presets",
                "a search under which the picked type vanishes falls back to All Types (" + b.getCountText() + ")");
        b.setSearchText ({});
        expect (b.setTypeFilter ("Other"), "pick Other");
        expect (b.getCountText() == "1 of 13 presets" && rowOf (b, "Mystery") >= 0, "'Other' lists the preset with no type");
        expect (! b.setTypeFilter ("Synth"), "a type nobody has is not offered");
        b.setTypeFilter ({});
        expect (b.getCountText() == "13 of 13 presets", "All Types restores the list");

        // --- folder dropdown + type.
        expect (b.setCategoryFilter ("Studio") && b.getCountText() == "5 of 13 presets", "the folder dropdown = the top-level folder, nested included (" + b.getCountText() + ")");
        expect (b.getOfferedTypes().joinIntoString (",") == "Drums,Vocals,Mixbus,Mastering", "...and narrows the offered types");
        expect (b.setTypeFilter ("Mastering") && b.getCountText() == "2 of 13 presets", "folder + type");
        b.setCategoryFilter ({});
        b.setTypeFilter ({});

        // --- group by type.
        b.setGroupBy (Browser::GroupBy::type);
        {
            juce::StringArray labels;
            for (int i = 0; i < b.getNumRows(); ++i) if (b.getRow (i).isGroup()) labels.add (b.getRow (i).label + ":" + juce::String (b.getRow (i).count));
            expect (labels.joinIntoString (",") == "Drums:3,Bass:1,Vocals:2,Guitar:1,FX:1,Mixbus:1,Mastering:2,Creative:1,Other:1",
                    "grouped by type, in the types' order with Other last: " + labels.joinIntoString (","));
        }
        expect (ui::settings::getPresetGroupMode() == 1, "the grouping is remembered");
        b.setTypeFilter ("Vocals");
        expect (b.getNumRows() == 3 && b.getRow (0).label == "Vocals" && b.presetAtRow (1)->name == "Lead Vox" && b.presetAtRow (2)->name == "Vocal Air", "type filter + type grouping");
        b.setTypeFilter ({});

        // --- prev / next follow the list as grouped, ignoring filters and collapsed groups.
        {
            const auto nameAfter = [&] (const juce::String& from, int steps)
            {
                pm.load (fileOf (pm, from));
                for (int i = 0; i < std::abs (steps); ++i)
                    steps > 0 ? pm.loadNext() : pm.loadPrevious();
                return pm.getCurrentName();
            };
            // Type grouping: Drums (Kick Punch, Room Drums, Snare Crack), Bass, Vocals ...
            expect (nameAfter ("Kick Punch", 1) == "Room Drums" && nameAfter ("Kick Punch", 3) == "Warm Bass" && nameAfter ("Snare Crack", 1) == "Warm Bass",
                    "next steps through the TYPE-grouped list (Drums, then Bass ...)");
            expect (nameAfter ("Kick Punch", -1) == "Mystery" && nameAfter ("Warm Bass", -1) == "Snare Crack", "previous wraps to the end (Other is last) and steps back across groups");
            // A type filter does not shorten the walk.
            b.setTypeFilter ("Drums");
            expect (nameAfter ("Snare Crack", 1) == "Warm Bass", "an active type filter does not restrict prev / next (as in SPASynth)");
            b.setTypeFilter ({});
            // Folder grouping: the User root's presets by name, then the folders.
            b.setGroupBy (Browser::GroupBy::folders);
            expect (nameAfter ("Mystery", 1) == "Snare Crack" && nameAfter ("Warm Bass", 1) == "FX Riser",
                    "next steps through the FOLDER-grouped list: User's own presets, then the folders (" + nameAfter ("Warm Bass", 1) + ")");
            // Collapsing a group does not remove it from the walk.
            b.toggleGroupRow (b.findGroupRow ("U:Sound Design"));
            expect (rowOf (b, "FX Riser") < 0 && nameAfter ("Warm Bass", 1) == "FX Riser", "presets inside a collapsed group are still visited by next");
            b.toggleGroupRow (b.findGroupRow ("U:Sound Design"));
        }

        // --- rename inside a type filter: the row stays (type kept), under its new name.
        b.setTypeFilter ("Drums");
        {
            const auto r = pm.rename (fileOf (pm, "Room Drums"), "Studio Kit");
            expect (r.ok, "rename");
            b.refresh();
            expect (rowOf (b, "Studio Kit") >= 0 && rowOf (b, "Room Drums") < 0 && b.getCountText() == "3 of 13 presets" && b.getTypeBoxForTest().getText() == "Drums",
                    "after a rename the row is still listed under the active type filter, and the filter is kept");
        }
        // --- delete inside a type filter: the row goes, the count follows, the loaded sound stays.
        {
            pm.load (fileOf (pm, "Snare Crack"));
            const auto sound = allValues (*proc);
            expect (b.deleteRow (rowOf (b, "Snare Crack")), "delete the loaded Snare Crack from the filtered list");
            expect (rowOf (b, "Snare Crack") < 0 && b.getCountText() == "2 of 12 presets" && b.getTypeBoxForTest().getText() == "Drums", "its row is gone at once; the filter and counts are re-applied (" + b.getCountText() + ")");
            expect (allValues (*proc) == sound && pm.getCurrentName() == "Snare Crack" && pm.getCurrentIndex() == -1, "the loaded sound is left as it is; the cursor is cleared");
            pm.loadNext();
            expect (pm.getCurrentName() == "Kick Punch", "next after a delete starts from the first preset of the list again (" + pm.getCurrentName() + ")");
            expect (! b.deleteRow (-1) && ! b.deleteRow (b.findGroupRow ("U:")), "a group row cannot be deleted");
        }
        // --- retyping inside a filter: the preset moves out; when the type empties it is no longer offered.
        {
            expect (b.applyTypeToRow (rowOf (b, "Kick Punch"), "Bass"), "set Kick Punch to Bass");
            expect (rowOf (b, "Kick Punch") < 0 && rowOf (b, "Studio Kit") >= 0 && b.getCountText() == "1 of 12 presets", "it leaves the Drums list (" + b.getCountText() + ")");
            expect (b.applyTypeToRow (rowOf (b, "Studio Kit"), "Other"), "'Other' clears the type");
            expect (b.getTypeBoxForTest().getSelectedId() == 1 && ! b.getOfferedTypes().contains ("Drums") && b.getCountText() == "12 of 12 presets",
                    "when no Drums preset is left the type is no longer offered and the filter falls back to All Types");
            expect (b.getOfferedTypes().contains ("Other") && infoOf (pm, "Studio Kit")->type.isEmpty() && ! rootHasAttr (fileOf (pm, "Studio Kit"), "type"), "...the retyped preset is listed under Other");
        }
        b.setTypeFilter ({});

        // --- favourites: the star toggles, filters, follows a rename, goes with a delete.
        {
            b.setGroupBy (Browser::GroupBy::folders);
            b.toggleFavouriteRow (rowOf (b, "Warm Bass"));
            b.toggleFavouriteRow (rowOf (b, "Bus Glue"));
            expect (ui::settings::getFavoritePresets().contains ("User/Warm Bass") && ui::settings::getFavoritePresets().contains ("Studio/Bus Glue"), "starring stores '<folder or User>/<name>'");
            b.setFavouritesOnly (true);
            expect (b.getCountText() == "2 of 12 presets" && rowOf (b, "Warm Bass") >= 0 && rowOf (b, "Bus Glue") >= 0, "the star filter shows only favourites");
            b.toggleFavouriteRow (rowOf (b, "Bus Glue"));
            expect (rowOf (b, "Bus Glue") < 0 && b.getCountText() == "1 of 12 presets", "un-starring removes it from the favourites view at once");
            b.setFavouritesOnly (false);
            pm.rename (fileOf (pm, "Warm Bass"), "Sub Bass");
            b.refresh();
            expect (ui::settings::getFavoritePresets().contains ("User/Sub Bass") && ! ui::settings::getFavoritePresets().contains ("User/Warm Bass"), "a rename moves the star");
            b.deleteRow (rowOf (b, "Sub Bass"));
            expect (! ui::settings::getFavoritePresets().contains ("User/Sub Bass"), "a delete drops it (it must not re-attach to a later preset of that name)");
            pm.save ("Sub Bass", "");
            expect (! ui::settings::getFavoritePresets().contains ("User/Sub Bass"), "...so a new preset with that name is not starred");
        }

        // --- groups: collapsing is remembered and a search forces them open.
        {
            ui::settings::setPresetGroupCollapsed ("U:Studio", false);
            b.toggleGroupRow (b.findGroupRow ("U:Studio"));
            expect (ui::settings::getCollapsedPresetGroups().contains ("U:Studio") && rowOf (b, "Vocal Air") < 0 && rowOf (b, "Final Polish") < 0, "collapsing persists and hides the folder's presets and its nested folder");
            b.setSearchText ("vocal air");
            expect (rowOf (b, "Vocal Air") >= 0, "a search reveals a match inside a collapsed folder");
            b.toggleGroupRow (b.findGroupRow ("U:Studio"));
            expect (ui::settings::getCollapsedPresetGroups().contains ("U:Studio"), "...and does not change the saved state");
            b.setSearchText ({});
            b.toggleGroupRow (b.findGroupRow ("U:Studio"));
            expect (rowOf (b, "Vocal Air") >= 0 && ! ui::settings::getCollapsedPresetGroups().contains ("U:Studio"), "expanding shows them again");
        }

        // --- the right-click menus: user rows can be changed, factory rows only exported.
        {
            const auto itemState = [] (juce::PopupMenu& m, const juce::String& text, bool& found)
            {
                found = false;
                bool enabled = false;
                juce::PopupMenu::MenuItemIterator it (m, true);
                while (it.next())
                    if (it.getItem().text == text) { found = true; enabled = it.getItem().isEnabled; }
                return enabled;
            };
            bool f = false;
            juce::StringArray typeNames, folders;
            auto menu = b.buildRowMenu (rowOf (b, "Kick Punch"), &typeNames, &folders);
            expect (itemState (menu, "Rename...", f) && f && itemState (menu, "Move to Trash", f) && f && itemState (menu, "Export preset...", f) && f,
                    "a user row: Rename, Move to Trash, Export are enabled");
            expect (typeNames == PM::presetTypes() && folders.contains ("Studio/Mastering") && folders.contains ("Empty Folder"), "'Set type' offers the ten types; 'Move to Folder' every user folder");
            expect (itemState (menu, "Set type", f) && f && itemState (menu, "Move to Folder", f) && f, "...both submenus are enabled");
            expect (b.buildRowMenu (-1).getNumItems() == 0 && b.buildRowMenu (b.findGroupRow ("U:")).getNumItems() == 0, "no row menu for nothing / a group row");
            auto gm = b.buildGroupMenu (b.findGroupRow ("U:Studio"));
            expect (itemState (gm, "New Folder...", f) && f && itemState (gm, "Rename Folder...", f) && f && itemState (gm, "Move Folder to Trash", f) && f, "a user folder row: New / Rename / Trash");
            auto nested = b.buildGroupMenu (b.findGroupRow ("U:Studio/Mastering"));
            expect (! itemState (nested, "New Folder...", f) && f, "...a nested (second-level) folder cannot hold another folder");
            auto rootMenu = b.buildGroupMenu (b.findGroupRow ("U:"));
            expect (itemState (rootMenu, "New Folder...", f) && f && ! itemState (rootMenu, "Rename Folder...", f) && f && ! itemState (rootMenu, "Move Folder to Trash", f) && f, "the User root: New Folder only");
            expect (b.buildBackgroundMenu().getNumItems() == 1, "right-click on empty space: New Folder");

            pm.setFactoryPresets ({ { "Fact Verb", "Dance", "<SPAStripPreset name=\"Fact Verb\" version=\"1\" type=\"FX\"><PARAMS/></SPAStripPreset>" } });
            b.refresh();
            const auto fr = rowOf (b, "Fact Verb");
            expect (fr > 0 && b.findGroupRow ("F:") >= 0 && b.findGroupRow ("F:Dance") >= 0 && ! b.canModifyRow (fr), "a Factory group appears with its content; factory rows cannot be modified");
            auto fm = b.buildRowMenu (fr);
            expect (! itemState (fm, "Rename...", f) && f && ! itemState (fm, "Move to Trash", f) && f && ! itemState (fm, "Set type", f) && f && ! itemState (fm, "Move to Folder", f) && f && itemState (fm, "Export preset...", f) && f,
                    "a factory row: Rename / Trash / Set type / Move are disabled, Export works");
            expect (! b.deleteRow (fr) && ! b.applyTypeToRow (fr, "Bass"), "...and the actions refuse");
            juce::SparseSet<int> sel;
            sel.addRange ({ fr, fr + 1 });
            expect (b.getDragSourceDescription (sel).isVoid(), "a factory row is not draggable");
            juce::String rel;
            expect (! b.dropTargetFolderForRow (fr, rel) && ! b.dropTargetFolderForRow (b.findGroupRow ("F:"), rel) && ! b.dropTargetFolderForRow (b.findGroupRow ("F:Dance"), rel), "...and factory rows / groups are no drop target");
            auto fg = b.buildGroupMenu (b.findGroupRow ("F:Dance"));
            expect (fg.getNumItems() > 0 && ! itemState (fg, "New Folder...", f) && f && ! itemState (fg, "Rename Folder...", f) && f, "a factory group's menu is shown but disabled");
            pm.setFactoryPresets ({});
            b.refresh();
        }

        // --- drag and drop: user rows in the folder view only; folder rows and presets are targets.
        {
            juce::SparseSet<int> sel;
            const auto kr = rowOf (b, "Kick Punch");
            sel.addRange ({ kr, kr + 1 });
            expect (b.getDragSourceDescription (sel).isArray() && b.getDragSourceDescription (sel).size() == 1, "a user preset row is draggable");
            juce::SparseSet<int> gsel;
            gsel.addRange ({ 0, 1 });
            expect (b.getDragSourceDescription (gsel).isVoid(), "a folder row is not");
            juce::String rel;
            expect (b.dropTargetFolderForRow (b.findGroupRow ("U:Studio"), rel) && rel == "Studio" && b.dropTargetFolderForRow (b.findGroupRow ("U:"), rel) && rel.isEmpty()
                        && b.dropTargetFolderForRow (rowOf (b, "Final Polish"), rel) && rel == "Studio/Mastering",
                    "folder rows, the User root and a preset (-> its folder) are drop targets");
            const auto s = b.moveFilesToFolder ({ fileOf (pm, "Kick Punch"), t.dir.getChildFile ("none.spastrip") }, "Sound Design", false);
            expect (s.moved == 1 && s.refused.size() == 1 && infoOf (pm, "Kick Punch")->folder == "Sound Design" && rowOf (b, "Kick Punch") > b.findGroupRow ("U:Sound Design"),
                    "dropping on a folder moves the presets that can move and reports the rest; the list shows it under its new folder");
            b.setGroupBy (Browser::GroupBy::type);
            expect (b.getDragSourceDescription (sel).isVoid() && ! b.dropTargetFolderForRow (0, rel) && b.buildGroupMenu (0).getNumItems() == 0 && b.buildBackgroundMenu().getNumItems() == 0,
                    "in the Type grouping nothing is draggable / droppable and there are no folder menus");
            b.setGroupBy (Browser::GroupBy::folders);
        }

        // --- the keyboard: arrows move the highlight, Return loads, Esc closes.
        {
            const auto first = rowOf (b, "Gtr Amp");
            b.selectRowForTest (first);
            expect (b.keyPressed (juce::KeyPress (juce::KeyPress::downKey)), "Down is handled by the drawer");
            expect (b.getListForTest().getSelectedRow() == first + 1, "...and moves the highlight one row (" + juce::String (b.getListForTest().getSelectedRow()) + " vs " + juce::String (first + 1) + ")");
            const auto target = b.presetAtRow (b.getListForTest().getSelectedRow());
            const auto expectName = target != nullptr ? target->name : juce::String ("<group>");
            b.keyPressed (juce::KeyPress (juce::KeyPress::returnKey));
            expect (target != nullptr ? pm.getCurrentName() == expectName : true, "Return loads the highlighted preset (" + pm.getCurrentName() + ")");
        }

        // --- nothing from the synth in the drawer.
        {
            juce::StringArray texts;
            std::function<void (juce::Component&)> walk = [&] (juce::Component& c)
            {
                if (auto* btn = dynamic_cast<juce::Button*> (&c)) texts.add (btn->getButtonText() + " " + btn->getTooltip());
                if (auto* lab = dynamic_cast<juce::Label*> (&c)) texts.add (lab->getText());
                if (auto* cb = dynamic_cast<juce::ComboBox*> (&c)) { for (int i = 0; i < cb->getNumItems(); ++i) texts.add (cb->getItemText (i)); texts.add (cb->getTextWhenNothingSelected()); texts.add (cb->getTooltip()); }
                for (auto* ch : c.getChildren()) walk (*ch);
            };
            walk (b);
            const auto all = texts.joinIntoString (" | ").toLowerCase();
            const auto words = juce::StringArray::fromTokens (all, " |,.()'\"/-:", "");   // whole words ("impulse" is not "pulse")
            bool clean = true;
            for (const char* word : { "spasynth", "library", "pack", "packs", "texture", "pulse", "rescan" })
                clean = clean && ! words.contains (word);
            expect (clean, "the drawer's controls mention no SPASynth concept (library, packs, Keys/Texture/Pulse flavours): " + all.substring (0, 400));
        }
    }

    //==========================================================================
    void presetSaveDialogTest()
    {
        std::cout << "presetSaveDialogTest\n";
        ForceLive live;
        TempDir t ("savedlg");
        SettingsGuard sg;
        auto proc = makeIsolated (t);
        auto& pm = proc->getPresetManager();
        const auto user = t.dir.getChildFile ("User");
        auto ed = openEditor (*proc);
        auto& content = ed->getContent();
        const auto dlg = [&] { return dynamic_cast<ui::SaveDialog*> (content.getDialogForTest()); };
        const auto select = [] (juce::ComboBox& cb, const juce::String& text)
        {
            for (int i = 0; i < cb.getNumItems(); ++i)
                if (cb.getItemText (i) == text) { cb.setSelectedId (cb.getItemId (i), juce::sendNotificationSync); return true; }
            return false;
        };

        // --- the panel: TYPE offers (none) + the ten types; FOLDER offers unfiled / Auto / New folder.
        content.showSaveDialog (true);
        auto* d = dlg();
        expect (d != nullptr, "SAVE on an unsaved patch opens the panel");
        if (d == nullptr)
            return;
        {
            juce::StringArray types;
            for (int i = 0; i < d->getTypeBox().getNumItems(); ++i) types.add (d->getTypeBox().getItemText (i));
            expect (types.joinIntoString (",") == "(none),Drums,Bass,Vocals,Guitar,Keys,Synth,FX,Mixbus,Mastering,Creative", "TYPE: " + types.joinIntoString (","));
            expect (d->getFolderBox().getItemText (0) == "User (unfiled)" && select (d->getFolderBox(), "Auto (by type)") && select (d->getFolderBox(), "New folder...") && select (d->getFolderBox(), "User (unfiled)"),
                    "FOLDER: User (unfiled), Auto (by type), New folder...");
        }
        expect (d->getNameEditor().getText() == "Preset 1" && d->getHintForTest().contains ("OTHER"), "default name 'Preset 1'; the hint says it will show under OTHER (" + d->getHintForTest() + ")");

        // --- the default name follows the TYPE until the user types one.
        expect (select (d->getTypeBox(), "Drums") && d->getNameEditor().getText() == "Drums 1" && d->getHintForTest().contains ("DRUMS"), "TYPE Drums -> 'Drums 1', the hint names DRUMS");
        pm.save ("Drums 1", "", false, "Drums");
        select (d->getTypeBox(), "Bass");
        select (d->getTypeBox(), "Drums");
        expect (d->getNameEditor().getText() == "Drums 2", "...skipping a taken name (Drums 2)");
        expect (! d->isNameOwnedForTest(), "(programmatic text is not the user's)");
        typeInto (d->getNameEditor(), "My Drum Bus");
        expect (d->isNameOwnedForTest(), "typing makes the name the user's");
        select (d->getTypeBox(), "Mixbus");
        expect (d->getNameEditor().getText() == "My Drum Bus", "...so a TYPE change leaves it alone");

        // --- Save: lands in User/ with the type; the panel closes; the patch is the unedited loaded one.
        setParam (*proc, fx::reverbMix, 0.66f);
        d->submit (false);
        pump (30);
        expect (user.getChildFile ("My Drum Bus.spastrip").existsAsFile() && rootAttr (user.getChildFile ("My Drum Bus.spastrip"), "type") == "Mixbus", "Save wrote User/My Drum Bus.spastrip with type=\"Mixbus\"");
        expect (pm.getCurrentName() == "My Drum Bus" && ! pm.isEdited() && infoOf (pm, "My Drum Bus")->type == "Mixbus", "the saved preset is the loaded one, unedited, typed");
        expect (content.getDialogForTest() == nullptr || ! content.getDialogForTest()->isVisible(), "the panel closed");

        // --- Save As from a loaded preset: its folder, its type and its name (free variant) are pre-selected.
        content.showSaveDialog (true);
        d = dlg();
        expect (d != nullptr && d->getNameEditor().getText() == "My Drum Bus 2" && d->getTypeBox().getText() == "Mixbus" && d->getFolderBox().getText() == "User (unfiled)",
                "Save As on a loaded preset: its name (a free variant), its type and its folder are pre-selected");
        d->submit (false);
        pump (30);
        expect (user.getChildFile ("My Drum Bus 2.spastrip").existsAsFile(), "...and saves as that");

        // --- clash: inline warning + Replace; nothing overwritten silently.
        content.showSaveDialog (true);
        d = dlg();
        typeInto (d->getNameEditor(), "My Drum Bus");
        select (d->getTypeBox(), "Vocals");
        d->submit (false);
        expect (dlg() == d && d->getWarningForTest().contains ("already exists") && d->isReplaceVisibleForTest() && rootAttr (user.getChildFile ("My Drum Bus.spastrip"), "type") == "Mixbus",
                "a clash keeps the panel open with a warning and a Replace button; the file is untouched");
        d->submit (true);
        pump (30);
        expect (rootAttr (user.getChildFile ("My Drum Bus.spastrip"), "type") == "Vocals", "Replace rewrote the file with the new type");

        // --- empty name.
        content.showSaveDialog (true);
        d = dlg();
        typeInto (d->getNameEditor(), "   ");
        d->submit (false);
        expect (dlg() == d && d->getWarningForTest().contains ("name"), "an empty name is refused inline");
        content.dismissDialog();

        // --- Auto (by type): files into User/<Type>/ only when chosen; the default is not Auto.
        pm.init();
        content.showSaveDialog (true);
        d = dlg();
        select (d->getTypeBox(), "Guitar");
        expect (d->getFolderBox().getText() == "User (unfiled)", "the default folder is User (unfiled), not Auto");
        typeInto (d->getNameEditor(), "Plain Gtr");
        d->submit (false);
        pump (30);
        expect (user.getChildFile ("Plain Gtr.spastrip").existsAsFile() && ! user.getChildFile ("Guitar").exists(), "unfiled save lands in the User root, no type folder");

        pm.init();
        content.showSaveDialog (true);
        d = dlg();
        select (d->getTypeBox(), "Guitar");
        select (d->getFolderBox(), "Auto (by type)");
        expect (d->getNameEditor().getText() == "Guitar 1", "(name checked against User/Guitar)");
        typeInto (d->getNameEditor(), "Auto Gtr");
        d->submit (false);
        pump (30);
        expect (user.getChildFile ("Guitar/Auto Gtr.spastrip").existsAsFile() && infoOf (pm, "Auto Gtr")->folder == "Guitar" && infoOf (pm, "Auto Gtr")->type == "Guitar",
                "Auto (by type) filed it into User/Guitar/ (created)");
        pm.init();
        content.showSaveDialog (true);
        d = dlg();
        expect (d->getFolderBox().getSelectedId() == d->getAutoItemId(), "Auto stays selected for the next Save As");
        content.dismissDialog();

        // --- New folder...
        pm.init();
        content.showSaveDialog (true);
        d = dlg();
        select (d->getFolderBox(), "New folder...");
        d->submit (false);
        expect (dlg() == d && d->getWarningForTest().contains ("folder"), "'New folder...' with no name is refused inline");
        typeInto (d->getNewFolderEditor(), "Fresh Kit");
        typeInto (d->getNameEditor(), "In Fresh");
        select (d->getTypeBox(), "Drums");
        d->submit (false);
        pump (30);
        expect (user.getChildFile ("Fresh Kit/In Fresh.spastrip").existsAsFile() && infoOf (pm, "In Fresh")->folder == "Fresh Kit", "New folder created and used");
        content.showSaveDialog (true);
        d = dlg();
        expect (d->getFolderBox().getText() == "Fresh Kit", "Save As on a preset in a folder pre-selects that folder");
        content.dismissDialog();
    }

    //==========================================================================
    void presetExportImportTest()
    {
        std::cout << "presetExportImportTest\n";
        ForceLive live;
        TempDir t ("impexp");
        TempDir ext ("external");
        SettingsGuard sg;
        auto proc = makeIsolated (t);
        auto& pm = proc->getPresetManager();
        const auto user = t.dir.getChildFile ("User");
        dirtyPatch (*proc);
        pm.save ("Alpha", "", false, "Drums");
        pm.save ("Beta", "Studio", false, "Mixbus");
        pm.save ("Gamma", "Studio/Sub", false, "FX");

        // --- export a preset: verbatim copy.
        juce::String err;
        const auto exported = ext.dir.getChildFile ("Alpha.spastrip");
        expect (pm.exportPreset (*infoOf (pm, "Alpha"), exported, &err) && exported.loadFileAsString() == fileOf (pm, "Alpha").loadFileAsString(), "export copies the file verbatim");
        pm.setFactoryPresets ({ { "Fact", "", "<SPAStripPreset name=\"Fact\" version=\"1\"><PARAMS/></SPAStripPreset>" } });
        expect (pm.exportPreset (*infoOf (pm, "Fact"), ext.dir.getChildFile ("Fact.spastrip")) && ext.dir.getChildFile ("Fact.spastrip").loadFileAsString().contains ("name=\"Fact\""), "a factory preset can be exported");
        pm.setFactoryPresets ({});

        // --- export a folder as a zip, import it into a fresh library: same layout, types kept.
        const auto zip = ext.dir.getChildFile ("Studio.zip");
        expect (pm.exportFolder ("Studio", zip, &err) && zip.existsAsFile(), "export a folder as a zip (" + err + ")");
        expect (! pm.exportFolder ("Nope", ext.dir.getChildFile ("n.zip")) && ! pm.exportFolder ("", ext.dir.getChildFile("n.zip")), "a missing folder / the root cannot be exported");
        {
            TempDir t2 ("impexp2");
            auto proc2 = makeIsolated (t2);
            auto& pm2 = proc2->getPresetManager();
            auto session = pm2.beginImport ({ zip, exported });
            auto step = session->advance();
            expect (step.finished && ! step.awaitingDecision, "importing into an empty library needs no decisions");
            const auto res = session->finish();
            expect (res.imported == 3 && res.malformed.isEmpty() && res.rejectedZipSlip.isEmpty(), "a zip pack + a loose file import (" + juce::String (res.imported) + ")");
            const auto* b2 = infoOf (pm2, "Beta");
            const auto* g2 = infoOf (pm2, "Gamma");
            expect (b2 != nullptr && b2->folder == "Studio" && b2->type == "Mixbus" && g2 != nullptr && g2->folder == "Studio/Sub" && g2->type == "FX" && infoOf (pm2, "Alpha")->folder.isEmpty() && infoOf (pm2, "Alpha")->type == "Drums",
                    "the zip became a folder named after it, keeping its sub-folders; types and the loose file (-> User root) arrived intact");
            expect (pm2.load (fileOf (pm2, "Gamma")).ok, "an imported preset loads");
            // --- clashes: the session pauses, one decision per clash or for all.
            auto s2 = pm2.beginImport ({ exported });
            auto st = s2->advance();
            expect (! st.finished && st.awaitingDecision && st.clashName == "Alpha", "a name clash pauses the import and names the preset");
            st = s2->advance();
            expect (st.awaitingDecision, "...and stays paused until it is decided (nothing consumed)");
            s2->decide (PM::ImportClash::keepBoth, false);
            st = s2->advance();
            expect (st.finished, "after the decision it finishes");
            expect (s2->finish().imported == 1 && fileOf (pm2, "Alpha 2").existsAsFile() && fileOf (pm2, "Alpha").existsAsFile(), "Keep both -> 'Alpha 2' beside 'Alpha'");
            auto s3 = pm2.beginImport ({ exported });
            s3->advance();
            s3->decide (PM::ImportClash::skip, false);
            expect (s3->advance().finished && s3->finish().imported == 0, "Skip imports nothing");
            auto s4 = pm2.beginImport ({ exported });
            const auto before = fileOf (pm2, "Alpha").loadFileAsString();
            fileOf (pm2, "Alpha").replaceWithText ("<SPAStripPreset name=\"Alpha\" version=\"1\"><PARAMS/></SPAStripPreset>");
            s4->advance();
            s4->decide (PM::ImportClash::replace, false);
            expect (s4->advance().finished && s4->finish().imported == 1 && fileOf (pm2, "Alpha").loadFileAsString() == before, "Replace overwrites it");
            // apply-to-rest: two clashing files, ONE decision.
            exported.copyFileTo (ext.dir.getChildFile ("Alpha Again.spastrip"));   // (its name attribute is "Alpha" too)
            auto s5 = pm2.beginImport ({ exported, ext.dir.getChildFile ("Alpha Again.spastrip") });
            expect (s5->advance().awaitingDecision, "(the first clash pauses)");
            s5->decide (PM::ImportClash::keepBoth, true);
            expect (s5->advance().finished && s5->finish().imported == 2 && fileOf (pm2, "Alpha 3").existsAsFile() && fileOf (pm2, "Alpha 4").existsAsFile(),
                    "'apply to the rest' answers every later clash of the session with the one decision");
        }

        // --- importing a FOLDER: becomes a folder named after it; deeper levels are flattened to the allowed depth.
        {
            const auto src = ext.dir.getChildFile ("My Pack");
            src.getChildFile ("Drums/Kicks/Deep/Deeper").createDirectory();
            copyAs (exported, src.getChildFile ("Top.spastrip"), "Top");
            copyAs (exported, src.getChildFile ("Drums/Kicks/Deep/Deeper/Way Down.spastrip"), "Way Down");
            src.getChildFile ("readme.txt").replaceWithText ("hello");
            TempDir t3 ("impexp3");
            auto p3 = makeIsolated (t3);
            auto s = p3->getPresetManager().beginImport ({ src });
            expect (s->advance().finished, "a folder import with no clashes runs to the end");
            const auto res = s->finish();
            expect (res.imported == 2 && res.malformed.isEmpty(), "both presets imported, readme.txt ignored (" + juce::String (res.imported) + ")");
            const auto* top = infoOf (p3->getPresetManager(), "Top");
            const auto* deep = infoOf (p3->getPresetManager(), "Way Down");
            expect (top != nullptr && top->folder == "My Pack" && deep != nullptr && deep->folder.startsWith ("My Pack/") && juce::StringArray::fromTokens (deep->folder, "/", "").size() <= PM::maxFolderDepth,
                    "folder 'My Pack'; nested levels are flattened to the depth the app creates (" + (deep != nullptr ? deep->folder : juce::String ("-")) + ")");
        }

        // --- what is NOT importable: another product's preset, garbage, wrong extension, zip-slip.
        {
            TempDir t4 ("impexp4");
            auto p4 = makeIsolated (t4);
            auto& m4 = p4->getPresetManager();
            const auto synth = ext.dir.getChildFile ("Synth Patch.spasynth");
            synth.replaceWithText ("<SPASynthPreset name=\"Synth Patch\" version=\"1\"><PARAMS><PARAM id=\"fxDist.drive\" value=\"0.9\"/></PARAMS></SPASynthPreset>");
            const auto disguised = ext.dir.getChildFile ("Disguised.spastrip");
            disguised.replaceWithText (synth.loadFileAsString());
            const auto garbage = ext.dir.getChildFile ("Garbage.spastrip");
            garbage.replaceWithText ("this is not xml");
            const auto noState = ext.dir.getChildFile ("NoState.spastrip");
            noState.replaceWithText ("<SPAStripPreset name=\"x\" version=\"1\"/>");
            const auto text = ext.dir.getChildFile ("notes.txt");
            text.replaceWithText ("hi");
            const auto before = allValues (*p4);
            auto s = m4.beginImport ({ synth, disguised, garbage, noState, text });
            expect (s->advance().finished, "(nothing to ask)");
            const auto res = s->finish();
            expect (res.imported == 0 && res.malformed.size() == 5, "a SPASynth preset (by extension or disguised), garbage, an empty state and a stray file are all refused (" + res.malformed.joinIntoString (", ") + ")");
            expect (m4.getPresets().empty() && allValues (*p4) == before, "nothing was written and the sound is untouched");

            // zip-slip: an entry that climbs out is rejected, a good one still imports.
            juce::ZipFile::Builder builder;
            const auto fine = ext.dir.getChildFile ("fine.spastrip");
            copyAs (exported, fine, "Fine");
            builder.addFile (fine, 9, "../../escape.spastrip");
            builder.addFile (fine, 9, "ok/Fine.spastrip");
            builder.addFile (fine, 9, "/abs/Abs.spastrip");
            const auto bad = ext.dir.getChildFile ("Evil.zip");
            {
                juce::FileOutputStream out (bad);
                builder.writeToStream (out, nullptr);
            }
            auto z = m4.beginImport ({ bad });
            expect (z->advance().finished, "(zip run)");
            const auto zr = z->finish();
            expect (zr.imported == 1 && zr.rejectedZipSlip.size() == 2 && infoOf (m4, "Fine") != nullptr && infoOf (m4, "Fine")->folder == "Evil/ok" && ! t4.dir.getChildFile ("escape.spastrip").exists() && ! t4.dir.getChildFile ("User/escape.spastrip").exists(),
                    "zip-slip entries ('../..' and absolute) are rejected, the good entry imports into its folder (" + juce::String (zr.rejectedZipSlip.size()) + " rejected)");
        }
    }

    //==========================================================================
    // The drawer's dialogs end to end: each answer arrives a turn after its dialog closed, and a
    // dialog that opens another one (an error message) must not be destroyed under its own handler.
    void presetDialogFlowsTest()
    {
        std::cout << "presetDialogFlowsTest\n";
        ForceLive live;
        TempDir t ("dialogs");
        TempDir ext ("dialogs-ext");
        SettingsGuard sg;
        auto proc = makeIsolated (t);
        auto& pm = proc->getPresetManager();
        populate (*proc);
        pm.rescan();
        auto ed = openEditor (*proc);
        auto& content = ed->getContent();
        auto& b = content.getPresetBrowser();
        content.setPresetBrowserOpen (true, false);
        const auto user = t.dir.getChildFile ("User");
        const auto dialogGone = [&] { pump (60); return content.getDialogForTest() == nullptr; };

        // --- rename: prompt pre-filled; OK renames; a clash shows an error dialog instead.
        content.showRenameDialog (fileOf (pm, "Kick Punch"));
        auto* prompt = dynamic_cast<ui::TextPromptDialog*> (content.getDialogForTest());
        expect (prompt != nullptr && prompt->getEditor().getText() == "Kick Punch", "Rename opens a prompt with the current name");
        if (prompt != nullptr)
        {
            prompt->getEditor().setText ("Snare Crack", false);
            prompt->getEditor().onReturnKey();
            pump (80);
            auto* msg = dynamic_cast<ui::MessageDialog*> (content.getDialogForTest());
            expect (msg != nullptr && msg->getBodyForTest().contains ("already exists") && user.getChildFile ("Kick Punch.spastrip").existsAsFile(),
                    "renaming onto an existing preset is refused with a message; nothing changed");
            content.dismissDialog();
        }
        content.showRenameDialog (fileOf (pm, "Kick Punch"));
        prompt = dynamic_cast<ui::TextPromptDialog*> (content.getDialogForTest());
        if (prompt != nullptr)
        {
            prompt->getEditor().setText ("Kick Two", false);
            prompt->getEditor().onReturnKey();
            expect (dialogGone() && user.getChildFile ("Kick Two.spastrip").existsAsFile() && ! user.getChildFile ("Kick Punch.spastrip").exists(), "OK renames the preset and closes the prompt");
        }

        // --- new folder from the drawer: valid name creates it, an illegal one reports.
        b.promptNewFolder ({});
        prompt = dynamic_cast<ui::TextPromptDialog*> (content.getDialogForTest());
        expect (prompt != nullptr, "New Folder opens a prompt");
        if (prompt != nullptr)
        {
            prompt->getEditor().setText ("Fresh", false);
            prompt->getEditor().onReturnKey();
            expect (dialogGone() && user.getChildFile ("Fresh").isDirectory() && b.findGroupRow ("U:Fresh") >= 0, "OK creates the folder and the drawer lists it");
        }
        b.promptNewFolder ("Studio/Mastering");   // a third level
        prompt = dynamic_cast<ui::TextPromptDialog*> (content.getDialogForTest());
        if (prompt != nullptr)
        {
            prompt->getEditor().setText ("Too Deep", false);
            prompt->getEditor().onReturnKey();
            pump (80);
            auto* msg = dynamic_cast<ui::MessageDialog*> (content.getDialogForTest());
            expect (msg != nullptr && msg->getBodyForTest().contains ("nested") && ! user.getChildFile ("Studio/Mastering/Too Deep").exists(), "a third level is refused with a message");
            content.dismissDialog();
        }
        b.promptRenameFolder ("Fresh");
        prompt = dynamic_cast<ui::TextPromptDialog*> (content.getDialogForTest());
        if (prompt != nullptr)
        {
            expect (prompt->getEditor().getText() == "Fresh", "Rename Folder is pre-filled with the folder's name");
            prompt->getEditor().setText ("Renamed", false);
            prompt->getEditor().onReturnKey();
            expect (dialogGone() && user.getChildFile ("Renamed").isDirectory() && ! user.getChildFile ("Fresh").exists(), "Rename Folder renames it");
        }

        // --- trash a folder: asks first; Cancel / Esc leaves it, OK trashes it (and drops its presets).
        b.confirmTrashFolder ("Sound Design");
        auto* confirm = dynamic_cast<ui::ConfirmDialog*> (content.getDialogForTest());
        expect (confirm != nullptr && confirm->getBodyForTest().contains ("3 presets") && confirm->getBodyForTest().contains ("Sound Design"), "the question names the folder and how many presets go with it");
        content.dismissDialog();
        expect (user.getChildFile ("Sound Design").isDirectory(), "(dismissing the question trashed nothing)");
        b.confirmTrashFolder ("Sound Design");
        confirm = dynamic_cast<ui::ConfirmDialog*> (content.getDialogForTest());
        if (confirm != nullptr)
        {
            confirm->accept();
            expect (dialogGone() && ! user.getChildFile ("Sound Design").exists() && b.findGroupRow ("U:Sound Design") < 0 && infoOf (pm, "Granular Wash") == nullptr, "OK moves the folder away and the drawer follows");
        }

        // --- importing a clashing preset: the question appears, the answer resumes the import, a summary follows.
        const auto src = ext.dir.getChildFile ("Kick Two.spastrip");
        fileOf (pm, "Kick Two").copyFileTo (src);
        const auto src2 = ext.dir.getChildFile ("Warm Bass.spastrip");
        fileOf (pm, "Warm Bass").copyFileTo (src2);
        b.importFromPaths ({ src, src2 });
        auto* clash = dynamic_cast<ui::ClashDialog*> (content.getDialogForTest());
        expect (clash != nullptr, "an import that clashes asks what to do");
        if (clash != nullptr)
        {
            clash->setApplyToRest (true);
            clash->choose (PM::ImportClash::keepBoth);
            pump (120);
            auto* done = dynamic_cast<ui::MessageDialog*> (content.getDialogForTest());
            expect (done != nullptr && done->getBodyForTest().contains ("2 presets imported") && infoOf (pm, "Kick Two 2") != nullptr && infoOf (pm, "Warm Bass 2") != nullptr,
                    "one 'Keep both' for the rest imported both as '<name> 2' and a summary followed");
            content.dismissDialog();
        }
        b.importFromPaths ({ ext.dir.getChildFile ("missing.spastrip") });
        pump (80);
        auto* bad = dynamic_cast<ui::MessageDialog*> (content.getDialogForTest());
        expect (bad != nullptr && bad->getBodyForTest().contains ("0 presets imported") && bad->getBodyForTest().contains ("Skipped"), "an import that finds nothing usable says so");
        content.dismissDialog();
        pump (60);
    }

    //==========================================================================
    // A preset file written by an older build (the nine-module chain order) still loads.
    void presetLegacyOrderLoadTest()
    {
        std::cout << "presetLegacyOrderLoadTest\n";
        TempDir t ("legacyorder");
        SettingsGuard sg;
        auto proc = makeIsolated (t);
        auto& pm = proc->getPresetManager();
        const auto saved = pm.save ("Old Order", "", false, "Creative");
        const auto packIds = [] (const std::vector<int>& ids) { juce::int64 v = 0; for (size_t i = 0; i < ids.size(); ++i) v |= (juce::int64) ids[i] << (i * 4); return v; };
        const struct { std::vector<int> ids; std::vector<int> expected; const char* what; } cases[] = {
            { { 8, 7, 4, 3, 6, 5, 2, 1, 0 }, { 11, 8, 9, 7, 4, 3, 6, 5, 10, 2, 1, 0 }, "a custom nine-module order" },
            { { 0, 1, 5, 6, 2, 3, 8, 4, 7 }, { 11, 0, 1, 5, 6, 10, 2, 3, 8, 4, 9, 7 }, "the old nine-module default" },
            { { 7, 9, 3, 4, 8, 10, 2, 5, 6, 1, 0 }, { 11, 7, 9, 3, 4, 8, 10, 2, 5, 6, 1, 0 }, "a custom eleven-module order (before the FILTER module)" },
        };
        for (const auto& c : cases)
        {
            auto xml = juce::XmlDocument::parse (saved.file);
            xml->getFirstChildElement()->setAttribute ("fxOrder", juce::String (packIds (c.ids)));
            const auto f = t.dir.getChildFile ("User/Legacy.spastrip");
            xml->writeTo (f);
            expect (pm.load (f).ok, juce::String (c.what) + ": the preset loads");
            const auto order = proc->getFxOrder();
            bool same = order.size() == (int) c.expected.size();
            for (int i = 0; same && i < order.size(); ++i)
                same = order[i] == c.expected[(size_t) i];
            expect (same, juce::String (c.what) + " migrates to the twelve-module chain (FILTER first, GRAIN / COMP slotted in)");
        }
        expect (rootAttr (saved.file, "type") == "Creative", "(and the type of the original is intact)");
    }

    //==========================================================================
    // Guard for "the SPASynth import is gone": no source, UI string or tool still carries it.
    void synthImportRemovedTest()
    {
        std::cout << "synthImportRemovedTest\n";
        const juce::File root (SPASTRIP_SOURCE_DIR);
        expect (! root.getChildFile ("source/presets/SPASynthImport.cpp").exists(), "source/presets/SPASynthImport.cpp is gone");
        int hits = 0;
        juce::StringArray where;
        for (const auto* sub : { "source", "tools", "CMakeLists.txt" })
        {
            const auto base = root.getChildFile (sub);
            juce::Array<juce::File> files;
            if (base.isDirectory())
                files = base.findChildFiles (juce::File::findFiles, true, "*.cpp;*.h;*.mm");
            else
                files.add (base);
            for (const auto& f : files)
            {
                const auto text = f.loadFileAsString();
                for (const char* needle : { "importSPASynthPreset", "Import SPASynth", "x_import_summary", "describeImport", "SPASynthImport", ".spasynth", "SPASynthPreset" })
                    if (text.contains (needle)) { ++hits; where.add (f.getFileName() + ": " + needle); }
            }
        }
        expect (hits == 0, "no SPASynth-import API, menu item, scene or file filter remains in source/, tools/ or CMakeLists.txt" + (hits ? " (" + where.joinIntoString (", ") + ")" : juce::String()));
    }
}
