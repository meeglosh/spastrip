#pragma once

#include "../SPAStripProcessor.h"
#include "Theme.h"

namespace spa::ui
{

// On-knob modulation visualisation. Drives the SAME slider-property protocol
// SPAStripLookAndFeel::drawRotarySlider reads (ported from SPASynth):
//   modAssigned          the parameter is the target of >= 1 mod slot
//   modRangeNeg/Pos      normalised reach of the slots' depths (the "reach arc")
//   modActive / modValue live modulated position (the overlay arc + dot)
// for every juce::Slider under `root` that carries a "paramID" property whose
// parameter is a mod target. One shared 30 Hz timer per editor; message
// thread only; reads the processor's slot targets, the depth parameters and
// Telemetry::modSlotOffset (all lock-free). Repaints a knob only when a
// published value actually changed.
class ModVizDriver : private juce::Timer
{
public:
    ModVizDriver (SPAStripProcessor&, juce::Component& root);
    ~ModVizDriver() override;

    // Additional roots that are not (always) parented under `root` -- the tab panels.
    void setExtraRoots (std::vector<juce::Component*>);
    // Re-collects the sliders under root (call after the panels are built).
    void rescan();
    // Starts/stops the 30 Hz poll (the editor stops it while hidden).
    void setActive (bool);
    // One poll, synchronously (tests / snapshot tool).
    void pollNow() { timerCallback(); }

    // Per-parameter view of the current assignment (also used by tests).
    struct Reach { bool assigned = false; float neg = 0.0f, pos = 0.0f, offset = 0.0f; };
    Reach getReach (const juce::String& paramID) const;

private:
    void timerCallback() override;
    SPAStripProcessor& processor;
    juce::Component& rootComponent;
    std::vector<juce::Component*> extraRoots;
    struct Entry { juce::Slider* slider; juce::String paramID; juce::RangedAudioParameter* parameter; };
    std::vector<Entry> entries;
    std::vector<juce::Component::SafePointer<juce::Slider>> safe;
};

} // namespace spa::ui
