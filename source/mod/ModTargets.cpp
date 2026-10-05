#include "ModTargets.h"

#include "../params/ParameterRegistry.h"

#include <atomic>

namespace spa::mod
{

namespace
{
    // Audited exclusions (see the phase 2a report for the measurements).
    std::vector<ModExclusion> buildExclusions()
    {
        namespace fx = params::id::fx;
        const juce::String reshape =
            "Changing it triggers an off-thread IR reshape and convolution-engine reload "
            "(FXChain::setConvolutionShaping -> reshapeConvolutionIR: allocation, a full "
            "re-decay/damp pass over the IR and a background partition rebuild), which "
            "cannot run at control rate.";
        const juce::String intDelay =
            "Integer-sample, non-interpolated read of a delay line: every change of the target "
            "re-points the read by whole samples, which splices the audio (measured: a click train "
            "at -20..-55 dB re a steady tone even with a per-sample refresh, versus -90 dB "
            "unmodulated), and it cannot be smoothed without changing the DSP.";
        return {
            { fx::convDecay,   reshape },
            { fx::convDamping, reshape },
            { fx::convStart,   reshape },
            { fx::reverbSize,
              "Re-sizes the tank: every delay / allpass length in the Dattorro network is an integer "
              "number of samples recomputed from it, so any change re-points taps inside the "
              "recirculating loops (measured: full-scale discontinuities, +3..-16 dB re a steady tone "
              "at any refresh rate). Unsafe at control rate; cannot be fixed without interpolated "
              "tank lengths." },
            { fx::reverbPreDelay, intDelay },
            { fx::convPreDelay,   intDelay },
            { fx::grainRelease,
              "A decay time whose top of travel is a hold (FREEZE): modulation across that point "
              "would switch the cloud in and out of hold, and below it the loop gain is set once per "
              "block. Not a mod destination in SPASynth either." },
        };
    }

   #ifdef SPASTRIP_MOD_AUDIT
    std::atomic<bool> auditIncludeExcluded { false };
   #endif

    std::vector<ModTarget> buildTargets (bool includeExcluded)
    {
        std::vector<ModTarget> out;
        for (const auto& def : params::all())
        {
            if (def.kind != params::ParamKind::floatParam)
                continue;
            if (def.section == params::Section::global || def.section == params::Section::sidechain
                || def.section == params::Section::modMatrix)
                continue;

            bool excluded = false;
            for (const auto& e : exclusions())
                excluded = excluded || e.id == def.id;
            if (excluded && ! includeExcluded)
                continue;

            out.push_back ({ def.id, def.name, params::sectionName (def.section) });
        }
        return out;
    }
}

const std::vector<ModExclusion>& exclusions()
{
    static const std::vector<ModExclusion> e = buildExclusions();
    return e;
}

#ifdef SPASTRIP_MOD_AUDIT
void setAuditIncludeExcluded (bool include) { auditIncludeExcluded.store (include); }
#endif

const std::vector<ModTarget>& targets()
{
    static const std::vector<ModTarget> t = buildTargets (false);
   #ifdef SPASTRIP_MOD_AUDIT
    static const std::vector<ModTarget> all = buildTargets (true);
    if (auditIncludeExcluded.load())
        return all;
   #endif
    return t;
}

int indexOf (const juce::String& parameterID)
{
    if (parameterID.isEmpty())
        return -1;
    const auto& t = targets();
    for (size_t i = 0; i < t.size(); ++i)
        if (t[i].id == parameterID)
            return (int) i;
    return -1;
}

} // namespace spa::mod
