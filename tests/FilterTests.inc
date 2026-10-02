// FILTER module tests (SPAStrip-only: the module is a port of SPASynth's filter section as an
// insert effect). Included from SPAStripTests.cpp after Phase3Tests.inc, so `expect`, `Proc`, `pid`,
// `setParam`, `getParam`, `makeProc`, `Noise`, `toneAmpCycles` and the phase2 / phase2b / phase3 helpers
// are in scope.

namespace filterTests
{
    namespace fx = spa::params::id::fx;
    namespace params = spa::params;
    namespace fr = spa::dsp::filterresponse;
    using FX = spa::dsp::FXChain;
    using Module = FX::Module;
    using FT = spa::params::FilterType;

    //==========================================================================
    // Helpers.
    void pump (int ms) { phase2bTests::pump (ms); }

    const char* typeName (int t)
    {
        static const char* names[] { "LP12", "LP24", "HP12", "HP24", "BP12", "BP24", "Notch12", "Notch24" };
        return names[t];
    }

    // FX::Params with only the filter doing anything.
    FX::Params filterParams (int type1, float cutoff1, float res1, float mix1,
                             bool on2 = false, int type2 = 0, float cutoff2 = 20000.0f, float res2 = 0.0f, float mix2 = 1.0f,
                             bool parallel = false)
    {
        FX::Params p;
        p.filterEnable = true;
        p.filterRouting = parallel ? 1 : 0;
        p.filter1Type = type1; p.filter1Cutoff = cutoff1; p.filter1Resonance = res1; p.filter1Mix = mix1;
        p.filter2Enable = on2;
        p.filter2Type = type2; p.filter2Cutoff = cutoff2; p.filter2Resonance = res2; p.filter2Mix = mix2;
        return p;
    }

    fr::Filter asResponseFilter (bool enabled, int type, float cutoff, float res, float mix)
    {
        fr::Filter f;
        f.enabled = enabled; f.type = (FT) type; f.cutoffHz = cutoff; f.resonance = res; f.mix = mix;
        return f;
    }

    // Channel 0 of `total` samples through the chain in 256-sample blocks (both channels carry the signal).
    std::vector<float> render (FX& chain, const FX::Params& p, int total, const std::function<float (int)>& gen)
    {
        std::vector<float> out;
        out.reserve ((size_t) total);
        juce::AudioBuffer<float> buf (2, 256);
        for (int pos = 0; pos < total; pos += 256)
        {
            for (int i = 0; i < 256; ++i) { const float v = gen (pos + i); buf.setSample (0, i, v); buf.setSample (1, i, v); }
            chain.process (buf, p);
            for (int i = 0; i < 256; ++i) out.push_back (buf.getSample (0, i));
        }
        return out;
    }

    // Steady-state gain (dB) of the chain at `hz`, measured with a small sine.
    double measuredDb (FX& chain, const FX::Params& p, double sr, double hz)
    {
        constexpr float amp = 0.05f;
        const int settle = (int) (0.3 * sr);
        const int total = settle + (int) (0.25 * sr) + (int) (sr / hz) + 512;
        chain.reset();
        const auto y = render (chain, p, total, [&] (int i) { return amp * (float) std::sin (juce::MathConstants<double>::twoPi * hz * (double) i / sr); });
        return 20.0 * std::log10 (std::max (1.0e-12, toneAmpCycles (y, (size_t) settle, sr, hz, 0.15) / (double) amp));
    }

    // Pink noise (Paul Kellet's filter), ~0.1 RMS.
    struct Pink
    {
        Noise n { 99 };
        float b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
        float next()
        {
            const float w = n.next();
            b0 = 0.99886f * b0 + w * 0.0555179f;  b1 = 0.99332f * b1 + w * 0.0750759f;
            b2 = 0.96900f * b2 + w * 0.1538520f;  b3 = 0.86650f * b3 + w * 0.3104856f;
            b4 = 0.55000f * b4 + w * 0.5329522f;  b5 = -0.7616f * b5 - w * 0.0168980f;
            const float out = b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362f;
            b6 = w * 0.115926f;
            return out * 0.1f;
        }
    };

    double rmsOf (const float* x, size_t n)
    {
        double e = 0.0;
        for (size_t i = 0; i < n; ++i) e += (double) x[i] * x[i];
        return std::sqrt (e / (double) std::max<size_t> (1, n));
    }

    //==========================================================================
    // All 8 modes against the exact analytic response, at 1x / 2x / 4x engine rates.
    void filterResponseTest()
    {
        std::cout << "filterResponseTest\n";
        FX chain;
        int points = 0, bad = 0;
        double worst = 0.0;
        juce::String worstAt;

        for (const double sr : { 48000.0, 96000.0, 192000.0 })     // 1x, 2x, 4x of 48 kHz
        {
            chain.prepare (sr, 256);
            for (int type = 0; type < 8; ++type)
                for (const float cutoff : { 200.0f, 1000.0f, 5000.0f })
                    for (const float res : { 0.0f, 0.5f, 0.9f })
                    {
                        const auto p = filterParams (type, cutoff, res, 1.0f);
                        for (const double ratio : { 0.2, 0.5, 0.8, 1.0, 1.25, 2.0, 5.0 })
                        {
                            const double hz = (double) cutoff * ratio;
                            if (hz < 30.0 || hz > 0.4 * sr || hz > 20000.0)
                                continue;
                            const double want = fr::toDb (fr::response ((FT) type, cutoff, res, sr, hz));
                            if (want < -40.0)
                                continue;   // a null: a dB error there says nothing about the filter
                            const double got = measuredDb (chain, p, sr, hz);
                            const double err = std::abs (got - want);
                            ++points;
                            if (err > 0.5) ++bad;
                            if (err > worst)
                            {
                                worst = err;
                                worstAt = juce::String (typeName (type)) + " fc " + juce::String (cutoff) + " res " + juce::String (res, 1) + " at " + juce::String (hz, 0) + " Hz, " + juce::String (sr / 48000.0, 0) + "x";
                            }
                        }
                    }
        }
        std::cout << "  worst deviation " << juce::String (worst, 3) << " dB (" << worstAt << ")\n";
        expect (points > 1200 && bad == 0, "measured magnitude is within +/-0.5 dB of the exact TPT-SVF response: " + juce::String (points) + " points (8 modes x 3 cutoffs x 3 resonances x up to 7 frequencies x 1x/2x/4x), " + juce::String (bad) + " outside");

        // The 24 dB modes really are the square of the 12 dB ones, and the cutoff clamp matches the DSP.
        const double h12 = fr::toDb (fr::response (FT::lp12, 1000.0, 0.0, 48000.0, 4000.0));
        const double h24 = fr::toDb (fr::response (FT::lp24, 1000.0, 0.0, 48000.0, 4000.0));
        expect (std::abs (h24 - 2.0 * h12) < 1.0e-9, "analytic: a 24 dB mode is exactly twice the dB of its 12 dB mode");
        expect (std::abs (fr::toDb (fr::response (FT::lp12, 1000.0, 0.0, 48000.0, 1000.0)) - fr::toDb (1.0 / std::complex<double> (0.0, 2.0))) < 1.0e-9,
                "analytic: LP at its cutoff with resonance 0 is 1/k = -6.02 dB");
    }

    // Series vs parallel vs single, mix honoured, against the exact module response.
    void filterRoutingTest()
    {
        std::cout << "filterRoutingTest\n";
        FX chain;
        struct Cfg { const char* name; bool on1, on2, parallel; int t1, t2; float c1, c2, r1, r2, m1, m2; };
        const Cfg cfgs[] {
            { "f1 only, LP24",                     true,  false, false, 1, 0, 1000.0f, 20000.0f, 0.6f, 0.0f, 0.8f, 1.0f },
            { "f1 only, parallel switch set",      true,  false, true,  5, 0, 800.0f,  20000.0f, 0.5f, 0.0f, 1.0f, 1.0f },
            { "f2 only, series",                   false, true,  false, 0, 3, 20000.0f, 400.0f,  0.0f, 0.4f, 1.0f, 0.7f },
            { "f2 only, parallel (0.5 x (1 + f2))",false, true,  true,  0, 3, 20000.0f, 400.0f,  0.0f, 0.4f, 1.0f, 0.7f },
            { "series LP24 -> HP12",               true,  true,  false, 1, 2, 3000.0f, 300.0f,  0.4f, 0.3f, 0.8f, 0.7f },
            { "parallel LP24 + HP12",              true,  true,  true,  1, 2, 3000.0f, 300.0f,  0.4f, 0.3f, 0.8f, 0.7f },
            { "series BP12 -> Notch24",            true,  true,  false, 4, 7, 1500.0f, 1500.0f, 0.7f, 0.6f, 1.0f, 1.0f },
            { "parallel BP12 + Notch24",           true,  true,  true,  4, 7, 1500.0f, 1500.0f, 0.7f, 0.6f, 1.0f, 1.0f },
            { "series HP24 -> LP12, half mixes",   true,  true,  false, 3, 0, 200.0f,  6000.0f, 0.2f, 0.8f, 0.5f, 0.5f },
        };
        int points = 0, bad = 0;
        double worst = 0.0;
        for (const double sr : { 48000.0, 192000.0 })
        {
            chain.prepare (sr, 256);
            for (const auto& c : cfgs)
            {
                const auto p = filterParams (c.t1, c.c1, c.r1, c.m1, c.on2, c.t2, c.c2, c.r2, c.m2, c.parallel);
                // filter 1 off but filter 2 on needs filterEnable false
                auto pp = p; pp.filterEnable = c.on1;
                const auto f1 = asResponseFilter (c.on1, c.t1, c.c1, c.r1, c.m1);
                const auto f2 = asResponseFilter (c.on2, c.t2, c.c2, c.r2, c.m2);
                int cfgBad = 0;
                for (const double hz : { 60.0, 150.0, 300.0, 600.0, 1000.0, 1800.0, 3000.0, 6000.0, 10000.0 })
                {
                    if (hz > 0.4 * sr)
                        continue;
                    const double want = fr::toDb (fr::module (f1, f2, c.parallel, sr, hz));
                    if (want < -40.0)
                        continue;
                    const double err = std::abs (measuredDb (chain, pp, sr, hz) - want);
                    ++points; worst = std::max (worst, err);
                    if (err > 0.5) { ++bad; ++cfgBad; }
                }
                if (cfgBad > 0)
                    std::cout << "    deviates: " << c.name << " at " << sr << " Hz\n";
            }
        }
        std::cout << "  worst deviation " << juce::String (worst, 3) << " dB over " << points << " points\n";
        expect (bad == 0, "series, parallel, filter-1-only and filter-2-only (mixes honoured) match the exact module response within 0.5 dB (" + juce::String (points) + " points, " + juce::String (bad) + " outside)");

        // Behavioural: series multiplies, parallel averages.
        const auto series = filterParams (0, 1000.0f, 0.0f, 1.0f, true, 0, 1000.0f, 0.0f, 1.0f, false);
        const auto parallel = filterParams (0, 1000.0f, 0.0f, 1.0f, true, 0, 1000.0f, 0.0f, 1.0f, true);
        const auto single = filterParams (0, 1000.0f, 0.0f, 1.0f);
        chain.prepare (48000.0, 256);
        const double dbSingle = measuredDb (chain, single, 48000.0, 4000.0);
        const double dbSeries = measuredDb (chain, series, 48000.0, 4000.0);
        const double dbParallel = measuredDb (chain, parallel, 48000.0, 4000.0);
        expect (std::abs (dbSeries - 2.0 * dbSingle) < 0.5, "two equal LP12 in series: twice the dB of one (" + juce::String (dbSeries, 2) + " vs " + juce::String (dbSingle, 2) + ")");
        expect (std::abs (dbParallel - dbSingle) < 0.5, "two equal LP12 in parallel: the same as one (the average of identical branches)");

        // Filter 1 off, filter 2 on in PARALLEL keeps the synth's behaviour: half of the dry plus half of filter 2.
        auto f2OnlyParallel = filterParams (0, 1000.0f, 0.0f, 1.0f, true, 0, 1000.0f, 0.0f, 1.0f, true);
        f2OnlyParallel.filterEnable = false;
        const double dbHi = measuredDb (chain, f2OnlyParallel, 48000.0, 12000.0);
        expect (dbHi > -6.5 && dbHi < -5.0, "filter 1 off + filter 2 LP in parallel: far above the cutoff only the dry half remains (-6 dB; got " + juce::String (dbHi, 2) + ")");
    }

    //==========================================================================
    // MIX 0 is a bit-exact wire; a disabled module is invisible to the rest of the chain.
    void filterTransparencyTest()
    {
        std::cout << "filterTransparencyTest\n";
        // Test signal: noise plus the awkward values.
        std::vector<float> in;
        {
            Noise nz (17);
            in = { -0.0f, 0.0f, 1.0e-40f, -1.0e-40f, 1.0f, -1.0f, 0.5f };
            for (int i = 0; i < 6000; ++i) in.push_back (0.8f * nz.next());
            while (in.size() % 256 != 0) in.push_back (0.0f);
        }
        const auto gen = [&] (int i) { return in[(size_t) i]; };
        const int total = (int) in.size();
        const auto sameBits = [&] (const std::vector<float>& a) -> bool
        {
            for (size_t i = 0; i < a.size(); ++i)
                if (std::memcmp (&a[i], &in[i], sizeof (float)) != 0)
                    return false;
            return true;
        };

        FX chain;
        bool allExact = true;
        int combos = 0;
        for (const double sr : { 48000.0, 192000.0 })
        {
            chain.prepare (sr, 256);
            for (int parallel = 0; parallel < 2; ++parallel)
                for (int enables = 1; enables <= 3; ++enables)   // f1 only, f2 only, both
                    for (const int type : { 1, 4, 7 })
                    {
                        auto p = filterParams (type, 700.0f, 0.8f, 0.0f, true, type, 2000.0f, 0.6f, 0.0f, parallel == 1);
                        p.filterEnable = (enables & 1) != 0;
                        p.filter2Enable = (enables & 2) != 0;
                        p.filter1Drive = 0.7f; p.filter2Drive = 0.5f;
                        chain.reset();
                        const auto y = render (chain, p, total, gen);
                        allExact = allExact && sameBits (y);
                        ++combos;
                    }
        }
        expect (allExact, "MIX 0 on every enabled filter is a bit-identical wire (" + juce::String (combos) + " combinations: series / parallel, f1 / f2 / both, LP24 / BP12 / Notch24, drive on, 48 kHz and 192 kHz)");

        // Disabled: with every other module busy, the filter's parameters (extreme, but off) change nothing, wherever it sits.
        auto busy = [] (FX::Params& p)
        {
            p.distEnable = true; p.distDrive = 0.6f;
            p.eqEnable = true; p.eqBands[2].enabled = true; p.eqBands[2].gainDb = 6.0f; p.eqBands[2].freq = 800.0f;
            p.chorusEnable = true; p.delayEnable = true; p.delayMix = 0.3f; p.delaySync = false; p.delayTimeMs = 40.0f;
        };
        FX::Params plain; busy (plain);
        FX::Params extreme = plain;
        extreme.filter1Type = 5; extreme.filter1Cutoff = 40.0f; extreme.filter1Resonance = 1.0f; extreme.filter1Drive = 1.0f; extreme.filter1Mix = 0.5f;
        extreme.filter2Type = 3; extreme.filter2Cutoff = 15000.0f; extreme.filter2Resonance = 1.0f; extreme.filter2Mix = 0.25f; extreme.filterRouting = 1;
        FX::Params movedToEnd = extreme;   // the same, with the (disabled) filter module moved to the last slot
        {
            FX::Module ord[FX::numModules];
            for (int i = 0; i < FX::numModules; ++i) ord[i] = plain.order[i];
            std::stable_partition (std::begin (ord), std::end (ord), [] (Module m) { return m != Module::filter; });
            for (int i = 0; i < FX::numModules; ++i) movedToEnd.order[i] = ord[i];
        }
        FX a, b, c;
        for (auto* ch : { &a, &b, &c }) ch->prepare (48000.0, 256);
        const auto ya = render (a, plain, total, gen);
        const auto yb = render (b, extreme, total, gen);
        const auto yc = render (c, movedToEnd, total, gen);
        bool bitsB = true, bitsC = true;
        for (size_t i = 0; i < ya.size(); ++i)
        {
            bitsB = bitsB && std::memcmp (&ya[i], &yb[i], sizeof (float)) == 0;
            bitsC = bitsC && std::memcmp (&ya[i], &yc[i], sizeof (float)) == 0;
        }
        expect (bitsB && bitsC, "a disabled FILTER (extreme settings, first or last in the chain) leaves the busy chain's output bit-identical");

        // And the chain with nothing on is the identity (the processor-level passthrough tests cover the same end to end).
        FX d; d.prepare (48000.0, 256);
        FX::Params none;
        expect (sameBits (render (d, none, total, gen)), "FX chain with everything off, FILTER included, is a bit-identical wire");
    }

    //==========================================================================
    // Switching a filter back on starts it from clear state.
    void filterEnableEdgeTest()
    {
        std::cout << "filterEnableEdgeTest\n";
        for (int which = 0; which < 2; ++which)    // 0: filter 1, 1: filter 2
        {
            FX chain;
            chain.prepare (48000.0, 256);
            auto p = which == 0 ? filterParams (5, 600.0f, 0.95f, 1.0f)
                                : filterParams (0, 20000.0f, 0.0f, 1.0f, true, 5, 600.0f, 0.95f, 1.0f);
            if (which == 1) p.filterEnable = false;
            Noise nz (3);
            const auto loud = [&] (int) { return 0.7f * nz.next(); };
            const auto silence = [] (int) { return 0.0f; };

            // Control: left running, the resonant filter keeps ringing into silence.
            render (chain, p, 4096, loud);
            const auto ringing = render (chain, p, 512, silence);
            float ringPeak = 0.0f; for (float v : ringing) ringPeak = std::max (ringPeak, std::abs (v));
            expect (ringPeak > 1.0e-3f, "filter " + juce::String (which + 1) + ": (control) left on, the resonance rings into silence (peak " + juce::String (ringPeak, 4) + ")");

            // Off for a few blocks with state full of signal, then on again over silence: exactly nothing comes out.
            chain.reset();
            render (chain, p, 4096, loud);
            auto off = p; off.filterEnable = false; off.filter2Enable = false;
            render (chain, off, 1024, loud);
            const auto afterEdge = render (chain, p, 2048, silence);
            bool allZero = true; for (float v : afterEdge) allZero = allZero && v == 0.0f;
            expect (allZero, "filter " + juce::String (which + 1) + ": re-enabled over silence the output is exactly zero (stale state was cleared on the enable edge)");

            // The other filter's state is not touched by this one's edge.
            if (which == 1)
            {
                auto both = filterParams (5, 500.0f, 0.95f, 1.0f, true, 5, 600.0f, 0.95f, 1.0f);
                chain.reset();
                render (chain, both, 4096, loud);
                auto f2off = both; f2off.filter2Enable = false;
                render (chain, f2off, 256, silence);    // f2 off for one block, f1 keeps ringing
                const auto ring1 = render (chain, f2off, 256, silence);
                float pk = 0.0f; for (float v : ring1) pk = std::max (pk, std::abs (v));
                expect (pk > 1.0e-4f, "filter 1 keeps its state while only filter 2 is switched off (peak " + juce::String (pk, 5) + ")");
            }
        }
    }

    //==========================================================================
    // Cutoff changes arrive in whole-call steps; the glide must make a big step smoother.
    void filterGlideTest()
    {
        std::cout << "filterGlideTest\n";
        // A 1.2 kHz tone through an LP12 whose cutoff jumps 400 Hz <-> 6 kHz every 512-sample block (automation).
        const auto worstStep = [] (bool glide)
        {
            spa::dsp::FXChain::filterGlideDisabledForAudit().store (! glide);
            FX chain;
            chain.prepare (48000.0, 512);
            std::vector<float> y;
            juce::AudioBuffer<float> buf (2, 512);
            for (int blk = 0; blk < 80; ++blk)
            {
                for (int i = 0; i < 512; ++i)
                {
                    const float v = 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * 1200.0 * (double) (blk * 512 + i) / 48000.0);
                    buf.setSample (0, i, v); buf.setSample (1, i, v);
                }
                chain.process (buf, filterParams (0, blk % 2 == 0 ? 400.0f : 6000.0f, 0.3f, 1.0f));
                for (int i = 0; i < 512; ++i) y.push_back (buf.getSample (0, i));
            }
            spa::dsp::FXChain::filterGlideDisabledForAudit().store (false);
            double pk = 0.0;
            for (size_t i = 2048 + 4; i < y.size(); ++i)
                pk = std::max (pk, std::abs ((double) y[i] - 4.0 * y[i - 1] + 6.0 * y[i - 2] - 4.0 * y[i - 3] + y[i - 4]));
            return 20.0 * std::log10 (pk / 6.0 / 0.5);
        };
        const double with = worstStep (true), without = worstStep (false);
        std::cout << "  largest step re the tone: glide " << juce::String (with, 1) << " dB, plain per-call coefficients " << juce::String (without, 1) << " dB\n";
        expect (with < without - 6.0, "gliding the coefficients over the call lowers the largest step by more than 6 dB (" + juce::String (with, 1) + " vs " + juce::String (without, 1) + " dB)");
    }

    //==========================================================================
    // Resonance 1.0, drive 1.0, 24 dB, full-depth fast cutoff modulation on loud input: finite and bounded.
    void filterStabilityTest()
    {
        std::cout << "filterStabilityTest\n";
        struct Combo { const char* name; int t1, t2; bool parallel; };
        const Combo combos[] {
            { "LP24 -> LP24",       1, 1, false }, { "BP24 -> BP24",       5, 5, false },
            { "BP24 -> LP24",       5, 1, false }, { "HP24 -> BP24",       3, 5, false },
            { "LP24 + BP24 (par)",  1, 5, true  }, { "Notch24 -> BP24",    7, 5, false },
        };
        double overallPeak = 0.0;
        juce::String overallAt;
        bool allFinite = true;
        int runs = 0;

        for (const int factorChoice : { 0, 1, 2 })
            for (const auto& c : combos)
            {
                phase2Tests::Rig rig (48000.0, 64);
                auto& proc = *rig.proc;
                setParam (proc, pid::oversampling, (float) factorChoice);
                setParam (proc, fx::filterEnable, 1.0f);
                setParam (proc, fx::filter2Enable, 1.0f);
                setParam (proc, fx::filterRouting, c.parallel ? 1.0f : 0.0f);
                for (const bool second : { false, true })
                {
                    setParam (proc, second ? fx::filter2Type : fx::filter1Type, (float) (second ? c.t2 : c.t1));
                    setParam (proc, second ? fx::filter2Res : fx::filter1Res, 1.0f);
                    setParam (proc, second ? fx::filter2Drive : fx::filter1Drive, 1.0f);
                    setParam (proc, second ? fx::filter2Cutoff : fx::filter1Cutoff, second ? 900.0f : 300.0f);
                }
                setParam (proc, pid::sc::attack, 0.1f);
                setParam (proc, pid::sc::release, 5.0f);
                setParam (proc, pid::sc::gain, 3.0103f);
                proc.setModSlotTarget (0, fx::filter1Cutoff);  setParam (proc, pid::modSlotDepth (0), 1.0f);
                proc.setModSlotTarget (1, fx::filter2Cutoff);  setParam (proc, pid::modSlotDepth (1), -1.0f);
                proc.setModSlotTarget (2, fx::filter1Res);     setParam (proc, pid::modSlotDepth (2), -1.0f);
                proc.setModSlotTarget (3, fx::filter2Drive);   setParam (proc, pid::modSlotDepth (3), 1.0f);

                Noise nz ((uint32_t) (21 + factorChoice));
                double peak = 0.0;
                const long total = 48000L * 3;
                for (long done = 0; done < total; done += 64)
                {
                    for (int i = 0; i < 64; ++i)
                    {
                        const double t = (double) (done + i) / 48000.0;
                        // Loud: a hot sine plus full-scale-ish noise (the sum can clip the input; that is the point).
                        const float x = 0.6f * (float) std::sin (juce::MathConstants<double>::twoPi * 110.0 * t) + 0.5f * nz.next();
                        // Fast square-ish sidechain envelope, 35 Hz, full 0..1 swings.
                        const float amp = std::fmod (t * 35.0, 1.0) < 0.5 ? 1.0f : 0.0f;
                        const float car = amp * (float) std::sin (juce::MathConstants<double>::twoPi * 12000.0 * t + juce::MathConstants<double>::pi * 0.25);
                        rig.buf.setSample (0, i, x); rig.buf.setSample (1, i, x);
                        rig.buf.setSample (2, i, car); rig.buf.setSample (3, i, car);
                    }
                    proc.processBlock (rig.buf, rig.midi);
                    for (int ch = 0; ch < 2; ++ch)
                        for (int i = 0; i < 64; ++i)
                        {
                            const float v = rig.buf.getSample (ch, i);
                            allFinite = allFinite && std::isfinite (v);
                            peak = std::max (peak, (double) std::abs (v));
                        }
                }
                ++runs;
                if (peak > overallPeak) { overallPeak = peak; overallAt = juce::String (c.name) + " at " + juce::String (1 << factorChoice) + "x"; }
            }
        std::cout << "  peak output over " << runs << " runs: " << juce::String (overallPeak, 2) << " (" << overallAt << ")\n";
        expect (allFinite, "res 1.0 / drive 1.0 / 24 dB / full-depth fast cutoff modulation on loud input at 1x, 2x, 4x: no NaN or Inf in " + juce::String (runs) + " runs");
        expect (overallPeak < 12.0, "...and the output stays bounded (peak " + juce::String (overallPeak, 2) + ", limit 12)");

        // The static worst case: a full-scale sine exactly at the cutoff of resonance-1.0, drive-1.0 24 dB filters.
        // The last filter's drive stage clamps its input to +/-0.5 -- a square-ish wave whose fundamental is 4/pi x 0.5 --
        // so (steady state) its output cannot exceed 0.64 x its peak gain (1/k per stage = 10, squared for 24 dB = 100)
        // = 64, whatever comes before it.
        double staticPeak = 0.0;
        bool staticFinite = true;
        for (const double sr : { 48000.0, 96000.0, 192000.0 })
            for (const int type : { 1, 5 })
                for (const bool both : { false, true })
                {
                    FX chain; chain.prepare (sr, 256);
                    auto p = filterParams (type, 1000.0f, 1.0f, 1.0f, both, type, 1000.0f, 1.0f, 1.0f, false);
                    p.filter1Drive = 1.0f; p.filter2Drive = 1.0f;
                    const auto y = render (chain, p, (int) (1.5 * sr), [&] (int i) { return (float) std::sin (juce::MathConstants<double>::twoPi * 1000.0 * (double) i / sr); });
                    for (float v : y) { staticFinite = staticFinite && std::isfinite (v); staticPeak = std::max (staticPeak, (double) std::abs (v)); }
                }
        std::cout << "  static worst case (full-scale sine at the cutoff, res 1, drive 1, 24 dB, 1 or 2 filters): peak " << juce::String (staticPeak, 2) << "\n";
        expect (staticFinite && staticPeak < 66.0, "...and a full-scale sine at the cutoff stays finite and at the 0.64 x 100 = 64 steady-state limit set by the drive stage (peak " + juce::String (staticPeak, 2) + ", limit 66)");
    }

    //==========================================================================
    // Randomize All: guards, bound, lock behaviour.
    void guardRulesTest()
    {
        std::cout << "filterGuardRulesTest\n";
        namespace g = params::filterguard;
        juce::Random rng (7);
        int violations = 0, notIdempotent = 0, changedGood = 0, trials = 20000;
        const auto kind = [] (int t) { return t / 2; };
        for (int trial = 0; trial < trials; ++trial)
        {
            params::FilterSettings s;
            s.parallel = rng.nextBool();
            for (auto* one : { &s.f1, &s.f2 })
            {
                one->enabled = rng.nextFloat() < 0.8f;
                one->type = rng.nextInt (8);
                one->cutoffHz = 20.0f * std::pow (1000.0f, rng.nextFloat());
                one->resonance = rng.nextFloat(); one->drive = rng.nextFloat(); one->mix = rng.nextFloat();
            }
            const auto before = s;
            params::guardFilterSettings (s);
            auto again = s; params::guardFilterSettings (again);
            const auto differs = [] (float a, float b) { return ! juce::exactlyEqual (a, b); };
            if (differs (again.f1.cutoffHz, s.f1.cutoffHz) || differs (again.f2.cutoffHz, s.f2.cutoffHz) || differs (again.f1.mix, s.f1.mix) || differs (again.f2.mix, s.f2.mix)
                || differs (again.f1.resonance, s.f1.resonance) || differs (again.f2.resonance, s.f2.resonance))
                ++notIdempotent;

            bool bad = false;
            for (auto* one : { &s.f1, &s.f2 })
            {
                const int k = kind (one->type);
                if (k == 0 && one->cutoffHz < g::lowpassMinHz - 0.01f) bad = true;
                if (k == 1 && one->cutoffHz > g::highpassMaxHz + 0.01f) bad = true;
                if ((k == 2 || k == 3) && (one->cutoffHz < g::bandMinHz - 0.01f || one->cutoffHz > g::bandMaxHz + 0.01f)) bad = true;
                if (k == 2 && one->mix > g::bandpassMaxMix + 1.0e-6f) bad = true;
                if (one->drive > g::driveResonanceThreshold && one->resonance > g::resonanceCapAtHighDrive + 1.0e-6f) bad = true;
                if (one->cutoffHz < 20.0f - 0.01f || one->cutoffHz > 20000.01f) bad = true;
            }
            if (s.f1.enabled && s.f2.enabled && ! s.parallel)
            {
                // The passbands (LP: 20..fc, HP: fc..20k, BP: fc/2..2fc) overlap by >= 1.5 octaves.
                const auto band = [&] (const params::FilterSettings::One& o) -> std::pair<float, float>
                {
                    const int k = kind (o.type);
                    return k == 0 ? std::make_pair (20.0f, o.cutoffHz) : k == 1 ? std::make_pair (o.cutoffHz, 20000.0f)
                         : k == 2 ? std::make_pair (o.cutoffHz * 0.5f, o.cutoffHz * 2.0f) : std::make_pair (20.0f, 20000.0f);
                };
                const auto a = band (s.f1), b = band (s.f2);
                const float overlap = std::log2 (std::min (a.second, b.second) / std::max (a.first, b.first));
                const bool bothNotch = kind (s.f1.type) == 3 || kind (s.f2.type) == 3;
                if (! bothNotch && overlap < g::seriesOverlapOctaves - 0.01f) bad = true;
            }
            violations += bad ? 1 : 0;
            // A setting that already satisfies every rule is left alone.
            if (juce::exactlyEqual (before.f1.cutoffHz, s.f1.cutoffHz) && juce::exactlyEqual (before.f2.cutoffHz, s.f2.cutoffHz)) ++changedGood;
        }
        expect (violations == 0, "guardFilterSettings: " + juce::String (trials) + " random settings all satisfy every rule afterwards (LP >= 300 Hz, HP <= 1.5 kHz, band centre 150..6000 Hz, BP mix <= 0.7, resonance <= 0.5 above drive 0.4, series passbands overlap >= 1.5 octaves), " + juce::String (violations) + " violations");
        expect (notIdempotent == 0, "...and applying the guards twice changes nothing more");
        expect (changedGood > trials / 20, "...and settings that already satisfy the rules mostly pass through untouched (" + juce::String (changedGood) + " of " + juce::String (trials) + " had both cutoffs unchanged)");
    }

    struct RollStats { double worstDb = 1.0e9, bestDb = -1.0e9; int worstSeed = 0; float worstWild = 0.0f; bool finite = true; int below = 0; int count = 0; };

    // `seeds` rolls with ONLY the filter unlocked, each followed by pink noise through the plugin. Returns the
    // level of the output re the input (dB, RMS over the last 0.5 s of 0.8 s).
    RollStats rollStats (int seeds, bool guards, double boundDb, const std::vector<float>& wilds)
    {
        auto proc = makeProc (48000.0, 512);
        for (int m = 0; m < FX::numModules; ++m)
            proc->setLocked ((Module) m, m != (int) Module::filter);
        Pink pinkGen;
        std::vector<float> pink (48000 * 8 / 10);
        for (auto& v : pink) v = pinkGen.next();
        const size_t from = pink.size() - 24000;
        const double inRms = rmsOf (pink.data() + from, pink.size() - from);

        RollStats st;
        juce::AudioBuffer<float> buf (2, 512);
        juce::MidiBuffer midi;
        for (int seed = 1; seed <= seeds; ++seed)
        {
            const float wild = wilds[(size_t) (seed - 1) % wilds.size()];
            proc->setRandomWildness (wild);
            juce::Random rng (seed);
            if (guards)
                proc->randomizeAll (rng);
            else
            {
                auto order = proc->getFxOrder();
                params::randomizeAll (proc->getAPVTS(), wild, proc->getFxLockMask(), order, rng, false);
                proc->setFxOrder (order);
            }

            std::vector<float> out;
            out.reserve (pink.size());
            for (size_t pos = 0; pos < pink.size(); pos += 512)
            {
                const size_t n = std::min<size_t> (512, pink.size() - pos);
                for (size_t i = 0; i < 512; ++i)
                {
                    const float v = i < n ? pink[pos + i] : 0.0f;
                    buf.setSample (0, (int) i, v); buf.setSample (1, (int) i, v);
                }
                proc->processBlock (buf, midi);
                for (size_t i = 0; i < n; ++i) out.push_back (buf.getSample (0, (int) i));
            }
            for (float v : out) st.finite = st.finite && std::isfinite (v);
            const double db = 20.0 * std::log10 (std::max (1.0e-9, rmsOf (out.data() + from, out.size() - from) / inRms));
            ++st.count;
            if (db < st.worstDb) { st.worstDb = db; st.worstSeed = seed; st.worstWild = wild; }
            st.bestDb = std::max (st.bestDb, db);
            if (db < boundDb) ++st.below;
        }
        return st;
    }

    void rollGuardTest()
    {
        std::cout << "filterRollGuardTest\n";
        constexpr double boundDb = -12.0;
        constexpr int seeds = 1000;
        const std::vector<float> wilds { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };   // 200 rolls each

        const auto off = rollStats (seeds, false, boundDb, wilds);
        const auto on = rollStats (seeds, true, boundDb, wilds);
        std::cout << "  pink noise through the module, level re input over " << seeds << " seeded rolls (WILD cycling 0 / .25 / .5 / .75 / 1):\n"
                  << "    unguarded: worst " << juce::String (off.worstDb, 1) << " dB (seed " << off.worstSeed << ", WILD " << off.worstWild << "), best " << juce::String (off.bestDb, 1) << " dB; "
                  << off.below << " of " << seeds << " rolls below " << boundDb << " dB\n"
                  << "    guarded:   worst " << juce::String (on.worstDb, 1) << " dB (seed " << on.worstSeed << ", WILD " << on.worstWild << "), best " << juce::String (on.bestDb, 1) << " dB; "
                  << on.below << " of " << seeds << " rolls below " << boundDb << " dB\n";
        expect (on.finite && off.finite, "no NaN / Inf from any of the " + juce::String (2 * seeds) + " rolls");
        expect (on.below == 0, "guarded: every one of 1000 rolls keeps pink noise within " + juce::String (boundDb, 0) + " dB of the input (worst " + juce::String (on.worstDb, 1) + " dB)");
        expect (off.below > 0, "(non-vacuous) without the guards the same seeds do breach " + juce::String (boundDb, 0) + " dB: " + juce::String (off.below) + " of " + juce::String (seeds) + " rolls, worst " + juce::String (off.worstDb, 1) + " dB");
        expect (on.bestDb < 12.0, "...and a guarded roll is never ruinously loud either (best " + juce::String (on.bestDb, 1) + " dB re input)");
    }

    void lockAndSlotTest()
    {
        std::cout << "filterLockTest\n";
        const auto idsOfFilter = [] { std::vector<juce::String> ids; for (const auto& d : params::all()) if (d.section == params::Section::fxFilter) ids.push_back (d.id); return ids; };
        const auto filterIds = idsOfFilter();
        expect (filterIds.size() == 13, "the FILTER module owns 13 parameters");

        auto proc = makeProc (48000.0, 256);
        juce::Array<int> order;
        for (int id : { 3, 1, 11, 5, 9, 0, 2, 10, 4, 6, 8, 7 }) order.add (id);   // filter in slot 2, limiter last
        proc->setFxOrder (order);
        setParam (*proc, fx::filterEnable, 1.0f);
        setParam (*proc, fx::filter1Type, 5.0f);
        setParam (*proc, fx::filter1Cutoff, 777.0f);
        setParam (*proc, fx::filter2Enable, 1.0f);
        setParam (*proc, fx::filter2Res, 0.66f);
        proc->setLocked (Module::filter, true);
        proc->setRandomWildness (1.0f);
        const auto snapshot = [&] { std::vector<float> v; for (const auto& id : filterIds) v.push_back (proc->getAPVTS().getParameter (id)->getValue()); return v; };
        const auto before = snapshot();
        bool unchanged = true, slotKept = true;
        for (int seed = 1; seed <= 100; ++seed)
        {
            juce::Random rng (seed);
            proc->randomizeAll (rng);
            unchanged = unchanged && snapshot() == before;
            slotKept = slotKept && proc->getFxOrder().indexOf ((int) Module::filter) == 2 && proc->getFxOrder().getLast() == (int) Module::limiter;
        }
        expect (unchanged, "FILTER locked: all 13 filter parameters are bit-identical after 100 seeded rolls (guards leave a locked module alone)");
        expect (slotKept, "FILTER locked: keeps its chain slot (2) across the 100 rolls, limiter still last");

        // Unlocked: it is rolled (moves) and can move in the chain.
        proc->setLocked (Module::filter, false);
        std::set<int> slots; bool moved = false;
        for (int seed = 1; seed <= 100; ++seed)
        {
            juce::Random rng (seed);
            proc->randomizeAll (rng);
            slots.insert (proc->getFxOrder().indexOf ((int) Module::filter));
            moved = moved || snapshot() != before;
        }
        expect (moved && slots.size() > 6, "FILTER unlocked: rolled, and shuffled across the chain (" + juce::String ((int) slots.size()) + " different slots in 100 rolls)");
        expect (! slots.count (11), "...never into the limiter's slot (the last)");
        expect (params::moduleForSection (params::Section::fxFilter) == Module::filter && params::moduleName (Module::filter) == "FILTER", "section -> module mapping and lock label");
    }

    //==========================================================================
    void modTargetsTest()
    {
        std::cout << "filterModTargetsTest\n";
        const char* ids[] { fx::filter1Cutoff, fx::filter1Res, fx::filter1Drive, fx::filter1Mix,
                            fx::filter2Cutoff, fx::filter2Res, fx::filter2Drive, fx::filter2Mix };
        bool all = true;
        for (auto* id : ids) all = all && spa::mod::indexOf (id) >= 0;
        expect (all, "cutoff, resonance, drive and mix of both filters are mod-slot targets");
        expect (spa::mod::indexOf (fx::filter1Type) < 0 && spa::mod::indexOf (fx::filterEnable) < 0 && spa::mod::indexOf (fx::filterRouting) < 0,
                "type, routing and the enables are not (not float parameters)");
        bool noneExcluded = true;
        for (const auto& e : spa::mod::exclusions())
            noneExcluded = noneExcluded && ! juce::String (e.id).startsWith ("fxFilter.");
        expect (noneExcluded, "no filter parameter is on the audited-exclusion list");

        // A slot on the cutoff really moves the sound.
        const auto run = [] (float depth)
        {
            phase2Tests::Rig rig (48000.0, 64);
            auto& proc = *rig.proc;
            setParam (proc, fx::filterEnable, 1.0f);
            setParam (proc, fx::filter1Type, 1.0f);
            setParam (proc, fx::filter1Cutoff, 300.0f);
            setParam (proc, pid::sc::attack, 0.1f);
            proc.setModSlotTarget (0, fx::filter1Cutoff);
            setParam (proc, pid::modSlotDepth (0), depth);
            Noise nz (5);
            double energy = 0.0;
            for (int blk = 0; blk < 300; ++blk)
            {
                for (int i = 0; i < 64; ++i)
                {
                    const float x = 0.3f * nz.next();
                    rig.buf.setSample (0, i, x); rig.buf.setSample (1, i, x);
                    rig.buf.setSample (2, i, 0.8f * (float) std::sin (juce::MathConstants<double>::twoPi * 12000.0 * (double) (blk * 64 + i) / 48000.0));
                    rig.buf.setSample (3, i, rig.buf.getSample (2, i));
                }
                proc.processBlock (rig.buf, rig.midi);
                if (blk >= 150) for (int i = 0; i < 64; ++i) energy += (double) rig.buf.getSample (0, i) * rig.buf.getSample (0, i);
            }
            return energy;
        };
        const double flat = run (0.0f), opened = run (0.6f);
        expect (opened > 4.0 * flat, "an envelope on filter 1 cutoff opens the LP24 (output energy " + juce::String (10.0 * std::log10 (opened / flat), 1) + " dB up)");
    }

    void stateRoundTripTest()
    {
        std::cout << "filterStateTest\n";
        auto a = makeProc (48000.0, 256);
        setParam (*a, fx::filterEnable, 1.0f);
        setParam (*a, fx::filterRouting, 1.0f);
        setParam (*a, fx::filter1Type, 6.0f);
        setParam (*a, fx::filter1Cutoff, 1234.5f);
        setParam (*a, fx::filter1Res, 0.42f);
        setParam (*a, fx::filter1Drive, 0.33f);
        setParam (*a, fx::filter1Mix, 0.61f);
        setParam (*a, fx::filter2Enable, 1.0f);
        setParam (*a, fx::filter2Type, 3.0f);
        setParam (*a, fx::filter2Cutoff, 87.0f);
        setParam (*a, fx::filter2Res, 0.9f);
        setParam (*a, fx::filter2Drive, 0.5f);
        setParam (*a, fx::filter2Mix, 0.25f);
        a->setModSlotTarget (2, fx::filter2Cutoff);
        juce::MemoryBlock mb; a->getStateInformation (mb);
        auto b = makeProc (48000.0, 256);
        b->setStateInformation (mb.getData(), (int) mb.getSize());
        bool same = true;
        for (const auto& d : params::all())
            if (d.section == params::Section::fxFilter)
                same = same && std::abs (b->getAPVTS().getParameter (d.id)->getValue() - a->getAPVTS().getParameter (d.id)->getValue()) < 1.0e-6f;
        expect (same, "all 13 filter parameters survive getStateInformation / setStateInformation");
        expect (b->getModSlotTarget (2) == fx::filter2Cutoff, "a mod slot on a filter parameter survives too");
        juce::MemoryBlock mb2; b->getStateInformation (mb2);
        expect (mb == mb2, "...and re-saving the restored state reproduces it byte for byte");

        // Defaults: off, wide open, transparent.
        auto c = makeProc (48000.0, 256);
        expect (getParam (*c, fx::filterEnable) == 0.0f && getParam (*c, fx::filter2Enable) == 0.0f, "FILTER defaults to OFF (both filters)");
        expect (getParam (*c, fx::filter1Cutoff) == 20000.0f && getParam (*c, fx::filter1Res) == 0.0f && getParam (*c, fx::filter1Drive) == 0.0f
                    && getParam (*c, fx::filter1Mix) == 1.0f && getParam (*c, fx::filter1Type) == 0.0f && getParam (*c, fx::filterRouting) == 0.0f,
                "filter 1 defaults: LP 12, 20 kHz, res 0, drive 0, mix 100 %, Series");
        expect (c->getFxOrder()[0] == (int) Module::filter, "FILTER is the first module of the default chain");
    }

    //==========================================================================
    // The tab.
    void filterPanelUiTest()
    {
        std::cout << "filterPanelUiTest\n";
        phase3Tests::ForceLive live;
        auto proc = makeProc (48000.0, 256);
        auto ed = phase3Tests::openEditor (*proc);
        auto& content = ed->getContent();
        auto& tabs = content.getFxTabs();

        expect (tabs.getTabNames()[0] == "FILTER" && tabs.getModuleNames().indexOf ("FILTER") == (int) Module::filter, "the FILTER tab exists, first in the default order, module id 11");
        content.selectTabForModule ((int) Module::filter);
        auto* panel = dynamic_cast<spa::ui::FilterPanel*> (tabs.getCurrentContentComponent());
        expect (panel != nullptr, "selecting it shows the FilterPanel");
        if (panel == nullptr)
            return;

        // Header: two titled toggles, bound to the two enables.
        auto& header = panel->getHeader();
        expect (header.numParts() == 2 && header.getTitle (0) == "FILTER 1" && header.getTitle (1) == "FILTER 2", "the header reads FILTER 1 / FILTER 2");
        header.getToggle (0).button.setToggleState (true, juce::sendNotificationSync);
        header.getToggle (1).button.setToggleState (true, juce::sendNotificationSync);
        expect (getParam (*proc, fx::filterEnable) == 1.0f && getParam (*proc, fx::filter2Enable) == 1.0f && header.isOn (0) && header.isOn (1), "the header toggles drive filter 1 / filter 2 enable");
        spa::ui::FilterDisplay& display = panel->getDisplay();
        const auto f1 = display.readFilter (0), f2 = display.readFilter (1);
        expect (f1.enabled && f2.enabled && std::abs (f1.cutoffHz - 20000.0) < 1.0, "the display reads both filters from the parameters");

        // Mod reach arcs work on the filter knobs like anywhere else.
        content.toggleModAssignment (fx::filter1Cutoff, 3);
        content.getModViz().pollNow();
        auto* cutoffKnob = panel->getKnob (0, "cutoff");
        expect (cutoffKnob != nullptr && (bool) cutoffKnob->slider.getProperties().getWithDefault ("modAssigned", false)
                    && std::abs ((double) cutoffKnob->slider.getProperties().getWithDefault ("modRangePos", 0.0) - 0.5) < 0.01,
                "assigning a slot to filter 1 cutoff shows the reach arc on its knob");
        auto* res2 = panel->getKnob (1, "res");
        expect (res2 != nullptr && ! (bool) res2->slider.getProperties().getWithDefault ("modAssigned", false), "...and not on the other knobs");
        content.toggleModAssignment (fx::filter1Cutoff, 3);

        // The lock on the tab is the module's lock.
        content.setLockedFromTab ("FILTER");
        expect (proc->isLocked (Module::filter) && tabs.isTabLocked ("FILTER"), "the FILTER tab's padlock locks the module");
        content.setLockedFromTab ("FILTER");
        // Bold when engaged follows either enable.
        setParam (*proc, fx::filterEnable, 0.0f); setParam (*proc, fx::filter2Enable, 0.0f);
        pump (30);
        expect (! tabs.isTabEngaged ("FILTER"), "FILTER tab is not engaged with both filters off");
        setParam (*proc, fx::filter2Enable, 1.0f);
        pump (30);
        expect (tabs.isTabEngaged ("FILTER"), "...and is engaged with filter 2 alone on");
    }
}
