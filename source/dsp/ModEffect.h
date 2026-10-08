#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <spa_fx/ModEffect.h>

namespace spa::dsp
{

// The DSP lives in the shared spa-fx module (spa::fx::ModEffect). This thin adapter
// only adds the juce::AudioBuffer entry point the FX chain and tests use.
class ModEffect : public spa::fx::ModEffect
{
public:
    using spa::fx::ModEffect::process;

    void process (juce::AudioBuffer<float>& buffer, const Params& p)
    {
        process (buffer.getArrayOfWritePointers(), buffer.getNumChannels(), buffer.getNumSamples(), p);
    }
};

} // namespace spa::dsp
