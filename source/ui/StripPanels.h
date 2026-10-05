#pragma once

#include "../SPAStripProcessor.h"
#include "Controls.h"

namespace spa::ui
{

//==============================================================================
// Mod target popup: grouped by effect (a submenu per effect), built from
// SPAStripProcessor::getModTargets(). Item id 1 = "None", target i = i + 2.
namespace modmenu
{
    juce::String groupName (const mod::ModTarget&);          // "FX Chorus" -> "Chorus"
    juce::String shortName (const juce::String& parameterID); // "Chorus Rate"; "" for ""/unknown
    juce::PopupMenu build (const juce::String& currentTargetID);
    // Menu result -> parameter ID ("" for None). `valid` false for a dismissed menu.
    juce::String resultToTarget (int result, bool& valid);
}

//==============================================================================
// Horizontal stereo peak meter (green / yellow / red zones on a -60..0 dB
// scale, a peak-hold tick, a latched clip cap -- click to reset), fed by the
// callback. 30 Hz repaint only when something moved and only while showing.
class LevelMeter : public juce::Component, private juce::Timer
{
public:
    using PeakFn = std::function<void (float& left, float& right)>;
    LevelMeter (juce::String caption, PeakFn);
    ~LevelMeter() override;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void setShowScale (bool shouldShow) { showScale = shouldShow; repaint(); }
    // Test hook: levels as displayed (linear, after ballistics).
    float getDisplayedForTest (int channel) const { return level[(size_t) channel]; }
    void tickForTest() { timerCallback(); }

private:
    void timerCallback() override;
    static float dbToFraction (float db) { return juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f); }

    juce::String caption;
    PeakFn peaks;
    std::array<float, 2> level {}, hold {}, shownLevel {}, shownHold {};
    std::array<int, 2> holdAge {};
    std::array<bool, 2> clipped {};
    bool showScale = false;
};

//==============================================================================
// Scrolling history of the sidechain detector's envelope (about 4 s) with the
// live value as a bar, and the "no sidechain input routed" notice.
class EnvelopeDisplay : public juce::Component, private juce::Timer
{
public:
    explicit EnvelopeDisplay (SPAStripProcessor&);
    ~EnvelopeDisplay() override;
    void paint (juce::Graphics&) override;

    // The routing condition the notice is shown for (also a test hook).
    bool shouldShowNoSidechainNotice() const;
    float getEnvelopeForTest() const { return shownEnv; }
    void tickForTest() { timerCallback(); }

private:
    void timerCallback() override;
    SPAStripProcessor& processor;
    static constexpr int historySize = 120;
    std::array<float, historySize> history {};
    int head = 0;
    float shownEnv = 0.0f;
};

//==============================================================================
class SidechainPanel : public juce::Component
{
public:
    explicit SidechainPanel (SPAStripProcessor&);
    void paint (juce::Graphics&) override;
    void resized() override;
    EnvelopeDisplay& getEnvelopeDisplay() { return envelope; }

private:
    Choice source;
    Toggle listen;
    Knob gain, attack, release, hpf;
    EnvelopeDisplay envelope;
};

//==============================================================================
// Eight mod slots: a target picker (grouped popup, "None" first) and a bipolar
// depth knob per slot, with a live activity bar fed by Telemetry::modSlotOffset.
class ModMatrixPanel : public juce::Component, private juce::Timer
{
public:
    explicit ModMatrixPanel (SPAStripProcessor&);
    ~ModMatrixPanel() override;
    void paint (juce::Graphics&) override;
    void resized() override;

    // Re-reads the slot targets (they are plugin state, not parameters).
    void refreshTargets();
    // Opens slot `slot`'s picker (tests drive chooseTarget instead of the modal menu).
    void showPicker (int slot);
    void chooseTarget (int slot, const juce::String& parameterID);
    PickerButton& getPickerForTest (int slot) { return cells[(size_t) slot]->picker; }
    juce::Button& getLockButtonForTest() { return lockButton; }
    Knob& getDepthKnobForTest (int slot) { return cells[(size_t) slot]->depth; }
    float getActivityForTest (int slot) const { return cells[(size_t) slot]->shownActivity; }

private:
    void timerCallback() override;

    struct ActivityBar : juce::Component, juce::SettableTooltipClient
    {
        float value = 0.0f;   // -1..1
        void paint (juce::Graphics&) override;
    };
    struct Cell : juce::Component
    {
        Cell (SPAStripProcessor&, int slot);
        void paint (juce::Graphics&) override;
        void resized() override;
        int slotIndex;
        PickerButton picker;
        Knob depth;
        ActivityBar activity;
        float shownActivity = 0.0f;
        juce::String shownTarget { "?" };
    };

    // The MODULATION lock (Randomize All keeps the slots), drawn like the effect
    // tabs' padlocks, just after the panel title.
    struct LockButton : juce::Button
    {
        explicit LockButton (SPAStripProcessor& p) : juce::Button ("Lock modulation"), proc (p)
        {
            setWantsKeyboardFocus (false);
            setTooltip ("Lock modulation: RANDOMIZE ALL keeps every slot's target and depth.");
        }
        void paintButton (juce::Graphics&, bool over, bool down) override;
        SPAStripProcessor& proc;
    };

    SPAStripProcessor& processor;
    std::array<std::unique_ptr<Cell>, SPAStripProcessor::numModSlots> cells;
    LockButton lockButton { processor };
};

//==============================================================================
// Input / output level meters + input gain, output gain and the global MIX.
class IoPanel : public juce::Component
{
public:
    explicit IoPanel (SPAStripProcessor&);
    void paint (juce::Graphics&) override;
    void resized() override;
    LevelMeter& getInputMeter() { return inMeter; }
    LevelMeter& getOutputMeter() { return outMeter; }

private:
    LevelMeter inMeter, outMeter;
    Knob inGain, outGain, mix;
};

} // namespace spa::ui
