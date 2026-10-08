#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <spa_fx/Limiter.h>

namespace spa::dsp
{

// LIMIT lives in spa-fx (JUCE-free); this adapter keeps the juce::AudioBuffer call.
class Limiter : public spa::fx::Limiter
{
public:
    using spa::fx::Limiter::process;
    void process (juce::AudioBuffer<float>& buffer, const Params& p)
    {
        const int numCh = juce::jmin (2, buffer.getNumChannels());
        spa::fx::Limiter::process (buffer.getWritePointer (0), numCh > 1 ? buffer.getWritePointer (1) : nullptr,
                                   buffer.getNumSamples(), p);
    }
};

} // namespace spa::dsp
