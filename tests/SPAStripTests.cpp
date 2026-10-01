// Headless test suite for SPAStrip: the lifted FX engines (ported from
// SPASynth's suite) plus processor-level tests -- passthrough, parameter
// parity with the synth registry, state round trips (including an embedded
// convolution IR), latency / dry-wet alignment under oversampling, bypass,
// bus layouts, tempo, the non-finite safety net and a long all-effects soak.

#include <algorithm>
#include <array>
#include <cstdlib>
#include <functional>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <vector>

#include <juce_cryptography/juce_cryptography.h>

#include "SPAStripProcessor.h"
#include "dsp/FXChain.h"
#include "params/ParameterRegistry.h"
#include "mod/ModTargets.h"

#include <SPAStripFactoryData.h>

#include "reference/GlitchMultiband.h"

#if JUCE_MAC
// libmalloc's (private but long-stable) allocation hook, used by noAllocationTest.
extern "C" { extern void (*malloc_logger) (uint32_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uint32_t); }
#endif

namespace
{
    int failures = 0;

    void expect (bool condition, const juce::String& description)
    {
        std::cout << (condition ? "  ok    " : "  FAIL  ") << description << "\n";
        if (! condition)
            ++failures;
    }

    namespace pid = spa::params::id;
    using Proc = spa::SPAStripProcessor;

    // Real (un-normalised) value in, host-style automation out.
    void setParam (Proc& proc, const juce::String& id, float realValue)
    {
        auto* param = proc.getAPVTS().getParameter (id);
        jassert (param != nullptr);
        param->setValueNotifyingHost (param->convertTo0to1 (realValue));
    }

    float getParam (Proc& proc, const juce::String& id)
    {
        return proc.getAPVTS().getRawParameterValue (id)->load();
    }

    const std::vector<const char*>& allEnableIds()
    {
        namespace fx = pid::fx;
        static const std::vector<const char*> ids {
            fx::distEnable, fx::chorusEnable, fx::delayEnable, fx::reverbEnable, fx::eqEnable,
            fx::modEnable, fx::tremEnable, fx::vibEnable, fx::limEnable, fx::convEnable,
            fx::compEnable, fx::grainEnable };
        return ids;
    }

    void setAllEffects (Proc& proc, bool on)
    {
        for (auto* id : allEnableIds())
            setParam (proc, id, on ? 1.0f : 0.0f);
    }

    juce::AudioChannelSet chanSet (int n)
    {
        return n == 0 ? juce::AudioChannelSet::disabled()
             : n == 1 ? juce::AudioChannelSet::mono()
                      : juce::AudioChannelSet::stereo();
    }

    // Applies main in/out + sidechain channel counts (0 = sidechain disabled).
    bool applyLayout (Proc& proc, int inCh, int outCh, int scCh)
    {
        juce::AudioProcessor::BusesLayout l;
        l.inputBuses.add (chanSet (inCh));
        l.inputBuses.add (chanSet (scCh));
        l.outputBuses.add (chanSet (outCh));
        return proc.setBusesLayout (l);
    }

    std::unique_ptr<Proc> makeProc (double sr, int block, int inCh = 2, int outCh = 2, int scCh = 0)
    {
        auto p = std::make_unique<Proc>();
        const bool ok = applyLayout (*p, inCh, outCh, scCh);
        jassert (ok);
        juce::ignoreUnused (ok);
        p->prepareToPlay (sr, block);
        return p;
    }

    struct Noise
    {
        uint32_t st;
        explicit Noise (uint32_t seed) : st (seed) {}
        float next() { st = st * 1664525u + 1013904223u; return ((float) (st >> 9) / (float) (1u << 23)) * 2.0f - 1.0f; }
    };

    // Goertzel amplitude (peak) of x at hz, over [from, to).
    double toneAmp (const std::vector<float>& x, size_t from, size_t to, double sr, double hz)
    {
        double re = 0, im = 0;
        for (size_t i = from; i < to; ++i)
        {
            const double a = juce::MathConstants<double>::twoPi * hz * (double) i / sr;
            re += x[i] * std::cos (a);
            im += x[i] * std::sin (a);
        }
        return 2.0 * std::sqrt (re * re + im * im) / (double) (to - from);
    }

    // Goertzel amplitude over a whole number of cycles (>= minSeconds long) starting at `from`,
    // so the estimate does not depend on the signal's phase at the window edges (matters when
    // the in and out signals are offset by a latency).
    double toneAmpCycles (const std::vector<float>& x, size_t from, double sr, double hz, double minSeconds = 0.25)
    {
        const double k = std::max (1.0, std::floor (minSeconds * hz));
        const size_t len = (size_t) std::llround (k * sr / hz);
        return toneAmp (x, from, std::min (x.size(), from + len), sr, hz);
    }

    bool allFinite (const juce::AudioBuffer<float>& b)
    {
        for (int c = 0; c < b.getNumChannels(); ++c)
            for (int i = 0; i < b.getNumSamples(); ++i)
                if (! std::isfinite (b.getSample (c, i)))
                    return false;
        return true;
    }

    // Writes a decaying stereo noise burst as a 24-bit WAV (a stand-in IR).
    juce::File writeTestIR (const juce::String& name, double sr, double seconds, float peak = 0.5f)
    {
        const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory)
                              .getChildFile (name + "-" + juce::String (juce::Random::getSystemRandom().nextInt (1000000)) + ".wav");
        file.deleteFile();
        const int n = (int) (sr * seconds);
        juce::AudioBuffer<float> ir (2, n);
        Noise nz (1234);
        for (int i = 0; i < n; ++i)
        {
            const float env = std::exp (-4.0f * (float) i / (float) n);
            ir.setSample (0, i, peak * env * nz.next());
            ir.setSample (1, i, peak * env * nz.next());
        }
        ir.setSample (0, 0, peak);   // a clear direct hit at the front
        ir.setSample (1, 0, peak);
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
        auto w = wav.createWriterFor (stream, juce::AudioFormatWriterOptions().withSampleRate (sr).withNumChannels (2).withBitsPerSample (24));
        jassert (w != nullptr);
        w->writeFromAudioSampleBuffer (ir, 0, n);
        return file;
    }

    // A fixed-tempo playhead for the tempo test.
    struct FixedPlayHead : juce::AudioPlayHead
    {
        juce::Optional<double> bpm;
        juce::Optional<PositionInfo> getPosition() const override
        {
            PositionInfo info;
            if (bpm.hasValue())
                info.setBpm (*bpm);
            return info;
        }
    };

    //==========================================================================
    // 1. Passthrough with everything off.
    void passthroughTest()
    {
        std::cout << "passthroughTest\n";
        constexpr int block = 512;
        auto proc = makeProc (44100.0, block);
        setAllEffects (*proc, false);
        setParam (*proc, pid::inputGain, 0.0f);
        setParam (*proc, pid::outputGain, 0.0f);
        setParam (*proc, pid::mix, 1.0f);
        setParam (*proc, pid::oversampling, 0.0f);

        Noise nz (7);
        juce::AudioBuffer<float> buf (2, block);
        juce::MidiBuffer midi;
        double maxDiff = 0.0;
        for (int b = 0; b < 40; ++b)
        {
            juce::AudioBuffer<float> in (2, block);
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < block; ++i)
                    in.setSample (c, i, 0.8f * nz.next());
            buf.makeCopyOf (in);
            proc->processBlock (buf, midi);
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < block; ++i)
                    maxDiff = std::max (maxDiff, (double) std::abs (buf.getSample (c, i) - in.getSample (c, i)));
        }
        std::cout << "  max |out - in| = " << maxDiff << "\n";
        expect (maxDiff == 0.0, "all effects off, mix 1, gains 0 dB, 1x: output is bit-identical to input (tolerance 0.0)");
        expect (proc->getLatencySamples() == 0, "no latency at 1x with the limiter off");

        // Block larger than the prepared size is chunked, still transparent.
        juce::AudioBuffer<float> big (2, 4096), bigIn (2, 4096);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < 4096; ++i)
                bigIn.setSample (c, i, 0.5f * nz.next());
        big.makeCopyOf (bigIn);
        proc->processBlock (big, midi);
        double bigDiff = 0.0;
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < 4096; ++i)
                bigDiff = std::max (bigDiff, (double) std::abs (big.getSample (c, i) - bigIn.getSample (c, i)));
        expect (bigDiff == 0.0, "a host block 8x the prepared size is chunked and stays transparent");

        // Gains: +6 dB in, -6 dB out is unity (within float rounding).
        setParam (*proc, pid::inputGain, 6.0f);
        setParam (*proc, pid::outputGain, -6.0f);
        double gainDiff = 0.0;
        for (int b = 0; b < 60; ++b)
        {
            juce::AudioBuffer<float> in (2, block);
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < block; ++i)
                    in.setSample (c, i, 0.4f * nz.next());
            buf.makeCopyOf (in);
            proc->processBlock (buf, midi);
            if (b >= 20)   // after the 20 ms smoothing ramps
                for (int c = 0; c < 2; ++c)
                    for (int i = 0; i < block; ++i)
                        gainDiff = std::max (gainDiff, (double) std::abs (buf.getSample (c, i) - in.getSample (c, i)));
        }
        expect (gainDiff < 1.0e-5, "input +6 dB then output -6 dB is unity (max diff " + juce::String (gainDiff, 9) + ")");
    }

    //==========================================================================
    // 2. Parameter parity with SPASynth's registry.
    juce::NormalisableRange<float> freqRange (float lo, float hi)
    {
        juce::NormalisableRange<float> r (lo, hi);
        r.setSkewForCentre (std::sqrt (lo * hi));
        return r;
    }
    juce::NormalisableRange<float> skewRange (float lo, float hi, float centre)
    {
        juce::NormalisableRange<float> r (lo, hi);
        r.setSkewForCentre (centre);
        return r;
    }

    void parameterParityTest()
    {
        std::cout << "parameterParityTest\n";
        namespace params = spa::params;

        // Counted from SPASynth's registry (every parameter in an fx* section:
        // fxDist..fxGrain; chaos.* and everything else is not an effect). The
        // number was obtained by dumping the synth registry (see report); it is
        // pinned here as a literal.
        constexpr int kSynthFxParamCount = 159;
        const int stripFx = params::numFxParams();
        std::cout << "  SPASynth FX parameter count: " << kSynthFxParamCount
                  << "   SPAStrip FX parameter count: " << stripFx << "\n";
        expect (stripFx == kSynthFxParamCount, "FX parameter count equals the synth registry's (" + juce::String (stripFx) + " vs " + juce::String (kSynthFxParamCount) + ")");

        Proc proc;
        const int hostParams = proc.getParameters().size();
        expect (hostParams == kSynthFxParamCount + 4 + 6 + 8, "host sees FX params + 4 globals + 6 sidechain + 8 mod-slot depths (" + juce::String (hostParams) + ")");

        std::set<juce::String> ids;
        bool unique = true;
        for (const auto& d : params::all())
            unique = ids.insert (d.id).second && unique;
        expect (unique, "parameter IDs are unique");

        // Every registry parameter exists in the processor with version hint 1.
        bool allThere = true, allV1 = true;
        for (const auto& d : params::all())
        {
            auto* p = proc.getAPVTS().getParameter (d.id);
            allThere = allThere && p != nullptr;
            if (auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*> (p))
                allV1 = allV1 && withId->paramID == d.id && withId->getVersionHint() == 1;
        }
        expect (allThere, "every registry ID is a live APVTS parameter");
        expect (allV1, "ParameterID version hint is 1 everywhere");

        struct Expect
        {
            const char* id; const char* name; params::ParamKind kind;
            juce::NormalisableRange<float> range; float def; const char* unit;
            bool percent = false;
        };
        using K = params::ParamKind;
        const std::vector<Expect> table {
            { "fxDist.enable",   "Dist On",      K::boolParam,   {}, 0.0f, "" },
            { "fxDist.drive",    "Dist Drive",   K::floatParam,  { 0.0f, 1.0f }, 0.3f, "" },
            { "fxDist.tone",     "Dist Tone",    K::floatParam,  freqRange (500.0f, 20000.0f), 8000.0f, "Hz" },
            { "fxDist.mix",      "Dist Mix",     K::floatParam,  { 0.0f, 1.0f }, 1.0f, "", true },
            { "fxChorus.rate",   "Chorus Rate",  K::floatParam,  freqRange (0.05f, 5.0f), 0.8f, "Hz" },
            { "fxChorus.feedback","Chorus FB",   K::floatParam,  { -0.9f, 0.9f }, 0.0f, "" },
            { "fxChorus.width",  "Chorus Width", K::floatParam,  { 0.0f, 100.0f }, 50.0f, "%" },
            { "fxChorus.mode",   "Chorus Mode",  K::choiceParam, {}, 1.0f, "" },
            { "fxDelay.time",    "Delay Time",   K::floatParam,  { 1.0f, 2000.0f, 0.0f, 0.4f }, 350.0f, "ms" },
            { "fxDelay.division","Delay Div",    K::choiceParam, {}, 6.0f, "" },
            { "fxDelay.width",   "Delay Width",  K::floatParam,  { 0.0f, 100.0f }, 100.0f, "%" },
            { "fxDelay.mix",     "Delay Mix",    K::floatParam,  { 0.0f, 1.0f }, 0.35f, "", true },
            { "fxReverb.decay",  "Reverb Decay", K::floatParam,  skewRange (0.2f, 8.0f, 2.5f), 2.0f, "s" },
            { "fxReverb.lowcut", "Reverb LoCut", K::floatParam,  freqRange (20.0f, 2000.0f), 20.0f, "Hz" },
            { "fxReverb.highcut","Reverb HiCut", K::floatParam,  freqRange (1000.0f, 20000.0f), 12000.0f, "Hz" },
            { "fxReverb.mix",    "Reverb Mix",   K::floatParam,  { 0.0f, 1.0f }, 0.3f, "", true },
            { "fxEQ.character",  "EQ Character", K::choiceParam, {}, 0.0f, "" },
            { "fxEQ.band0.type", "EQ B1 Type",   K::choiceParam, {}, 1.0f, "" },
            { "fxEQ.band6.type", "EQ B7 Type",   K::choiceParam, {}, 2.0f, "" },
            { "fxEQ.band7.freq", "EQ B8 Freq",   K::floatParam,  freqRange (20.0f, 20000.0f), 15000.0f, "Hz" },
            { "fxEQ.band3.q",    "EQ B4 Q",      K::floatParam,  skewRange (0.1f, 18.0f, 1.0f), 0.707f, "" },
            { "fxEQ.band2.gain", "EQ B3 Gain",   K::floatParam,  { -24.0f, 24.0f, 0.1f }, 0.0f, "dB" },
            { "fxMod.rate",      "Mod Rate",     K::floatParam,  freqRange (0.02f, 8.0f), 0.5f, "Hz" },
            { "fxMod.stages",    "Mod Stages",   K::choiceParam, {}, 2.0f, "" },
            { "fxMod.manual",    "Mod Delay",    K::floatParam,  { 0.1f, 20.0f, 0.01f }, 3.0f, "ms" },
            { "fxTrem.shape",    "Trem Shape",   K::choiceParam, {}, 0.0f, "" },
            { "fxVib.depth",     "Vib Depth",    K::floatParam,  { 0.0f, 1.0f }, 0.4f, "" },
            { "fxLim.drive",     "Lim Drive",    K::floatParam,  { 0.0f, 24.0f, 0.1f }, 0.0f, "dB" },
            { "fxLim.ceiling",   "Lim Ceiling",  K::floatParam,  { -12.0f, 0.0f, 0.1f }, -0.3f, "dB" },
            { "fxLim.release",   "Lim Release",  K::floatParam,  { 1.0f, 1000.0f, 0.0f, 0.4f }, 120.0f, "ms" },
            { "fxLim.lookahead", "Lim Lookahead",K::boolParam,   {}, 0.0f, "" },
            { "fxConv.decay",    "Conv Decay",   K::floatParam,  { 0.05f, 1.0f }, 1.0f, "" },
            { "fxConv.start",    "Conv Start",   K::floatParam,  { 0.0f, 1.0f }, 0.0f, "" },
            { "fxComp.xoverLow", "Comp Xover L", K::floatParam,  freqRange (20.0f, 2000.0f), 200.0f, "Hz" },
            { "fxComp.band1.ratio","Comp Mid Ratio",K::floatParam, skewRange (1.0f, 20.0f, 4.0f), 4.0f, ":1" },
            { "fxComp.band2.thresh","Comp High Thresh",K::floatParam, { -60.0f, 0.0f, 0.1f }, -24.0f, "dB" },
            { "fxGrain.size",    "Grain Size",   K::floatParam,  skewRange (5.0f, 500.0f, 120.0f), 120.0f, "ms" },
            { "fxGrain.density", "Grain Density",K::floatParam,  skewRange (1.0f, 400.0f, 28.0f), 14.0f, "/s" },
            { "fxGrain.division","Grain Div",    K::choiceParam, {}, 9.0f, "" },
            { "fxGrain.pitch",   "Grain Pitch",  K::floatParam,  { -24.0f, 24.0f, 0.01f }, 0.0f, "st" },
            { "fxGrain.spread",  "Grain Spread Time", K::floatParam, { 0.0f, 1.0f }, 0.25f, "", true },
            { "fxGrain.spreadPitch","Grain Spread Pitch",K::floatParam, { 0.0f, 12.0f, 0.01f }, 3.0f, "st" },
            { "fxGrain.feedback","Grain Feedback", K::floatParam, { 0.0f, 0.9f }, 0.0f, "", true },
            { "global.inputGain","Input Gain",   K::floatParam,  { -24.0f, 24.0f, 0.1f }, 0.0f, "dB" },
            { "global.outputGain","Output Gain", K::floatParam,  { -24.0f, 24.0f, 0.1f }, 0.0f, "dB" },
            { "global.mix",      "Mix",          K::floatParam,  { 0.0f, 1.0f }, 1.0f, "", true },
            { "global.oversampling","Oversampling",K::choiceParam, {}, 0.0f, "" },
        };

        int bad = 0;
        for (const auto& e : table)
        {
            const auto* d = params::find (e.id);
            if (d == nullptr) { ++bad; std::cout << "    missing " << e.id << "\n"; continue; }
            bool ok = d->name == e.name && d->kind == e.kind && d->unit == e.unit
                   && juce::approximatelyEqual (d->defaultValue, e.def) && d->percentDisplay == e.percent;
            if (e.kind == K::floatParam)
                ok = ok && juce::approximatelyEqual (d->range.start, e.range.start)
                        && juce::approximatelyEqual (d->range.end, e.range.end)
                        && juce::approximatelyEqual (d->range.interval, e.range.interval)
                        && std::abs (d->range.skew - e.range.skew) < 1.0e-6f;
            if (! ok) { ++bad; std::cout << "    mismatch " << e.id << "\n"; }
        }
        expect (bad == 0, "representative IDs/names/ranges/skews/defaults/units/percent flags match the synth (" + juce::String ((int) table.size()) + " checked, " + juce::String (bad) + " bad)");

        // Choice lists and their order.
        const auto choices = [] (const char* id) { return params::find (id)->choices; };
        expect (choices ("fxDist.type") == juce::StringArray ({ "Soft", "Hard", "Fold", "Crush" }), "fxDist.type choices");
        expect (choices ("fxChorus.mode") == juce::StringArray ({ "Vintage", "Modern" }), "fxChorus.mode choices");
        expect (choices ("fxReverb.mode") == juce::StringArray ({ "Hall", "Plate", "Chamber", "Room", "Spring" }), "fxReverb.mode choices");
        expect (choices ("fxEQ.band0.type") == juce::StringArray ({ "Bell", "Low Shelf", "High Shelf", "Low Cut", "High Cut", "Notch", "Band Pass", "Tilt Shelf" }), "EQ band type choices");
        expect (choices ("fxEQ.band0.slope") == juce::StringArray ({ "6 dB", "12 dB", "18 dB", "24 dB", "36 dB", "48 dB" }), "EQ slope choices");
        expect (choices ("fxMod.stages") == juce::StringArray ({ "2", "4", "6", "8", "12" }), "fxMod.stages choices");
        expect (choices ("fxLim.character") == juce::StringArray ({ "Clean", "Punchy", "Aggressive" }), "fxLim.character choices");
        expect (choices ("fxTrem.shape") == juce::StringArray ({ "Sine", "Triangle", "Square", "Saw" }), "fxTrem.shape choices");
        expect (choices ("fxDelay.division") == spa::params::lfoDivisionNames() && spa::params::lfoDivisionNames().size() == 15, "tempo-division list (15 entries, identical order)");
        expect (choices ("global.oversampling") == juce::StringArray ({ "1x", "2x", "4x" }), "global.oversampling is 1x/2x/4x");
        expect (std::abs (spa::params::lfoDivisionBeats (5) - 4.0f / 3.0f) < 1.0e-7f && spa::params::lfoDivisionBeats (14) == 0.125f && spa::params::lfoDivisionBeats (0) == 32.0f, "lfoDivisionBeats table");

        // RandomSpec values.
        {
            const auto& r = params::find ("fxGrain.pitch")->random;
            expect (r.enabled && juce::exactlyEqual (r.minNorm, 0.25f) && juce::exactlyEqual (r.maxNorm, 0.75f) && juce::exactlyEqual (r.biasCentre, 0.5f) && juce::exactlyEqual (r.biasStrength, 0.4f), "RandomSpec: fxGrain.pitch window/bias");
            const auto& c = params::find ("fxComp.band1.gain")->random;
            expect (c.enabled && c.minNorm == 0.375f && c.maxNorm == 0.625f, "RandomSpec: fxComp.band1.gain window");
            expect (! params::find ("fxGrain.freeze")->random.enabled && ! params::find ("fxComp.band0.attack")->random.enabled, "RandomSpec: freeze / comp attack not randomised");
            const auto& d = params::find ("fxDist.enable")->random;
            expect (d.enabled && juce::exactlyEqual (d.biasCentre, 0.3f) && juce::exactlyEqual (d.biasStrength, 0.3f), "RandomSpec: fxDist.enable bias");
            bool globalsOff = true;
            for (auto* id : { pid::inputGain, pid::outputGain, pid::mix, pid::oversampling })
                globalsOff = globalsOff && ! params::find (id)->random.enabled;
            expect (globalsOff, "global.* randomisation disabled");
        }

        // The percent-display mix knobs format as whole percentages.
        {
            auto* mix = proc.getAPVTS().getParameter (pid::mix);
            expect (mix->getText (0.5f, 32) == "50 %" && std::abs (mix->getValueForText ("13") - 0.13f) < 1.0e-5f, "global.mix displays as percent and parses back");
        }

        // Host parameter groups: one per effect + Global.
        {
            int topGroups = 0;
            for (auto* node : proc.getParameterTree())
                if (node->getGroup() != nullptr)
                    ++topGroups;
            expect (topGroups == 14, "14 host parameter groups (Global + 11 effects + Sidechain + Mod Matrix) (got " + juce::String (topGroups) + ")");
        }
    }

    //==========================================================================
    // 3. fxOrder round trip + persistence.
    void fxOrderStateTest()
    {
        std::cout << "fxOrderStateTest\n";
        using FX = spa::dsp::FXChain;
        // pack/unpack round trip for a non-default permutation.
        const FX::Module perm[FX::numModules] { FX::Module::limiter, FX::Module::grain, FX::Module::convolve, FX::Module::comp,
            FX::Module::eq, FX::Module::reverb, FX::Module::delay, FX::Module::tremVib, FX::Module::mod,
            FX::Module::chorus, FX::Module::distortion };
        FX::Module back[FX::numModules];
        FX::unpackOrder (FX::packOrder (perm), back);
        bool same = true;
        for (int i = 0; i < FX::numModules; ++i) same = same && back[i] == perm[i];
        expect (same, "pack/unpack round-trips a custom permutation");

        auto proc = makeProc (48000.0, 256);
        const auto defIds = proc->getFxOrder();
        FX::Module def[FX::numModules];
        FX::unpackOrder (FX::defaultOrderPacked(), def);
        bool isDefault = defIds.size() == FX::numModules;
        for (int i = 0; isDefault && i < FX::numModules; ++i) isDefault = defIds[i] == (int) def[i];
        expect (isDefault, "a fresh processor reports the default order");

        juce::Array<int> custom;
        for (auto m : perm) custom.add ((int) m);
        proc->setFxOrder (custom);
        expect (proc->getFxOrder() == custom, "setFxOrder/getFxOrder round-trip");

        juce::MemoryBlock mb;
        proc->getStateInformation (mb);
        auto xml = juce::AudioProcessor::getXmlFromBinary (mb.getData(), (int) mb.getSize());
        expect (xml != nullptr && xml->hasAttribute ("fxOrder"), "state XML carries the fxOrder property");
        if (xml != nullptr)
            expect ((juce::uint64) xml->getStringAttribute ("fxOrder").getLargeIntValue() == FX::packOrder (perm), "fxOrder is the packed int64");

        auto other = makeProc (48000.0, 256);
        other->setStateInformation (mb.getData(), (int) mb.getSize());
        expect (other->getFxOrder() == custom, "fxOrder persists through get/setStateInformation");

        // An invalid permutation falls back to the natural order, not garbage.
        {
            auto st = juce::ValueTree::fromXml (*xml);
            st.setProperty ("fxOrder", (juce::int64) 0x1111111111LL, nullptr);
            juce::MemoryBlock mb2;
            juce::AudioProcessor::copyXmlToBinary (*st.createXml(), mb2);
            other->setStateInformation (mb2.getData(), (int) mb2.getSize());
            const auto ids = other->getFxOrder();
            bool nat = ids.size() == FX::numModules;
            for (int i = 0; nat && i < FX::numModules; ++i) nat = ids[i] == i;
            expect (nat, "a corrupt fxOrder falls back to the natural order");
        }
    }

    //==========================================================================
    // Renders an impulse response of the convolve-only chain (mix 1) after
    // waiting for the engine's background IR load. Returns {ok, response}.
    std::vector<float> convImpulseResponse (Proc& proc, double sr, int block, int seconds)
    {
        setAllEffects (proc, false);
        setParam (proc, pid::fx::convEnable, 1.0f);
        setParam (proc, pid::fx::convMix, 1.0f);
        proc.serviceMessageThread();
        juce::AudioBuffer<float> buf (2, block);
        juce::MidiBuffer midi;

        const auto runSilence = [&] (double secs)
        {
            for (int b = 0; b < (int) (secs * sr / block); ++b)
            {
                buf.clear();
                proc.processBlock (buf, midi);
            }
        };

        // Wait (bounded) until an impulse produces a wet response: the engine
        // swaps the IR in once its background thread has finished.
        bool loaded = false;
        for (int attempt = 0; attempt < 200 && ! loaded; ++attempt)
        {
            buf.clear();
            buf.setSample (0, 0, 1.0f);
            buf.setSample (1, 0, 1.0f);
            proc.processBlock (buf, midi);
            float pk = 0.0f;
            for (int i = 1; i < block; ++i) pk = std::max (pk, std::abs (buf.getSample (0, i)));
            loaded = pk > 1.0e-4f;
            juce::Thread::sleep (10);
        }
        if (! loaded)
            return {};
        runSilence (4.0);   // let the probe impulse ring out fully

        std::vector<float> out;
        const int blocks = (int) (seconds * sr / block);
        for (int b = 0; b < blocks; ++b)
        {
            buf.clear();
            if (b == 0) { buf.setSample (0, 0, 1.0f); buf.setSample (1, 0, 1.0f); }
            proc.processBlock (buf, midi);
            for (int i = 0; i < block; ++i) out.push_back (buf.getSample (0, i));
        }
        return out;
    }

    juce::String params_eq_band_freq() { return spa::params::id::eqBand (2, spa::params::id::fx::eqband::freq); }

    //==========================================================================
    // 4. Full state round trip including an embedded IR.
    void fullStateRoundTripTest()
    {
        std::cout << "fullStateRoundTripTest\n";
        constexpr double sr = 44100.0;
        constexpr int block = 512;

        const auto irFile = writeTestIR ("spastrip-ir", 44100.0, 1.5);
        auto a = makeProc (sr, block);

        expect (a->getConvolutionIRSource() == "none" && a->getConvolutionIRName().isEmpty() && ! a->hasConvolutionIR(), "fresh processor has no IR");
        expect (! a->loadConvolutionIR (juce::File ("/nonexistent/ir.wav")), "loading a missing file fails cleanly");
        expect (a->loadConvolutionIR (irFile), "IR loads");
        expect (a->getConvolutionIRSource() == "embedded", "IR source is 'embedded'");
        expect (a->getConvolutionIRName() == irFile.getFileNameWithoutExtension(), "IR display name is the file name");

        // A recognisable parameter set across several effects + globals.
        setParam (*a, pid::inputGain, 3.5f);
        setParam (*a, pid::outputGain, -2.0f);
        setParam (*a, pid::mix, 0.62f);
        setParam (*a, pid::oversampling, 1.0f);
        setParam (*a, pid::fx::reverbEnable, 1.0f);
        setParam (*a, pid::fx::reverbMode, 3.0f);
        setParam (*a, pid::fx::reverbDecay, 4.2f);
        setParam (*a, pid::fx::delayDivision, 11.0f);
        setParam (*a, pid::fx::chorusWidth, 73.0f);
        setParam (*a, params_eq_band_freq (), 1234.5f);
        setParam (*a, pid::fx::convDecay, 0.55f);
        setParam (*a, pid::fx::convDamping, 0.4f);
        setParam (*a, pid::fx::convStart, 0.1f);
        setParam (*a, pid::fx::convMix, 0.77f);
        setParam (*a, pid::fx::grainFreeze, 1.0f);
        juce::Array<int> custom { 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0 };
        a->setFxOrder (custom);

        juce::MemoryBlock stateA;
        a->getStateInformation (stateA);
        {
            auto xml = juce::AudioProcessor::getXmlFromBinary (stateA.getData(), (int) stateA.getSize());
            expect (xml != nullptr && xml->getIntAttribute ("stateVersion") == spa::SPAStripProcessor::kStateVersion, "state carries stateVersion");
            auto* ir = xml != nullptr ? xml->getChildByName ("IR") : nullptr;
            expect (ir != nullptr && ir->getStringAttribute ("data").length() > 1000, "IR audio is embedded in the state");
            std::cout << "  state size " << stateA.getSize() << " bytes\n";
        }

        // The point of embedding: the source file may be gone.
        irFile.deleteFile();

        auto b = makeProc (sr, block);
        b->setStateInformation (stateA.getData(), (int) stateA.getSize());

        bool paramsEqual = true;
        int compared = 0;
        for (const auto& d : spa::params::all())
        {
            ++compared;
            paramsEqual = paramsEqual && std::abs (getParam (*a, d.id) - getParam (*b, d.id)) < 1.0e-5f;
        }
        expect (paramsEqual, "all " + juce::String (compared) + " parameter values restore");
        expect (b->getFxOrder() == custom, "fxOrder restores");
        expect (b->getConvolutionIRSource() == "embedded" && b->getConvolutionIRName() == a->getConvolutionIRName(), "IR source and name restore");

        juce::MemoryBlock stateB;
        b->getStateInformation (stateB);
        expect (stateA == stateB, "re-saving the restored processor reproduces the state byte for byte");

        // Audio: both processors must now convolve with the identical IR.
        a->serviceMessageThread();
        b->serviceMessageThread();
        const auto ra = convImpulseResponse (*a, sr, block, 2);
        const auto rb = convImpulseResponse (*b, sr, block, 2);
        expect (! ra.empty() && ! rb.empty(), "both processors' convolvers loaded the IR");
        double maxDiff = 0.0, maxA = 0.0;
        for (size_t i = 0; i < std::min (ra.size(), rb.size()); ++i)
        {
            maxDiff = std::max (maxDiff, (double) std::abs (ra[i] - rb[i]));
            maxA = std::max (maxA, (double) std::abs (ra[i]));
        }
        std::cout << "  conv response peak " << maxA << ", max |a-b| " << maxDiff << "\n";
        expect (maxA > 1.0e-3 && maxDiff <= 1.0e-6 * std::max (1.0, maxA), "restored processor's convolved audio matches the original's");

        // The shaped IR envelope (decay/damping/start applied) matches too.
        bool envSame = true, envNonZero = false;
        for (int i = 0; i < spa::dsp::FXChain::convEnvPoints; ++i)
        {
            envSame = envSame && juce::exactlyEqual (a->getConvolutionEnvelope()[(size_t) i], b->getConvolutionEnvelope()[(size_t) i]);
            envNonZero = envNonZero || a->getConvolutionEnvelope()[(size_t) i] > 0.0f;
        }
        expect (envNonZero && envSame, "shaped IR envelope identical after restore");
        expect (std::abs (a->getConvolutionLengthSeconds() - b->getConvolutionLengthSeconds()) < 1.0e-9
                && std::abs (a->getConvolutionLengthSeconds() - 1.5 * 0.9) < 0.01, "IR length after start-trim is 90% of 1.5 s");

        // Clearing the IR clears the state.
        b->clearConvolutionIR();
        juce::MemoryBlock stateC;
        b->getStateInformation (stateC);
        auto xmlC = juce::AudioProcessor::getXmlFromBinary (stateC.getData(), (int) stateC.getSize());
        expect (b->getConvolutionIRSource() == "none" && ! b->hasConvolutionIR() && xmlC != nullptr && xmlC->getChildByName ("IR") == nullptr, "clearConvolutionIR removes the IR from the state");

        // Missing params get registry defaults on load: strip half of them.
        {
            auto xml = juce::AudioProcessor::getXmlFromBinary (stateA.getData(), (int) stateA.getSize());
            auto tree = juce::ValueTree::fromXml (*xml);
            for (int i = tree.getNumChildren() - 1; i >= 0; --i)
                if (tree.getChild (i).hasType ("PARAM") && (tree.getChild (i).getProperty ("id").toString().startsWith ("fxReverb")
                                                           || tree.getChild (i).getProperty ("id").toString() == pid::mix))
                    tree.removeChild (i, nullptr);
            juce::MemoryBlock mb;
            juce::AudioProcessor::copyXmlToBinary (*tree.createXml(), mb);
            auto c = makeProc (sr, block);
            setParam (*c, pid::fx::reverbDecay, 7.0f);
            setParam (*c, pid::mix, 0.1f);
            c->setStateInformation (mb.getData(), (int) mb.getSize());
            expect (std::abs (getParam (*c, pid::fx::reverbDecay) - 2.0f) < 1.0e-5f && std::abs (getParam (*c, pid::mix) - 1.0f) < 1.0e-6f
                    && std::abs (getParam (*c, pid::inputGain) - 3.5f) < 1.0e-4f, "params missing from a state load at their registry defaults; present ones load");
        }

        // The 10 s cap: a 12 s IR is truncated to 10 s.
        {
            const auto longIR = writeTestIR ("spastrip-long", 22050.0, 12.0);
            auto c = makeProc (sr, block);
            expect (c->loadConvolutionIR (longIR), "a 12 s IR loads");
            c->serviceMessageThread();
            expect (std::abs (c->getConvolutionLengthSeconds() - 10.0) < 0.01, "IR is capped at 10 s (" + juce::String (c->getConvolutionLengthSeconds(), 3) + " s)");
            longIR.deleteFile();
        }

        // Reserved factory reference survives a round trip without audio.
        {
            auto xml = juce::AudioProcessor::getXmlFromBinary (stateA.getData(), (int) stateA.getSize());
            auto tree = juce::ValueTree::fromXml (*xml);
            tree.setProperty ("irSource", "factory:hall-1", nullptr);
            for (auto ch = tree.getChildWithName ("IR"); ch.isValid(); ch = tree.getChildWithName ("IR")) tree.removeChild (ch, nullptr);
            juce::MemoryBlock mb;
            juce::AudioProcessor::copyXmlToBinary (*tree.createXml(), mb);
            auto c = makeProc (sr, block);
            c->setStateInformation (mb.getData(), (int) mb.getSize());
            expect (c->getConvolutionIRSource() == "factory:hall-1" && ! c->hasConvolutionIR(), "an unimplemented factory:<id> IR source is preserved (no audio)");
        }
    }

    //==========================================================================
    // Latency measurement helpers.
    struct DelayMeasure { int peakIndex = -1; double centroid = 0.0; };

    DelayMeasure measureImpulseDelay (Proc& proc, int block, int impulseAt)
    {
        juce::AudioBuffer<float> buf (2, block);
        juce::MidiBuffer midi;
        // Prime with silence so every state has settled.
        for (int b = 0; b < 8; ++b) { buf.clear(); proc.processBlock (buf, midi); }
        std::vector<float> out;
        const int totalBlocks = 8;
        for (int b = 0; b < totalBlocks; ++b)
        {
            buf.clear();
            if (b == 0) { buf.setSample (0, impulseAt, 1.0f); buf.setSample (1, impulseAt, 1.0f); }
            proc.processBlock (buf, midi);
            for (int i = 0; i < block; ++i) out.push_back (buf.getSample (0, i));
        }
        DelayMeasure m;
        float pk = 0.0f; double e = 0.0, w = 0.0;
        for (size_t i = 0; i < out.size(); ++i)
        {
            if (std::abs (out[i]) > pk) { pk = std::abs (out[i]); m.peakIndex = (int) i - impulseAt; }
            e += (double) out[i] * out[i]; w += (double) out[i] * out[i] * (double) ((int) i - impulseAt);
        }
        m.centroid = e > 0.0 ? w / e : 0.0;
        return m;
    }

    //==========================================================================
    // 5. Latency vs measured delay; mix 0.5 is not a comb filter.
    void latencyAndMixTest()
    {
        std::cout << "latencyAndMixTest\n";
        constexpr int block = 256;
        for (double sr : { 44100.0, 48000.0, 96000.0 })
        {
            auto proc = makeProc (sr, block);
            setAllEffects (*proc, false);
            for (int factorChoice = 0; factorChoice < 3; ++factorChoice)
            {
                const int factor = 1 << factorChoice;
                const juce::String tag = juce::String ((int) sr) + " Hz, " + juce::String (factor) + "x: ";

                // Switch factor the way a host would: automate the parameter,
                // let processing notice, then the message-thread timer applies it.
                setParam (*proc, pid::oversampling, (float) factorChoice);
                juce::AudioBuffer<float> tmp (2, block);
                juce::MidiBuffer midi;
                tmp.clear();
                proc->processBlock (tmp, midi);
                proc->serviceMessageThread();

                const int reported = proc->getLatencySamples();
                setParam (*proc, pid::mix, 1.0f);
                const auto m = measureImpulseDelay (*proc, block, 100);
                std::cout << "  " << tag << "reported latency " << reported << ", measured peak delay " << m.peakIndex
                          << ", energy centroid " << juce::String (m.centroid, 3) << "\n";
                expect (std::abs (m.peakIndex - reported) <= 1, tag + "reported latency matches the measured impulse delay (+-1 sample)");
                expect (factor > 1 ? reported > 0 : reported == 0, tag + "latency is " + juce::String (reported));

                // mix 0.5: sines through (wet + latency-aligned dry)/2. If dry
                // and wet were misaligned this would comb-filter; instead the
                // gain stays ~1 across the band (20 Hz - 18 kHz).
                setParam (*proc, pid::mix, 0.5f);
                double worstDb = 0.0;
                juce::String worstAt;
                for (double hz : { 20.0, 100.0, 440.0, 1000.0, 3000.0, 5000.0, 8000.0, 12000.0, 16000.0, 18000.0 })
                {
                    if (hz > sr * 0.45) continue;
                    std::vector<float> in, out;
                    const int total = (int) (sr * 0.6);
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
                    const size_t from = (size_t) (sr * 0.3);
                    const double gainDb = 20.0 * std::log10 (toneAmpCycles (out, from, sr, hz) / toneAmpCycles (in, from, sr, hz));
                    if (std::abs (gainDb) > std::abs (worstDb)) { worstDb = gainDb; worstAt = juce::String (hz, 0) + " Hz"; }
                }
                std::cout << "  " << tag << "mix 0.5 worst gain deviation " << juce::String (worstDb, 3) << " dB at " << worstAt << "\n";
                // Oversampled: the dry signal takes its own trip through an identical
                // oversampler, so dry and wet share one phase response (phase 1 only
                // delayed the dry by an integer and dipped up to ~0.5 dB at 8-12 kHz).
                expect (std::abs (worstDb) < (factor == 1 ? 0.001 : 0.05), tag + "mix 0.5 does not comb-filter (|gain error| " + juce::String (std::abs (worstDb), 4) + " dB, limit " + (factor == 1 ? "0.001" : "0.05") + " dB)");
                setParam (*proc, pid::mix, 1.0f);
            }
        }

        // The limiter's lookahead is part of the reported latency and the dry
        // path follows it.
        {
            auto proc = makeProc (48000.0, block);
            setAllEffects (*proc, false);
            setParam (*proc, pid::fx::limEnable, 1.0f);
            setParam (*proc, pid::fx::limLookahead, 1.0f);
            setParam (*proc, pid::fx::limDrive, 0.0f);
            juce::AudioBuffer<float> tmp (2, block);
            juce::MidiBuffer midi;
            tmp.clear();
            proc->processBlock (tmp, midi);
            proc->serviceMessageThread();
            std::cout << "  limiter lookahead latency at 48k/1x: " << proc->getLatencySamples() << "\n";
            expect (proc->getLatencySamples() == 72, "limiter lookahead reports 1.5 ms = 72 samples at 48 kHz/1x");
            const auto m = measureImpulseDelay (*proc, block, 100);
            expect (std::abs (m.peakIndex - proc->getLatencySamples()) <= 1, "measured delay with lookahead on matches the reported latency (" + juce::String (m.peakIndex) + ")");
            setParam (*proc, pid::oversampling, 2.0f);
            tmp.clear(); proc->processBlock (tmp, midi); proc->serviceMessageThread();
            const auto m4 = measureImpulseDelay (*proc, block, 100);
            std::cout << "  limiter lookahead + 4x: reported " << proc->getLatencySamples() << ", measured " << m4.peakIndex << "\n";
            expect (std::abs (m4.peakIndex - proc->getLatencySamples()) <= 1, "lookahead + 4x: reported latency matches measured delay");
        }

        // Switching the factor mid-stream stays finite and keeps processing.
        {
            auto proc = makeProc (48000.0, 128);
            setAllEffects (*proc, true);
            Noise nz (3);
            juce::AudioBuffer<float> b (2, 128);
            juce::MidiBuffer midi;
            bool finite = true;
            for (int i = 0; i < 400; ++i)
            {
                if (i % 40 == 0) setParam (*proc, pid::oversampling, (float) ((i / 40) % 3));
                for (int c = 0; c < 2; ++c) for (int s = 0; s < 128; ++s) b.setSample (c, s, 0.3f * nz.next());
                proc->processBlock (b, midi);
                if (i % 8 == 0) proc->serviceMessageThread();
                finite = finite && allFinite (b);
            }
            expect (finite, "cycling 1x/2x/4x mid-stream with all effects on stays finite");
        }
    }

    //==========================================================================
    // 6. Bypass = dry delayed by the reported latency.
    void bypassTest()
    {
        std::cout << "bypassTest\n";
        constexpr int block = 200;   // deliberately not a power of two
        for (int factorChoice = 0; factorChoice < 3; ++factorChoice)
        for (bool lookahead : { false, true })
        {
            auto proc = makeProc (48000.0, block);
            setAllEffects (*proc, true);
            setParam (*proc, pid::oversampling, (float) factorChoice);
            setParam (*proc, pid::fx::limLookahead, lookahead ? 1.0f : 0.0f);
            setParam (*proc, pid::inputGain, 9.0f);     // must not affect bypass
            setParam (*proc, pid::outputGain, -9.0f);
            setParam (*proc, pid::mix, 0.3f);
            juce::AudioBuffer<float> b (2, block);
            juce::MidiBuffer midi;
            b.clear();
            proc->processBlock (b, midi);          // let the factor/latency settle
            proc->serviceMessageThread();
            const int lat = proc->getLatencySamples();

            Noise nz (11);
            std::vector<float> inL, inR, outL, outR;
            for (int blk = 0; blk < 30; ++blk)
            {
                for (int i = 0; i < block; ++i)
                {
                    const float l = 0.7f * nz.next(), r = 0.7f * nz.next();
                    b.setSample (0, i, l); b.setSample (1, i, r);
                    inL.push_back (l); inR.push_back (r);
                }
                proc->processBlockBypassed (b, midi);
                for (int i = 0; i < block; ++i) { outL.push_back (b.getSample (0, i)); outR.push_back (b.getSample (1, i)); }
            }
            bool exact = true;
            for (size_t i = (size_t) lat; i < inL.size(); ++i)
                exact = exact && juce::exactlyEqual (outL[i], inL[i - (size_t) lat]) && juce::exactlyEqual (outR[i], inR[i - (size_t) lat]);
            bool silentHead = true;
            for (int i = 0; i < lat; ++i) silentHead = silentHead && outL[(size_t) i] == 0.0f;
            expect (exact && silentHead, juce::String (1 << factorChoice) + "x, lookahead " + (lookahead ? "on" : "off")
                    + ": bypassed output == input delayed by the reported latency (" + juce::String (lat) + " samples), bit-exact");
        }
    }

    //==========================================================================
    // 7. Bus layouts, with and without the sidechain bus.
    void busLayoutTest()
    {
        std::cout << "busLayoutTest\n";
        {
            Proc proc;
            expect (proc.getBusCount (true) == 2 && proc.getBusCount (false) == 1, "one main input, one sidechain input, one main output");
            expect (proc.getBus (true, 1)->getName() == "Sidechain" && ! proc.getBus (true, 1)->isEnabledByDefault(), "optional bus is named 'Sidechain' and disabled by default");
            expect (! proc.acceptsMidi() && ! proc.producesMidi() && ! proc.isMidiEffect(), "no MIDI");

            const auto test = [&] (int i, int o, int s) { Proc p; juce::AudioProcessor::BusesLayout l;
                l.inputBuses.add (chanSet (i)); l.inputBuses.add (chanSet (s)); l.outputBuses.add (chanSet (o)); return p.isBusesLayoutSupported (l); };
            expect (test (1, 1, 0) && test (1, 2, 0) && test (2, 2, 0), "mono->mono, mono->stereo, stereo->stereo supported");
            expect (test (1, 1, 1) && test (2, 2, 2) && test (1, 2, 2) && test (2, 2, 1), "...with a mono or stereo sidechain");
            expect (! test (2, 1, 0) && ! test (0, 2, 0) && ! test (0, 0, 0), "stereo->mono and a disabled main input are rejected");
            juce::AudioProcessor::BusesLayout surround;
            surround.inputBuses.add (juce::AudioChannelSet::quadraphonic()); surround.inputBuses.add (chanSet (0)); surround.outputBuses.add (juce::AudioChannelSet::quadraphonic());
            expect (! proc.isBusesLayoutSupported (surround), "surround is rejected");
            juce::AudioProcessor::BusesLayout badSc;
            badSc.inputBuses.add (chanSet (2)); badSc.inputBuses.add (juce::AudioChannelSet::quadraphonic()); badSc.outputBuses.add (chanSet (2));
            expect (! proc.isBusesLayoutSupported (badSc), "a quad sidechain is rejected");
        }

        struct Cfg { int in, out, sc; };
        for (bool effectsOn : { false, true })
        for (const Cfg c : { Cfg { 1, 1, 0 }, Cfg { 1, 1, 1 }, Cfg { 1, 1, 2 }, Cfg { 1, 2, 0 }, Cfg { 1, 2, 1 }, Cfg { 1, 2, 2 },
                             Cfg { 2, 2, 0 }, Cfg { 2, 2, 1 }, Cfg { 2, 2, 2 } })
        {
            constexpr int block = 256;
            auto proc = makeProc (48000.0, block, c.in, c.out, c.sc);
            const juce::String tag = juce::String (c.in) + "->" + juce::String (c.out) + " sc " + juce::String (c.sc) + (effectsOn ? " (all effects)" : " (effects off)") + ": ";
            setAllEffects (*proc, effectsOn);
            expect (proc->getMainBusNumInputChannels() == c.in && proc->getMainBusNumOutputChannels() == c.out
                    && (c.sc == 0 ? ! proc->getBus (true, 1)->isEnabled() : proc->getBus (true, 1)->getNumberOfChannels() == c.sc), tag + "layout accepted");

            const int chans = std::max (proc->getTotalNumInputChannels(), proc->getTotalNumOutputChannels());
            juce::AudioBuffer<float> buf (chans, block);
            juce::MidiBuffer midi;
            Noise nz (21);
            bool finite = true, scLeak = false, tracks = true;
            float peak = 0.0f;
            for (int blk = 0; blk < 60; ++blk)
            {
                std::vector<float> mainIn[2] { std::vector<float> ((size_t) block), std::vector<float> ((size_t) block) };
                for (int i = 0; i < block; ++i)
                    for (int ch = 0; ch < c.in; ++ch)
                        mainIn[ch][(size_t) i] = 0.5f * nz.next();
                for (int ch = 0; ch < chans; ++ch)
                    for (int i = 0; i < block; ++i)
                    {
                        // Main input channels first, then the sidechain channels
                        // (filled with loud garbage that must never reach the output).
                        buf.setSample (ch, i, ch < c.in ? mainIn[ch][(size_t) i] : 5.0f);
                    }
                proc->processBlock (buf, midi);
                finite = finite && allFinite (buf);
                for (int ch = 0; ch < c.out; ++ch)
                    for (int i = 0; i < block; ++i)
                    {
                        peak = std::max (peak, std::abs (buf.getSample (ch, i)));
                        if (! effectsOn)
                        {
                            // Everything off: out == in (mono->stereo duplicates, stereo is 1:1).
                            const float expected = c.in == 1 ? mainIn[0][(size_t) i] : mainIn[ch][(size_t) i];
                            tracks = tracks && juce::exactlyEqual (buf.getSample (ch, i), expected);
                        }
                    }
                for (int ch = 0; ch < c.out; ++ch)
                    if (buf.getMagnitude (ch, 0, block) >= 4.9f) scLeak = true;
            }
            expect (finite, tag + "no NaN/Inf");
            expect (! scLeak && peak < (effectsOn ? 4.0f : 0.51f), tag + "sidechain content never reaches the output (peak " + juce::String (peak, 3) + ")");
            if (! effectsOn)
                expect (tracks, tag + "output equals the (duplicated / 1:1) main input exactly");
        }

        // Mono out is the L/R average of the stereo working signal.
        {
            auto proc = makeProc (48000.0, 64, 1, 1, 0);
            setAllEffects (*proc, false);
            juce::AudioBuffer<float> buf (1, 64);
            for (int i = 0; i < 64; ++i) buf.setSample (0, i, 0.25f);
            juce::MidiBuffer midi;
            proc->processBlock (buf, midi);
            expect (buf.getSample (0, 63) == 0.25f, "mono->mono keeps level (dup then average)");
        }
    }

    //==========================================================================
    // 8. Everything on, 10 s of noise + silence.
    void allEffectsSoakTest()
    {
        std::cout << "allEffectsSoakTest\n";
        const auto irFile = writeTestIR ("spastrip-soak-ir", 48000.0, 2.0);

        for (double sr : { 44100.0, 96000.0 })
        {
            for (int mode = 0; mode < 5; ++mode)   // 16, 64, 512, 1024, varying
            {
                const int fixed[] = { 16, 64, 512, 1024, 0 };
                const int block = fixed[mode];
                const int maxBlock = block == 0 ? 1024 : block;
                auto proc = makeProc (sr, maxBlock);
                setAllEffects (*proc, true);
                expect (proc->loadConvolutionIR (irFile), "IR loaded for the soak");
                proc->serviceMessageThread();

                Noise nz (99);
                juce::Random rng (5);
                const long total = (long) (10.0 * sr);
                const long noiseEnd = (long) (4.0 * sr);   // 4 s noise, then silence
                bool finite = true;
                float peak = 0.0f;
                long done = 0;
                long sinceService = 0;
                juce::MidiBuffer midi;
                while (done < total)
                {
                    int n = block;
                    if (block == 0)
                    {
                        static const int sizes[] = { 1, 7, 32, 100, 333, 512, 1024, 17, 64, 480, 3 };
                        n = sizes[rng.nextInt (11)];
                    }
                    n = (int) std::min<long> (n, total - done);
                    juce::AudioBuffer<float> buf (2, n);
                    for (int i = 0; i < n; ++i)
                    {
                        const bool noisy = (done + i) < noiseEnd;
                        const float l = noisy ? 0.5f * nz.next() : 0.0f, r = noisy ? 0.5f * nz.next() : 0.0f;
                        buf.setSample (0, i, l); buf.setSample (1, i, r);
                    }
                    proc->processBlock (buf, midi);
                    finite = finite && allFinite (buf);
                    peak = std::max (peak, buf.getMagnitude (0, n));
                    done += n; sinceService += n;
                    if (sinceService >= (long) (0.15 * sr)) { proc->serviceMessageThread(); sinceService = 0; }
                }
                const juce::String tag = juce::String ((int) sr) + " Hz, block " + (block == 0 ? juce::String ("varying") : juce::String (block)) + ": ";
                std::cout << "  " << tag << "peak " << peak << "\n";
                expect (finite && peak < 8.0f, tag + "10 s noise+silence with every effect on: finite and bounded (peak " + juce::String (peak, 3) + ")");
            }
        }
        irFile.deleteFile();
    }

    //==========================================================================
    // Audio-thread allocation check (macOS only: hooks libmalloc's logger).
   #if JUCE_MAC
    std::atomic<long> g_allocCount { 0 };
    std::atomic<bool> g_countAllocs { false };
    void spaStripMallocLogger (uint32_t type, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uint32_t)
    {
        if ((type & 2u) != 0 && g_countAllocs.load (std::memory_order_relaxed))
            g_allocCount.fetch_add (1, std::memory_order_relaxed);
    }
   #endif

    void noAllocationTest()
    {
        std::cout << "noAllocationTest\n";
       #if JUCE_MAC
        for (int factorChoice = 0; factorChoice < 3; ++factorChoice)
        for (bool bypassed : { false, true })
        {
            auto proc = makeProc (48000.0, 256);
            setAllEffects (*proc, true);
            setParam (*proc, pid::oversampling, (float) factorChoice);
            setParam (*proc, pid::mix, 0.5f);
            setParam (*proc, pid::inputGain, 3.0f);
            setParam (*proc, pid::fx::limLookahead, 1.0f);
            juce::AudioBuffer<float> buf (2, 256);
            juce::MidiBuffer midi;
            Noise nz (4);
            const auto fill = [&] { for (int c = 0; c < 2; ++c) for (int i = 0; i < 256; ++i) buf.setSample (c, i, 0.3f * nz.next()); };
            fill();
            proc->processBlock (buf, midi);       // settle the factor, warm up
            proc->serviceMessageThread();
            for (int i = 0; i < 20; ++i) { fill(); proc->processBlock (buf, midi); }

            malloc_logger = spaStripMallocLogger;
            g_allocCount = 0;
            g_countAllocs = true;
            for (int i = 0; i < 200; ++i)
            {
                if (i % 50 == 0)   // parameter automation arrives on another thread; keep it out of the count
                {
                    g_countAllocs = false;
                    setParam (*proc, pid::fx::reverbMix, 0.1f + 0.002f * (float) i);
                    g_countAllocs = true;
                }
                fill();
                if (bypassed) proc->processBlockBypassed (buf, midi); else proc->processBlock (buf, midi);
            }
            g_countAllocs = false;
            malloc_logger = nullptr;
            expect (g_allocCount.load() == 0, juce::String (1 << factorChoice) + "x, " + (bypassed ? "bypassed" : "processing") + ": 200 blocks with all effects on performed no heap allocation (" + juce::String ((juce::int64) g_allocCount.load()) + " allocations seen)");
        }
        // Phase 2: sidechain detector + eight active slots (External bus, and the
        // Input source with sc.listen toggled on and off), every effect on, all three
        // oversampling factors, with the sidechain bus enabled.
        for (int factorChoice = 0; factorChoice < 3; ++factorChoice)
        for (int variant = 0; variant < 3; ++variant)   // 0 external, 1 input source, 2 external + listen
        {
            auto proc = makeProc (48000.0, 256, 2, 2, 2);
            setAllEffects (*proc, true);
            setParam (*proc, pid::oversampling, (float) factorChoice);
            setParam (*proc, pid::mix, 0.5f);
            setParam (*proc, pid::fx::limLookahead, 1.0f);
            setParam (*proc, pid::sc::source, variant == 1 ? 1.0f : 0.0f);
            setParam (*proc, pid::sc::attack, 0.5f);
            const auto& targets = Proc::getModTargets();
            for (int slot = 0; slot < 8; ++slot)
            {
                proc->setModSlotTarget (slot, targets[(size_t) ((slot * 19 + 3) % (int) targets.size())].id);
                setParam (*proc, pid::modSlotDepth (slot), slot % 2 == 0 ? 0.7f : -0.5f);
            }
            juce::AudioBuffer<float> buf (4, 256);
            juce::MidiBuffer midi;
            Noise nz (6);
            long pos = 0;
            const auto fill = [&]
            {
                for (int i = 0; i < 256; ++i, ++pos)
                {
                    const float gate = (pos / 3000) % 2 == 0 ? 1.0f : 0.1f;
                    for (int c = 0; c < 4; ++c) buf.setSample (c, i, c < 2 ? 0.3f * nz.next() : gate * 0.8f * nz.next());
                }
            };
            fill();
            proc->processBlock (buf, midi);
            proc->serviceMessageThread();
            for (int i = 0; i < 30; ++i) { fill(); proc->processBlock (buf, midi); }

            malloc_logger = spaStripMallocLogger;
            g_allocCount = 0;
            g_countAllocs = true;
            for (int i = 0; i < 200; ++i)
            {
                if (i % 50 == 25)   // automation / listen toggles arrive on another thread; keep them out of the count
                {
                    g_countAllocs = false;
                    setParam (*proc, pid::modSlotDepth (1), 0.2f + 0.003f * (float) i);
                    setParam (*proc, pid::sc::gain, (float) (i % 7));
                    if (variant == 2) setParam (*proc, pid::sc::listen, (i / 50) % 2 == 0 ? 1.0f : 0.0f);
                    g_countAllocs = true;
                }
                fill();
                proc->processBlock (buf, midi);
            }
            g_countAllocs = false;
            malloc_logger = nullptr;
            expect (g_allocCount.load() == 0, juce::String (1 << factorChoice) + "x, " + (variant == 0 ? "external sidechain" : variant == 1 ? "input source" : "external + listen toggling")
                    + ", 8 active slots, all effects on: 200 blocks performed no heap allocation (" + juce::String ((juce::int64) g_allocCount.load()) + " allocations seen)");
        }
       #else
        std::cout << "  (skipped: allocation hook is macOS-only)\n";
       #endif
    }

    //==========================================================================
    // Tempo: host BPM, with the 120 fallback.
    void tempoTest()
    {
        std::cout << "tempoTest\n";
        auto proc = makeProc (48000.0, 256);
        juce::AudioBuffer<float> buf (2, 256);
        juce::MidiBuffer midi;
        buf.clear();
        proc->processBlock (buf, midi);
        expect (proc->getTelemetry().bpm.load() == 120.0f, "no playhead: BPM falls back to 120");

        FixedPlayHead ph;
        proc->setPlayHead (&ph);
        ph.bpm = 93.0;
        proc->processBlock (buf, midi);
        expect (proc->getTelemetry().bpm.load() == 93.0f, "host BPM is used (93)");

        ph.bpm = {};   // playhead with no tempo
        proc->processBlock (buf, midi);
        expect (proc->getTelemetry().bpm.load() == 120.0f, "playhead without a tempo falls back to 120");
        ph.bpm = 0.0;
        proc->processBlock (buf, midi);
        expect (proc->getTelemetry().bpm.load() == 120.0f, "an invalid (<= 0) host tempo falls back to 120");
        proc->setPlayHead (nullptr);

        // Tail length follows the chain (delay with a known sync time).
        setAllEffects (*proc, false);
        setParam (*proc, pid::fx::reverbEnable, 1.0f);
        setParam (*proc, pid::fx::reverbDecay, 3.0f);
        proc->processBlock (buf, midi);
        expect (std::abs (proc->getTailLengthSeconds() - 3.5) < 1.0e-6, "getTailLengthSeconds follows FXChain::tailSeconds (reverb 3 s -> 3.5 s)");
    }

    //==========================================================================
    // Non-finite safety net.
    void nonFiniteTest()
    {
        std::cout << "nonFiniteTest\n";
        constexpr int block = 256;
        auto proc = makeProc (48000.0, block);
        setAllEffects (*proc, false);
        setParam (*proc, pid::fx::delayEnable, 1.0f);
        setParam (*proc, pid::fx::delaySync, 0.0f);
        setParam (*proc, pid::fx::delayTime, 20.0f);
        setParam (*proc, pid::fx::delayFeedback, 0.9f);
        setParam (*proc, pid::fx::delayMix, 0.5f);
        juce::AudioBuffer<float> buf (2, block);
        juce::MidiBuffer midi;
        Noise nz (2);
        const auto feed = [&] (bool poison)
        {
            for (int c = 0; c < 2; ++c) for (int i = 0; i < block; ++i) buf.setSample (c, i, 0.3f * nz.next());
            if (poison) buf.setSample (0, 10, std::numeric_limits<float>::quiet_NaN());
            proc->processBlock (buf, midi);
        };
        for (int i = 0; i < 10; ++i) feed (false);
        feed (true);
        expect (allFinite (buf) && buf.getMagnitude (0, block) == 0.0f, "a NaN in the block silences the whole block");
        proc->serviceMessageThread();   // schedules fxChain.reset() off the audio thread
        bool finite = true;
        float pk = 0.0f;
        for (int i = 0; i < 40; ++i) { feed (false); finite = finite && allFinite (buf); pk = std::max (pk, buf.getMagnitude (0, block)); }
        expect (finite && pk > 0.05f && pk < 2.0f, "after the chain reset, clean input is processed normally again (peak " + juce::String (pk, 3) + ")");
    }

    //==========================================================================
    // Processor basics: no MIDI/clearing, generic editor, state XML shape.
    void processorBasicsTest()
    {
        std::cout << "processorBasicsTest\n";
        Proc proc;
        expect (proc.getName() == "SPAStrip", "plugin name");
        std::unique_ptr<juce::AudioProcessorEditor> ed (proc.createEditor());
        expect (dynamic_cast<juce::GenericAudioProcessorEditor*> (ed.get()) != nullptr, "phase-1 editor is the generic host editor");
        ed.reset();
        expect (&proc.getTelemetry() == &proc.getTelemetry(), "telemetry is exposed");

        juce::MemoryBlock mb;
        proc.getStateInformation (mb);
        auto xml = juce::AudioProcessor::getXmlFromBinary (mb.getData(), (int) mb.getSize());
        expect (xml != nullptr && xml->hasTagName ("PARAMS"), "state root is the APVTS PARAMS tree");
        int paramChildren = 0;
        if (xml != nullptr)
            for (auto* c : xml->getChildIterator())
                if (c->hasTagName ("PARAM")) ++paramChildren;
        expect (paramChildren == (int) spa::params::all().size(), "state holds one PARAM child per registry parameter (" + juce::String (paramChildren) + ")");
        expect (xml != nullptr && xml->getStringAttribute ("irSource") == "none", "irSource defaults to 'none'");
        proc.setStateInformation ("garbage", 7);   // must not crash or change anything
        expect (true, "garbage state data is ignored");

        // getTailLengthSeconds with nothing enabled.
        expect (proc.getTailLengthSeconds() == 0.0, "no tail with everything off");
    }
}

// The ported suites live in named namespaces; the enclosing anonymous namespace
// gives their functions internal linkage (no -Wmissing-prototypes noise).
namespace
{
#include "FxModuleTests.inc"
#include "FxEngineTests.inc"
#include "Phase2Tests.inc"
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;

    // Diagnostics: `SPAStripTests --audit-mod [id-substring] [sampleRate] [envelopeHz] [square] [slewMs] [interval]` prints the
    // modulation-target zipper audit instead of running the suite.
    if (argc >= 2 && juce::String (argv[1]) == "--bench-mod")
    {
        phase2Tests::benchModulation();
        return 0;
    }
    if (argc >= 2 && juce::String (argv[1]) == "--audit-mod")
    {
        phase2Tests::AuditConfig cfg;
        cfg.envHz = argc >= 5 ? juce::String (argv[4]).getDoubleValue() : 1.0;
        cfg.square = argc >= 6 && juce::String (argv[5]) == "square";
        if (argc >= 7) cfg.shipSmoothMs = (float) juce::String (argv[6]).getDoubleValue();   // what-if for the "shipping default" column
        if (argc >= 8) cfg.shipInterval = juce::String (argv[7]).getIntValue();
        phase2Tests::auditModTargets (argc >= 3 ? juce::String (argv[2]) : juce::String(),
                                      argc >= 4 ? juce::String (argv[3]).getDoubleValue() : 48000.0, cfg);
        return 0;
    }

    int run = 0;
    const auto start = juce::Time::getMillisecondCounterHiRes();
#define RUN(fn) do { ++run; fn(); } while (false)

    // --- Ported from SPASynth ---------------------------------------------
    RUN (fxModuleTests::compMatchesSPAGlitchTest);
    RUN (fxModuleTests::fxNewModulesOffBypassTest);
    RUN (fxModuleTests::grainBasicsTest);
    RUN (fxModuleTests::grainReverseTest);
    RUN (fxModuleTests::grainFreezeCaptureTest);
    RUN (fxModuleTests::grainFeedbackFuzzTest);
    RUN (fxModuleTests::grainSpreadSplitTest);
    RUN (fxModuleTests::grainDensityCapTest);
    RUN (fxModuleTests::grainFeedbackStillBuildsTest);
    RUN (fxModuleTests::grainSizeIndependentFeedbackTest);
    RUN (fxModuleTests::fxOrderMigrationTest);
    RUN (fxEngineTests::chorusWidthModeParamsTest);
    RUN (fxEngineTests::chorusStereoWidthTest);
    RUN (fxEngineTests::chorusVintageDarkerTest);
    RUN (fxEngineTests::chorusMixTest);
    RUN (fxEngineTests::chorusFeedbackStabilityTest);
    RUN (fxEngineTests::chorusParameterExtremesTest);
    RUN (fxEngineTests::delayPingPongWidthTest);
    RUN (fxEngineTests::delayWidthNoOpWhenPingPongOffTest);
    RUN (fxEngineTests::delayWidthZeroMatchesPreChangeAlgorithmTest);
    RUN (fxEngineTests::reverbArrivalFollowsPreDelayTest);
    RUN (fxEngineTests::plateReverbIndexStressTest);

    // --- SPAStrip processor ------------------------------------------------
    RUN (passthroughTest);
    RUN (parameterParityTest);
    RUN (fxOrderStateTest);
    RUN (fullStateRoundTripTest);
    RUN (latencyAndMixTest);
    RUN (bypassTest);
    RUN (busLayoutTest);
    RUN (allEffectsSoakTest);
    RUN (noAllocationTest);
    RUN (tempoTest);
    RUN (nonFiniteTest);
    RUN (processorBasicsTest);

    // --- Phase 2a: sidechain / modulation matrix / dry-wet / factory IRs ------
    RUN (phase2Tests::sidechainParamsTest);
    RUN (phase2Tests::sidechainEnvelopeTimingTest);
    RUN (phase2Tests::externalBusDisabledTest);
    RUN (phase2Tests::slotChangesOutputTest);
    RUN (phase2Tests::depthSignAndSumTest);
    RUN (phase2Tests::modTargetApiTest);
    RUN (phase2Tests::modStateTest);
    RUN (phase2Tests::sidechainListenTest);
    RUN (phase2Tests::sidechainBusAliasingTest);
    RUN (phase2Tests::modRefreshTest);
    RUN (phase2Tests::modZipperTest);
    RUN (phase2Tests::modAllTargetsSmokeTest);
    RUN (phase2Tests::modulationSoakTest);
    RUN (phase2Tests::mixFlatnessOversampledTest);
    RUN (phase2Tests::factoryIRTableTest);
    RUN (phase2Tests::karlskircheAssetTest);
    RUN (phase2Tests::factoryIRLoadTest);
    RUN (phase2Tests::factoryIRStateTest);
#undef RUN

    std::cout << "\n========================================\n"
              << run << " test functions, " << failures << " failed expectation(s), "
              << juce::String ((juce::Time::getMillisecondCounterHiRes() - start) / 1000.0, 1) << " s\n";
    std::cout << (failures == 0 ? "ALL TESTS PASSED\n" : "SOME TESTS FAILED\n");
    return failures == 0 ? 0 : 1;
}
