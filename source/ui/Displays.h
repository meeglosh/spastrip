#pragma once

#include <array>
#include <functional>
#include <vector>

#include "../dsp/Telemetry.h"
#include "../params/ParameterRegistry.h"
#include "Theme.h"

namespace spa::ui
{

// Frozen wall-clock for the animated FX displays (snapshot tool / tests).
void setFxDisplayFrozenMsForTest (double ms);

// Base for the module scopes: watches a set of parameters and repaints (at a
// throttled rate) when any of them move, and keeps animating while audio is
// flowing so the scopes stay alive. Paints nothing while not showing.
class DisplayComponent : public juce::Component,
                         private juce::Timer,
                         private juce::AudioProcessorValueTreeState::Listener
{
public:
    DisplayComponent (juce::AudioProcessorValueTreeState&, juce::StringArray paramIDs,
                      const dsp::Telemetry* telemetry = nullptr);
    ~DisplayComponent() override;

    void paint (juce::Graphics&) final;
    void markDirty() { dirty.store (true); }

protected:
    virtual void paintDisplay (juce::Graphics&, juce::Rectangle<float>) = 0;

    // Subclasses that keep an idle animation running (a scrolling trace, a
    // travelling playhead) override this so the 24 Hz timer keeps repainting
    // while it's true (and the display is showing).
    virtual bool wantsAnimation() const { return false; }

    bool isLive() const;   // audio currently flowing

    juce::AudioProcessorValueTreeState& apvts;
    const dsp::Telemetry* telemetry = nullptr;

    float value (const juce::String& paramID) const;   // real-world value

private:
    void parameterChanged (const juce::String&, float) override { dirty.store (true); }
    void timerCallback() override;

    juce::StringArray watched;
    std::atomic<bool> dirty { true };
};

// FX scopes: one class, several characters.
class FXDisplay : public DisplayComponent
{
public:
    // mod = phaser/flanger (fxMod section); tremVib = tremolo+vibrato.
    enum class Kind { distortion, chorus, delay, reverb, eq, mod, tremVib, grain };

    // telemetry: the processor's Telemetry, so synced kinds can read the live
    // resolved tempo (Telemetry::bpm) and the grain cloud can draw its grains.
    FXDisplay (juce::AudioProcessorValueTreeState&, Kind, const dsp::Telemetry* telemetry = nullptr);

private:
    void paintDisplay (juce::Graphics&, juce::Rectangle<float>) override;
    bool wantsAnimation() const override;
    static juce::StringArray watchedFor (Kind);

    const Kind kind;
    mutable float lastDrawnBpm = -1.0f;
};

} // namespace spa::ui
