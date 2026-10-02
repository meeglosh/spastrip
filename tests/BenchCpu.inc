// `SPAStripTests --bench-cpu [key=v1,v2 ...]`: non-asserting real-time CPU bench.
// Included from SPAStripTests.cpp (inside the anonymous namespace), so `Proc`,
// `makeProc`, `setParam`, `setAllEffects`, `Noise`, `pid` are in scope.
//
// Measures per-processBlock wall time against the block's real-time budget
// (blockSize / sampleRate): mean, p99, max (all as % of budget) and the count of
// blocks over budget. Keys (comma lists, defaults in brackets):
//   sr=44100,48000 [48000]   block=32,..,1024 [128,512]   os=1,2,4 [1,2,4]
//   ir=none|<factory id> [sixties-spring]   fx=conv|mix|owner|all [conv]
//   sc=off|silent|floor|live [silent]   slot=off|zero|ramp|held [off,zero,ramp,held]
//   pace=0|1 (1 = one block per budget period, sleeping in between, like a real host) [0]
//   secs=<audio seconds per config> [3]   dist=0|1 (distortion on) [0]
// slot: off = no target; zero = fxDist.tone assigned, depth 0; ramp = depth
// 0 -> 0.5 over the run, changed before every block (a knob drag); held = 0.5.
namespace benchCpu
{
    struct Stats
    {
        double meanPct = 0, p99Pct = 0, maxPct = 0;
        int over = 0, blocks = 0;
        double chainPerBlock = 0;
        double modChunkFrac = 0;
    };

    inline std::vector<juce::String> splitList (const juce::String& v)
    {
        std::vector<juce::String> out;
        for (auto& t : juce::StringArray::fromTokens (v, ",", ""))
            out.push_back (t.trim());
        return out;
    }

    inline Stats run (double sr, int block, int osChoice, const juce::String& irId, const juce::String& fxSet,
                      const juce::String& scMode, const juce::String& slotMode, double secs, bool distOn,
                      bool withServiceThread = false, bool silentInput = false, bool paced = false)
    {
        auto proc = makeProc (sr, block, 2, 2, scMode == "off" ? 0 : 2);
        setAllEffects (*proc, false);
        if (fxSet == "all")
            setAllEffects (*proc, true);
        else if (fxSet == "owner")   // the field report: EQ, TREM/VIB, COMP, LIMIT (+ CONV below)
            for (auto* id : { pid::fx::eqEnable, pid::fx::tremEnable, pid::fx::vibEnable, pid::fx::compEnable, pid::fx::limEnable })
                setParam (*proc, id, 1.0f);
        else if (fxSet == "mix")
            for (auto* id : { pid::fx::modEnable, pid::fx::eqEnable, pid::fx::delayEnable })
                setParam (*proc, id, 1.0f);
        if (distOn) setParam (*proc, pid::fx::distEnable, 1.0f);
        setParam (*proc, pid::oversampling, (float) osChoice);
        setParam (*proc, pid::mix, 1.0f);

        const bool conv = irId != "none";
        if (conv)
        {
            setParam (*proc, pid::fx::convEnable, 1.0f);
            setParam (*proc, pid::fx::convMix, 1.0f);
            if (! proc->loadFactoryIR (irId)) { std::cout << "  unknown IR " << irId << "\n"; return {}; }
        }

        juce::AudioBuffer<float> buf (4, block);
        juce::MidiBuffer midi;
        Noise nz (3);
        const bool scLive = scMode == "live";
        const bool scFloor = scMode == "floor";   // enabled bus carrying only a -60 dBFS noise floor
        bool measuring = false;
        const auto fill = [&]
        {
            for (int c = 0; c < 4; ++c)
            {
                const bool isSc = c >= 2;
                for (int i = 0; i < block; ++i)
                    buf.setSample (c, i, isSc ? (scLive ? 0.5f * nz.next() : scFloor ? 1.0e-3f * nz.next() : 0.0f) : (silentInput && measuring ? 0.0f : 0.25f * nz.next()));
            }
        };

        // processBlock publishes the wanted oversampling factor; the message-thread
        // service then rebuilds the chain (and reshapes/reloads the IR) at it.
        fill(); proc->processBlock (buf, midi);
        proc->serviceMessageThread();

        // Wait for the background engine build (IR resample + FFT partitions) to be
        // installed: keep feeding blocks, in real time, for a while.
        if (conv)
        {
            int warmBlocks = 0;
            const double warmSecs = 1.5 + 0.5 * (double) (1 << osChoice) * (irId.contains ("tunnel") || irId.contains ("tank") ? 2.0 : 1.0);
            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            while ((juce::Time::getMillisecondCounterHiRes() - t0) < warmSecs * 1000.0)
            {
                fill();
                proc->processBlock (buf, midi);
                juce::Thread::sleep (1);
                if (++warmBlocks % 20 == 0) proc->serviceMessageThread();
            }
        }
        for (int i = 0; i < 40; ++i) { fill(); proc->processBlock (buf, midi); }
        proc->serviceMessageThread();   // finish crossfade, warm caches

        // Slot set-up.
        const bool haveSlot = slotMode != "off";
        if (haveSlot) proc->setModSlotTarget (0, pid::fx::distTone);
        const int totalBlocks = juce::jmax (200, (int) (secs * sr / block));
        if (slotMode == "held") setParam (*proc, pid::modSlotDepth (0), 0.5f);

        measuring = true;
        std::vector<double> pct;
        pct.reserve ((size_t) totalBlocks);
        const double budgetSec = (double) block / sr;
        const auto c0 = proc->getChainInvocationCount();
        const auto m0 = proc->getModulatedChunkCount();

        auto nextDeadline = std::chrono::steady_clock::now();
        const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration> (std::chrono::duration<double> (budgetSec));
        for (int b = 0; b < totalBlocks; ++b)
        {
            if (paced)   // real-time cadence: one block per budget period, idle in between like a host callback
            {
                nextDeadline += period;
                std::this_thread::sleep_until (nextDeadline);
            }
            if (slotMode == "ramp")
                setParam (*proc, pid::modSlotDepth (0), 0.5f * (float) b / (float) totalBlocks);
            if (withServiceThread && b % juce::jmax (1, (int) (0.15 * sr / block)) == 0)
                proc->serviceMessageThread();
            fill();
            const auto t0 = juce::Time::getHighResolutionTicks();
            proc->processBlock (buf, midi);
            const auto t1 = juce::Time::getHighResolutionTicks();
            pct.push_back (100.0 * juce::Time::highResolutionTicksToSeconds (t1 - t0) / budgetSec);
        }

        Stats s;
        s.blocks = totalBlocks;
        double sum = 0;
        for (auto v : pct) { sum += v; s.maxPct = juce::jmax (s.maxPct, v); if (v > 100.0) ++s.over; }
        s.meanPct = sum / (double) pct.size();
        auto sorted = pct;
        std::sort (sorted.begin(), sorted.end());
        s.p99Pct = sorted[(size_t) ((double) sorted.size() * 0.99)];
        s.chainPerBlock = (double) (proc->getChainInvocationCount() - c0) / (double) totalBlocks;
        s.modChunkFrac = (double) (proc->getModulatedChunkCount() - m0) / (double) totalBlocks;
        return s;
    }

    inline void bench (int argc, char** argv)
    {
        std::map<juce::String, std::vector<juce::String>> k {
            { "sr", { "48000" } }, { "block", { "128", "512" } }, { "os", { "1", "2", "4" } },
            { "ir", { "sixties-spring" } }, { "fx", { "conv" } }, { "sc", { "silent" } },
            { "slot", { "off", "zero", "ramp", "held" } }, { "secs", { "3" } }, { "dist", { "0" } },
            { "svc", { "0" } }, { "inp", { "noise" } }, { "pace", { "0" } } };
        for (int i = 2; i < argc; ++i)
        {
            const juce::String a (argv[i]);
            k[a.upToFirstOccurrenceOf ("=", false, false)] = splitList (a.fromFirstOccurrenceOf ("=", false, false));
        }

        std::cout << "sr     block  os  ir                 fx    sc      slot  | mean%  p99%   max%   over/blk | chain/blk modChunk%\n";
        for (auto& srS : k["sr"]) for (auto& blS : k["block"]) for (auto& osS : k["os"])
        for (auto& ir : k["ir"]) for (auto& fxs : k["fx"]) for (auto& sc : k["sc"]) for (auto& slot : k["slot"])
        {
            const double sr = srS.getDoubleValue();
            const int block = blS.getIntValue();
            const int osv = osS.getIntValue();
            const int osChoice = osv == 4 ? 2 : osv == 2 ? 1 : 0;
            const auto s = run (sr, block, osChoice, ir, fxs, sc, slot, k["secs"][0].getDoubleValue(),
                                k["dist"][0].getIntValue() != 0, k["svc"][0].getIntValue() != 0, k["inp"][0] == "silence", k["pace"][0] != "0");
            std::cout << juce::String ((int) sr).paddedRight (' ', 7) << juce::String (block).paddedRight (' ', 7)
                      << juce::String (osv) << "x  " << ir.paddedRight (' ', 19) << fxs.paddedRight (' ', 6)
                      << sc.paddedRight (' ', 8) << slot.paddedRight (' ', 6) << "| "
                      << juce::String (s.meanPct, 1).paddedLeft (' ', 6) << " " << juce::String (s.p99Pct, 1).paddedLeft (' ', 6)
                      << " " << juce::String (s.maxPct, 1).paddedLeft (' ', 7) << " "
                      << (juce::String (s.over) + "/" + juce::String (s.blocks)).paddedLeft (' ', 9) << " | "
                      << juce::String (s.chainPerBlock, 1).paddedLeft (' ', 8) << " "
                      << juce::String (100.0 * s.modChunkFrac, 0).paddedLeft (' ', 6) << "\n" << std::flush;
        }
    }
}

// `SPAStripTests --bench-gesture [os=4] [block=1024] [secs=6] [ir=sixties-spring]`: the audio thread runs
// PACED in real time (taking getCallbackLock() around processBlock like an AU/VST3 wrapper) while the
// main thread -- the message thread -- has the editor open (its timers forced live), keeps the plugin's
// 150 ms service timer going and performs knob-drag-style parameter gestures on mod slot 1's depth
// (beginChangeGesture / setValueNotifyingHost every ~8 ms / endChangeGesture, i.e. an undo step each).
// Reports, per phase, the callback-lock wait and block time as a % of the budget and the over-budget
// count. Sidechain bus enabled but silent, slot 1 = fxDist.tone, as in the field report.
namespace benchGesture
{
    struct PhaseStats { double lockMaxPct = 0, meanPct = 0, p99Pct = 0, maxPct = 0; int over = 0, blocks = 0; };

    inline void bench (int argc, char** argv)
    {
        int osv = 4, block = 1024; double secs = 6.0; juce::String ir = "sixties-spring"; bool withEditor = true, withGesture = true; int stepMs = 8; bool osPhase = true;
        for (int i = 2; i < argc; ++i)
        {
            const juce::String a (argv[i]);
            const auto key = a.upToFirstOccurrenceOf ("=", false, false);
            const auto val = a.fromFirstOccurrenceOf ("=", false, false);
            if (key == "os") osv = val.getIntValue();
            if (key == "block") block = val.getIntValue();
            if (key == "secs") secs = val.getDoubleValue();
            if (key == "ir") ir = val;
            if (key == "editor") withEditor = val.getIntValue() != 0;
            if (key == "gesture") withGesture = val.getIntValue() != 0;
            if (key == "step") stepMs = val.getIntValue();
            if (key == "osphase") osPhase = val.getIntValue() != 0;
        }
        const double sr = 48000.0;
        // Hermetic: never touch the user's real presets / settings.
        const auto root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                              .getChildFile ("spastrip-bench-presets-" + juce::String (juce::Random::getSystemRandom().nextInt (1000000)));
        spa::preset::PresetManager::setPresetsRootOverride (root);
        spa::ui::settings::setSettingsFileOverride (root.getSiblingFile (root.getFileName() + "-settings.xml"));
        spa::ui::forceLiveFlag().store (true);
        auto proc = makeProc (sr, block, 2, 2, 2);
        setAllEffects (*proc, false);
        for (auto* id : { pid::fx::eqEnable, pid::fx::tremEnable, pid::fx::vibEnable, pid::fx::compEnable,
                          pid::fx::limEnable, pid::fx::convEnable })
            setParam (*proc, id, 1.0f);
        setParam (*proc, pid::fx::convMix, 1.0f);
        setParam (*proc, pid::oversampling, (float) (osv == 4 ? 2 : osv == 2 ? 1 : 0));
        proc->loadFactoryIR (ir);
        proc->setModSlotTarget (0, pid::fx::distTone);

        juce::AudioBuffer<float> warm (4, block);
        juce::MidiBuffer midi;
        Noise nzw (1);
        for (int c = 0; c < 4; ++c) for (int i = 0; i < block; ++i) warm.setSample (c, i, c < 2 ? 0.25f * nzw.next() : 0.0f);
        proc->processBlock (warm, midi);
        proc->serviceMessageThread();

        std::unique_ptr<spa::SPAStripEditor> editor;
        if (withEditor)
        {
            editor = std::make_unique<spa::SPAStripEditor> (*proc);
            editor->setSize (spa::ui::metrics::baseWidth, spa::ui::metrics::baseHeight);
        }

        std::atomic<bool> stop { false };
        std::atomic<int> phase { 0 };   // phases accumulate stats separately
        constexpr int numPhases = 4;
        std::vector<std::vector<double>> blockPct (numPhases), lockPct (numPhases);
        const double budget = (double) block / sr;
        std::thread audio ([&]
        {
           #if JUCE_MAC
            pthread_set_qos_class_self_np (QOS_CLASS_USER_INTERACTIVE, 0);
           #endif
            juce::AudioBuffer<float> buf (4, block);
            Noise nz (2);
            auto next = std::chrono::steady_clock::now();
            while (! stop.load())
            {
                for (int c = 0; c < 4; ++c) for (int i = 0; i < block; ++i) buf.setSample (c, i, c < 2 ? 0.25f * nz.next() : 0.0f);
                const auto t0 = juce::Time::getHighResolutionTicks();
                {
                    const juce::ScopedLock sl (proc->getCallbackLock());
                    const auto t1 = juce::Time::getHighResolutionTicks();
                    proc->processBlock (buf, midi);
                    const auto t2 = juce::Time::getHighResolutionTicks();
                    const int ph = phase.load();
                    lockPct[(size_t) ph].push_back (100.0 * juce::Time::highResolutionTicksToSeconds (t1 - t0) / budget);
                    blockPct[(size_t) ph].push_back (100.0 * juce::Time::highResolutionTicksToSeconds (t2 - t1) / budget);
                }
                next += std::chrono::duration_cast<std::chrono::steady_clock::duration> (std::chrono::duration<double> (budget));
                std::this_thread::sleep_until (next);
            }
        });

        const auto pump = [&] (double ms)
        {
            const auto end = juce::Time::getMillisecondCounterHiRes() + ms;
            while (juce::Time::getMillisecondCounterHiRes() < end)
                juce::MessageManager::getInstance()->runDispatchLoopUntil (4);
        };
        auto* depth = proc->getAPVTS().getParameter (pid::modSlotDepth (0));

        pump (2500);                                  // let the IR engine install and the audio thread settle
        phase = 1;                                    // phase 0 = settle (discarded), 1 = editor idle
        pump (secs * 1000.0 / 2);
        phase = 2;                                    // phase 2 = repeated knob drags 0 -> 0.6 -> 0
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        while (juce::Time::getMillisecondCounterHiRes() - t0 < secs * 1000.0 / 2)
        {
            if (withGesture) depth->beginChangeGesture();
            for (int k = 0; k <= 60; ++k) { depth->setValueNotifyingHost (0.5f + 0.3f * (float) k / 60.0f); pump (stepMs); }
            if (withGesture) depth->endChangeGesture();
            pump (100);
            if (withGesture) depth->beginChangeGesture();
            depth->setValueNotifyingHost (0.5f);
            if (withGesture) depth->endChangeGesture();
            pump (100);
        }
        if (osPhase) {
        phase = 3;                                    // phase 3 = oversampling switched 4x -> 2x -> 4x (rebuild under the lock)
        auto* osParam = proc->getAPVTS().getParameter (pid::oversampling);
        osParam->setValueNotifyingHost (osParam->convertTo0to1 (1.0f));
        pump (4000);
        osParam->setValueNotifyingHost (osParam->convertTo0to1 (2.0f));
        pump (6000);
        }
        stop = true;
        audio.join();
        editor.reset();
        spa::ui::forceLiveFlag().store (false);

        const char* names[numPhases] = { "settle (ignore)", "editor open, idle", "knob drags on slot 1 depth", "os 4x->2x->4x switch" };
        std::cout << "os=" << osv << "x block=" << block << " ir=" << ir << " (paced, " << budget * 1000.0 << " ms budget)\n"
                  << "phase                        | blocks | lock-wait max (% of budget / ms) | block mean% p99% max% | over budget\n";
        for (int p = 1; p < numPhases; ++p)
        {
            auto v = blockPct[(size_t) p];
            if (v.empty()) continue;
            double sum = 0, mx = 0; int over = 0;
            for (auto x : v) { sum += x; mx = juce::jmax (mx, x); if (x > 100.0) ++over; }
            auto sorted = v; std::sort (sorted.begin(), sorted.end());
            double lockMax = 0; for (auto x : lockPct[(size_t) p]) lockMax = juce::jmax (lockMax, x);
            std::cout << juce::String (names[p]).paddedRight (' ', 29) << "| " << juce::String ((int) v.size()).paddedLeft (' ', 6)
                      << " | " << juce::String (lockMax, 2).paddedLeft (' ', 10) << " / " << juce::String (lockMax * budget * 10.0, 2).paddedRight (' ', 18) << " | "
                      << juce::String (sum / (double) v.size(), 1).paddedLeft (' ', 10) << " "
                      << juce::String (sorted[(size_t) ((double) sorted.size() * 0.99)], 1).paddedLeft (' ', 5) << " "
                      << juce::String (mx, 1).paddedLeft (' ', 5) << " | " << over << "\n";
        }
    }
}

//==============================================================================
// Deterministic regression guard for the 4x-oversampling CPU overload (field report: Logic,
// 1024-sample buffer, 4x, CONV on with a long IR, slot 1 -> Dist Tone, depth dragged up from 0
// with the sidechain bus off/silent). Wall-clock budgets are flaky on shared CI, so this asserts
// the structural quantities that determined the cost:
//   * the convolution's partition scales with the engine rate (cost ~ IR x rate / partition) and
//     its per-call block is bounded;
//   * with zero / sub-threshold applied modulation the chain runs ONCE per host chunk (plain
//     path), however the depth moves; a live envelope subdivides as designed (1 call / 8 samples);
//   * steady-state processing with the IR installed, at 4x, plain and modulated, allocates nothing.
namespace benchCpu
{
    inline void cpuCostRegressionTest()
    {
        std::cout << "cpuCostRegressionTest\n";
        constexpr double sr = 48000.0;
        constexpr int block = 1024;

        const auto owner = [] (Proc& p)
        {
            setAllEffects (p, false);
            for (auto* id : { pid::fx::eqEnable, pid::fx::tremEnable, pid::fx::vibEnable, pid::fx::compEnable,
                              pid::fx::limEnable, pid::fx::convEnable })
                setParam (p, id, 1.0f);
        };

        // 1. Partition sizes. The legacy 256-sample partition made the convolution's tail cost
        // grow with the SQUARE of the oversampling factor (IR length x block rate); every
        // factor now gets a 4096-sample partition at 1024-sample host buffers, and 4x with
        // 128/256-sample buffers is matched to the engine block (one fill per block).
        const auto partitionFor = [&] (int hostBlock, int choice)
        {
            auto proc = makeProc (sr, hostBlock);
            owner (*proc);
            setParam (*proc, pid::oversampling, (float) choice);
            juce::AudioBuffer<float> b (2, hostBlock);
            juce::MidiBuffer midi;
            b.clear();
            proc->processBlock (b, midi);
            proc->serviceMessageThread();
            return std::pair<int, int> { proc->getConvolutionHeadSamples(), proc->getConvolutionChunkSamples() };
        };
        for (int choice = 0; choice < 3; ++choice)
        {
            const auto [head, chunk] = partitionFor (block, choice);
            expect (head == 4096, juce::String (1 << choice) + "x, 1024-sample host buffer: 4096-sample convolution partition (" + juce::String (head) + ")");
            expect (chunk > 0 && chunk <= 256, juce::String (1 << choice) + "x: one convolution call is capped at 256 samples (" + juce::String (chunk) + ")");
        }
        expect (partitionFor (256, 2).first == 1024 && partitionFor (128, 2).first == 512,
                "4x with 256/128-sample host buffers: partition matched to the engine block (one fill per block)");

        // 2. Chain invocations per host chunk at 4x, owner's effects, slot 1 -> Dist Tone.
        struct Counts { uint64_t chain, modulated; int blocks; };
        const auto run = [&] (int scCh, float scLevel, std::function<float (int)> depthAt, int blocks = 40)
        {
            auto proc = makeProc (sr, block, 2, 2, scCh);
            owner (*proc);
            setParam (*proc, pid::oversampling, 2.0f);
            proc->setModSlotTarget (0, pid::fx::distTone);
            juce::AudioBuffer<float> b (juce::jmax (2, 2 + scCh), block);
            juce::MidiBuffer midi;
            Noise nz (9);
            const auto fill = [&]
            {
                for (int c = 0; c < b.getNumChannels(); ++c)
                    for (int i = 0; i < block; ++i)
                        b.setSample (c, i, c < 2 ? 0.25f * nz.next() : scLevel * nz.next());
            };
            fill(); proc->processBlock (b, midi); proc->serviceMessageThread();   // factor change applied
            for (int i = 0; i < 5; ++i) { fill(); proc->processBlock (b, midi); }
            const auto c0 = proc->getChainInvocationCount();
            const auto m0 = proc->getModulatedChunkCount();
            for (int i = 0; i < blocks; ++i)
            {
                setParam (*proc, pid::modSlotDepth (0), depthAt (i));   // a parameter change every block, like a drag
                fill();
                proc->processBlock (b, midi);
            }
            return Counts { proc->getChainInvocationCount() - c0, proc->getModulatedChunkCount() - m0, blocks };
        };
        const auto ramp = [] (int i) { return 0.5f * (float) i / 40.0f; };

        auto c = run (0, 0.0f, ramp);
        expect (c.modulated == 0 && c.chain == (uint64_t) c.blocks,
                "sidechain bus disabled, depth ramping: plain path, 1 chain call per host block (" + juce::String ((int) c.chain) + " calls / " + juce::String (c.blocks) + " blocks)");
        c = run (2, 0.0f, ramp);
        expect (c.modulated == 0 && c.chain == (uint64_t) c.blocks,
                "sidechain bus enabled but silent, depth ramping: plain path (" + juce::String ((int) c.chain) + " calls / " + juce::String (c.blocks) + " blocks)");
        c = run (2, 1.0e-3f, [] (int) { return 1.0e-3f; });
        expect (c.modulated == 0 && c.chain == (uint64_t) c.blocks,
                "a -60 dBFS sidechain noise floor with a near-zero depth (offset < 1e-4): plain path (" + juce::String ((int) c.chain) + " calls)");
        c = run (2, 1.0e-3f, [] (int) { return 1.0f; });
        expect (c.modulated == (uint64_t) c.blocks,
                "the same noise floor at depth 1.0 does modulate (so the previous check is not vacuous)");
        c = run (2, 0.5f, [] (int) { return 0.5f; });
        expect (c.modulated == (uint64_t) c.blocks && c.chain == (uint64_t) c.blocks * (block / 8),
                "a live envelope subdivides as designed: 1 chain call per 8 host samples (" + juce::String ((int) c.chain) + " calls / " + juce::String (c.blocks) + " blocks)");

       #if JUCE_MAC
        // 3. No heap allocation in steady state with the IR installed at 4x, plain and modulated.
        {
            auto proc = makeProc (sr, block, 2, 2, 2);
            owner (*proc);
            setParam (*proc, pid::fx::convMix, 1.0f);
            setParam (*proc, pid::oversampling, 2.0f);
            proc->loadFactoryIR ("studio-b-near");
            proc->setModSlotTarget (0, pid::fx::distTone);
            juce::AudioBuffer<float> b (4, block);
            juce::MidiBuffer midi;
            Noise nz (11);
            float scLevel = 0.0f;
            const auto fill = [&] { for (int ch = 0; ch < 4; ++ch) for (int i = 0; i < block; ++i) b.setSample (ch, i, ch < 2 ? 0.25f * nz.next() : scLevel * nz.next()); };
            fill(); proc->processBlock (b, midi);
            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            int n = 0;
            while (juce::Time::getMillisecondCounterHiRes() - t0 < 2500.0)   // IR resample + partition build on the worker thread
            {
                fill(); proc->processBlock (b, midi); juce::Thread::sleep (1);
                if (++n % 20 == 0) proc->serviceMessageThread();
            }
            for (int i = 0; i < 20; ++i) { fill(); proc->processBlock (b, midi); }
            for (int variant = 0; variant < 2; ++variant)
            {
                scLevel = variant == 0 ? 0.0f : 0.5f;
                for (int i = 0; i < 10; ++i) { fill(); proc->processBlock (b, midi); }
                malloc_logger = spaStripMallocLogger;
                g_allocCount = 0;
                g_countAllocs = true;
                for (int i = 0; i < 60; ++i)
                {
                    if (i % 10 == 0) { g_countAllocs = false; setParam (*proc, pid::modSlotDepth (0), 0.01f * (float) i); g_countAllocs = true; }
                    fill();
                    proc->processBlock (b, midi);
                }
                g_countAllocs = false;
                malloc_logger = nullptr;
                expect (g_allocCount.load() == 0,
                        juce::String ("4x with the convolution IR installed, ") + (variant == 0 ? "silent sidechain (plain path)" : "live sidechain (modulated path)")
                            + ": 60 blocks, no heap allocation (" + juce::String ((juce::int64) g_allocCount.load()) + " seen)");
            }
        }
       #endif
    }
}
