// Phase 2a tests: sidechain envelope follower, modulation matrix, phase-coherent
// dry/wet under oversampling, factory impulse responses.
// Included from SPAStripTests.cpp, where `expect`, `Proc`, `pid`, `setParam`,
// `makeProc`, `Noise` ... are in scope.

namespace phase2Tests
{
    namespace fx = spa::params::id::fx;

    //==========================================================================
    // Helpers.
    constexpr double kTwoPi = juce::MathConstants<double>::twoPi;

    // The enable parameter of the effect a parameter ID belongs to.
    juce::String enableIdFor (const juce::String& paramId)
    {
        const auto prefix = paramId.upToFirstOccurrenceOf (".", false, false);
        return prefix + ".enable";
    }

    // Reads telemetry and the processor without caring about layout: a
    // steady main tone plus an amplitude-modulated sidechain carrier.
    struct Rig
    {
        double sr;
        int block;
        std::unique_ptr<Proc> proc;
        juce::AudioBuffer<float> buf;
        juce::MidiBuffer midi;
        long pos = 0;

        Rig (double sampleRate, int blockSize, int inCh = 2, int outCh = 2, int scCh = 2)
            : sr (sampleRate), block (blockSize), proc (makeProc (sampleRate, blockSize, inCh, outCh, scCh))
        {
            buf.setSize (juce::jmax (proc->getTotalNumInputChannels(), proc->getTotalNumOutputChannels()), blockSize);
            buf.clear();
            setAllEffects (*proc, false);
        }

        int numIn() const { return proc->getMainBusNumInputChannels(); }
    };

    //==========================================================================
    // Zipper / discontinuity audit of the modulation targets.
    struct AuditResult
    {
        // Equivalent step size (dB re the 0.5-amplitude tone) of the largest
        // discontinuity in the rendered output, for each modulation refresh
        // interval; `base` is the same render with the slot off. Measured with a
        // 4th difference (a 440 Hz tone is ~-100 dB there; a step of size s
        // shows as 6s), over both channels.
        double stepDb32 = 0.0, stepDb8 = 0.0, stepDb8s = 0.0, stepDb4 = 0.0, stepDb1 = 0.0, stepDbBase = 0.0;
        double effectDb = 0.0;   // rms of (modulated - unmodulated) re the signal: is the target audible at all
    };

    using Stereo = std::array<std::vector<float>, 2>;

    // FILTER audit set-up (--audit-mod ... <type> <resonance> [noglide]): the filter type and
    // resonance the filter targets are measured at.
    // `factorChoice` (--audit-mod ... [oversampling 0..2], applies to every target) is the oversampling choice index.
    struct FilterAuditSetup { int type = 0; float resonance = 0.0f; int factorChoice = 0; };
    FilterAuditSetup& filterAuditSetup() { static FilterAuditSetup s; return s; }

    // Puts the effect owning `id` into a state where `id` is audible, with the
    // parameter at normalised `baseNorm`. Returns the tone frequency to use.
    double prepareTarget (Proc& proc, const juce::String& id, float baseNorm, const juce::File& irFile)
    {
        setParam (proc, enableIdFor (id), 1.0f);
        const auto set = [&] (const juce::String& p, float v) { setParam (proc, p, v); };
        double toneHz = 440.0;

        if (id.startsWith ("fxEQ."))
        {
            const auto band = id.fromFirstOccurrenceOf ("fxEQ.", false, false).upToFirstOccurrenceOf (".", false, false);
            set ("fxEQ." + band + ".enable", 1.0f);
            set ("fxEQ." + band + ".type", 0.0f);
            set ("fxEQ." + band + ".gain", 12.0f);
            set ("fxEQ." + band + ".freq", 600.0f);
        }
        if (id.startsWith ("fxDelay."))     { set (fx::delaySync, 0.0f); set (fx::delayPingPong, 1.0f); set (fx::delayFeedback, 0.5f); set (fx::delayTime, 120.0f); }
        if (id.startsWith ("fxMod."))       { set (fx::modType, id == fx::modCentre ? 0.0f : 1.0f); }   // centre: phaser; the rest: flanger
        if (id.startsWith ("fxLim."))       { set (fx::limDrive, 12.0f); }
        if (id.startsWith ("fxChorus."))    { set (fx::chorusMode, 1.0f); }
        if (id.startsWith ("fxConv."))
        {
            proc.loadConvolutionIR (irFile);
            proc.serviceMessageThread();
        }
        if (id.startsWith ("fxGrain."))     { set (fx::grainMix, 0.5f); }
        if (id.startsWith ("fxFilter."))
        {
            // Filter 1 on for any filter target; a filter 2 target also switches filter 2 on (in series,
            // behind a filter 1 that is wide open). The audited filter runs at the set-up type/resonance.
            const bool second = id.startsWith ("fxFilter.f2.");
            set (fx::filterEnable, 1.0f);
            if (second) set (fx::filter2Enable, 1.0f);
            set (second ? fx::filter2Type : fx::filter1Type, (float) filterAuditSetup().type);
            set (second ? fx::filter2Res : fx::filter1Res, filterAuditSetup().resonance);
        }
        if (id.startsWith ("fxComp.band0")) toneHz = 90.0;
        if (id.startsWith ("fxComp.band2")) toneHz = 5000.0;

        if (auto* p = proc.getAPVTS().getParameter (id))
            p->setValueNotifyingHost (baseNorm);
        return toneHz;
    }

    // Renders `seconds` of a steady 0.5-amplitude tone (main input) with the
    // sidechain fed a 12 kHz carrier whose amplitude follows a slow sine (0..1,
    // `envHz`). `interval` = modulation refresh interval (host samples); depth 0
    // = no slot active.
    Stereo renderTarget (const juce::String& id, int interval, float depth, double sr,
                         const juce::File& irFile, double seconds, double envHz,
                         float baseNorm, int factorChoice = 0, int block = 64,
                         bool squareEnv = false, float smoothMs = 0.0f)
    {
        Rig rig (sr, block);
        auto& proc = *rig.proc;
        const double toneHz = prepareTarget (proc, id, baseNorm, irFile);
        setParam (proc, pid::oversampling, (float) factorChoice);
        setParam (proc, pid::sc::attack, 0.1f);
        setParam (proc, pid::sc::release, 50.0f);
        setParam (proc, pid::sc::gain, 3.0103f);
        proc.setModUpdateInterval (interval);
        if (smoothMs >= 0.0f)
            proc.setModSmoothingMs (smoothMs);
        if (! proc.setModSlotTarget (0, id) && depth != 0.0f)
            return {};
        setParam (proc, pid::modSlotDepth (0), depth);

        if (id.startsWith ("fxConv."))
            juce::Thread::sleep (400);

        Stereo out;
        const long total = (long) (seconds * sr);
        for (long done = 0; done < total; done += block)
        {
            for (int i = 0; i < block; ++i)
            {
                const double t = (double) (done + i) / sr;
                const float tone = 0.5f * (float) std::sin (kTwoPi * toneHz * t);
                const float amp = squareEnv ? (std::fmod (t * envHz, 1.0) < 0.5 ? 1.0f : 0.0f)
                                            : 0.5f - 0.5f * (float) std::cos (kTwoPi * envHz * t);
                // 12 kHz carrier at 48 kHz with a 45 degree phase: |x| is exactly
                // 0.707 x amp on every sample, so the detector sees a ripple-free
                // slow sine (+3 dB sc.gain restores unity).
                const float car = amp * (float) std::sin (kTwoPi * 12000.0 * t + juce::MathConstants<double>::pi * 0.25);
                rig.buf.setSample (0, i, tone);
                rig.buf.setSample (1, i, tone);
                rig.buf.setSample (2, i, car);
                rig.buf.setSample (3, i, car);
            }
            proc.processBlock (rig.buf, rig.midi);
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < block; ++i)
                    out[(size_t) ch].push_back (rig.buf.getSample (ch, i));
            if ((done / block) % 4 == 0)
                proc.serviceMessageThread();
        }
        return out;
    }

    // dB (re the 0.5 tone) of the largest 4th-difference spike / 6.
    double stepDbOf (const Stereo& y, size_t from)
    {
        double pk = 0.0;
        for (const auto& c : y)
            for (size_t i = from + 4; i < c.size(); ++i)
            {
                const double d4 = (double) c[i] - 4.0 * c[i - 1] + 6.0 * c[i - 2] - 4.0 * c[i - 3] + c[i - 4];
                pk = std::max (pk, std::abs (d4));
            }
        return 20.0 * std::log10 (std::max (1.0e-9, pk / 6.0 / 0.5));
    }

    double diffDb (const Stereo& a, const Stereo& b, size_t from)
    {
        double e = 0.0, ref = 0.0;
        for (size_t ch = 0; ch < 2; ++ch)
        {
            const size_t n = std::min (a[ch].size(), b[ch].size());
            for (size_t i = from; i < n; ++i)
            {
                const double d = (double) a[ch][i] - b[ch][i];
                e += d * d;
                ref += (double) b[ch][i] * b[ch][i];
            }
        }
        return 10.0 * std::log10 (std::max (1.0e-18, e) / std::max (1.0e-18, ref));
    }

    struct AuditConfig { double envHz = 1.0; bool square = false; float shipSmoothMs = Proc::kDefaultModSmoothingMs; int shipInterval = Proc::kDefaultModUpdateInterval; };

    AuditResult auditTarget (const juce::String& id, double sr, const juce::File& irFile,
                             const AuditConfig& cfg, float depth = 0.5f, float baseNorm = 0.4f)
    {
        const double seconds = 3.0;
        const size_t from = (size_t) (1.0 * sr);
        const auto run = [&] (int interval, float d, float smooth)
        {
            return renderTarget (id, interval, d, sr, irFile, seconds, cfg.envHz, baseNorm, filterAuditSetup().factorChoice, 64, cfg.square, smooth);
        };
        const auto base = run (32, 0.0f, 0.0f);
        AuditResult r;
        r.stepDbBase = stepDbOf (base, from);
        const auto m32 = run (32, depth, 0.0f);
        r.stepDb32 = stepDbOf (m32, from);
        r.effectDb = diffDb (m32, base, from);
        r.stepDb8  = stepDbOf (run (8, depth, 0.0f), from);
        r.stepDb8s = stepDbOf (run (cfg.shipInterval, depth, cfg.shipSmoothMs), from);
        r.stepDb4  = stepDbOf (run (4, depth, 0.0f), from);
        r.stepDb1  = stepDbOf (run (1, depth, 0.0f), from);
        return r;
    }

    // `--audit-mod [substring] [sampleRate] [envelopeHz]`: prints the audit for every float
    // FX parameter (excluded ones included) whose ID contains the substring.
    void auditModTargets (const juce::String& filter, double sr, const AuditConfig& cfg)
    {
        spa::mod::setAuditIncludeExcluded (true);   // measure the excluded targets too
        const auto irFile = writeTestIR ("spastrip-audit-ir", sr, 1.5);
        std::cout << "step = largest discontinuity, dB re the 0.5 tone. Columns: slot off | refresh every 32 | 8 | 8 + 4 ms offset slew (shipping default) | 4 | 1 host samples (slew off unless stated) | effect of the modulation (dB re signal)\n";
        for (const auto& d : spa::params::all())
        {
            if (d.kind != spa::params::ParamKind::floatParam || d.section == spa::params::Section::global
                || d.section == spa::params::Section::sidechain || d.section == spa::params::Section::modMatrix)
                continue;
            if (filter.isNotEmpty() && ! d.id.contains (filter))
                continue;
            const auto r = auditTarget (d.id, sr, irFile, cfg);
            const auto f = [] (double v) { return juce::String (v, 1).paddedLeft (' ', 7); };
            std::cout << "  " << d.id.paddedRight (' ', 24) << " | " << f (r.stepDbBase) << " | " << f (r.stepDb32) << " " << f (r.stepDb8)
                      << " " << f (r.stepDb8s) << " " << f (r.stepDb4) << " " << f (r.stepDb1) << " | " << f (r.effectDb) << "\n";
        }
        irFile.deleteFile();
    }
}

namespace phase2Tests
{
    // `--bench-mod`: CPU cost (percent of one core) of the chain with every effect on
    // and all 8 slots active, per modulation refresh interval and oversampling factor.
    void benchModulation()
    {
        const auto irFile = writeTestIR ("spastrip-bench-ir", 48000.0, 2.0);
        const char* ids[8] = { "fxEQ.band2.gain", "fxDelay.mix", "fxReverb.mix", "fxChorus.depth",
                               "fxComp.band1.thresh", "fxDist.drive", "fxTrem.depth", "fxMod.depth" };
        for (int factorChoice : { 0, 2 })
        {
            for (int interval : { 0, 32, 16, 8, 4, 2, 1 })
            {
                Rig rig (48000.0, 256);
                auto& proc = *rig.proc;
                setAllEffects (proc, true);
                setParam (proc, pid::oversampling, (float) factorChoice);
                proc.loadConvolutionIR (irFile);
                proc.serviceMessageThread();
                juce::Thread::sleep (300);
                for (int s = 0; s < 8; ++s)
                {
                    proc.setModSlotTarget (s, ids[s]);
                    setParam (proc, pid::modSlotDepth (s), interval == 0 ? 0.0f : 0.1f);
                }
                if (interval > 0) proc.setModUpdateInterval (interval);
                Noise nz (5);
                const int blocks = 600;
                double secs = 0.0;
                for (int pass = 0; pass < 2; ++pass)
                {
                    const auto t0 = juce::Time::getMillisecondCounterHiRes();
                    for (int b = 0; b < blocks; ++b)
                    {
                        for (int c = 0; c < 4; ++c) for (int i = 0; i < 256; ++i) rig.buf.setSample (c, i, 0.3f * nz.next());
                        proc.processBlock (rig.buf, rig.midi);
                        if (b % 40 == 0) proc.serviceMessageThread();
                    }
                    secs = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
                }
                const double audio = blocks * 256.0 / 48000.0;
                std::cout << "  " << (factorChoice == 0 ? "1x" : "4x") << "  interval " << (interval == 0 ? juce::String ("off (no slots)") : juce::String (interval))
                          << ": " << juce::String (100.0 * secs / audio, 1) << " % of one core\n";
            }
        }
        irFile.deleteFile();
    }
}


//==============================================================================
// Phase 2a tests.
namespace phase2Tests
{
    using Filler = std::function<float (int ch, long n)>;   // buffer channel, absolute sample -> value
    using BlockHook = std::function<void (long blockEnd)>;

    // Runs the rig for `total` samples. Outputs of channels 0/1 are appended to
    // outL/outR when given.
    void runRig (Rig& r, long total, const Filler& fill, std::vector<float>* outL = nullptr,
                 std::vector<float>* outR = nullptr, const BlockHook& hook = {}, int serviceEvery = 4)
    {
        const int nch = r.buf.getNumChannels();
        long blk = 0;
        for (long done = 0; done < total; done += r.block, ++blk)
        {
            const int n = (int) std::min<long> (r.block, total - done);
            for (int ch = 0; ch < nch; ++ch)
                for (int i = 0; i < n; ++i)
                    r.buf.setSample (ch, i, fill (ch, done + i));
            juce::AudioBuffer<float> view (r.buf.getArrayOfWritePointers(), nch, n);
            r.proc->processBlock (view, r.midi);
            for (int i = 0; i < n; ++i)
            {
                if (outL != nullptr) outL->push_back (view.getSample (0, i));
                if (outR != nullptr) outR->push_back (view.getSample (juce::jmin (1, nch - 1), i));
            }
            if (hook) hook (done + n);
            if (serviceEvery > 0 && blk % serviceEvery == 0)
                r.proc->serviceMessageThread();
        }
    }

    // |x| = a on every sample for a 48 kHz host: a 12 kHz sine at 45 degrees has
    // |sin| = 0.7071, so amplitude a / 0.7071 gives a constant detector input.
    float carrier (long n, double a, double sr = 48000.0)
    {
        const double amp = a / 0.70710678118654752;
        return (float) (amp * std::sin (kTwoPi * 12000.0 * (double) n / sr + juce::MathConstants<double>::pi * 0.25));
    }

    float sine (long n, double hz, double amp, double sr = 48000.0)
    {
        return (float) (amp * std::sin (kTwoPi * hz * (double) n / sr));
    }

    double rmsRange (const std::vector<float>& x, size_t from, size_t to)
    {
        double e = 0.0;
        for (size_t i = from; i < to; ++i) e += (double) x[i] * x[i];
        return std::sqrt (e / (double) std::max<size_t> (1, to - from));
    }

    bool finiteVec (const std::vector<float>& x)
    {
        for (float v : x) if (! std::isfinite (v)) return false;
        return true;
    }

    //==========================================================================
    void sidechainParamsTest()
    {
        std::cout << "sidechainParamsTest\n";
        namespace params = spa::params;
        const auto chk = [] (const char* id, params::ParamKind kind, float lo, float hi, float def, bool skewed)
        {
            const auto* d = params::find (id);
            bool ok = d != nullptr && d->kind == kind && ! d->random.enabled && juce::approximatelyEqual (d->defaultValue, def);
            if (ok && kind == params::ParamKind::floatParam)
                ok = juce::approximatelyEqual (d->range.start, lo) && juce::approximatelyEqual (d->range.end, hi)
                     && (skewed ? std::abs (d->range.skew - 1.0f) > 1.0e-4f : true);
            expect (ok, juce::String (id) + ": registry entry (range, default, skew, randomisation off)");
        };
        using K = params::ParamKind;
        chk ("sc.source",  K::choiceParam, 0, 0, 0.0f, false);
        chk ("sc.gain",    K::floatParam, -24.0f, 24.0f, 0.0f, false);
        chk ("sc.attack",  K::floatParam, 0.1f, 500.0f, 10.0f, true);
        chk ("sc.release", K::floatParam, 5.0f, 2000.0f, 150.0f, true);
        chk ("sc.hpf",     K::floatParam, 20.0f, 2000.0f, 20.0f, true);
        chk ("sc.listen",  K::boolParam, 0, 0, 0.0f, false);
        for (int i = 0; i < 8; ++i)
            chk (params::id::modSlotDepth (i).toRawUTF8(), K::floatParam, -1.0f, 1.0f, 0.0f, false);
        expect (params::find ("sc.source")->choices == juce::StringArray ({ "External", "Input" }), "sc.source choices: External, Input");
        expect (params::id::modSlotDepth (0) == "mod.slot1.depth" && params::id::modSlotDepth (7) == "mod.slot8.depth", "slot depth IDs mod.slot1..8.depth");
        // Skews put the stated default at the geometric/"centre" position.
        const auto atDefault = [] (const char* id) { const auto* d = params::find (id); return d->range.convertTo0to1 (d->defaultValue); };
        expect (std::abs (atDefault ("sc.attack") - 0.5f) < 1.0e-3f && std::abs (atDefault ("sc.release") - 0.5f) < 1.0e-3f, "sc.attack / sc.release are skewed with their defaults at mid-travel");
        const auto* hpf = params::find ("sc.hpf");
        expect (std::abs (hpf->range.convertTo0to1 (std::sqrt (20.0f * 2000.0f)) - 0.5f) < 1.0e-3f, "sc.hpf is log-skewed (geometric centre at mid-travel)");

        // Existing FX parameters are untouched: the ported FX count is still the synth's 159;
        // the FILTER module adds its own 13 on top.
        expect (params::numFxParams() == 160 + 13 + 9, "FX parameter count is the ported 160 plus 13 FILTER and 9 COMP band parameters");
        Proc proc;
        expect (proc.getParameters().size() == 160 + 13 + 9 + 4 + 6 + 8, "host sees 160 + 13 + 9 FX + 4 global + 6 sidechain + 8 slot-depth parameters");
        // Slot targets are not host parameters.
        bool anyTarget = false;
        for (auto* p : proc.getParameters())
            if (auto* wid = dynamic_cast<juce::AudioProcessorParameterWithID*> (p))
                anyTarget = anyTarget || wid->paramID.containsIgnoreCase ("target");
        expect (! anyTarget, "slot targets are plugin state, not host parameters");
    }

    //==========================================================================
    void sidechainEnvelopeTimingTest()
    {
        std::cout << "sidechainEnvelopeTimingTest\n";
        constexpr double sr = 48000.0;
        constexpr int block = 16;
        struct Case { float attack, release; };
        for (const Case c : { Case { 20.0f, 100.0f }, Case { 5.0f, 400.0f }, Case { 80.0f, 40.0f } })
        {
            Rig rig (sr, block);
            setParam (*rig.proc, pid::sc::attack, c.attack);
            setParam (*rig.proc, pid::sc::release, c.release);
            const long onAt = (long) (0.1 * sr), offAt = (long) (2.0 * sr), total = (long) (3.5 * sr);
            double tAttack = -1.0, tRelease = -1.0;
            float atEnd = 0.0f;
            const float level = 0.5f;
            runRig (rig, total,
                    [&] (int ch, long n) { return ch < 2 ? 0.0f : (n >= onAt && n < offAt ? carrier (n, level) : 0.0f); },
                    nullptr, nullptr,
                    [&] (long end)
                    {
                        const float e = rig.proc->getTelemetry().scEnvelope.load();
                        if (tAttack < 0.0 && end > onAt && e >= level * (1.0f - std::exp (-1.0f)))
                            tAttack = 1000.0 * (double) (end - onAt) / sr;
                        if (end <= offAt) atEnd = e;
                        if (tRelease < 0.0 && end > offAt && e <= level * std::exp (-1.0f))
                            tRelease = 1000.0 * (double) (end - offAt) / sr;
                    }, 0);
            const juce::String tag = "attack " + juce::String (c.attack, 0) + " ms / release " + juce::String (c.release, 0) + " ms: ";
            std::cout << "  " << tag << "63% rise at " << tAttack << " ms, 37% fall at " << tRelease << " ms, steady " << atEnd << "\n";
            expect (std::abs (tAttack - c.attack) <= 0.05 * c.attack + 0.4, tag + "attack time constant within 5% (+0.4 ms block resolution)");
            expect (std::abs (tRelease - c.release) <= 0.05 * c.release + 0.4, tag + "release time constant within 5%");
            expect (std::abs (atEnd - level) < 0.01f, tag + "steady-state envelope equals the peak level (0.5)");
        }

        // Output clamped 0..1: +24 dB on a loud signal.
        {
            Rig rig (sr, 64);
            setParam (*rig.proc, pid::sc::gain, 24.0f);
            float maxEnv = 0.0f;
            runRig (rig, (long) sr, [&] (int ch, long n) { return ch < 2 ? 0.0f : sine (n, 1000.0, 0.9); }, nullptr, nullptr,
                    [&] (long) { maxEnv = std::max (maxEnv, rig.proc->getTelemetry().scEnvelope.load()); });
            expect (maxEnv <= 1.0f && maxEnv > 0.99f, "envelope output is clamped to 1.0 (" + juce::String (maxEnv, 4) + ")");
        }

        // Gain and HPF shape the detector input.
        {
            const auto steady = [&] (float gainDb, float hpf, double hz)
            {
                Rig rig (sr, 64);
                setParam (*rig.proc, pid::sc::gain, gainDb);
                setParam (*rig.proc, pid::sc::hpf, hpf);
                setParam (*rig.proc, pid::sc::attack, 1.0f);
                float e = 0.0f;
                runRig (rig, (long) (1.5 * sr), [&] (int ch, long n) { return ch < 2 ? 0.0f : sine (n, hz, 0.25); }, nullptr, nullptr,
                        [&] (long) { e = rig.proc->getTelemetry().scEnvelope.load(); });
                return e;
            };
            const float base = steady (0.0f, 20.0f, 2000.0);
            const float plus6 = steady (6.0206f, 20.0f, 2000.0);
            const float hp = steady (0.0f, 2000.0f, 100.0);
            expect (plus6 > 1.9f * base && plus6 < 2.1f * base, "sc.gain +6 dB doubles the envelope (" + juce::String (base, 3) + " -> " + juce::String (plus6, 3) + ")");
            expect (hp < 0.01f, "sc.hpf 2 kHz removes a 100 Hz sidechain signal (env " + juce::String (hp, 4) + ")");
        }
    }

    //==========================================================================
    // Renders a noise-fed all-effects run (no sidechain content) and returns L/R.
    void noiseRun (Rig& rig, long total, std::vector<float>& l, std::vector<float>& r)
    {
        Noise nz (77);
        runRig (rig, total, [&] (int ch, long) { const float v = 0.3f * nz.next(); return ch < 2 ? v : 0.0f; }, &l, &r);
    }

    void externalBusDisabledTest()
    {
        std::cout << "externalBusDisabledTest\n";
        for (int factorChoice : { 0, 1 })
        {
            const auto render = [&] (bool withSlot, std::vector<float>& l, std::vector<float>& r, float* env, bool* present)
            {
                Rig rig (48000.0, 256, 2, 2, 0);   // sidechain bus disabled
                auto& proc = *rig.proc;
                setAllEffects (proc, true);
                setParam (proc, fx::convEnable, 0.0f);
                setParam (proc, pid::oversampling, (float) factorChoice);
                if (withSlot)
                {
                    proc.setModSlotTarget (0, fx::reverbMix);
                    proc.setModSlotTarget (1, "fxEQ.band2.gain");
                    proc.setModSlotTarget (2, fx::delayTime);
                    setParam (proc, pid::modSlotDepth (0), 0.9f);
                    setParam (proc, pid::modSlotDepth (1), -0.7f);
                    setParam (proc, pid::modSlotDepth (2), 0.5f);
                }
                noiseRun (rig, 48000, l, r);
                *env = proc.getTelemetry().scEnvelope.load();
                *present = proc.getTelemetry().scPresent.load();
            };
            std::vector<float> l0, r0, l1, r1;
            float e0 = 1.0f, e1 = 1.0f;
            bool p0 = true, p1 = true;
            render (false, l0, r0, &e0, &p0);
            render (true, l1, r1, &e1, &p1);
            const juce::String tag = juce::String (1 << factorChoice) + "x, External source, bus disabled: ";
            expect (e1 == 0.0f && ! p1, tag + "envelope is 0 and the 'sidechain present' flag is false");
            expect (l0 == l1 && r0 == r1, tag + "output with three active slots is bit-identical to the plugin without slots");
        }

        // Source Input with the bus disabled works (present flag true).
        {
            Rig rig (48000.0, 128, 2, 2, 0);
            setParam (*rig.proc, pid::sc::source, 1.0f);
            std::vector<float> l, r;
            noiseRun (rig, 24000, l, r);
            expect (rig.proc->getTelemetry().scPresent.load() && rig.proc->getTelemetry().scEnvelope.load() > 0.05f, "Input source: flag true and envelope follows the main input with no sidechain bus");
        }
        // Source External with a bus enabled but nothing assigned: flag true.
        {
            Rig rig (48000.0, 128, 2, 2, 2);
            std::vector<float> l, r;
            noiseRun (rig, 12000, l, r);
            expect (rig.proc->getTelemetry().scPresent.load(), "External source with the bus enabled: 'present' flag true");
        }
    }

    //==========================================================================
    // Distortion MIX as the probe target: hard clip, drive 1, a 1 kHz tone; the
    // 3 kHz component measures how much distortion is mixed in.
    struct DistRig
    {
        Rig rig;
        float scLevel;
        DistRig (float scAmp, float baseMix, bool useInputSource = false, int scCh = 2)
            : rig (48000.0, 128, 2, 2, scCh), scLevel (scAmp)
        {
            auto& p = *rig.proc;
            setParam (p, fx::distEnable, 1.0f);
            setParam (p, fx::distType, 1.0f);
            setParam (p, fx::distDrive, 1.0f);
            setParam (p, fx::distMix, baseMix);
            setParam (p, pid::sc::source, useInputSource ? 1.0f : 0.0f);
            setParam (p, pid::sc::attack, 1.0f);
        }
        // Returns the 3 kHz amplitude of the last 0.3 s.
        double run (double seconds = 1.0)
        {
            std::vector<float> l, r;
            runRig (rig, (long) (seconds * 48000.0), [&] (int ch, long n)
            {
                if (ch < 2) return sine (n, 1000.0, 0.4);
                return carrier (n, (double) scLevel);
            }, &l, &r);
            return toneAmp (l, l.size() - 14400, l.size(), 48000.0, 3000.0);
        }
    };

    void slotChangesOutputTest()
    {
        std::cout << "slotChangesOutputTest\n";
        const double base = DistRig (0.5f, 0.0f).run();
        DistRig a (0.5f, 0.0f);
        a.rig.proc->setModSlotTarget (0, fx::distMix);
        setParam (*a.rig.proc, pid::modSlotDepth (0), 0.8f);
        const auto* param = a.rig.proc->getAPVTS().getParameter (fx::distMix);
        const float normBefore = param->getValue();
        const double mod = a.run();
        std::cout << "  3 kHz distortion product: slot off " << base << ", slot on (depth 0.8, env 0.5 -> mix 0.4) " << mod << "\n";
        expect (base < 1.0e-4 && mod > 0.02, "a slot on fxDist.mix with an external sidechain measurably changes the output");
        expect (juce::exactlyEqual (param->getValue(), normBefore) && juce::exactlyEqual (getParam (*a.rig.proc, fx::distMix), 0.0f),
                "host-visible parameter value is unchanged by the modulation (normalised and raw)");
        juce::MemoryBlock mb;
        a.rig.proc->getStateInformation (mb);
        auto xml = juce::AudioProcessor::getXmlFromBinary (mb.getData(), (int) mb.getSize());
        bool savedBase = false;
        if (xml != nullptr)
            for (auto* c : xml->getChildIterator())
                if (c->getStringAttribute ("id") == fx::distMix)
                    savedBase = std::abs (c->getDoubleAttribute ("value")) < 1.0e-9;
        expect (savedBase, "the saved state holds the base value, not the modulated one");

        // Telemetry: per-slot applied offset = depth x envelope.
        const float off = a.rig.proc->getTelemetry().modSlotOffset[0].load();
        expect (std::abs (off - 0.8f * 0.5f) < 0.01f && a.rig.proc->getTelemetry().modSlotOffset[1].load() == 0.0f,
                "telemetry: slot 1 applied offset = depth x envelope = 0.4 (" + juce::String (off, 3) + "), other slots 0");
        expect (std::abs (a.rig.proc->getTelemetry().scEnvelope.load() - 0.5f) < 0.01f && a.rig.proc->getTelemetry().scPresent.load(),
                "telemetry: envelope 0.5 and sidechain present");

        // Internal source, no sidechain bus at all.
        DistRig b (0.0f, 0.0f, true, 0);
        b.rig.proc->setModSlotTarget (0, fx::distMix);
        setParam (*b.rig.proc, pid::modSlotDepth (0), 1.0f);
        const double internal = b.run();
        expect (internal > 0.02, "Input source works with no sidechain bus (3 kHz product " + juce::String (internal, 4) + ")");

        // Bypass: no modulation, no envelope.
        {
            Rig rig (48000.0, 128);
            setParam (*rig.proc, fx::distEnable, 1.0f);
            rig.proc->setModSlotTarget (0, fx::distMix);
            setParam (*rig.proc, pid::modSlotDepth (0), 1.0f);
            std::vector<float> l, r;
            juce::AudioBuffer<float> view (rig.buf.getArrayOfWritePointers(), 4, 128);
            for (int b2 = 0; b2 < 20; ++b2)
            {
                for (int ch = 0; ch < 4; ++ch) for (int i = 0; i < 128; ++i) view.setSample (ch, i, ch < 2 ? 0.4f : carrier (b2 * 128 + i, 0.5));
                rig.proc->processBlockBypassed (view, rig.midi);
            }
            expect (rig.proc->getTelemetry().scEnvelope.load() == 0.0f && rig.proc->getTelemetry().modSlotOffset[0].load() == 0.0f, "bypassed: envelope and slot offsets read 0");
        }
    }

    void depthSignAndSumTest()
    {
        std::cout << "depthSignAndSumTest\n";
        const auto run = [] (float baseMix, std::vector<std::pair<float, float>> slots)   // {depth} for slots on fxDist.mix
        {
            DistRig d (0.5f, baseMix);
            int i = 0;
            for (const auto& s : slots)
            {
                d.rig.proc->setModSlotTarget (i, fx::distMix);
                setParam (*d.rig.proc, pid::modSlotDepth (i), s.first);
                ++i;
            }
            return d.run (1.2);
        };
        const double flat  = run (0.5f, {});
        const double plus  = run (0.5f, { { 0.5f, 0.f } });
        const double minus = run (0.5f, { { -0.5f, 0.f } });
        std::cout << "  3 kHz product: base mix 0.5 -> " << flat << ", depth +0.5 -> " << plus << ", depth -0.5 -> " << minus << "\n";
        expect (plus > flat * 1.2 && minus < flat * 0.8, "positive depth raises the target, negative depth lowers it");

        // Two slots on one target add: 0.2 + 0.5 x env + 0.5 x env == 0.2 + 1.0 x env, and clamp at 1.
        const double one = run (0.2f, { { 1.0f, 0.f } });
        const double two = run (0.2f, { { 0.5f, 0.f }, { 0.5f, 0.f } });
        expect (std::abs (one - two) < 1.0e-4 * std::max (1.0, one), "two slots on one target sum (depth 0.5 + 0.5 == depth 1.0): " + juce::String (two, 5) + " vs " + juce::String (one, 5));
        const double over = run (0.8f, { { 1.0f, 0.f }, { 1.0f, 0.f }, { 1.0f, 0.f } });   // 0.8 + 1.5 -> clamped to mix 1.0
        const double full = run (1.0f, {});
        expect (std::abs (over - full) < 1.0e-4 * std::max (1.0, full), "summed offsets clamp at the end of the range (mix 0.8 + 1.5 behaves as mix 1.0): " + juce::String (over, 5) + " vs " + juce::String (full, 5));
        const double neg = run (0.2f, { { -1.0f, 0.f }, { -1.0f, 0.f } });
        const double zero = run (0.0f, {});
        expect (std::abs (neg - zero) < 1.0e-4, "negative sums clamp at the bottom of the range");
    }

    //==========================================================================
    void modTargetApiTest()
    {
        std::cout << "modTargetApiTest\n";
        namespace params = spa::params;
        const auto& targets = Proc::getModTargets();
        const auto& excl = spa::mod::exclusions();

        int floatFx = 0;
        for (const auto& d : params::all())
            if (d.kind == params::ParamKind::floatParam && d.section != params::Section::global
                && d.section != params::Section::sidechain && d.section != params::Section::modMatrix)
                ++floatFx;
        std::cout << "  float FX parameters " << floatFx << ": " << targets.size() << " targets, " << excl.size() << " excluded\n";
        expect ((int) (targets.size() + excl.size()) == floatFx, "targets + exclusions cover exactly the float FX parameters");
        bool noDup = true, hasNames = true, fxOnly = true;
        std::set<juce::String> seen;
        for (const auto& t : targets)
        {
            noDup = seen.insert (t.id).second && noDup;
            hasNames = hasNames && t.displayName.isNotEmpty() && t.owner.isNotEmpty();
            const auto* d = params::find (t.id);
            fxOnly = fxOnly && d != nullptr && d->kind == params::ParamKind::floatParam;
        }
        expect (noDup && hasNames && fxOnly, "targets are unique, named, owned, and all float registry parameters");
        bool noBoolChoice = true;
        for (const auto& t : targets)
        {
            const auto k = params::find (t.id)->kind;
            noBoolChoice = noBoolChoice && k != params::ParamKind::boolParam && k != params::ParamKind::choiceParam;
        }
        expect (noBoolChoice, "no bool or choice parameter is a target");
        expect (excl.size() >= 3 && std::all_of (excl.begin(), excl.end(), [] (const spa::mod::ModExclusion& e) { return e.reason.length() > 40; }), "every exclusion states a reason");

        Proc proc;
        for (const auto* id : { "fxConv.decay", "fxConv.damping", "fxConv.start", "fxReverb.size", "fxReverb.predelay", "fxConv.predelay" })
            expect (! proc.setModSlotTarget (0, id) && proc.getModSlotTarget (0).isEmpty(), juce::String ("excluded target ") + id + " is rejected by the API");
        expect (! proc.setModSlotTarget (0, "no.such.param") && ! proc.setModSlotTarget (0, "fxDist.enable") && ! proc.setModSlotTarget (0, "fxDist.type")
                && ! proc.setModSlotTarget (0, "global.mix") && ! proc.setModSlotTarget (0, "sc.gain") && ! proc.setModSlotTarget (0, "mod.slot1.depth"),
                "unknown, bool, choice, global, sidechain and slot-depth IDs are rejected");
        expect (! proc.setModSlotTarget (-1, fx::distMix) && ! proc.setModSlotTarget (8, fx::distMix) && proc.getModSlotTarget (8).isEmpty(), "out-of-range slots are rejected");
        expect (proc.setModSlotTarget (3, fx::reverbMix) && proc.getModSlotTarget (3) == fx::reverbMix, "a valid target is accepted and read back");
        expect (proc.setModSlotTarget (3, {}) && proc.getModSlotTarget (3).isEmpty(), "an empty ID unassigns");
        expect (! proc.setModSlotTarget (4, "fxConv.decay") && proc.getModSlotTarget (4).isEmpty(), "a rejected set leaves the slot as it was");
        proc.setModSlotTarget (5, fx::delayMix);
        expect (! proc.setModSlotTarget (5, "bogus") && proc.getModSlotTarget (5) == fx::delayMix, "a rejected set leaves an assigned slot untouched");

        // The owning effect names follow the registry's section names.
        const auto owner = [&] (const char* id) { for (const auto& t : targets) if (t.id == id) return t.owner; return juce::String(); };
        expect (owner ("fxEQ.band3.gain") == "FX EQ" && owner (fx::reverbMix) == "FX Reverb" && owner (fx::compMix) == "FX Comp", "targets name their owning effect");
    }

    //==========================================================================
    void modStateTest()
    {
        std::cout << "modStateTest\n";
        constexpr double sr = 48000.0;
        auto a = makeProc (sr, 256, 2, 2, 2);
        a->setModSlotTarget (0, fx::reverbMix);
        a->setModSlotTarget (2, "fxEQ.band4.gain");
        a->setModSlotTarget (7, fx::delayFeedback);
        setParam (*a, pid::modSlotDepth (0), -0.35f);
        setParam (*a, pid::modSlotDepth (7), 0.8f);
        setParam (*a, pid::sc::gain, 6.0f);
        setParam (*a, pid::sc::attack, 33.0f);
        setParam (*a, pid::sc::release, 400.0f);
        setParam (*a, pid::sc::hpf, 150.0f);
        setParam (*a, pid::sc::source, 1.0f);
        setParam (*a, pid::sc::listen, 1.0f);

        juce::MemoryBlock mb;
        a->getStateInformation (mb);
        auto xml = juce::AudioProcessor::getXmlFromBinary (mb.getData(), (int) mb.getSize());
        expect (xml != nullptr && xml->getStringAttribute ("modSlot1Target") == fx::reverbMix && xml->getStringAttribute ("modSlot2Target").isEmpty()
                && xml->getStringAttribute ("modSlot3Target") == "fxEQ.band4.gain", "state XML carries the slot target IDs");

        auto b = makeProc (sr, 256, 2, 2, 2);
        b->setStateInformation (mb.getData(), (int) mb.getSize());
        bool same = true;
        for (int i = 0; i < 8; ++i) same = same && a->getModSlotTarget (i) == b->getModSlotTarget (i);
        expect (same && b->getModSlotTarget (0) == fx::reverbMix && b->getModSlotTarget (7) == fx::delayFeedback && b->getModSlotTarget (1).isEmpty(), "slot targets survive a state round trip");
        bool paramsSame = true;
        for (const auto& d : spa::params::all())
            paramsSame = paramsSame && std::abs (getParam (*a, d.id) - getParam (*b, d.id)) < 1.0e-5f;
        expect (paramsSame && std::abs (getParam (*b, pid::modSlotDepth (0)) + 0.35f) < 1.0e-4f && std::abs (getParam (*b, pid::sc::hpf) - 150.0f) < 0.01f,
                "slot depths and sidechain parameters survive the round trip");
        juce::MemoryBlock mb2;
        b->getStateInformation (mb2);
        expect (mb == mb2, "re-saving the restored processor reproduces the state byte for byte");

        // Unknown, excluded, bool, and garbage IDs become unassigned; the rest load.
        {
            auto tree = juce::ValueTree::fromXml (*xml);
            tree.setProperty ("modSlot1Target", "no.such.parameter", nullptr);
            tree.setProperty ("modSlot2Target", "fxConv.decay", nullptr);
            tree.setProperty ("modSlot4Target", "fxDist.enable", nullptr);
            tree.setProperty ("modSlot8Target", "  ", nullptr);
            juce::MemoryBlock bad;
            juce::AudioProcessor::copyXmlToBinary (*tree.createXml(), bad);
            auto c = makeProc (sr, 256, 2, 2, 2);
            c->setModSlotTarget (1, fx::distMix);   // pre-assigned: must be cleared by the load
            c->setStateInformation (bad.getData(), (int) bad.getSize());
            expect (c->getModSlotTarget (0).isEmpty() && c->getModSlotTarget (1).isEmpty() && c->getModSlotTarget (3).isEmpty() && c->getModSlotTarget (7).isEmpty(),
                    "unknown / excluded / non-float / blank IDs on load become unassigned");
            expect (c->getModSlotTarget (2) == "fxEQ.band4.gain", "valid IDs next to bad ones still load");
        }

        // A state from before the matrix existed (no slot properties) clears the slots.
        {
            auto tree = juce::ValueTree::fromXml (*xml);
            for (int i = 0; i < 8; ++i) tree.removeProperty ("modSlot" + juce::String (i + 1) + "Target", nullptr);
            juce::MemoryBlock old;
            juce::AudioProcessor::copyXmlToBinary (*tree.createXml(), old);
            auto c = makeProc (sr, 256, 2, 2, 2);
            c->setModSlotTarget (0, fx::distMix);
            c->setStateInformation (old.getData(), (int) old.getSize());
            bool none = true;
            for (int i = 0; i < 8; ++i) none = none && c->getModSlotTarget (i).isEmpty();
            expect (none, "a state without slot properties leaves every slot unassigned");
        }

        // Modulating does not leak into what is saved.
        {
            Rig rig (sr, 256);
            auto& p = *rig.proc;
            setParam (p, fx::reverbEnable, 1.0f);
            setParam (p, fx::reverbMix, 0.3f);
            p.setModSlotTarget (0, fx::reverbMix);
            setParam (p, pid::modSlotDepth (0), 0.6f);
            std::vector<float> l, r;
            runRig (rig, 24000, [&] (int ch, long n) { return ch < 2 ? sine (n, 300.0, 0.3) : carrier (n, 0.8); }, &l, &r);
            juce::MemoryBlock m3;
            p.getStateInformation (m3);
            auto x3 = juce::AudioProcessor::getXmlFromBinary (m3.getData(), (int) m3.getSize());
            double saved = -1.0;
            for (auto* c : x3->getChildIterator())
                if (c->getStringAttribute ("id") == fx::reverbMix) saved = c->getDoubleAttribute ("value");
            expect (std::abs (saved - 0.3) < 1.0e-6, "saved reverb mix is the base value 0.3 after modulating it (" + juce::String (saved, 6) + ")");
        }
    }

    //==========================================================================
    void sidechainListenTest()
    {
        std::cout << "sidechainListenTest\n";
        constexpr double sr = 48000.0;
        // External: the output is the sidechain after HPF and gain, not the main signal.
        {
            Rig rig (sr, 128);
            auto& p = *rig.proc;
            setAllEffects (p, true);
            setParam (p, fx::convEnable, 0.0f);
            setParam (p, pid::sc::listen, 1.0f);
            setParam (p, pid::sc::gain, 6.0206f);
            std::vector<float> l, r;
            Noise nz (3);
            runRig (rig, (long) sr, [&] (int ch, long n)
            {
                if (ch < 2) return 0.5f * nz.next();                                 // loud main noise
                return ch == 2 ? sine (n, 1000.0, 0.2) : sine (n, 3000.0, 0.1);      // different L / R sidechain
            }, &l, &r);
            const size_t from = (size_t) (0.5 * sr);
            const double aL = toneAmp (l, from, l.size(), sr, 1000.0), aR = toneAmp (r, from, r.size(), sr, 3000.0);
            // Residual after removing the expected tones.
            double resL = 0.0, resR = 0.0;
            for (size_t i = from; i < l.size(); ++i)
            {
                const double eL = (double) l[i] - 0.4 * std::sin (kTwoPi * 1000.0 * (double) i / sr);
                const double eR = (double) r[i] - 0.2 * std::sin (kTwoPi * 3000.0 * (double) i / sr);
                resL += eL * eL; resR += eR * eR;
            }
            resL = std::sqrt (resL / (double) (l.size() - from)); resR = std::sqrt (resR / (double) (r.size() - from));
            std::cout << "  listen: L 1 kHz amp " << aL << " (expect 0.4), R 3 kHz amp " << aR << " (expect 0.2), residual " << resL << " / " << resR << "\n";
            expect (std::abs (aL - 0.4) < 0.01 && std::abs (aR - 0.2) < 0.005, "sc.listen outputs the sidechain after gain (+6 dB) on the matching channels");
            expect (resL < 0.01 && resR < 0.01, "sc.listen output holds nothing of the (loud, effected) main signal");
        }
        // HPF is in the listen path; listen off returns the main path.
        {
            Rig rig (sr, 128);
            auto& p = *rig.proc;
            setParam (p, pid::sc::listen, 1.0f);
            setParam (p, pid::sc::hpf, 2000.0f);
            std::vector<float> l, r;
            runRig (rig, (long) sr, [&] (int ch, long n) { return ch < 2 ? 0.3f : sine (n, 100.0, 0.5); }, &l, &r);
            expect (rmsRange (l, 24000, l.size()) < 0.01, "sc.listen with HPF 2 kHz removes a 100 Hz sidechain tone (rms " + juce::String (rmsRange (l, 24000, l.size()), 5) + ")");
            setParam (p, pid::sc::listen, 0.0f);
            std::vector<float> l2, r2;
            runRig (rig, (long) sr, [&] (int ch, long n) { return ch < 2 ? sine (n, 440.0, 0.3) : sine (n, 100.0, 0.5); }, &l2, &r2);
            const double a = toneAmp (l2, 24000, l2.size(), sr, 440.0);
            expect (std::abs (a - 0.3) < 0.005, "with listen off the processed (here: transparent) main signal returns (440 Hz amp " + juce::String (a, 4) + ")");
        }
        // Input source: the detector signal is the main input after input gain.
        {
            Rig rig (sr, 128, 2, 2, 0);
            auto& p = *rig.proc;
            setParam (p, pid::sc::source, 1.0f);
            setParam (p, pid::sc::listen, 1.0f);
            setParam (p, pid::inputGain, 6.0206f);
            setParam (p, fx::distEnable, 1.0f);   // processing must not show
            setParam (p, fx::distDrive, 1.0f);
            std::vector<float> l, r;
            runRig (rig, (long) sr, [&] (int, long n) { return sine (n, 1000.0, 0.2); }, &l, &r);
            const double a = toneAmp (l, 24000, l.size(), sr, 1000.0), h3 = toneAmp (l, 24000, l.size(), sr, 3000.0);
            expect (std::abs (a - 0.4) < 0.01 && h3 < 0.002, "Input source listens to the post-input-gain signal (1 kHz amp " + juce::String (a, 3) + "), not the distorted output");
        }
    }

    //==========================================================================
    // mono in -> stereo out with a sidechain: the sidechain's host-buffer
    // channels alias the main OUTPUT channels (channel 1 is both sidechain
    // channel 0 and output channel 1).
    void sidechainBusAliasingTest()
    {
        std::cout << "sidechainBusAliasingTest\n";
        constexpr double sr = 48000.0;
        struct L { int in, out, sc; };
        for (const L lay : { L { 1, 2, 2 }, L { 1, 2, 1 }, L { 1, 1, 1 }, L { 2, 2, 2 } })
        {
            // Prepared block (128) is smaller than the host block (512), so each call is several chunks.
            auto proc = makeProc (sr, 128, lay.in, lay.out, lay.sc);
            const int chans = std::max (proc->getTotalNumInputChannels(), proc->getTotalNumOutputChannels());
            setParam (*proc, pid::sc::listen, 1.0f);
            setParam (*proc, pid::sc::attack, 1.0f);
            juce::AudioBuffer<float> buf (chans, 512);
            juce::MidiBuffer midi;
            Noise nz (9);
            std::vector<float> o0, o1;
            float env = 0.0f;
            for (int b = 0; b < 100; ++b)
            {
                for (int ch = 0; ch < chans; ++ch)
                    for (int i = 0; i < 512; ++i)
                    {
                        const long n = (long) b * 512 + i;
                        float v = 0.0f;
                        if (ch < lay.in) v = 0.9f * nz.next();                             // loud main input noise
                        else if (ch == lay.in) v = sine (n, 1000.0, 0.3);                  // sidechain ch 0
                        else v = sine (n, 2000.0, 0.15);                                   // sidechain ch 1
                        buf.setSample (ch, i, v);
                    }
                proc->processBlock (buf, midi);
                for (int i = 0; i < 512; ++i)
                {
                    o0.push_back (buf.getSample (0, i));
                    if (lay.out > 1) o1.push_back (buf.getSample (1, i));
                }
                env = proc->getTelemetry().scEnvelope.load();
            }
            const juce::String tag = juce::String (lay.in) + "->" + juce::String (lay.out) + " sc " + juce::String (lay.sc) + ": ";
            const size_t from = o0.size() / 2;
            const double a0 = toneAmp (o0, from, o0.size(), sr, 1000.0);
            expect (env > 0.25f && env < 0.32f, tag + "envelope reads the real sidechain (0.3-peak tone), not output-overwritten data (env " + juce::String (env, 3) + ")");
            expect (std::abs (a0 - 0.3) < 0.01, tag + "listen output L is the sidechain ch 0 (1 kHz amp " + juce::String (a0, 3) + ")");
            if (lay.out > 1)
            {
                const double a1 = lay.sc > 1 ? toneAmp (o1, from, o1.size(), sr, 2000.0) : toneAmp (o1, from, o1.size(), sr, 1000.0);
                const double expect1 = lay.sc > 1 ? 0.15 : 0.3;
                expect (std::abs (a1 - expect1) < 0.01, tag + "listen output R is sidechain ch " + juce::String (lay.sc > 1 ? 1 : 0) + " (amp " + juce::String (a1, 3) + ")");
            }
        }
    }

    //==========================================================================
    // The modulation is refreshed at least every 32 host samples: an envelope
    // step part-way through a block reaches the audio within one interval.
    void modRefreshTest()
    {
        std::cout << "modRefreshTest\n";
        constexpr double sr = 48000.0;
        for (int interval : { 32, Proc::kDefaultModUpdateInterval, 1 })
        {
            Rig rig (sr, 256);
            auto& p = *rig.proc;
            setParam (p, fx::distEnable, 1.0f);
            setParam (p, fx::distType, 1.0f);
            setParam (p, fx::distDrive, 1.0f);
            setParam (p, fx::distMix, 0.0f);
            setParam (p, pid::sc::attack, 0.1f);
            p.setModUpdateInterval (interval);
            p.setModSmoothingMs (0.0f);
            p.setModSlotTarget (0, fx::distMix);
            setParam (p, pid::modSlotDepth (0), 1.0f);
            const long stepAt = 5000 + 77;   // not on a chunk or piece boundary
            std::vector<float> l, r;
            runRig (rig, 10000, [&] (int ch, long n) { return ch < 2 ? 0.9f * (float) std::sin (0.07 * (double) n) : (n >= stepAt ? carrier (n, 1.0) : 0.0f); }, &l, &r);
            long firstDev = -1;
            for (long n = 4000; n < 10000; ++n)
                if (std::abs ((double) l[(size_t) n] - 0.9 * std::sin (0.07 * (double) n)) > 1.0e-3) { firstDev = n; break; }
            const long delay = firstDev - stepAt;
            std::cout << "  refresh " << interval << ": first audible change " << delay << " samples after the envelope step\n";
            expect (firstDev >= 0 && delay <= 32 && delay >= -interval, "refresh every " + juce::String (interval) + " host samples: the envelope step reaches the audio within 32 samples (" + juce::String ((int) delay) + ")");
        }
        expect (Proc::kDefaultModUpdateInterval <= 32 && Proc::kDefaultModUpdateInterval >= 1, "the shipped refresh interval is <= 32 host samples");

        // Oversampled: the same bound holds in host samples.
        for (int factorChoice : { 1, 2 })
        {
            Rig rig (sr, 256);
            auto& p = *rig.proc;
            setParam (p, pid::oversampling, (float) factorChoice);
            setParam (p, fx::distEnable, 1.0f);
            setParam (p, fx::distType, 1.0f);
            setParam (p, fx::distDrive, 1.0f);
            setParam (p, fx::distMix, 0.0f);
            setParam (p, pid::sc::attack, 0.1f);
            p.setModSmoothingMs (0.0f);
            p.setModSlotTarget (0, fx::distMix);
            setParam (p, pid::modSlotDepth (0), 1.0f);
            const long stepAt = 6000 + 77;
            std::vector<float> l, r, ref;
            runRig (rig, 12000, [&] (int ch, long n) { return ch < 2 ? 0.9f * (float) std::sin (0.07 * (double) n) : (n >= stepAt ? carrier (n, 1.0) : 0.0f); }, &l, &r);
            // Compare against the same run without modulation (the oversampler's own delay/filtering cancels).
            Rig rig2 (sr, 256);
            setParam (*rig2.proc, pid::oversampling, (float) factorChoice);
            setParam (*rig2.proc, fx::distEnable, 1.0f);
            setParam (*rig2.proc, fx::distType, 1.0f);
            setParam (*rig2.proc, fx::distDrive, 1.0f);
            setParam (*rig2.proc, fx::distMix, 0.0f);
            std::vector<float> l2, r2;
            runRig (rig2, 12000, [&] (int ch, long n) { return ch < 2 ? 0.9f * (float) std::sin (0.07 * (double) n) : 0.0f; }, &l2, &r2);
            long firstDev = -1;
            for (long n = 4000; n < 12000; ++n)
                if (std::abs ((double) l[(size_t) n] - l2[(size_t) n]) > 1.0e-3) { firstDev = n; break; }
            const long delay = firstDev - stepAt;
            const int lat = rig.proc->getLatencySamples();
            expect (firstDev >= 0 && delay >= -8 && delay <= 32 + lat, juce::String (1 << factorChoice) + "x: the envelope step reaches the audio within 32 samples + the oversampler latency (" + juce::String ((int) delay) + " vs " + juce::String (32 + lat) + ")");
        }
    }

    //==========================================================================
    // Every target applies without trouble: finite, host value untouched.
    void modAllTargetsSmokeTest()
    {
        std::cout << "modAllTargetsSmokeTest\n";
        const auto irFile = writeTestIR ("spastrip-smoke-ir", 48000.0, 1.0);
        int bad = 0, changedHost = 0, count = 0;
        for (const auto& t : Proc::getModTargets())
        {
            Rig rig (48000.0, 128);
            auto& p = *rig.proc;
            setParam (p, enableIdFor (t.id), 1.0f);
            if (t.id.startsWith ("fxConv.")) { p.loadConvolutionIR (irFile); p.serviceMessageThread(); }
            const float before = getParam (p, t.id);
            if (! p.setModSlotTarget (3, t.id)) { ++bad; continue; }
            setParam (p, pid::modSlotDepth (3), 1.0f);
            setParam (p, pid::sc::attack, 1.0f);
            std::vector<float> l, r;
            Noise nz (1);
            runRig (rig, 9600, [&] (int ch, long n) { return ch < 2 ? 0.3f * nz.next() : carrier (n, (n / 2400) % 2 == 0 ? 0.9 : 0.05); }, &l, &r);
            if (! finiteVec (l) || ! finiteVec (r)) ++bad;
            if (! juce::exactlyEqual (getParam (p, t.id), before)) ++changedHost;
            ++count;
        }
        expect (bad == 0, "every one of the " + juce::String (count) + " targets can be assigned and runs to finite output with full-depth modulation (" + juce::String (bad) + " bad)");
        expect (changedHost == 0, "no target's host-visible value moved");
        irFile.deleteFile();
    }

    //==========================================================================
    void modulationSoakTest()
    {
        std::cout << "modulationSoakTest\n";
        const auto irFile = writeTestIR ("spastrip-modsoak-ir", 48000.0, 2.0);
        const auto& targets = Proc::getModTargets();
        for (int factorChoice = 0; factorChoice < 3; ++factorChoice)
        {
            for (int mode = 0; mode < 4; ++mode)   // 16, 64, 512, varying
            {
                const int fixed[] = { 16, 64, 512, 0 };
                const int block = fixed[mode];
                const int maxBlock = block == 0 ? 1024 : block;
                Rig rig (48000.0, maxBlock);
                auto& p = *rig.proc;
                setAllEffects (p, true);
                setParam (p, pid::oversampling, (float) factorChoice);
                expect (p.loadConvolutionIR (irFile), "IR loaded for the modulation soak");
                p.serviceMessageThread();
                // 8 slots on varied targets (deterministic spread over the table).
                juce::Random rng (11 + mode + 7 * factorChoice);
                for (int s = 0; s < 8; ++s)
                {
                    p.setModSlotTarget (s, targets[(size_t) ((s * 17 + 5 + mode * 3) % (int) targets.size())].id);
                    setParam (p, pid::modSlotDepth (s), (s % 2 == 0 ? 1.0f : -1.0f) * (0.4f + 0.1f * (float) s));
                }
                setParam (p, pid::sc::attack, 0.5f);
                setParam (p, pid::sc::release, 40.0f);

                const long total = (long) (4.0 * 48000.0);
                Noise nz (21), sn (22);
                bool finite = true;
                float peak = 0.0f;
                long done = 0;
                const int nch = rig.buf.getNumChannels();
                while (done < total)
                {
                    int n = block;
                    if (block == 0)
                    {
                        static const int sizes[] = { 1, 7, 32, 100, 333, 512, 1024, 17, 64, 480, 3 };
                        n = sizes[rng.nextInt (11)];
                    }
                    n = (int) std::min<long> (n, total - done);
                    juce::AudioBuffer<float> buf (nch, n);
                    for (int i = 0; i < n; ++i)
                    {
                        const long g = done + i;
                        const float gate = (g / 6000) % 2 == 0 ? 1.0f : 0.02f;   // bursty sidechain
                        for (int ch = 0; ch < nch; ++ch)
                            buf.setSample (ch, i, ch < 2 ? (g < (long) (3.0 * 48000.0) ? 0.5f * nz.next() : 0.0f) : gate * 0.9f * sn.next());
                    }
                    p.processBlock (buf, rig.midi);
                    for (int ch = 0; ch < 2; ++ch)
                    {
                        finite = finite && allFinite (buf);
                        peak = std::max (peak, buf.getMagnitude (ch, 0, n));
                    }
                    done += n;
                    if ((done / 4800) != ((done - n) / 4800)) p.serviceMessageThread();
                }
                const juce::String tag = juce::String (1 << factorChoice) + "x, block " + (block == 0 ? juce::String ("varying") : juce::String (block)) + ": ";
                expect (finite && peak < 8.0f, tag + "4 s, all effects on, 8 active slots: finite and bounded (peak " + juce::String (peak, 3) + ")");
            }
        }
        irFile.deleteFile();
    }

    //==========================================================================
    // Zipper at the shipped settings, on a steady tone (the audit's worst case).
    void modZipperTest()
    {
        std::cout << "modZipperTest\n";
        const auto irFile = writeTestIR ("spastrip-zipper-ir", 48000.0, 1.5);
        struct C { const char* id; double limitDb; };
        // Largest step re the 0.5 tone, 1 Hz envelope, depth 0.5, shipping defaults.
        for (const C c : { C { "fxEQ.band2.gain", -80.0 }, C { "fxEQ.band2.freq", -65.0 }, C { "fxReverb.mix", -50.0 },
                           C { "fxComp.band1.thresh", -60.0 }, C { "fxDelay.mix", -70.0 }, C { "fxChorus.mix", -65.0 },
                           C { "fxTrem.depth", -70.0 } })
        {
            const auto y = renderTarget (c.id, Proc::kDefaultModUpdateInterval, 0.5f, 48000.0, irFile, 3.0, 1.0, 0.4f, 0, 64, false, -1.0f);
            const double step = stepDbOf (y, 48000);
            std::cout << "  " << juce::String (c.id).paddedRight (' ', 22) << " step " << juce::String (step, 1) << " dB re tone\n";
            expect (step < c.limitDb, juce::String (c.id) + ": no step above " + juce::String (c.limitDb, 0) + " dB re the tone at the shipped refresh (" + juce::String (step, 1) + ")");
        }
        irFile.deleteFile();
    }
}

//==============================================================================
// Task B: phase-coherent dry/wet under oversampling.
namespace phase2Tests
{
    void mixFlatnessOversampledTest()
    {
        std::cout << "mixFlatnessOversampledTest\n";
        constexpr int block = 256;
        double worstOverall = 0.0;
        for (double sr : { 44100.0, 48000.0, 96000.0 })
        for (int factorChoice : { 1, 2 })
        for (bool lookahead : { false, true })
        {
            auto proc = makeProc (sr, block);
            setAllEffects (*proc, false);
            if (lookahead)
            {
                setParam (*proc, fx::limEnable, 1.0f);
                setParam (*proc, fx::limLookahead, 1.0f);
                setParam (*proc, fx::limCeiling, 0.0f);   // 0.5-amplitude tones are never limited
            }
            setParam (*proc, pid::oversampling, (float) factorChoice);
            setParam (*proc, pid::mix, 0.5f);
            juce::AudioBuffer<float> tmp (2, block);
            juce::MidiBuffer midi;
            tmp.clear();
            proc->processBlock (tmp, midi);
            proc->serviceMessageThread();

            double worst = 0.0, worstHz = 0.0;
            for (double hz : { 20.0, 50.0, 100.0, 440.0, 1000.0, 2500.0, 5000.0, 8000.0, 10000.0, 12000.0, 14000.0, 16000.0, 18000.0 })
            {
                std::vector<float> in, out;
                const int total = (int) (sr * 0.7);
                juce::AudioBuffer<float> b (2, block);
                for (int pos = 0; pos < total; pos += block)
                {
                    for (int i = 0; i < block; ++i)
                    {
                        const float v = 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * hz * (double) (pos + i) / sr);
                        b.setSample (0, i, v); b.setSample (1, i, v);
                        in.push_back (v);
                    }
                    proc->processBlock (b, midi);
                    for (int i = 0; i < block; ++i) out.push_back (b.getSample (0, i));
                }
                const size_t from = (size_t) (sr * 0.4);
                const double gainDb = 20.0 * std::log10 (toneAmpCycles (out, from, sr, hz) / toneAmpCycles (in, from, sr, hz));
                if (std::abs (gainDb) > std::abs (worst)) { worst = gainDb; worstHz = hz; }
            }
            worstOverall = std::max (worstOverall, std::abs (worst));
            std::cout << "  " << (int) sr << " Hz, " << (1 << factorChoice) << "x" << (lookahead ? " + limiter lookahead" : "")
                      << ": mix 0.5 worst deviation " << juce::String (worst, 4) << " dB at " << juce::String (worstHz, 0) << " Hz\n";
            expect (std::abs (worst) <= 0.05, juce::String ((int) sr) + " Hz, " + juce::String (1 << factorChoice) + "x" + (lookahead ? " + lookahead" : "")
                    + ": mix 0.5 is flat to 0.05 dB over 20 Hz - 18 kHz (worst " + juce::String (std::abs (worst), 4) + " dB)");
        }
        std::cout << "  worst case over all combinations: " << juce::String (worstOverall, 5) << " dB\n";

        // Dry path keeps running while mix is 1: raising mix later does not click.
        {
            auto proc = makeProc (48000.0, 256);
            setAllEffects (*proc, false);
            setParam (*proc, pid::oversampling, 1.0f);
            setParam (*proc, pid::mix, 1.0f);
            juce::AudioBuffer<float> b (2, 256);
            juce::MidiBuffer midi;
            b.clear(); proc->processBlock (b, midi); proc->serviceMessageThread();
            std::vector<float> out;
            for (int blk = 0; blk < 60; ++blk)
            {
                if (blk == 30) setParam (*proc, pid::mix, 0.5f);
                for (int i = 0; i < 256; ++i)
                {
                    const float v = 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * 1000.0 * (double) (blk * 256 + i) / 48000.0);
                    b.setSample (0, i, v); b.setSample (1, i, v);
                }
                proc->processBlock (b, midi);
                for (int i = 0; i < 256; ++i) out.push_back (b.getSample (0, i));
            }
            double maxStep = 0.0;
            for (size_t i = 6 * 256; i < out.size(); ++i)
                maxStep = std::max (maxStep, (double) std::abs (out[i] - 2.0f * out[i - 1] + out[i - 2]));
            expect (maxStep < 0.01, "changing mix at 2x with the dry oversampler already running: no discontinuity (max d2 " + juce::String (maxStep, 5) + ")");
        }
    }
}

//==============================================================================
// Task C: factory impulse responses.
namespace phase2Tests
{
    struct ManifestEntry { juce::String id, name, category, role, file, sha256; int channels = 0; };

    std::vector<ManifestEntry> readManifest()
    {
        std::vector<ManifestEntry> out;
        const auto json = juce::JSON::parse (juce::File (SPASTRIP_SOURCE_DIR).getChildFile ("assets/irs/manifest.json"));
        if (auto* irs = json.getProperty ("irs", {}).getArray())
            for (const auto& e : *irs)
            {
                ManifestEntry m;
                m.id = e.getProperty ("id", {}).toString();
                m.name = e.getProperty ("display_name", {}).toString();
                m.category = e.getProperty ("category", {}).toString();
                m.role = e.getProperty ("role", {}).toString();
                const auto proc = e.getProperty ("processed", {});
                m.file = proc.getProperty ("file", {}).toString();
                m.sha256 = proc.getProperty ("sha256", {}).toString();
                m.channels = (int) proc.getProperty ("channels", 0);
                out.push_back (m);
            }
        return out;
    }

    // The embedded bytes of a manifest file name, or nullptr.
    const char* embedded (const juce::String& fileName, int& size)
    {
        namespace data = spa_factory_data;
        for (int i = 0; i < data::namedResourceListSize; ++i)
            if (juce::String (data::getNamedResourceOriginalFilename (data::namedResourceList[i])) == fileName)
                return data::getNamedResource (data::namedResourceList[i], size);
        size = 0;
        return nullptr;
    }

    // Impulse on both channels, then the wet L/R of the convolve-only chain at mix 1.
    struct Wet { std::vector<float> l, r; bool loaded = false; };

    Wet factoryWet (Proc& proc, double sr, int block, double seconds)
    {
        setAllEffects (proc, false);
        setParam (proc, fx::convEnable, 1.0f);
        setParam (proc, fx::convMix, 1.0f);
        proc.serviceMessageThread();
        juce::AudioBuffer<float> buf (2, block);
        juce::MidiBuffer midi;
        Wet w;
        // Wait (bounded) for the engine's background IR load: probe with impulses until a response shows.
        for (int attempt = 0; attempt < 400 && ! w.loaded; ++attempt)
        {
            // (Sample 0 of the impulse block is skipped: until the engine has an IR the
            // convolver passes its input through, which would read as "loaded".)
            double e = 0.0;
            for (int b = 0; b < 8; ++b)
            {
                buf.clear();
                if (b == 0) { buf.setSample (0, 0, 1.0f); buf.setSample (1, 0, 1.0f); }
                proc.processBlock (buf, midi);
                for (int c = 0; c < 2; ++c) for (int i = (b == 0 ? 1 : 0); i < block; ++i) e += (double) buf.getSample (c, i) * buf.getSample (c, i);
            }
            w.loaded = e > 1.0e-10;
            if (! w.loaded) juce::Thread::sleep (10);
        }
        if (! w.loaded)
            return w;
        for (int b = 0; b < (int) (6.0 * sr / block); ++b) { buf.clear(); proc.processBlock (buf, midi); }   // let the probes ring out
        const int blocks = (int) (seconds * sr / block);
        for (int b = 0; b < blocks; ++b)
        {
            buf.clear();
            if (b == 0) { buf.setSample (0, 0, 1.0f); buf.setSample (1, 0, 1.0f); }
            proc.processBlock (buf, midi);
            for (int i = 0; i < block; ++i) { w.l.push_back (buf.getSample (0, i)); w.r.push_back (buf.getSample (1, i)); }
        }
        return w;
    }

    void factoryIRTableTest()
    {
        std::cout << "factoryIRTableTest\n";
        const auto manifest = readManifest();
        expect (! manifest.empty(), "assets/irs/manifest.json is readable (" + juce::String ((int) manifest.size()) + " entries)");

        // Expected list: representative entries, categories in order of first appearance.
        std::vector<ManifestEntry> reps;
        for (const auto& m : manifest) if (m.role == "representative") reps.push_back (m);
        std::vector<juce::String> cats;
        for (const auto& m : reps) if (std::find (cats.begin(), cats.end(), m.category) == cats.end()) cats.push_back (m.category);
        std::vector<ManifestEntry> expected;
        for (const auto& c : cats) for (const auto& m : reps) if (m.category == c) expected.push_back (m);

        const auto& list = Proc::getFactoryIRs();
        bool same = list.size() == expected.size();
        for (size_t i = 0; same && i < list.size(); ++i)
            same = list[i].id == expected[i].id && list[i].name == expected[i].name && list[i].category == expected[i].category;
        std::cout << "  " << list.size() << " factory IRs in " << cats.size() << " categories\n";
        expect (same && list.size() == 19, "the factory list (id, display name, category) matches manifest.json: 19 'representative' entries, manifest order grouped by category");

        // Grouping really groups.
        bool grouped = true;
        std::set<juce::String> closed;
        for (size_t i = 0; i < list.size(); ++i)
        {
            if (i > 0 && list[i].category != list[i - 1].category)
                closed.insert (list[i - 1].category);
            grouped = grouped && closed.count (list[i].category) == 0;
        }
        expect (grouped, "each category appears as one contiguous group");

        // The alternate is not shipped: neither listed nor embedded.
        bool altOk = true, altEmbedded = false;
        for (const auto& m : manifest)
            if (m.role != "representative")
            {
                altOk = altOk && Proc::getFactoryIRs().end() == std::find_if (list.begin(), list.end(), [&] (const spa::factory::IR& f) { return f.id == m.id; });
                int sz = 0;
                altEmbedded = altEmbedded || embedded (juce::File (m.file).getFileName(), sz) != nullptr;
            }
        expect (altOk && ! altEmbedded && std::none_of (list.begin(), list.end(), [] (const spa::factory::IR& f) { return f.id == "akir-uni-springer"; }),
                "role 'alternate' (akir-uni-springer) is neither listed nor embedded");

        // Embedded set == representative files + CREDITS.md, byte-identical to the manifest's hashes.
        namespace data = spa_factory_data;
        expect (data::namedResourceListSize == (int) expected.size() + 1, "exactly the 19 FLACs plus CREDITS.md are embedded (" + juce::String (data::namedResourceListSize) + " resources)");
        int hashOk = 0;
        for (const auto& m : expected)
        {
            int sz = 0;
            const char* p = embedded (juce::File (m.file).getFileName(), sz);
            if (p != nullptr && juce::SHA256 (p, (size_t) sz).toHexString() == m.sha256) ++hashOk;
        }
        expect (hashOk == (int) expected.size(), "every embedded FLAC matches its sha256 in the manifest (" + juce::String (hashOk) + "/" + juce::String ((int) expected.size()) + ")");

        const auto credits = Proc::getFactoryIRCredits();
        expect (credits.contains ("Adventure Kid") && credits.contains ("jmuehlhans") && credits.contains ("CC BY 4.0") && credits.length() > 3000, "credits text is embedded and accessible (" + juce::String (credits.length()) + " chars)");
        int sz = 0;
        const char* cp = embedded ("CREDITS.md", sz);
        expect (cp != nullptr && juce::SHA256 (cp, (size_t) sz) == juce::SHA256 (juce::File (SPASTRIP_SOURCE_DIR).getChildFile ("assets/irs/CREDITS.md")), "embedded CREDITS.md equals assets/irs/CREDITS.md");
    }

    void karlskircheAssetTest()
    {
        std::cout << "karlskircheAssetTest\n";
        juce::AudioBuffer<float> ir;
        double rate = 0.0;
        expect (spa::factory::decode ("karlskirche", ir, rate), "karlskirche decodes");
        const int n = ir.getNumSamples();
        const float peak = ir.getMagnitude (0, n);
        int onset = -1;
        for (int i = 0; i < n && onset < 0; ++i)
            if (std::abs (ir.getSample (0, i)) >= 0.01f * peak) onset = i;
        const double dbPeak = 20.0 * std::log10 (peak);
        std::cout << "  karlskirche: " << ir.getNumChannels() << " ch, " << rate << " Hz, " << n << " samples, peak " << dbPeak << " dBFS, onset " << 1000.0 * onset / rate << " ms\n";
        expect (ir.getNumChannels() == 1 && std::abs (rate - 48000.0) < 1.0e-9, "karlskirche: mono, 48 kHz");
        expect (std::abs (dbPeak + 1.0) < 0.02, "karlskirche: peak -1 dBFS");
        expect (onset >= 0 && 1000.0 * onset / rate <= 2.0, "karlskirche: direct sound (first sample within 40 dB of the peak) is within 2 ms of the start (the 67 ms pre-roll is gone)");
        expect (std::abs ((double) n / rate - 5.0926) < 0.001, "karlskirche: duration 5.0926 s (tail untouched)");
    }

    void factoryIRLoadTest()
    {
        std::cout << "factoryIRLoadTest\n";
        const auto manifest = readManifest();
        std::map<juce::String, int> channelsById;
        for (const auto& m : manifest) channelsById[m.id] = m.channels;
        int monoCount = 0;
        for (const auto& f : Proc::getFactoryIRs())
        {
            auto proc = makeProc (44100.0, 512);   // host rate != IR rate: the engine resamples
            expect (! proc->hasConvolutionIR(), f.id + ": no IR before loading");
            const bool ok = proc->loadFactoryIR (f.id);
            const bool tagged = proc->getConvolutionIRSource() == "factory:" + f.id && proc->getConvolutionIRName() == f.name && proc->hasConvolutionIR();
            const auto w = factoryWet (*proc, 44100.0, 512, 1.5);
            bool fin = finiteVec (w.l) && finiteVec (w.r);
            const double eL = rmsRange (w.l, 0, w.l.size()), eR = rmsRange (w.r, 0, w.r.size());
            bool bothWet = eL > 1.0e-4 && eR > 1.0e-4;
            bool lrEqual = true;
            const bool mono = channelsById[f.id] == 1;
            if (mono)
            {
                ++monoCount;
                for (size_t i = 0; i < w.l.size(); ++i) lrEqual = lrEqual && std::abs (w.l[i] - w.r[i]) <= 1.0e-6f * (1.0f + std::abs (w.l[i]));
            }
            std::cout << "  " << f.id.paddedRight (' ', 26) << (mono ? "mono  " : "stereo") << "  wet rms L " << juce::String (eL, 5) << "  R " << juce::String (eR, 5) << "\n";
            expect (ok && tagged && w.loaded && fin && bothWet, f.id + ": loads from the embedded FLAC, finite, non-silent wet output on both channels");
            if (mono)
                expect (lrEqual, f.id + ": mono IR gives the same wet signal on both channels for a centred input");
        }
        expect (monoCount == 5, "five of the shipped IRs are mono (" + juce::String (monoCount) + ")");

        // A mono IR on genuinely stereo input: each channel is convolved on its own.
        {
            auto proc = makeProc (48000.0, 512);
            proc->loadFactoryIR ("karlskirche");
            setAllEffects (*proc, false);
            setParam (*proc, fx::convEnable, 1.0f);
            setParam (*proc, fx::convMix, 1.0f);
            juce::AudioBuffer<float> buf (2, 512);
            juce::MidiBuffer midi;
            bool loaded = false;
            for (int a = 0; a < 400 && ! loaded; ++a)
            {
                buf.clear(); buf.setSample (0, 0, 1.0f); buf.setSample (1, 0, 1.0f);
                proc->processBlock (buf, midi);
                for (int b = 0; b < 4; ++b) { buf.clear(); proc->processBlock (buf, midi); loaded = loaded || buf.getMagnitude (0, 512) > 1.0e-5f; }
                if (! loaded) juce::Thread::sleep (10);
            }
            for (int b = 0; b < 600; ++b) { buf.clear(); proc->processBlock (buf, midi); }
            std::vector<float> l, r;
            for (int b = 0; b < 100; ++b)
            {
                buf.clear();
                if (b == 0) { buf.setSample (0, 0, 1.0f); buf.setSample (1, 200, 1.0f); }   // R is the L impulse 200 samples later
                proc->processBlock (buf, midi);
                for (int i = 0; i < 512; ++i) { l.push_back (buf.getSample (0, i)); r.push_back (buf.getSample (1, i)); }
            }
            double maxDiff = 0.0, mx = 0.0;
            for (size_t i = 0; i + 200 < l.size(); ++i) { maxDiff = std::max (maxDiff, (double) std::abs (r[i + 200] - l[i])); mx = std::max (mx, (double) std::abs (l[i])); }
            expect (loaded && mx > 1.0e-3 && maxDiff < 1.0e-5 * std::max (1.0, mx), "mono IR, stereo input with different timing per channel: R is exactly L delayed by 200 samples (both wet, independently convolved)");
        }

        // loadFactoryIR: unknown id changes nothing; clearing and user files still work.
        {
            auto proc = makeProc (48000.0, 256);
            expect (! proc->loadFactoryIR ("no-such-ir") && proc->getConvolutionIRSource() == "none", "an unknown factory id is rejected and changes nothing");
            expect (proc->loadFactoryIR ("dark-plate") && proc->getConvolutionIRSource() == "factory:dark-plate" && proc->getConvolutionIRName() == "Dark Plate", "loadFactoryIR sets source and display name");
            expect (! proc->loadFactoryIR ("no-such-ir") && proc->getConvolutionIRSource() == "factory:dark-plate", "a rejected load keeps the current factory IR");
            const auto user = writeTestIR ("spastrip-user-ir", 48000.0, 0.5);
            expect (proc->loadConvolutionIR (user) && proc->getConvolutionIRSource() == "embedded", "a user file replaces the factory IR (source 'embedded')");
            user.deleteFile();
            proc->clearConvolutionIR();
            expect (proc->getConvolutionIRSource() == "none" && ! proc->hasConvolutionIR(), "clearConvolutionIR works after a factory IR");
        }
    }

    void factoryIRStateTest()
    {
        std::cout << "factoryIRStateTest\n";
        constexpr double sr = 48000.0;
        constexpr int block = 512;
        auto a = makeProc (sr, block);
        juce::MemoryBlock empty;
        a->getStateInformation (empty);
        a->loadFactoryIR ("wine-tank");   // the largest FLAC (1.2 MB)
        setParam (*a, fx::convMix, 0.6f);
        juce::MemoryBlock withFactory;
        a->getStateInformation (withFactory);

        const auto user = writeTestIR ("spastrip-state-ir", 48000.0, 2.0);
        auto u = makeProc (sr, block);
        u->loadConvolutionIR (user);
        juce::MemoryBlock withUser;
        u->getStateInformation (withUser);
        user.deleteFile();
        std::cout << "  state sizes: no IR " << empty.getSize() << " B, factory IR " << withFactory.getSize() << " B, 2 s user IR " << withUser.getSize() << " B\n";
        expect (withFactory.getSize() <= empty.getSize() + 200, "a factory IR adds almost nothing to the state (" + juce::String ((int) (withFactory.getSize() - empty.getSize())) + " bytes over the empty state)");
        expect (withFactory.getSize() < 12000 && withFactory.getSize() * 3 < withUser.getSize(), "factory state is small: " + juce::String ((int) withFactory.getSize()) + " B (< 12000, and a small fraction of an embedded user IR's " + juce::String ((int) withUser.getSize()) + " B)");
        auto xml = juce::AudioProcessor::getXmlFromBinary (withFactory.getData(), (int) withFactory.getSize());
        expect (xml != nullptr && xml->getStringAttribute ("irSource") == "factory:wine-tank" && xml->getChildByName ("IR") == nullptr
                && xml->getStringAttribute ("convIRName") == "Wine Tank", "state holds only 'factory:<id>' (no IR audio child)");

        auto b = makeProc (sr, block);
        b->setStateInformation (withFactory.getData(), (int) withFactory.getSize());
        expect (b->getConvolutionIRSource() == "factory:wine-tank" && b->getConvolutionIRName() == "Wine Tank" && b->hasConvolutionIR(), "factory IR restores from the id alone (decoded from the embedded FLAC)");
        juce::MemoryBlock again;
        b->getStateInformation (again);
        expect (again == withFactory, "re-saving the restored processor reproduces the state byte for byte");
        expect (std::abs (getParam (*b, fx::convMix) - 0.6f) < 1.0e-5f, "parameters travel with it");

        a->serviceMessageThread(); b->serviceMessageThread();
        const auto ra = convImpulseResponse (*a, sr, block, 2);
        const auto rb = convImpulseResponse (*b, sr, block, 2);
        double maxDiff = 0.0, maxA = 0.0;
        for (size_t i = 0; i < std::min (ra.size(), rb.size()); ++i) { maxDiff = std::max (maxDiff, (double) std::abs (ra[i] - rb[i])); maxA = std::max (maxA, (double) std::abs (ra[i])); }
        expect (! ra.empty() && ! rb.empty() && maxA > 1.0e-3 && maxDiff <= 1.0e-6 * std::max (1.0, maxA), "the restored processor convolves with the identical factory IR");

        // Unknown factory id on restore: no IR, no crash, the source string is preserved
        // (also: a following re-save keeps it, and a later known load still works).
        {
            auto tree = juce::ValueTree::fromXml (*xml);
            tree.setProperty ("irSource", "factory:from-the-future", nullptr);
            tree.setProperty ("convIRName", "Future IR", nullptr);
            juce::MemoryBlock mb;
            juce::AudioProcessor::copyXmlToBinary (*tree.createXml(), mb);
            auto c = makeProc (sr, block);
            c->setStateInformation (mb.getData(), (int) mb.getSize());
            expect (c->getConvolutionIRSource() == "factory:from-the-future" && ! c->hasConvolutionIR() && c->getConvolutionIRName() == "Future IR", "an unknown factory id restores with no IR and the source string preserved");
            juce::MemoryBlock re;
            c->getStateInformation (re);
            auto rx = juce::AudioProcessor::getXmlFromBinary (re.getData(), (int) re.getSize());
            expect (rx != nullptr && rx->getStringAttribute ("irSource") == "factory:from-the-future", "...and survives a re-save");
            juce::AudioBuffer<float> buf (2, block); juce::MidiBuffer midi;
            for (int i = 0; i < 20; ++i) { buf.clear(); c->processBlock (buf, midi); }
            expect (allFinite (buf) && c->loadFactoryIR ("chamber") && c->hasConvolutionIR(), "...processing is fine and a real factory IR can be loaded afterwards");
        }
    }
}
