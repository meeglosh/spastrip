// FX module tests ported from SPASynth's tests/FxModuleTests.inc: COMP versus
// the SPAGlitch reference, GRAIN behaviour, chain-order (un)packing including
// the legacy nine-module path. Bodies are unchanged except that the processor
// type is spa::SPAStripProcessor. NOT ported (synth/UI-only): the snapshot and
// layout reports, RANDOMIZE ALL tests (no randomizer in SPAStrip yet), the
// 1.0.29->1.0.30 grain-spread state migration (deliberately not carried over),
// and the eleven-tab layout test.
// Included from SPAStripTests.cpp, where `expect` is in scope.

namespace fxModuleTests
{
    namespace id = spa::params::id;
    using FX = spa::dsp::FXChain;

    struct Noise
    {
        uint32_t st;
        explicit Noise (uint32_t seed) : st (seed) {}
        float next() { st = st * 1664525u + 1013904223u; return ((float) (st >> 9) / (float) (1u << 23)) * 2.0f - 1.0f; }
    };

    // Goertzel magnitude of channel 0 at hz.
    double tone (const std::vector<float>& x, double sr, double hz)
    {
        double re = 0, im = 0;
        for (size_t i = 0; i < x.size(); ++i)
        {
            const double a = juce::MathConstants<double>::twoPi * hz * (double) i / sr;
            re += x[i] * std::cos (a); im += x[i] * std::sin (a);
        }
        return std::sqrt (re * re + im * im) / (double) x.size();
    }

    // Runs `blocks` blocks of 256 through the chain; `gen(i)` gives sample i.
    std::vector<float> runChain (FX& fx, const FX::Params& p, int totalSamples,
                                 const std::function<float (int)>& gen, int skip = 0)
    {
        std::vector<float> out;
        juce::AudioBuffer<float> buf (2, 256);
        for (int pos = 0; pos < totalSamples; pos += 256)
        {
            for (int i = 0; i < 256; ++i) { const float v = gen (pos + i); buf.setSample (0, i, v); buf.setSample (1, i, v); }
            fx.process (buf, p);
            if (pos >= skip)
                for (int i = 0; i < 256; ++i) out.push_back (buf.getSample (0, i));
        }
        return out;
    }

    FX::Params grainParams()
    {
        FX::Params p;
        p.grainEnable = true; p.grainMix = 1.0f; p.grainSpread = 0.0f; p.grainSpreadPitch = 0.0f; p.grainReverse = 0.0f;
        p.grainSizeMs = 100.0f; p.grainDensityHz = 40.0f; p.grainPositionMs = 500.0f; p.grainPitch = 0.0f;
        p.grainFeedback = 0.0f;
        return p;
    }

    // Per-second GRAIN statistics (channel 0 spectrum of the first 32768
    // samples of each second): peak, spectral centroid, share of energy
    // above 8 kHz. Input is a noise + tone mix for `inputSeconds`, then silence.
    struct SecStat { float peak = 0.0f; double centroid = 0.0, hf = 0.0, hfAbs = 0.0, rms = 0.0; };
    struct GrainRun { std::vector<SecStat> sec; bool finite = true; float peak = 0.0f; };

    GrainRun grainRun (const FX::Params& p, double seconds, double inputSeconds, double sr = 48000.0)
    {
        FX fx; fx.prepare (sr, 256);
        GrainRun run;
        const int nSec = (int) seconds;
        run.sec.resize ((size_t) nSec);
        constexpr int fftOrder = 15, fftN = 1 << fftOrder;
        juce::dsp::FFT fft (fftOrder);
        std::vector<float> cap ((size_t) fftN * 2, 0.0f);
        Noise nz (77);
        juce::AudioBuffer<float> buf (2, 256);
        const long total = (long) (seconds * sr);
        const long inEnd = (long) (inputSeconds * sr);
        int curSec = -1; int capN = 0; double secE = 0.0; long secN = 0;
        const auto flushSec = [&]
        {
            if (curSec < 0) return;
            auto& st = run.sec[(size_t) curSec];
            st.rms = std::sqrt (secE / (double) juce::jmax (1L, secN));
            if (capN >= fftN)
            {
                for (int i = 0; i < fftN; ++i)
                    cap[(size_t) i] *= 0.5f - 0.5f * (float) std::cos (6.2831853 * i / fftN);
                fft.performFrequencyOnlyForwardTransform (cap.data());
                double tot = 0, wsum = 0, hf = 0;
                for (int k = 1; k < fftN / 2; ++k)
                {
                    const double f = (double) k * sr / fftN, m = (double) cap[(size_t) k] * cap[(size_t) k];
                    tot += m; wsum += f * m; if (f > 8000.0) hf += m;
                }
                st.centroid = wsum / juce::jmax (1.0e-30, tot);
                st.hf = hf / juce::jmax (1.0e-30, tot);
                st.hfAbs = hf / (double) fftN;
            }
        };
        for (long pos = 0; pos < total; pos += 256)
        {
            for (int i = 0; i < 256; ++i)
            {
                const long n = pos + i;
                const float v = n < inEnd ? 0.25f * nz.next() + 0.25f * (float) std::sin (6.2831853 * 330.0 * (double) n / sr) : 0.0f;
                buf.setSample (0, i, v); buf.setSample (1, i, v);
            }
            fx.process (buf, p);
            for (int i = 0; i < 256; ++i)
            {
                const long n = pos + i;
                const int sIdx = (int) (n / (long) sr);
                if (sIdx >= nSec) break;
                if (sIdx != curSec) { flushSec(); curSec = sIdx; capN = 0; secE = 0.0; secN = 0; }
                const float a = buf.getSample (0, i), b = buf.getSample (1, i);
                if (! std::isfinite (a) || ! std::isfinite (b)) { run.finite = false; continue; }
                const float m = std::max (std::abs (a), std::abs (b));
                auto& st = run.sec[(size_t) curSec];
                st.peak = std::max (st.peak, m); run.peak = std::max (run.peak, m);
                secE += (double) a * a; ++secN;
                if (capN < fftN) cap[(size_t) capN++] = a;
            }
        }
        flushSec();
        return run;
    }


    // ------------------------------------------------------------------ tests
    // ------------------------------------------------------------------ tests
    void compMatchesSPAGlitchTest()
    {
        std::cout << "compMatchesSPAGlitchTest\n";
        constexpr double sr = 48000.0;
        spa::dsp::Multiband a; glitch::fx::Multiband b;
        a.prepare (sr, 512); b.prepare (sr, 512);
        spa::dsp::Multiband::Params pa; glitch::fx::Multiband::Params pb;
        pa.enable = pb.enable = true; pa.mix = pb.mix = 0.8f;
        pa.crossoverLowHz = pb.crossoverLowHz = 150.0f; pa.crossoverHighHz = pb.crossoverHighHz = 3500.0f;
        for (int k = 0; k < 3; ++k)
        {
            pa.bands[(size_t) k] = { -30.0f + 4.0f * (float) k, 5.0f, 2.5f, 8.0f + 5.0f * (float) k, 90.0f, 2.0f - (float) k };
            pb.bands[(size_t) k] = { -30.0f + 4.0f * (float) k, 5.0f, 2.5f, 8.0f + 5.0f * (float) k, 90.0f, 2.0f - (float) k };
        }
        Noise nz (99);
        double maxDiff = 0.0; float maxMeterDiff = 0.0f;
        juce::AudioBuffer<float> x (2, 512), y (2, 512);
        for (int blk = 0; blk < 80; ++blk)
        {
            const float level = blk < 40 ? 0.6f : 0.02f;   // loud then quiet: both curve halves
            for (int i = 0; i < 512; ++i)
            {
                const float l = level * (nz.next() * 0.5f + 0.5f * std::sin (0.05f * (float) (blk * 512 + i)));
                const float r = level * nz.next();
                x.setSample (0, i, l); x.setSample (1, i, r); y.setSample (0, i, l); y.setSample (1, i, r);
            }
            a.process (x, pa); b.process (y, pb);
            for (int c = 0; c < 2; ++c) for (int i = 0; i < 512; ++i)
                maxDiff = std::max (maxDiff, (double) std::abs (x.getSample (c, i) - y.getSample (c, i)));
            for (int k = 0; k < 3; ++k) maxMeterDiff = std::max (maxMeterDiff, std::abs (a.bandGainDb (k) - b.bandGainDb (k)));
        }
        expect (maxDiff == 0.0, "COMP output is bit-identical to SPAGlitch's reference (max diff " + juce::String (maxDiff, 12) + ")");
        expect (maxMeterDiff == 0.0f, "COMP band meters match the reference");
        // And it actually compresses (not a vacuous pass on a pass-through).
        juce::AudioBuffer<float> loud (2, 512);
        for (int blk = 0; blk < 30; ++blk) { for (int i = 0; i < 512; ++i) { loud.setSample (0, i, 0.8f * nz.next()); loud.setSample (1, i, 0.8f * nz.next()); } a.process (loud, pa); }
        expect (a.bandGainDb (1) < -3.0f, "COMP mid band reduces gain on loud noise (" + juce::String (a.bandGainDb (1), 1) + " dB)");
    }

    void fxNewModulesOffBypassTest()
    {
        std::cout << "fxNewModulesOffBypassTest\n";
        FX fx; fx.prepare (48000.0, 256);
        FX::Params p;   // everything off, incl. COMP and GRAIN
        Noise nz (5);
        std::vector<float> in;
        const auto out = runChain (fx, p, 2048, [&] (int) { const float v = nz.next() * 0.5f; in.push_back (v); return v; });
        bool same = out.size() == in.size();
        for (size_t i = 0; same && i < in.size(); ++i) same = juce::exactlyEqual (out[i], in[i]);
        expect (same, "COMP + GRAIN off: chain output bit-identical to input");

        // A factory-style chain (reverb+delay+chorus) renders identically whether the
        // disabled new modules sit at their defaults or at the very front of the order.
        const auto render = [&] (bool reorder)
        {
            FX f; f.prepare (48000.0, 256);
            FX::Params q; q.reverbEnable = q.delayEnable = q.chorusEnable = true;
            if (reorder)
            {
                const FX::Module o[FX::numModules] { FX::Module::comp, FX::Module::grain, FX::Module::distortion, FX::Module::chorus,
                    FX::Module::mod, FX::Module::tremVib, FX::Module::delay, FX::Module::reverb, FX::Module::convolve,
                    FX::Module::eq, FX::Module::limiter };
                for (int i = 0; i < FX::numModules; ++i) q.order[i] = o[i];
            }
            Noise n2 (11);
            return runChain (f, q, 8192, [&] (int i) { return 0.4f * n2.next() * (i < 2000 ? 1.0f : 0.0f); });
        };
        expect (render (false) == render (true), "disabled COMP/GRAIN are transparent wherever they sit in the order");
    }

    void grainBasicsTest()
    {
        std::cout << "grainBasicsTest\n";
        constexpr double sr = 48000.0;
        // Energy: grains of a tone appear (input ends; wet persists from the ring).
        {
            FX fx; fx.prepare (sr, 256);
            auto p = grainParams();
            const auto out = runChain (fx, p, 48000 * 2, [&] (int i) { return i < 48000 ? 0.5f * (float) std::sin (6.2831853 * 440.0 * i / sr) : 0.0f; }, 48000 + 512);
            double e = 0; for (float v : out) e += v * v;
            expect (e / (double) out.size() > 1.0e-4, "GRAIN produces grains from the ring after the input stops");
            expect (fx.grain().grainsSpawned() > 40, "GRAIN spawns grains (" + juce::String ((int) fx.grain().grainsSpawned()) + ")");
        }
        // FREEZE: input stops writing and the held buffer keeps granulating.
        {
            FX fx; fx.prepare (sr, 256);
            auto p = grainParams();
            runChain (fx, p, 48000, [&] (int i) { return 0.5f * (float) std::sin (6.2831853 * 440.0 * i / sr); });
            p.grainFreeze = true;
            const auto before = fx.grain().ringPeak();
            const auto out = runChain (fx, p, 48000, [&] (int i) { return 0.5f * (float) std::sin (6.2831853 * 3000.0 * i / sr); }, 4096);
            double e = 0; for (float v : out) e += v * v;
            expect (e / (double) out.size() > 1.0e-4, "FREEZE keeps granulating with the input now different");
            expect (juce::exactlyEqual (fx.grain().ringPeak(), before), "FREEZE stops writing the ring");
            expect (tone (out, sr, 440.0) > 4.0 * tone (out, sr, 3000.0), "frozen output holds the OLD content (440 Hz), not the new input");
            expect (fx.grain().isFrozen(), "freeze flag reported");
        }
        // FEEDBACK at max over 30 s stays bounded and finite.
        {
            FX fx; fx.prepare (sr, 256);
            auto p = grainParams();
            p.grainFeedback = 0.9f; p.grainDensityHz = 100.0f; p.grainSizeMs = 500.0f; p.grainPositionMs = 50.0f; p.grainMix = 1.0f; p.grainSpread = 0.5f;
            Noise nz (3);
            float peak = 0; bool finite = true;
            juce::AudioBuffer<float> buf (2, 256);
            for (int b = 0; b < 30 * 48000 / 256; ++b)
            {
                for (int i = 0; i < 256; ++i) { const float v = b < 200 ? 0.9f * nz.next() : 0.0f; buf.setSample (0, i, v); buf.setSample (1, i, v); }
                fx.process (buf, p);
                for (int i = 0; i < 256; ++i) { const float v = buf.getSample (0, i); finite = finite && std::isfinite (v); peak = std::max (peak, std::abs (v)); }
            }
            expect (finite && peak <= 2.0f + 1.0e-3f, "max FEEDBACK stays bounded over 30 s (peak " + juce::String (peak, 3) + ")");
            expect (fx.grain().ringPeak() < 3.0f, "ring content bounded (" + juce::String (fx.grain().ringPeak(), 3) + ")");
        }
        // PITCH +12 doubles the frequency of the grains.
        {
            FX fx; fx.prepare (sr, 256);
            auto p = grainParams(); p.grainPitch = 12.0f; p.grainSizeMs = 80.0f; p.grainPositionMs = 700.0f;
            const auto out = runChain (fx, p, 48000 * 2, [&] (int i) { return 0.5f * (float) std::sin (6.2831853 * 1000.0 * i / sr); }, 48000);
            const double t1 = tone (out, sr, 1000.0), t2 = tone (out, sr, 2000.0);
            expect (t2 > 5.0 * t1, "PITCH +12 moves 1 kHz to 2 kHz (2k " + juce::String (t2, 4) + " vs 1k " + juce::String (t1, 4) + ")");
        }
        // SYNC: grain rate follows tempo.
        {
            const auto spawns = [&] (double bpm)
            {
                FX fx; fx.prepare (sr, 256);
                auto p = grainParams(); p.grainSync = true; p.grainDivision = 9; p.bpm = bpm;   // 1/8
                runChain (fx, p, 48000 * 4, [] (int) { return 0.1f; });
                return (int) fx.grain().grainsSpawned();
            };
            const int a = spawns (120.0), b = spawns (240.0);
            expect (std::abs (a - 16) <= 1 && std::abs (b - 32) <= 1, "SYNC 1/8: 16 grains in 4 s at 120 bpm (" + juce::String (a) + "), 32 at 240 (" + juce::String (b) + ")");
        }
        // Oversampled rates.
        for (double rate : { 96000.0, 192000.0, 384000.0 })
        {
            FX fx; fx.prepare (rate, 512);
            auto p = grainParams(); p.grainFeedback = 0.5f;
            Noise nz (8);
            const auto out = runChain (fx, p, (int) rate, [&] (int) { return 0.3f * nz.next(); }, 4096);
            double e = 0; bool ok = true; for (float v : out) { e += v * v; ok = ok && std::isfinite (v); }
            expect (ok && e > 0.0, "GRAIN runs at " + juce::String ((int) rate) + " Hz");
            expect (fx.grain().ringLength() >= (int) (8.0 * rate), "ring holds 8 s at " + juce::String ((int) rate) + " Hz");
        }
        // Disabling clears the ring (no replay of stale audio on re-enable).
        {
            FX fx; fx.prepare (sr, 256);
            auto p = grainParams();
            runChain (fx, p, 24000, [] (int) { return 0.5f; });
            p.grainEnable = false; runChain (fx, p, 256, [] (int) { return 0.0f; });
            expect (fx.grain().ringPeak() == 0.0f, "disabling GRAIN forgets the ring");
        }
    }

    void grainFreezeCaptureTest()
    {
        std::cout << "grainFreezeCaptureTest\n";
        constexpr double sr = 48000.0;
        const auto sine = [&] (double hz) { return [=] (int i) { return 0.5f * (float) std::sin (6.2831853 * hz * i / sr); }; };
        const auto energy = [] (const std::vector<float>& v, size_t a, size_t b)
        {
            double e = 0; for (size_t i = a; i < b && i < v.size(); ++i) e += (double) v[i] * v[i];
            return e / (double) std::max<size_t> (1, b - a);
        };
        // FREEZE already on, ring empty, audio arrives: captures, then sounds, then holds.
        {
            FX fx; fx.prepare (sr, 256);
            auto p = grainParams(); p.grainFreeze = true;
            const auto out = runChain (fx, p, 48000 * 3, sine (440.0));
            expect (fx.grain().isCapturing() == false && fx.grain().isFrozen(), "freeze holds once enough audio is captured");
            expect (energy (out, 48000, 48000 * 3) > 1.0e-4, "FREEZE on from the start becomes audible within the capture window");
            const auto held = fx.grain().ringPeak();
            const auto out2 = runChain (fx, p, 48000, sine (3000.0), 4096);
            expect (juce::exactlyEqual (fx.grain().ringPeak(), held), "ring stops changing once held");
            expect (tone (out2, sr, 440.0) > 4.0 * tone (out2, sr, 3000.0), "held content is the captured one");
        }
        // Silent input while capturing keeps capturing; audio arriving later still works.
        {
            FX fx; fx.prepare (sr, 256);
            auto p = grainParams(); p.grainFreeze = true;
            runChain (fx, p, 48000 * 2, [] (int) { return 0.0f; });
            expect (fx.grain().isCapturing() && ! fx.grain().isFrozen(), "silent input keeps capturing (no silent hold)");
            const auto out = runChain (fx, p, 48000 * 3, sine (440.0));
            expect (energy (out, 48000, 48000 * 3) > 1.0e-4, "audio after silent capture is heard");
        }
        // Disable / enable cycle with freeze on: not dead once input arrives.
        {
            FX fx; fx.prepare (sr, 256);
            auto p = grainParams();
            runChain (fx, p, 48000, sine (440.0));
            p.grainFreeze = true; p.grainEnable = false;
            runChain (fx, p, 512, [] (int) { return 0.0f; });
            p.grainEnable = true;
            const auto out = runChain (fx, p, 48000 * 3, sine (440.0));
            expect (energy (out, 48000, 48000 * 3) > 1.0e-4, "freeze after disable/enable is not silent once input arrives");
        }
        // Right after a clear, grains read only the valid span: a constant DC-free burst
        // never produces output before it could have been recorded.
        {
            FX fx; fx.prepare (sr, 256);
            auto p = grainParams(); p.grainPositionMs = 2000.0f; p.grainSizeMs = 300.0f;
            const auto out = runChain (fx, p, 48000, [] (int i) { return i < 4800 ? 0.5f : 0.0f; });
            double worst = 0; for (float v : out) worst = std::max (worst, (double) std::abs (v));
            expect (worst <= 1.0 && std::isfinite (worst), "grains after a clear stay sane");
        }
    }

    void grainReverseTest()
    {
        std::cout << "grainReverseTest\n";
        constexpr double sr = 48000.0;
        constexpr int len = 19200;   // 400 ms burst with a decaying envelope
        Noise nz (21);
        std::vector<float> burst ((size_t) len);
        for (int i = 0; i < len; ++i) burst[(size_t) i] = 0.8f * nz.next() * (float) std::exp (-(double) i / (0.08 * sr));
        const auto hann = [&] (int i) { return 0.5f - 0.5f * (float) std::cos (6.2831853 * i / len); };

        const auto grainOut = [&] (float reverse, float posMs)
        {
            FX fx; fx.prepare (sr, 256);
            FX::Params p = grainParams();
            p.grainDensityHz = 0.5f; p.grainSizeMs = 400.0f; p.grainReverse = reverse; p.grainPositionMs = posMs;
            p.grainSpread = 0.0f; p.grainSpreadPitch = 0.0f;
            const int start = 14400;   // burst occupies [0.3 s, 0.7 s)
            return runChain (fx, p, 96000 + 20480, [&] (int i) { const int k = i - start; return (k >= 0 && k < len) ? burst[(size_t) k] : 0.0f; }, 96000);
        };
        const auto corr = [&] (const std::vector<float>& o, bool rev)
        {
            double dot = 0, no = 0, nt = 0;
            for (int i = 0; i < len && i < (int) o.size(); ++i)
            {
                // A reversed grain starts ON the sample after the burst's end, so it lags the mirror by one.
                const float t = (rev ? (i >= 1 ? burst[(size_t) (len - i)] : 0.0f) : burst[(size_t) i]) * hann (i);
                dot += o[(size_t) i] * t; no += o[(size_t) i] * o[(size_t) i]; nt += t * t;
            }
            return dot / std::sqrt (no * nt + 1.0e-12);
        };
        const auto fwd = grainOut (0.0f, 1700.0f);
        expect (corr (fwd, false) > 0.9 && corr (fwd, true) < 0.5, "forward grain matches the windowed burst (fwd " + juce::String (corr (fwd, false), 3) + ", rev " + juce::String (corr (fwd, true), 3) + ")");
        const auto rv = grainOut (1.0f, 1700.0f);   // POSITION is the delay at the grain centre: 1300 ms at the start + half a 400 ms grain each way
        expect (corr (rv, true) > 0.9 && corr (rv, false) < 0.5, "REVERSE 100% plays the burst backwards (rev " + juce::String (corr (rv, true), 3) + ", fwd " + juce::String (corr (rv, false), 3) + ")");
    }

    // ---- 1.0.30 GRAIN safety / SPREAD split / density ----------------------
    // Random-parameter fuzz, 20 s per combo (2 s of input, then silence):
    // the wet bus never passes the ceiling, nothing goes non-finite, the
    // tail left after the input stops is not louder than the input was, and
    // its energy above 8 kHz does not grow over time.
    void grainFeedbackFuzzTest()
    {
        std::cout << "grainFeedbackFuzzTest\n";
        juce::Random rnd (20260930);
        int bad = 0; double worstPeak = 0.0, worstLate = 0.0, worstHfRatio = 0.0;
        const int combos = 200;
        for (int k = 0; k < combos; ++k)
        {
            const auto pick = [&] (float lo, float hi, float extremeChance = 0.35f)
            {
                const float r = rnd.nextFloat();
                if (r < extremeChance * 0.5f) return lo;
                if (r < extremeChance) return hi;
                return lo + (hi - lo) * rnd.nextFloat();
            };
            FX::Params p;
            p.grainEnable = true; p.grainMix = 1.0f;
            p.grainPitch = pick (-24.0f, 24.0f);
            p.grainFeedback = rnd.nextFloat() < 0.5f ? 0.9f : pick (0.0f, 0.9f);
            p.grainSizeMs = std::exp (pick (std::log (2.0f), std::log (500.0f)));
            p.grainDensityHz = std::exp (pick (0.0f, std::log (400.0f)));
            p.grainSpread = pick (0.0f, 1.0f);
            p.grainSpreadPitch = pick (0.0f, 12.0f);
            p.grainReverse = pick (0.0f, 1.0f);
            p.grainPositionMs = pick (0.0f, 4000.0f);
            // The first combos are the hostile settings from the tester report
            // (PITCH up, FEEDBACK max, short SIZE, high DENSITY, SPREAD, REVERSE):
            // the 1.0.29 build sat at a full-scale, mostly >8 kHz squeal on these.
            if (k < 6)
            {
                struct H { float pitch, size, dens, spT, spP, rev, pos; };
                static constexpr H hostile[6] = {
                    { 7.0f, 20.0f, 100.0f, 0.0f, 0.0f, 0.0f, 100.0f }, { 12.0f, 30.0f, 100.0f, 0.0f, 0.0f, 0.0f, 60.0f },
                    { 12.0f, 20.0f, 100.0f, 1.0f, 12.0f, 0.0f, 60.0f }, { 24.0f, 10.0f, 400.0f, 0.0f, 0.0f, 0.0f, 50.0f },
                    { 5.0f, 50.0f, 400.0f, 0.5f, 6.0f, 0.5f, 100.0f }, { 12.0f, 30.0f, 250.0f, 0.0f, 12.0f, 1.0f, 60.0f } };
                const auto& h = hostile[k];
                p.grainPitch = h.pitch; p.grainFeedback = 0.9f; p.grainSizeMs = h.size; p.grainDensityHz = h.dens;
                p.grainSpread = h.spT; p.grainSpreadPitch = h.spP; p.grainReverse = h.rev; p.grainPositionMs = h.pos;
            }
            if (rnd.nextFloat() < 0.1f) { p.grainSync = true; p.grainDivision = rnd.nextInt (4) + 6; p.bpm = 60.0 + 180.0 * rnd.nextFloat(); }
            const auto r = grainRun (p, 20.0, 2.0);

            const auto mean = [&] (int a, int b, double SecStat::*m) { double t = 0; for (int i = a; i < b; ++i) t += r.sec[(size_t) i].*m; return t / (b - a); };
            const double late = mean (15, 19, &SecStat::rms);
            const double hfEarly = mean (3, 6, &SecStat::hfAbs), hfLate = mean (15, 19, &SecStat::hfAbs);
            const double hfRatio = hfLate / (hfEarly + 1.0e-9);
            worstPeak = std::max (worstPeak, (double) r.peak); worstLate = std::max (worstLate, late);
            if (hfEarly > 1.0e-6) worstHfRatio = std::max (worstHfRatio, hfRatio);
            // The tail never grows: its loudest late second is no louder than its loudest early one.
            double earlyMax = 0, lateMax = 0;
            for (int i = 2; i < 12; ++i) earlyMax = std::max (earlyMax, r.sec[(size_t) i].rms);
            for (int i = 12; i < 19; ++i) lateMax = std::max (lateMax, r.sec[(size_t) i].rms);
            const bool ok = r.finite && r.peak <= 1.0005f && late <= 0.1 && lateMax <= 1.05 * earlyMax + 1.0e-4
                            && hfLate <= 1.25 * hfEarly + 1.0e-7;
            if (! ok)
            {
                ++bad;
                std::cout << "  combo " << k << " FAIL: pitch " << p.grainPitch << " fb " << p.grainFeedback << " size " << p.grainSizeMs
                          << " dens " << p.grainDensityHz << " spT " << p.grainSpread << " spP " << p.grainSpreadPitch << " rev " << p.grainReverse
                          << " pos " << p.grainPositionMs << " sync " << p.grainSync << " peak " << r.peak << " lateRms " << late
                          << " hf " << hfEarly << " -> " << hfLate << (r.finite ? "" : " NONFINITE") << "\n";
            }
        }
        std::cout << "  worst peak " << worstPeak << ", worst late rms " << worstLate << ", worst HF late/early " << worstHfRatio << "\n";
        expect (bad == 0, "GRAIN fuzz: " + juce::String (combos) + " random/extreme combos stay under the ceiling, finite, with a non-growing HF tail (" + juce::String (bad) + " bad)");
    }

    void grainSpreadSplitTest()
    {
        std::cout << "grainSpreadSplitTest\n";
        constexpr double sr = 48000.0;
        struct Stats { double semisMean = 0, semisSd = 0, backMean = 0, backSd = 0; int n = 0; };
        const auto measure = [&] (float spreadTime, float spreadPitch, float pitch)
        {
            spa::dsp::GrainFX g; g.prepare (sr, 64);
            spa::dsp::GrainFX::Params p;
            p.enable = true; p.mix = 1.0f; p.sizeMs = 20.0f; p.densityHz = 300.0f; p.positionMs = 800.0f;
            p.pitchSt = pitch; p.spread = spreadTime; p.spreadPitchSt = spreadPitch; p.reverse = 0.0f;
            Noise nz (5);
            juce::AudioBuffer<float> b (2, 64);
            std::vector<double> semis, backs;
            std::uint64_t seen = 0;
            for (int blk = 0; blk < 48000 * 6 / 64; ++blk)
            {
                for (int i = 0; i < 64; ++i) { const float v = 0.3f * nz.next(); b.setSample (0, i, v); b.setSample (1, i, v); }
                g.process (b, p);
                if (g.grainsSpawned() != seen && blk > 48000 * 2 / 64)
                {
                    semis.push_back (g.lastSpawnSemis()); backs.push_back (g.lastSpawnBackSamples());
                }
                seen = g.grainsSpawned();
            }
            Stats s; s.n = (int) semis.size();
            for (size_t i = 0; i < semis.size(); ++i) { s.semisMean += semis[i]; s.backMean += backs[i]; }
            s.semisMean /= s.n; s.backMean /= s.n;
            for (size_t i = 0; i < semis.size(); ++i) { s.semisSd += (semis[i] - s.semisMean) * (semis[i] - s.semisMean); s.backSd += (backs[i] - s.backMean) * (backs[i] - s.backMean); }
            s.semisSd = std::sqrt (s.semisSd / s.n); s.backSd = std::sqrt (s.backSd / s.n);
            return s;
        };
        const auto a = measure (0.0f, 12.0f, 0.0f);    // pitch jitter only
        const auto b = measure (1.0f, 0.0f, 0.0f);     // time jitter only
        const auto c = measure (0.0f, 0.0f, 7.0f);     // neither
        expect (a.n > 300 && b.n > 300, "spread measurement saw enough grains (" + juce::String (a.n) + ", " + juce::String (b.n) + ")");
        expect (a.semisSd > 5.0 && a.semisSd < 8.0, "SPREAD PITCH 12: pitch of grains scatters (sd " + juce::String (a.semisSd, 2) + " st, uniform +/-12 = 6.93)");
        expect (a.backSd < 1.0, "SPREAD TIME 0: start position does not move with SPREAD PITCH (sd " + juce::String (a.backSd, 3) + " samples)");
        expect (b.semisSd < 1.0e-4, "SPREAD PITCH 0: pitch does not move with SPREAD TIME (sd " + juce::String (b.semisSd, 6) + " st)");
        expect (b.backSd > 0.9 * 0.25 * sr / std::sqrt (12.0) && b.backSd < 1.1 * 0.25 * sr / std::sqrt (12.0), "SPREAD TIME 1: start positions scatter (sd " + juce::String (b.backSd, 0) + " samples, uniform 0.25 s = " + juce::String (0.25 * sr / std::sqrt (12.0), 0) + ")");
        expect (c.semisSd < 1.0e-4 && c.backSd < 1.0 && std::abs (c.semisMean - 7.0) < 1.0e-4, "both at 0: exactly PITCH, exactly POSITION");
    }

    void grainDensityCapTest()
    {
        std::cout << "grainDensityCapTest\n";
        constexpr double sr = 48000.0;
        for (float dens : { 100.0f, 250.0f, 400.0f, 1000.0f })
        {
            spa::dsp::GrainFX g; g.prepare (sr, 256);
            spa::dsp::GrainFX::Params p;
            p.enable = true; p.mix = 1.0f; p.sizeMs = 10.0f; p.densityHz = dens; p.spread = 0.0f; p.spreadPitchSt = 0.0f; p.positionMs = 300.0f;
            Noise nz (9);
            juce::AudioBuffer<float> b (2, 256);
            std::uint64_t at1 = 0;
            for (int blk = 0; blk < 48000 * 3 / 256; ++blk)
            {
                for (int i = 0; i < 256; ++i) { const float v = 0.3f * nz.next(); b.setSample (0, i, v); b.setSample (1, i, v); }
                g.process (b, p);
                if (blk == 48000 / 256) at1 = g.grainsSpawned();
            }
            const double rate = (double) (g.grainsSpawned() - at1) / ((double) (48000 * 3 / 256 - 48000 / 256 - 1) * 256.0 / sr);
            const double expected = std::min (dens, 400.0f);
            expect (std::abs (rate - expected) < 0.02 * expected + 1.0, "DENSITY " + juce::String (dens, 0) + "/s spawns " + juce::String (rate, 1) + " grains per second (expect " + juce::String (expected, 0) + ")");
        }
        {   // Long grains at the cap: many overlap, all slots usable, never past maxGrains, still bounded.
            spa::dsp::GrainFX g; g.prepare (sr, 256);
            spa::dsp::GrainFX::Params p;
            p.enable = true; p.mix = 1.0f; p.sizeMs = 500.0f; p.densityHz = 400.0f; p.spread = 0.0f; p.spreadPitchSt = 0.0f; p.positionMs = 600.0f;
            Noise nz (9);
            juce::AudioBuffer<float> b (2, 256);
            int maxActive = 0; float peak = 0.0f;
            for (int blk = 0; blk < 48000 * 4 / 256; ++blk)
            {
                for (int i = 0; i < 256; ++i) { const float v = 0.3f * nz.next(); b.setSample (0, i, v); b.setSample (1, i, v); }
                g.process (b, p);
                maxActive = std::max (maxActive, g.activeGrains());
                peak = std::max (peak, b.getMagnitude (0, 256));
            }
            expect (maxActive >= 190 && maxActive <= spa::dsp::GrainFX::maxGrains, "400/s x 500 ms keeps ~200 grains alive (" + juce::String (maxActive) + " of " + juce::String (spa::dsp::GrainFX::maxGrains) + ")");
            expect (peak <= 1.0005f, "dense long cloud stays under the ceiling (" + juce::String (peak, 3) + ")");
        }
        // The DENSITY knob keeps its resolution at the low end: the stored value of an
        // old session is unchanged and the default still sits at 14/s.
        auto* proc = new spa::SPAStripProcessor();
        std::unique_ptr<spa::SPAStripProcessor> own (proc);
        auto* d = proc->getAPVTS().getParameter (id::fx::grainDensity);
        expect (d != nullptr && d->convertFrom0to1 (1.0f) == 400.0f && d->convertFrom0to1 (0.0f) == 1.0f, "DENSITY range is 1..400");
        expect (d != nullptr && d->convertFrom0to1 (0.5f) < 40.0f && d->convertFrom0to1 (0.25f) < 12.0f, "DENSITY taper keeps the low end fine (25% = " + juce::String (d->convertFrom0to1 (0.25f), 1) + "/s, 50% = " + juce::String (d->convertFrom0to1 (0.5f), 1) + "/s)");
    }

    void grainFeedbackStillBuildsTest()
    {
        std::cout << "grainFeedbackStillBuildsTest\n";
        // The safety must not kill the effect: a plain unpitched cloud with FEEDBACK
        // keeps ringing after the input stops, and without FEEDBACK it does not.
        auto p = grainParams(); p.grainDensityHz = 14.0f; p.grainSizeMs = 120.0f; p.grainPositionMs = 300.0f;
        p.grainFeedback = 0.9f; p.grainSpread = 0.0f; p.grainSpreadPitch = 0.0f;
        const auto withFb = grainRun (p, 8.0, 1.0);
        p.grainFeedback = 0.0f;
        const auto noFb = grainRun (p, 8.0, 1.0);
        expect (withFb.sec[4].rms > 1.0e-3 && withFb.sec[4].rms > 100.0 * noFb.sec[4].rms + 1.0e-6, "FEEDBACK 0.9 still sustains repeats 3 s after the input stops (rms " + juce::String (withFb.sec[4].rms, 4) + " vs " + juce::String (noFb.sec[4].rms, 5) + " without)");
        // Repeats get darker: the centroid of the late tail sits below the first pass.
        expect (withFb.sec[3].centroid < withFb.sec[1].centroid + 1.0, "repeats do not get brighter");
    }

    void grainSizeIndependentFeedbackTest()
    {
        std::cout << "grainSizeIndependentFeedbackTest\n";
        constexpr double sr = 48000.0;
        // A click in, feedback on: the echoes land at multiples of POSITION and
        // each repeat loses the same amount, whatever SIZE and DENSITY are.
        struct Echo { std::array<double, 3> at {}; std::array<float, 3> amp {}; };
        const auto run = [&] (float sizeMs, float dens, float pitch, float reverse)
        {
            FX::Params p = grainParams();
            p.grainSizeMs = sizeMs; p.grainDensityHz = dens; p.grainPositionMs = 500.0f; p.grainFeedback = 0.8f;
            p.grainPitch = pitch; p.grainReverse = reverse;
            FX fx; fx.prepare (sr, 256);
            const int total = (int) (sr * 2.4);
            const auto o = runChain (fx, p, total, [] (int i) { return i == 24000 ? 0.5f : 0.0f; });
            Echo e;
            for (int n = 1; n <= 3; ++n)
            {
                const int centre = 24000 + (int) (n * 0.5 * sr);
                float best = 0.0f; int bi = centre;
                for (int i = centre - 4800; i < centre + 4800 && i < (int) o.size(); ++i)
                    if (std::abs (o[(size_t) i]) > best) { best = std::abs (o[(size_t) i]); bi = i; }
                e.at[(size_t) (n - 1)] = (double) (bi - 24000) / sr; e.amp[(size_t) (n - 1)] = best;
            }
            return e;
        };
        for (const auto& cfg : { std::make_pair (0.0f, 0.0f), std::make_pair (7.0f, 0.0f) })
        {
            const auto small = run (40.0f, 100.0f, cfg.first, cfg.second);
            const auto big   = run (200.0f, 100.0f, cfg.first, cfg.second);
            const auto dense = run (200.0f, 300.0f, cfg.first, cfg.second);
            const juce::String tag = " (pitch " + juce::String (cfg.first, 0) + ", reverse " + juce::String (cfg.second, 0) + ")";
            for (int n = 0; n < 3; ++n)
            {
                expect (std::abs (small.at[(size_t) n] - big.at[(size_t) n]) < 0.004 && std::abs (small.at[(size_t) n] - dense.at[(size_t) n]) < 0.004,
                        "echo " + juce::String (n + 1) + " lands at the same time for SIZE 40 / 200 ms and DENSITY 100 / 300 ("
                        + juce::String (small.at[(size_t) n], 4) + " / " + juce::String (big.at[(size_t) n], 4) + " / " + juce::String (dense.at[(size_t) n], 4) + " s)" + tag);
            }
            if (cfg.second == 0.0f)
                expect (std::abs (small.at[0] - 0.5) < 0.004, "first echo sits at POSITION (" + juce::String (small.at[0], 4) + " s)" + tag);
            if (cfg.first != 0.0f) continue;   // a pitched click is reshaped each pass: only its timing is comparable
            const auto ratio = [] (const Echo& e) { return e.amp[1] / std::max (1.0e-9f, e.amp[0]); };
            expect (ratio (small) > 0.0f && std::abs (ratio (big) / ratio (small) - 1.0f) < 0.2f && std::abs (ratio (dense) / ratio (small) - 1.0f) < 0.2f,
                    "decay per repeat is the same for every SIZE / DENSITY (" + juce::String (ratio (small), 3) + " / " + juce::String (ratio (big), 3) + " / " + juce::String (ratio (dense), 3) + ")" + tag);
        }
    }

    void fxOrderMigrationTest()
    {
        std::cout << "fxOrderMigrationTest\n";
        // A custom nine-module order saved before 1.0.29.
        const int legacyIds[9] { 8, 7, 4, 3, 6, 5, 2, 1, 0 };   // conv, limit, eq, reverb, trem, mod, delay, chorus, dist
        juce::uint64 packed = 0;
        for (int i = 0; i < 9; ++i) packed |= (juce::uint64) legacyIds[i] << (i * 4);
        FX::Module out[FX::numModules];
        FX::unpackOrder (packed, out);
        const int expected[11] { 8, 9, 7, 4, 3, 6, 5, 10, 2, 1, 0 };   // COMP before LIMIT, GRAIN before DELAY
        bool ok = true;
        for (int i = 0; i < 11; ++i) ok = ok && (int) out[i] == expected[i];
        expect (ok, "legacy nine-module order keeps the user's order, GRAIN before DELAY, COMP before LIMIT");

        // The old DEFAULT order maps onto the new default.
        juce::uint64 oldDef = 0; const int od[9] { 0, 1, 5, 6, 2, 3, 8, 4, 7 };
        for (int i = 0; i < 9; ++i) oldDef |= (juce::uint64) od[i] << (i * 4);
        FX::unpackOrder (oldDef, out);
        FX::Module def[FX::numModules]; FX::unpackOrder (FX::defaultOrderPacked(), def);
        bool same = true; for (int i = 0; i < 11; ++i) same = same && out[i] == def[i];
        expect (same, "old default order migrates to exactly the new default order");

        // A full 11-module order round-trips; corrupt falls back to natural.
        FX::unpackOrder (FX::packOrder (def), out);
        bool rt = true; for (int i = 0; i < 11; ++i) rt = rt && out[i] == def[i];
        expect (rt, "11-module order round-trips");
        FX::unpackOrder (0xFFFFFFFFFFFFFFFFull, out);
        bool nat = true; for (int i = 0; i < 11; ++i) nat = nat && (int) out[i] == i;
        expect (nat, "corrupt value still falls back to the natural order");

        // Through the processor: a session carrying the legacy value.
        spa::SPAStripProcessor proc; proc.prepareToPlay (48000.0, 512);
        auto state = proc.getAPVTS().copyState();
        state.setProperty ("fxOrder", (juce::int64) packed, nullptr);
        juce::MemoryBlock mb;
        if (auto xml = state.createXml()) juce::AudioProcessor::copyXmlToBinary (*xml, mb);
        proc.setStateInformation (mb.getData(), (int) mb.getSize());
        const auto ids = proc.getFxOrder();
        bool pok = ids.size() == 11; for (int i = 0; pok && i < 11; ++i) pok = ids[i] == expected[i];
        expect (pok, "setStateInformation with a legacy order loads the migrated custom order");
    }

}
