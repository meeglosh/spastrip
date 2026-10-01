#pragma once

#include <juce_core/juce_core.h>

#include <iterator>

namespace spa::params
{

// Tempo-sync divisions, in beats (quarter notes). Choice order matches
// lfoDivisionBeats(). Append-only.
//
// Lifted verbatim (names, beat values and order) from SPASynth's
// ParameterRegistry.cpp so the DSP (FXChain, GrainFX) and the parameter
// registry share one table without the DSP depending on the registry.
inline const juce::StringArray& lfoDivisionNames()
{
    static const juce::StringArray names {
        "8/1", "4/1", "2/1", "1/1",
        "1/2", "1/2T", "1/4", "1/4.", "1/4T",
        "1/8", "1/8.", "1/8T", "1/16", "1/16T", "1/32",
    };
    return names;
}

inline float lfoDivisionBeats (int divisionChoice)
{
    static constexpr float beats[] = {
        32.0f, 16.0f, 8.0f, 4.0f,
        2.0f, 4.0f / 3.0f, 1.0f, 1.5f, 2.0f / 3.0f,
        0.5f, 0.75f, 1.0f / 3.0f, 0.25f, 1.0f / 6.0f, 0.125f,
    };
    static_assert (std::size (beats) == 15);
    return beats[juce::jlimit (0, (int) std::size (beats) - 1, divisionChoice)];
}

} // namespace spa::params
