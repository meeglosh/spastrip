// Phase 2b tests: Randomize All / WILD / per-effect locks, undo / redo, presets
// plus the two review fixes (sidechain follower clamp,
// shipped-IR credits). Included from SPAStripTests.cpp, where `expect`, `Proc`,
// `pid`, `setParam`, `getParam`, `makeProc`, `Noise`, `writeTestIR` ... are in scope.

#include "reference/SPASynthRandomizer.h"
#include "presets/PresetManager.h"

namespace phase2bTests
{
    namespace fx = spa::params::id::fx;
    namespace params = spa::params;
    using Module = spa::dsp::FXChain::Module;
    using PM = spa::preset::PresetManager;

    constexpr int kNumModules = spa::dsp::FXChain::numModules;

    //==========================================================================
    // Helpers.
    void pump (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); }

    bool sameBits (float a, float b) { return std::memcmp (&a, &b, sizeof (float)) == 0; }

    // Normalised value of every registry parameter, registry order.
    std::vector<float> allValues (Proc& proc)
    {
        std::vector<float> v;
        for (const auto& def : params::all())
            v.push_back (proc.getAPVTS().getParameter (def.id)->getValue());
        return v;
    }

    // Normalised values equal within float rounding. A value an undo / preset load WRITES goes
    // norm -> real -> norm through the parameter's range (skewed / stepped), which can move it
    // by an ulp or two; one that is not written stays bit-identical.
    bool nearValues (const std::vector<float>& a, const std::vector<float>& b, float tol = 1.0e-6f)
    {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (std::abs (a[i] - b[i]) > tol)
                return false;
        return true;
    }

    bool identicalValues (const std::vector<float>& a, const std::vector<float>& b)
    {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (! sameBits (a[i], b[i]))
                return false;
        return true;
    }

    // Parameter IDs of one chain module (TREM/VIB: both fxTrem.* and fxVib.*).
    std::vector<juce::String> idsOf (Module m)
    {
        std::vector<juce::String> ids;
        for (const auto& def : params::all())
            if (const auto mod = params::moduleForSection (def.section); mod.has_value() && *mod == m)
                ids.push_back (def.id);
        return ids;
    }

    std::vector<float> valuesOf (Proc& proc, Module m)
    {
        std::vector<float> v;
        for (const auto& id : idsOf (m))
            v.push_back (proc.getAPVTS().getParameter (id)->getValue());
        return v;
    }

    int slotOf (Proc& proc, Module m) { return proc.getFxOrder().indexOf ((int) m); }

    juce::String orderKey (const juce::Array<int>& o)
    {
        juce::String s;
        for (int v : o) s << juce::String::toHexString (v);
        return s;
    }

    bool isPermutation (const juce::Array<int>& o)
    {
        if (o.size() != kNumModules)
            return false;
        std::set<int> seen (o.begin(), o.end());
        return (int) seen.size() == kNumModules && *seen.begin() == 0 && *seen.rbegin() == kNumModules - 1;
    }

    // One editor gesture: begin, many value writes, end.
    void gestureDrag (Proc& proc, const juce::String& id, float from, float to, int ticks)
    {
        auto* p = proc.getAPVTS().getParameter (id);
        p->beginChangeGesture();
        for (int i = 1; i <= ticks; ++i)
            p->setValueNotifyingHost (p->convertTo0to1 (from + (to - from) * (float) i / (float) ticks));
        p->endChangeGesture();
    }
    void gestureSet (Proc& proc, const juce::String& id, float real) { gestureDrag (proc, id, real, real, 1); }

    struct TempDir
    {
        juce::File dir;
        explicit TempDir (const juce::String& tag)
            : dir (juce::File::getSpecialLocation (juce::File::tempDirectory)
                       .getChildFile ("spastrip-" + tag + "-" + juce::String (juce::Random::getSystemRandom().nextInt (1000000))))
        {
            dir.createDirectory();
        }
        ~TempDir() { dir.deleteRecursively(); }
    };

    // A processor whose preset manager lives in its own temp folder and deletes for real.
    std::unique_ptr<Proc> makeIsolated (const TempDir& t, double sr = 48000.0, int block = 256)
    {
        auto p = makeProc (sr, block);
        p->getPresetManager().setPresetsRoot (t.dir);
        p->getPresetManager().setUseTrash (false);
        p->clearUndoHistory();
        return p;
    }

    // A moderately distinctive patch (not defaults), independent of randomize.
    void dirtyPatch (Proc& p)
    {
        setParam (p, fx::distDrive, 0.62f);
        setParam (p, fx::distEnable, 1.0f);
        setParam (p, fx::chorusRate, 1.7f);
        setParam (p, fx::reverbMode, 3.0f);
        setParam (p, fx::reverbMix, 0.41f);
        setParam (p, fx::eqEnable, 1.0f);
        setParam (p, pid::eqBand (2, fx::eqband::gain), 5.5f);
        setParam (p, fx::grainSpreadPitch, 4.25f);
        setParam (p, pid::inputGain, -3.0f);
        setParam (p, pid::mix, 0.8f);
        juce::Array<int> order;
        for (int id : { 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 11 }) order.add (id);
        p.setFxOrder (order);
        p.setModSlotTarget (0, fx::chorusRate);
        p.setModSlotTarget (4, fx::delayMix);
        setParam (p, pid::modSlotDepth (0), 0.5f);
    }

    //==========================================================================
    // D1. Sidechain follower: state clamped at 1, release starts immediately.
    void sidechainClampTest()
    {
        std::cout << "sidechainClampTest\n";
        constexpr double sr = 48000.0;
        for (float releaseMs : { 100.0f, 300.0f })
        {
            spa::mod::SidechainDetector det;
            det.prepare (sr);
            det.setSettings ({ 0.0f, 1.0f, releaseMs, 20.0f });
            constexpr int block = 64;
            std::vector<float> L (block), R (block), env (block);

            // |x| = 8 on every sample (12 kHz at 45 degrees, amplitude 8 / 0.7071): 18 dB over full scale.
            long n = 0;
            const auto feed = [&] (double level, long samples, std::vector<float>* collect)
            {
                for (long done = 0; done < samples; done += block)
                {
                    const int m = (int) std::min<long> (block, samples - done);
                    for (int i = 0; i < m; ++i, ++n)
                    {
                        const float v = (float) (level / 0.70710678118654752 * std::sin (2.0 * juce::MathConstants<double>::pi * 12000.0 * (double) n / sr + juce::MathConstants<double>::pi * 0.25));
                        L[(size_t) i] = R[(size_t) i] = v;
                    }
                    det.process (L.data(), R.data(), m, env.data(), nullptr, nullptr);
                    if (collect != nullptr) collect->insert (collect->end(), env.begin(), env.begin() + m);
                }
            };

            std::vector<float> attack;
            feed (8.0, (long) (0.3 * sr), &attack);
            float maxEnv = 0.0f;
            for (float e : attack) maxEnv = std::max (maxEnv, e);
            expect (maxEnv <= 1.0f && det.currentEnvelope() <= 1.0f, "release " + juce::String (releaseMs, 0) + " ms: envelope output and follower state never exceed 1.0 on a +18 dB signal");
            // The attack still aims at the true level, so a hot signal saturates as fast as an unclamped follower would.
            size_t at = 0;
            while (at < attack.size() && attack[at] < 0.999f) ++at;
            expect ((double) at * 1000.0 / sr < 0.5, "...and the hot signal still saturates within 0.5 ms (took " + juce::String ((double) at * 1000.0 / sr, 3) + " ms): attack not slowed by the clamp");

            // Signal stops: the release is a clean exponential from 1.0 starting AT ONCE.
            // (Before the fix the state sat at ~8 and the output stayed pinned at 1 for ln(8) time constants.)
            std::vector<float> rel;
            feed (0.0, (long) (3.0 * releaseMs * 0.001 * sr), &rel);
            const auto at_ms = [&] (double ms) { return rel[(size_t) std::llround (ms * 0.001 * sr)]; };
            const double tc = releaseMs;
            expect (std::abs (at_ms (tc * 0.1) - std::exp (-0.1)) < 0.02, "release " + juce::String (releaseMs, 0) + " ms: at 0.1 tau the envelope is e^-0.1 (" + juce::String (at_ms (tc * 0.1), 3) + "), i.e. already falling (pinned would read 1.0)");
            expect (std::abs (at_ms (tc) - std::exp (-1.0)) < 0.03, "...and at 1 tau it is 1/e (" + juce::String (at_ms (tc), 3) + ")");
            bool monotone = true;
            for (size_t i = 1; i < rel.size(); ++i) monotone = monotone && rel[i] <= rel[i - 1] + 1.0e-6f;
            expect (monotone, "...falling monotonically from the first sample");
        }
    }

    //==========================================================================
    // D2. The embedded credits describe exactly the shipped IRs.
    void creditsMatchShippedIRsTest()
    {
        std::cout << "creditsMatchShippedIRsTest\n";
        const auto credits = Proc::getFactoryIRCredits();
        const auto manifest = juce::JSON::parse (juce::File (SPASTRIP_SOURCE_DIR).getChildFile ("assets/irs/manifest.json"));
        const auto* irs = manifest.getProperty ("irs", juce::var()).getArray();
        expect (irs != nullptr && irs->size() > 19, "manifest lists the shipped IRs and the alternate (" + juce::String (irs != nullptr ? irs->size() : 0) + " entries)");
        if (irs == nullptr)
            return;

        // The title as the manifest's credit line spells it: "Title" by ..., or "AKIR Name impulse response by ...".
        const auto titleOf = [] (const juce::String& creditLine)
        {
            if (creditLine.startsWithChar ('"'))
                return creditLine.substring (1).upToFirstOccurrenceOf ("\" by ", false, false);
            return creditLine.upToFirstOccurrenceOf (" impulse response by", false, false);
        };

        int shipped = 0, found = 0;
        juce::StringArray missing;
        for (const auto& e : *irs)
        {
            const auto role = e.getProperty ("role", {}).toString();
            const auto id = e.getProperty ("id", {}).toString();
            const auto title = titleOf (e.getProperty ("credit_line", {}).toString());
            const auto url = e.getProperty ("source_url", {}).toString();
            if (role == "representative")
            {
                ++shipped;
                // Display names are short ("Hall", "Forest") and would match anything, so the
                // credit line's own title is the key; Freesound items also carry a unique page URL.
                const bool isAkir = url.contains ("adventurekid");
                if (title.isNotEmpty() && credits.contains (title) && (isAkir || credits.contains (url)))
                    ++found;
                else
                    missing.add (id);
            }
            else
            {
                const auto display = e.getProperty ("display_name", {}).toString();
                expect (title.isNotEmpty() && ! credits.contains (title) && ! credits.contains (id)
                            && ! credits.contains (display.upToFirstOccurrenceOf (" (", false, false)),
                        "the unshipped '" + role + "' IR (" + id + ") is NOT in the embedded credits");
            }
        }
        expect (shipped == 19 && found == 19, "every one of the 19 shipped IRs is credited by title (" + juce::String (found) + "/" + juce::String (shipped) + (missing.isEmpty() ? juce::String() : " missing: " + missing.joinIntoString (", ")) + ")");
        expect (! credits.contains ("Uni Springer") && ! credits.contains ("UniSpringer"), "no 'Uni Springer' anywhere in the shipped credits text");

        int bullets = 0;
        for (const auto& line : juce::StringArray::fromLines (credits))
            if (line.startsWith ("- "))
                ++bullets;
        expect (bullets == (int) Proc::getFactoryIRs().size(), "the credits list exactly as many entries as there are shipped IRs (" + juce::String (bullets) + ")");
    }

    //==========================================================================
    // Sampling parity with SPASynth's sampleRandomValue.
    void samplingParityTest()
    {
        std::cout << "samplingParityTest\n";
        // Every spec in the registry, plus synthetic edge specs, at a table of WILD values.
        std::vector<params::RandomSpec> specs;
        for (const auto& def : params::all())
            specs.push_back (def.random);
        specs.push_back ({ true, 0.0f, 1.0f, 0.5f, 0.0f });
        specs.push_back ({ true, 0.3f, 0.7f, 0.9f, 1.0f });
        specs.push_back ({ true, 0.8f, 0.2f, 0.5f, 0.5f });    // inverted window
        specs.push_back ({ true, 0.5f, 0.5f, 0.1f, 0.3f });    // zero-width window
        const float wilds[] = { 0.0f, 0.1f, 0.25f, 0.4999f, 0.5f, 0.5001f, 0.6f, 0.75f, 0.9f, 1.0f };

        long compared = 0, mismatches = 0;
        for (const auto& spec : specs)
            for (float w : wilds)
                for (int seed : { 1, 7, 12345 })
                {
                    juce::Random a (seed), b (seed);
                    for (int i = 0; i < 40; ++i, ++compared)
                    {
                        const float x = params::sampleRandomValue (spec, w, a);
                        const float y = synthref::sampleRandomValue (spec, w, b);
                        if (! sameBits (x, y))
                            ++mismatches;
                    }
                }
        expect (mismatches == 0, "sampleRandomValue is bit-identical to SPASynth's over " + juce::String ((juce::int64) compared) + " draws (" + juce::String ((int) specs.size()) + " specs x 10 WILD values x 3 seeds), " + juce::String ((juce::int64) mismatches) + " mismatches");

        // Sanity of the shape itself (so the table is not vacuous): WILD 1 opens to the full range, WILD 0 tightens.
        juce::Random r (3);
        params::RandomSpec narrow { true, 0.4f, 0.6f, 0.5f, 0.0f };
        float lo1 = 1, hi1 = 0, lo0 = 1, hi0 = 0;
        for (int i = 0; i < 4000; ++i)
        {
            const float a = params::sampleRandomValue (narrow, 1.0f, r), b = params::sampleRandomValue (narrow, 0.0f, r);
            lo1 = std::min (lo1, a); hi1 = std::max (hi1, a); lo0 = std::min (lo0, b); hi0 = std::max (hi0, b);
        }
        expect (lo1 < 0.02f && hi1 > 0.98f, "WILD 1 opens a 0.4..0.6 window to the full range");
        expect (lo0 > 0.47f && hi0 < 0.53f, "WILD 0 tightens it toward the centre");
    }

    // End to end: the whole roll (parameters, order, limiter, convStart, freeze) against the transcribed synth flow.
    void randomizeParityWithSynthTest()
    {
        std::cout << "randomizeParityWithSynthTest\n";
        int mismatches = 0, runs = 0;
        for (float wild : { 0.0f, 0.3f, 0.5f, 0.8f, 1.0f })
            for (int seed = 1; seed <= 12; ++seed)
            {
                auto a = makeProc (48000.0, 256);
                auto b = makeProc (48000.0, 256);
                for (auto* p : { a.get(), b.get() })
                {
                    setParam (*p, fx::grainFreeze, 1.0f);          // must be forced off by the roll
                    setParam (*p, fx::convStart, 0.9f);
                    p->setRandomWildness (wild);
                }
                juce::Random ra (seed), rb (seed);
                // Chained: the order shuffle and every "previous value" dependency carry across rolls.
                for (int roll = 0; roll < 4; ++roll, ++runs)
                {
                    a->randomizeAll (ra);
                    synthref::randomizeAllFx (*b, wild, rb);
                    // The reference is the synth's flow and knows no FILTER roll guards (a strip-only rule);
                    // apply them to its result, so the comparison covers every parameter, the filter's
                    // included, and still proves the draws and the order shuffle are the synth's.
                    params::applyFilterGuards (b->getAPVTS());
                    if (! identicalValues (allValues (*a), allValues (*b)) || a->getFxOrder() != b->getFxOrder())
                        ++mismatches;
                }
            }
        expect (mismatches == 0, "Randomize All (no locks) leaves the same parameters and chain order as SPASynth's FX flow: " + juce::String (runs) + " chained rolls over 5 WILD values x 12 seeds, " + juce::String (mismatches) + " differences");
    }

    //==========================================================================
    // Behaviour of the roll.
    void randomizeBasicsTest()
    {
        std::cout << "randomizeBasicsTest\n";
        auto proc = makeProc (48000.0, 256);
        auto& apvts = proc->getAPVTS();

        const auto before = allValues (*proc);
        proc->randomizeAll();
        const auto after = allValues (*proc);
        int changed = 0;
        for (size_t i = 0; i < before.size(); ++i)
            if (std::abs (before[i] - after[i]) > 1.0e-4f)
                ++changed;
        expect (changed > 40, "a re-roll changes many parameters (" + juce::String (changed) + ")");

        // Constrained windows hold at default WILD; ones RandomSpec disables never roll.
        bool distDrive = true, noEnableRolls = true;
        const auto drive0 = apvts.getParameter (fx::distDrive)->getValue();
        juce::ignoreUnused (drive0);
        const auto convEnable0 = apvts.getParameter (fx::convEnable)->getValue();
        const auto eqType0 = apvts.getParameter (pid::eqBand (0, fx::eqband::type))->getValue();
        const auto grainSync0 = apvts.getParameter (fx::grainSync)->getValue();
        for (int i = 0; i < 60; ++i)
        {
            proc->randomizeAll();
            distDrive = distDrive && apvts.getParameter (fx::distDrive)->getValue() <= 0.8f + 1.0e-6f;
            noEnableRolls = noEnableRolls
                && sameBits (apvts.getParameter (fx::convEnable)->getValue(), convEnable0)
                && sameBits (apvts.getParameter (pid::eqBand (0, fx::eqband::type))->getValue(), eqType0)
                && sameBits (apvts.getParameter (fx::grainSync)->getValue(), grainSync0);
        }
        expect (distDrive, "dist drive stays inside its constrained window (<= 0.8) at default WILD");
        expect (noEnableRolls, "parameters whose RandomSpec is disabled (fxConv.enable, EQ band type, grain sync) never roll");

        // WILD default and clamping.
        expect (juce::approximatelyEqual (proc->getRandomWildness(), 0.5f), "WILD defaults to 0.5");
        proc->setRandomWildness (3.0f);
        expect (juce::approximatelyEqual (proc->getRandomWildness(), 1.0f), "WILD is clamped to 0..1");
        proc->setRandomWildness (0.5f);

        // WILD actually changes the spread of the roll.
        const auto spread = [&] (float wild)
        {
            proc->setRandomWildness (wild);
            juce::Random rng (99);
            double lo = 1.0, hi = 0.0;
            for (int i = 0; i < 300; ++i)
            {
                proc->randomizeAll (rng);
                const double v = apvts.getParameter (fx::reverbSize)->getValue();
                lo = std::min (lo, v); hi = std::max (hi, v);
            }
            return hi - lo;
        };
        const double tight = spread (0.0), wide = spread (1.0);
        expect (tight < wide && tight < 0.2 && wide > 0.9, "reverb size spans " + juce::String (tight, 2) + " at WILD 0 and " + juce::String (wide, 2) + " at WILD 1");
    }

    void nonFxStateUntouchedTest()
    {
        std::cout << "nonFxStateUntouchedTest\n";
        auto proc = makeProc (48000.0, 256, 2, 2, 2);
        setParam (*proc, pid::inputGain, -5.0f);
        setParam (*proc, pid::outputGain, 2.5f);
        setParam (*proc, pid::mix, 0.7f);
        setParam (*proc, pid::oversampling, 1.0f);
        setParam (*proc, pid::sc::source, 1.0f);
        setParam (*proc, pid::sc::gain, 6.0f);
        setParam (*proc, pid::sc::attack, 3.0f);
        setParam (*proc, pid::sc::release, 300.0f);
        setParam (*proc, pid::sc::hpf, 200.0f);
        setParam (*proc, pid::sc::listen, 1.0f);
        for (int s = 0; s < 8; ++s)
        {
            setParam (*proc, pid::modSlotDepth (s), (s % 2 == 0 ? 0.4f : -0.6f));
            proc->setModSlotTarget (s, Proc::getModTargets()[(size_t) (s * 11)].id);
        }
        proc->loadFactoryIR ("dark-plate");
        proc->setLocked (Module::reverb, true);
        proc->setLocked (Module::grain, true);
        proc->setRandomWildness (0.9f);

        std::vector<juce::String> keep;
        for (const auto& def : params::all())
            if (! params::moduleForSection (def.section).has_value())
                keep.push_back (def.id);
        std::vector<float> keepBefore;
        for (const auto& id : keep) keepBefore.push_back (proc->getAPVTS().getParameter (id)->getValue());
        const auto mask = proc->getFxLockMask();
        juce::StringArray slots;
        for (int s = 0; s < 8; ++s) slots.add (proc->getModSlotTarget (s));

        for (int i = 0; i < 80; ++i)
            proc->randomizeAll();

        bool same = true;
        for (size_t i = 0; i < keep.size(); ++i)
            same = same && sameBits (keepBefore[i], proc->getAPVTS().getParameter (keep[i])->getValue());
        expect (same, "global.*, sc.* and mod.slotN.depth (" + juce::String ((int) keep.size()) + " parameters) are bit-identical after 80 rolls");
        bool slotsSame = true;
        for (int s = 0; s < 8; ++s) slotsSame = slotsSame && proc->getModSlotTarget (s) == slots[s];
        expect (slotsSame, "mod slot targets untouched");
        expect (proc->getConvolutionIRSource() == "factory:dark-plate" && proc->hasConvolutionIR(), "the loaded IR is untouched");
        expect (proc->getFxLockMask() == mask && juce::approximatelyEqual (proc->getRandomWildness(), 0.9f), "lock mask and WILD are untouched");
    }

    void lockedModuleTest()
    {
        std::cout << "lockedModuleTest\n";
        for (int mi = 0; mi < kNumModules; ++mi)
        {
            const auto module = (Module) mi;
            auto proc = makeProc (48000.0, 256);
            juce::Random setup (900 + mi);
            proc->randomizeAll (setup);                         // a distinctive, non-default start
            // Make "forced on / reset to defaults" visible for the limiter, whichever way it is tested.
            setParam (*proc, fx::limEnable, 0.0f);
            setParam (*proc, fx::limDrive, 7.0f);
            setParam (*proc, fx::limRelease, 400.0f);
            // Put every module in a non-default state first, so "unchanged" is not just "default".
            proc->setLocked (module, true);

            const auto lockedBefore = valuesOf (*proc, module);
            const int slotBefore = slotOf (*proc, module);
            std::vector<std::vector<float>> others (kNumModules);
            for (int o = 0; o < kNumModules; ++o) others[(size_t) o] = valuesOf (*proc, (Module) o);
            std::vector<bool> otherChanged (kNumModules, false);
            std::set<juce::String> orders;

            bool lockedOk = true, slotOk = true, permOk = true;
            for (int seed = 1; seed <= 200; ++seed)
            {
                juce::Random rng (seed);
                proc->randomizeAll (rng);
                lockedOk = lockedOk && identicalValues (valuesOf (*proc, module), lockedBefore);
                slotOk = slotOk && slotOf (*proc, module) == slotBefore;
                permOk = permOk && isPermutation (proc->getFxOrder());
                orders.insert (orderKey (proc->getFxOrder()));
                for (int o = 0; o < kNumModules; ++o)
                    if (o != mi && ! identicalValues (valuesOf (*proc, (Module) o), others[(size_t) o]))
                        otherChanged[(size_t) o] = true;
            }
            const auto name = params::moduleName (module);
            expect (lockedOk, name + " locked: all " + juce::String ((int) lockedBefore.size()) + " parameters (enable included) bit-identical across 200 seeded rolls");
            expect (slotOk && permOk, name + " locked: keeps slot " + juce::String (slotBefore) + " in every one of the 200 orders, which are always valid permutations");
            int unchanged = 0;
            for (int o = 0; o < kNumModules; ++o)
                if (o != mi && ! otherChanged[(size_t) o]) { ++unchanged; std::cout << "      (module " << params::moduleName ((Module) o) << " never changed)\n"; }
            expect (unchanged == 0, name + " locked: every other module still rolls");
            expect (orders.size() > 20, name + " locked: the rest of the chain still shuffles (" + juce::String ((int) orders.size()) + " distinct orders in 200 rolls)");
        }
    }

    void allLockedTest()
    {
        std::cout << "allLockedTest\n";
        auto proc = makeProc (48000.0, 256);
        juce::Random setup (4);
        proc->randomizeAll (setup);
        setParam (*proc, fx::limEnable, 0.0f);
        setParam (*proc, fx::limDrive, 9.0f);
        setParam (*proc, fx::grainFreeze, 1.0f);
        setParam (*proc, fx::convStart, 0.9f);
        for (int m = 0; m < kNumModules; ++m)
            proc->setLocked ((Module) m, true);
        proc->clearUndoHistory();
        const auto before = allValues (*proc);
        const auto orderBefore = proc->getFxOrder();
        const auto snap = proc->capturePatchSnapshot();
        proc->setRandomWildness (1.0f);
        for (int seed = 1; seed <= 100; ++seed)
        {
            juce::Random rng (seed);
            proc->randomizeAll (rng);
        }
        expect (identicalValues (allValues (*proc), before), "all 12 locked: every parameter (limiter, freeze, convStart included) bit-identical after 100 rolls");
        expect (proc->getFxOrder() == orderBefore && proc->capturePatchSnapshot() == snap, "...and the chain order and whole patch are unchanged");
        expect (! proc->canUndo(), "...and a roll that changes nothing records no undo step");
    }

    void limiterRulesTest()
    {
        std::cout << "limiterRulesTest\n";
        {   // unlocked: forced on at defaults, same slot, every roll, every WILD
            auto proc = makeProc (48000.0, 256);
            bool ok = true, slotOk = true;
            const int slot0 = slotOf (*proc, Module::limiter);
            for (float wild : { 0.0f, 0.5f, 1.0f })
            {
                proc->setRandomWildness (wild);
                for (int seed = 1; seed <= 60; ++seed)
                {
                    setParam (*proc, fx::limEnable, 0.0f);
                    setParam (*proc, fx::limDrive, 11.0f);
                    setParam (*proc, fx::limCeiling, -6.0f);
                    setParam (*proc, fx::limTruePeak, 1.0f);
                    juce::Random rng (seed);
                    proc->randomizeAll (rng);
                    for (const auto& id : idsOf (Module::limiter))
                    {
                        auto* p = proc->getAPVTS().getParameter (id);
                        const float want = id == fx::limEnable ? 1.0f : p->getDefaultValue();
                        ok = ok && std::abs (p->getValue() - want) < 1.0e-6f;
                    }
                    slotOk = slotOk && slotOf (*proc, Module::limiter) == slot0;
                }
            }
            expect (ok, "limiter unlocked: forced ON with all 10 parameters at their defaults after every roll (180 rolls, 3 WILD values)");
            expect (slotOk, "limiter unlocked: keeps its slot (" + juce::String (slot0) + ") whatever else shuffles");
        }
        {   // the limiter keeps a non-default slot too
            auto proc = makeProc (48000.0, 256);
            juce::Array<int> order;
            for (int id : { 3, 1, 7, 5, 9, 0, 2, 10, 4, 6, 8, 11 }) order.add (id);
            proc->setFxOrder (order);
            bool ok = true;
            for (int seed = 1; seed <= 100; ++seed)
            {
                juce::Random rng (seed);
                proc->randomizeAll (rng);
                ok = ok && slotOf (*proc, Module::limiter) == 2;
            }
            expect (ok, "limiter in slot 2 of a custom order stays in slot 2");
        }
        {   // locked: not even looked at
            auto proc = makeProc (48000.0, 256);
            setParam (*proc, fx::limEnable, 0.0f);
            setParam (*proc, fx::limDrive, 5.0f);
            proc->setLocked (Module::limiter, true);
            for (int seed = 1; seed <= 60; ++seed)
            {
                juce::Random rng (seed);
                proc->randomizeAll (rng);
            }
            expect (getParam (*proc, fx::limEnable) == 0.0f && std::abs (getParam (*proc, fx::limDrive) - 5.0f) < 1.0e-4f, "limiter locked: left completely untouched (still off, drive 5.0)");
        }
    }

    void tremVibLockTest()
    {
        std::cout << "tremVibLockTest\n";
        const auto ids = idsOf (Module::tremVib);
        bool hasTrem = false, hasVib = false;
        for (const auto& id : ids) { hasTrem = hasTrem || id.startsWith ("fxTrem."); hasVib = hasVib || id.startsWith ("fxVib."); }
        expect (hasTrem && hasVib && ids.size() == 14, "the TREM/VIB module owns both fxTrem.* and fxVib.* (" + juce::String ((int) ids.size()) + " parameters)");

        auto proc = makeProc (48000.0, 256);
        juce::Random setup (21);
        proc->randomizeAll (setup);
        setParam (*proc, fx::tremEnable, 1.0f);
        setParam (*proc, fx::vibEnable, 1.0f);
        proc->setLocked (Module::tremVib, true);
        const auto before = valuesOf (*proc, Module::tremVib);
        bool ok = true;
        for (int seed = 1; seed <= 100; ++seed)
        {
            juce::Random rng (seed);
            proc->randomizeAll (rng);
            ok = ok && identicalValues (valuesOf (*proc, Module::tremVib), before);
        }
        expect (ok, "locking TREM/VIB holds every fxTrem.* AND fxVib.* parameter (the two enables included) over 100 rolls");
        proc->setLocked (Module::tremVib, false);
        juce::Random rng (5);
        proc->randomizeAll (rng);
        expect (! identicalValues (valuesOf (*proc, Module::tremVib), before), "unlocked again, the module rolls");
    }

    void grainConvRulesTest()
    {
        std::cout << "grainConvRulesTest\n";
        auto proc = makeProc (48000.0, 256);
        auto& apvts = proc->getAPVTS();
        proc->setRandomWildness (1.0f);

        bool freezeOff = true, startCap = true;
        float maxStart = 0.0f;
        for (int seed = 1; seed <= 400; ++seed)
        {
            setParam (*proc, fx::grainFreeze, 1.0f);
            juce::Random rng (seed);
            proc->randomizeAll (rng);
            freezeOff = freezeOff && apvts.getParameter (fx::grainFreeze)->getValue() == 0.0f;
            const float s = apvts.getParameter (fx::convStart)->convertFrom0to1 (apvts.getParameter (fx::convStart)->getValue());
            maxStart = std::max (maxStart, s);
            startCap = startCap && s <= 0.5f;
        }
        expect (freezeOff, "grain unlocked: FREEZE is forced off by every roll (it is never rolled on)");
        expect (startCap, "convolve start never exceeds 0.5 at WILD 1 over 400 rolls (max " + juce::String (maxStart, 3) + ")");

        // anti-vacuous: the cap really is what holds it, because WILD 1 opens the window to 1.0
        float uncapped = 0.0f;
        {
            juce::Random rng (3);
            for (int i = 0; i < 400; ++i)
                uncapped = std::max (uncapped, params::sampleRandomValue (params::find (fx::convStart)->random, 1.0f, rng));
        }
        expect (uncapped > 0.9f, "(the spec alone would reach " + juce::String (uncapped, 2) + " at WILD 1, so the cap is doing the work)");

        // locked: neither rule applies
        proc->setLocked (Module::grain, true);
        proc->setLocked (Module::convolve, true);
        setParam (*proc, fx::grainFreeze, 1.0f);
        setParam (*proc, fx::convStart, 0.9f);
        for (int seed = 1; seed <= 50; ++seed)
        {
            juce::Random rng (seed);
            proc->randomizeAll (rng);
        }
        expect (getParam (*proc, fx::grainFreeze) == 1.0f && std::abs (getParam (*proc, fx::convStart) - 0.9f) < 1.0e-6f, "grain / convolve locked: FREEZE stays on and start stays at 0.9");
    }

    void orderRulesTest()
    {
        std::cout << "orderRulesTest\n";
        // Direct, on the pure shuffle: every lock mask class.
        const juce::Array<int> start = [] { juce::Array<int> a; for (int id : { 5, 0, 8, 2, 10, 4, 7, 1, 9, 11, 3, 6 }) a.add (id); return a; }();
        bool perm = true, fixedOk = true;
        std::set<juce::String> shuffledOrders;
        for (int seed = 1; seed <= 300; ++seed)
        {
            juce::Random pick (seed * 31);
            const juce::uint32 mask = (juce::uint32) pick.nextInt (1 << kNumModules);
            juce::Random rng (seed);
            const auto out = params::shuffleFxOrder (start, mask, rng);
            perm = perm && isPermutation (out);
            for (int slot = 0; slot < kNumModules; ++slot)
            {
                const int id = start[slot];
                const bool fixed = id == (int) Module::limiter || (mask & (1u << id)) != 0;
                if (fixed && out[slot] != id)
                    fixedOk = false;
            }
            if (mask == 0)
                shuffledOrders.insert (orderKey (out));
        }
        expect (perm, "shuffleFxOrder always returns a valid permutation (300 random lock masks)");
        expect (fixedOk, "locked modules and the limiter keep their exact slot index under every mask");
        // Nothing movable: unchanged. Exactly one movable: unchanged. A bad input is returned as given.
        juce::Random rng (1);
        expect (params::shuffleFxOrder (start, params::allLocksMask, rng) == start, "all locked: order unchanged");
        juce::Array<int> bad; bad.add (1); bad.add (1);
        expect (params::shuffleFxOrder (bad, 0, rng) == bad, "a non-permutation is returned untouched");

        // Without locks, it is exactly the synth's shuffle: limiter fixed, the other ten Fisher-Yates.
        bool synthSame = true;
        for (int seed = 1; seed <= 100; ++seed)
        {
            juce::Random a (seed), b (seed);
            const auto got = params::shuffleFxOrder (start, 0, a);
            const int limiterPos = start.indexOf ((int) Module::limiter);
            juce::Array<int> others;
            for (int id : start) if (id != (int) Module::limiter) others.add (id);
            for (int i = others.size() - 1; i > 0; --i) std::swap (others.getReference (i), others.getReference (b.nextInt (i + 1)));
            juce::Array<int> want; int oi = 0;
            for (int pos = 0; pos < start.size(); ++pos) want.add (pos == limiterPos ? (int) Module::limiter : others[oi++]);
            synthSame = synthSame && got == want;
        }
        expect (synthSame, "with no locks the shuffle equals SPASynth's (limiter keeps its slot, the rest Fisher-Yates) for 100 seeds");

        // On the processor: orders actually vary across seeds.
        auto proc = makeProc (48000.0, 256);
        std::set<juce::String> seen;
        bool ok = true;
        for (int seed = 1; seed <= 200; ++seed)
        {
            juce::Random r (seed);
            proc->randomizeAll (r);
            ok = ok && isPermutation (proc->getFxOrder()) && slotOf (*proc, Module::limiter) == kNumModules - 1;
            seen.insert (orderKey (proc->getFxOrder()));
        }
        expect (ok && seen.size() > 150, "the processor's order is a permutation with the limiter last and shuffles across seeds (" + juce::String ((int) seen.size()) + " distinct orders in 200 rolls)");
    }

    // Randomize All, repeatedly, while audio runs: the limiter safety net must keep the output
    // finite and bounded whatever the dice land on (SPASynth's "randomize is never painful" intent).
    void randomizeAudioSoakTest()
    {
        std::cout << "randomizeAudioSoakTest\n";
        constexpr int block = 256;
        for (float wild : { 0.5f, 1.0f })
        {
            auto proc = makeProc (48000.0, block);
            proc->setRandomWildness (wild);
            juce::AudioBuffer<float> buf (2, block); juce::MidiBuffer midi; Noise nz (17);
            bool finite = true;
            float peak = 0.0f;
            juce::Random rng (2024);
            for (int roll = 0; roll < 80; ++roll)
            {
                proc->randomizeAll (rng);
                for (int b = 0; b < 12; ++b)
                {
                    for (int c = 0; c < 2; ++c) for (int k = 0; k < block; ++k) buf.setSample (c, k, 0.5f * nz.next());
                    proc->processBlock (buf, midi);
                    finite = finite && allFinite (buf);
                    peak = std::max (peak, buf.getMagnitude (0, block));
                }
                proc->serviceMessageThread();
            }
            std::cout << "  ..   WILD " << wild << ": peak " << peak << " over 80 rolls\n";
            expect (finite && peak < 1.0f, "WILD " + juce::String (wild, 1) + ": 80 chained rolls of noise stay finite and bounded (peak " + juce::String (peak, 2) + ")");
        }
    }

    void randomizeKeepsFxStateTest()
    {
        std::cout << "randomizeKeepsFxStateTest\n";
        // SPASynth does not reset the FX chain on Randomize All (killing tails on every roll was judged
        // unwanted); SPAStrip mirrors that. Reverb locked so its tail is demonstrably still ringing.
        constexpr int block = 256;
        auto proc = makeProc (48000.0, block);
        setAllEffects (*proc, false);
        setParam (*proc, fx::reverbEnable, 1.0f);
        setParam (*proc, fx::reverbMix, 1.0f);
        setParam (*proc, fx::reverbDecay, 6.0f);
        proc->setLocked (Module::reverb, true);
        proc->setRandomWildness (0.0f);
        juce::AudioBuffer<float> buf (2, block); juce::MidiBuffer midi; Noise nz (8);
        for (int i = 0; i < 60; ++i) { for (int c = 0; c < 2; ++c) for (int k = 0; k < block; ++k) buf.setSample (c, k, 0.4f * nz.next()); proc->processBlock (buf, midi); }
        juce::Random rng (2);
        proc->randomizeAll (rng);
        setParam (*proc, fx::limEnable, 0.0f);
        setParam (*proc, fx::compEnable, 0.0f);
        buf.clear();
        proc->processBlock (buf, midi);
        expect (buf.getMagnitude (0, block) > 1.0e-4f, "after Randomize All the locked reverb's tail is still ringing: the roll did not reset the FX chain (peak " + juce::String (buf.getMagnitude (0, block), 5) + ")");
    }

    //==========================================================================
    // Persistence of locks / WILD; preset exclusions.
    void lockPersistenceTest()
    {
        std::cout << "lockPersistenceTest\n";
        TempDir t ("persist");
        auto a = makeIsolated (t);
        a->setLocked (Module::distortion, true);
        a->setLocked (Module::tremVib, true);
        a->setLocked (Module::grain, true);
        a->setRandomWildness (0.83f);
        const auto mask = a->getFxLockMask();
        expect (mask == ((1u << (int) Module::distortion) | (1u << (int) Module::tremVib) | (1u << (int) Module::grain)), "lock mask is keyed by the Module enum value");
        expect (a->isLocked (Module::distortion) && ! a->isLocked (Module::reverb) && a->isLocked (Module::tremVib), "isLocked / setLocked");

        juce::MemoryBlock block;
        a->getStateInformation (block);
        auto xml = juce::AudioProcessor::getXmlFromBinary (block.getData(), (int) block.getSize());
        expect (xml != nullptr && xml->getIntAttribute ("fxLockMask") == (int) mask && std::abs (xml->getDoubleAttribute ("randomWildness") - 0.83) < 1.0e-6, "host state carries fxLockMask and randomWildness");
        auto b = makeIsolated (t);
        b->setStateInformation (block.getData(), (int) block.getSize());
        expect (b->getFxLockMask() == mask && std::abs (b->getRandomWildness() - 0.83f) < 1.0e-6f, "a restored session has the same locks and WILD");

        // An old state without them: unlocked, default WILD.
        auto c = makeIsolated (t);
        c->setLocked (Module::eq, true);
        c->setRandomWildness (0.1f);
        juce::MemoryBlock fresh;
        makeIsolated (t)->getStateInformation (fresh);
        c->setStateInformation (fresh.getData(), (int) fresh.getSize());
        expect (c->getFxLockMask() == 0 && juce::approximatelyEqual (c->getRandomWildness(), 0.5f), "a state saved without them restores unlocked, WILD 0.5");

        // Presets: not saved, not applied.
        dirtyPatch (*a);
        setParam (*a, pid::oversampling, 2.0f);
        const auto saved = a->getPresetManager().save ("Exclusions", "");
        expect (saved.ok, "preset saves");
        const auto text = saved.file.loadFileAsString();
        expect (! text.contains ("fxLockMask") && ! text.contains ("randomWildness") && ! text.contains ("global.oversampling")
                    && ! text.contains ("presetName") && ! text.contains ("presetEdited"),
                "the .spastrip file carries no lock mask, WILD, oversampling or preset identity");

        auto d = makeIsolated (t);
        d->setLocked (Module::comp, true);
        d->setRandomWildness (0.2f);
        setParam (*d, pid::oversampling, 1.0f);
        d->getAPVTS().state.setProperty ("uiScale", 1.5, nullptr);
        expect (d->getPresetManager().load (saved.file).ok, "preset loads");
        expect (d->getFxLockMask() == (1u << (int) Module::comp) && std::abs (d->getRandomWildness() - 0.2f) < 1.0e-6f, "loading a preset leaves the session's locks and WILD alone");
        expect (getParam (*d, pid::oversampling) == 1.0f, "...and its oversampling (the preset's session had 4x, this one keeps 2x)");
        expect (d->getAPVTS().state.getProperty ("uiScale").toString() == "1.5", "...and UI properties");
        expect (std::abs (getParam (*d, fx::distDrive) - 0.62f) < 1.0e-5f && getParam (*d, pid::mix) > 0.79f, "...while the sound itself loaded");

        // A hand-edited file that tries to carry them anyway.
        auto doc = juce::XmlDocument::parse (saved.file);
        auto* state = doc->getFirstChildElement();
        state->setAttribute ("fxLockMask", 2047);
        state->setAttribute ("randomWildness", 0.99);
        state->setAttribute ("uiSmuggled", "yes");
        auto* os = state->createNewChildElement ("PARAM");
        os->setAttribute ("id", "global.oversampling");
        os->setAttribute ("value", 0.0);
        const auto sneaky = t.dir.getChildFile ("sneaky.spastrip");
        doc->writeTo (sneaky);
        expect (d->getPresetManager().load (sneaky).ok, "the hand-edited preset loads");
        expect (d->getFxLockMask() == (1u << (int) Module::comp) && std::abs (d->getRandomWildness() - 0.2f) < 1.0e-6f && getParam (*d, pid::oversampling) == 1.0f
                    && ! d->getAPVTS().state.hasProperty ("uiSmuggled"),
                "...but it cannot set locks, WILD, oversampling or UI properties");
    }

    //==========================================================================
    // Undo / redo.
    void undoGestureTest()
    {
        std::cout << "undoGestureTest\n";
        auto proc = makeProc (48000.0, 256);
        proc->clearUndoHistory();
        expect (! proc->canUndo() && ! proc->canRedo(), "fresh: nothing to undo or redo");

        gestureDrag (*proc, fx::chorusRate, 0.8f, 3.0f, 25);
        expect (proc->getUndoStepCount() == 1, "a whole knob drag (25 value writes) is ONE step, got " + juce::String (proc->getUndoStepCount()));
        expect (proc->getUndoLabel().containsIgnoreCase ("Chorus Rate"), "label names the control: " + proc->getUndoLabel());
        expect (std::abs (getParam (*proc, fx::chorusRate) - 3.0f) < 0.01f, "drag landed");
        expect (proc->undo() && std::abs (getParam (*proc, fx::chorusRate) - 0.8f) < 0.01f, "undo restores the old value in one go");
        expect (! proc->canUndo() && proc->canRedo(), "after undo: redo available");
        expect (proc->redo() && std::abs (getParam (*proc, fx::chorusRate) - 3.0f) < 0.01f, "redo re-applies it");

        const int steps = proc->getUndoStepCount();
        gestureSet (*proc, fx::chorusRate, 3.0f);
        expect (proc->getUndoStepCount() == steps, "a click that lands on the same value records nothing");
        {
            auto* p = proc->getAPVTS().getParameter (fx::chorusRate);
            p->beginChangeGesture();
            p->setValueNotifyingHost (p->convertTo0to1 (1.0f));
            p->setValueNotifyingHost (p->convertTo0to1 (3.0f));
            p->endChangeGesture();
        }
        expect (proc->getUndoStepCount() == steps, "a drag that returns to where it started records nothing");
        gestureSet (*proc, fx::reverbMode, 2.0f);
        expect (proc->getUndoStepCount() == steps + 1 && proc->undo() && getParam (*proc, fx::reverbMode) != 2.0f, "a choice click is one step and undoes");

        // Host automation: plain writes, no gesture, interleaved with audio. Never recorded.
        proc->clearUndoHistory();
        juce::AudioBuffer<float> buf (2, 256); juce::MidiBuffer midi; buf.clear();
        for (int i = 0; i < 400; ++i)
        {
            proc->getAPVTS().getParameter (fx::chorusRate)->setValueNotifyingHost ((float) (i % 100) / 100.0f);
            proc->getAPVTS().getParameter (fx::reverbMix)->setValue ((float) (i % 50) / 50.0f);   // as a wrapper's setValueFromHost does
            if (i % 10 == 0) proc->processBlock (buf, midi);
        }
        expect (proc->getUndoStepCount() == 0 && ! proc->canUndo(), "400 host-automation writes (no gesture) record no undo step");
        // ...and a gesture opened by the UI in the middle of automation records ONE step for the drag only.
        gestureDrag (*proc, fx::delayMix, 0.1f, 0.6f, 10);
        expect (proc->getUndoStepCount() == 1, "a UI drag after automation is still exactly one step");

        // New edit after undo clears redo; lock / WILD are not history.
        proc->undo();
        expect (proc->canRedo(), "redo available after an undo");
        gestureSet (*proc, fx::delayMix, 0.2f);
        expect (! proc->canRedo(), "a new edit clears the redo stack");
        const int n = proc->getUndoStepCount();
        proc->setLocked (Module::eq, true);
        proc->setRandomWildness (0.9f);
        expect (proc->getUndoStepCount() == n, "changing locks / WILD records no step");
        proc->undo();
        expect (proc->isLocked (Module::eq) && std::abs (proc->getRandomWildness() - 0.9f) < 1.0e-6f, "...and undo never touches them");
    }

    void undoBoundTest()
    {
        std::cout << "undoBoundTest\n";
        auto proc = makeProc (48000.0, 256);
        proc->clearUndoHistory();
        for (int i = 0; i < 150; ++i)
            gestureSet (*proc, fx::chorusRate, 0.1f + 0.03f * (float) i);
        expect (Proc::kMaxUndoSteps == 100 && proc->getUndoStepCount() == 100, "bound: 150 edits keep exactly " + juce::String (Proc::kMaxUndoSteps) + " steps, got " + juce::String (proc->getUndoStepCount()));
        int undone = 0;
        while (proc->undo()) ++undone;
        expect (undone == 100 && proc->getRedoStepCount() == 100, "exactly 100 undos, all redoable");
        expect (std::abs (getParam (*proc, fx::chorusRate) - (0.1f + 0.03f * 49.0f)) < 0.01f, "the oldest 50 are gone: undo ends at the state before edit 51 (" + juce::String (getParam (*proc, fx::chorusRate), 3) + ")");

        // The shared-IR byte budget: distinct IR blobs kept alive by history are capped.
        TempDir t ("budget");
        auto q = makeIsolated (t);
        std::vector<juce::File> files;
        for (int i = 0; i < 6; ++i)
        {
            files.push_back (writeTestIR ("budget" + juce::String (i), 48000.0, 1.0 + 0.1 * i));
            q->loadConvolutionIR (files.back());
            gestureSet (*q, fx::chorusRate, 0.5f + (float) i);
        }
        const auto oneBlob = q->capturePatchSnapshot().irBlobBytes();
        q->getUndoStacksForTest().setMaxIRBytes ((size_t) ((double) oneBlob * 2.5));
        const auto retained = q->getUndoStacksForTest().retainedIRBytes();
        expect (retained <= (size_t) ((double) oneBlob * 2.5) && q->getUndoStepCount() >= 1 && q->getUndoStepCount() < 12,
                "IR budget: history keeps <= 2.5 blobs alive (" + juce::String ((int) (retained / 1024)) + " KB retained of " + juce::String ((int) (oneBlob / 1024)) + " KB per IR), " + juce::String (q->getUndoStepCount()) + " of 12 steps remain");
        expect (q->undo(), "...and the newest steps are still undoable");
        for (auto& f : files) f.deleteFile();
    }

    void undoContentTest()
    {
        std::cout << "undoContentTest\n";
        TempDir t ("undocontent");
        auto proc = makeIsolated (t);
        const auto user = writeTestIR ("undo-ir", 48000.0, 1.5);

        // IR source: factory -> embedded user file -> cleared, each one step.
        proc->loadFactoryIR ("chamber");
        proc->loadConvolutionIR (user);
        const auto embeddedBlob = proc->capturePatchSnapshot().ir;
        proc->clearConvolutionIR();
        expect (proc->getUndoStepCount() == 3, "factory load, user load and clear are three steps (" + juce::String (proc->getUndoStepCount()) + ")");
        expect (proc->getConvolutionIRSource() == "none" && ! proc->hasConvolutionIR(), "cleared");
        proc->undo();
        expect (proc->getConvolutionIRSource() == "embedded" && proc->hasConvolutionIR() && proc->getConvolutionIRName().startsWith ("undo-ir"), "undo -> the embedded user IR is back (" + proc->getConvolutionIRName() + ")");
        expect (proc->capturePatchSnapshot().ir == embeddedBlob, "...as the SAME shared blob, not a copy");
        proc->undo();
        expect (proc->getConvolutionIRSource() == "factory:chamber" && proc->getConvolutionIRName() == "Chamber" && proc->hasConvolutionIR(), "undo -> factory:chamber");
        proc->undo();
        expect (proc->getConvolutionIRSource() == "none" && ! proc->hasConvolutionIR(), "undo -> no IR");
        proc->redo(); proc->redo();
        expect (proc->getConvolutionIRSource() == "embedded" && proc->capturePatchSnapshot().ir == embeddedBlob, "redo x2 -> the embedded IR, same blob");
        proc->redo();
        expect (proc->getConvolutionIRSource() == "none", "redo x3 -> cleared again");

        // Slot targets.
        proc->clearUndoHistory();
        proc->setModSlotTarget (0, fx::chorusRate);
        proc->setModSlotTarget (3, fx::delayMix);
        proc->setModSlotTarget (3, fx::reverbMix);
        proc->setModSlotTarget (0, "");
        expect (proc->getUndoStepCount() == 4, "each slot-target change is a step");
        proc->setModSlotTarget (5, "no.such.parameter");
        proc->setModSlotTarget (2, "fxConv.decay");   // excluded from modulation
        expect (proc->getUndoStepCount() == 4, "a refused slot target records nothing");
        proc->undo();
        expect (proc->getModSlotTarget (0) == fx::chorusRate, "undo restores slot 1's target");
        proc->undo();
        expect (proc->getModSlotTarget (3) == fx::delayMix, "undo restores slot 4's earlier target");
        proc->undo(); proc->undo();
        expect (proc->getModSlotTarget (0).isEmpty() && proc->getModSlotTarget (3).isEmpty(), "undo to the start: all slots unassigned");
        proc->redo(); proc->redo(); proc->redo(); proc->redo();
        expect (proc->getModSlotTarget (0).isEmpty() && proc->getModSlotTarget (3) == fx::reverbMix, "redo all: final targets");

        // Chain order through the API.
        proc->clearUndoHistory();
        const auto order0 = proc->getFxOrder();
        auto swapped = order0; std::swap (swapped.getReference (0), swapped.getReference (4));
        proc->setFxOrder (swapped);
        expect (proc->getUndoStepCount() == 1 && proc->undo() && proc->getFxOrder() == order0 && proc->redo() && proc->getFxOrder() == swapped, "chain reorder is one undoable step");

        // History carries the IR by reference: 100 edits after one load hold the blob ONCE.
        proc->loadConvolutionIR (user);
        proc->clearUndoHistory();
        gestureSet (*proc, fx::chorusRate, 1.0f);
        for (int i = 0; i < 60; ++i)
            gestureSet (*proc, fx::chorusRate, 1.0f + 0.05f * (float) (i + 1));
        const auto blob = proc->capturePatchSnapshot().ir;
        int sharing = 0;
        for (const auto& e : proc->getUndoStacksForTest().undoEntries())
            if (e.snapshot.ir == blob) ++sharing;
        const auto oneBlob = blob->flac.getSize();
        const auto retained = proc->getUndoStacksForTest().retainedIRBytes();
        std::cout << "  ..   " << sharing << " history steps share one " << oneBlob / 1024 << " KB blob; retained " << retained / 1024 << " KB\n";
        expect (sharing == proc->getUndoStepCount() && retained == oneBlob, "61 steps reference the IR blob by pointer; the history retains exactly one copy (" + juce::String ((int) (retained / 1024)) + " KB)");
        size_t perStep = 0;
        for (const auto& e : proc->getUndoStacksForTest().undoEntries()) perStep = std::max (perStep, e.snapshot.approxBytes());
        expect (perStep < 16 * 1024, "a step's own footprint is " + juce::String ((int) perStep) + " bytes (no audio in it)");
        user.deleteFile();
    }

    void randomizeUndoTest()
    {
        std::cout << "randomizeUndoTest\n";
        TempDir t ("rndundo");
        auto proc = makeIsolated (t);
        dirtyPatch (*proc);
        proc->loadFactoryIR ("wine-tank");
        proc->setLocked (Module::delay, true);
        proc->clearUndoHistory();

        const auto before = proc->capturePatchSnapshot();
        const auto beforeValues = allValues (*proc);
        const auto orderBefore = proc->getFxOrder();
        juce::Random rng (77);
        proc->randomizeAll (rng);
        const auto after = proc->capturePatchSnapshot();
        const auto afterValues = allValues (*proc);
        const auto orderAfter = proc->getFxOrder();
        expect (proc->getUndoStepCount() == 1 && proc->getUndoLabel() == "RANDOMIZE ALL", "Randomize All is ONE undo step (hundreds of writes), label '" + proc->getUndoLabel() + "'");
        expect (after != before && orderAfter != orderBefore, "the roll changed the patch and the order");
        proc->undo();
        expect (nearValues (allValues (*proc), beforeValues) && proc->getFxOrder() == orderBefore, "undo restores every parameter (to within 1e-6 normalised: float rounding of the range conversion) and the chain order exactly");
        expect (proc->capturePatchSnapshot() == before && proc->getModSlotTarget (0) == fx::chorusRate && proc->getConvolutionIRSource() == "factory:wine-tank", "...and the slot targets and IR source");
        proc->redo();
        expect (nearValues (allValues (*proc), afterValues) && proc->getFxOrder() == orderAfter, "redo re-applies the roll exactly (same tolerance)");
        // Locks are not undone.
        proc->undo();
        expect (proc->isLocked (Module::delay), "the lock survives the undo");
    }

    //==========================================================================
    // Presets.
    // Registry-order comparison of the real values; oversampling excluded (session setting).
    bool identicalValuesExceptOversampling (Proc& a, Proc& b)
    {
        for (const auto& def : params::all())
        {
            if (def.id == pid::oversampling) continue;
            const float x = getParam (a, def.id), y = getParam (b, def.id);
            if (std::abs (x - y) > 1.0e-5f * std::max (1.0f, std::abs (x)))
            {
                std::cout << "      differs: " << def.id << " " << x << " vs " << y << "\n";
                return false;
            }
        }
        return true;
    }

    void presetRoundTripTest()
    {
        std::cout << "presetRoundTripTest\n";
        TempDir t ("roundtrip");
        const auto user = writeTestIR ("rt-ir", 44100.0, 1.0);
        for (int variant = 0; variant < 3; ++variant)   // 0: no IR, 1: factory IR, 2: embedded user IR
        {
            auto a = makeIsolated (t);
            dirtyPatch (*a);
            if (variant == 1) a->loadFactoryIR ("karlskirche");
            if (variant == 2) a->loadConvolutionIR (user);
            const auto saved = a->getPresetManager().save ("Round Trip " + juce::String (variant), "Bank A");
            expect (saved.ok && saved.file.existsAsFile() && saved.file.getParentDirectory().getFileName() == "Bank A" && saved.file.hasFileExtension (".spastrip"), "variant " + juce::String (variant) + ": saved to User/Bank A/*.spastrip");

            auto b = makeIsolated (t);
            const auto r = b->getPresetManager().load (saved.file);
            expect (r.ok, "variant " + juce::String (variant) + ": loads in a fresh processor");
            expect (identicalValuesExceptOversampling (*a, *b), "...every parameter equal (the real values, within the parameter's own precision)");
            expect (a->getFxOrder() == b->getFxOrder(), "...chain order equal");
            bool slots = true;
            for (int s = 0; s < 8; ++s) slots = slots && a->getModSlotTarget (s) == b->getModSlotTarget (s);
            expect (slots && b->getModSlotTarget (0) == fx::chorusRate && b->getModSlotTarget (4) == fx::delayMix, "...mod slot targets equal");
            expect (a->getConvolutionIRSource() == b->getConvolutionIRSource() && a->getConvolutionIRName() == b->getConvolutionIRName() && a->hasConvolutionIR() == b->hasConvolutionIR(),
                    "...IR source '" + b->getConvolutionIRSource() + "' / name equal, IR loaded: " + juce::String ((int) b->hasConvolutionIR()));
            if (variant == 2)
            {
                const auto ia = a->capturePatchSnapshot().ir, ib = b->capturePatchSnapshot().ir;
                expect (ia != nullptr && ib != nullptr && ia->flac == ib->flac && juce::exactlyEqual (ia->sampleRate, ib->sampleRate), "...the embedded user IR travelled inside the preset byte for byte (" + juce::String ((int) (ib->flac.getSize() / 1024)) + " KB FLAC): the source file is not needed");
            }
        }
        user.deleteFile();
    }

    void presetFileFormatTest()
    {
        std::cout << "presetFileFormatTest\n";
        TempDir t ("format");
        auto proc = makeIsolated (t);
        dirtyPatch (*proc);
        const auto saved = proc->getPresetManager().save ("Fmt", "");
        auto xml = juce::XmlDocument::parse (saved.file);
        expect (xml != nullptr && xml->hasTagName ("SPAStripPreset") && xml->getStringAttribute ("name") == "Fmt" && xml->getIntAttribute ("version") == 1, "root is <SPAStripPreset name=\"Fmt\" version=\"1\">");
        const auto* state = xml != nullptr ? xml->getFirstChildElement() : nullptr;
        int nParams = 0;
        if (state != nullptr)
            for (auto* c = state->getFirstChildElement(); c != nullptr; c = c->getNextElement())
                if (c->hasTagName ("PARAM")) ++nParams;
        // every registry parameter except oversampling
        expect (state != nullptr && nParams == (int) params::all().size() - 1, "wraps the state tree: one PARAM per registry parameter but oversampling (" + juce::String (nParams) + ")");
        expect (state != nullptr && state->hasAttribute ("fxOrder") && state->hasAttribute ("modSlot1Target") && state->getStringAttribute ("irSource") == "none", "carries fxOrder, the mod slot targets and the IR source");
        expect (saved.file.getParentDirectory() == t.dir.getChildFile ("User"), "saved under <root>/User");
        expect (! t.dir.getChildFile ("User").getChildFile ("Fmt.spastrip.tmp").exists(), "no temp file left behind");
        {
            const auto current = PM::defaultPresetsRoot();      // the test override
            PM::setPresetsRootOverride (juce::File());
            const auto real = PM::defaultPresetsRoot();
            PM::setPresetsRootOverride (current);
            const auto sep = juce::File::getSeparatorString();
            expect (real.isAChildOf (juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory))
                        && real.getFullPathName().endsWith ("Silverplatter Audio" + sep + "SPAStrip" + sep + "Presets"),
                    "the real default root is <userApplicationDataDirectory>/Silverplatter Audio/SPAStrip/Presets (" + real.getFullPathName() + ")");
        }
    }

    void presetManagementTest()
    {
        std::cout << "presetManagementTest\n";
        TempDir t ("manage");
        auto proc = makeIsolated (t);
        auto& pm = proc->getPresetManager();

        // --- sanitising.
        expect (PM::sanitiseFileName ("CON") == "_CON" && PM::sanitiseFileName ("nul.txt") == "_nul.txt" && PM::sanitiseFileName ("com1") == "_com1" && PM::sanitiseFileName ("LPT9") == "_LPT9",
                "Windows device names are prefixed (CON, nul.txt, com1, LPT9)");
        expect (PM::sanitiseFileName ("COM10") == "COM10" && PM::sanitiseFileName ("console") == "console", "...but COM10 / 'console' are fine");
        expect (PM::sanitiseFileName ("..").isEmpty() && PM::sanitiseFileName ("   ").isEmpty() && PM::sanitiseFileName ("").isEmpty() && PM::sanitiseFileName ("///").isEmpty(), "dots / blanks / separators only -> empty (refused)");
        expect (PM::sanitiseFileName ("a.") == "a" && PM::sanitiseFileName ("  x  ") == "x" && PM::sanitiseFileName (".hidden") == "hidden", "trailing dot / spaces and leading dots stripped");
        bool noBad = true;
        for (const auto& s : { "a/b", "a\\b", "a:b", "a*b?", "a<b>|c", "\"q\"", "../../etc/passwd", "x\ty" })
        {
            const auto n = PM::sanitiseFileName (s);
            noBad = noBad && n.isNotEmpty() && ! n.containsAnyOf ("/\\:*?<>|\"\t") && ! n.startsWithChar ('.');
        }
        expect (noBad, "path separators, wildcards, quotes and control characters never survive");
        const auto longName = PM::sanitiseFileName (juce::String::repeatedString (juce::String::fromUTF8 ("\xc3\xa9"), 300));
        expect (longName.isNotEmpty() && longName.getNumBytesAsUTF8() <= 200, "a long non-ASCII name is capped in bytes (" + juce::String ((int) longName.getNumBytesAsUTF8()) + ")");
        expect (PM::sanitiseFileName (juce::String::fromUTF8 ("Pr\xc3\xa9set \xe2\x9c\xa8")) == juce::String::fromUTF8 ("Pr\xc3\xa9set \xe2\x9c\xa8"), "ordinary Unicode names are kept");

        // Traversal attempts stay inside User/.
        const auto evil = pm.save ("../../escape", "../evilbank");
        expect (evil.ok && evil.file.isAChildOf (t.dir.getChildFile ("User")) && evil.file.getParentDirectory().getParentDirectory() == t.dir.getChildFile ("User"), "'../../escape' into bank '../evilbank' lands inside User/ (" + evil.file.getRelativePathFrom (t.dir) + ")");
        expect (! pm.save ("..", "").ok && ! pm.save ("", "").ok && ! pm.save ("ok", "..").ok, "empty / dot-only names and banks are refused");
        pm.deletePreset (evil.file);
        evil.file.getParentDirectory().deleteRecursively();

        // --- save, clash, overwrite.
        setParam (*proc, fx::distDrive, 0.2f);
        const auto first = pm.save ("Alpha", "");
        expect (first.ok && ! first.clash && ! first.overwritten, "first save: ok, not a clash, not an overwrite");
        const auto text1 = first.file.loadFileAsString();
        setParam (*proc, fx::distDrive, 0.9f);
        const auto clash = pm.save ("Alpha", "");
        expect (! clash.ok && clash.clash && ! clash.overwritten && first.file.loadFileAsString() == text1, "saving over an existing name is refused and reported as a clash; the file is untouched");
        const auto clash2 = pm.save ("Alpha?", "");   // sanitises to the same file
        expect (! clash2.ok && clash2.clash, "a different spelling that sanitises to the same file is a clash too");
        const auto over = pm.save ("Alpha", "", true);
        expect (over.ok && over.overwritten && over.file == first.file && first.file.loadFileAsString() != text1, "replace=true overwrites and says so");
        expect (std::abs (getParam (*proc, fx::distDrive) - 0.9f) < 1.0e-6f, "(saving does not touch the sound)");

        // --- banks and folders: the User root first, then folders (SPASynth's layout: two levels
        // can be created, deeper ones on disk are still listed); other files are ignored.
        pm.save ("Beta", "Drums");
        pm.save ("Gamma", "Drums");
        pm.save ("Delta", "Bass");
        const auto deepDir = t.dir.getChildFile ("User/Drums/Deep");
        deepDir.createDirectory();
        first.file.copyFileTo (deepDir.getChildFile ("Nested.spastrip"));
        t.dir.getChildFile ("User/Drums/notes.txt").replaceWithText ("x");
        pm.rescan();
        juce::StringArray names;
        for (const auto& p : pm.getPresets()) names.add (p.bank + "/" + p.name);
        expect (names.joinIntoString (",") == "/Alpha,Bass/Delta,Drums/Beta,Drums/Gamma,Drums/Nested", "list: User root first, then folders (a nested folder counts under its top-level bank), sorted; other files ignored (" + names.joinIntoString (",") + ")");
        expect (pm.getBanks() == juce::StringArray ({ "Bass", "Drums" }), "getBanks lists the top-level folders only (" + pm.getBanks().joinIntoString (",") + ")");
        expect (pm.getUserFolders() == juce::StringArray ({ "Bass", "Drums", "Drums/Deep" }), "getUserFolders lists every folder, nested ones as paths (" + pm.getUserFolders().joinIntoString (",") + ")");
        deepDir.deleteRecursively();
        pm.rescan();

        // --- load by index, next / previous.
        setParam (*proc, fx::distDrive, 0.1f);
        pm.save ("Zeta", "Drums", false);
        std::map<juce::String, float> driveOf { { "Alpha", 0.9f }, { "Delta", 0.1f }, { "Beta", 0.1f }, { "Gamma", 0.1f }, { "Zeta", 0.1f } };
        juce::ignoreUnused (driveOf);
        pm.rescan();
        const int n = (int) pm.getPresets().size();
        pm.loadPreset (0);
        expect (pm.getCurrentIndex() == 0 && pm.getCurrentName() == pm.getPresets()[0].name, "loadPreset(0) sets the current preset");
        juce::StringArray visited;
        for (int i = 0; i < n; ++i) { pm.loadNext(); visited.add (pm.getCurrentName()); }
        expect (pm.getCurrentIndex() == 0 && visited[n - 1] == pm.getPresets()[0].name, "loadNext cycles the whole list and wraps to the start");
        pm.loadPrevious();
        expect (pm.getCurrentIndex() == n - 1, "loadPrevious wraps to the end");
        pm.loadPrevious();
        expect (pm.getCurrentIndex() == n - 2, "...and steps back");

        // --- rename.
        const auto beta = [&] { for (const auto& p : pm.getPresets()) if (p.name == "Beta") return p.file; return juce::File(); }();
        pm.load (beta);
        const auto renamed = pm.rename (beta, "Beta Two");
        expect (renamed.ok && renamed.newFile.getFileName() == "Beta Two.spastrip" && renamed.newFile.existsAsFile() && ! beta.exists(), "rename moves the file");
        expect (pm.getCurrentName() == "Beta Two" && pm.getCurrentFile() == renamed.newFile, "...and the loaded preset follows the rename");
        auto renamedXml = juce::XmlDocument::parse (renamed.newFile);
        expect (renamedXml != nullptr && renamedXml->getStringAttribute ("name") == "Beta Two", "...the name attribute inside is rewritten");
        expect (! pm.rename (renamed.newFile, "Gamma").ok && renamed.newFile.existsAsFile(), "rename onto an existing preset is refused (nothing lost)");
        expect (! pm.rename (renamed.newFile, "").ok && ! pm.rename (renamed.newFile, "...").ok, "empty / illegal names are refused");
        expect (! pm.rename (t.dir.getChildFile ("nonexistent.spastrip"), "X").ok, "an unscanned file cannot be renamed");
        const auto caseOnly = pm.rename (renamed.newFile, "beta two");
        expect (caseOnly.ok && caseOnly.newFile.getFileName() == "beta two.spastrip" && caseOnly.newFile.existsAsFile(), "a case-only rename works");
        expect (pm.load (caseOnly.newFile).ok, "the renamed preset still loads");

        // --- delete.
        const auto deltaFile = [&] { for (const auto& p : pm.getPresets()) if (p.name == "Delta") return p.file; return juce::File(); }();
        pm.load (deltaFile);
        const auto soundBefore = allValues (*proc);
        juce::String err;
        expect (pm.deletePreset (deltaFile, &err) && ! deltaFile.exists(), "delete removes the file");
        expect (identicalValues (allValues (*proc), soundBefore), "...the loaded SOUND is left as it is");
        expect (pm.getCurrentFile() == juce::File() && pm.getCurrentIndex() == -1, "...and the identity no longer points at the deleted file");
        bool stillListed = false;
        for (const auto& p : pm.getPresets()) stillListed = stillListed || p.name == "Delta";
        expect (! stillListed, "...which drops out of the list");
        expect (! pm.deletePreset (t.dir.getChildFile ("User/nothere.spastrip"), &err) && err.isNotEmpty(), "an unscanned file is not deleted (" + err + ")");

        // --- corrupt files.
        const auto bad1 = t.dir.getChildFile ("User/bad1.spastrip"); bad1.replaceWithText ("this is not xml");
        const auto bad2 = t.dir.getChildFile ("User/bad2.spastrip"); bad2.replaceWithText ("<Something/>");
        const auto bad3 = t.dir.getChildFile ("User/bad3.spastrip"); bad3.replaceWithText ("<SPAStripPreset name=\"x\" version=\"1\"/>");
        const auto bad4 = t.dir.getChildFile ("User/bad4.spastrip"); bad4.replaceWithText ("<SPAStripPreset name=\"x\" version=\"1\"><NotParams/></SPAStripPreset>");
        proc->clearUndoHistory();
        const auto soundNow = allValues (*proc);
        const auto nameNow = pm.getCurrentName();
        bool allRefused = true;
        for (const auto& f : { bad1, bad2, bad3, bad4, t.dir.getChildFile ("nope.spastrip") })
        {
            const auto r = pm.load (f);
            allRefused = allRefused && ! r.ok && r.error.isNotEmpty();
        }
        expect (allRefused && identicalValues (allValues (*proc), soundNow) && pm.getCurrentName() == nameNow && ! proc->canUndo(), "corrupt / foreign / missing preset files are refused with a message and change nothing (no undo step)");
        bad1.deleteFile(); bad2.deleteFile(); bad3.deleteFile(); bad4.deleteFile();
        pm.rescan();

        // --- navigation steps over a broken file instead of sticking.
        const auto badMid = t.dir.getChildFile ("User/Alpha2.spastrip"); badMid.replaceWithText ("garbage");
        pm.rescan();
        pm.load (t.dir.getChildFile ("User/Alpha.spastrip"));
        const auto nx = pm.loadNext();
        expect (nx.ok && pm.getCurrentName() != "Alpha2", "loadNext skips an unreadable preset (now on '" + pm.getCurrentName() + "')");
        badMid.deleteFile();
        pm.rescan();
    }

    void presetFactorySeamTest()
    {
        std::cout << "presetFactorySeamTest\n";
        TempDir t ("factory");
        auto proc = makeIsolated (t);
        auto& pm = proc->getPresetManager();
        expect (pm.getPresets().empty(), "no presets ship in this phase");

        setParam (*proc, fx::reverbMix, 0.77f);
        setParam (*proc, pid::oversampling, 1.0f);
        const auto saved = pm.save ("Source", "");
        const auto doc = saved.file.loadFileAsString();
        pm.deletePreset (saved.file);
        setParam (*proc, fx::reverbMix, 0.1f);

        pm.setFactoryPresets ({ { "Embedded One", "Factory", doc } });
        expect (pm.getPresets().size() == 1 && pm.getPresets()[0].isFactory && pm.getPresets()[0].file == juce::File(), "an embedded preset appears in the list, read-only");
        expect (pm.loadPreset (0).ok && std::abs (getParam (*proc, fx::reverbMix) - 0.77f) < 1.0e-5f && pm.getCurrentName() == "Embedded One" && pm.getCurrentIndex() == 0, "it loads from its embedded text");
        expect (! pm.isEdited(), "...clean");
        expect (! pm.rename (juce::File(), "x").ok && ! pm.deletePreset (juce::File()), "factory presets cannot be renamed or deleted");
        setParam (*proc, fx::reverbMix, 0.3f);
        const auto toUser = pm.save ("Embedded One", "");
        expect (toUser.ok && pm.getPresets().size() == 2 && pm.getPresets()[0].isFactory, "saving the same name goes to the User area, never over the factory one");
        pm.setFactoryPresets ({});
        expect (pm.getPresets().size() == 1 && ! pm.getPresets()[0].isFactory, "removing the factory set leaves the user list");
    }

    void presetDirtyFlagTest()
    {
        std::cout << "presetDirtyFlagTest\n";
        TempDir t ("dirty");
        auto proc = makeIsolated (t);
        auto& pm = proc->getPresetManager();
        struct Count : juce::ChangeListener { int n = 0; void changeListenerCallback (juce::ChangeBroadcaster*) override { ++n; } } count;
        pm.addChangeListener (&count);

        expect (pm.getCurrentName() == "Init" && ! pm.isEdited(), "a fresh processor is 'Init', not edited");
        pump (50);
        count.n = 0;

        setParam (*proc, fx::distDrive, 0.77f);
        expect (pm.isEdited(), "a parameter change marks it edited");
        proc->serviceMessageThread();
        pump (50);
        expect (count.n >= 1, "...and the change broadcast fires on the transition (" + juce::String (count.n) + ")");
        count.n = 0;
        setParam (*proc, fx::distDrive, 0.78f);
        proc->serviceMessageThread();
        pump (50);
        expect (count.n == 0, "...but not again while it stays edited");

        const auto saved = pm.save ("Dirty Base", "");
        expect (! pm.isEdited() && pm.getCurrentName() == "Dirty Base", "saving makes the saved patch the unedited reference");
        setParam (*proc, fx::distDrive, 0.3f);
        expect (pm.isEdited(), "edited again");
        pm.load (saved.file);
        expect (! pm.isEdited(), "loading a preset lands clean");

        // Undo back to the loaded state clears it; redo brings it back.
        gestureSet (*proc, fx::delayMix, 0.66f);
        expect (pm.isEdited(), "a gesture edit marks it edited");
        proc->undo();
        expect (! pm.isEdited(), "undoing back to the loaded patch clears the flag");
        proc->redo();
        expect (pm.isEdited(), "redo sets it again");
        proc->undo();

        // Things that are not part of a preset do not count.
        setParam (*proc, pid::oversampling, 2.0f);
        proc->setLocked (Module::eq, true);
        proc->setRandomWildness (0.9f);
        proc->getAPVTS().state.setProperty ("uiScale", 2.0, nullptr);
        expect (! pm.isEdited(), "oversampling, locks, WILD and UI properties never mark a preset edited");

        // Things that are do.
        proc->setModSlotTarget (1, fx::delayTime);
        expect (pm.isEdited(), "a mod slot target change marks it edited");
        proc->undo();
        expect (! pm.isEdited(), "...and undoes clean");
        proc->loadFactoryIR ("chamber");
        expect (pm.isEdited(), "an IR change marks it edited");
        proc->undo();
        auto o = proc->getFxOrder(); std::swap (o.getReference (0), o.getReference (1));
        proc->setFxOrder (o);
        expect (pm.isEdited(), "a chain reorder marks it edited");
        proc->undo();
        proc->randomizeAll();
        expect (pm.isEdited(), "Randomize All marks it edited");
        proc->undo();
        expect (! pm.isEdited(), "...and undoing it lands clean again");

        // The 150 ms timer path: automation without any gesture.
        proc->getAPVTS().getParameter (fx::chorusMix)->setValueNotifyingHost (0.9f);
        count.n = 0;
        proc->serviceMessageThread();
        pump (50);
        expect (pm.isEditedCached() && count.n >= 1, "automation is noticed by the timer and broadcast");

        pm.removeChangeListener (&count);
    }

    void presetInitTest()
    {
        std::cout << "presetInitTest\n";
        TempDir t ("init");
        auto proc = makeIsolated (t);
        auto& pm = proc->getPresetManager();
        const auto pristine = allValues (*proc);
        const auto pristineOrder = proc->getFxOrder();

        dirtyPatch (*proc);
        proc->loadFactoryIR ("chamber");
        setParam (*proc, pid::oversampling, 2.0f);
        proc->setLocked (Module::reverb, true);
        proc->setRandomWildness (0.31f);
        pm.save ("Something", "");
        proc->clearUndoHistory();
        const auto beforeInit = proc->capturePatchSnapshot();

        pm.init();
        expect (pm.getCurrentName() == "Init" && ! pm.isEdited(), "Init: name 'Init', clean");
        bool defaults = true;
        for (const auto& def : params::all())
            if (def.id != pid::oversampling)
                defaults = defaults && std::abs (proc->getAPVTS().getParameter (def.id)->getValue() - proc->getAPVTS().getParameter (def.id)->getDefaultValue()) < 1.0e-6f;
        expect (defaults, "every parameter (but oversampling) is at its registry default");
        expect (proc->getFxOrder() == juce::Array<int> ([] { juce::Array<int> a; spa::dsp::FXChain::Module m[spa::dsp::FXChain::numModules]; spa::dsp::FXChain::unpackOrder (spa::dsp::FXChain::defaultOrderPacked(), m); for (auto x : m) a.add ((int) x); return a; }()) && proc->getFxOrder() == pristineOrder, "chain order is the default order");
        bool noSlots = true;
        for (int s = 0; s < 8; ++s) noSlots = noSlots && proc->getModSlotTarget (s).isEmpty();
        expect (noSlots && proc->getConvolutionIRSource() == "none" && ! proc->hasConvolutionIR(), "no mod slots and no IR");
        expect (getParam (*proc, pid::oversampling) == 2.0f && proc->isLocked (Module::reverb) && std::abs (proc->getRandomWildness() - 0.31f) < 1.0e-6f, "oversampling (4x), locks and WILD are kept");
        expect (proc->getUndoStepCount() == 1 && proc->getUndoLabel() == "INIT", "Init is one undo step labelled INIT");
        proc->undo();
        expect (proc->capturePatchSnapshot() == beforeInit && pm.getCurrentName() == "Something" && ! pm.isEdited(), "undo brings the whole patch AND the preset identity back, still clean");
        proc->redo();
        expect (pm.getCurrentName() == "Init" && ! pm.isEdited() && sameBits (getParam (*proc, fx::distDrive), 0.3f), "redo re-applies Init");
        juce::ignoreUnused (pristine);
    }

    void presetLoadUndoAndResetTest()
    {
        std::cout << "presetLoadUndoAndResetTest\n";
        TempDir t ("loadundo");
        auto proc = makeIsolated (t);
        auto& pm = proc->getPresetManager();
        setParam (*proc, fx::chorusRate, 1.5f);
        const auto a = pm.save ("PA", "");
        setParam (*proc, fx::chorusRate, 3.5f);
        proc->setModSlotTarget (2, fx::reverbMix);
        const auto b = pm.save ("PB", "");
        proc->clearUndoHistory();

        const auto beforeLoad = proc->capturePatchSnapshot();
        expect (pm.load (a.file).ok, "load PA");
        expect (proc->getUndoStepCount() == 1 && proc->getUndoLabel() == "LOAD PRESET", "a preset load is ONE undo step (label '" + proc->getUndoLabel() + "')");
        expect (std::abs (getParam (*proc, fx::chorusRate) - 1.5f) < 1.0e-5f && proc->getModSlotTarget (2).isEmpty(), "PA applied (slot target dropped: PA had none)");
        proc->undo();
        expect (proc->capturePatchSnapshot() == beforeLoad && pm.getCurrentName() == "PB" && ! pm.isEdited(), "undo returns to the previous patch and the previous preset's name / clean state");
        proc->redo();
        expect (pm.getCurrentName() == "PA" && ! pm.isEdited() && std::abs (getParam (*proc, fx::chorusRate) - 1.5f) < 1.0e-5f, "redo reloads it, named and clean");

        // Loading the very same preset twice is still a step (identity), and an identical sound still records.
        proc->clearUndoHistory();
        pm.load (a.file);
        expect (proc->getUndoStepCount() == 1, "reloading the same preset is recorded (its identity is part of the step)");

        // FX state: a preset load resets the chain; a host session restore does not.
        constexpr int block = 256;
        const auto tailAfter = [&] (bool viaPreset)
        {
            TempDir t2 ("tail");
            auto p = makeIsolated (t2, 48000.0, block);
            setAllEffects (*p, false);
            setParam (*p, fx::reverbEnable, 1.0f);
            setParam (*p, fx::reverbMix, 1.0f);
            setParam (*p, fx::reverbDecay, 6.0f);
            const auto saved = p->getPresetManager().save ("Tail", "");
            juce::MemoryBlock state;
            p->getStateInformation (state);
            juce::AudioBuffer<float> buf (2, block); juce::MidiBuffer midi; Noise nz (3);
            for (int i = 0; i < 60; ++i) { for (int c = 0; c < 2; ++c) for (int k = 0; k < block; ++k) buf.setSample (c, k, 0.4f * nz.next()); p->processBlock (buf, midi); }
            if (viaPreset) p->getPresetManager().load (saved.file); else p->setStateInformation (state.getData(), (int) state.getSize());
            buf.clear();
            p->processBlock (buf, midi);
            return buf.getMagnitude (0, block);
        };
        const float reset = tailAfter (true), kept = tailAfter (false);
        expect (reset < 1.0e-6f && kept > 1.0e-4f, "preset load resets the FX chain (tail " + juce::String (reset, 8) + "), a host state restore leaves it (tail " + juce::String (kept, 5) + ")");
    }

    void hostStatePresetNameTest()
    {
        std::cout << "hostStatePresetNameTest\n";
        TempDir t ("hostname");
        auto a = makeIsolated (t);
        auto& pm = a->getPresetManager();
        dirtyPatch (*a);
        pm.save ("Session Preset", "");
        juce::MemoryBlock clean;
        a->getStateInformation (clean);
        setParam (*a, fx::delayMix, 0.123f);
        juce::MemoryBlock dirty;
        a->getStateInformation (dirty);
        gestureSet (*a, fx::delayMix, 0.2f);
        expect (a->canUndo(), "(an edit is undoable before the restore)");

        auto b = makeIsolated (t);
        b->setStateInformation (clean.getData(), (int) clean.getSize());
        expect (b->getPresetManager().getCurrentName() == "Session Preset" && ! b->getPresetManager().isEdited(), "a restored session shows its preset name, unedited");
        auto c = makeIsolated (t);
        c->setStateInformation (dirty.getData(), (int) dirty.getSize());
        expect (c->getPresetManager().getCurrentName() == "Session Preset" && c->getPresetManager().isEdited(), "...and edited when it was saved edited");
        a->setStateInformation (clean.getData(), (int) clean.getSize());
        expect (! a->canUndo() && ! a->canRedo(), "a host session restore starts with an empty undo history");
        auto xml = juce::AudioProcessor::getXmlFromBinary (clean.getData(), (int) clean.getSize());
        expect (xml != nullptr && ! xml->toString().containsIgnoreCase ("undo"), "no undo data in the saved state");
    }

    //==========================================================================
    // Audio-thread allocation with message-thread edits running concurrently.
   #if JUCE_MAC
    // A heap allocation the optimiser cannot elide (the pointer escapes to a global).
    std::atomic<char*> g_sink { nullptr };
    __attribute__ ((noinline)) void touchHeap (int n)
    {
        char* p = (char*) std::malloc ((size_t) n);
        g_sink.store (p);
        p[0] = 1;
        g_sink.store (nullptr);
        std::free (p);
    }
    std::atomic<uintptr_t> g_audioThread { 0 };
    std::atomic<long> g_audioAllocs { 0 };
    std::atomic<bool> g_countAudio { false };
    void audioThreadMallocLogger (uint32_t type, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uint32_t)
    {
        if ((type & 2u) != 0 && g_countAudio.load (std::memory_order_relaxed)
            && (uintptr_t) pthread_self() == g_audioThread.load (std::memory_order_relaxed))
            g_audioAllocs.fetch_add (1, std::memory_order_relaxed);
    }
   #endif

    void concurrentEditsAllocationTest()
    {
        std::cout << "concurrentEditsAllocationTest\n";
        TempDir t ("concurrent");
       #if JUCE_MAC
        {
            // Anti-vacuous: the per-thread hook really sees an allocation made on the counted thread,
            // and ignores the same allocation made on another one.
            malloc_logger = audioThreadMallocLogger;
            g_audioThread.store ((uintptr_t) pthread_self());
            std::atomic<bool> go { false };
            std::thread other ([&go] { while (! go.load()) std::this_thread::yield(); touchHeap (4096); });
            g_audioAllocs = 0;
            g_countAudio = true;
            go = true;
            other.join();
            const long otherThread = g_audioAllocs.load();
            touchHeap (4096);
            const long ownThread = g_audioAllocs.load();
            g_countAudio = false;
            malloc_logger = nullptr;
            expect (otherThread == 0 && ownThread >= 1, "(hook self-check: an allocation on the counted thread is seen (" + juce::String ((juce::int64) ownThread) + "), one on another thread is not (" + juce::String ((juce::int64) otherThread) + "))");
        }
       #endif
        for (int factorChoice : { 0, 2 })
        {
            auto proc = makeProc (48000.0, 256, 2, 2, 2);
            proc->getPresetManager().setPresetsRoot (t.dir);
            proc->getPresetManager().setUseTrash (false);
            setAllEffects (*proc, true);
            setParam (*proc, pid::oversampling, (float) factorChoice);
            setParam (*proc, pid::mix, 0.7f);
            setParam (*proc, fx::limLookahead, 1.0f);
            setParam (*proc, pid::sc::attack, 0.5f);
            proc->loadFactoryIR ("living-room");
            for (int s = 0; s < 8; ++s)
            {
                proc->setModSlotTarget (s, Proc::getModTargets()[(size_t) ((s * 13 + 5) % (int) Proc::getModTargets().size())].id);
                setParam (*proc, pid::modSlotDepth (s), s % 2 == 0 ? 0.6f : -0.4f);
            }
            const auto a = proc->getPresetManager().save ("Conc A", "");
            setParam (*proc, fx::reverbMix, 0.9f);
            proc->loadFactoryIR ("wine-tank");
            const auto b = proc->getPresetManager().save ("Conc B", "");
            const auto ir = writeTestIR ("conc-ir", 48000.0, 1.0);
            proc->loadConvolutionIR (ir);
            const auto c = proc->getPresetManager().save ("Conc C", "");   // embedded user IR
            proc->clearUndoHistory();

            // Warm up on this (the "audio") thread before counting.
            juce::AudioBuffer<float> buf (4, 256); juce::MidiBuffer midi; Noise nz (6);
            const auto fill = [&] { for (int ch = 0; ch < 4; ++ch) for (int i = 0; i < 256; ++i) buf.setSample (ch, i, 0.3f * nz.next()); };
            fill(); proc->processBlock (buf, midi); proc->serviceMessageThread();
            for (int i = 0; i < 30; ++i) { fill(); proc->processBlock (buf, midi); }

            std::atomic<bool> stop { false };
            std::atomic<long> blocks { 0 };
            std::atomic<bool> sawNaN { false };
            std::thread audio ([&]
            {
                juce::AudioBuffer<float> ab (4, 256); juce::MidiBuffer am; Noise an (11);
               #if JUCE_MAC
                g_audioThread.store ((uintptr_t) pthread_self());
               #endif
                while (! stop.load())
                {
                    for (int ch = 0; ch < 4; ++ch) for (int i = 0; i < 256; ++i) ab.setSample (ch, i, 0.3f * an.next());
                    {
                        // What a plugin wrapper does: processBlock under the callback lock.
                       #if JUCE_MAC
                        g_countAudio = true;
                       #endif
                        const juce::ScopedLock sl (proc->getCallbackLock());
                        proc->processBlock (ab, am);
                       #if JUCE_MAC
                        g_countAudio = false;
                       #endif
                    }
                    for (int ch = 0; ch < 2; ++ch)
                        for (int i = 0; i < 256; ++i)
                            if (! std::isfinite (ab.getSample (ch, i))) sawNaN = true;
                    ++blocks;
                    std::this_thread::sleep_for (std::chrono::microseconds (500));
                }
            });

           #if JUCE_MAC
            malloc_logger = audioThreadMallocLogger;
            g_audioAllocs = 0;
           #endif
            // Message thread: everything an editor / host can do to the state.
            int edits = 0;
            const auto start = juce::Time::getMillisecondCounter();
            juce::Random rng (123);
            while (juce::Time::getMillisecondCounter() - start < 2500)
            {
                switch (edits % 9)
                {
                    case 0: proc->randomizeAll (rng); break;
                    case 1: proc->undo(); break;
                    case 2: proc->redo(); break;
                    case 3: proc->getPresetManager().load (a.file); break;
                    case 4: proc->getPresetManager().load (c.file); break;   // embedded IR
                    case 5: proc->getPresetManager().init(); break;
                    case 6: proc->getPresetManager().load (b.file); proc->undo(); break;
                    case 7: gestureDrag (*proc, fx::reverbMix, 0.1f, 0.8f, 5); proc->setModSlotTarget (edits % 8, fx::delayMix); break;
                    case 8: proc->setLocked ((Module) (edits % kNumModules), (edits / 9) % 2 == 0); proc->randomizeAll (rng); break;
                }
                proc->serviceMessageThread();
                ++edits;
                std::this_thread::sleep_for (std::chrono::milliseconds (5));
            }
           #if JUCE_MAC
            malloc_logger = nullptr;
            const long allocs = g_audioAllocs.load();
           #else
            const long allocs = 0;
           #endif
            stop = true;
            audio.join();
            expect (blocks.load() > 50 && edits > 50, juce::String (1 << factorChoice) + "x: " + juce::String (edits) + " randomize / undo / redo / preset-load / init edits ran against " + juce::String ((long) blocks.load()) + " concurrent audio blocks");
            expect (allocs == 0, juce::String (1 << factorChoice) + "x: the audio thread performed no heap allocation while they ran (" + juce::String ((juce::int64) allocs) + " allocations seen on it)");
            expect (! sawNaN.load(), juce::String (1 << factorChoice) + "x: no NaN / Inf reached the output");
            ir.deleteFile();
        }
       #if ! JUCE_MAC
        std::cout << "  (allocation counting skipped: the hook is macOS-only; the NaN / concurrency part ran)\n";
       #endif
    }
}
