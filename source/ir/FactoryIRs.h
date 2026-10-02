#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include <vector>

namespace spa::factory
{

// Factory impulse responses embedded in the plugin (juce_add_binary_data, driven
// by assets/irs/manifest.json at CMake configure time: only entries with role
// "representative" are embedded). Message-thread use only.
struct IR
{
    juce::String id;         // manifest id, e.g. "dark-plate"; the state stores "factory:<id>"
    juce::String name;       // display name
    juce::String category;
};

// Manifest order, grouped by category (categories in order of first
// appearance, entries within a category in manifest order).
const std::vector<IR>& list();

// nullptr when the id is not an embedded factory IR.
const IR* find (const juce::String& id);

// Decodes the embedded FLAC (up to 2 channels). False for an unknown id or a
// decode failure; `out` / `sampleRate` are then untouched.
bool decode (const juce::String& id, juce::AudioBuffer<float>& out, double& sampleRate);

// The embedded assets/irs/CREDITS.md.
juce::String credits();

// "factory:" prefix of the IR source string kept in the plugin state.
inline constexpr const char* sourcePrefix = "factory:";

} // namespace spa::factory
