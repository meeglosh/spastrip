// On-disk format fixtures for the preset system: SPAStrip's preset files, user
// folders / banks, favourite keys and settings keys, committed under
// tests/fixtures/presets-1.0.4/ and checked byte for byte. They were written by the
// code BEFORE the preset browser moved into the shared spa-presets module, so the move
// (and anything after it) cannot change a customer's existing files, folders, stars or
// remembered groups. Never regenerate them to make a failing test pass: a mismatch is a
// format change, which needs its own decision.
// Regenerate deliberately with SPASTRIP_WRITE_FIXTURES=1 (writes into the source tree).
// Included from SPAStripTests.cpp after PresetTests.inc.

namespace presetFixtureTests
{
    namespace ui = spa::ui;
    namespace fx = spa::params::id::fx;
    namespace pid = spa::params::id;
    using PM = spa::preset::PresetManager;
    using Browser = ui::PresetBrowser;
    using phase2bTests::TempDir;
    using phase2bTests::makeIsolated;
    using presetTests::fileOf;
    using presetTests::infoOf;

    juce::File fixtureRoot()
    {
        return juce::File (SPASTRIP_SOURCE_DIR).getChildFile ("tests").getChildFile ("fixtures").getChildFile ("presets-1.0.4");
    }

    // The recipe each fixture preset was saved from: a distinctive patch per index.
    void applyRecipe (spa::SPAStripProcessor& p, int index)
    {
        phase2bTests::dirtyPatch (p);
        setParam (p, fx::reverbMix, 0.10f + 0.1f * (float) index);
        setParam (p, pid::outputGain, -1.5f * (float) index);
    }

    struct Recipe { const char* name; const char* folder; const char* type; int index; };
    const Recipe recipes[] = {
        { "Fixture Root",  "",                  "Mixbus",    1 },
        { "Fixture Untyped", "",                "",          2 },
        { "Fixture Kick",  "Drums",             "Drums",     3 },
        { "Fixture Final", "Studio/Mastering",  "Mastering", 4 },
    };

    juce::File fixtureFile (const juce::File& root, const char* folder, const char* name)
    {
        auto d = root.getChildFile ("User");
        for (const auto& part : juce::StringArray::fromTokens (folder, "/", ""))
            d = d.getChildFile (part);
        return d.getChildFile (juce::String (name) + ".spastrip");
    }

    // Copies the committed fixture tree (User/ and the settings file) into a temp location.
    void copyTree (const juce::File& from, const juce::File& to)
    {
        to.createDirectory();
        for (const auto& f : from.findChildFiles (juce::File::findFilesAndDirectories, false, "*", juce::File::FollowSymlinks::no))
        {
            if (f.isDirectory())
                copyTree (f, to.getChildFile (f.getFileName()));
            else
                f.copyFileTo (to.getChildFile (f.getFileName()));
        }
    }

    juce::String bytes (const juce::File& f) { return f.loadFileAsString(); }

    // Hex-escapes CR / LF and other control bytes so a mismatch can be read in a CI log.
    juce::String escaped (const juce::String& s, int from, int len)
    {
        juce::String out;
        for (int i = juce::jmax (0, from); i < juce::jmin (s.length(), from + len); ++i)
        {
            const auto c = s[i];
            if (c == '\r') out << "\\r";
            else if (c == '\n') out << "\\n";
            else if (c < 32 || c > 126) out << "\\x" << juce::String::toHexString ((int) c);
            else out << juce::String::charToString (c);
        }
        return out;
    }

    // Fixtures were written on macOS. Other platforms' float maths can round a parameter's
    // value differently in the last bits (e.g. a zero default saved as 3.576e-7 on macOS but
    // 0.0 on Windows), so off macOS the files are compared as parsed XML: same element tree,
    // same attribute names in the same order, attribute values equal as strings OR as numbers
    // within 1e-6 (scaled up for large magnitudes). macOS stays strictly byte-for-byte.
    bool looksNumeric (const juce::String& s)
    {
        return s.isNotEmpty() && s.containsOnly ("0123456789.-+eE") && s.containsAnyOf ("0123456789");
    }

    // Returns {} when equal up to float noise, else a description of the first divergence.
    juce::String xmlDivergence (const juce::XmlElement& a, const juce::XmlElement& b, const juce::String& path)
    {
        const auto here = path + "/" + a.getTagName();
        if (a.getTagName() != b.getTagName()) return here + ": tag " + a.getTagName() + " vs " + b.getTagName();
        if (a.getNumAttributes() != b.getNumAttributes())
            return here + ": attribute count " + juce::String (a.getNumAttributes()) + " vs " + juce::String (b.getNumAttributes());
        for (int i = 0; i < a.getNumAttributes(); ++i)
        {
            const auto n = a.getAttributeName (i);
            if (n != b.getAttributeName (i)) return here + ": attribute #" + juce::String (i) + " name " + n + " vs " + b.getAttributeName (i);
            const auto va = a.getAttributeValue (i), vb = b.getAttributeValue (i);
            if (va == vb) continue;
            if (looksNumeric (va) && looksNumeric (vb))
            {
                const double x = va.getDoubleValue(), y = vb.getDoubleValue();
                if (std::abs (x - y) <= 1.0e-6 * juce::jmax (1.0, std::abs (x), std::abs (y))) continue;
            }
            return here + " [" + a.getStringAttribute ("id") + "]: " + n + "=\"" + va + "\" vs \"" + vb + "\"";
        }
        if (a.getNumChildElements() != b.getNumChildElements())
            return here + ": child count " + juce::String (a.getNumChildElements()) + " vs " + juce::String (b.getNumChildElements());
        auto* ca = a.getFirstChildElement();
        auto* cb = b.getFirstChildElement();
        while (ca != nullptr && cb != nullptr)
        {
            const auto d = xmlDivergence (*ca, *cb, here);
            if (d.isNotEmpty()) return d;
            ca = ca->getNextElement();
            cb = cb->getNextElement();
        }
        if (a.getAllSubText().trim() != b.getAllSubText().trim()) return here + ": text differs";
        return {};
    }

    // Empty when the texts match under the platform's rule; else a message with the first
    // differing offset and ~60 characters of context from both sides (hex-escaped), plus, off
    // macOS, where the tolerant XML comparison diverged.
    juce::String compareBytes (const juce::String& a, const juce::String& b)
    {
        if (a == b) return {};
        juce::String extra;
       #if ! JUCE_MAC
        {
            auto xa = juce::XmlDocument::parse (a);
            auto xb = juce::XmlDocument::parse (b);
            if (xa == nullptr || xb == nullptr)
                extra = juce::String ("; tolerant compare: ") + (xa == nullptr ? "saved" : "fixture") + " does not parse as XML";
            else
            {
                const auto d = xmlDivergence (*xa, *xb, {});
                if (d.isEmpty()) return {};
                extra = "; tolerant compare diverged at " + d;
            }
        }
       #endif
        int i = 0;
        while (i < a.length() && i < b.length() && a[i] == b[i]) ++i;
        return " [first difference at offset " + juce::String (i) + ", lengths " + juce::String (a.length()) + " vs " + juce::String (b.length())
               + "; saved: ..." + escaped (a, i - 30, 60) + "... fixture: ..." + escaped (b, i - 30, 60) + "..." + extra + "]";
    }

    juce::StringArray zipEntries (const juce::File& zip)
    {
        juce::ZipFile z (zip);
        juce::StringArray names;
        for (int i = 0; i < z.getNumEntries(); ++i)
            names.add (z.getEntry (i)->filename);
        names.sort (false);
        return names;
    }

    //==========================================================================
    // Writes the fixtures (opt-in).
    void maybeWriteFixtures()
    {
        if (juce::SystemStats::getEnvironmentVariable ("SPASTRIP_WRITE_FIXTURES", {}).isEmpty())
            return;
        const auto root = fixtureRoot();
        root.deleteRecursively();
        root.createDirectory();
        {
            auto proc = makeProc (48000.0, 256);
            auto& pm = proc->getPresetManager();
            pm.setPresetsRoot (root);
            pm.setUseTrash (false);
            for (const auto& r : recipes)
            {
                applyRecipe (*proc, r.index);
                const auto res = pm.save (r.name, r.folder, false, r.type);
                expect (res.ok, juce::String ("fixture written: ") + r.name);
            }
            // A preset carrying a type this build does not know (kept as it is on save-in-place).
            applyRecipe (*proc, 5);
            const auto odd = pm.save ("Fixture Odd Type", "");
            auto xml = juce::XmlDocument::parse (odd.file);
            xml->setAttribute ("type", "Dubstep");
            xml->writeTo (odd.file);
            root.getChildFile ("User").getChildFile ("Empty Folder").createDirectory();
            root.getChildFile ("User").getChildFile ("Empty Folder").getChildFile (".gitkeep").create();
        }
        const auto settings = root.getChildFile ("SPAStrip.settings");
        {
            const auto previous = ui::settings::getSettingsFile();
            ui::settings::setSettingsFileOverride (settings);
            ui::settings::setPresetFavorite ("User/Fixture Root", true);
            ui::settings::setPresetFavorite ("Drums/Fixture Kick", true);
            ui::settings::setPresetFavorite ("Studio/Fixture Final", true);
            ui::settings::setPresetGroupCollapsed ("U:Studio", true);
            ui::settings::setPresetGroupCollapsed ("T:Drums", true);
            ui::settings::setPresetGroupMode (1);
            ui::settings::setAccentColor (juce::Colour (0xff51d0bf));
            ui::settings::setSettingsFileOverride (previous);
        }
        std::cout << "  (fixtures written to " << root.getFullPathName() << ")\n";
    }

    //==========================================================================
    void presetFormatFixtureTest()
    {
        std::cout << "presetFormatFixtureTest\n";
        maybeWriteFixtures();
        const auto fixtures = fixtureRoot();
        expect (fixtures.getChildFile ("User").isDirectory(), "the committed fixture tree exists: " + fixtures.getFullPathName());

        // --- the document shape: <SPAStripPreset name version type> + the patch state, attributes in that order.
        {
            const auto text = bytes (fixtureFile (fixtures, "", "Fixture Root"));
            expect (text.startsWith ("<?xml version=\"1.0\" encoding=\"UTF-8\"?>"), "a preset file starts with the XML declaration");
            expect (text.contains ("<SPAStripPreset name=\"Fixture Root\" version=\"1\" type=\"Mixbus\">"),
                    "root tag SPAStripPreset, attributes name / version / type in that order");
            auto xml = juce::XmlDocument::parse (text);
            expect (xml != nullptr && xml->getNumChildElements() == 1 && xml->getFirstChildElement()->hasTagName ("PARAMS"),
                    "one child: the PARAMS state");
            expect (fixtureFile (fixtures, "", "Fixture Untyped").loadFileAsString().contains ("<SPAStripPreset name=\"Fixture Untyped\" version=\"1\">"),
                    "an untyped preset has no type attribute");
        }

        // --- SAVE reproduces the fixtures byte for byte (name, folder, type, state).
        {
            TempDir t ("fixture-save");
            auto proc = makeIsolated (t);
            auto& pm = proc->getPresetManager();
            for (const auto& r : recipes)
            {
                applyRecipe (*proc, r.index);
                const auto res = pm.save (r.name, r.folder, false, r.type);
                expect (res.ok, juce::String ("save ") + r.name);
                expect (res.file == fixtureFile (t.dir, r.folder, r.name), juce::String (r.name) + ": lands in User/<folder>/<name>.spastrip");
                const auto diff = compareBytes (bytes (res.file), bytes (fixtureFile (fixtures, r.folder, r.name)));
                expect (diff.isEmpty(), juce::String (r.name) + ": the saved file is byte-identical to the fixture" + diff);
            }
        }

        // --- the fixture tree: scan, load, save-in-place, export, favourites.
        TempDir t ("fixture-tree");
        copyTree (fixtures.getChildFile ("User"), t.dir.getChildFile ("User"));
        const auto settingsCopy = t.dir.getChildFile ("settings.xml");
        fixtures.getChildFile ("SPAStrip.settings").copyFileTo (settingsCopy);
        const auto previousSettings = ui::settings::getSettingsFile();
        ui::settings::setSettingsFileOverride (settingsCopy);
        struct Restore { juce::File f; ~Restore() { ui::settings::setSettingsFileOverride (f); } } restore { previousSettings };

        auto proc = makeIsolated (t);
        auto& pm = proc->getPresetManager();
        pm.rescan();

        {
            const auto* root = infoOf (pm, "Fixture Root");
            const auto* kick = infoOf (pm, "Fixture Kick");
            const auto* fin = infoOf (pm, "Fixture Final");
            const auto* untyped = infoOf (pm, "Fixture Untyped");
            const auto* odd = infoOf (pm, "Fixture Odd Type");
            expect (root && kick && fin && untyped && odd && pm.getPresets().size() == 5, "the scan lists the five fixture presets");
            if (! (root && kick && fin && untyped && odd))
                return;
            expect (root->type == "Mixbus" && root->bank.isEmpty() && root->folder.isEmpty() && ! root->isFactory, "root preset: typed, unfiled");
            expect (kick->bank == "Drums" && kick->folder == "Drums" && kick->type == "Drums", "Drums/Fixture Kick: bank and folder");
            expect (fin->bank == "Studio" && fin->folder == "Studio/Mastering" && fin->type == "Mastering", "nested preset: bank is the top-level folder, folder the full path");
            expect (untyped->type.isEmpty() && untyped->storedType.isEmpty(), "untyped preset");
            expect (odd->type.isEmpty() && odd->storedType == "Dubstep", "an unknown stored type is kept but not shown");
            expect (PM::favouriteKey (*root) == "User/Fixture Root" && PM::favouriteKey (*kick) == "Drums/Fixture Kick"
                        && PM::favouriteKey (*fin) == "Studio/Fixture Final",
                    "favourite keys are '<top-level folder or User>/<name>'");
            expect (PM::keyOf (*root) == root->file.getFullPathName(), "a user preset is keyed by its path");
            PM::PresetInfo factory;
            factory.name = "Warm"; factory.bank = "Dance"; factory.isFactory = true;
            expect (PM::favouriteKey (factory) == "Factory:Dance/Warm" && PM::keyOf (factory) == "F:Dance/Warm", "factory keys");
            expect (pm.getBanks().joinIntoString (",") == "Drums,Empty Folder,Studio", "banks: " + pm.getBanks().joinIntoString (","));
            expect (pm.getUserFolders().joinIntoString (",") == "Drums,Empty Folder,Studio,Studio/Mastering",
                    "folders (empty ones included): " + pm.getUserFolders().joinIntoString (","));
        }

        // --- the browser's group keys over the fixture tree.
        {
            std::vector<PM::PresetInfo> list (pm.getPresets().begin(), pm.getPresets().end());
            std::vector<int> all;
            for (size_t i = 0; i < list.size(); ++i)
                all.push_back ((int) i);
            juce::StringArray keys;
            for (const auto& r : Browser::buildRows (list, all, pm.getUserFolders(), Browser::GroupBy::folders, {}, false, false))
                if (r.isGroup())
                    keys.add (r.key);
            expect (keys.joinIntoString (",") == "U:,U:Drums,U:Empty Folder,U:Studio,U:Studio/Mastering", "folder group keys: " + keys.joinIntoString (","));
            juce::StringArray tkeys;
            for (const auto& r : Browser::buildRows (list, all, pm.getUserFolders(), Browser::GroupBy::type, {}, false, false))
                if (r.isGroup())
                    tkeys.add (r.key);
            expect (tkeys.joinIntoString (",") == "T:Drums,T:Mixbus,T:Mastering,T:Other", "type group keys: " + tkeys.joinIntoString (","));
        }

        // --- settings: the committed file reads back; writes keep every key.
        {
            expect (ui::settings::getFavoritePresets().joinIntoString ("|") == "User/Fixture Root|Drums/Fixture Kick|Studio/Fixture Final",
                    "favourites read from the fixture settings");
            expect (ui::settings::getCollapsedPresetGroups().joinIntoString ("|") == "U:Studio|T:Drums", "collapsed groups read from the fixture settings");
            expect (ui::settings::getPresetGroupMode() == 1, "group mode read from the fixture settings");
            const auto raw = bytes (settingsCopy);
            expect (raw.contains ("name=\"favoritePresets\"") && raw.contains ("name=\"collapsedPresetGroups\"")
                        && raw.contains ("name=\"presetGroupMode\"") && raw.contains ("name=\"accentColour\""),
                    "settings keys: favoritePresets, collapsedPresetGroups, presetGroupMode, accentColour");
            ui::settings::setPresetFavorite ("User/Another", true);
            const auto after = bytes (settingsCopy);
            expect (ui::settings::getFavoritePresets().contains ("User/Another") && after.contains ("name=\"accentColour\"")
                        && ui::settings::getPresetGroupMode() == 1,
                    "a settings write keeps the other keys");
            ui::settings::setPresetFavorite ("User/Another", false);
        }

        // --- LOAD + SAVE IN PLACE: loading a fixture and saving it back rewrites the same bytes.
        for (const auto* name : { "Fixture Root", "Fixture Untyped", "Fixture Kick", "Fixture Final", "Fixture Odd Type" })
        {
            const auto file = fileOf (pm, name);
            const auto before = bytes (file);
            expect (pm.load (file).ok, juce::String (name) + ": loads");
            const auto saved = pm.saveInPlace (file).ok;
            const auto diff = compareBytes (bytes (file), before);
            expect (saved && diff.isEmpty(), juce::String (name) + ": load then save in place is byte-identical (stored type kept)" + diff);
        }

        // --- EXPORT: verbatim copy of the file; a folder zips with paths relative to that folder.
        {
            const auto out = t.dir.getChildFile ("export");
            expect (pm.exportPreset (*infoOf (pm, "Fixture Kick"), out.getChildFile ("kick.spastrip"))
                        && bytes (out.getChildFile ("kick.spastrip")) == bytes (fixtureFile (fixtures, "Drums", "Fixture Kick")),
                    "exportPreset writes the file verbatim");
            expect (pm.exportFolder ("Studio", out.getChildFile ("studio.zip")), "exportFolder");
            expect (zipEntries (out.getChildFile ("studio.zip")).joinIntoString ("|") == "Mastering/Fixture Final.spastrip",
                    "zip entries are relative to the exported folder: " + zipEntries (out.getChildFile ("studio.zip")).joinIntoString ("|"));
            expect (pm.exportFolder ("Studio", out.getChildFile ("studio.zip")) , "re-export over an existing zip");

            // ...and import it into a fresh root: bank named after the zip, nested folder kept, bytes identical.
            TempDir u ("fixture-import");
            auto other = makeIsolated (u);
            auto session = other->getPresetManager().beginImport ({ out.getChildFile ("studio.zip") });
            const auto step = session->advance();
            const auto result = session->finish();
            expect (step.finished && result.imported == 1, "importing the zip: one preset");
            expect (bytes (u.dir.getChildFile ("User").getChildFile ("studio").getChildFile ("Mastering").getChildFile ("Fixture Final.spastrip"))
                        == bytes (fixtureFile (fixtures, "Studio/Mastering", "Fixture Final")),
                    "...landing in User/<zip name>/<folders>/ with identical bytes");
        }

        // --- favourites follow rename / move / folder rename / trash.
        {
            expect (pm.rename (fileOf (pm, "Fixture Root"), "Fixture Renamed").ok, "rename");
            auto favs = ui::settings::getFavoritePresets();
            expect (favs.contains ("User/Fixture Renamed") && ! favs.contains ("User/Fixture Root"), "a rename moves the star");

            expect (pm.moveUserPreset (fileOf (pm, "Fixture Kick"), "Studio").ok, "move to a folder");
            favs = ui::settings::getFavoritePresets();
            expect (favs.contains ("Studio/Fixture Kick") && ! favs.contains ("Drums/Fixture Kick"), "a move re-keys the star to the new top-level folder");

            expect (pm.renameUserFolder ("Studio", "Mix").ok, "rename a top-level folder");
            favs = ui::settings::getFavoritePresets();
            expect (favs.contains ("Mix/Fixture Kick") && favs.contains ("Mix/Fixture Final") && ! favs.contains ("Studio/Fixture Final"),
                    "a top-level folder rename re-keys every star inside");

            expect (pm.deletePreset (fileOf (pm, "Fixture Final")), "delete");
            expect (! ui::settings::getFavoritePresets().contains ("Mix/Fixture Final"), "a delete drops the star");
            expect (pm.trashUserFolder ("Mix").ok, "trash a folder");
            expect (! ui::settings::getFavoritePresets().contains ("Mix/Fixture Kick"), "a folder trash drops its stars");
            expect (ui::settings::getFavoritePresets().joinIntoString ("|") == "User/Fixture Renamed", "only the untouched star is left");
        }
    }
}
