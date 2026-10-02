#pragma once

#include <array>
#include <complex>
#include <functional>

#include "Controls.h"
#include "Displays.h"
#include "FxPanelHeader.h"
#include "Theme.h"
#include "../dsp/FilterResponse.h"
#include "../params/ParameterRegistry.h"

namespace spa::ui
{

// FILTER tab (SPAStrip): the two SVF filters ported from SPASynth's filter
// section. Layout, top to bottom:
//   header   "[toggle] FILTER 1  [toggle] FILTER 2" (two titled toggles, like
//            TREM/VIB) with the ROUTING choice to the right of the titles
//   display  the module's COMBINED frequency response (Series = product,
//            Parallel = average, honouring each MIX), computed exactly from the
//            filter's transfer function (dsp/FilterResponse.h)
//   row 1    type | CUTOFF RES DRIVE MIX      (filter 1)
//   row 2    type | CUTOFF RES DRIVE MIX      (filter 2)
// Every control is a Knob / Choice, so mod-slot reach arcs and the assign menu
// work exactly as on the other tabs.

class FilterDisplay final : public DisplayComponent
{
public:
    // hostSampleRate: the plugin's rate before oversampling (the filter runs at
    // host rate x oversampling factor, which the display reads from the
    // oversampling parameter). May return 0 before the first prepare.
    FilterDisplay (juce::AudioProcessorValueTreeState& state, std::function<double()> hostSampleRate)
        : DisplayComponent (state, watchedIds(), nullptr), hostRate (std::move (hostSampleRate))
    {
    }

    static juce::StringArray watchedIds()
    {
        namespace fx = params::id::fx;
        return { params::id::oversampling, fx::filterEnable, fx::filterRouting,
                 fx::filter1Type, fx::filter1Cutoff, fx::filter1Res, fx::filter1Mix,
                 fx::filter2Enable, fx::filter2Type, fx::filter2Cutoff, fx::filter2Res, fx::filter2Mix };
    }

    // The two filters as the display reads them from the parameters (test hook).
    dsp::filterresponse::Filter readFilter (int which) const
    {
        namespace fx = params::id::fx;
        dsp::filterresponse::Filter f;
        f.enabled = value (which == 0 ? fx::filterEnable : fx::filter2Enable) >= 0.5f;
        f.type = (params::FilterType) juce::jlimit (0, 7, (int) value (which == 0 ? fx::filter1Type : fx::filter2Type));
        f.cutoffHz = value (which == 0 ? fx::filter1Cutoff : fx::filter2Cutoff);
        f.resonance = value (which == 0 ? fx::filter1Res : fx::filter2Res);
        f.mix = value (which == 0 ? fx::filter1Mix : fx::filter2Mix);
        return f;
    }
    bool isParallel() const { return value (params::id::fx::filterRouting) >= 0.5f; }
    double engineRate() const
    {
        const double host = hostRate ? hostRate() : 0.0;
        return (host > 0.0 ? host : 48000.0) * (double) (1 << juce::jlimit (0, 2, (int) value (params::id::oversampling)));
    }

    static constexpr float minDb = -36.0f, maxDb = 18.0f;
    static constexpr float minHz = 20.0f, maxHz = 20000.0f;

private:
    void paintDisplay (juce::Graphics& g, juce::Rectangle<float> area) override
    {
        namespace fr = dsp::filterresponse;
        const auto& t = currentTheme();
        const auto f1 = readFilter (0), f2 = readFilter (1);
        const bool parallel = isParallel();
        const double sr = engineRate();
        const bool anyOn = f1.enabled || f2.enabled;

        const auto xForHz = [&] (float hz)
        {
            return area.getX() + area.getWidth() * std::log (hz / minHz) / std::log (maxHz / minHz);
        };
        // Not clamped to the display's range: a curve that leaves the plot is clipped at its edge
        // (rather than running along the bottom as a flat line) -- only absurd values are limited.
        const auto yForDb = [&] (float db)
        {
            return juce::jmap (juce::jlimit (minDb - 200.0f, maxDb + 100.0f, db), minDb, maxDb, area.getBottom(), area.getY());
        };
        const juce::Graphics::ScopedSaveState clipState (g);
        g.reduceClipRegion (area.toNearestInt());

        // Grid: octave-ish decades and 12 dB steps, 0 dB a touch stronger.
        g.setFont (metrics::smallFont());
        for (const float hz : { 100.0f, 1000.0f, 10000.0f })
        {
            const auto x = xForHz (hz);
            g.setColour (t.outline.withAlpha (0.45f));
            g.drawVerticalLine ((int) x, area.getY(), area.getBottom());
            g.setColour (t.textSecondary.withAlpha (0.55f));
            g.drawText (hz >= 1000.0f ? juce::String ((int) (hz / 1000.0f)) + "k" : juce::String ((int) hz),
                        juce::Rectangle<float> (x + 2.0f, area.getBottom() - 10.0f, 30.0f, 10.0f),
                        juce::Justification::centredLeft);
        }
        for (const float db : { -24.0f, -12.0f, 0.0f, 12.0f })
        {
            const auto y = yForDb (db);
            g.setColour (t.outline.withAlpha (db == 0.0f ? 0.9f : 0.45f));
            g.drawHorizontalLine ((int) y, area.getX(), area.getRight());
            g.setColour (t.textSecondary.withAlpha (0.55f));
            g.drawText ((db > 0.0f ? "+" : "") + juce::String ((int) db) + " dB",
                        juce::Rectangle<float> (area.getX() + 3.0f, y - 10.0f, 44.0f, 10.0f),
                        juce::Justification::centredLeft);
        }

        constexpr int steps = 200;
        const auto buildPath = [&] (const std::function<std::complex<double> (double)>& h)
        {
            juce::Path path;
            for (int i = 0; i <= steps; ++i)
            {
                const double hz = (double) minHz * std::pow ((double) (maxHz / minHz), (double) i / steps);
                const auto x = area.getX() + area.getWidth() * (float) i / steps;
                const auto y = yForDb ((float) fr::toDb (h (hz)));
                if (i == 0) path.startNewSubPath (x, y); else path.lineTo (x, y);
            }
            return path;
        };

        const auto off = t.textSecondary.withAlpha (0.45f);
        const auto second = t.textPrimary;   // filter 2's own line: neutral white against the accent

        if (anyOn)
        {
            // Both running: each filter's own response, thin, under the combined one.
            if (f1.enabled && f2.enabled)
            {
                g.setColour (t.accent.withAlpha (0.55f));
                g.strokePath (buildPath ([&] (double hz) { return fr::blended (f1, sr, hz); }), juce::PathStrokeType (1.0f));
                g.setColour (second.withAlpha (0.45f));
                g.strokePath (buildPath ([&] (double hz) { return fr::blended (f2, sr, hz); }), juce::PathStrokeType (1.0f));
            }
            draw::glowStroke (g, buildPath ([&] (double hz) { return fr::module (f1, f2, parallel, sr, hz); }),
                              t.accent, 1.8f);
        }
        else
        {
            // Everything off: what each filter WOULD do, greyed.
            auto a = f1; a.enabled = true;
            auto b = f2; b.enabled = true;
            g.setColour (off.withAlpha (0.35f));
            g.strokePath (buildPath ([&] (double hz) { return fr::blended (b, sr, hz); }), juce::PathStrokeType (1.0f));
            draw::glowStroke (g, buildPath ([&] (double hz) { return fr::blended (a, sr, hz); }), off, 1.6f);
        }

        // Cutoff markers: a short tick and the filter number along the top edge.
        g.setFont (metrics::smallFontBold());
        for (int i = 0; i < 2; ++i)
        {
            const auto& f = i == 0 ? f1 : f2;
            const bool lit = f.enabled;
            const auto x = xForHz (juce::jlimit (minHz, maxHz, (float) f.cutoffHz));
            const auto colour = (lit ? (i == 0 ? t.accent : second) : off).withAlpha (lit ? 0.9f : 0.5f);
            g.setColour (colour.withAlpha (0.35f));
            g.drawVerticalLine ((int) x, area.getY() + 11.0f, area.getBottom() - 10.0f);
            g.setColour (colour);
            g.drawText (juce::String (i + 1), juce::Rectangle<float> (x - 6.0f, area.getY(), 12.0f, 11.0f),
                        juce::Justification::centred);
        }
    }

    std::function<double()> hostRate;
};

class FilterPanel final : public juce::Component
{
public:
    FilterPanel (juce::AudioProcessorValueTreeState& state, std::function<double()> hostSampleRate)
        : display (state, std::move (hostSampleRate)),
          header (state, { { "Filter 1", params::id::fx::filterEnable },
                           { "Filter 2", params::id::fx::filter2Enable } }),
          routing (state, params::id::fx::filterRouting)
    {
        namespace fx = params::id::fx;
        addAndMakeVisible (display);
        addAndMakeVisible (header);
        addAndMakeVisible (routing);

        const char* typeIds[2] { fx::filter1Type, fx::filter2Type };
        const char* cutoffIds[2] { fx::filter1Cutoff, fx::filter2Cutoff };
        const char* resIds[2] { fx::filter1Res, fx::filter2Res };
        const char* driveIds[2] { fx::filter1Drive, fx::filter2Drive };
        const char* mixIds[2] { fx::filter1Mix, fx::filter2Mix };
        for (int i = 0; i < 2; ++i)
        {
            auto& r = rows[(size_t) i];
            r.type = std::make_unique<Choice> (state, typeIds[i]);
            r.cutoff = std::make_unique<Knob> (state, cutoffIds[i], "Cutoff");
            r.res = std::make_unique<Knob> (state, resIds[i], "Res");
            r.drive = std::make_unique<Knob> (state, driveIds[i], "Drive");
            r.mix = std::make_unique<Knob> (state, mixIds[i], "Mix");
            for (juce::Component* c : { (juce::Component*) r.type.get(), (juce::Component*) r.cutoff.get(),
                                        (juce::Component*) r.res.get(), (juce::Component*) r.drive.get(),
                                        (juce::Component*) r.mix.get() })
                addAndMakeVisible (c);
        }

        routing.combo.setTooltip ("Series: filter 2 filters the output of filter 1.  "
                                  "Parallel: both filters hear the input and their outputs are averaged.");
        for (auto& r : rows)
        {
            r.type->combo.setTooltip ("Filter mode and slope: LP / HP / BP / Notch, 12 or 24 dB per octave.");
            r.cutoff->slider.setTooltip ("Cutoff or centre frequency.");
            r.res->slider.setTooltip ("Resonance: emphasis at the cutoff.");
            r.drive->slider.setTooltip ("Saturation in front of the filter.");
            r.mix->slider.setTooltip ("Dry / filtered balance of this filter.");
        }

        powerTracker = std::make_unique<TabEngagementTracker> (state,
            std::vector<std::pair<juce::String, std::vector<juce::String>>> {
                { "on", { fx::filterEnable, fx::filter2Enable } } }, *this);
    }

    void paint (juce::Graphics& g) override
    {
        const auto& t = currentTheme();
        g.setFont (metrics::smallFont());
        g.setColour (t.textSecondary);
        g.drawText ("ROUTING", routingCaptionRect, juce::Justification::centredRight);
        g.setFont (metrics::smallFontBold());
        for (int i = 0; i < 2; ++i)
            g.drawText ("FILTER " + juce::String (i + 1), rows[(size_t) i].captionRect, juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto headerArea = getLocalBounds().removeFromTop (metrics::sectionHeaderHeight);
        header.setBounds (headerArea);
        {
            // ROUTING sits right of the two titles, vertically centred in the header band.
            const int bandTop = metrics::sectionHeaderTopInset;
            const int cy = bandTop + (headerArea.getHeight() - bandTop) / 2;
            const int x = header.getContentRight() + 28;
            routingCaptionRect = { x, cy - 8, 52, 16 };
            routing.setBounds (x + 58, cy - 11, 104, 22);
        }

        auto area = getLocalBounds().withTrimmedTop (metrics::sectionHeaderHeight).reduced (7, 3);
        constexpr int rowHeight = 88;
        auto bottom = area.removeFromBottom (rowHeight * 2);
        area.removeFromBottom (4);
        display.setBounds (area);

        constexpr int typeColumn = 150;
        for (int i = 0; i < 2; ++i)
        {
            auto& r = rows[(size_t) i];
            auto row = bottom.removeFromTop (rowHeight);
            auto left = row.removeFromLeft (typeColumn);
            r.captionRect = juce::Rectangle<int> (left.getX() + 6, row.getY() + rowHeight / 2 - 28, typeColumn - 12, 12);
            r.type->setBounds (left.getX() + 4, row.getY() + rowHeight / 2 - 12, typeColumn - 14, 24);
            const int cell = row.getWidth() / 4;
            r.cutoff->setBounds (row.removeFromLeft (cell));
            r.res->setBounds (row.removeFromLeft (cell));
            r.drive->setBounds (row.removeFromLeft (cell));
            r.mix->setBounds (row);
        }
    }

    // Test hooks.
    FilterDisplay& getDisplay() { return display; }
    FxPanelHeader& getHeader() { return header; }
    Choice& getRoutingChoice() { return routing; }
    Knob* getKnob (int filter, const juce::String& which)
    {
        auto& r = rows[(size_t) filter];
        return which == "cutoff" ? r.cutoff.get() : which == "res" ? r.res.get()
             : which == "drive" ? r.drive.get() : which == "mix" ? r.mix.get() : nullptr;
    }
    Choice* getTypeChoice (int filter) { return rows[(size_t) filter].type.get(); }

private:
    struct Row
    {
        std::unique_ptr<Choice> type;
        std::unique_ptr<Knob> cutoff, res, drive, mix;
        juce::Rectangle<int> captionRect;
    };

    FilterDisplay display;
    FxPanelHeader header;
    Choice routing;
    std::array<Row, 2> rows;
    juce::Rectangle<int> routingCaptionRect;
    std::unique_ptr<TabEngagementTracker> powerTracker;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FilterPanel)
};

} // namespace spa::ui
