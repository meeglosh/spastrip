#pragma once

#include <juce_core/juce_core.h>

#include <vector>

namespace spa::mod
{

// Modulation-matrix targets: the FLOAT effect parameters that can be modulated
// safely at control rate. The list is derived from the parameter registry
// (every floatParam in an fx* section) minus the audited exclusions below, so a
// new effect parameter is a target by default and has to be excluded
// deliberately. The position in targets() is the stable table index the audio
// thread uses (an atomic int per slot); it is NOT persisted (the parameter ID
// string is).
struct ModTarget
{
    juce::String id;            // parameter ID, e.g. "fxEQ.band2.gain"
    juce::String displayName;   // e.g. "EQ B3 Gain"
    juce::String owner;         // owning effect, e.g. "FX EQ"
};

struct ModExclusion
{
    juce::String id;
    juce::String reason;
};

const std::vector<ModTarget>& targets();
const std::vector<ModExclusion>& exclusions();

#ifdef SPASTRIP_MOD_AUDIT
// Test-target-only (defined by CMake for SPAStripTests, never for the plugin):
// when true, targets()/indexOf() ALSO list the excluded parameters, so the
// zipper audit (SPAStripTests --audit-mod) can measure them and back the
// exclusions with numbers. Set before constructing any processor.
void setAuditIncludeExcluded (bool include);
#endif

// Index into targets(), or -1 for an unknown OR excluded ID (and for "").
int indexOf (const juce::String& parameterID);

} // namespace spa::mod
