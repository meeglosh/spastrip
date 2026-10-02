// Reference implementation of SPASynth's RANDOMIZE ALL, FX part only, used by
// Phase2bTests.inc to check SPAStrip's port for parity.
//
// The text below is copied from SPASynth (source/params/Randomizer.cpp and
// SPASynthProcessor::randomizeAll, FX branch) and changed ONLY where SPAStrip's
// types force it (no lock groups: the synth's "FX group unlocked" case; the
// strip's proc.setFxOrder / apvts). Do not "tidy" it: its job is to be an
// independent transcription to compare the production code against.
#pragma once

#include "SPAStripProcessor.h"
#include "params/ParameterRegistry.h"

namespace synthref
{

// SPASynth params::sampleRandomValue, verbatim.
inline float sampleRandomValue (const spa::params::RandomSpec& spec, float wildness, juce::Random& rng)
{
    auto lo = spec.minNorm;
    auto hi = spec.maxNorm;

    if (wildness > 0.5f)
    {
        const auto t = (wildness - 0.5f) * 2.0f;
        lo = juce::jmap (t, lo, 0.0f);
        hi = juce::jmap (t, hi, 1.0f);
    }
    else
    {
        const auto t = (0.5f - wildness) * 2.0f;
        lo = lo + (spec.biasCentre - lo) * t * 0.8f;
        hi = hi - (hi - spec.biasCentre) * t * 0.8f;
    }

    if (hi < lo)
        std::swap (lo, hi);

    auto v = lo + rng.nextFloat() * (hi - lo);

    const auto strength = juce::jlimit (0.0f, 1.0f,
                                        spec.biasStrength * (1.5f - wildness));
    v += (spec.biasCentre - v) * strength;

    return juce::jlimit (0.0f, 1.0f, v);
}

// SPASynth: params::randomizeAll (the FX parameters, FX group unlocked) followed by
// the FX branch of SPASynthProcessor::randomizeAll (order shuffle with the limiter
// keeping its slot, limiter forced on at defaults, convStart cap).
inline void randomizeAllFx (spa::SPAStripProcessor& proc, float wildness, juce::Random& rng)
{
    namespace params = spa::params;
    auto& apvts = proc.getAPVTS();

    for (const auto& def : params::all())
    {
        if (! def.random.enabled)
            continue;
        if (def.section == params::Section::global || def.section == params::Section::sidechain
            || def.section == params::Section::modMatrix)
            continue;   // SPAStrip: these are never part of a roll (all disabled)

        if (auto* param = apvts.getParameter (def.id))
        {
            auto v = synthref::sampleRandomValue (def.random, wildness, rng);
            param->beginChangeGesture();
            param->setValueNotifyingHost (v);
            param->endChangeGesture();
        }
    }

    if (auto* freeze = apvts.getParameter (params::id::fx::grainFreeze))
        if (freeze->getValue() != 0.0f)
        {
            freeze->beginChangeGesture();
            freeze->setValueNotifyingHost (0.0f);
            freeze->endChangeGesture();
        }

    // --- SPASynthProcessor::randomizeAll, FX branch ---
    {
        auto order = proc.getFxOrder();
        const int limiterId = (int) spa::dsp::FXChain::Module::limiter;
        const int limiterPos = order.indexOf (limiterId);
        juce::Array<int> others;
        for (int id : order) if (id != limiterId) others.add (id);
        for (int i = others.size() - 1; i > 0; --i)
            std::swap (others.getReference (i), others.getReference (rng.nextInt (i + 1)));

        juce::Array<int> shuffled;
        int oi = 0;
        for (int pos = 0; pos < order.size(); ++pos)
            shuffled.add (pos == limiterPos ? limiterId : others[oi++]);
        proc.setFxOrder (shuffled);

        namespace fx = params::id::fx;
        const auto realValue = [&apvts] (const juce::String& id)
        {
            auto* param = apvts.getParameter (id);
            return param != nullptr ? param->convertFrom0to1 (param->getValue()) : 0.0f;
        };
        const auto setNorm = [&apvts] (const juce::String& id, float norm)
        {
            if (auto* param = apvts.getParameter (id))
                param->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, norm));
        };
        const auto resetToDefault = [&apvts] (const juce::String& id)
        {
            if (auto* param = apvts.getParameter (id))
                param->setValueNotifyingHost (param->getDefaultValue());
        };
        if (auto* enableParam = apvts.getParameter (fx::limEnable))
            enableParam->setValueNotifyingHost (1.0f);
        resetToDefault (fx::limDrive);
        resetToDefault (fx::limCeiling);
        resetToDefault (fx::limRelease);
        resetToDefault (fx::limAutoRelease);
        resetToDefault (fx::limCharacter);
        resetToDefault (fx::limStereoLink);
        resetToDefault (fx::limTruePeak);
        resetToDefault (fx::limLookahead);
        resetToDefault (fx::limAutoGain);

        if (realValue (fx::convStart) > 0.5f)
            setNorm (fx::convStart, 0.5f);
    }
}

} // namespace synthref
