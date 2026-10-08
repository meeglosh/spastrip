#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <spa_fx/TremVib.h>

namespace spa::dsp
{

// The DSP lives in the shared spa-fx module (spa::fx::TremVib). This thin adapter
// only adds the juce::AudioBuffer entry point the FX chain and tests use.
class TremVib : public spa::fx::TremVib
{
public:
    using spa::fx::TremVib::process;

    void process (juce::AudioBuffer<float>& buffer, const Params& p)
    {
        process (buffer.getArrayOfWritePointers(), buffer.getNumChannels(), buffer.getNumSamples(), p);
    }
};

} // namespace spa::dsp
