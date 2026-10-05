#include "ParameterRegistry.h"

namespace spa::params
{

juce::String sectionName (Section s)
{
    switch (s)
    {
        case Section::global:  return "Global";
        case Section::fxDist:   return "FX Dist";
        case Section::fxChorus: return "FX Chorus";
        case Section::fxDelay:  return "FX Delay";
        case Section::fxReverb: return "FX Reverb";
        case Section::fxEQ:     return "FX EQ";
        case Section::fxMod:    return "FX Mod";
        case Section::fxTremVib: return "FX Trem/Vib";
        case Section::fxLimiter: return "FX Limiter";
        case Section::fxConvolve: return "FX Convolve";
        case Section::fxComp:   return "FX Comp";
        case Section::fxGrain:  return "FX Glitter";
        case Section::sidechain: return "Sidechain";
        case Section::modMatrix: return "Mod Matrix";
        case Section::fxFilter:  return "FX Filter";
        case Section::fxCompBands: return "FX Comp Bands";
    }
    return {};
}

namespace id
{
    juce::String eqBand (int band, const char* key)
    {
        return "fxEQ.band" + juce::String (band) + "." + key;
    }

    juce::String compBand (int band, const char* key)
    {
        return "fxComp.band" + juce::String (band) + "." + key;
    }

    juce::String modSlotDepth (int slot)
    {
        return "mod.slot" + juce::String (slot + 1) + ".depth";
    }
}

static juce::NormalisableRange<float> frequencyRange (float min, float max)
{
    juce::NormalisableRange<float> r (min, max);
    r.setSkewForCentre (std::sqrt (min * max)); // log-ish response, geometric centre
    return r;
}

static juce::NormalisableRange<float> skewedRange (float min, float max, float centre)
{
    juce::NormalisableRange<float> r (min, max);
    r.setSkewForCentre (centre); // more resolution around `centre`
    return r;
}

juce::NormalisableRange<float> grainReleaseRange()
{
    // normalised: [0, 0.03] = off, (0.03, 0.97) = 0.1..30 s (log), [0.97, 1] = infinite
    constexpr float lo = 0.03f, hi = 0.97f, minS = 0.1f, maxS = 30.0f;
    const auto from0to1 = [] (float, float, float n)
    {
        if (n <= lo) return 0.0f;
        if (n >= hi) return grainReleaseInfinite;
        return minS * std::pow (maxS / minS, (n - lo) / (hi - lo));
    };
    const auto to0to1 = [] (float, float, float v)
    {
        if (v < 0.05f) return 0.0f;
        if (v >= 30.5f) return 1.0f;
        return lo + (hi - lo) * std::log (juce::jlimit (minS, maxS, v) / minS) / std::log (maxS / minS);
    };
    const auto snap = [] (float, float, float v)
    {
        if (v < 0.05f) return 0.0f;
        if (v >= 30.5f) return grainReleaseInfinite;
        return juce::jlimit (minS, maxS, v);
    };
    return { 0.0f, grainReleaseInfinite, from0to1, to0to1, snap };
}

static std::vector<ParamDef> buildDefs()
{
    std::vector<ParamDef> p;

    // --- SPAStrip globals (new) -------------------------------------------
    // Randomization is disabled for all of them: they are level/routing
    // controls, not creative parameters.
    p.push_back ({ id::inputGain, "Input Gain", Section::global,
                   ParamKind::floatParam, { -24.0f, 24.0f, 0.1f }, 0.0f, "dB",
                   { .enabled = false } });
    p.push_back ({ id::outputGain, "Output Gain", Section::global,
                   ParamKind::floatParam, { -24.0f, 24.0f, 0.1f }, 0.0f, "dB",
                   { .enabled = false } });
    p.push_back ({ id::mix, "Mix", Section::global,
                   ParamKind::floatParam, { 0.0f, 1.0f }, 1.0f, "",
                   { .enabled = false }, {}, true });
    p.push_back ({ id::oversampling, "Oversampling", Section::global,
                   ParamKind::choiceParam, {}, 0.0f, "",
                   { .enabled = false }, juce::StringArray { "1x", "2x", "4x" } });

    // --- FX chain -------------------------------------------------------
    namespace fx = id::fx;

    p.push_back ({ fx::distEnable, "Dist On", Section::fxDist,
                   ParamKind::boolParam, {}, 0.0f, "",
                   { .enabled = true, .biasCentre = 0.3f, .biasStrength = 0.3f } });
    p.push_back ({ fx::distType, "Dist Type", Section::fxDist,
                   ParamKind::choiceParam, {}, 0.0f, "", { .enabled = true },
                   { "Soft", "Hard", "Fold", "Crush" } });
    p.push_back ({ fx::distDrive, "Dist Drive", Section::fxDist,
                   ParamKind::floatParam, { 0.0f, 1.0f }, 0.3f, "",
                   { .enabled = true, .maxNorm = 0.8f, .biasCentre = 0.3f,
                            .biasStrength = 0.3f } });
    p.push_back ({ fx::distTone, "Dist Tone", Section::fxDist,
                   ParamKind::floatParam, frequencyRange (500.0f, 20000.0f), 8000.0f, "Hz",
                   { .enabled = true, .minNorm = 0.3f } });
    p.push_back ({ fx::distMix, "Dist Mix", Section::fxDist,
                   ParamKind::floatParam, { 0.0f, 1.0f }, 1.0f, "",
                   { .enabled = true, .minNorm = 0.3f } , {}, true});

    p.push_back ({ fx::chorusEnable, "Chorus On", Section::fxChorus,
                   ParamKind::boolParam, {}, 0.0f, "",
                   { .enabled = true, .biasCentre = 0.4f, .biasStrength = 0.3f } });
    p.push_back ({ fx::chorusRate, "Chorus Rate", Section::fxChorus,
                   ParamKind::floatParam, frequencyRange (0.05f, 5.0f), 0.8f, "Hz",
                   { .enabled = true, .maxNorm = 0.7f } });
    p.push_back ({ fx::chorusDepth, "Chorus Depth", Section::fxChorus,
                   ParamKind::floatParam, { 0.0f, 1.0f }, 0.3f, "",
                   { .enabled = true, .biasCentre = 0.35f, .biasStrength = 0.3f } });
    p.push_back ({ fx::chorusFeedback, "Chorus FB", Section::fxChorus,
                   ParamKind::floatParam, { -0.9f, 0.9f }, 0.0f, "",
                   { .enabled = true, .biasCentre = 0.5f, .biasStrength = 0.6f } });
    // WIDTH is the phase offset between the left and right chorus LFOs (see
    // StereoChorus.h). Stored as a percentage because that is how it reads on
    // the knob; the DSP wants 0..1 and the divide happens in exactly one
    // place, SPASynthProcessor::updateFXParams. Default 50 rather than 0: at
    // 0 both channels sweep together, which is precisely the mono, phaser-ish
    // sound this control exists to fix, so shipping at 0 would reintroduce it.
    p.push_back ({ fx::chorusWidth, "Chorus Width", Section::fxChorus,
                   ParamKind::floatParam, { 0.0f, 100.0f }, 50.0f, "%",
                   { .enabled = true, .biasCentre = 0.6f, .biasStrength = 0.3f } });
    // Append-only (serialized): Vintage = index 0, Modern = index 1. Modern
    // is the default because it is the closest match to the clean digital
    // character of the juce::dsp::Chorus this engine replaced, so presets
    // saved before the change shift as little as possible.
    p.push_back ({ fx::chorusMode, "Chorus Mode", Section::fxChorus,
                   ParamKind::choiceParam, {}, 1.0f /* Modern */, "",
                   { .enabled = true }, { "Vintage", "Modern", "VHS" } });
    p.push_back ({ fx::chorusMix, "Chorus Mix", Section::fxChorus,
                   ParamKind::floatParam, { 0.0f, 1.0f }, 0.5f, "",
                   { .enabled = true, .biasCentre = 0.5f, .biasStrength = 0.3f } , {}, true});

    // VHS-mode controls (SPASynth 1.0.32, same IDs / ranges / specs and the
    // same registry position, so Randomize All keeps drawing exactly as the
    // synth's FX flow does). Shown only when Mode = VHS. Percentages (the /100
    // is in SPAStripProcessor::updateFXParams). Not mod destinations (excluded
    // in ModTargets.cpp, as in the synth). RANDOMIZE ALL keeps them moderate:
    // DROPOUTS and HISS never past 40 %.
    p.push_back ({ fx::chorusVhsWow, "Chorus Wow", Section::fxChorus,
                   ParamKind::floatParam, { 0.0f, 100.0f }, 40.0f, "%",
                   { .enabled = true, .minNorm = 0.15f, .maxNorm = 0.7f } });
    p.push_back ({ fx::chorusVhsFlutter, "Chorus Flutter", Section::fxChorus,
                   ParamKind::floatParam, { 0.0f, 100.0f }, 25.0f, "%",
                   { .enabled = true, .maxNorm = 0.6f } });
    p.push_back ({ fx::chorusVhsTone, "Chorus Tone", Section::fxChorus,
                   ParamKind::floatParam, { 0.0f, 100.0f }, 45.0f, "%",
                   { .enabled = true, .minNorm = 0.2f, .maxNorm = 0.9f } });
    p.push_back ({ fx::chorusVhsSat, "Chorus Saturation", Section::fxChorus,
                   ParamKind::floatParam, { 0.0f, 100.0f }, 30.0f, "%",
                   { .enabled = true, .maxNorm = 0.7f } });
    p.push_back ({ fx::chorusVhsHiss, "Chorus Hiss", Section::fxChorus,
                   ParamKind::floatParam, { 0.0f, 100.0f }, 15.0f, "%",
                   { .enabled = true, .maxNorm = 0.4f } });
    p.push_back ({ fx::chorusVhsDropouts, "Chorus Dropouts", Section::fxChorus,
                   ParamKind::floatParam, { 0.0f, 100.0f }, 10.0f, "%",
                   { .enabled = true, .maxNorm = 0.4f } });

    p.push_back ({ fx::delayEnable, "Delay On", Section::fxDelay,
                   ParamKind::boolParam, {}, 0.0f, "",
                   { .enabled = true, .biasCentre = 0.4f, .biasStrength = 0.3f } });
    p.push_back ({ fx::delaySync, "Delay Sync", Section::fxDelay,
                   ParamKind::boolParam, {}, 1.0f, "",
                   { .enabled = true, .biasCentre = 0.8f, .biasStrength = 0.5f } });
    p.push_back ({ fx::delayTime, "Delay Time", Section::fxDelay,
                   ParamKind::floatParam, { 1.0f, 2000.0f, 0.0f, 0.4f }, 350.0f, "ms",
                   { .enabled = true, .minNorm = 0.2f, .maxNorm = 0.8f } });
    p.push_back ({ fx::delayDivision, "Delay Div", Section::fxDelay,
                   ParamKind::choiceParam, {}, 6.0f /* 1/4 */, "",
                   { .enabled = true, .minNorm = 0.3f, .maxNorm = 0.9f },
                   lfoDivisionNames() });
    p.push_back ({ fx::delayFeedback, "Delay FB", Section::fxDelay,
                   ParamKind::floatParam, { 0.0f, 0.95f }, 0.35f, "",
                   { .enabled = true, .maxNorm = 0.75f, .biasCentre = 0.4f,
                            .biasStrength = 0.3f } });
    p.push_back ({ fx::delayPingPong, "Ping Pong", Section::fxDelay,
                   ParamKind::boolParam, {}, 0.0f, "", { .enabled = true } });
    // Only acts when PING PONG is on (see FXChain::processDelay). At 0 the
    // injection matches the pre-1.0.25 ping-pong (crossed feedback only, no
    // mono sum), so existing presets/sessions with ping-pong on would bounce
    // wider once this defaults to 100% -- documented in the 1.0.25 changelog.
    p.push_back ({ fx::delayWidth, "Delay Width", Section::fxDelay,
                   ParamKind::floatParam, { 0.0f, 100.0f }, 100.0f, "%",
                   { .enabled = true, .biasCentre = 0.6f, .biasStrength = 0.3f } });
    p.push_back ({ fx::delayMix, "Delay Mix", Section::fxDelay,
                   ParamKind::floatParam, { 0.0f, 1.0f }, 0.35f, "",
                   { .enabled = true, .maxNorm = 0.8f, .biasCentre = 0.35f,
                            .biasStrength = 0.3f } , {}, true});

    p.push_back ({ fx::reverbEnable, "Reverb On", Section::fxReverb,
                   ParamKind::boolParam, {}, 0.0f, "",
                   { .enabled = true, .biasCentre = 0.6f, .biasStrength = 0.3f } });
    p.push_back ({ fx::reverbMode, "Reverb Mode", Section::fxReverb,
                   ParamKind::choiceParam, {}, 0.0f, "",
                   { .enabled = true },
                   juce::StringArray { "Hall", "Plate", "Chamber", "Room", "Spring" } });
    p.push_back ({ fx::reverbPreDelay, "Reverb Pre", Section::fxReverb,
                   ParamKind::floatParam, { 0.0f, 200.0f }, 20.0f, "ms",
                   { .enabled = true, .maxNorm = 0.4f } });
    p.push_back ({ fx::reverbSize, "Reverb Size", Section::fxReverb,
                   ParamKind::floatParam, { 0.0f, 1.0f }, 0.5f, "",
                   { .enabled = true } });
    // Top end pulled in from 12s: at max Decay + Hall's decay multiplier the
    // old range produced an effective RT60 approaching 17s, which read as
    // uncontrolled runaway feedback rather than a long tail (tester report).
    p.push_back ({ fx::reverbDecay, "Reverb Decay", Section::fxReverb,
                   ParamKind::floatParam, skewedRange (0.2f, 8.0f, 2.5f), 2.0f, "s",
                   { .enabled = true, .maxNorm = 0.6f } });
    p.push_back ({ fx::reverbDamping, "Reverb Damp", Section::fxReverb,
                   ParamKind::floatParam, { 0.0f, 1.0f }, 0.5f, "",
                   { .enabled = true } });
    p.push_back ({ fx::reverbModDepth, "Reverb Mod", Section::fxReverb,
                   ParamKind::floatParam, { 0.0f, 1.0f }, 0.2f, "",
                   { .enabled = true, .maxNorm = 0.6f } });
    p.push_back ({ fx::reverbLowCut, "Reverb LoCut", Section::fxReverb,
                   ParamKind::floatParam, frequencyRange (20.0f, 2000.0f), 20.0f, "Hz",
                   { .enabled = true, .maxNorm = 0.5f } });
    p.push_back ({ fx::reverbHighCut, "Reverb HiCut", Section::fxReverb,
                   ParamKind::floatParam, frequencyRange (1000.0f, 20000.0f), 12000.0f, "Hz",
                   { .enabled = true, .minNorm = 0.4f } });
    p.push_back ({ fx::reverbWidth, "Reverb Width", Section::fxReverb,
                   ParamKind::floatParam, { 0.0f, 1.0f }, 1.0f, "",
                   { .enabled = true, .minNorm = 0.4f } });
    p.push_back ({ fx::reverbMix, "Reverb Mix", Section::fxReverb,
                   ParamKind::floatParam, { 0.0f, 1.0f }, 0.3f, "",
                   { .enabled = true, .maxNorm = 0.8f, .biasCentre = 0.3f,
                            .biasStrength = 0.3f } , {}, true});

    p.push_back ({ fx::eqEnable, "EQ On", Section::fxEQ,
                   ParamKind::boolParam, {}, 0.0f, "",
                   { .enabled = true, .biasCentre = 0.3f, .biasStrength = 0.4f } });
    p.push_back ({ fx::eqCharacter, "EQ Character", Section::fxEQ,
                   ParamKind::choiceParam, {}, 0.0f, "",
                   { .enabled = true },
                   juce::StringArray { "Clean", "Modern", "Vintage", "Tube" } });

    // 8 parametric bands. Disabled by default (flat); default freqs spread log-
    // wide with shelf types at the ends, so enabling a band drops a sensible node.
    {
        struct BandDef { float freq; int type; };
        const BandDef defs[8] = {
            {    80.0f, 1 /* Low Shelf */ }, {   200.0f, 0 }, {  500.0f, 0 },
            {  1200.0f, 0 }, {  3000.0f, 0 }, {  6000.0f, 0 },
            { 10000.0f, 2 /* High Shelf */ }, { 15000.0f, 0 } };

        for (int b = 0; b < 8; ++b)
        {
            const auto bn = "EQ B" + juce::String (b + 1) + " ";
            p.push_back ({ id::eqBand (b, fx::eqband::enable), bn + "On", Section::fxEQ,
                           ParamKind::boolParam, {}, 0.0f, "",
                           { .enabled = true, .biasCentre = 0.2f, .biasStrength = 0.5f } });
            p.push_back ({ id::eqBand (b, fx::eqband::type), bn + "Type", Section::fxEQ,
                           ParamKind::choiceParam, {}, (float) defs[b].type, "",
                           { .enabled = false },
                           // Append-only (ParametricEQ::Type indices).
                           juce::StringArray { "Bell", "Low Shelf", "High Shelf",
                                               "Low Cut", "High Cut", "Notch",
                                               "Band Pass", "Tilt Shelf" } });
            p.push_back ({ id::eqBand (b, fx::eqband::slope), bn + "Slope", Section::fxEQ,
                           ParamKind::choiceParam, {}, 1.0f /* 12 dB, matches pre-slope behaviour */, "",
                           { .enabled = false },
                           // Append-only. Only meaningful for Low Cut / High Cut bands.
                           juce::StringArray { "6 dB", "12 dB", "18 dB", "24 dB", "36 dB", "48 dB" } });
            p.push_back ({ id::eqBand (b, fx::eqband::freq), bn + "Freq", Section::fxEQ,
                           ParamKind::floatParam, frequencyRange (20.0f, 20000.0f),
                           defs[b].freq, "Hz", { .enabled = true } });
            p.push_back ({ id::eqBand (b, fx::eqband::gain), bn + "Gain", Section::fxEQ,
                           ParamKind::floatParam, { -24.0f, 24.0f, 0.1f }, 0.0f, "dB",
                           { .enabled = true, .biasCentre = 0.5f, .biasStrength = 0.6f } });
            p.push_back ({ id::eqBand (b, fx::eqband::q), bn + "Q", Section::fxEQ,
                           ParamKind::floatParam, skewedRange (0.1f, 18.0f, 1.0f), 0.707f, "",
                           { .enabled = true, .biasCentre = 0.3f } });
        }
    }

    // FX Mod (Phaser / Flanger, switchable).
    p.push_back ({ fx::modEnable, "Mod On", Section::fxMod,
                   ParamKind::boolParam, {}, 0.0f, "",
                   { .enabled = true, .biasCentre = 0.4f, .biasStrength = 0.3f } });
    p.push_back ({ fx::modType, "Mod Type", Section::fxMod,
                   ParamKind::choiceParam, {}, 0.0f, "",
                   { .enabled = false },
                   juce::StringArray { "Phaser", "Flanger" } });
    p.push_back ({ fx::modRate, "Mod Rate", Section::fxMod,
                   ParamKind::floatParam, frequencyRange (0.02f, 8.0f), 0.5f, "Hz",
                   { .enabled = true, .maxNorm = 0.6f } });
    p.push_back ({ fx::modSync, "Mod Sync", Section::fxMod,
                   ParamKind::boolParam, {}, 0.0f, "", { .enabled = true } });
    p.push_back ({ fx::modDivision, "Mod Div", Section::fxMod,
                   ParamKind::choiceParam, {}, 6.0f, "",
                   { .enabled = false }, lfoDivisionNames() });
    p.push_back ({ fx::modDepth, "Mod Depth", Section::fxMod,
                   ParamKind::floatParam, { 0.0f, 1.0f }, 0.5f, "",
                   { .enabled = true, .biasCentre = 0.5f, .biasStrength = 0.3f } });
    p.push_back ({ fx::modFeedback, "Mod FB", Section::fxMod,
                   ParamKind::floatParam, { -0.95f, 0.95f }, 0.3f, "",
                   { .enabled = true, .biasCentre = 0.5f, .biasStrength = 0.5f } });
    p.push_back ({ fx::modStages, "Mod Stages", Section::fxMod,
                   ParamKind::choiceParam, {}, 2.0f, "",
                   { .enabled = false },
                   juce::StringArray { "2", "4", "6", "8", "12" } });
    p.push_back ({ fx::modCentre, "Mod Centre", Section::fxMod,
                   ParamKind::floatParam, frequencyRange (100.0f, 6000.0f), 800.0f, "Hz",
                   { .enabled = true } });
    p.push_back ({ fx::modManual, "Mod Delay", Section::fxMod,
                   ParamKind::floatParam, { 0.1f, 20.0f, 0.01f }, 3.0f, "ms",
                   { .enabled = true } });
    p.push_back ({ fx::modWidth, "Mod Width", Section::fxMod,
                   ParamKind::floatParam, { 0.0f, 1.0f }, 0.5f, "",
                   { .enabled = true } });
    p.push_back ({ fx::modMix, "Mod Mix", Section::fxMod,
                   ParamKind::floatParam, { 0.0f, 1.0f }, 0.5f, "",
                   { .enabled = true, .biasCentre = 0.5f, .biasStrength = 0.3f } , {}, true});

    // FX Tremolo / Vibrato (independent sections in one tab).
    p.push_back ({ fx::tremEnable, "Trem On", Section::fxTremVib,
                   ParamKind::boolParam, {}, 0.0f, "", { .enabled = true } });
    p.push_back ({ fx::tremRate, "Trem Rate", Section::fxTremVib,
                   ParamKind::floatParam, frequencyRange (0.05f, 20.0f), 5.0f, "Hz",
                   { .enabled = true, .maxNorm = 0.6f } });
    p.push_back ({ fx::tremSync, "Trem Sync", Section::fxTremVib,
                   ParamKind::boolParam, {}, 0.0f, "", { .enabled = true } });
    p.push_back ({ fx::tremDivision, "Trem Div", Section::fxTremVib,
                   ParamKind::choiceParam, {}, 6.0f, "",
                   { .enabled = false }, lfoDivisionNames() });
    p.push_back ({ fx::tremDepth, "Trem Depth", Section::fxTremVib,
                   ParamKind::floatParam, { 0.0f, 1.0f }, 0.5f, "",
                   { .enabled = true } });
    p.push_back ({ fx::tremShape, "Trem Shape", Section::fxTremVib,
                   ParamKind::choiceParam, {}, 0.0f, "",
                   { .enabled = false },
                   juce::StringArray { "Sine", "Triangle", "Square", "Saw" } });
    p.push_back ({ fx::tremStereo, "Trem Stereo", Section::fxTremVib,
                   ParamKind::floatParam, { 0.0f, 1.0f }, 0.0f, "",
                   { .enabled = true } });
    p.push_back ({ fx::tremMix, "Trem Mix", Section::fxTremVib,
                   ParamKind::floatParam, { 0.0f, 1.0f }, 1.0f, "",
                   { .enabled = true } , {}, true});
    p.push_back ({ fx::vibEnable, "Vib On", Section::fxTremVib,
                   ParamKind::boolParam, {}, 0.0f, "", { .enabled = true } });
    p.push_back ({ fx::vibRate, "Vib Rate", Section::fxTremVib,
                   ParamKind::floatParam, frequencyRange (0.05f, 14.0f), 5.0f, "Hz",
                   { .enabled = true, .maxNorm = 0.6f } });
    p.push_back ({ fx::vibSync, "Vib Sync", Section::fxTremVib,
                   ParamKind::boolParam, {}, 0.0f, "", { .enabled = true } });
    p.push_back ({ fx::vibDivision, "Vib Div", Section::fxTremVib,
                   ParamKind::choiceParam, {}, 6.0f, "",
                   { .enabled = false }, lfoDivisionNames() });
    p.push_back ({ fx::vibDepth, "Vib Depth", Section::fxTremVib,
                   ParamKind::floatParam, { 0.0f, 1.0f }, 0.4f, "",
                   { .enabled = true } });
    p.push_back ({ fx::vibMix, "Vib Mix", Section::fxTremVib,
                   ParamKind::floatParam, { 0.0f, 1.0f }, 1.0f, "",
                   { .enabled = true } , {}, true});

    // FX Limiter / Maximizer (defaults last in the chain).
    p.push_back ({ fx::limEnable, "Lim On", Section::fxLimiter,
                   ParamKind::boolParam, {}, 0.0f, "", { .enabled = true } });
    p.push_back ({ fx::limDrive, "Lim Drive", Section::fxLimiter,
                   ParamKind::floatParam, { 0.0f, 24.0f, 0.1f }, 0.0f, "dB",
                   { .enabled = true, .maxNorm = 0.5f } });
    p.push_back ({ fx::limCeiling, "Lim Ceiling", Section::fxLimiter,
                   ParamKind::floatParam, { -12.0f, 0.0f, 0.1f }, -0.3f, "dB",
                   { .enabled = false } });
    p.push_back ({ fx::limRelease, "Lim Release", Section::fxLimiter,
                   ParamKind::floatParam, { 1.0f, 1000.0f, 0.0f, 0.4f }, 120.0f, "ms",
                   { .enabled = true } });
    p.push_back ({ fx::limAutoRelease, "Lim Auto Rel", Section::fxLimiter,
                   ParamKind::boolParam, {}, 0.0f, "", { .enabled = true } });
    p.push_back ({ fx::limCharacter, "Lim Character", Section::fxLimiter,
                   ParamKind::choiceParam, {}, 0.0f, "",
                   { .enabled = false },
                   juce::StringArray { "Clean", "Punchy", "Aggressive" } });
    p.push_back ({ fx::limStereoLink, "Lim Link", Section::fxLimiter,
                   ParamKind::floatParam, { 0.0f, 1.0f }, 1.0f, "",
                   { .enabled = false } });
    p.push_back ({ fx::limTruePeak, "Lim True Peak", Section::fxLimiter,
                   ParamKind::boolParam, {}, 0.0f, "", { .enabled = false } });
    p.push_back ({ fx::limLookahead, "Lim Lookahead", Section::fxLimiter,
                   ParamKind::boolParam, {}, 0.0f, "", { .enabled = false } });
    p.push_back ({ fx::limAutoGain, "Lim Auto Gain", Section::fxLimiter,
                   ParamKind::boolParam, {}, 0.0f, "", { .enabled = false } });

    // FX Convolve (SFX / user WAV as impulse; IR path stored in the state tree).
    p.push_back ({ fx::convEnable, "Conv On", Section::fxConvolve,
                   ParamKind::boolParam, {}, 0.0f, "", { .enabled = false } });
    p.push_back ({ fx::convMix, "Conv Mix", Section::fxConvolve,
                   ParamKind::floatParam, { 0.0f, 1.0f }, 0.3f, "",
                   { .enabled = true, .maxNorm = 0.6f } , {}, true});
    p.push_back ({ fx::convWidth, "Conv Width", Section::fxConvolve,
                   ParamKind::floatParam, { 0.0f, 1.0f }, 1.0f, "",
                   { .enabled = false } });
    p.push_back ({ fx::convPreDelay, "Conv Pre", Section::fxConvolve,
                   ParamKind::floatParam, { 0.0f, 200.0f }, 0.0f, "ms",
                   { .enabled = true, .maxNorm = 0.4f } });
    p.push_back ({ fx::convDecay, "Conv Decay", Section::fxConvolve,
                   ParamKind::floatParam, { 0.05f, 1.0f }, 1.0f, "",
                   { .enabled = true, .minNorm = 0.3f } });
    p.push_back ({ fx::convDamping, "Conv Damp", Section::fxConvolve,
                   ParamKind::floatParam, { 0.0f, 1.0f }, 0.0f, "",
                   { .enabled = true, .maxNorm = 0.6f } });

    // Convolve start position -- appended at the end of the registry (keyed
    // by ID like every other param, so this doesn't disturb the append-only
    // mod-destination/choice-order rules). Trims from the front of the raw
    // impulse before decay/damping reshape it (see FXChain::reshapeConvolutionIR).
    // Deliberately NOT a mod destination (dest budget is tight -- see
    // ParameterRegistry.h's maxModDests comment -- and FX params as mod
    // destinations are a separate planned piece of work). Past the halfway
    // point the impulse gets progressively thinner, so RANDOMIZE ALL is
    // capped there (maxNorm); the user still gets the full range.
    p.push_back ({ fx::convStart, "Conv Start", Section::fxConvolve,
                   ParamKind::floatParam, { 0.0f, 1.0f }, 0.0f, "",
                   { .enabled = true, .maxNorm = 0.5f } });

    // FX COMP (1.0.29): the SPAGlitch three-band OTT-style compressor, ported
    // with its parameter set, ranges, tapers and defaults unchanged (MIX is
    // 0..1 shown as %, like every other MIX knob). Off by default, so every
    // existing preset and session is untouched. The crossovers and per-band
    // controls are driven by the tab's own editor (drag the crossovers on the
    // graph, controls follow the selected band); none is a mod destination,
    // and only the enable and MIX take part in RANDOMIZE ALL, with the band
    // controls constrained so a roll can never be a harsh one (see below).
    {
        namespace c = fx;
        p.push_back ({ c::compEnable, "Comp On", Section::fxComp,
                       ParamKind::boolParam, {}, 0.0f, "",
                       { .enabled = true, .biasCentre = 0.2f, .biasStrength = 0.5f } });
        p.push_back ({ c::compMix, "Comp Mix", Section::fxComp,
                       ParamKind::floatParam, { 0.0f, 1.0f }, 1.0f, "",
                       { .enabled = true, .minNorm = 0.4f, .maxNorm = 1.0f }, {}, true });
        p.push_back ({ c::compXoverLow, "Comp Low Crossover", Section::fxComp,
                       ParamKind::floatParam, frequencyRange (20.0f, 2000.0f), 200.0f, "Hz",
                       { .enabled = false } });
        p.push_back ({ c::compXoverHigh, "Comp High Crossover", Section::fxComp,
                       ParamKind::floatParam, frequencyRange (200.0f, 18000.0f), 2000.0f, "Hz",
                       { .enabled = false } });

        // Low bands need slower attacks than high ones: a 60 Hz cycle is 16 ms
        // long, and a detector faster than that follows the waveform instead of
        // the envelope (SPAGlitch's own note).
        const struct { float attack, release; const char* name; } timing[] {
            { 30.0f, 200.0f, "Low" }, { 12.0f, 120.0f, "Mid" }, { 5.0f, 80.0f, "High" } };

        for (int b = 0; b < 3; ++b)
        {
            const juce::String bn = juce::String ("Comp ") + timing[b].name + " ";
            // RANDOMIZE ALL: moderate settings only. Threshold stays in the
            // upper-middle of its range (a roll that crushes everything to
            // -60 dB would just be loud and flat), ratios stay musical, the
            // upward ratio is held near 1:1 (it lifts noise and tails), and
            // makeup gain is +/-6 dB at most.
            p.push_back ({ id::compBand (b, c::compband::threshold), bn + "Threshold", Section::fxComp,
                           ParamKind::floatParam, { -60.0f, 0.0f, 0.1f }, -24.0f, "dB",
                           { .enabled = true, .minNorm = 0.35f, .maxNorm = 0.8f } });
            p.push_back ({ id::compBand (b, c::compband::ratio), bn + "Down Ratio", Section::fxComp,
                           ParamKind::floatParam, skewedRange (1.0f, 20.0f, 4.0f), 4.0f, ":1",
                           { .enabled = true, .minNorm = 0.2f, .maxNorm = 0.65f } });
            p.push_back ({ id::compBand (b, c::compband::upRatio), bn + "Up Ratio", Section::fxComp,
                           ParamKind::floatParam, skewedRange (1.0f, 10.0f, 2.0f), 1.0f, ":1",
                           { .enabled = true, .minNorm = 0.0f, .maxNorm = 0.3f } });
            p.push_back ({ id::compBand (b, c::compband::attack), bn + "Attack", Section::fxComp,
                           ParamKind::floatParam, skewedRange (0.1f, 300.0f, 20.0f), timing[b].attack, "ms",
                           { .enabled = false } });
            p.push_back ({ id::compBand (b, c::compband::release), bn + "Release", Section::fxComp,
                           ParamKind::floatParam, skewedRange (5.0f, 2000.0f, 150.0f), timing[b].release, "ms",
                           { .enabled = false } });
            p.push_back ({ id::compBand (b, c::compband::gain), bn + "Makeup", Section::fxComp,
                           ParamKind::floatParam, { -24.0f, 24.0f, 0.1f }, 0.0f, "dB",
                           { .enabled = true, .minNorm = 0.375f, .maxNorm = 0.625f } });
        }
    }

    // FX GRAIN (1.0.29): granular delay / texture (see source/dsp/GrainFX.h).
    // Off by default. None of these is a mod destination. RANDOMIZE ALL keeps
    // it gentle: moderate grain sizes and densities, pitch within an octave,
    // limited spread / reverse / feedback, mix capped at half. FREEZE, SYNC and
    // the division are never rolled (a frozen or tempo-locked cloud from a dice
    // roll reads as a fault, not a choice).
    p.push_back ({ fx::grainEnable, "Glitter On", Section::fxGrain,
                   ParamKind::boolParam, {}, 0.0f, "",
                   { .enabled = true, .biasCentre = 0.2f, .biasStrength = 0.5f } });
    p.push_back ({ fx::grainSize, "Glitter Size", Section::fxGrain,
                   ParamKind::floatParam, skewedRange (5.0f, 500.0f, 120.0f), 120.0f, "ms",
                   { .enabled = true, .minNorm = 0.25f, .maxNorm = 0.75f } });
    p.push_back ({ fx::grainDensity, "Glitter Density", Section::fxGrain,
                   ParamKind::floatParam, skewedRange (1.0f, 400.0f, 28.0f), 14.0f, "/s",
                   { .enabled = true, .minNorm = 0.25f, .maxNorm = 0.57f } });   // rolls stay <= ~45/s
    p.push_back ({ fx::grainSync, "Glitter Sync", Section::fxGrain,
                   ParamKind::boolParam, {}, 0.0f, "", { .enabled = false } });
    p.push_back ({ fx::grainDivision, "Glitter Div", Section::fxGrain,
                   ParamKind::choiceParam, {}, 9.0f /* 1/8 */, "",
                   { .enabled = false }, lfoDivisionNames() });
    p.push_back ({ fx::grainPitch, "Glitter Pitch", Section::fxGrain,
                   ParamKind::floatParam, { -24.0f, 24.0f, 0.01f }, 0.0f, "st",
                   { .enabled = true, .minNorm = 0.25f, .maxNorm = 0.75f,
                            .biasCentre = 0.5f, .biasStrength = 0.4f } });
    // SPREAD TIME keeps the 1.0.29 ID "fxGrain.spread" (random start position,
    // interval and stereo scatter). SPREAD PITCH (1.0.30) took over the pitch
    // jitter the old SPREAD also did: it was +/-12 st x SPREAD, so a 1.0.29
    // value s migrates to spreadPitch = 12 s semitones (restoreStateTree).
    p.push_back ({ fx::grainSpread, "Glitter Spread Time", Section::fxGrain,
                   ParamKind::floatParam, { 0.0f, 1.0f }, 0.25f, "",
                   { .enabled = true, .maxNorm = 0.6f }, {}, true });
    p.push_back ({ fx::grainSpreadPitch, "Glitter Spread Pitch", Section::fxGrain,
                   ParamKind::floatParam, { 0.0f, 12.0f, 0.01f }, 3.0f, "st",
                   { .enabled = true, .maxNorm = 0.5f } });   // rolls stay <= 6 st
    p.push_back ({ fx::grainPosition, "Glitter Position", Section::fxGrain,
                   ParamKind::floatParam, skewedRange (0.0f, 4000.0f, 600.0f), 300.0f, "ms",
                   { .enabled = true, .maxNorm = 0.6f } });
    p.push_back ({ fx::grainReverse, "Glitter Reverse", Section::fxGrain,
                   ParamKind::floatParam, { 0.0f, 1.0f }, 0.0f, "",
                   { .enabled = true, .maxNorm = 0.5f, .biasCentre = 0.0f,
                            .biasStrength = 0.3f }, {}, true });
    p.push_back ({ fx::grainFeedback, "Glitter Feedback", Section::fxGrain,
                   ParamKind::floatParam, { 0.0f, 0.9f }, 0.0f, "",
                   { .enabled = true, .maxNorm = 0.5f, .biasCentre = 0.1f,
                            .biasStrength = 0.4f }, {}, true });
    p.push_back ({ fx::grainMix, "Glitter Mix", Section::fxGrain,
                   ParamKind::floatParam, { 0.0f, 1.0f }, 0.35f, "",
                   { .enabled = true, .minNorm = 0.15f, .maxNorm = 0.5f }, {}, true });
    // FREEZE stays registered (sessions and host automation reference it) but
    // has no control any more: RELEASE at infinity is the same hold, and a
    // state that carries freeze = on loads as RELEASE infinite (restoreStateTree).
    p.push_back ({ fx::grainFreeze, "Glitter Freeze", Section::fxGrain,
                   ParamKind::boolParam, {}, 0.0f, "", { .enabled = false } });

    // FX FILTER (SPAStrip): two filters ported from SPASynth's filter section
    // (MultiModeFilter). Appended after every existing FX parameter. Defaults and
    // RandomSpecs are the synth's, except fxFilter.enable defaults OFF (an insert
    // effect must not change the sound until it is switched on). The pass/ceiling
    // rules that keep a roll from silencing the track are NOT specs: they are
    // params::applyFilterGuards (Randomizer.cpp), applied after the draws.
    {
        const juce::StringArray filterTypes { "LP 12", "LP 24", "HP 12", "HP 24",
                                              "BP 12", "BP 24", "Notch 12", "Notch 24" };
        p.push_back ({ fx::filterEnable, "Filter 1 On", Section::fxFilter,
                       ParamKind::boolParam, {}, 0.0f, "",
                       { .enabled = true, .biasCentre = 0.8f, .biasStrength = 0.5f } });
        p.push_back ({ fx::filterRouting, "Filter Routing", Section::fxFilter,
                       ParamKind::choiceParam, {}, 0.0f, "", { .enabled = true },
                       { "Series", "Parallel" } });

        const struct { const char* type; const char* cutoff; const char* res; const char* drive;
                       const char* mix; const char* prefix; } f[2] {
            { fx::filter1Type, fx::filter1Cutoff, fx::filter1Res, fx::filter1Drive, fx::filter1Mix, "Filter 1 " },
            { fx::filter2Type, fx::filter2Cutoff, fx::filter2Res, fx::filter2Drive, fx::filter2Mix, "Filter 2 " } };

        for (int i = 0; i < 2; ++i)
        {
            const juce::String n = f[i].prefix;
            if (i == 1)
                p.push_back ({ fx::filter2Enable, n + "On", Section::fxFilter,
                               ParamKind::boolParam, {}, 0.0f, "",
                               { .enabled = true, .biasCentre = 0.3f, .biasStrength = 0.4f } });
            p.push_back ({ f[i].type, n + "Type", Section::fxFilter,
                           ParamKind::choiceParam, {}, 0.0f, "", { .enabled = true }, filterTypes });
            p.push_back ({ f[i].cutoff, n + "Cutoff", Section::fxFilter,
                           ParamKind::floatParam, frequencyRange (20.0f, 20000.0f), 20000.0f, "Hz",
                           { .enabled = true, .minNorm = 0.2f, .biasCentre = 0.6f, .biasStrength = 0.3f } });
            p.push_back ({ f[i].res, n + "Res", Section::fxFilter,
                           ParamKind::floatParam, { 0.0f, 1.0f }, 0.0f, "",
                           { .enabled = true, .maxNorm = 0.85f, .biasCentre = 0.3f, .biasStrength = 0.4f } });
            p.push_back ({ f[i].drive, n + "Drive", Section::fxFilter,
                           ParamKind::floatParam, { 0.0f, 1.0f }, 0.0f, "",
                           { .enabled = true, .maxNorm = 0.7f, .biasCentre = 0.2f, .biasStrength = 0.5f } });
            p.push_back ({ f[i].mix, n + "Mix", Section::fxFilter,
                           ParamKind::floatParam, { 0.0f, 1.0f }, 1.0f, "",
                           { .enabled = true, .minNorm = 0.5f, .biasCentre = 0.95f, .biasStrength = 0.5f },
                           {}, true });
        }
    }

    // --- Sidechain detector (phase 2) --------------------------------------
    // Randomization is disabled for all of these: routing / detector set-up,
    // not creative parameters.
    {
        namespace sc = id::sc;
        p.push_back ({ sc::source, "SC Source", Section::sidechain,
                       ParamKind::choiceParam, {}, 0.0f /* External */, "",
                       { .enabled = false }, juce::StringArray { "External", "Input" } });
        p.push_back ({ sc::gain, "SC Gain", Section::sidechain,
                       ParamKind::floatParam, { -24.0f, 24.0f, 0.1f }, 0.0f, "dB",
                       { .enabled = false } });
        p.push_back ({ sc::attack, "SC Attack", Section::sidechain,
                       ParamKind::floatParam, skewedRange (0.1f, 500.0f, 10.0f), 10.0f, "ms",
                       { .enabled = false } });
        p.push_back ({ sc::release, "SC Release", Section::sidechain,
                       ParamKind::floatParam, skewedRange (5.0f, 2000.0f, 150.0f), 150.0f, "ms",
                       { .enabled = false } });
        p.push_back ({ sc::hpf, "SC HPF", Section::sidechain,
                       ParamKind::floatParam, frequencyRange (20.0f, 2000.0f), 20.0f, "Hz",
                       { .enabled = false } });
        p.push_back ({ sc::listen, "SC Listen", Section::sidechain,
                       ParamKind::boolParam, {}, 0.0f, "", { .enabled = false } });
    }

    // --- Modulation matrix (phase 2): per-slot bipolar depth ---------------
    for (int s = 0; s < id::numModSlots; ++s)
        p.push_back ({ id::modSlotDepth (s), "Mod Slot " + juce::String (s + 1) + " Depth",
                       Section::modMatrix, ParamKind::floatParam, { -1.0f, 1.0f }, 0.0f, "",
                       { .enabled = false } });

    // --- COMP rebuild: per-band knee, solo, bypass -------------------------
    // Appended last, in their own host group, so no existing parameter's host
    // index or RANDOMIZE draw moves. Knee 0 dB is the original hard knee, so
    // every existing preset sounds the same. None is rolled by RANDOMIZE.
    {
        namespace c = id::fx;
        const char* names[3] { "Low", "Mid", "High" };
        for (int b = 0; b < 3; ++b)
        {
            const juce::String bn = juce::String ("Comp ") + names[b] + " ";
            p.push_back ({ id::compBand (b, c::compband::knee), bn + "Knee", Section::fxCompBands,
                           ParamKind::floatParam, { 0.0f, 24.0f, 0.1f }, 0.0f, "dB", { .enabled = false } });
            p.push_back ({ id::compBand (b, c::compband::solo), bn + "Solo", Section::fxCompBands,
                           ParamKind::boolParam, {}, 0.0f, "", { .enabled = false } });
            p.push_back ({ id::compBand (b, c::compband::bypass), bn + "Bypass", Section::fxCompBands,
                           ParamKind::boolParam, {}, 0.0f, "", { .enabled = false } });
        }
    }

    // GLITTER RELEASE (SPASynth 1.0.31's approach): how long the cloud takes
    // to die away after the input stops; the top of the knob holds it forever.
    // Appended last so no existing RANDOMIZE draw moves; rolls stay <= ~5 s and
    // are never infinite. Not a mod destination.
    p.push_back ({ id::fx::grainRelease, "Glitter Release", Section::fxGrain,
                   ParamKind::floatParam, grainReleaseRange(), 0.0f, "",
                   { .enabled = true, .maxNorm = 0.7f, .biasCentre = 0.0f, .biasStrength = 0.5f } });

    return p;
}

const std::vector<ParamDef>& all()
{
    static const std::vector<ParamDef> defs = buildDefs();
    return defs;
}

int numFxParams()
{
    int n = 0;
    for (const auto& def : all())
        if (def.section != Section::global && def.section != Section::sidechain
            && def.section != Section::modMatrix)
            ++n;
    return n;
}

const ParamDef* find (const juce::String& paramID)
{
    for (const auto& def : all())
        if (paramID == def.id)
            return &def;

    return nullptr;
}

static std::unique_ptr<juce::RangedAudioParameter> makeParameter (const ParamDef& def)
{
    const juce::ParameterID pid { def.id, 1 };

    switch (def.kind)
    {
        case ParamKind::boolParam:
            return std::make_unique<juce::AudioParameterBool> (pid, def.name,
                                                               def.defaultValue >= 0.5f);
        case ParamKind::intParam:
            return std::make_unique<juce::AudioParameterInt> (pid, def.name,
                                                              (int) def.range.start,
                                                              (int) def.range.end,
                                                              (int) def.defaultValue);
        case ParamKind::choiceParam:
            return std::make_unique<juce::AudioParameterChoice> (pid, def.name, def.choices,
                                                                 (int) def.defaultValue);
        case ParamKind::floatParam:
            break;
    }

    // Displayed values never need more than 2 decimals (and big values like
    // cutoff frequencies need fewer) — raw float noise like 0.6434523 is
    // useless to read, in the plugin UI and host automation lanes alike.
    const auto formatValue = [] (float value, int)
    {
        const auto magnitude = std::abs (value);
        return juce::String (value, magnitude >= 1000.0f ? 0
                                  : magnitude >= 100.0f ? 1 : 2);
    };

    if (def.id == id::fx::grainRelease)
    {
        // "Off", "0.35 s", "12.0 s", infinity glyph at the top. The unit is
        // part of the text so the glyph does not read "inf s".
        const auto formatRelease = [] (float v, int)
        {
            if (v < 0.05f) return juce::String ("Off");
            if (v >= 30.5f) return juce::String (juce::CharPointer_UTF8 ("\xe2\x88\x9e"));
            return juce::String (v, v >= 10.0f ? 1 : 2) + " s";
        };
        const auto parseRelease = [] (const juce::String& text)
        {
            const auto t = text.trim().toLowerCase();
            if (t.startsWith ("off") || t.isEmpty()) return 0.0f;
            if (t.contains ("inf") || t.contains (juce::String (juce::CharPointer_UTF8 ("\xe2\x88\x9e"))))
                return grainReleaseInfinite;
            return t.retainCharacters ("0123456789.").getFloatValue();
        };
        return std::make_unique<juce::AudioParameterFloat> (
            pid, def.name, def.range, def.defaultValue,
            juce::AudioParameterFloatAttributes().withStringFromValueFunction (formatRelease)
                .withValueFromStringFunction (parseRelease));
    }

    if (def.percentDisplay)
    {
        // 0..1 stored range displayed as a whole-number percentage (the FX
        // MIX knobs). "%" is a real unit label, not decoration, so it goes
        // through withLabel like every other unit — round-trips via
        // withValueFromStringFunction so host automation lanes that type
        // "13" or "13 %" both parse back to 0.13.
        const auto formatPercent = [] (float value, int) { return juce::String (juce::roundToInt (value * 100.0f)) + " %"; };
        const auto parsePercent = [] (const juce::String& text)
        {
            return juce::jlimit (0.0f, 1.0f, text.retainCharacters ("0123456789.-").getFloatValue() * 0.01f);
        };
        return std::make_unique<juce::AudioParameterFloat> (
            pid, def.name, def.range, def.defaultValue,
            juce::AudioParameterFloatAttributes().withLabel ("%")
                .withStringFromValueFunction (formatPercent)
                .withValueFromStringFunction (parsePercent));
    }

    return std::make_unique<juce::AudioParameterFloat> (
        pid, def.name, def.range, def.defaultValue,
        juce::AudioParameterFloatAttributes().withLabel (def.unit)
            .withStringFromValueFunction (formatValue));
}

juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    for (auto section : allSections)
    {
        auto group = std::make_unique<juce::AudioProcessorParameterGroup> (
            sectionName (section), sectionName (section), "|");

        for (const auto& def : all())
            if (def.section == section)
                group->addChild (makeParameter (def));

        layout.add (std::move (group));
    }

    return layout;
}

} // namespace spa::params
