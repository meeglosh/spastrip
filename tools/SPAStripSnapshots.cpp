// SPAStripSnapshots: headless console tool that constructs the processor + editor
// off-screen, feeds a few seconds of test audio (so the analyzers, meters and live
// displays are alive) and writes PNG snapshots of the editor with
// Component::createComponentSnapshot at 1x and 2x.
//
//   SPAStripSnapshots [output-dir]
//
// Everything is hermetic: the accent settings file and the preset folder are
// redirected to a temp location, so a run never touches the user's real ones.

#include <iostream>

#include "SPAStripProcessor.h"
#include "presets/PresetManager.h"
#include "ui/Displays.h"
#include "ui/SPAStripEditor.h"
#include "ui/SPAStripLookAndFeel.h"
#include "ui/UiSettings.h"

namespace
{
    using Proc = spa::SPAStripProcessor;
    namespace pid = spa::params::id;
    namespace fx = spa::params::id::fx;
    constexpr double kSampleRate = 48000.0;
    constexpr int kBlock = 512;

    void setParam (Proc& proc, const juce::String& id, float realValue)
    {
        auto* p = proc.getAPVTS().getParameter (id);
        jassert (p != nullptr);
        p->setValueNotifyingHost (p->convertTo0to1 (realValue));
    }

    // A looping 4-beat musical bed at 120 BPM: kick, saw bass, a minor-chord pad and
    // hats. `kickOnly` gives the sidechain feed.
    struct Music
    {
        long long n = 0;
        juce::Random rng { 1234 };

        void fill (juce::AudioBuffer<float>& buf, int numMain, int numSc)
        {
            const int len = buf.getNumSamples();
            for (int i = 0; i < len; ++i, ++n)
            {
                const double t = (double) n / kSampleRate;
                const double beat = std::fmod (t, 0.5);              // kick every 0.5 s
                const double eighth = std::fmod (t, 0.25);
                const float kick = (float) (std::sin (juce::MathConstants<double>::twoPi * (45.0 * beat + 22.0 * (1.0 - std::exp (-beat * 40.0)) / 40.0 * 40.0 / 1.0) )
                                            * std::exp (-beat * 9.0));
                const double bassHz = (std::fmod (t, 2.0) < 1.0) ? 55.0 : 65.4;
                const double ph = std::fmod (t * bassHz, 1.0);
                const float bass = (float) ((2.0 * ph - 1.0) * 0.35 * std::exp (-std::fmod (t, 0.5) * 2.0));
                float pad = 0.0f;
                for (double f : { 220.0, 261.63, 329.63, 440.0 })
                    pad += (float) (std::sin (juce::MathConstants<double>::twoPi * f * t) * 0.05);
                const float hat = (float) ((rng.nextFloat() * 2.0f - 1.0f) * 0.18f * std::exp (-eighth * 55.0));
                const float noiseBed = (rng.nextFloat() * 2.0f - 1.0f) * 0.012f;
                constexpr float g = 0.6f;
                const float l = g * (0.55f * kick + bass + pad + hat + noiseBed);
                const float r = g * (0.55f * kick + bass * 0.9f + pad * 1.1f - hat * 0.8f + noiseBed);
                for (int c = 0; c < numMain; ++c)
                    buf.setSample (c, i, c == 0 ? l : r);
                for (int c = 0; c < numSc; ++c)
                    buf.setSample (numMain + c, i, 0.9f * kick);
            }
        }
    };

    struct Rig
    {
        std::unique_ptr<Proc> proc;
        Music music;
        int scChannels = 0;

        explicit Rig (int sc) : scChannels (sc)
        {
            proc = std::make_unique<Proc>();
            juce::AudioProcessor::BusesLayout l;
            l.inputBuses.add (juce::AudioChannelSet::stereo());
            l.inputBuses.add (sc > 0 ? juce::AudioChannelSet::stereo() : juce::AudioChannelSet::disabled());
            l.outputBuses.add (juce::AudioChannelSet::stereo());
            proc->setBusesLayout (l);
            proc->prepareToPlay (kSampleRate, kBlock);
        }

        void process (int blocks)
        {
            juce::AudioBuffer<float> buf (2 + scChannels, kBlock);
            juce::MidiBuffer midi;
            for (int b = 0; b < blocks; ++b)
            {
                buf.clear();
                music.fill (buf, 2, scChannels);
                proc->processBlock (buf, midi);
                if ((b & 15) == 0)
                    proc->serviceMessageThread();
            }
        }
    };

    // Runs the message loop for ~ms while still feeding audio, so the editor's timers
    // (30 Hz displays, 10 Hz housekeeping) see live telemetry.
    void pump (Rig& rig, int ms)
    {
        const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) ms;
        while (juce::Time::getMillisecondCounter() < end)
        {
            rig.process (2);
            juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
        }
    }

    void save (const juce::Image& img, const juce::File& file)
    {
        file.deleteFile();
        juce::FileOutputStream out (file);
        if (out.openedOk())
        {
            juce::PNGImageFormat png;
            png.writeImageToStream (img, out);
        }
    }

    void snap (spa::SPAStripEditor& editor, const juce::File& dir, const juce::String& name, bool both = true)
    {
        auto bounds = editor.getLocalBounds();
        save (editor.createComponentSnapshot (bounds, true, 1.0f), dir.getChildFile (name + "_1x.png"));
        if (both)
            save (editor.createComponentSnapshot (bounds, true, 2.0f), dir.getChildFile (name + "_2x.png"));
        std::cout << "  wrote " << name << "\n";
    }

    void resetAll (Proc& proc)
    {
        proc.getPresetManager().init();
        for (int m = 0; m < spa::dsp::FXChain::numModules; ++m)
            proc.setLocked ((spa::dsp::FXChain::Module) m, false);
        proc.setRandomWildness (0.5f);
        proc.clearUndoHistory();
    }

    void setBand (Proc& proc, int b, int type, float f, float gain, float q)
    {
        namespace eb = fx::eqband;
        setParam (proc, pid::eqBand (b, eb::enable), 1.0f);
        setParam (proc, pid::eqBand (b, eb::type), (float) type);
        setParam (proc, pid::eqBand (b, eb::freq), f);
        setParam (proc, pid::eqBand (b, eb::gain), gain);
        setParam (proc, pid::eqBand (b, eb::q), q);
    }

    // Enables module `name` with plausible non-default settings.
    void configureEffect (Proc& proc, const juce::String& name)
    {
        if (name == "DIST")
        {
            setParam (proc, fx::distEnable, 1); setParam (proc, fx::distType, 2); setParam (proc, fx::distDrive, 0.62f);
            setParam (proc, fx::distTone, 4800.0f); setParam (proc, fx::distMix, 0.8f);
        }
        else if (name == "CHORUS")
        {
            setParam (proc, fx::chorusEnable, 1); setParam (proc, fx::chorusRate, 0.9f); setParam (proc, fx::chorusDepth, 0.55f);
            setParam (proc, fx::chorusFeedback, 0.25f); setParam (proc, fx::chorusWidth, 80.0f); setParam (proc, fx::chorusMix, 0.5f);
        }
        else if (name == "MOD")
        {
            setParam (proc, fx::modEnable, 1); setParam (proc, fx::modType, 0); setParam (proc, fx::modRate, 0.35f);
            setParam (proc, fx::modDepth, 0.7f); setParam (proc, fx::modFeedback, 0.55f); setParam (proc, fx::modStages, 3); setParam (proc, fx::modMix, 0.6f);
        }
        else if (name == "TREM/VIB")
        {
            setParam (proc, fx::tremEnable, 1); setParam (proc, fx::tremRate, 5.5f); setParam (proc, fx::tremDepth, 0.7f);
            setParam (proc, fx::tremStereo, 0.5f); setParam (proc, fx::tremShape, 1);
            setParam (proc, fx::vibEnable, 1); setParam (proc, fx::vibRate, 4.2f); setParam (proc, fx::vibDepth, 0.45f);
        }
        else if (name == "GRAIN" || name == "GLITTER")
        {
            setParam (proc, fx::grainEnable, 1); setParam (proc, fx::grainSize, 90.0f); setParam (proc, fx::grainDensity, 40.0f);
            setParam (proc, fx::grainPitch, 7.0f); setParam (proc, fx::grainSpread, 0.5f); setParam (proc, fx::grainSpreadPitch, 4.0f);
            setParam (proc, fx::grainPosition, 600.0f); setParam (proc, fx::grainReverse, 0.25f); setParam (proc, fx::grainFeedback, 0.3f);
            setParam (proc, fx::grainMix, 0.55f); setParam (proc, fx::grainSync, 0);
        }
        else if (name == "DELAY")
        {
            setParam (proc, fx::delayEnable, 1); setParam (proc, fx::delaySync, 1); setParam (proc, fx::delayDivision, 6);
            setParam (proc, fx::delayFeedback, 0.5f); setParam (proc, fx::delayPingPong, 1); setParam (proc, fx::delayWidth, 85.0f);
            setParam (proc, fx::delayMix, 0.4f);
        }
        else if (name == "REVERB")
        {
            setParam (proc, fx::reverbEnable, 1); setParam (proc, fx::reverbPreDelay, 35.0f); setParam (proc, fx::reverbSize, 0.7f);
            setParam (proc, fx::reverbDecay, 3.2f); setParam (proc, fx::reverbDamping, 0.55f); setParam (proc, fx::reverbMix, 0.4f);
            setParam (proc, fx::reverbLowCut, 140.0f); setParam (proc, fx::reverbHighCut, 9000.0f);
        }
        else if (name == "CONV")
        {
            proc.loadFactoryIR ("dark-plate");
            setParam (proc, fx::convEnable, 1); setParam (proc, fx::convMix, 0.45f); setParam (proc, fx::convPreDelay, 18.0f);
            setParam (proc, fx::convDecay, 0.7f); setParam (proc, fx::convDamping, 0.3f); setParam (proc, fx::convStart, 0.1f);
        }
        else if (name == "EQ")
        {
            setParam (proc, fx::eqEnable, 1);
            setBand (proc, 0, 3, 55.0f, 0.0f, 0.7f);    // low cut
            setBand (proc, 1, 0, 240.0f, -3.5f, 1.1f);
            setBand (proc, 2, 0, 1200.0f, 4.0f, 0.9f);
            setBand (proc, 3, 0, 4300.0f, -2.5f, 2.2f);
            setBand (proc, 4, 2, 9500.0f, 3.5f, 0.7f);  // high shelf
        }
        else if (name == "COMP")
        {
            setParam (proc, fx::compEnable, 1); setParam (proc, fx::compMix, 0.9f);
            for (int b = 0; b < 3; ++b)
            {
                setParam (proc, pid::compBand (b, fx::compband::threshold), -26.0f + (float) b * 4.0f);
                setParam (proc, pid::compBand (b, fx::compband::ratio), 4.0f);
                setParam (proc, pid::compBand (b, fx::compband::gain), 2.0f);
            }
        }
        else if (name == "FILTER")
        {
            // Filter 1: 24 dB low-pass at 3.2 kHz with some resonance and drive; filter 2: 12 dB high-pass
            // at 160 Hz, in series. Both on.
            setParam (proc, fx::filterEnable, 1); setParam (proc, fx::filterRouting, 0);
            setParam (proc, fx::filter1Type, 1); setParam (proc, fx::filter1Cutoff, 3200.0f); setParam (proc, fx::filter1Res, 0.5f);
            setParam (proc, fx::filter1Drive, 0.25f); setParam (proc, fx::filter1Mix, 1.0f);
            setParam (proc, fx::filter2Enable, 1);
            setParam (proc, fx::filter2Type, 2); setParam (proc, fx::filter2Cutoff, 160.0f); setParam (proc, fx::filter2Res, 0.2f);
            setParam (proc, fx::filter2Drive, 0.0f); setParam (proc, fx::filter2Mix, 0.85f);
        }
        else if (name == "LIMIT")
        {
            setParam (proc, fx::limEnable, 1); setParam (proc, fx::limDrive, 7.0f); setParam (proc, fx::limCeiling, -1.0f);
            setParam (proc, fx::limRelease, 80.0f); setParam (proc, fx::limTruePeak, 1); setParam (proc, fx::limLookahead, 1);
        }
    }
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    spa::ui::forceLiveFlag().store (true);
    spa::ui::setFxDisplayFrozenMsForTest (4321.0);

    const auto stamp = juce::String (juce::Random::getSystemRandom().nextInt (1000000));
    const auto tmp = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("spastrip-snap-" + stamp);
    spa::ui::settings::setSettingsFileOverride (tmp.getChildFile ("settings.xml"));
    spa::preset::PresetManager::setPresetsRootOverride (tmp.getChildFile ("presets"));

    const auto outDir = juce::File (argc >= 2 ? juce::String (argv[1]) : juce::String ("./ui-snapshots"));
    outDir.createDirectory();
    std::cout << "SPAStripSnapshots -> " << outDir.getFullPathName() << "\n";

    const int baseW = spa::ui::metrics::baseWidth, baseH = spa::ui::metrics::baseHeight;
    const juce::StringArray tabs { "DIST", "CHORUS", "MOD", "TREM/VIB", "GLITTER", "DELAY", "REVERB", "CONV", "EQ", "COMP", "LIMIT", "FILTER" };

    // ---- (a) default state: nothing enabled, no sidechain bus ------------------
    {
        Rig rig (0);
        auto editor = std::make_unique<spa::SPAStripEditor> (*rig.proc);
        editor->setSize (baseW, baseH);
        rig.process (60);
        pump (rig, 900);
        snap (*editor, outDir, "a_default");
    }

    // ---- (b) one per effect tab, the effect enabled, audio flowing ---------------
    {
        Rig rig (2);
        rig.process (150);
        for (const auto& tab : tabs)
        {
            resetAll (*rig.proc);
            configureEffect (*rig.proc, tab);
            rig.proc->getAPVTS().state.setProperty ("uiFxTab", tab, nullptr);
            auto editor = std::make_unique<spa::SPAStripEditor> (*rig.proc);
            editor->setSize (baseW, baseH);
            rig.process (250);
            pump (rig, 1300);
            snap (*editor, outDir, "b_tab_" + tab.replace ("/", "-").toLowerCase());
        }
    }

    // ---- (c) locks + two mod slots with reach arcs + a re-ordered chain ----------
    {
        Rig rig (2);
        rig.process (150);
        resetAll (*rig.proc);
        configureEffect (*rig.proc, "DELAY");
        configureEffect (*rig.proc, "REVERB");
        configureEffect (*rig.proc, "DIST");
        configureEffect (*rig.proc, "GRAIN");
        rig.proc->setModSlotTarget (0, fx::delayFeedback);
        rig.proc->setModSlotTarget (1, fx::delayMix);
        rig.proc->setModSlotTarget (2, fx::reverbDecay);
        rig.proc->setModSlotTarget (3, fx::distDrive);
        setParam (*rig.proc, pid::modSlotDepth (0), 0.45f);
        setParam (*rig.proc, pid::modSlotDepth (1), -0.3f);
        setParam (*rig.proc, pid::modSlotDepth (2), 0.5f);
        setParam (*rig.proc, pid::modSlotDepth (3), 0.35f);
        rig.proc->setLocked (spa::dsp::FXChain::Module::distortion, true);
        rig.proc->setLocked (spa::dsp::FXChain::Module::reverb, true);
        rig.proc->setLocked (spa::dsp::FXChain::Module::grain, true);
        rig.proc->setFxOrder ({ 11, 0, 2, 3, 1, 10, 8, 4, 5, 6, 9, 7 });
        rig.proc->getAPVTS().state.setProperty ("uiFxTab", "DELAY", nullptr);
        auto editor = std::make_unique<spa::SPAStripEditor> (*rig.proc);
        editor->setSize (baseW, baseH);
        rig.process (300);
        pump (rig, 1500);
        snap (*editor, outDir, "c_locks_mod_delay");
        // The same state on REVERB (the other assigned knob) -- shows a locked + assigned decay knob.
        editor->getContent().selectTabForModule (3);
        pump (rig, 600);
        snap (*editor, outDir, "c_locks_mod_reverb");
    }

    // ---- (c2) FILTER with two mod slots (reach arcs), parallel routing, a band-pass, locked ---
    {
        Rig rig (2);
        rig.process (150);
        resetAll (*rig.proc);
        configureEffect (*rig.proc, "FILTER");
        setParam (*rig.proc, fx::filterRouting, 1);
        setParam (*rig.proc, fx::filter2Type, 4); setParam (*rig.proc, fx::filter2Cutoff, 1100.0f); setParam (*rig.proc, fx::filter2Res, 0.7f);
        setParam (*rig.proc, fx::filter2Mix, 0.7f);
        rig.proc->setModSlotTarget (0, fx::filter1Cutoff);
        rig.proc->setModSlotTarget (1, fx::filter2Res);
        rig.proc->setModSlotTarget (2, fx::filter2Cutoff);
        setParam (*rig.proc, pid::modSlotDepth (0), 0.4f);
        setParam (*rig.proc, pid::modSlotDepth (1), -0.25f);
        setParam (*rig.proc, pid::modSlotDepth (2), 0.3f);
        rig.proc->setLocked (spa::dsp::FXChain::Module::filter, true);
        rig.proc->getAPVTS().state.setProperty ("uiFxTab", "FILTER", nullptr);
        auto editor = std::make_unique<spa::SPAStripEditor> (*rig.proc);
        editor->setSize (baseW, baseH);
        rig.process (300);
        pump (rig, 1500);
        snap (*editor, outDir, "c2_filter_mod_parallel");
        // Everything off (the greyed preview) with the filters still configured.
        setParam (*rig.proc, fx::filterEnable, 0); setParam (*rig.proc, fx::filter2Enable, 0);
        pump (rig, 600);
        snap (*editor, outDir, "c2_filter_off", false);
    }

    // ---- (d) preset drawer open: a dozen presets across the types, filed in folders ----
    {
        Rig rig (2);
        rig.process (100);
        auto& pm = rig.proc->getPresetManager();
        resetAll (*rig.proc);
        const struct { const char* name; const char* folder; const char* type; const char* fx; } list[] = {
            { "Kick Punch",     "",                  "Drums",     "DIST" },
            { "Snare Crack",    "",                  "Drums",     "COMP" },
            { "Warm Bass",      "",                  "Bass",      "FILTER" },
            { "Lead Vox Air",   "",                  "Vocals",    "REVERB" },
            { "Mystery Patch",  "",                  "",          "GRAIN" },
            { "Room Drums",     "Studio",            "Drums",     "REVERB" },
            { "Vocal Plate",    "Studio",            "Vocals",    "REVERB" },
            { "Bus Glue",       "Studio",            "Mixbus",    "COMP" },
            { "Final Polish",   "Studio/Mastering",  "Mastering", "LIMIT" },
            { "Loud Master",    "Studio/Mastering",  "Mastering", "EQ" },
            { "Granular Wash",  "Sound Design",      "Creative",  "GRAIN" },
            { "Tape Echo",      "Sound Design",      "FX",        "DELAY" },
            { "Amp Chorus",     "Sound Design",      "Guitar",    "CHORUS" } };
        for (const auto& e : list)
        {
            resetAll (*rig.proc);
            configureEffect (*rig.proc, e.fx);
            pm.save (e.name, e.folder, true, e.type);
        }
        pm.createUserFolder ({}, "Live Rig");
        pm.rescan();
        for (int i = 0; i < (int) pm.getPresets().size(); ++i)
            if (pm.getPresets()[(size_t) i].name == "Vocal Plate")
                pm.loadPreset (i);
        configureEffect (*rig.proc, "EQ");   // an edit after the load: the name gets its edited marker
        spa::ui::settings::setPresetFavorite ("User/Warm Bass", true);
        spa::ui::settings::setPresetFavorite ("Studio/Bus Glue", true);
        spa::ui::settings::setPresetGroupMode (0);
        auto editor = std::make_unique<spa::SPAStripEditor> (*rig.proc);
        editor->setSize (baseW, baseH);
        editor->getContent().setPresetBrowserOpen (true, false);
        rig.process (200);
        pump (rig, 1200);
        snap (*editor, outDir, "d_preset_drawer");
        editor->getContent().getPresetBrowser().getListForTest().scrollToEnsureRowIsOnscreen (0);
        pump (rig, 400);
        snap (*editor, outDir, "d1_preset_drawer_top", false);

        // Grouped by TYPE.
        editor->getContent().getPresetBrowser().setGroupBy (spa::ui::PresetBrowser::GroupBy::type);
        pump (rig, 500);
        snap (*editor, outDir, "d2_preset_drawer_by_type", false);

        // Filtered: a type, then a type + search that leaves one, then favourites.
        auto& browser = editor->getContent().getPresetBrowser();
        browser.setGroupBy (spa::ui::PresetBrowser::GroupBy::folders);
        browser.setTypeFilter ("Mastering");
        pump (rig, 500);
        snap (*editor, outDir, "d3_preset_drawer_type_filter", false);
        browser.setTypeFilter ({});
        browser.setSearchText ("zzzz");
        pump (rig, 300);
        snap (*editor, outDir, "d4_preset_drawer_no_match", false);
        browser.setSearchText ({});
        browser.setFavouritesOnly (true);
        pump (rig, 300);
        snap (*editor, outDir, "d5_preset_drawer_favourites", false);
        browser.setFavouritesOnly (false);

        // A narrow look: the folder menu pick + a collapsed group.
        browser.toggleGroupRow (browser.findGroupRow ("U:Studio"));
        pump (rig, 300);
        snap (*editor, outDir, "d6_preset_drawer_collapsed", false);
        browser.toggleGroupRow (browser.findGroupRow ("U:Studio"));

        // The search box with text in it: the clear "x" is showing.
        browser.setSearchText ("vocal");
        pump (rig, 300);
        snap (*editor, outDir, "d8_preset_drawer_search_clear");
        browser.setSearchText ({});
    }

    // ---- (d7) an empty library --------------------------------------------------------
    {
        Rig rig (2);
        rig.process (60);
        auto editor = std::make_unique<spa::SPAStripEditor> (*rig.proc);
        editor->setSize (baseW, baseH);
        auto& pm = rig.proc->getPresetManager();
        // (the hermetic folder already holds the presets of scene d: use a fresh root)
        const auto emptyRoot = tmp.getChildFile ("empty-presets");
        pm.setPresetsRoot (emptyRoot);
        editor->getContent().setPresetBrowserOpen (true, false);
        pump (rig, 600);
        snap (*editor, outDir, "d7_preset_drawer_empty", false);
        pm.setPresetsRoot (tmp.getChildFile ("presets"));
    }

    // ---- (e) about / credits ------------------------------------------------------
    {
        Rig rig (0);
        auto editor = std::make_unique<spa::SPAStripEditor> (*rig.proc);
        editor->setSize (baseW, baseH);
        rig.process (60);
        editor->getContent().showAboutPanel();
        pump (rig, 500);
        snap (*editor, outDir, "e_about");
    }

    // ---- (f) 50% and 200% window -----------------------------------------------------
    {
        Rig rig (2);
        rig.process (150);
        resetAll (*rig.proc);
        configureEffect (*rig.proc, "REVERB");
        configureEffect (*rig.proc, "EQ");
        rig.proc->getAPVTS().state.setProperty ("uiFxTab", "EQ", nullptr);
        auto editor = std::make_unique<spa::SPAStripEditor> (*rig.proc);
        rig.process (250);
        editor->setSize (baseW / 2, baseH / 2);
        pump (rig, 1200);
        snap (*editor, outDir, "f_scale_50");
        editor->setSize (baseW * 2, baseH * 2);
        pump (rig, 800);
        snap (*editor, outDir, "f_scale_200", false);
    }

    // ---- (g) custom accent colour ----------------------------------------------------------
    {
        Rig rig (2);
        rig.process (150);
        resetAll (*rig.proc);
        configureEffect (*rig.proc, "DELAY");
        configureEffect (*rig.proc, "CHORUS");
        rig.proc->setModSlotTarget (0, fx::delayFeedback);
        setParam (*rig.proc, pid::modSlotDepth (0), 0.5f);
        rig.proc->setLocked (spa::dsp::FXChain::Module::chorus, true);
        rig.proc->getAPVTS().state.setProperty ("uiFxTab", "DELAY", nullptr);
        spa::ui::setAccentColor (juce::Colour (0xffe8873a));   // warm orange
        auto editor = std::make_unique<spa::SPAStripEditor> (*rig.proc);
        editor->setSize (baseW, baseH);
        editor->getContent().applyTheme();
        rig.process (250);
        pump (rig, 1300);
        snap (*editor, outDir, "g_accent_orange");
        spa::ui::resetAccentColor();
    }

    // ---- extras: save dialog, accent picker -------------------------------------------
    {
        Rig rig (2);
        rig.process (100);
        resetAll (*rig.proc);
        configureEffect (*rig.proc, "REVERB");
        auto editor = std::make_unique<spa::SPAStripEditor> (*rig.proc);
        editor->setSize (baseW, baseH);
        auto& content = editor->getContent();
        content.showSaveDialog (true);
        pump (rig, 400);
        snap (*editor, outDir, "x_save_dialog", false);

        auto* sd = dynamic_cast<spa::ui::SaveDialog*> (content.getDialogForTest());
        if (sd != nullptr)
        {
            const auto select = [] (juce::ComboBox& cb, const juce::String& text)
            {
                for (int i = 0; i < cb.getNumItems(); ++i)
                    if (cb.getItemText (i) == text)
                        cb.setSelectedId (cb.getItemId (i), juce::sendNotificationSync);
            };
            select (sd->getTypeBox(), "Mixbus");
            select (sd->getFolderBox(), "Auto (by type)");
            pump (rig, 200);
            snap (*editor, outDir, "x_save_dialog_mixbus_auto", false);

            // The clash state: a taken name in the folder, with Replace offered.
            select (sd->getTypeBox(), "Drums");
            select (sd->getFolderBox(), "User (unfiled)");
            sd->getNameEditor().setText ("Kick Punch", false);
            sd->submit (false);
            pump (rig, 200);
            snap (*editor, outDir, "x_save_dialog_clash", false);

            select (sd->getFolderBox(), "New folder...");
            sd->getNameEditor().setText ("Kick Punch 2", false);
            pump (rig, 200);
            snap (*editor, outDir, "x_save_dialog_new_folder", false);
        }
        content.dismissDialog();

        content.showConfirm ("Move folder to Trash", "Move the folder \"Sound Design\" and the 3 presets in it to the Trash? You can recover them from the Trash.",
                             "Move to Trash", [] {});
        pump (rig, 300);
        snap (*editor, outDir, "x_confirm_trash_folder", false);
        content.dismissDialog();

        content.showClashDialog ("Kick Punch", [] (spa::preset::PresetManager::ImportClash, bool) {});
        pump (rig, 300);
        snap (*editor, outDir, "x_import_clash", false);
        content.dismissDialog();

        content.promptText ("Rename folder", "NAME", "Sound Design", "Rename", [] (const juce::String&) {});
        pump (rig, 300);
        snap (*editor, outDir, "x_rename_folder", false);
        content.dismissDialog();

        content.showAccentPicker();
        pump (rig, 300);
        snap (*editor, outDir, "x_accent_dialog", false);
        content.dismissDialog();
    }

    // ---- (p) popup-menu rows: ticked / unticked / highlighted / disabled items -----
    {
        spa::ui::SPAStripLookAndFeel laf;
        struct Row { const char* text; bool ticked; bool highlighted; bool active; };
        const Row rows[] = { { "Stereo", true, false, true }, { "Mid / Side", false, false, true },
                             { "Left Only", false, true, true }, { "Right Only", true, true, true },
                             { "Mono (disabled)", true, false, false }, { "Bypass", false, false, true } };
        for (float scale : { 1.0f, 2.0f })
        {
            const int w = 190, rowH = 24, pad = 4;
            const int h = pad * 2 + rowH * (int) std::size (rows);
            juce::Image img (juce::Image::ARGB, juce::roundToInt (w * scale), juce::roundToInt (h * scale), true);
            juce::Graphics g (img);
            g.addTransform (juce::AffineTransform::scale (scale));
            laf.drawPopupMenuBackgroundWithOptions (g, w, h, juce::PopupMenu::Options());
            int y = pad;
            for (const auto& r : rows)
            {
                laf.drawPopupMenuItem (g, { 0, y, w, rowH }, false, r.active, r.highlighted, r.ticked, false,
                                       r.text, {}, nullptr, nullptr);
                y += rowH;
            }
            save (img, outDir.getChildFile (juce::String ("p_popup_menu_") + (scale > 1.5f ? "2x" : "1x") + ".png"));
        }
        std::cout << "  wrote p_popup_menu\n";
    }

    tmp.deleteRecursively();
    return 0;
}
