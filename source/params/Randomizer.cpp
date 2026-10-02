#include "Randomizer.h"

namespace spa::params
{

std::optional<Module> moduleForSection (Section s)
{
    switch (s)
    {
        case Section::fxDist:      return Module::distortion;
        case Section::fxChorus:    return Module::chorus;
        case Section::fxDelay:     return Module::delay;
        case Section::fxReverb:    return Module::reverb;
        case Section::fxEQ:        return Module::eq;
        case Section::fxMod:       return Module::mod;
        case Section::fxTremVib:   return Module::tremVib;   // fxTrem.* and fxVib.* share one section
        case Section::fxLimiter:   return Module::limiter;
        case Section::fxConvolve:  return Module::convolve;
        case Section::fxComp:      return Module::comp;
        case Section::fxGrain:     return Module::grain;
        case Section::global:
        case Section::sidechain:
        case Section::modMatrix:   break;
    }
    return std::nullopt;
}

juce::String moduleName (Module m)
{
    switch (m)
    {
        case Module::distortion: return "DIST";
        case Module::chorus:     return "CHORUS";
        case Module::delay:      return "DELAY";
        case Module::reverb:     return "REVERB";
        case Module::eq:         return "EQ";
        case Module::mod:        return "MOD";
        case Module::tremVib:    return "TREM/VIB";
        case Module::limiter:    return "LIMITER";
        case Module::convolve:   return "CONVOLVE";
        case Module::comp:       return "COMP";
        case Module::grain:      return "GRAIN";
    }
    return {};
}

// Verbatim from SPASynth (source/params/Randomizer.cpp).
float sampleRandomValue (const RandomSpec& spec, float wildness, juce::Random& rng)
{
    auto lo = spec.minNorm;
    auto hi = spec.maxNorm;

    if (wildness > 0.5f)
    {
        // Open the window toward the full range.
        const auto t = (wildness - 0.5f) * 2.0f;
        lo = juce::jmap (t, lo, 0.0f);
        hi = juce::jmap (t, hi, 1.0f);
    }
    else
    {
        // Shrink the window toward the musical centre.
        const auto t = (0.5f - wildness) * 2.0f;
        lo = lo + (spec.biasCentre - lo) * t * 0.8f;
        hi = hi - (hi - spec.biasCentre) * t * 0.8f;
    }

    if (hi < lo)
        std::swap (lo, hi);

    auto v = lo + rng.nextFloat() * (hi - lo);

    // Bias fades as wildness rises: full-wild rolls are uniform.
    const auto strength = juce::jlimit (0.0f, 1.0f,
                                        spec.biasStrength * (1.5f - wildness));
    v += (spec.biasCentre - v) * strength;

    return juce::jlimit (0.0f, 1.0f, v);
}

namespace
{
    bool isLocked (juce::uint32 mask, Module m) { return (mask & lockBit (m)) != 0; }

    bool isPermutation (const juce::Array<int>& order)
    {
        if (order.size() != numLockModules)
            return false;
        juce::uint32 seen = 0;
        for (int id : order)
        {
            if (id < 0 || id >= numLockModules || (seen & (1u << id)) != 0)
                return false;
            seen |= 1u << id;
        }
        return true;
    }

    void writeNorm (juce::RangedAudioParameter& p, float norm)
    {
        p.beginChangeGesture();
        p.setValueNotifyingHost (norm);
        p.endChangeGesture();
    }
}

juce::Array<int> shuffleFxOrder (const juce::Array<int>& order, juce::uint32 lockMask, juce::Random& rng)
{
    if (! isPermutation (order))
        return order;

    const int limiterId = (int) Module::limiter;
    const auto isFixed = [&] (int id)
    {
        return id == limiterId || isLocked (lockMask, (Module) id);
    };

    // The modules free to move, in slot order. SPASynth: `others` (everything but
    // the limiter); here additionally minus the locked ones.
    juce::Array<int> movable;
    for (int id : order)
        if (! isFixed (id))
            movable.add (id);

    for (int i = movable.size() - 1; i > 0; --i)
        std::swap (movable.getReference (i), movable.getReference (rng.nextInt (i + 1)));

    juce::Array<int> shuffled;
    int next = 0;
    for (int id : order)
        shuffled.add (isFixed (id) ? id : movable[next++]);
    return shuffled;
}

void randomizeAll (juce::AudioProcessorValueTreeState& apvts, float wildness, juce::uint32 lockMask,
                   juce::Array<int>& fxOrder, juce::Random& rng)
{
    // 1. Parameters, registry order, one draw per rolled parameter.
    for (const auto& def : all())
    {
        if (! def.random.enabled)
            continue;

        const auto module = moduleForSection (def.section);
        if (! module.has_value() || isLocked (lockMask, *module))
            continue;

        auto* param = apvts.getParameter (def.id);
        if (param == nullptr)
            continue;

        const auto v = sampleRandomValue (def.random, wildness, rng);

        // Limiter: drawn (RNG parity with the synth) but not written; step 3
        // sets every limiter parameter to its final value.
        if (*module == Module::limiter)
            continue;

        writeNorm (*param, v);
    }

    // 5. Hold-type toggle: a value left over from the previous state must not
    // survive a roll of the GRAIN module (a GRAIN left on FREEZE can hold an
    // empty ring and sound dead). Assigned, never drawn, so no seed reshuffles.
    if (! isLocked (lockMask, Module::grain))
        if (auto* freeze = apvts.getParameter (id::fx::grainFreeze))
            if (freeze->getValue() != 0.0f)
                writeNorm (*freeze, 0.0f);

    // 2. Chain order: locked modules and the limiter keep their slots.
    fxOrder = shuffleFxOrder (fxOrder, lockMask, rng);

    // 3. The limiter doubles as the post-randomize safety ceiling: forced on at
    // transparent defaults (not a randomized creative setting) so a painful
    // combination still gets caught. Users can switch it back off.
    if (! isLocked (lockMask, Module::limiter))
    {
        for (const auto& def : all())
        {
            if (def.section != Section::fxLimiter)
                continue;
            if (auto* param = apvts.getParameter (def.id))
                writeNorm (*param, def.id == id::fx::limEnable ? 1.0f : param->getDefaultValue());
        }
    }

    // 4. sampleRandomValue() deliberately opens a window toward 1.0 as WILD rises
    // past 0.5, so RandomSpec's maxNorm=0.5 on Convolve START is only a soft
    // bias. Past the halfway point the impulse gets progressively thinner, so
    // RANDOMIZE ALL must never land there whatever the wildness (the full range
    // stays available by hand).
    if (! isLocked (lockMask, Module::convolve))
        if (auto* start = apvts.getParameter (id::fx::convStart))
            if (start->convertFrom0to1 (start->getValue()) > 0.5f)
                writeNorm (*start, start->convertTo0to1 (0.5f));
}

} // namespace spa::params
