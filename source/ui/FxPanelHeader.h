#pragma once

#include "Controls.h"
#include "Theme.h"

namespace spa::ui
{

// The ONE title row every FX tab shares: "[toggle] TITLE", toggle first, the
// title in the section font, ALL CAPS, secondary accent (t.accentMod) while the
// effect is on and the display off-grey while it is off. No separate "ON"
// caption anywhere. TREM/VIB is two effects in one tab, so it gets two parts
// ("[toggle] TREM  [toggle] VIB"), each with its own toggle + title.
//
// The header owns the Toggle(s) (paramID property intact, so MIDI Learn works;
// no keyboard focus, via Toggle). Clicking a title toggles its effect too.
// It only ever occupies the top metrics::sectionHeaderHeight band of its
// panel, and only the toggle and title text are hit-testable, so nothing
// underneath (EQ's graph, a display) is covered.
class FxPanelHeader : public juce::Component
{
public:
    struct Part { juce::String title; juce::String paramID; };

    // Geometry shared by every panel (header is placed at the panel's 0,0).
    static constexpr int toggleX = metrics::sectionHeaderLeftInset;
    static constexpr int toggleW = 28;
    static constexpr int toggleH = 20;
    static constexpr int gapAfterToggle = 6;
    static constexpr int gapBetweenParts = 18;

    FxPanelHeader (juce::AudioProcessorValueTreeState& apvts, std::vector<Part> partsIn)
        : apvtsRef (&apvts)
    {
        for (auto& p : partsIn)
        {
            auto e = std::make_unique<Entry>();
            e->title = p.title.toUpperCase();
            e->paramID = p.paramID;
            e->toggle = std::make_unique<Toggle> (apvts, p.paramID, juce::String());
            e->tracker = std::make_unique<TabEngagementTracker> (apvts,
                std::vector<std::pair<juce::String, std::vector<juce::String>>> { { "on", { p.paramID } } },
                *this);
            addAndMakeVisible (*e->toggle);
            entries.push_back (std::move (e));
        }
        setOpaque (false);
    }

    int numParts() const { return (int) entries.size(); }
    Toggle& getToggle (int i) { return *entries[(size_t) i]->toggle; }
    bool isOn (int i) const { return entries[(size_t) i]->tracker->isEngaged ("on"); }
    juce::Rectangle<int> getTitleRect (int i) const { return entries[(size_t) i]->titleRect; }
    juce::String getTitle (int i) const { return entries[(size_t) i]->title; }

    // Where the title row ends horizontally, so a panel can put controls
    // (EQ's character menu) to the right of it without overlap.
    int getContentRight() const
    {
        return entries.empty() ? 0 : entries.back()->titleRect.getRight();
    }

    void resized() override
    {
        auto bandTop = metrics::sectionHeaderTopInset;                 // same line sectionHeader() used
        const int centreY = bandTop + (getHeight() - bandTop) / 2;
        int x = toggleX;
        for (auto& e : entries)
        {
            e->toggle->setBounds (x, centreY - toggleH / 2, toggleW, toggleH);
            x += toggleW + gapAfterToggle;
            const int tw = textWidth (e->title);
            e->titleRect = { x, bandTop, tw, getHeight() - bandTop };
            x += tw + gapBetweenParts;
        }
    }

    void paint (juce::Graphics& g) override
    {
        const auto& t = currentTheme();
        g.setFont (metrics::sectionFont());
        for (auto& e : entries)
        {
            g.setColour (draw::fxTitleColour (t, e->tracker->isEngaged ("on")));
            g.drawText (e->title, e->titleRect, juce::Justification::centredLeft, false);
        }
    }

    bool hitTest (int x, int y) override
    {
        // JUCE's getComponentAt/dispatch asks THIS hitTest before descending
        // into children, so the toggles' bounds must count as hits too or the
        // toggles are unreachable (the 1.0.29 "unclickable toggle" bug).
        for (auto& e : entries)
            if (e->titleRect.contains (x, y) || e->toggle->getBounds().contains (x, y))
                return true;
        return false;
    }

    // A click on a title's text toggles that effect; returns true if one did.
    bool clickTitleAt (juce::Point<int> p)
    {
        for (auto& e : entries)
            if (e->titleRect.contains (p))
            {
                if (auto* prm = apvtsRef->getParameter (e->paramID))
                {
                    prm->beginChangeGesture();
                    prm->setValueNotifyingHost (prm->getValue() > 0.5f ? 0.0f : 1.0f);
                    prm->endChangeGesture();
                }
                return true;
            }
        return false;
    }

    void mouseUp (const juce::MouseEvent& ev) override
    {
        if (! ev.mouseWasDraggedSinceMouseDown())
            clickTitleAt (ev.getPosition());
    }

    void detach() { for (auto& e : entries) e->toggle->detach(); }

private:
    static int textWidth (const juce::String& s)
    {
        juce::GlyphArrangement ga;
        ga.addLineOfText (metrics::sectionFont(), s, 0.0f, 0.0f);
        return (int) std::ceil (ga.getBoundingBox (0, -1, true).getWidth()) + 2;
    }

    juce::AudioProcessorValueTreeState* apvtsRef;

    struct Entry
    {
        juce::String title, paramID;
        std::unique_ptr<Toggle> toggle;
        std::unique_ptr<TabEngagementTracker> tracker;
        juce::Rectangle<int> titleRect;
    };
    std::vector<std::unique_ptr<Entry>> entries;
};

} // namespace spa::ui
