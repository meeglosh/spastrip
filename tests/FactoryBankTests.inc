// Factory bank tests (source/presets/FactoryBank*.cpp): 12 types x 6 presets, written as recipes and
// delivered as embedded factory presets. Renders every preset over four synthetic sources and checks
// loudness, finiteness, headroom, tail decay and audibility; also the listing, the types, the stamp and
// that user presets are left alone. `SPAStripTests --render-factory-presets [report.md]` prints (and
// writes) the per-preset loudness table the `out=` trims are calibrated from.
// Included from SPAStripTests.cpp after PresetTests.inc.

namespace factoryBankTests
{
    namespace params = spa::params;
    using PM = spa::preset::PresetManager;
    using phase2bTests::TempDir;

    constexpr double kSampleRate = 48000.0;
    constexpr int kBlock = 256;
    constexpr double kSourceSeconds = 4.0;
    constexpr double kTailSeconds = 20.0;
    constexpr int kNumSources = 4;
    const char* const kSourceNames[kNumSources] = { "drums", "vocal", "bass", "pad" };

    using Stereo = std::array<std::vector<float>, 2>;

    // --- synthetic sources, all peaking at -6 dBFS --------------------------------------------
    void normalise (Stereo& s, float peak = 0.5f)
    {
        float m = 1.0e-9f;
        for (auto& ch : s)
            for (float v : ch)
                m = std::max (m, std::abs (v));
        for (auto& ch : s)
            for (auto& v : ch)
                v *= peak / m;
    }

    Stereo makeDrums()
    {
        const int n = (int) (kSampleRate * kSourceSeconds);
        Stereo s { std::vector<float> ((size_t) n, 0.0f), std::vector<float> ((size_t) n, 0.0f) };
        Noise nz (11);
        const double beat = kSampleRate * 0.5;   // 120 bpm
        const auto add = [&] (int at, int len, auto&& gen)
        {
            for (int i = 0; i < len && at + i < n; ++i)
            {
                const float v = gen (i);
                s[0][(size_t) (at + i)] += v;
                s[1][(size_t) (at + i)] += v;
            }
        };
        for (int b = 0; b < 8; ++b)
        {
            const int at = (int) (b * beat);
            if (b % 2 == 0)   // kick
            {
                double ph = 0.0;
                add (at, (int) (0.4 * kSampleRate), [&] (int i)
                {
                    const double t = i / kSampleRate;
                    const double f = 45.0 + 80.0 * std::exp (-t * 30.0);
                    ph += juce::MathConstants<double>::twoPi * f / kSampleRate;
                    return (float) (std::sin (ph) * std::exp (-t * 9.0));
                });
            }
            else              // snare
                add (at, (int) (0.3 * kSampleRate), [&] (int i)
                {
                    const double t = i / kSampleRate;
                    return (float) ((0.7 * nz.next() + 0.5 * std::sin (juce::MathConstants<double>::twoPi * 190.0 * t)) * std::exp (-t * 18.0));
                });
            for (int h = 0; h < 2; ++h)   // hats on the 8ths
            {
                float last = 0.0f;
                add ((int) (at + h * beat * 0.5), (int) (0.05 * kSampleRate), [&] (int i)
                {
                    const float x = nz.next();
                    const float v = x - last;   // crude high-pass
                    last = x;
                    return 0.18f * v * (float) std::exp (-(i / kSampleRate) * 90.0);
                });
            }
        }
        normalise (s);
        return s;
    }

    Stereo makeVocal()
    {
        const int n = (int) (kSampleRate * kSourceSeconds);
        Stereo s { std::vector<float> ((size_t) n), std::vector<float> ((size_t) n) };
        Noise nz (21);
        const double vowels[4][3] = { { 800, 1200, 2600 }, { 400, 2000, 2800 }, { 300, 800, 2300 }, { 600, 1000, 2500 } };
        double phase = 0.0, z[3][2] = {};
        for (int i = 0; i < n; ++i)
        {
            const double t = i / kSampleRate;
            const int vi = (int) (t / 0.5) % 4;
            const double f0 = (200.0 + 25.0 * std::sin (t * 2.0)) * (1.0 + 0.01 * std::sin (t * 2.0 * juce::MathConstants<double>::twoPi * 2.5));
            phase += f0 / kSampleRate;
            phase -= std::floor (phase);
            const double glottal = (phase < 0.6 ? phase / 0.6 : (1.0 - phase) / 0.4) * 2.0 - 1.0;
            const double excitation = glottal + 0.05 * nz.next();
            double y = 0.0;
            for (int f = 0; f < 3; ++f)
            {
                const double r = 0.97, th = juce::MathConstants<double>::twoPi * vowels[vi][f] / kSampleRate;
                const double out = excitation * (1.0 - r) + 2.0 * r * std::cos (th) * z[f][0] - r * r * z[f][1];
                z[f][1] = z[f][0];
                z[f][0] = out;
                y += out * (f == 0 ? 1.0 : 0.6);
            }
            const double phrase = (std::fmod (t, 1.0) < 0.8) ? 1.0 : 0.05;   // breaths between phrases
            s[0][(size_t) i] = s[1][(size_t) i] = (float) (y * phrase);
        }
        normalise (s);
        return s;
    }

    Stereo makeBass()
    {
        const int n = (int) (kSampleRate * kSourceSeconds);
        Stereo s { std::vector<float> ((size_t) n), std::vector<float> ((size_t) n) };
        const double notes[4] = { 55.0, 73.42, 49.0, 55.0 };
        double ph = 0.0, lp = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const double t = i / kSampleRate;
            const int ni = juce::jmin (3, (int) (t / 1.0));
            const double nt = t - ni;
            ph += notes[ni] / kSampleRate;
            ph -= std::floor (ph);
            const double saw = 2.0 * ph - 1.0;
            lp += 0.06 * (saw - lp);
            const double y = (0.7 * lp + 0.8 * std::sin (juce::MathConstants<double>::twoPi * ph)) * std::exp (-nt * 1.2) * juce::jmin (1.0, nt * 400.0);
            s[0][(size_t) i] = s[1][(size_t) i] = (float) y;
        }
        normalise (s);
        return s;
    }

    Stereo makePad()
    {
        const int n = (int) (kSampleRate * kSourceSeconds);
        Stereo s { std::vector<float> ((size_t) n), std::vector<float> ((size_t) n) };
        const double freqs[4] = { 220.0, 261.63, 329.63, 392.0 };
        double ph[2][4] = {}, lp[2] = {};
        for (int i = 0; i < n; ++i)
        {
            const double env = juce::jmin (1.0, i / (0.5 * kSampleRate));
            for (int c = 0; c < 2; ++c)
            {
                double acc = 0.0;
                for (int k = 0; k < 4; ++k)
                {
                    ph[c][k] += freqs[k] * (1.0 + (c == 0 ? -0.003 : 0.003) * (k + 1)) / kSampleRate;
                    ph[c][k] -= std::floor (ph[c][k]);
                    acc += 2.0 * ph[c][k] - 1.0;
                }
                lp[c] += 0.12 * (acc - lp[c]);
                s[(size_t) c][(size_t) i] = (float) (lp[c] * env);
            }
        }
        normalise (s);
        return s;
    }

    const std::array<Stereo, kNumSources>& sources()
    {
        static const std::array<Stereo, kNumSources> all { makeDrums(), makeVocal(), makeBass(), makePad() };
        return all;
    }

    struct Metrics
    {
        double inRmsDb = -200, outRmsDb = -200, deltaDb = -200, peak = 0, tailFirstRms = 0, tailLastRms = 0;
        bool finite = true;
    };

    double rmsDbOf (const Stereo& s, int from, int to)
    {
        double acc = 0.0;
        for (int c = 0; c < 2; ++c)
            for (int i = from; i < to; ++i)
                acc += (double) s[(size_t) c][(size_t) i] * s[(size_t) c][(size_t) i];
        return 10.0 * std::log10 (acc / (2.0 * juce::jmax (1, to - from)) + 1.0e-20);
    }

    // Loads factory preset `index` (list index in pm.getPresets()) and renders `src`, then silence.
    Metrics render (Proc& proc, int index, const Stereo& src, bool withTail)
    {
        auto& pm = proc.getPresetManager();
        const bool loaded = pm.loadPreset (index).ok;
        jassert (loaded);
        juce::ignoreUnused (loaded);
        proc.reset();

        Metrics m;
        const int n = (int) src[0].size();
        const int tail = withTail ? (int) (kTailSeconds * kSampleRate) : 0;
        const int total = n + tail;
        Stereo out { std::vector<float> ((size_t) total), std::vector<float> ((size_t) total) };
        juce::AudioBuffer<float> buf (2, kBlock);
        juce::MidiBuffer midi;
        for (int pos = 0; pos < total; pos += kBlock)
        {
            const int len = juce::jmin (kBlock, total - pos);
            buf.clear();
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < len; ++i)
                    if (pos + i < n)
                        buf.setSample (c, i, src[(size_t) c][(size_t) (pos + i)]);
            proc.processBlock (buf, midi);
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < len; ++i)
                {
                    const float v = buf.getSample (c, i);
                    if (! std::isfinite (v)) m.finite = false;
                    out[(size_t) c][(size_t) (pos + i)] = std::isfinite (v) ? v : 0.0f;
                    m.peak = std::max (m.peak, (double) std::abs (std::isfinite (v) ? v : 0.0f));
                }
        }
        m.inRmsDb = rmsDbOf (src, 0, n);
        m.outRmsDb = rmsDbOf (out, 0, n);
        m.deltaDb = m.outRmsDb - m.inRmsDb;
        if (withTail)
        {
            const int sec = (int) kSampleRate;
            m.tailFirstRms = std::pow (10.0, rmsDbOf (out, n, n + sec) / 20.0);
            m.tailLastRms = std::pow (10.0, rmsDbOf (out, total - sec, total) / 20.0);
        }
        return m;
    }

    int factoryIndex (PM& pm, const juce::String& name)
    {
        for (int i = 0; i < (int) pm.getPresets().size(); ++i)
            if (pm.getPresets()[(size_t) i].isFactory && pm.getPresets()[(size_t) i].name == name)
                return i;
        return -1;
    }

    // Which source each type is judged against first (the rest must stay in a wider window).
    int primarySource (const juce::String& type)
    {
        if (type == "Drums")  return 0;
        if (type == "Vocals") return 1;
        if (type == "Bass")   return 2;
        if (type == "Guitar" || type == "Keys" || type == "Synth" || type == "Ambient") return 3;
        return -1;   // full-range types: the mean of the four
    }

    double ceilingOf (const spa::preset::FactoryEntry& e)
    {
        const auto state = spa::preset::buildFactoryState (e);
        for (int i = 0; i < state.getNumChildren(); ++i)
            if (state.getChild (i).getProperty ("id").toString() == "fxLim.ceiling")
                return (double) state.getChild (i).getProperty ("value");
        return -0.3;
    }

    juce::String enabledModules (const spa::preset::FactoryEntry& e)
    {
        const auto state = spa::preset::buildFactoryState (e);
        juce::StringArray enabled;
        for (int i = 0; i < state.getNumChildren(); ++i)
        {
            const auto c = state.getChild (i);
            const auto id = c.getProperty ("id").toString();
            if (id.endsWithIgnoreCase (".enable") && (double) c.getProperty ("value") > 0.5)
                enabled.add (id.upToFirstOccurrenceOf (".", false, false).substring (2));
        }
        // chain order
        spa::dsp::FXChain::Module order[spa::dsp::FXChain::numModules];
        spa::dsp::FXChain::unpackOrder ((juce::uint64) (juce::int64) state.getProperty ("fxOrder"), order);
        const char* names[] = { "Dist", "Chorus", "Delay", "Reverb", "EQ", "Mod", "TremVib", "Limiter", "Conv", "Comp", "Grain", "Filter" };
        juce::StringArray chain;
        for (auto m : order)
        {
            const juce::String n = names[(int) m];
            const auto key = n == "TremVib" ? juce::StringArray { "Trem", "Vib" } : juce::StringArray { n == "Limiter" ? "Lim" : n };
            for (const auto& k : key)
                for (const auto& e2 : enabled)
                    if (e2.equalsIgnoreCase (k) || (n == "Filter" && e2 == "Filter"))
                        chain.addIfNotAlreadyThere (n == "TremVib" ? k : n);
        }
        return chain.joinIntoString (" > ");
    }

    juce::String settingsSummary (const spa::preset::FactoryEntry& e)
    {
        juce::StringArray out;
        for (const auto& t : juce::StringArray::fromTokens (juce::String (e.recipe), " \t\r\n", ""))
            if (t.isNotEmpty() && ! t.endsWith (".on=1") && ! t.startsWith ("order="))
                out.add (t);
        return out.joinIntoString (" ");
    }

    // ------------------------------------------------------------------------------------------
    void factoryBankListingTest()
    {
        std::cout << "factoryBankListingTest\n";
        const auto& entries = spa::preset::factoryEntries();
        expect (entries.size() == 72, "the bank has 72 presets");

        juce::String errors;
        std::set<juce::String> names;
        std::map<juce::String, int> perType;
        bool wordOk = true, unique = true, typeOk = true;
        for (const auto& e : entries)
        {
            spa::preset::buildFactoryState (e, &errors);
            unique = unique && names.insert (e.name).second;
            typeOk = typeOk && PM::canonicalType (e.type) == juce::String (e.type);
            ++perType[e.type];
            const auto word = juce::String (e.name).upToFirstOccurrenceOf (" ", false, false);
            wordOk = wordOk && juce::String (e.name).containsChar (' ') && word.isNotEmpty()
                     && juce::String (e.name).containsOnly ("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -");
        }
        expect (errors.isEmpty(), "every recipe parses (unknown parameters / bad values: none)" + (errors.isEmpty() ? juce::String() : "\n" + errors));
        expect (unique, "preset names are unique");
        expect (typeOk, "every type is one of presetTypes()");
        expect (wordOk, "names are '<Type word> <Descriptor>' (letters, digits, spaces, hyphens)");
        bool sixEach = (int) perType.size() == PM::presetTypes().size();
        for (const auto& t : PM::presetTypes())
            sixEach = sixEach && perType[t] == 6;
        expect (sixEach, "12 types x 6 presets");

        bool noConv = true, noInfFeedback = true;
        for (const auto& e : entries)
        {
            noConv = noConv && ! juce::String (e.recipe).contains ("conv.");
            noInfFeedback = noInfFeedback && ! juce::String (e.recipe).contains ("glt.release=31");
        }
        expect (noConv && noInfFeedback, "no CONV (needs an impulse) and no infinite GLITTER hold");

        // Listed in the browser model: Factory bank, read-only, right type.
        TempDir t ("factorybank");
        PM::setFactoryBankEnabled (true);
        auto proc = phase2bTests::makeIsolated (t);
        proc->getPresetManager().installFactoryBank();
        auto& pm = proc->getPresetManager();
        int factory = 0, right = 0;
        for (const auto& p : pm.getPresets())
            if (p.isFactory)
            {
                ++factory;
                for (const auto& e : entries)
                    if (p.name == e.name && p.type == e.type && p.bank == "Factory" && p.storedType == e.type && p.folder.isEmpty())
                        ++right;
            }
        expect (factory == 72 && right == 72, "all 72 are listed under Factory with their type (" + juce::String (factory) + " listed, " + juce::String (right) + " right)");
        expect (presetTests::Browser::availableTypes (pm.getPresets(), {}, {}).joinIntoString (",")
                    == "Drums,Bass,Vocals,Guitar,Keys,Synth,FX,Mixbus,Mastering,Creative,Ambient,Lo-Fi",
                "the browser's type dropdown offers all 12 types in order");

        // The recipe stamp is on every document.
        bool stamped = true;
        for (const auto& f : spa::preset::buildFactoryBank())
        {
            auto xml = juce::XmlDocument::parse (f.xml);
            stamped = stamped && xml != nullptr && xml->getIntAttribute ("factoryRecipe") == spa::preset::kFactoryRecipeVersion
                      && xml->hasTagName (PM::presetTag) && xml->getIntAttribute ("version") == PM::presetFormatVersion;
        }
        expect (stamped, "every factory document carries <SPAStripPreset ... factoryRecipe=\"" + juce::String (spa::preset::kFactoryRecipeVersion) + "\">");

        // Regenerates: dropping the list and installing again brings the bank back.
        pm.setFactoryPresets ({});
        int gone = 0;
        for (const auto& p : pm.getPresets()) gone += p.isFactory ? 1 : 0;
        pm.installFactoryBank();
        int back = 0;
        for (const auto& p : pm.getPresets()) back += p.isFactory ? 1 : 0;
        expect (gone == 0 && back == 72, "installFactoryBank rebuilds the whole bank from the table");
        PM::setFactoryBankEnabled (false);
    }

    void factoryBankLoadTest()
    {
        std::cout << "factoryBankLoadTest\n";
        TempDir t ("factoryload");
        PM::setFactoryBankEnabled (true);
        auto proc = phase2bTests::makeIsolated (t);
        proc->getPresetManager().installFactoryBank();
        auto& pm = proc->getPresetManager();

        // Spot checks: the recipe values reach the parameters, the order and the mod slot.
        expect (pm.loadPreset (factoryIndex (pm, "Vocal Radio Lead")).ok, "load Vocal Radio Lead");
        expect (std::abs (getParam (*proc, "fxDist.drive") - 0.3f) < 1.0e-4f && getParam (*proc, "fxDist.type") == 0.0f
                    && getParam (*proc, "fxEQ.band1.type") == 4.0f && std::abs (getParam (*proc, "fxEQ.band1.freq") - 5200.0f) < 1.0f,
                "recipe values: DIST drive 0.3 Soft, EQ band 2 High Cut at 5200 Hz");
        expect (proc->getFxOrder()[0] == (int) spa::dsp::FXChain::Module::eq && proc->getFxOrder()[1] == (int) spa::dsp::FXChain::Module::distortion,
                "order=eq,dist puts EQ then DIST first");

        expect (pm.loadPreset (factoryIndex (pm, "FX Radio Sweep")).ok, "load FX Radio Sweep");
        expect (proc->getModSlotTarget (0) == "fxFilter.f1.cutoff" && std::abs (getParam (*proc, "mod.slot1.depth") - 0.4f) < 1.0e-4f
                    && getParam (*proc, "sc.source") == 1.0f,
                "mod slot 1 -> FILTER 1 cutoff at 0.4 from the strip's own input envelope");

        // Loading a factory preset after another one leaves nothing of the first behind.
        expect (pm.loadPreset (factoryIndex (pm, "Master Safe Limiter")).ok, "load Master Safe Limiter");
        expect (getParam (*proc, "fxDist.enable") == 0.0f && getParam (*proc, "fxFilter.enable") == 0.0f && proc->getModSlotTarget (0).isEmpty()
                    && getParam (*proc, "fxLim.enable") == 1.0f,
                "a recipe starts from the defaults: only what it names is on, the mod slots are empty");
        expect (std::abs (getParam (*proc, pid::inputGain)) < 1.0e-6f && getParam (*proc, "fxConv.enable") == 0.0f, "...CONV is never switched on");
        PM::setFactoryBankEnabled (false);
    }

    void factoryBankUserPresetsTest()
    {
        std::cout << "factoryBankUserPresetsTest\n";
        TempDir t ("factoryuser");
        PM::setFactoryBankEnabled (true);
        auto proc = phase2bTests::makeIsolated (t);
        auto& pm = proc->getPresetManager();
        pm.installFactoryBank();

        phase2bTests::dirtyPatch (*proc);
        const auto r = pm.save ("My Own Vocal", "", false, "Vocals");
        const auto r2 = pm.save ("My Old Type", "", false, "Creative");
        expect (r.ok && r2.ok, "save two user presets");
        // A user file with a type this build predates / a legacy spelling still resolves.
        const auto before = r.file.loadFileAsString();
        const auto before2 = r2.file.loadFileAsString();

        pm.installFactoryBank();   // "regenerate": a rescan with the bank again
        pm.rescan();
        int users = 0, factory = 0;
        for (const auto& p : pm.getPresets())
            (p.isFactory ? factory : users) += 1;
        expect (users == 2 && factory == 72, "factory 72 + user 2 listed side by side (" + juce::String (factory) + " + " + juce::String (users) + ")");
        expect (r.file.loadFileAsString() == before && r2.file.loadFileAsString() == before2, "user preset files are byte-identical after the bank is reinstalled");
        expect (presetTests::infoOf (pm, "My Own Vocal") != nullptr && ! presetTests::infoOf (pm, "My Own Vocal")->isFactory
                    && presetTests::infoOf (pm, "My Own Vocal")->type == "Vocals",
                "the user preset keeps its stored type");
        // The factory folder on disk is never written (the bank is embedded).
        expect (! t.dir.getChildFile ("Factory").exists(), "nothing is written under Presets/Factory");

        // Types 11 and 12 are accepted for user presets too.
        expect (pm.save ("Amb Test", "", false, "Ambient").ok && pm.save ("Lofi Test", "", false, "Lo-Fi").ok
                    && presetTests::infoOf (pm, "Amb Test")->type == "Ambient" && presetTests::infoOf (pm, "Lofi Test")->type == "Lo-Fi",
                "Ambient and Lo-Fi are storable types");
        PM::setFactoryBankEnabled (false);
    }

    // The big one: 72 presets x 4 sources.
    void factoryBankRenderTest()
    {
        std::cout << "factoryBankRenderTest\n";
        TempDir t ("factoryrender");
        PM::setFactoryBankEnabled (true);
        auto proc = phase2bTests::makeIsolated (t, kSampleRate, kBlock);
        proc->getPresetManager().installFactoryBank();
        auto& pm = proc->getPresetManager();

        int failedLoudness = 0, failedMean = 0, failedPrimary = 0, failedNonFinite = 0, failedPeak = 0, failedTail = 0, failedSilent = 0, failedCeil = 0;
        juce::StringArray detail;
        for (const auto& e : spa::preset::factoryEntries())
        {
            const int index = factoryIndex (pm, e.name);
            if (index < 0) { expect (false, juce::String ("missing ") + e.name); continue; }
            const bool master = juce::String (e.type) == "Mastering";
            double mean = 0.0;
            for (int s = 0; s < kNumSources; ++s)
            {
                const auto m = render (*proc, index, sources()[(size_t) s], true);
                mean += m.deltaDb / kNumSources;
                const juce::String tag = juce::String (e.name) + " / " + kSourceNames[s] + ": ";
                const auto note = [&] (int& counter, const juce::String& what) { ++counter; detail.add (tag + what); };
                if (! m.finite) note (failedNonFinite, "non-finite sample");
                if (m.outRmsDb < -60.0) note (failedSilent, "silent (" + juce::String (m.outRmsDb, 1) + " dBFS)");
                const double peakLimit = master ? juce::Decibels::decibelsToGain (ceilingOf (e) + 0.1) : 1.0;
                if (m.peak > peakLimit) note (failedPeak, "peak " + juce::String (juce::Decibels::gainToDecibels (m.peak), 2) + " dBFS");
                if (m.tailLastRms > 1.0e-3 || m.tailLastRms > 0.5 * juce::jmax (m.tailFirstRms, 1.0e-6))
                    note (failedTail, "tail does not decay (first " + juce::String (juce::Decibels::gainToDecibels (m.tailFirstRms), 1) + " dB, last second "
                                          + juce::String (juce::Decibels::gainToDecibels (m.tailLastRms), 1) + " dB)");
                if (! master && (m.deltaDb > 10.0 || m.deltaDb < -10.0))
                    note (failedLoudness, "loudness change " + juce::String (m.deltaDb, 1) + " dB outside -10..+10");
                if (master && (m.deltaDb > 12.0 || m.deltaDb < -3.0))
                    note (failedLoudness, "mastering change " + juce::String (m.deltaDb, 1) + " dB outside -3..+12");
                const int primary = primarySource (e.type);
                if (! master && primary == s && std::abs (m.deltaDb) > 3.0)
                    note (failedPrimary, "primary source off by " + juce::String (m.deltaDb, 1) + " dB (limit +-3)");
            }
            if (! master && std::abs (mean) > 3.0)
            {
                ++failedMean;
                detail.add (juce::String (e.name) + ": mean change " + juce::String (mean, 1) + " dB (limit +-3)");
            }
            if (master)
            {
                const double ceil = ceilingOf (e);
                for (int s = 0; s < kNumSources; ++s)
                {
                    const auto m = render (*proc, index, sources()[(size_t) s], false);
                    if (m.peak > juce::Decibels::decibelsToGain (ceil + 0.1))
                    {
                        ++failedCeil;
                        detail.add (juce::String (e.name) + " / " + kSourceNames[s] + ": over the ceiling");
                    }
                }
            }
        }
        for (const auto& d : detail)
            std::cout << "        " << d << "\n";
        expect (failedNonFinite == 0, "no NaN / inf in any of 288 renders");
        expect (failedSilent == 0, "no render is silent");
        expect (failedPeak == 0 && failedCeil == 0, "no sample peak past 0 dBFS (Mastering: past its ceiling)");
        expect (failedTail == 0, "every tail decays (20 s of silence after 4 s of source: last second under -60 dBFS and quieter than the first)");
        expect (failedLoudness == 0, "every render within the wide loudness window (non-Mastering -10..+10 dB, Mastering -3..+12 dB)");
        expect (failedMean == 0, "every non-Mastering preset averages within +-3 dB of the input over the four sources");
        expect (failedPrimary == 0, "every non-Mastering preset is within +-3 dB on its own type's source");
        PM::setFactoryBankEnabled (false);
    }

    // `SPAStripTests --render-factory-presets [report.md]`: the audition report and the trim table.
    void renderReport (const juce::File& reportFile)
    {
        TempDir t ("factoryreport");
        PM::setFactoryBankEnabled (true);
        auto proc = phase2bTests::makeIsolated (t, kSampleRate, kBlock);
        proc->getPresetManager().installFactoryBank();
        auto& pm = proc->getPresetManager();

        juce::String md;
        md << "# SPAStrip 1.0.6 factory presets\n\n"
           << "72 presets, 12 types x 6, in the browser under Factory. Recipes are in `source/presets/FactoryBankTable.cpp`; "
              "the grammar is at the top of `source/presets/FactoryBank.cpp`. Every preset starts from the plug-in defaults (only the modules named below are on), "
              "never uses CONV, and never holds GLITTER forever.\n\n"
           << "## How the numbers were measured\n\n"
           << "`SPAStripTests --render-factory-presets` loads each preset through the browser path and renders four synthetic sources (48 kHz, 120 bpm, all peaking at -6 dBFS): "
              "**drums** (kick / snare / hats loop), **vocal** (formant-filtered glottal source with breaths), **bass** (55 Hz saw + sine notes), **pad** (four detuned saws, stereo, slow swell). "
              "Level change is the RMS of the first 4 s of output minus the RMS of the source over the same 4 s, so a reverb or delay tail that rings into the window counts as added level. "
              "Then 20 s of silence is fed in and the last second is measured (tail). Peak is the highest sample over the whole render.\n\n"
           << "Pass rules: non-Mastering presets average within +-3 dB of the input over the four sources and within +-3 dB on their own type's source "
              "(Drums on drums, Vocals on vocal, Bass on bass, Guitar / Keys / Synth / Ambient on pad); Mastering may add level but its sample peak stays at or under its limiter ceiling. "
              "All renders: no NaN or inf, no sample past 0 dBFS, not silent, tail under -60 dBFS after 20 s.\n\n"
           << "## Things to know before auditioning\n\n"
              "- **Level trims.** Make-up gain is the LIMIT module's DRIVE (ceiling -1 dB, lookahead on where the peaks needed it) or, when a preset is too loud, OUTPUT gain; INPUT gain is used a few times for band-limited sounds. "
              "Every non-Mastering preset ends in a limiter or has at least 5 dB of headroom on a -6 dBFS source, so none reaches 0 dBFS.\n"
              "- **COMP MIX is always 100 %.** Below 100 % the compressor's dry path is not phase-aligned with its crossover-split wet path, so the blend combs (a vocal at 200 Hz measured -11 dB with the ratio at 1:1 and MIX at 55 %). "
              "Parallel-style presets use COMP's UP RATIO instead. This is in the shared spa-fx Multiband, not in this bank; it is worth a look separately.\n"
              "- **Band-limited sounds** (Telephone, Radio, Megaphone, Broken Speaker, AM Radio, Radio Sweep) are trimmed to the mean of the four sources, so a bass-heavy source reads a few dB quieter and a mid-heavy one a few dB louder than the average. The tables show it.\n"
              "- **Tempo-synced settings** follow the host; the numbers below were measured at 120 bpm.\n"
              "- **Mastering** presets add level on purpose (LIMIT drive) and never pass their ceiling; they use true-peak and lookahead, which reports latency to the host.\n\n"
           << "Level change in dB (positive = louder than the input); the source a type is judged on is marked with an asterisk.\n\n";

        juce::String currentType;
        double worstMean = 0.0;
        for (const auto& e : spa::preset::factoryEntries())
        {
            if (currentType != e.type)
            {
                currentType = e.type;
                md << "## " << currentType << "\n\n";
            }
            const int index = factoryIndex (pm, e.name);
            double d[kNumSources];
            double peak = 0.0, tailDb = -200.0, tailFirstDb = -200.0, mean = 0.0;
            for (int s = 0; s < kNumSources; ++s)
            {
                const auto m = render (*proc, index, sources()[(size_t) s], true);
                d[s] = m.deltaDb;
                mean += m.deltaDb / kNumSources;
                peak = std::max (peak, m.peak);
                tailDb = std::max (tailDb, (double) juce::Decibels::gainToDecibels (m.tailLastRms, -200.0));
                tailFirstDb = std::max (tailFirstDb, (double) juce::Decibels::gainToDecibels (m.tailFirstRms, -200.0));
            }
            if (juce::String (e.type) != "Mastering")
                worstMean = std::max (worstMean, std::abs (mean));
            const int primary = primarySource (e.type);
            const auto cell = [&] (int s) { return juce::String (d[s] >= 0 ? "+" : "") + juce::String (d[s], 1) + (s == primary ? "*" : ""); };
            md << "### " << e.name << "\n\n"
               << "- Chain: " << enabledModules (e) << "\n"
               << "- Settings: `" << settingsSummary (e) << "`\n"
               << "- Level change (dB): drums " << cell (0) << ", vocal " << cell (1) << ", bass " << cell (2) << ", pad " << cell (3)
               << "; mean " << (mean >= 0 ? "+" : "") << juce::String (mean, 1) << "\n"
               << "- Peak " << juce::String (juce::Decibels::gainToDecibels (peak), 1) << " dBFS; tail after 20 s of silence "
               << (tailDb <= -120.0 ? juce::String ("below -120") : juce::String (tailDb, 0)) << " dBFS\n\n";
            std::cout << juce::String (e.name).paddedRight (' ', 28) << " drums " << cell (0).paddedLeft (' ', 6) << " vocal " << cell (1).paddedLeft (' ', 6)
                      << " bass " << cell (2).paddedLeft (' ', 6) << " pad " << cell (3).paddedLeft (' ', 6) << " mean " << juce::String (mean, 2)
                      << " peak " << juce::String (juce::Decibels::gainToDecibels (peak), 1) << " tail " << juce::String (tailDb, 0) << "\n";
        }
        md << "---\nLargest absolute mean level change over the four sources: " << juce::String (worstMean, 1) << " dB (Mastering excluded from the limit).\n";
        if (reportFile != juce::File())
            reportFile.replaceWithText (md);
        PM::setFactoryBankEnabled (false);
    }

    // `SPAStripTests --render-recipe "<recipe>"`: the same four renders for one ad-hoc recipe (calibration aid).
    void renderRecipe (const juce::String& recipe)
    {
        TempDir t ("factoryrecipe");
        auto proc = phase2bTests::makeIsolated (t, kSampleRate, kBlock);
        juce::String errors;
        const spa::preset::FactoryEntry e { "Recipe Probe", "FX", recipe.toRawUTF8() };
        const auto state = spa::preset::buildFactoryState (e, &errors);
        if (errors.isNotEmpty()) { std::cout << errors; return; }
        double mean = 0.0;
        for (int s = 0; s < kNumSources; ++s)
        {
            proc->applyPresetState (state, "probe");
            proc->reset();
            // render through the same path as the bank: a one-entry factory list
            const Stereo& src = sources()[(size_t) s];
            juce::AudioBuffer<float> buf (2, kBlock);
            juce::MidiBuffer midi;
            const int n = (int) src[0].size();
            Stereo out { std::vector<float> ((size_t) n), std::vector<float> ((size_t) n) };
            double peak = 0.0;
            for (int pos = 0; pos < n; pos += kBlock)
            {
                const int len = juce::jmin (kBlock, n - pos);
                for (int c = 0; c < 2; ++c)
                    for (int i = 0; i < kBlock; ++i)
                        buf.setSample (c, i, i < len ? src[(size_t) c][(size_t) (pos + i)] : 0.0f);
                proc->processBlock (buf, midi);
                for (int c = 0; c < 2; ++c)
                    for (int i = 0; i < len; ++i)
                    {
                        out[(size_t) c][(size_t) (pos + i)] = buf.getSample (c, i);
                        peak = std::max (peak, (double) std::abs (buf.getSample (c, i)));
                    }
            }
            const double d = rmsDbOf (out, 0, n) - rmsDbOf (src, 0, n);
            mean += d / kNumSources;
            std::cout << kSourceNames[s] << " " << juce::String (d, 2) << " dB, peak " << juce::String (juce::Decibels::gainToDecibels (peak), 1) << "\n";
        }
        std::cout << "mean " << juce::String (mean, 2) << "\n";
    }
}
