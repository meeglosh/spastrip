#pragma once

#include "FxPanelHeader.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include "Theme.h"
#include "Controls.h"
#include "../dsp/Telemetry.h"
#include "../params/ParameterRegistry.h"
#include <array>
#include <cmath>

namespace spa::ui
{

// COMP tab: the crossover graph and band editor ported from SPAGlitch's
// MultibandEditor, restyled to SPASynth's Theme tokens (graph and handles in
// the secondary accent while the module is on, plain off-grey when it is not;
// knobs and the header follow the same rules as every other FX tab).
//
// The graph is a log frequency axis with the two crossover points as
// draggable vertical handles, the three bands tinted between them, and each
// band's live gain (signed: up is lift, down is reduction) filled from the
// centre line. Clicking a band selects it. Only the selected band's six
// controls are shown, since a compressor is edited one band at a time and
// every band's activity stays visible in the graph meanwhile; all eighteen
// knobs are built up front and hidden rather than rebuilt on selection, so no
// parameter attachment is torn down while the audio thread reads it.
class CompCrossoverGraph final : public juce::Component,
                                 private juce::Timer
{
public:
    CompCrossoverGraph (juce::AudioProcessorValueTreeState& state, const dsp::Telemetry& tel)
        : apvts (state), telemetry (tel)
    {
        // A click target inside an editor that also hosts the on-screen
        // keyboard: taking focus here would steal its QWERTY notes.
        setWantsKeyboardFocus (false);
        startTimerHz (30);
    }

    ~CompCrossoverGraph() override { stopTimer(); }

    void setSelectedBand (int band)
    {
        if (selected == band) return;
        selected = band;
        repaint();
    }
    std::function<void (int)> onBandSelected;

    // Exposed for tests: the x of a crossover handle in this component's space.
    float crossoverX (int which) const { return xForHz (crossover (which)); }

    void paint (juce::Graphics& g) override
    {
        const auto& t = currentTheme();
        const auto on = parameterValue (params::id::fx::compEnable) >= 0.5f;
        const auto live = on ? t.accentMod : t.textSecondary.withAlpha (0.45f);
        const auto bounds = getLocalBounds().toFloat();

        draw::displayWell (g, bounds, false);

        const auto area = plotArea();
        g.setFont (metrics::smallFont());

        // Decade gridlines, so a dragged crossover can be read against something.
        for (const float hz : { 100.0f, 1000.0f, 10000.0f })
        {
            const auto x = xForHz (hz);
            g.setColour (t.outline.withAlpha (0.45f));
            g.drawVerticalLine ((int) x, area.getY(), area.getBottom());
            g.setColour (t.textSecondary.withAlpha (0.55f));
            g.drawText (hz >= 1000.0f ? juce::String (hz / 1000.0f, 0) + "k" : juce::String ((int) hz),
                        juce::Rectangle<float> (x + 2.0f, area.getBottom(), 30.0f, 10.0f),
                        juce::Justification::centredLeft);
        }

        const auto centreY = area.getCentreY();
        const auto halfHeight = area.getHeight() * 0.5f;

        // The vertical axis is gain applied, not level: above the line a band
        // is being lifted, below it held down.
        {
            const auto offset = halfHeight * 12.0f / meterRangeDb;
            g.setColour (t.outline.withAlpha (0.5f));
            g.drawHorizontalLine ((int) (centreY - offset), area.getX(), area.getRight());
            g.drawHorizontalLine ((int) (centreY + offset), area.getX(), area.getRight());
            g.setColour (t.textSecondary.withAlpha (0.55f));
            g.drawText ("+12 dB", juce::Rectangle<float> (area.getX() + 3.0f, centreY - offset - 9.0f, 40.0f, 9.0f),
                        juce::Justification::centredLeft);
            g.drawText ("-12 dB", juce::Rectangle<float> (area.getX() + 3.0f, centreY + offset, 40.0f, 9.0f),
                        juce::Justification::centredLeft);
        }

        const float edges[4] { area.getX(), xForHz (crossover (0)), xForHz (crossover (1)), area.getRight() };
        const juce::String names[3] { "LOW", "MID", "HIGH" };

        for (int b = 0; b < 3; ++b)
        {
            const juce::Rectangle<float> region (edges[b], area.getY(),
                                                 juce::jmax (1.0f, edges[b + 1] - edges[b]),
                                                 area.getHeight());
            g.setColour (live.withAlpha (b == selected ? 0.10f : 0.04f));
            g.fillRect (region);

            // Both directions matter, so a one-sided meter would hide half of
            // what the band is doing.
            const auto db = juce::jlimit (-meterRangeDb, meterRangeDb, shown[(size_t) b]);
            const auto extent = halfHeight * std::abs (db) / meterRangeDb;
            if (on && extent > 0.5f)
            {
                const auto fill = db >= 0.0f
                    ? juce::Rectangle<float> (region.getX(), centreY - extent, region.getWidth(), extent)
                    : juce::Rectangle<float> (region.getX(), centreY, region.getWidth(), extent);
                g.setColour ((db >= 0.0f ? t.accent : t.accentMod).withAlpha (0.42f));
                g.fillRect (fill.reduced (1.0f, 0.0f));
            }

            g.setColour (t.textSecondary.withAlpha (b == selected ? 0.95f : 0.55f));
            g.setFont (metrics::smallFontBold());
            g.drawText (names[b], region.withHeight (10.0f).translated (0.0f, 2.0f),
                        juce::Justification::centred);
        }

        g.setColour (t.outline);
        g.drawHorizontalLine ((int) centreY, area.getX(), area.getRight());

        // The crossover handles last, so they sit over the band fills.
        for (int i = 0; i < 2; ++i)
        {
            const auto x = xForHz (crossover (i));
            const auto active = dragging == i || hovered == i;
            g.setColour (active ? t.textPrimary : live.withAlpha (on ? 0.8f : 1.0f));
            g.drawVerticalLine ((int) x, area.getY(), area.getBottom());
            g.fillRoundedRectangle (juce::Rectangle<float> (x - 3.0f, area.getY() - 1.0f, 6.0f, 9.0f), 2.0f);

            g.setColour (active ? t.textPrimary : t.textSecondary.withAlpha (0.8f));
            g.setFont (metrics::smallFont());
            const auto width = 50.0f;
            const auto textX = juce::jlimit (area.getX(), area.getRight() - width, x - width * 0.5f);
            g.drawText (frequencyText (crossover (i)),
                        juce::Rectangle<float> (textX, bounds.getY() + 1.0f, width, 10.0f),
                        juce::Justification::centred);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragging = handleNear ((float) e.x);
        if (dragging >= 0)
        {
            if (auto* p = apvts.getParameter (idFor (dragging)))
                p->beginChangeGesture();
            return;
        }
        const auto band = bandAt ((float) e.x);
        setSelectedBand (band);
        if (onBandSelected) onBandSelected (band);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging < 0) return;
        setCrossover (dragging, hzForX ((float) e.x));
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragging < 0) return;
        if (auto* p = apvts.getParameter (idFor (dragging)))
            p->endChangeGesture();
        dragging = -1;
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const auto near = handleNear ((float) e.x);
        if (near == hovered) return;
        hovered = near;
        setMouseCursor (near >= 0 ? juce::MouseCursor::LeftRightResizeCursor
                                  : juce::MouseCursor::PointingHandCursor);
        repaint();
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        if (hovered < 0) return;
        hovered = -1;
        repaint();
    }

private:
    static constexpr float minHz = 20.0f, maxHz = 20000.0f, meterRangeDb = 18.0f;
    static constexpr float grabRadius = 9.0f;   // how close the pointer must get to grab a crossover

    static const char* idFor (int which)
    {
        return which == 0 ? params::id::fx::compXoverLow : params::id::fx::compXoverHigh;
    }

    float parameterValue (const juce::String& id) const
    {
        auto* p = apvts.getParameter (id);
        return p != nullptr ? p->convertFrom0to1 (p->getValue()) : 0.0f;
    }

    static juce::String frequencyText (float hz)
    {
        return hz >= 1000.0f ? juce::String (hz / 1000.0f, hz >= 10000.0f ? 0 : 1) + " kHz"
                             : juce::String (juce::roundToInt (hz)) + " Hz";
    }

    void timerCallback() override
    {
        if (! isLiveShowing (*this))
            return;
        const auto on = parameterValue (params::id::fx::compEnable) >= 0.5f;
        for (int b = 0; b < 3; ++b)
        {
            const auto target = on ? telemetry.compBandDb[(size_t) b].load (std::memory_order_relaxed) : 0.0f;
            // Snap toward a bigger excursion and ease back from it, so the
            // fill reads as movement rather than flicker at 30 fps.
            auto& value = shown[(size_t) b];
            value = std::abs (target) > std::abs (value) ? target : value * 0.84f;
        }
        if (isLiveShowing (*this))
            repaint();
    }

    juce::Rectangle<float> plotArea() const
    {
        return getLocalBounds().toFloat().reduced (2.0f).withTrimmedTop (12.0f).withTrimmedBottom (10.0f);
    }

    float xForHz (float hz) const
    {
        const auto r = plotArea();
        const auto t = std::log (juce::jlimit (minHz, maxHz, hz) / minHz) / std::log (maxHz / minHz);
        return r.getX() + r.getWidth() * t;
    }

    float hzForX (float x) const
    {
        const auto r = plotArea();
        const auto t = juce::jlimit (0.0f, 1.0f, (x - r.getX()) / juce::jmax (1.0f, r.getWidth()));
        return minHz * std::pow (maxHz / minHz, t);
    }

    float crossover (int which) const { return parameterValue (idFor (which)); }

    void setCrossover (int which, float hz)
    {
        // The two must not cross, or the mid band inverts; a little more than
        // an octave of separation also keeps the middle band worth having.
        constexpr float separation = 1.25f;
        if (which == 0) hz = juce::jmin (hz, crossover (1) / separation);
        else            hz = juce::jmax (hz, crossover (0) * separation);
        if (auto* p = apvts.getParameter (idFor (which)))
            p->setValueNotifyingHost (p->convertTo0to1 (hz));
    }

    int handleNear (float x) const
    {
        for (int i = 0; i < 2; ++i)
            if (std::abs (x - xForHz (crossover (i))) <= grabRadius)
                return i;
        return -1;
    }

    int bandAt (float x) const
    {
        if (x < xForHz (crossover (0))) return 0;
        if (x < xForHz (crossover (1))) return 1;
        return 2;
    }

    juce::AudioProcessorValueTreeState& apvts;
    const dsp::Telemetry& telemetry;
    std::array<float, 3> shown {};
    int selected = 0;
    int dragging = -1;
    int hovered = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CompCrossoverGraph)
};

class CompPanel final : public juce::Component
{
public:
    CompPanel (juce::AudioProcessorValueTreeState& state, const dsp::Telemetry& tel)
        : graph (state, tel),
          header (state, { { "Multi-band compressor", params::id::fx::compEnable } }),
          mix (state, params::id::fx::compMix, "Mix")
    {
        addAndMakeVisible (graph);
        addAndMakeVisible (header);
        addAndMakeVisible (mix);
        graph.onBandSelected = [this] (int band) { selectBand (band); };

        static const char* keys[6] { params::id::fx::compband::threshold, params::id::fx::compband::ratio,
                                     params::id::fx::compband::upRatio,   params::id::fx::compband::attack,
                                     params::id::fx::compband::release,   params::id::fx::compband::gain };
        static const char* captions[6] { "Thresh", "Ratio", "Up Ratio", "Attack", "Release", "Gain" };
        static const char* bandNames[3] { "LOW", "MID", "HIGH" };

        for (int b = 0; b < 3; ++b)
        {
            auto button = std::make_unique<BandButton> (bandNames[b]);
            button->onClick = [this, b] { selectBand (b); };
            addAndMakeVisible (*button);
            bandButtons[(size_t) b] = std::move (button);

            for (int k = 0; k < 6; ++k)
            {
                auto knob = std::make_unique<Knob> (state, params::id::compBand (b, keys[k]), captions[k]);
                addChildComponent (*knob);
                knobs[(size_t) b][(size_t) k] = std::move (knob);
            }
        }

        powerTracker = std::make_unique<TabEngagementTracker> (state,
            std::vector<std::pair<juce::String, std::vector<juce::String>>> {
                { "on", { params::id::fx::compEnable } } }, *this);
        selectBand (0);
    }

    void paint (juce::Graphics& g) override
    {
        // Title row ("[toggle] COMPRESSOR") is the shared FxPanelHeader child.
        juce::ignoreUnused (g);
    }

    void resized() override
    {
        header.setBounds (getLocalBounds().removeFromTop (metrics::sectionHeaderHeight));
        auto area = getLocalBounds().withTrimmedTop (metrics::sectionHeaderHeight).reduced (7, 3);

        // The graph takes the larger share (it is the part you aim at); the
        // controls only ever show one band's worth.
        const auto controlsWidth = juce::jlimit (260, 440, juce::roundToInt ((float) area.getWidth() * 0.62f));
        auto controls = area.removeFromRight (controlsWidth);
        area.removeFromRight (8);
        graph.setBounds (area);

        // Selector + one band's knob row, vertically centred in the column (the knobs stay a
        // sensible size however tall the panel is).
        const int knobRowHeight = 92;
        controls = controls.withSizeKeepingCentre (controls.getWidth(), juce::jmin (controls.getHeight(), 22 + knobRowHeight));
        auto selector = controls.removeFromTop (20);
        const auto buttonWidth = juce::jmin (62, selector.getWidth() / 3);
        for (int b = 0; b < 3; ++b)
            bandButtons[(size_t) b]->setBounds (selector.removeFromLeft (buttonWidth).reduced (2, 0));

        controls.removeFromTop (2);

        // Six band knobs and MIX in one row: seven equal cells.
        const auto cellWidth = controls.getWidth() / 7;
        for (int k = 0; k < 6; ++k)
            if (auto& knob = knobs[(size_t) selected][(size_t) k])
                knob->setBounds (controls.removeFromLeft (cellWidth));
        mix.setBounds (controls);
    }

    // Test hooks.
    CompCrossoverGraph& getGraph() { return graph; }
    Knob* getBandKnob (int band, int index) { return knobs[(size_t) band][(size_t) index].get(); }
    Knob& getMixKnob() { return mix; }
    juce::Component* getBandButton (int band) { return bandButtons[(size_t) band].get(); }
    int getSelectedBand() const { return selected; }

private:
    // The pill a band is selected with, painted like the chain's own tabs so
    // the two read as the same kind of control.
    class BandButton final : public juce::Button
    {
    public:
        explicit BandButton (const juce::String& name) : juce::Button (name)
        {
            setClickingTogglesState (false);
            setWantsKeyboardFocus (false);
        }

        void setSelected (bool shouldBeSelected)
        {
            if (selected == shouldBeSelected) return;
            selected = shouldBeSelected;
            repaint();
        }

    private:
        void paintButton (juce::Graphics& g, bool over, bool down) override
        {
            const auto& t = currentTheme();
            auto bounds = getLocalBounds().toFloat().reduced (1.0f);
            if (selected)
            {
                g.setColour (t.accent);
                g.fillRect (bounds.removeFromBottom (2.0f).reduced (4.0f, 0.0f));
            }
            else if (over || down)
            {
                g.setColour (t.seam.withAlpha (0.6f));
                g.fillRoundedRectangle (bounds, 2.0f);
            }
            g.setColour (selected ? t.textPrimary : t.textSecondary);
            g.setFont (selected ? metrics::smallFontBold() : metrics::smallFont());
            g.drawText (getButtonText(), getLocalBounds(), juce::Justification::centred);
        }

        bool selected = false;
    };

    void selectBand (int band)
    {
        selected = juce::jlimit (0, 2, band);
        graph.setSelectedBand (selected);
        for (int b = 0; b < 3; ++b)
        {
            if (auto* button = dynamic_cast<BandButton*> (bandButtons[(size_t) b].get()))
                button->setSelected (b == selected);
            for (auto& knob : knobs[(size_t) b])
                if (knob != nullptr)
                    knob->setVisible (b == selected);
        }
        resized();
    }

    CompCrossoverGraph graph;
    FxPanelHeader header;
    Knob mix;
    std::array<std::unique_ptr<juce::Button>, 3> bandButtons;
    std::array<std::array<std::unique_ptr<Knob>, 6>, 3> knobs;
    std::unique_ptr<TabEngagementTracker> powerTracker;
    int selected = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CompPanel)
};

} // namespace spa::ui
