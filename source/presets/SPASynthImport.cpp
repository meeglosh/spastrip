// Import of SPASynth (.spasynth) presets into SPAStrip: FX parameters + chain
// order only. See PresetManager::importSPASynthPreset.

#include "PresetManager.h"

#include <cmath>

namespace spa::preset
{

namespace
{
    constexpr const char* synthPresetTag = "SPASynthPreset";

    // The synth's own preset-file spelling of these (see SPASynth's
    // SPASynthProcessor.cpp: paramValueType / convIR / fxOrder).
    constexpr const char* kParamType = "PARAM";
    constexpr const char* kConvIRProperty = "convIR";
    constexpr const char* kFxOrderProperty = "fxOrder";

    bool isFxParam (const juce::String& id)
    {
        const auto* def = params::find (id);
        return def != nullptr && params::moduleForSection (def->section).has_value();
    }

    // A saved order from before 1.0.29 (nine nibbles forming a permutation of the
    // nine original modules, nothing above them) or, SPAStripAdded, from before the
    // FILTER module (eleven nibbles, the synth's own current order). Both are
    // widened by FXChain::unpackOrder.
    bool isLegacyModuleOrder (juce::uint64 packed, int n)
    {
        if ((packed >> (n * 4)) != 0)
            return false;
        unsigned seen = 0;
        for (int i = 0; i < n; ++i)
        {
            const int id = (int) ((packed >> (i * 4)) & 0xF);
            if (id >= n || (seen & (1u << id)) != 0)
                return false;
            seen |= 1u << id;
        }
        return true;
    }

    // A numeric PARAM value: finite, written as a number.
    bool parseValue (const juce::var& v, double& out)
    {
        const auto text = v.toString().trim();
        if (text.isEmpty() || ! text.containsOnly ("0123456789+-.eE"))
            return false;
        out = text.getDoubleValue();
        return std::isfinite (out);
    }

    // The real (un-normalised) value, clamped into the parameter's legal range.
    double legalValue (const params::ParamDef& def, double v)
    {
        switch (def.kind)
        {
            case params::ParamKind::boolParam:   return v >= 0.5 ? 1.0 : 0.0;
            case params::ParamKind::choiceParam: return (double) juce::jlimit (0, juce::jmax (0, def.choices.size() - 1), (int) std::lround (v));
            case params::ParamKind::intParam:
            case params::ParamKind::floatParam:  return juce::jlimit ((double) def.range.start, (double) def.range.end, v);
        }
        return v;
    }
}

PresetManager::ImportResult PresetManager::importSPASynthPreset (const juce::File& file)
{
    ImportResult r;

    if (! file.existsAsFile())
    {
        r.error = "The file does not exist.";
        return r;
    }
    if (file.getSize() > maxPresetFileBytes)
    {
        r.error = "The file is too large.";
        return r;
    }
    const auto xml = juce::XmlDocument::parse (file);
    if (xml == nullptr || ! xml->hasTagName (synthPresetTag) || xml->getFirstChildElement() == nullptr)
    {
        r.error = "That is not a SPASynth preset.";
        return r;
    }
    const auto synthState = juce::ValueTree::fromXml (*xml->getFirstChildElement());
    if (! synthState.isValid())
    {
        r.error = "That preset's data could not be read.";
        return r;
    }
    r.presetName = xml->getStringAttribute ("name", file.getFileNameWithoutExtension()).trim();
    if (r.presetName.isEmpty())
        r.presetName = file.getFileNameWithoutExtension();

    // --- 1. Collect the synth's PARAM id -> real value pairs (last one wins, as
    // an APVTS restore would) and sort them into applied / skipped.
    std::vector<std::pair<juce::String, double>> fileValues;
    const auto setFileValue = [&fileValues] (const juce::String& id, double v)
    {
        for (auto& e : fileValues)
            if (e.first == id)
            {
                e.second = v;
                return;
            }
        fileValues.emplace_back (id, v);
    };
    for (int i = 0; i < synthState.getNumChildren(); ++i)
    {
        const auto child = synthState.getChild (i);
        if (! child.hasType (kParamType))
            continue;
        const auto id = child.getProperty ("id").toString();
        double v = 0.0;
        if (id.isEmpty() || ! isFxParam (id) || ! parseValue (child.getProperty ("value"), v))
        {
            if (id.isNotEmpty())
                r.skippedIds.addIfNotAlreadyThere (id);   // synth-only / non-FX / unreadable
            continue;
        }
        setFileValue (id, v);
    }

    // --- 2. SPASynth's 1.0.29 -> 1.0.30 grain migration (SPASynthProcessor.cpp
    // migrateGrainSpread): the old single SPREAD jittered pitch by +/-12 st x
    // SPREAD too, so a state that carries fxGrain.spread but no
    // fxGrain.spreadPitch gets spreadPitch = 12 x spread, and sounds as it did.
    {
        double oldSpread = -1.0;
        bool hasPitch = false;
        for (const auto& e : fileValues)
        {
            if (e.first == params::id::fx::grainSpreadPitch) hasPitch = true;
            if (e.first == params::id::fx::grainSpread)       oldSpread = e.second;
        }
        if (! hasPitch && oldSpread >= 0.0)
        {
            setFileValue (params::id::fx::grainSpreadPitch, juce::jlimit (0.0, 12.0, 12.0 * oldSpread));
            r.grainSpreadMigrated = true;
        }
    }

    // --- 3. Start from the live preset-state and overlay the FX parameters. Every
    // FX parameter the file does not carry (a preset from before COMP / GRAIN
    // existed, say) takes its registry default, exactly as the synth restores a
    // preset with a missing parameter. Everything that is not an FX parameter
    // keeps its live value.
    auto incoming = processor.capturePresetState();
    for (int i = 0; i < incoming.getNumChildren(); ++i)
    {
        auto child = incoming.getChild (i);
        if (! child.hasType (kParamType))
            continue;
        const auto id = child.getProperty ("id").toString();
        const auto* def = params::find (id);
        if (def == nullptr || ! params::moduleForSection (def->section).has_value())
            continue;

        bool found = false;
        for (const auto& e : fileValues)
            if (e.first == id)
            {
                child.setProperty ("value", legalValue (*def, e.second), nullptr);
                ++r.applied;
                found = true;
                break;
            }
        if (! found)
        {
            child.setProperty ("value", (double) def->defaultValue, nullptr);
            ++r.defaulted;
        }
    }

    // --- 4. Chain order. A legacy nine-module value (before COMP / GRAIN) is
    // widened by FXChain::unpackOrder (GRAIN before DELAY, COMP before LIMITER);
    // a value that is no permutation at all falls back to the natural order.
    if (synthState.hasProperty (kFxOrderProperty))
    {
        const auto packed = (juce::uint64) (juce::int64) synthState.getProperty (kFxOrderProperty);
        dsp::FXChain::Module order[dsp::FXChain::numModules];
        dsp::FXChain::unpackOrder (packed, order);
        const auto normalised = dsp::FXChain::packOrder (order);
        incoming.setProperty ("fxOrder", (juce::int64) normalised, nullptr);
        if (normalised == packed)
            r.order = ImportResult::Order::applied;
        else if (isLegacyModuleOrder (packed, dsp::FXChain::legacyNumModules)
                 || isLegacyModuleOrder (packed, dsp::FXChain::preFilterNumModules))
            r.order = ImportResult::Order::migratedLegacy;
        else
            r.order = ImportResult::Order::invalid;
    }
    else
    {
        incoming.setProperty ("fxOrder", (juce::int64) dsp::FXChain::defaultOrderPacked(), nullptr);
    }

    // --- 5. Convolution IR. The synth keeps a path: "$LIB$..." is relative to the
    // synth's sample library (code SPAStrip does not have), anything else is an
    // absolute path. Only an absolute path to an existing file is resolved; in
    // every other case the IR is left exactly as it is.
    juce::File irFile;
    r.irPath = synthState.getProperty (kConvIRProperty).toString();
    if (r.irPath.isNotEmpty())
    {
        r.ir = ImportResult::IR::unresolved;
        if (! r.irPath.startsWith ("$LIB$") && juce::File::isAbsolutePath (r.irPath))
        {
            const juce::File candidate (r.irPath);
            if (candidate.existsAsFile())
                irFile = candidate;
        }
    }

    // --- 6. Apply as ONE undo step (the inner steps of applyPresetState and the
    // IR load fold into this one).
    {
        SPAStripProcessor::UndoStep step (processor, "IMPORT SPASYNTH PRESET", true, true);
        if (! processor.applyPresetState (incoming, "IMPORT SPASYNTH PRESET"))
        {
            r.error = "The preset could not be applied.";
            r.applied = r.defaulted = 0;
            r.order = ImportResult::Order::notInFile;
            return r;
        }
        if (irFile != juce::File() && processor.loadConvolutionIR (irFile))
            r.ir = ImportResult::IR::resolved;
    }

    // A SPASynth preset has no SPAStrip file behind it: nothing to compare with,
    // so it reads as "edited" until the user saves it.
    setIdentity (r.presetName, juce::File(), {}, false, false);
    r.ok = true;
    return r;
}

} // namespace spa::preset
