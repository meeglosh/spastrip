#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <spa_fx/ParametricEQ.h>

namespace spa::dsp
{

// The EQ DSP lives in the shared spa-fx module (spa::fx::ParametricEQ). This thin
// adapter only adds the juce::AudioBuffer entry point the FX chain and tests use.
class ParametricEQ : public spa::fx::ParametricEQ
{
public:
    using spa::fx::ParametricEQ::process;

    void process (juce::AudioBuffer<float>& buffer)
    {
        process (buffer.getArrayOfWritePointers(), buffer.getNumChannels(), buffer.getNumSamples());
    }
};

} // namespace spa::dsp
