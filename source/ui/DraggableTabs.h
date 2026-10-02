#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

namespace spa::ui
{

// Padlock glyph (SPAStripLookAndFeel.cpp): closed + filled when `locked`,
// open shackle when not.
void drawPadlockGlyph (juce::Graphics&, juce::Rectangle<float>, juce::Colour, bool locked);

class DraggableTabs;

// A tab button with a grip-dots handle on the left (drag to reorder the bar)
// and a small padlock toggle on the right (the effect's Randomize All lock).
// The actual move is delegated to the owning DraggableTabs so both the button
// AND its content component move together (the bar's own moveTab would reorder
// only the buttons, desyncing them from the content array).
class DraggableTabButton : public juce::TabBarButton
{
public:
    static constexpr int gripReserve = 16;   // left: grip dots
    static constexpr int lockReserve = 20;   // right: padlock

    DraggableTabButton (const juce::String& name, juce::TabbedButtonBar& bar,
                        std::function<void (int from, int to)> mover)
        : juce::TabBarButton (name, bar), onMove (std::move (mover))
    {
        setMouseClickGrabsKeyboardFocus (false);
    }

    juce::Rectangle<int> getLockRect() const
    {
        return getLocalBounds().removeFromRight (lockReserve).reduced (2, 0);
    }

    bool isLockedNow() const;                 // asks the owning DraggableTabs
    bool lockHovered() const { return lockHover; }

    void mouseDown (const juce::MouseEvent& e) override
    {
        pressedOnLock = getLockRect().contains (e.getPosition());
        if (! pressedOnLock)
            juce::TabBarButton::mouseDown (e);
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        setLockHover (getLockRect().contains (e.getPosition()));
        juce::TabBarButton::mouseMove (e);
    }

    void mouseExit (const juce::MouseEvent& e) override
    {
        setLockHover (false);
        juce::TabBarButton::mouseExit (e);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (pressedOnLock)
        {
            pressedOnLock = false;
            if (getLockRect().contains (e.getPosition()))
                toggleLock();
            return;
        }
        juce::TabBarButton::mouseUp (e);
        notifyDragEnd();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (pressedOnLock)
            return;   // a press that started on the padlock never reorders

        auto& bar = getTabbedButtonBar();
        const int idx = getIndex();
        const auto px = bar.getLocalPoint (this, e.position).x;

        int target = idx;   // the tab whose slot the pointer is over
        for (int i = 0; i < bar.getNumTabs(); ++i)
            if (auto* b = bar.getTabButton (i))
                if (px >= (float) b->getX() && px < (float) b->getRight()) { target = i; break; }

        if (target != idx && target >= 0 && onMove)
            onMove (idx, target);
    }

    void toggleLock();                        // asks the owning DraggableTabs
    void notifyDragEnd();                     // asks the owning DraggableTabs

    void paintButton (juce::Graphics& g, bool over, bool down) override;

private:
    void setLockHover (bool h)
    {
        if (h != lockHover)
        {
            lockHover = h;
            repaint();
        }
    }

    std::function<void (int from, int to)> onMove;
    bool pressedOnLock = false;
    bool lockHover = false;
};

// TabbedComponent whose tabs can be dragged to reorder. Tabs are identified by
// name; setModuleNames() maps each name (by index) to a stable module id so the
// current order and a saved order can be read/applied as module ids.
class DraggableTabs : public juce::TabbedComponent
{
public:
    DraggableTabs() : juce::TabbedComponent (juce::TabbedButtonBar::TabsAtTop) {}

    std::function<void()> onOrderChanged;   // fires after each swap of a drag reorder
    std::function<void()> onDragEnd;        // fires when the mouse is released on a tab

    // "Is this tab's underlying effect engaged" hook, queried by name (tabs
    // are drag-reorderable, so index isn't stable); the LookAndFeel bolds the
    // label.
    std::function<bool (const juce::String& tabName)> isTabEngaged;
    // Randomize All lock state of a tab's effect, and the padlock click.
    std::function<bool (const juce::String& tabName)> isTabLocked;
    std::function<void (const juce::String& tabName)> onLockClicked;

    void setModuleNames (juce::StringArray namesByModuleId)
    {
        moduleNames = std::move (namesByModuleId);
    }
    const juce::StringArray& getModuleNames() const { return moduleNames; }

    juce::Array<int> currentOrder() const
    {
        juce::Array<int> ids;
        for (const auto& n : getTabNames())
            ids.add (moduleNames.indexOf (n));
        return ids;
    }

    // Reorders the tabs to `ids` (module ids). Idempotent; ids outside the
    // module list are ignored.
    void applyOrder (const juce::Array<int>& ids)
    {
        for (int pos = 0; pos < ids.size(); ++pos)
        {
            if (! juce::isPositiveAndBelow (ids[pos], moduleNames.size()))
                continue;
            const auto name = moduleNames[ids[pos]];
            const int cur = getTabNames().indexOf (name);
            if (cur >= 0 && cur != pos)
                moveTab (cur, pos, false);   // TabbedComponent::moveTab: button + content
        }
    }

    juce::TabBarButton* createTabButton (const juce::String& name, int /*index*/) override
    {
        return new DraggableTabButton (name, getTabbedButtonBar(),
                                       [this] (int from, int to)
                                       {
                                           moveTab (from, to, true);   // button + content
                                           if (onOrderChanged) onOrderChanged();
                                       });
    }

    void repaintTabs()
    {
        getTabbedButtonBar().repaint();
        for (int i = 0; i < getTabbedButtonBar().getNumTabs(); ++i)
            if (auto* b = getTabbedButtonBar().getTabButton (i))
                b->repaint();
    }

private:
    juce::StringArray moduleNames;   // index = module id
};

inline bool DraggableTabButton::isLockedNow() const
{
    if (auto* tabs = dynamic_cast<DraggableTabs*> (getTabbedButtonBar().getParentComponent()))
        if (tabs->isTabLocked)
            return tabs->isTabLocked (getButtonText());
    return false;
}

inline void DraggableTabButton::notifyDragEnd()
{
    if (auto* tabs = dynamic_cast<DraggableTabs*> (getTabbedButtonBar().getParentComponent()))
        if (tabs->onDragEnd)
            tabs->onDragEnd();
}

inline void DraggableTabButton::toggleLock()
{
    if (auto* tabs = dynamic_cast<DraggableTabs*> (getTabbedButtonBar().getParentComponent()))
        if (tabs->onLockClicked)
            tabs->onLockClicked (getButtonText());
    repaint();
}

} // namespace spa::ui
