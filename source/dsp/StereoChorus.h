#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <spa_fx/StereoChorus.h>

namespace spa::dsp
{

// The DSP lives in the shared spa-fx module (spa::fx::StereoChorus). This thin adapter
// only adds the juce::AudioBuffer entry point the FX chain and tests use.
class StereoChorus : public spa::fx::StereoChorus
{
public:
    using spa::fx::StereoChorus::process;

    void process (juce::AudioBuffer<float>& buffer, const Params& p)
    {
        process (buffer.getArrayOfWritePointers(), buffer.getNumChannels(), buffer.getNumSamples(), p);
    }
};

} // namespace spa::dsp
