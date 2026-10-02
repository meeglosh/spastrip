#pragma once

#include "Controls.h"
#include "Displays.h"
#include "FxPanelHeader.h"
#include "SectionPanel.h"

namespace spa::ui
{

// One generic FX tab: "[toggle] TITLE" header, the effect's live display on
// top, the section's registry controls (knobs / combos / toggles) below.
class FXPanel : public juce::Component
{
public:
    // enableParamIds: the section's on/off toggle param id(s) -- two for a tab
    // covering two effects (TREM/VIB). telemetry: forwarded to the FXDisplay.
    FXPanel (juce::AudioProcessorValueTreeState&, FXDisplay::Kind,
             params::Section, const juce::String& title,
             const juce::StringArray& enableParamIds = {},
             const dsp::Telemetry* telemetry = nullptr);

    void paint (juce::Graphics&) override;
    void resized() override;

    FxPanelHeader* getHeaderForTest() { return header.get(); }

private:
    juce::String panelTitle;
    FXDisplay display;
    SectionPanel controls;
    std::unique_ptr<FxPanelHeader> header;

    // Delay / grain: time vs. division dimming (and ping-pong width).
    std::unique_ptr<DependentEnable> delayTimeEnable, delayDivisionEnable;
    std::unique_ptr<DependentEnable> delayWidthEnable;
    std::unique_ptr<TabEngagementTracker> powerTracker;
};

} // namespace spa::ui
