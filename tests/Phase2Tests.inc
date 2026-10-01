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

    double rmsOf (const std::vector<float>& x, size_t from)
    {
        double e = 0.0;
        for (size_t i = from; i < x.size(); ++i) e += (double) x[i] * x[i];
        return std::sqrt (e / (double) std::max<size_t> ((size_t) 1, x.size() - from));
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

    struct AuditConfig { double envHz = 1.0; bool square = false; };

    AuditResult auditTarget (const juce::String& id, double sr, const juce::File& irFile,
                             const AuditConfig& cfg, float depth = 0.5f, float baseNorm = 0.4f)
    {
        const double seconds = 3.0;
        const size_t from = (size_t) (1.0 * sr);
        const auto run = [&] (int interval, float d, float smooth)
        {
            return renderTarget (id, interval, d, sr, irFile, seconds, cfg.envHz, baseNorm, 0, 64, cfg.square, smooth);
        };
        const auto base = run (32, 0.0f, 0.0f);
        AuditResult r;
        r.stepDbBase = stepDbOf (base, from);
        const auto m32 = run (32, depth, 0.0f);
        r.stepDb32 = stepDbOf (m32, from);
        r.effectDb = diffDb (m32, base, from);
        r.stepDb8  = stepDbOf (run (8, depth, 0.0f), from);
        r.stepDb8s = stepDbOf (run (Proc::kDefaultModUpdateInterval, depth, Proc::kDefaultModSmoothingMs), from);
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
        std::cout << "step = largest discontinuity, dB re the 0.5 tone. Columns: slot off | refresh every 32 | 8 | 8 + 2 ms offset slew (shipping default) | 4 | 1 host samples (slew off unless stated) | effect of the modulation (dB re signal)\n";
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
