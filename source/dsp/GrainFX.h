#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <spa_fx/GrainFX.h>
#include "Telemetry.h"
#include "../params/LfoDivisions.h"

namespace spa::dsp
{

// GLITTER (formerly GRAIN): the DSP lives in the shared spa-fx module
// (spa_fx/GrainFX.h). This is the thin product adapter: it resolves the tempo
// division, wraps the JUCE buffer and denormal guard, and publishes into
// Telemetry. The grain cloud itself is documented in the shared header.
class GrainFX
{
public:
    using Core = spa::fx::GrainFX;
    static constexpr int maxGrains = Core::maxGrains;
    static constexpr float maxDensityHz = Core::maxDensityHz;
    static constexpr float kCeiling = Core::kCeiling;
    static constexpr double bufferSeconds = Core::bufferSeconds;
    static constexpr float infiniteRelease = Core::infiniteRelease;
    static constexpr float minRelease = Core::minRelease;
    static constexpr float kMaxReleaseGain = Core::kMaxReleaseGain;
    static constexpr float maxFeedback = Core::maxFeedback;

    struct Params
    {
        bool enable = false;
        float sizeMs = 120.0f;
        float densityHz = 14.0f;
        bool sync = false;
        int division = 9;            // lfoDivisionNames() index, used when sync
        double bpm = 120.0;
        float pitchSt = 0.0f;
        float spread = 0.25f;
        float spreadPitchSt = 3.0f;
        float positionMs = 300.0f;
        float reverse = 0.0f;
        float feedback = 0.0f;
        float mix = 0.35f;
        float releaseSec = 0.0f;     // 0 = off, ~0.1..30 s to -60 dB, >= infiniteRelease = hold forever
    };

    void prepare (double sr, int maxBlock) { core.prepare (sr, maxBlock); }
    void reset() { core.reset(); }

    void process (juce::AudioBuffer<float>& buffer, const Params& p)
    {
        if (! p.enable)
        {
            core.process (nullptr, nullptr, 0, convert (p));   // disable edge only
            return;
        }
        const int n = buffer.getNumSamples();
        const int numCh = juce::jmin (2, buffer.getNumChannels());
        if (n <= 0 || numCh <= 0)
            return;
        juce::ScopedNoDenormals noDenormals;
        core.process (buffer.getWritePointer (0), numCh > 1 ? buffer.getWritePointer (1) : nullptr, n, convert (p));
    }

    std::uint64_t grainsSpawned() const { return core.grainsSpawned(); }
    bool isFrozen() const { return core.isFrozen(); }
    float lastSpawnSemis() const { return core.lastSpawnSemis(); }
    double lastSpawnBackSamples() const { return core.lastSpawnBackSamples(); }
    bool isCapturing() const { return core.isCapturing(); }
    int audibleSamples() const { return core.audibleSamples(); }
    int activeGrains() const { return core.activeGrains(); }
    float ringPeak() const { return core.ringPeak(); }
    const float* ringData (int ch) const { return core.ringData (ch); }
    int ringLength() const { return core.ringLength(); }
    std::uint64_t limitedSamples() const { return core.limitedSamples(); }
    // Host-reported tail (RELEASE infinite = the 300 s ceiling).
    static double tailSeconds (const Params& p)
    {
        Core::Params c;
        c.positionMs = p.positionMs; c.sizeMs = p.sizeMs; c.feedback = p.feedback; c.releaseSec = p.releaseSec;
        return Core::tailSeconds (c);
    }
    void publish (Telemetry::GrainFxViz& viz) const { core.publish (viz); }

private:
    static Core::Params convert (const Params& p)
    {
        Core::Params c;
        c.enable = p.enable; c.sizeMs = p.sizeMs; c.densityHz = p.densityHz; c.sync = p.sync;
        c.syncBeats = params::lfoDivisionBeats (p.division);
        c.bpm = p.bpm; c.pitchSt = p.pitchSt; c.spread = p.spread; c.spreadPitchSt = p.spreadPitchSt;
        c.positionMs = p.positionMs; c.reverse = p.reverse; c.feedback = p.feedback; c.mix = p.mix;
        c.releaseSec = p.releaseSec;
        return c;
    }

    Core core;
};

} // namespace spa::dsp
