#include "Randomizer.h"
#include "../mod/ModTargets.h"

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
        case Section::fxFilter:    return Module::filter;
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
        case Module::grain:      return "GLITTER";
        case Module::filter:     return "FILTER";
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

//==============================================================================
// FILTER roll guards.
//
// The roll draws the filter parameters like any others (so the RNG sequence of
// every other parameter is unchanged), then these rules bend the result. They
// consume no random numbers, which keeps the chain-order shuffle that follows
// bit-identical to SPASynth's. Thresholds are chosen against the measurement in
// filterRollGuardTest (1000 seeded rolls, pink noise through the module): see
// that test for the bound and the worst case.
namespace
{
    enum class FilterKind { lowpass, highpass, bandpass, notch };

    FilterKind kindOf (int type)
    {
        switch (juce::jlimit (0, 7, type) / 2)
        {
            case 0:  return FilterKind::lowpass;
            case 1:  return FilterKind::highpass;
            case 2:  return FilterKind::bandpass;
            default: return FilterKind::notch;
        }
    }

    // Fold a value that fell on the wrong side of a limit back across it
    // (mirror in log frequency: a * a / x), instead of clamping onto the limit,
    // so rolls do not pile up on one frequency.
    float foldAbove (float hz, float floorHz) { return hz < floorHz ? juce::jmin (20000.0f, floorHz * floorHz / hz) : hz; }
    float foldBelow (float hz, float ceilHz)  { return hz > ceilHz ? juce::jmax (20.0f, ceilHz * ceilHz / hz) : hz; }
}

void guardFilterSettings (FilterSettings& s)
{
    namespace g = filterguard;

    FilterSettings::One* f[2] { &s.f1, &s.f2 };

    for (auto* one : f)
    {
        switch (kindOf (one->type))
        {
            case FilterKind::lowpass:  one->cutoffHz = foldAbove (one->cutoffHz, g::lowpassMinHz); break;
            case FilterKind::highpass: one->cutoffHz = foldBelow (one->cutoffHz, g::highpassMaxHz); break;
            case FilterKind::bandpass:
                // A band-pass keeps only a sliver of the spectrum: centre it in the
                // musical range and always leave part of the dry signal in.
                one->cutoffHz = foldBelow (foldAbove (one->cutoffHz, g::bandMinHz), g::bandMaxHz);
                one->mix = juce::jmin (one->mix, g::bandpassMaxMix);
                break;
            case FilterKind::notch:
                one->cutoffHz = foldBelow (foldAbove (one->cutoffHz, g::bandMinHz), g::bandMaxHz);
                break;
        }

        // Resonance on top of a hard-driven input just rings and saturates.
        if (one->drive > g::driveResonanceThreshold)
            one->resonance = juce::jmin (one->resonance, g::resonanceCapAtHighDrive);
    }

    // Two band-limiting filters in series multiply their passbands: when those
    // do not overlap there is nothing left. Keep at least g::seriesOverlapOctaves.
    // (A notch passes everything but its notch; Parallel adds its branches, so
    // neither can silence the sum.)
    if (s.f1.enabled && s.f2.enabled && ! s.parallel)
    {
        const auto k1 = kindOf (s.f1.type), k2 = kindOf (s.f2.type);
        const auto lower = [] (float hz, float octaves) { return hz / std::exp2 (octaves); };
        const float olap = g::seriesOverlapOctaves;
        const float bandHalfOct = g::bandpassHalfWidthOctaves;

        const auto fix = [&] (FilterSettings::One& a, FilterKind ka, FilterSettings::One& b, FilterKind kb, bool first)
        {
            if (ka == FilterKind::lowpass && kb == FilterKind::highpass)          // passband = hp .. lp
                b.cutoffHz = juce::jmax (20.0f, juce::jmin (b.cutoffHz, lower (a.cutoffHz, olap)));
            else if (ka == FilterKind::lowpass && kb == FilterKind::bandpass)     // raise the low-pass
                a.cutoffHz = juce::jmin (20000.0f, juce::jmax (a.cutoffHz, b.cutoffHz * std::exp2 (olap - bandHalfOct)));
            else if (ka == FilterKind::highpass && kb == FilterKind::bandpass)    // lower the high-pass
                a.cutoffHz = juce::jmax (20.0f, juce::jmin (a.cutoffHz, b.cutoffHz * std::exp2 (bandHalfOct - olap)));
            else if (first && ka == FilterKind::bandpass && kb == FilterKind::bandpass)
            {
                const float maxRatio = std::exp2 (2.0f * bandHalfOct - olap);     // keep the two centres close
                b.cutoffHz = juce::jlimit (juce::jmax (g::bandMinHz, a.cutoffHz / maxRatio),
                                           juce::jmin (g::bandMaxHz, a.cutoffHz * maxRatio), b.cutoffHz);
            }
        };
        fix (s.f1, k1, s.f2, k2, true);
        fix (s.f2, k2, s.f1, k1, false);
    }
}

void applyFilterGuards (juce::AudioProcessorValueTreeState& apvts)
{
    namespace fx = id::fx;

    const auto real = [&apvts] (const char* pid)
    {
        auto* p = apvts.getParameter (pid);
        return p != nullptr ? p->convertFrom0to1 (p->getValue()) : 0.0f;
    };

    FilterSettings s;
    s.parallel = (int) real (fx::filterRouting) == 1;
    s.f1 = { real (fx::filterEnable) >= 0.5f, (int) real (fx::filter1Type), real (fx::filter1Cutoff),
             real (fx::filter1Res), real (fx::filter1Drive), real (fx::filter1Mix) };
    s.f2 = { real (fx::filter2Enable) >= 0.5f, (int) real (fx::filter2Type), real (fx::filter2Cutoff),
             real (fx::filter2Res), real (fx::filter2Drive), real (fx::filter2Mix) };
    const auto before = s;
    guardFilterSettings (s);

    const auto write = [&apvts] (const char* pid, float oldValue, float newValue)
    {
        if (juce::exactlyEqual (newValue, oldValue))
            return;
        if (auto* p = apvts.getParameter (pid))
            writeNorm (*p, p->convertTo0to1 (newValue));
    };
    write (fx::filter1Cutoff, before.f1.cutoffHz, s.f1.cutoffHz);
    write (fx::filter1Res,    before.f1.resonance, s.f1.resonance);
    write (fx::filter1Mix,    before.f1.mix, s.f1.mix);
    write (fx::filter2Cutoff, before.f2.cutoffHz, s.f2.cutoffHz);
    write (fx::filter2Res,    before.f2.resonance, s.f2.resonance);
    write (fx::filter2Mix,    before.f2.mix, s.f2.mix);
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
                   juce::Array<int>& fxOrder, juce::Random& rng, bool guardFilter)
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

        auto v = sampleRandomValue (def.random, wildness, rng);

        // Chorus mode (SPASynth 1.0.32): VHS (index 2) is reached by the top
        // 18% of the roll; the rest keeps the old Vintage/Modern split
        // (round(v) over 0..1 becomes round(v / 0.82)), so no extra RNG draw.
        if (def.id == id::fx::chorusMode && def.choices.size() == 3)
        {
            constexpr float vhsShare = 0.18f;
            const int index = v >= 1.0f - vhsShare ? 2 : (v / (1.0f - vhsShare) < 0.5f ? 0 : 1);
            v = (float) index / 2.0f;
        }

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

    // 6. FILTER guards (SPAStrip): bend the rolled filter so it cannot leave the
    // track near-silent or ruinously thin. Deterministic, draws nothing. Only an
    // unlocked filter was rolled, so only an unlocked filter is touched.
    if (guardFilter && ! isLocked (lockMask, Module::filter))
        applyFilterGuards (apvts);

    // 4. sampleRandomValue() deliberately opens a window toward 1.0 as WILD rises
    // past 0.5, so RandomSpec's maxNorm=0.5 on Convolve START is only a soft
    // bias. Past the halfway point the impulse gets progressively thinner, so
    // RANDOMIZE ALL must never land there whatever the wildness (the full range
    // stays available by hand).
    if (! isLocked (lockMask, Module::convolve))
        if (auto* start = apvts.getParameter (id::fx::convStart))
            if (start->convertFrom0to1 (start->getValue()) > 0.5f)
                writeNorm (*start, start->convertTo0to1 (0.5f));

    // 7. Reverb lo-cut guard (SPASynth 1.0.32): a mostly-wet reverb high-passed
    // above the low register carries almost none of the track, so the sum goes
    // quiet and thin. Keep at least half the dry. Deterministic, draws nothing.
    if (! isLocked (lockMask, Module::reverb))
    {
        const auto real = [&apvts] (const char* pid)
        {
            auto* p = apvts.getParameter (pid);
            return p != nullptr ? p->convertFrom0to1 (p->getValue()) : 0.0f;
        };
        if (real (id::fx::reverbEnable) >= 0.5f && real (id::fx::reverbLowCut) > 250.0f
            && real (id::fx::reverbMix) > 0.5f)
            if (auto* mix = apvts.getParameter (id::fx::reverbMix))
                writeNorm (*mix, 0.5f);
    }
}

bool isAudibleModTarget (juce::AudioProcessorValueTreeState& apvts, const juce::String& targetId, juce::uint32 lockMask)
{
    const auto* def = find (targetId);
    if (def == nullptr)
        return false;
    const auto module = moduleForSection (def->section);
    if (! module.has_value() || *module == Module::limiter || isLocked (lockMask, *module))
        return false;

    const auto on = [&apvts] (const juce::String& pid)
    {
        auto* v = apvts.getRawParameterValue (pid);
        return v != nullptr && v->load() >= 0.5f;
    };

    // The effect's own switch: "<prefix>.enable" (fxTrem.* and fxVib.* have one
    // each; COMP's per-band params live under fxComp. like the rest).
    const auto prefix = targetId.upToFirstOccurrenceOf (".", false, false);
    if (! on (prefix + ".enable"))
        return false;

    // A sub-unit with its own switch.
    if (targetId.startsWith ("fxEQ.band"))
        if (! on (targetId.upToLastOccurrenceOf (".", false, false) + ".enable"))
            return false;
    if (targetId.startsWith ("fxFilter.f2.") && ! on (id::fx::filter2Enable))
        return false;

    // Free-running controls that do nothing while their effect is synced.
    namespace fx = id::fx;
    const struct { const char* target; const char* sync; } synced[] {
        { fx::delayTime, fx::delaySync }, { fx::modRate, fx::modSync }, { fx::tremRate, fx::tremSync },
        { fx::vibRate, fx::vibSync }, { fx::grainDensity, fx::grainSync } };
    for (const auto& s : synced)
        if (targetId == s.target && on (s.sync))
            return false;

    return true;
}

std::array<juce::String, id::numModSlots> rollModSlots (juce::AudioProcessorValueTreeState& apvts, float wildness,
                                                       juce::uint32 lockMask, juce::Random& rng)
{
    wildness = juce::jlimit (0.0f, 1.0f, wildness);

    juce::StringArray candidates;
    for (const auto& t : mod::targets())
        if (isAudibleModTarget (apvts, t.id, lockMask))
            candidates.add (t.id);

    // Nothing eligible (every effect off or locked): leave the slots as they are
    // rather than clearing modulation the user may have set up. Draws nothing.
    std::array<juce::String, id::numModSlots> targets;
    if (candidates.isEmpty())
    {
        targets[0] = keepSlotsMarker;
        return targets;
    }

    const int lo = 1 + juce::roundToInt (5.0f * wildness);
    const int count = juce::jmin (id::numModSlots, lo + rng.nextInt (3), candidates.size());

    const float maxDepth = 0.4f + 0.5f * wildness;
    for (int s = 0; s < id::numModSlots; ++s)
    {
        float depth = 0.0f;
        if (s < count)
        {
            const int pick = rng.nextInt (candidates.size());
            targets[(size_t) s] = candidates[pick];
            candidates.remove (pick);   // one slot per target
            const float magnitude = 0.15f + rng.nextFloat() * (maxDepth - 0.15f);
            depth = rng.nextBool() ? magnitude : -magnitude;
        }
        if (auto* p = apvts.getParameter (id::modSlotDepth (s)))
            writeNorm (*p, p->convertTo0to1 (depth));
    }
    return targets;
}

} // namespace spa::params
