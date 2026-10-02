#pragma once

#include <juce_core/juce_core.h>

#include <memory>

namespace spa
{

// The convolution impulse response as it lives in the plugin state.
//
// Immutable once built and always handled through a StoredIRPtr, so undo
// snapshots can share the (possibly multi-megabyte) FLAC blob by reference
// instead of copying it into every history step.
struct StoredIR
{
    juce::String source { "none" };   // "none" | "embedded" | "factory:<id>"
    juce::String name;                // original file name (no extension), for display
    double sampleRate = 0.0;
    int numChannels = 0;
    int numSamples = 0;
    juce::MemoryBlock flac;           // 24-bit FLAC of the (peak-normalised) IR; empty for factory IRs

    bool isEmbedded() const { return source == "embedded"; }
    bool isFactory() const { return source.startsWith ("factory:"); }
};

using StoredIRPtr = std::shared_ptr<const StoredIR>;

} // namespace spa
