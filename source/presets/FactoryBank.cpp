#include "FactoryBank.h"

#include "../dsp/FXChain.h"
#include "../mod/ModTargets.h"
#include "../params/ParameterRegistry.h"
#include "PresetManager.h"

#include <algorithm>
#include <array>

// SPAStrip factory bank: 12 types x 6 presets = 72 (the table is FactoryBankTable.cpp; this file
// is the recipe parser). Every preset is a text recipe, one
// token per parameter, whitespace separated:
//
//   <alias>.<leaf>=<value>     alias: dist cho dly rev eq phs trem vib lim comp glt sc
//                              (phs = the MOD phaser / flanger, glt = GLITTER)
//   <alias>.on=1               the module's enable (leaf "on" -> "enable")
//   eq.b3.freq=250             EQ band 3 (1..8) -> fxEQ.band2.freq
//   comp.lo|mid|hi.thresh      COMP band: thresh ratio upratio attack release gain knee
//   flt.on / flt.type / flt.cutoff / flt.res / flt.drive / flt.mix / flt.routing = FILTER 1,
//   flt2.* = FILTER 2
//   in= out= mix= os=          global input gain (dB), output gain (dB), whole-chain mix, oversampling
//   order=eq,comp,...          those modules first, the rest in the default order
//                              (names: flt dist cho phs trem vib glt dly rev eq comp lim; trem and vib are one tab)
//   m1=<target>:<depth>        mod slot 1..8 (target is an alias.leaf); sc.source=Input makes the
//                              strip's own signal the envelope source
//
// Values are the parameter's real units (dB, Hz, ms, 0..1, percent for the % knobs); a choice is
// its name with spaces as underscores (Low_Cut, LP_24, 1/8.), a toggle is 0/1 or on/off.
// CONV is deliberately never used: it needs an impulse response and a bank must sound the same
// with none installed.
//
// Loudness: `out=` is the per-preset trim, calibrated with `SPAStripTests --render-factory-presets`
// (prints the level change per source); every type but Mastering lands within about +-3 dB of the
// input on typical material. Bump kFactoryRecipeVersion whenever this table changes.
//
// Reverb note: REVERB's wet is -8 dB since 1.0.5, so MIX near 0.5 is about equal wet and dry.
// A MIX of 0.10 to 0.25 is a room or plate that sits behind the sound; ambient presets run 0.4+.

namespace spa::preset
{

namespace
{
    namespace pid = params::id;

    struct Alias { const char* alias; const char* section; };
    const Alias kAliases[] = {
        { "dist", "fxDist" },  { "cho", "fxChorus" }, { "dly", "fxDelay" }, { "rev", "fxReverb" },
        { "eq", "fxEQ" },      { "phs", "fxMod" },    { "trem", "fxTrem" }, { "vib", "fxVib" },
        { "lim", "fxLim" },    { "comp", "fxComp" },  { "glt", "fxGrain" },
    };

    // alias.leaf (with the shorthands above) -> parameter ID; "" when it does not parse.
    juce::String expandId (const juce::String& key)
    {
        if (key == "in")  return pid::inputGain;
        if (key == "out") return pid::outputGain;
        if (key == "mix") return pid::mix;
        if (key == "os")  return pid::oversampling;

        const auto alias = key.upToFirstOccurrenceOf (".", false, false);
        auto leaf = key.fromFirstOccurrenceOf (".", false, false);
        if (leaf.isEmpty())
            return {};

        if (alias == "sc")
            return "sc." + leaf;
        if (alias == "flt" || alias == "flt2")
        {
            if (leaf == "on")      return alias == "flt" ? "fxFilter.enable" : "fxFilter.f2.enable";
            if (leaf == "routing") return "fxFilter.routing";
            return juce::String ("fxFilter.") + (alias == "flt" ? "f1." : "f2.") + leaf;
        }
        if (alias == "eq" && leaf.length() > 3 && leaf[0] == 'b' && juce::CharacterFunctions::isDigit (leaf[1]) && leaf[2] == '.')
        {
            auto key2 = leaf.substring (3);
            return "fxEQ.band" + juce::String (leaf.substring (1, 2).getIntValue() - 1) + "." + (key2 == "on" ? juce::String ("enable") : key2);
        }
        if (alias == "comp")
        {
            const juce::StringArray bands { "lo", "mid", "hi" };
            const auto band = leaf.upToFirstOccurrenceOf (".", false, false);
            if (leaf.contains (".") && bands.contains (band))
                return "fxComp.band" + juce::String (bands.indexOf (band)) + "." + leaf.fromFirstOccurrenceOf (".", false, false);
        }
        if (leaf == "on")
            leaf = "enable";
        for (const auto& a : kAliases)
            if (alias == a.alias)
                return juce::String (a.section) + "." + leaf;
        return {};
    }

    bool parseValue (const params::ParamDef& def, juce::String text, double& out)
    {
        text = text.trim();
        switch (def.kind)
        {
            case params::ParamKind::boolParam:
                if (text == "on" || text == "1")  { out = 1.0; return true; }
                if (text == "off" || text == "0") { out = 0.0; return true; }
                return false;
            case params::ParamKind::choiceParam:
            {
                const auto want = text.replaceCharacter ('_', ' ');
                for (int i = 0; i < def.choices.size(); ++i)
                    if (def.choices[i].equalsIgnoreCase (want)) { out = (double) i; return true; }
                return false;
            }
            default:
            {
                if (text.isEmpty() || ! text.containsOnly ("-+.0123456789e"))
                    return false;
                const double v = text.getDoubleValue();
                if (v < (double) def.range.start - 1.0e-6 || v > (double) def.range.end + 1.0e-6)
                    return false;
                out = v;
                return true;
            }
        }
    }

    // Module names for order=, in the FXChain::Module enum.
    bool moduleFromName (const juce::String& n, dsp::FXChain::Module& m)
    {
        using M = dsp::FXChain::Module;
        const struct { const char* name; M m; } t[] = {
            { "flt", M::filter }, { "dist", M::distortion }, { "cho", M::chorus }, { "phs", M::mod },
            { "trem", M::tremVib }, { "vib", M::tremVib }, { "glt", M::grain }, { "dly", M::delay }, { "rev", M::reverb },
            { "eq", M::eq }, { "comp", M::comp }, { "lim", M::limiter } };
        for (const auto& e : t)
            if (n == e.name) { m = e.m; return true; }
        return false;
    }

    void setParam (juce::ValueTree& state, const juce::String& id, double value)
    {
        for (int i = 0; i < state.getNumChildren(); ++i)
        {
            auto c = state.getChild (i);
            if (c.getProperty ("id").toString() == id)
            {
                c.setProperty ("value", value, nullptr);
                return;
            }
        }
        juce::ValueTree p ("PARAM");
        p.setProperty ("id", id, nullptr);
        p.setProperty ("value", value, nullptr);
        state.appendChild (p, nullptr);
    }
}

juce::ValueTree buildFactoryState (const FactoryEntry& entry, juce::String* errors)
{
    juce::ValueTree state ("PARAMS");
    const auto fail = [&] (const juce::String& why)
    {
        if (errors != nullptr)
            *errors << entry.name << ": " << why << "\n";
    };

    juce::String order;
    std::array<juce::String, (size_t) pid::numModSlots> slotTargets;

    const auto tokens = juce::StringArray::fromTokens (juce::String (entry.recipe), " \t\r\n", "");
    for (const auto& token : tokens)
    {
        if (token.isEmpty())
            continue;
        if (! token.contains ("="))
        {
            fail ("token without a value: " + token);
            continue;
        }
        const auto key = token.upToFirstOccurrenceOf ("=", false, false);
        const auto value = token.fromFirstOccurrenceOf ("=", false, false);
        if (key == "order")
        {
            order = value;
            continue;
        }
        if (key.length() == 2 && key[0] == 'm' && juce::CharacterFunctions::isDigit (key[1]))
        {
            const int slot = key.substring (1).getIntValue();
            const auto target = expandId (value.upToFirstOccurrenceOf (":", false, false));
            const double depth = value.fromFirstOccurrenceOf (":", false, false).getDoubleValue();
            if (slot < 1 || slot > pid::numModSlots || mod::indexOf (target) < 0 || std::abs (depth) > 1.0)
            {
                fail ("bad mod slot: " + token);
                continue;
            }
            slotTargets[(size_t) slot - 1] = target;
            setParam (state, pid::modSlotDepth (slot - 1), depth);
            continue;
        }
        const auto id = expandId (key);
        const auto* def = params::find (id);
        double v = 0.0;
        if (def == nullptr)
            fail ("unknown parameter: " + token);
        else if (! parseValue (*def, value, v))
            fail ("bad value: " + token);
        else
            setParam (state, id, v);
    }

    // Chain order: the named modules first, the rest in the default order.
    {
        dsp::FXChain::Module def[dsp::FXChain::numModules];
        dsp::FXChain::unpackOrder (dsp::FXChain::defaultOrderPacked(), def);
        std::vector<dsp::FXChain::Module> result;
        for (const auto& n : juce::StringArray::fromTokens (order, ",", ""))
        {
            dsp::FXChain::Module m;
            if (! moduleFromName (n, m))
                fail ("unknown module in order: " + n);
            else if (std::find (result.begin(), result.end(), m) == result.end())
                result.push_back (m);
        }
        for (const auto m : def)
            if (std::find (result.begin(), result.end(), m) == result.end())
                result.push_back (m);
        state.setProperty ("fxOrder", (juce::int64) dsp::FXChain::packOrder (result.data()), nullptr);
    }
    for (int s = 0; s < pid::numModSlots; ++s)
        state.setProperty ("modSlot" + juce::String (s + 1) + "Target", slotTargets[(size_t) s], nullptr);
    state.setProperty ("irSource", "none", nullptr);
    state.setProperty ("convIRName", "", nullptr);
    state.setProperty ("stateVersion", 1, nullptr);
    return state;
}

std::vector<spa::presets::PresetManager::FactoryPreset> buildFactoryBank()
{
    std::vector<spa::presets::PresetManager::FactoryPreset> list;
    for (const auto& e : factoryEntries())
    {
        auto doc = std::make_unique<juce::XmlElement> (PresetManager::presetTag);
        doc->setAttribute ("name", e.name);
        doc->setAttribute ("version", PresetManager::presetFormatVersion);
        doc->setAttribute ("type", e.type);
        doc->setAttribute ("factoryRecipe", kFactoryRecipeVersion);
        doc->addChildElement (buildFactoryState (e).createXml().release());
        list.push_back ({ e.name, "Factory", doc->toString (juce::XmlElement::TextFormat().singleLine()) });
    }
    return list;
}

} // namespace spa::preset
