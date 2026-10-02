#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include "../params/FilterType.h"

#include <array>
#include <cmath>

namespace spa::dsp
{

// TPT state-variable filter (Zavalishin) computing LP/BP/HP simultaneously so
// all of the registry's modes (incl. notch = LP + HP) fall out of one core.
// 24 dB modes cascade two stages. Per-voice, stereo.
class MultiModeFilter
{
public:
    void prepare (double newSampleRate) noexcept
    {
        sampleRate = newSampleRate;
        reset();
    }

    void reset() noexcept
    {
        for (auto& s : stages)
            s = {};
    }

    void setParams (params::FilterType newType, float cutoffHz, float resonance, float newDrive) noexcept
    {
        type = newType;
        drive = newDrive;

        const auto fc = juce::jlimit (20.0f, (float) (sampleRate * 0.45), cutoffHz);
        g = (float) std::tan (juce::MathConstants<double>::pi * fc / sampleRate);
        // Map resonance 0..1 onto k = 2..0.1 (self-oscillation guarded off).
        k = 2.0f - 1.9f * juce::jlimit (0.0f, 1.0f, resonance);

        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }

    float processSample (int channel, float x) noexcept
    {
        if (drive > 0.0f)
            x = std::tanh (x * (1.0f + 4.0f * drive)) / (1.0f + drive);

        const bool is24dB = type == params::FilterType::lp24 || type == params::FilterType::hp24
                         || type == params::FilterType::bp24 || type == params::FilterType::notch24;

        auto y = tick (stages[(size_t) channel * 2], x);
        if (is24dB)
            y = tick (stages[(size_t) channel * 2 + 1], y);

        return y;
    }

private:
    struct State { float ic1eq = 0.0f, ic2eq = 0.0f; };

    float tick (State& s, float x) noexcept
    {
        const auto v3 = x - s.ic2eq;
        const auto v1 = a1 * s.ic1eq + a2 * v3;
        const auto v2 = s.ic2eq + a2 * s.ic1eq + a3 * v3;
        s.ic1eq = 2.0f * v1 - s.ic1eq;
        s.ic2eq = 2.0f * v2 - s.ic2eq;

        const auto lp = v2;
        const auto bp = v1;
        const auto hp = x - k * v1 - v2;

        switch (type)
        {
            case params::FilterType::lp12:
            case params::FilterType::lp24:    return lp;
            case params::FilterType::hp12:
            case params::FilterType::hp24:    return hp;
            case params::FilterType::bp12:
            case params::FilterType::bp24:    return bp;
            case params::FilterType::notch12:
            case params::FilterType::notch24: return lp + hp;
        }
        return lp;
    }

    double sampleRate = 44100.0;
    params::FilterType type = params::FilterType::lp12;
    float g = 0.5f, k = 2.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
    float drive = 0.0f;
    std::array<State, 4> stages {};  // 2 channels x 2 cascade stages
};

} // namespace spa::dsp
