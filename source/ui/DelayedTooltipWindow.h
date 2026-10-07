#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

namespace spa::ui
{

// JUCE's TooltipWindow::timerCallback shows a new tip IMMEDIATELY when one is
// visible or "has just disappeared" (`isVisible() || now < lastHideTime + 500`),
// so sweeping the pointer from one control to the next skips the hover delay.
// Those members are private, so instead of patching the window we withhold the
// tip itself: TooltipDelayGate answers "has the pointer rested on THIS
// component for the full delay yet?", and DelayedTooltipWindow::getTipFor
// returns an empty tip until it has. An empty tip hides the previous one at
// once (the base class hides when the tip under the mouse is empty), and when
// the real tip finally arrives it may appear immediately because our gate has
// already enforced the wait. The base class's own delay is set to 0 so the
// wait is not paid twice.
class TooltipDelayGate
{
public:
    explicit TooltipDelayGate (int delayMs = 700, int maxQueryGapMs = 350)
        : delay (delayMs), maxGap (maxQueryGapMs) {}

    // Call once per tooltip poll with the component under the pointer.
    // True when its tip may be shown.
    bool allow (const void* target, juce::uint32 nowMs)
    {
        // The window polls every ~120 ms while the pointer is over anything;
        // a longer silence means the pointer left (or was over nothing), so
        // coming back to the same control starts a fresh wait.
        if (target != current || nowMs - lastQuery > (juce::uint32) maxGap)
        {
            current = target;
            since = nowMs;
        }
        lastQuery = nowMs;
        return nowMs - since >= (juce::uint32) delay;
    }

    void reset() { current = nullptr; }

private:
    int delay, maxGap;
    const void* current = nullptr;
    juce::uint32 since = 0, lastQuery = 0;
};

class DelayedTooltipWindow : public juce::TooltipWindow
{
public:
    explicit DelayedTooltipWindow (juce::Component* parent, int delayMs = 700)
        : juce::TooltipWindow (parent, 0), gate (delayMs) {}

    juce::String getTipFor (juce::Component& c) override
    {
        // A press dismisses the tip; make the next one wait afresh.
        if (juce::ModifierKeys::getCurrentModifiers().isAnyMouseButtonDown())
            gate.reset();

        const auto tip = baseTipForTest ? baseTipForTest (c) : juce::TooltipWindow::getTipFor (c);
        const auto now = clock ? clock() : juce::Time::getApproximateMillisecondCounter();
        return gate.allow (&c, now) ? tip : juce::String();
    }

    // Test hooks: deterministic clock, and a stand-in for the base lookup
    // (which returns nothing unless the process is the foreground app).
    std::function<juce::uint32()> clock;
    std::function<juce::String (juce::Component&)> baseTipForTest;

private:
    TooltipDelayGate gate;
};

} // namespace spa::ui
