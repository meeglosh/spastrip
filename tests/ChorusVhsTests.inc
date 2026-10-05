// CHORUS VHS mode, ported from SPASynth 1.0.32 (its tests/ChorusVhsTests.inc; the DSP
// tests are verbatim, the registry / randomize / UI ones adapted to SPAStrip).
// (reference/StereoChorus_1_0_2.h is included at the top of SPAStripTests.cpp.)

namespace chorusVhsTests
{
    using spa::dsp::StereoChorus;
    namespace id = spa::params::id;

    inline StereoChorus::Params vhsBase()
    {
        StereoChorus::Params p;
        p.mode = StereoChorus::Mode::vhs;
        p.mix = 1.0f;
        p.feedback = 0.0f;
        p.vhsWow = p.vhsFlutter = p.vhsSat = p.vhsHiss = p.vhsDropouts = 0.0f;
        p.vhsTone = 1.0f;
        return p;
    }

    // Renders mono-in-both-channels input through a fresh StereoChorus.
    template <typename Gen>
    inline std::vector<float> render (const StereoChorus::Params& p, double sr, int total, Gen gen,
                                      std::vector<float>* right = nullptr)
    {
        StereoChorus c;
        c.prepare (sr, 512);
        juce::AudioBuffer<float> buf (2, 512);
        std::vector<float> out, outR;
        out.reserve ((size_t) total);
        int done = 0;
        while (done < total)
        {
            for (int i = 0; i < 512; ++i)
            {
                const float v = gen (done + i);
                buf.setSample (0, i, v);
                buf.setSample (1, i, v);
            }
            c.process (buf, p);
            for (int i = 0; i < 512 && done + i < total; ++i)
            {
                out.push_back (buf.getSample (0, i));
                outR.push_back (buf.getSample (1, i));
            }
            done += 512;
        }
        if (right != nullptr)
            *right = outR;
        return out;
    }

    inline float rms (const std::vector<float>& v, size_t a, size_t b)
    {
        double s = 0;
        for (size_t i = a; i < b; ++i) s += (double) v[i] * v[i];
        return (float) std::sqrt (s / (double) std::max<size_t> (1, b - a));
    }

    // RMS deviation (cents) of the cycle-to-cycle frequency of a tone, from
    // positive-going zero crossings (interpolated). Skips the first second.
    inline void pitchDeviation (const std::vector<float>& x, double sr, double nominalHz,
                                float& rmsCents, float& peakCents)
    {
        std::vector<double> cross;
        for (size_t i = (size_t) sr; i + 1 < x.size(); ++i)
            if (x[i] <= 0.0f && x[i + 1] > 0.0f)
                cross.push_back ((double) i + (double) (-x[i]) / (double) (x[i + 1] - x[i]));
        double acc = 0; peakCents = 0; int n = 0;
        for (size_t i = 1; i < cross.size(); ++i)
        {
            const double f = sr / (cross[i] - cross[i - 1]);
            const double cents = 1200.0 * std::log2 (f / nominalHz);
            acc += cents * cents; ++n;
            peakCents = juce::jmax (peakCents, (float) std::abs (cents));
        }
        rmsCents = n > 0 ? (float) std::sqrt (acc / n) : 0.0f;
    }

    // Averaged power spectrum (dB per bin) of a long noise render.
    inline std::vector<float> powerSpectrum (const std::vector<float>& x, int order = 12)
    {
        const int N = 1 << order;
        juce::dsp::FFT fft (order);
        std::vector<float> acc ((size_t) N / 2, 0.0f);
        std::vector<float> frame ((size_t) N * 2);
        int frames = 0;
        for (size_t start = 4800; start + (size_t) N <= x.size(); start += (size_t) N / 2)
        {
            std::fill (frame.begin(), frame.end(), 0.0f);
            for (int i = 0; i < N; ++i)
            {
                const float w = 0.5f - 0.5f * std::cos (6.2831853f * (float) i / (float) N);
                frame[(size_t) i] = x[start + (size_t) i] * w;
            }
            fft.performRealOnlyForwardTransform (frame.data());
            for (int k = 0; k < N / 2; ++k)
                acc[(size_t) k] += frame[(size_t) 2 * k] * frame[(size_t) 2 * k]
                                 + frame[(size_t) 2 * k + 1] * frame[(size_t) 2 * k + 1];
            ++frames;
        }
        for (auto& v : acc) v /= (float) juce::jmax (1, frames);
        return acc;
    }

    // Frequency at which the spectrum has fallen 3 dB below its 1-3 kHz mean.
    inline float minus3dBFreq (const std::vector<float>& ps, double sr, int N)
    {
        const double bin = sr / N;
        double ref = 0; int cnt = 0;
        for (int k = (int) (1000 / bin); k < (int) (3000 / bin); ++k) { ref += ps[(size_t) k]; ++cnt; }
        ref /= cnt;
        for (int k = (int) (3000 / bin); k < N / 2; ++k)
        {
            // 5-bin smoothing against noise-spectrum ripple
            double a = 0; int c = 0;
            for (int j = juce::jmax (0, k - 8); j <= juce::jmin (N / 2 - 1, k + 8); ++j) { a += ps[(size_t) j]; ++c; }
            if (a / c < ref * 0.5)
                return (float) (k * bin);
        }
        return (float) (sr * 0.5);
    }

    inline auto noiseGen (float amp, unsigned seed = 7)
    {
        auto rng = std::make_shared<juce::Random> (seed);
        return [rng, amp] (int) { return amp * (rng->nextFloat() * 2.0f - 1.0f); };
    }

    inline auto sineGen (double sr, double hz, float amp)
    {
        return [=] (int i) { return amp * (float) std::sin (6.283185307179586 * hz * (double) i / sr); };
    }

    // ---- Vintage / Modern untouched ---------------------------------------
    static void legacyIdenticalTest()
    {
        std::cout << "chorusVhs: Vintage/Modern bit-identical to 1.0.31\n";
        int compared = 0, mismatches = 0;
        for (double sr : { 44100.0, 48000.0, 96000.0 })
            for (int mode = 0; mode < 2; ++mode)
                for (int variant = 0; variant < 3; ++variant)
                {
                    StereoChorus neu;
                    spa::dsp::legacy1031::StereoChorus old;
                    neu.prepare (sr, 512);
                    old.prepare (sr, 512);
                    juce::Random rng (100 + variant);
                    juce::AudioBuffer<float> a (2, 512), b (2, 512);
                    for (int blk = 0; blk < 60; ++blk)
                    {
                        for (int ch = 0; ch < 2; ++ch)
                            for (int i = 0; i < 512; ++i)
                                a.setSample (ch, i, 0.5f * (rng.nextFloat() * 2.0f - 1.0f));
                        b.makeCopyOf (a);
                        StereoChorus::Params pn;
                        pn.mode = mode == 0 ? StereoChorus::Mode::vintage : StereoChorus::Mode::modern;
                        pn.rateHz = 0.3f + 1.7f * (float) variant;
                        pn.depth = 0.3f + 0.3f * (float) variant;
                        pn.feedback = variant == 2 ? 0.7f : 0.0f;
                        pn.width = 0.2f * (float) (variant + 1);
                        pn.mix = 0.5f;
                        pn.enable = ! (variant == 1 && blk >= 20 && blk < 30);   // exercise the enable edge
                        // VHS params set to extremes: must have zero effect on the other modes.
                        pn.vhsWow = 1.0f; pn.vhsHiss = 1.0f; pn.vhsDropouts = 1.0f; pn.vhsSat = 1.0f;
                        spa::dsp::legacy1031::StereoChorus::Params po;
                        po.enable = pn.enable;
                        po.mode = mode == 0 ? spa::dsp::legacy1031::StereoChorus::Mode::vintage
                                            : spa::dsp::legacy1031::StereoChorus::Mode::modern;
                        po.rateHz = pn.rateHz; po.depth = pn.depth; po.feedback = pn.feedback;
                        po.width = pn.width; po.mix = pn.mix;
                        neu.process (a, pn);
                        old.process (b, po);
                        for (int ch = 0; ch < 2; ++ch)
                            for (int i = 0; i < 512; ++i)
                            {
                                ++compared;
                                if (a.getSample (ch, i) != b.getSample (ch, i)) ++mismatches;
                            }
                    }
                }
        expect (compared > 1000000, "legacy comparison covered samples");
        expect (mismatches == 0, "Vintage and Modern are bit-identical to the 1.0.31 code (" + juce::String (mismatches) + " mismatches)");
    }

    static void pitchModulationTest()
    {
        std::cout << "chorusVhs: wow/flutter pitch modulation\n";
        const double sr = 48000.0;
        const int total = (int) (sr * 14);
        auto measure = [&] (float wow, float flutter, float rate, float depth)
        {
            auto p = vhsBase();
            p.vhsWow = wow; p.vhsFlutter = flutter; p.rateHz = rate; p.depth = depth;
            const auto y = render (p, sr, total, sineGen (sr, 1000.0, 0.3f));
            float r, pk; pitchDeviation (y, sr, 1000.0, r, pk);
            return std::pair<float, float> { r, pk };
        };
        const auto none = measure (0, 0, 0.8f, 0.3f);
        const auto w40 = measure (0.4f, 0, 0.8f, 0.3f);
        const auto w100 = measure (1.0f, 0, 0.8f, 0.3f);
        const auto f25 = measure (0, 0.25f, 0.8f, 0.3f);
        const auto f100 = measure (0, 1.0f, 0.8f, 0.3f);
        std::cout << "  rms/peak cents: off " << none.first << "/" << none.second
                  << "  wow40 " << w40.first << "/" << w40.second << "  wow100 " << w100.first << "/" << w100.second
                  << "  flut25 " << f25.first << "/" << f25.second << "  flut100 " << f100.first << "/" << f100.second << "\n";
        expect (none.first < 0.5f, "no wow/flutter: pitch is steady");
        expect (w40.first > 3.0f * juce::jmax (none.first, 0.2f), "wow 40% modulates pitch");
        expect (w100.first > 1.6f * w40.first, "more WOW, more pitch deviation");
        expect (f25.first > 3.0f * juce::jmax (none.first, 0.2f), "flutter 25% modulates pitch");
        expect (f100.first > 1.6f * f25.first, "more FLUTTER, more pitch deviation");
        const auto d0 = measure (0.4f, 0.25f, 0.8f, 0.0f);
        expect (d0.first < w40.first, "DEPTH scales the modulation down");
    }

    static void toneTest()
    {
        std::cout << "chorusVhs: TONE bandwidth\n";
        const double sr = 48000.0;
        auto spectrumFor = [&] (float tone)
        {
            auto p = vhsBase();
            p.vhsTone = tone;
            const auto y = render (p, sr, (int) (sr * 6), noiseGen (0.3f));
            return powerSpectrum (y);
        };
        const auto dark = spectrumFor (0.0f), bright = spectrumFor (1.0f);
        const float fd = minus3dBFreq (dark, sr, 4096), fb = minus3dBFreq (bright, sr, 4096);
        auto centroid = [&] (const std::vector<float>& ps)
        { double n = 0, d = 0; for (size_t k = 1; k < ps.size(); ++k) { n += (double) k * ps[k]; d += ps[k]; } return n / d * sr / 4096.0; };
        std::cout << "  -3 dB: tone0 " << fd << " Hz, tone1 " << fb << " Hz; centroid " << centroid (dark) << " vs " << centroid (bright) << "\n";
        expect (fd > 4500.0f && fd < 6500.0f, "darkest TONE rolls off near 5.5 kHz");
        expect (fb > 13000.0f && fb < 18500.0f, "brightest TONE rolls off near 16 kHz");
        expect (centroid (dark) < centroid (bright), "darker tone has the lower spectral centroid");
        // Warmth: 250 Hz sits above the 1-3 kHz band at the dark setting.
        const double bin = sr / 4096.0;
        const float warm = dark[(size_t) (250 / bin)], mid = dark[(size_t) (2000 / bin)];
        expect (warm > mid * 1.3f, "low-mid warmth bump present at darkest tone");
        // Sample-rate independence: same corner at 96 k and 192 k.
        for (double sr2 : { 96000.0, 192000.0 })
        {
            auto p = vhsBase(); p.vhsTone = 0.0f;
            const auto y = render (p, sr2, (int) (sr2 * 3), noiseGen (0.3f));
            const float f = minus3dBFreq (powerSpectrum (y, 13), sr2, 8192);
            std::cout << "  tone0 -3 dB at " << sr2 << ": " << f << " Hz\n";
            expect (f > 4500.0f && f < 6500.0f, "TONE corner follows the real sample rate (" + juce::String (sr2) + ")");
        }
    }

    static void saturationTest()
    {
        std::cout << "chorusVhs: SATURATION level match\n";
        const double sr = 48000.0;
        auto levelAndH3 = [&] (float sat)
        {
            auto p = vhsBase(); p.vhsSat = sat;
            const auto y = render (p, sr, (int) sr * 2, sineGen (sr, 1000.0, 0.3f));
            const float level = rms (y, 24000, y.size());
            // third harmonic (3 kHz) magnitude by correlation
            double c = 0, s = 0;
            for (size_t i = 24000; i < y.size(); ++i)
            { c += y[i] * std::cos (6.283185307 * 3000.0 * (double) i / sr); s += y[i] * std::sin (6.283185307 * 3000.0 * (double) i / sr); }
            return std::pair<float, float> { level, (float) std::sqrt (c * c + s * s) * 2.0f / (float) (y.size() - 24000) };
        };
        const auto base = levelAndH3 (0.0f);
        for (float sat : { 0.3f, 0.6f, 1.0f })
        {
            const auto r = levelAndH3 (sat);
            const float db = 20.0f * std::log10 (r.first / base.first);
            std::cout << "  sat " << sat << ": " << db << " dB vs off, H3 " << r.second << " (off " << base.second << ")\n";
            expect (std::abs (db) < 1.0f, "SATURATION " + juce::String (sat) + " stays within 1 dB of the clean level");
            expect (r.second > base.second * 5.0f + 1.0e-4f, "SATURATION " + juce::String (sat) + " adds harmonics");
        }
    }

    static void hissTest()
    {
        std::cout << "chorusVhs: HISS follows the signal\n";
        const double sr = 48000.0;
        const int play = (int) sr * 1, silence = (int) sr * 5;
        auto gen = [=] (int i) { return i < play ? 0.3f * (float) std::sin (6.2831853 * 440.0 * i / sr) : 0.0f; };
        auto p = vhsBase(); p.vhsHiss = 1.0f;
        std::vector<float> r;
        const auto y = render (p, sr, play + silence, gen, &r);
        auto p0 = p; p0.vhsHiss = 0.0f;
        const auto y0 = render (p0, sr, play + silence, gen);
        double ds = 0; int n = 0;
        for (int i = (int) (sr * 0.5); i < play; ++i) { const double d = y[(size_t) i] - y0[(size_t) i]; ds += d * d; ++n; }
        const float hissRms = (float) std::sqrt (ds / n);
        const float sigRms = rms (y0, (size_t) (sr * 0.5), (size_t) play);
        std::cout << "  max hiss while playing: " << 20.0f * std::log10 (hissRms / sigRms) << " dB re signal\n";
        expect (hissRms > 0.002f, "hiss is audible while playing at 100%");
        auto pd = vhsBase(); pd.vhsHiss = 0.15f;
        const auto yd = render (pd, sr, play, gen);
        const auto yd0 = render (p0, sr, play, gen);
        double dd = 0; n = 0;
        for (int i = (int) (sr * 0.5); i < play; ++i) { const double d = yd[(size_t) i] - yd0[(size_t) i]; dd += d * d; ++n; }
        std::cout << "  default hiss (15%): " << 20.0f * std::log10 (std::sqrt (dd / n) / sigRms) << " dB re signal\n";
        // Exactly zero well after the release.
        bool allZero = true; size_t firstNonZero = 0;
        for (size_t i = (size_t) (play + sr * 4.0); i < y.size(); ++i)
            if (y[i] != 0.0f || r[i] != 0.0f) { allZero = false; firstNonZero = i; break; }
        expect (allZero, "output is EXACTLY zero after the input has been silent past the release (first non-zero at " + juce::String ((int) firstNonZero) + ")");
        // And it fades: the hiss is quieter 1.5 s after the note stops than at note end.
        const float early = rms (y, (size_t) play + 100, (size_t) play + 2000) - 0.0f;
        const float late = rms (y, (size_t) (play + sr * 3.0), (size_t) (play + sr * 3.2));
        expect (late < early, "hiss fades with the sound");
    }

    static void dropoutTest()
    {
        std::cout << "chorusVhs: DROPOUTS\n";
        const double sr = 48000.0;
        const int total = (int) sr * 40;
        auto analyse = [&] (float drop, int& events, float& maxD2, float& minRel)
        {
            auto p = vhsBase(); p.vhsDropouts = drop;
            const auto y = render (p, sr, total, sineGen (sr, 220.0, 0.3f));
            maxD2 = 0;
            for (size_t i = 48000; i + 2 < y.size(); ++i)
                maxD2 = juce::jmax (maxD2, std::abs (y[i + 2] - 2.0f * y[i + 1] + y[i]));
            // 5 ms RMS windows, dip = below 70 % of the median level
            std::vector<float> lv;
            for (size_t i = 48000; i + 240 < y.size(); i += 240) lv.push_back (rms (y, i, i + 240));
            auto sorted = lv; std::sort (sorted.begin(), sorted.end());
            const float med = sorted[sorted.size() / 2];
            events = 0; minRel = 1.0f; bool in = false;
            for (float v : lv)
            {
                minRel = juce::jmin (minRel, v / med);
                if (v < 0.8f * med && ! in) { ++events; in = true; }
                else if (v >= 0.9f * med) in = false;
            }
        };
        int e0, e1, e10; float d0, d1, d10, m0, m1, m10;
        analyse (0.0f, e0, d0, m0);
        analyse (1.0f, e1, d1, m1);
        analyse (0.1f, e10, d10, m10);
        std::cout << "  events in 39 s: off " << e0 << ", 10% " << e10 << ", 100% " << e1
                  << "; deepest dip " << m10 << " (10%), " << m1 << " (100%); max |d2| off " << d0 << " 100% " << d1 << "\n";
        expect (e0 == 0 && m0 > 0.9f, "no dropouts at 0");
        expect (e1 >= 15 && e1 <= 120, "100% dropouts: plenty of events");
        expect (e10 < e1 && e10 <= 25, "10% dropouts are occasional");
        expect (m1 < 0.8f, "dropouts dip the level");
        expect (d1 < d0 * 3.0f + 1.0e-5f, "dropout edges are smooth (no clicks: max second difference " + juce::String (d1) + " vs clean " + juce::String (d0) + ")");
    }

    static void extremesTest()
    {
        std::cout << "chorusVhs: extremes finite and bounded\n";
        for (double sr : { 44100.0, 96000.0, 192000.0 })
        {
            auto p = vhsBase();
            p.rateHz = 5.0f; p.depth = 1.0f; p.feedback = 0.9f; p.width = 1.0f;
            p.vhsWow = p.vhsFlutter = p.vhsSat = p.vhsHiss = p.vhsDropouts = 1.0f; p.vhsTone = 0.0f;
            std::vector<float> r;
            const auto y = render (p, sr, (int) (sr * 3), noiseGen (1.0f, 3), &r);
            float peak = 0; bool fin = true;
            for (size_t i = 0; i < y.size(); ++i)
            { fin = fin && std::isfinite (y[i]) && std::isfinite (r[i]); peak = juce::jmax (peak, std::abs (y[i]), std::abs (r[i])); }
            std::cout << "  " << sr << " Hz: peak " << peak << "\n";
            expect (fin, "VHS output finite at " + juce::String (sr));
            expect (peak < 3.0f, "VHS output bounded at " + juce::String (sr) + " (peak " + juce::String (peak) + ")");
        }
    }

    static void registryAndRandomizeTest()
    {
        std::cout << "chorusVhs: registry + RANDOMIZE\n";
        using namespace spa::params;
        const char* ids[] { id::fx::chorusVhsWow, id::fx::chorusVhsFlutter, id::fx::chorusVhsTone,
                            id::fx::chorusVhsSat, id::fx::chorusVhsHiss, id::fx::chorusVhsDropouts };
        const float defs[] { 40, 25, 45, 30, 15, 10 };
        for (int i = 0; i < 6; ++i)
        {
            const auto* d = find (ids[i]);
            expect (d != nullptr, juce::String (ids[i]) + " registered");
            if (d == nullptr) continue;
            expect (d->section == Section::fxChorus && d->unit == "%" && d->range.start == 0.0f
                        && d->range.end == 100.0f && d->defaultValue == defs[i],
                    juce::String (ids[i]) + " range/default as in SPASynth");
            expect (spa::mod::indexOf (ids[i]) < 0, juce::String (ids[i]) + " is not a mod target (as in SPASynth)");
        }
        if (const auto* m = find (id::fx::chorusMode))
            expect (m->choices.size() == 3 && m->choices[2] == "VHS", "chorus mode choices append VHS at index 2");

        spa::SPAStripProcessor proc;
        proc.prepareToPlay (48000.0, 512);
        auto& apvts = proc.getAPVTS();
        juce::Random rng (4242);
        int vhs = 0, vint = 0, modern = 0;
        const int rolls = 3000;
        float maxHiss = 0, maxDrop = 0;
        for (int i = 0; i < rolls; ++i)
        {
            auto order = proc.getFxOrder();
            randomizeAll (apvts, 0.5f, 0, order, rng);
            const int m = (int) apvts.getRawParameterValue (id::fx::chorusMode)->load();
            (m == 2 ? vhs : m == 0 ? vint : modern)++;
            maxHiss = juce::jmax (maxHiss, apvts.getRawParameterValue (id::fx::chorusVhsHiss)->load());
            maxDrop = juce::jmax (maxDrop, apvts.getRawParameterValue (id::fx::chorusVhsDropouts)->load());
        }
        std::cout << "  " << rolls << " rolls: VHS " << vhs << ", Vintage " << vint << ", Modern " << modern
                  << "; max hiss " << maxHiss << " max dropouts " << maxDrop << "\n";
        expect (vhs >= rolls * 0.13 && vhs <= rolls * 0.23, "RANDOMIZE picks VHS roughly 18% of the time");
        expect (vint > 0 && modern > 0, "RANDOMIZE still picks Vintage and Modern");
        expect (maxHiss <= 40.01f && maxDrop <= 40.01f, "rolled HISS/DROPOUTS never exceed 40%");
    }

    static void uiVisibilityTest()
    {
        std::cout << "chorusVhs: UI visibility + captions\n";
        phase3Tests::ForceLive live;
        spa::SPAStripProcessor proc;
        proc.prepareToPlay (48000.0, 512);
        std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
        editor->setSize (spa::ui::metrics::baseWidth, spa::ui::metrics::baseHeight);
        editor->addToDesktop (0);
        editor->setVisible (true);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (150);
        for (float scale : { 1.0f, 1.5f })
        {
            editor->setSize ((int) (spa::ui::metrics::baseWidth * scale), (int) (spa::ui::metrics::baseHeight * scale));
            juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
            juce::TabbedComponent* tabs = nullptr;
            std::function<void (juce::Component&)> find = [&] (juce::Component& c)
            {
                if (auto* t = dynamic_cast<juce::TabbedComponent*> (&c))
                    if (t->getTabNames().contains ("TREM/VIB")) tabs = t;
                for (auto* ch : c.getChildren()) find (*ch);
            };
            find (*editor);
            expect (tabs != nullptr, "fx tabs found");
            if (tabs == nullptr) return;
            tabs->setCurrentTabIndex (tabs->getTabNames().indexOf ("CHORUS"));
            auto* panel = tabs->getCurrentContentComponent();
            const char* vhsIds[] { id::fx::chorusVhsWow, id::fx::chorusVhsFlutter, id::fx::chorusVhsTone,
                                   id::fx::chorusVhsSat, id::fx::chorusVhsHiss, id::fx::chorusVhsDropouts };
            auto findByParam = [&] (const juce::String& pid) -> juce::Component*
            {
                juce::Component* r = nullptr;
                std::function<void (juce::Component&)> w = [&] (juce::Component& c)
                {
                    if (c.getProperties()["paramID"].toString() == pid) r = &c;
                    for (auto* ch : c.getChildren()) w (*ch);
                };
                w (*panel);
                return r;
            };
            auto* mode = dynamic_cast<juce::ComboBox*> (findByParam (id::fx::chorusMode));
            expect (mode != nullptr, "mode combo found");
            if (mode == nullptr) return;
            auto setMode = [&] (int idx) { mode->setSelectedItemIndex (idx, juce::sendNotificationSync); };

            for (int idx : { 1, 0 })
            {
                setMode (idx);
                for (auto* pid : vhsIds)
                    expect (findByParam (pid) != nullptr && ! findByParam (pid)->isVisible(),
                            juce::String (pid) + " hidden outside VHS mode");
            }
            // Real hit-test on the combo, then through its selection path.
            auto centre = editor->getLocalArea (mode, mode->getLocalBounds()).getCentre();
            expect (editor->getComponentAt (centre) == mode || mode->isParentOf (editor->getComponentAt (centre)),
                    "the mode combo is hit-testable at its centre");
            setMode (2);
            expect (std::abs (proc.getAPVTS().getRawParameterValue (id::fx::chorusMode)->load() - 2.0f) < 0.01f, "combo drives the parameter");
            for (auto* pid : vhsIds)
            {
                auto* c = findByParam (pid);
                expect (c != nullptr && c->isVisible() && c->getWidth() > 10 && c->getHeight() > 10, juce::String (pid) + " visible in VHS mode");
                if (c == nullptr) continue;
                auto cp = editor->getLocalArea (c, c->getLocalBounds()).getCentre();
                auto* hit = editor->getComponentAt (cp);
                expect (hit == c || c->isParentOf (hit), juce::String (pid) + " is hit-testable at its centre (real dispatch target)");
            }
            // Captions fit their columns.
            std::function<void (juce::Component&)> walk = [&] (juce::Component& c)
            {
                if (&c != panel && ! c.isVisible()) return;
                if (auto* lbl = dynamic_cast<juce::Label*> (&c))
                    if (dynamic_cast<juce::ComboBox*> (lbl->getParentComponent()) == nullptr && lbl->getText().isNotEmpty())
                    {
                        const auto avail = (float) lbl->getWidth() - 10;
                        const auto need = juce::GlyphArrangement::getStringWidth (lbl->getFont(), lbl->getText());
                        expect (avail >= need, "VHS caption '" + lbl->getText() + "' fits (" + juce::String (need, 1) + " needed, " + juce::String (avail, 1) + " available)");
                        expect (lbl->getBottom() <= lbl->getParentComponent()->getHeight(), "caption '" + lbl->getText() + "' inside its panel");
                    }
                for (auto* ch : c.getChildren()) walk (*ch);
            };
            walk (*panel);
            setMode (1);
        }
        editor->removeFromDesktop();
    }

    // Every knob in an FX panel has the same diameter (a long caption widens
}
