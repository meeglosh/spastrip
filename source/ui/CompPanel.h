#pragma once

#include "FxPanelHeader.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_dsp/juce_dsp.h>
#include "Theme.h"
#include "Controls.h"
#include "../dsp/Multiband.h"
#include "../dsp/Telemetry.h"
#include "../params/ParameterRegistry.h"
#include <array>
#include <cmath>

namespace spa::ui
{

// COMP tab, rebuilt after Phil's feedback (Pro-MB / C6 / Quad Comp as the
// reference for how a modern multiband compressor should feel):
//
//   * CompBandGraph: a live spectrum (input filled, output traced) behind the
//     three bands, on a level axis shared with the thresholds. Each band is a
//     block whose top edge is its threshold: drag a band up / down to set the
//     threshold, drag a crossover line sideways, wheel for ratio, double-click
//     to reset the threshold. Gain change is drawn from the threshold line on
//     the same dB scale (reduction below in the mod colour, upward lift above
//     in the accent), with a tick at the band's live detector level.
//   * CompTransferCurve: input vs output for the selected band, drawn with
//     Multiband::staticGainDb (the curve the audio runs, knee and upward half
//     included) and a dot riding it at the band's live level.
//   * CompMeters: the module's own IN and OUT levels and the selected band's
//     gain change.
//   * One row of controls for the selected band (standard names: Threshold,
//     Ratio, Upward Ratio, Knee, Attack, Release, Makeup, plus Solo / Bypass),
//     then the module-wide crossovers and Mix.
//
// Every per-band control is built up front and hidden rather than rebuilt on
// selection, so no parameter attachment is torn down while the audio thread
// reads it.

namespace comp
{
    inline constexpr int numBands = 3;
    inline const char* const bandNames[numBands] { "LOW", "MID", "HIGH" };

    inline float value (juce::AudioProcessorValueTreeState& apvts, const juce::String& id)
    {
        auto* p = apvts.getParameter (id);
        return p != nullptr ? p->convertFrom0to1 (p->getValue()) : 0.0f;
    }

    inline void set (juce::AudioProcessorValueTreeState& apvts, const juce::String& id, float v)
    {
        if (auto* p = apvts.getParameter (id))
            p->setValueNotifyingHost (p->convertTo0to1 (v));
    }

    inline float band (juce::AudioProcessorValueTreeState& apvts, int b, const char* key)
    {
        return value (apvts, params::id::compBand (b, key));
    }

    // The gain the band's static curve applies at levelDb (no makeup).
    inline float curveDb (juce::AudioProcessorValueTreeState& apvts, int b, float levelDb)
    {
        namespace k = params::id::fx::compband;
        const float ratio = juce::jmax (1.0f, band (apvts, b, k::ratio));
        const float up    = juce::jmax (1.0f, band (apvts, b, k::upRatio));
        return dsp::Multiband::staticGainDb (levelDb, band (apvts, b, k::threshold),
                                             1.0f - 1.0f / ratio, 1.0f - 1.0f / up,
                                             band (apvts, b, k::knee));
    }

    inline bool isOn (juce::AudioProcessorValueTreeState& apvts)
    {
        return value (apvts, params::id::fx::compEnable) >= 0.5f;
    }

    // Gesture-wrapped edit, so a drag is one host gesture and one undo step.
    struct Gesture
    {
        juce::RangedAudioParameter* param = nullptr;
        void begin (juce::AudioProcessorValueTreeState& apvts, const juce::String& id)
        {
            end();
            param = apvts.getParameter (id);
            if (param != nullptr) param->beginChangeGesture();
        }
        void end()
        {
            if (param != nullptr) param->endChangeGesture();
            param = nullptr;
        }
        ~Gesture() { end(); }
    };
}

//==============================================================================
class CompBandGraph final : public juce::Component,
                            public juce::SettableTooltipClient,
                            private juce::Timer
{
public:
    CompBandGraph (juce::AudioProcessorValueTreeState& state, const dsp::Telemetry& tel,
                   std::function<double()> sampleRateFn)
        : apvts (state), telemetry (tel), getSampleRate (std::move (sampleRateFn))
    {
        // A click target inside an editor that also hosts the on-screen
        // keyboard: taking focus here would steal its QWERTY notes.
        setWantsKeyboardFocus (false);
        setTooltip ("Drag a band up or down: threshold.  Drag a crossover line: split frequency.  "
                    "Wheel over a band: down ratio.  Double-click a band: reset its threshold.");
        spectrumIn.fill (-120.0f);
        spectrumOut.fill (-120.0f);
        startTimerHz (30);
    }

    ~CompBandGraph() override { stopTimer(); }

    void setSelectedBand (int b)
    {
        if (selected == b) return;
        selected = b;
        repaint();
    }
    std::function<void (int)> onBandSelected;

    // Test hooks.
    float crossoverX (int which) const { return xForHz (crossover (which)); }
    float thresholdY (int b) const { return yForDb (comp::band (apvts, b, params::id::fx::compband::threshold)); }
    juce::Rectangle<float> getPlotArea() const { return plotArea(); }

    void paint (juce::Graphics& g) override
    {
        const auto& t = currentTheme();
        const bool on = comp::isOn (apvts);
        const auto live = on ? t.accentMod : t.textSecondary.withAlpha (0.45f);
        const auto bounds = getLocalBounds().toFloat();
        draw::displayWell (g, bounds, false);

        const auto area = plotArea();
        g.setFont (metrics::smallFont());

        // Grid: decades across, 12 dB steps down.
        for (const float hz : { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f })
        {
            const auto x = xForHz (hz);
            const bool decade = hz == 100.0f || hz == 1000.0f || hz == 10000.0f;
            g.setColour (t.outline.withAlpha (decade ? 0.45f : 0.22f));
            g.drawVerticalLine ((int) x, area.getY(), area.getBottom());
            g.setColour (t.textSecondary.withAlpha (0.5f));
            g.drawText (hz >= 1000.0f ? juce::String ((int) (hz / 1000.0f)) + "k" : juce::String ((int) hz),
                        juce::Rectangle<float> (x - 15.0f, area.getBottom() + 1.0f, 30.0f, 10.0f),
                        juce::Justification::centred);
        }
        for (float db = -12.0f; db > bottomDb; db -= 12.0f)
        {
            const auto y = yForDb (db);
            g.setColour (t.outline.withAlpha (0.22f));
            g.drawHorizontalLine ((int) y, area.getX(), area.getRight());
            g.setColour (t.textSecondary.withAlpha (0.5f));
            g.drawText (juce::String ((int) db), juce::Rectangle<float> (area.getX() + 3.0f, y - 10.0f, 30.0f, 10.0f),
                        juce::Justification::centredLeft);
        }

        // Spectrum: what comes in (filled), what goes out (traced).
        drawSpectrum (g, spectrumIn, t.textSecondary, 0.10f, 0.30f);
        drawSpectrum (g, spectrumOut, t.accent, 0.0f, on ? 0.75f : 0.35f);

        bool anySolo = false;
        for (int b = 0; b < comp::numBands; ++b)
            anySolo = anySolo || comp::band (apvts, b, params::id::fx::compband::solo) >= 0.5f;

        const float edges[4] { area.getX(), xForHz (crossover (0)), xForHz (crossover (1)), area.getRight() };
        for (int b = 0; b < comp::numBands; ++b)
        {
            namespace k = params::id::fx::compband;
            const bool bypassed = comp::band (apvts, b, k::bypass) >= 0.5f;
            const bool soloed = comp::band (apvts, b, k::solo) >= 0.5f;
            const bool muted = anySolo && ! soloed;
            const bool isSel = b == selected;
            const bool isHover = b == hoverBand && dragXover < 0;
            const float dim = (bypassed || muted) ? 0.35f : 1.0f;

            const juce::Rectangle<float> column (edges[b], area.getY(), juce::jmax (1.0f, edges[b + 1] - edges[b]),
                                                 area.getHeight());
            const auto thrY = yForDb (comp::band (apvts, b, k::threshold));

            // The band block: everything below the threshold is tinted, so
            // the threshold reads as the block's top edge, the thing you grab.
            const auto block = column.withTop (thrY);
            g.setColour (live.withAlpha ((isSel ? 0.16f : (isHover ? 0.11f : 0.06f)) * dim));
            g.fillRect (block.reduced (1.0f, 0.0f));

            // Live gain change from the threshold line, on the graph's own dB scale.
            const float gr = on && ! bypassed ? juce::jlimit (-36.0f, 24.0f, shownGr[(size_t) b]) : 0.0f;
            if (std::abs (gr) > 0.05f)
            {
                const float pxPerDb = area.getHeight() / (topDb - bottomDb);
                const float h = std::abs (gr) * pxPerDb;
                const auto fill = gr < 0.0f ? juce::Rectangle<float> (column.getX(), thrY, column.getWidth(), h)
                                            : juce::Rectangle<float> (column.getX(), thrY - h, column.getWidth(), h);
                g.setColour ((gr < 0.0f ? t.accentMod : t.accent).withAlpha (0.45f * dim));
                g.fillRect (fill.reduced (1.0f, 0.0f).getIntersection (area));
            }

            // Threshold line + centre grip.
            const auto lineColour = isSel || isHover ? t.textPrimary : live;
            g.setColour (lineColour.withAlpha (dim));
            g.drawLine (column.getX() + 1.0f, thrY, column.getRight() - 1.0f, thrY, isSel ? 2.0f : 1.4f);
            const auto grip = juce::Rectangle<float> (18.0f, 6.0f).withCentre ({ column.getCentreX(), thrY });
            g.fillRoundedRectangle (grip, 3.0f);

            // Live detector level: a tick at the band's right edge.
            const float lvl = shownLevel[(size_t) b];
            if (on && lvl > bottomDb)
            {
                const auto y = yForDb (lvl);
                juce::Path tick;
                tick.addTriangle (column.getRight() - 2.0f, y, column.getRight() - 9.0f, y - 4.0f,
                                  column.getRight() - 9.0f, y + 4.0f);
                g.setColour (t.textPrimary.withAlpha (0.8f * dim));
                g.fillPath (tick);
            }

            // Caption: name, threshold, ratio(s), state.
            g.setColour (t.textSecondary.withAlpha (isSel ? 0.95f : 0.6f));
            g.setFont (metrics::smallFontBold());
            const auto caption = column.withHeight (12.0f).translated (0.0f, 3.0f);
            juce::String name = comp::bandNames[b];
            if (soloed) name << "  SOLO";
            if (bypassed) name << "  BYPASS";
            g.drawText (name, caption, juce::Justification::centred);
            g.setFont (metrics::smallFont());
            juce::String detail = juce::String (comp::band (apvts, b, k::threshold), 1) + " dB  "
                                + "down " + juce::String (comp::band (apvts, b, k::ratio), 1) + ":1";
            const float up = comp::band (apvts, b, k::upRatio);
            if (up > 1.01f) detail << "  up " << juce::String (up, 1) << ":1";
            g.drawText (detail, caption.translated (0.0f, 12.0f), juce::Justification::centred);
        }

        // Crossover lines and their frequency pills, last so they sit on top.
        for (int i = 0; i < 2; ++i)
        {
            const auto x = xForHz (crossover (i));
            const bool active = dragXover == i || hoverXover == i;
            g.setColour (active ? t.textPrimary : live.withAlpha (on ? 0.85f : 1.0f));
            g.drawLine (x, area.getY(), x, area.getBottom(), active ? 2.0f : 1.2f);

            const auto text = frequencyText (crossover (i));
            const float w = 46.0f;
            const auto pill = juce::Rectangle<float> (juce::jlimit (area.getX(), area.getRight() - w, x - w * 0.5f),
                                                      area.getBottom() - 16.0f, w, 14.0f);
            g.setColour (active ? t.textPrimary : live);
            g.fillRoundedRectangle (pill, 7.0f);
            g.setColour (t.background);
            g.setFont (metrics::smallFontBold());
            g.drawText (text, pill, juce::Justification::centred);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragXover = xoverNear (e.position.x);
        if (dragXover >= 0)
        {
            gesture.begin (apvts, xoverId (dragXover));
            return;
        }
        const auto b = bandAt (e.position.x);
        setSelectedBand (b);
        if (onBandSelected) onBandSelected (b);
        dragBand = b;
        dragStartY = e.position.y;
        dragStartDb = comp::band (apvts, b, params::id::fx::compband::threshold);
        gesture.begin (apvts, params::id::compBand (b, params::id::fx::compband::threshold));
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragXover >= 0)
        {
            setCrossover (dragXover, hzForX (e.position.x));
        }
        else if (dragBand >= 0)
        {
            // Relative, so a click never makes the threshold jump; Shift for fine.
            const float pxPerDb = plotArea().getHeight() / (topDb - bottomDb);
            const float scale = e.mods.isShiftDown() ? 0.25f : 1.0f;
            const float db = juce::jlimit (-60.0f, 0.0f, dragStartDb - (e.position.y - dragStartY) / pxPerDb * scale);
            comp::set (apvts, params::id::compBand (dragBand, params::id::fx::compband::threshold), db);
        }
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        gesture.end();
        dragXover = dragBand = -1;
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (xoverNear (e.position.x) >= 0) return;
        const auto id = params::id::compBand (bandAt (e.position.x), params::id::fx::compband::threshold);
        if (auto* p = apvts.getParameter (id))
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost (p->getDefaultValue());
            p->endChangeGesture();
        }
        repaint();
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        const auto b = bandAt (e.position.x);
        const auto id = params::id::compBand (b, params::id::fx::compband::ratio);
        const float ratio = comp::band (apvts, b, params::id::fx::compband::ratio);
        if (auto* p = apvts.getParameter (id))
        {
            p->beginChangeGesture();
            comp::set (apvts, id, juce::jlimit (1.0f, 20.0f, ratio * (1.0f + w.deltaY * 0.5f)));
            p->endChangeGesture();
        }
        repaint();
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const auto x = xoverNear (e.position.x);
        const auto b = x >= 0 ? -1 : bandAt (e.position.x);
        if (x == hoverXover && b == hoverBand) return;
        hoverXover = x;
        hoverBand = b;
        setMouseCursor (x >= 0 ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::UpDownResizeCursor);
        repaint();
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        hoverXover = hoverBand = -1;
        repaint();
    }

private:
    static constexpr float minHz = 20.0f, maxHz = 20000.0f, topDb = 0.0f, bottomDb = -66.0f;
    static constexpr float grabRadius = 8.0f;
    static constexpr int fftOrder = 12, fftSize = 1 << fftOrder;
    static_assert (fftSize == dsp::Telemetry::scopeSize);
    using Spectrum = std::array<float, fftSize / 2>;

    static const char* xoverId (int which)
    {
        return which == 0 ? params::id::fx::compXoverLow : params::id::fx::compXoverHigh;
    }

    float crossover (int which) const { return comp::value (apvts, xoverId (which)); }

    void setCrossover (int which, float hz)
    {
        // The two must not cross, or the mid band inverts.
        constexpr float separation = 1.25f;
        if (which == 0) hz = juce::jlimit (20.0f, 2000.0f, juce::jmin (hz, crossover (1) / separation));
        else            hz = juce::jlimit (200.0f, 18000.0f, juce::jmax (hz, crossover (0) * separation));
        comp::set (apvts, xoverId (which), hz);
    }

    juce::Rectangle<float> plotArea() const
    {
        return getLocalBounds().toFloat().reduced (2.0f).withTrimmedBottom (11.0f);
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

    float yForDb (float db) const
    {
        const auto r = plotArea();
        return juce::jmap (juce::jlimit (bottomDb, topDb, db), topDb, bottomDb, r.getY(), r.getBottom());
    }

    int xoverNear (float x) const
    {
        int best = -1;
        float bestDist = grabRadius;
        for (int i = 0; i < 2; ++i)
        {
            const auto d = std::abs (x - xForHz (crossover (i)));
            if (d <= bestDist) { best = i; bestDist = d; }
        }
        return best;
    }

    int bandAt (float x) const
    {
        if (x < xForHz (crossover (0))) return 0;
        if (x < xForHz (crossover (1))) return 1;
        return 2;
    }

    static juce::String frequencyText (float hz)
    {
        return hz >= 1000.0f ? juce::String (hz / 1000.0f, hz >= 10000.0f ? 0 : 1) + "k"
                             : juce::String (juce::roundToInt (hz));
    }

    void drawSpectrum (juce::Graphics& g, const Spectrum& spec, juce::Colour colour, float fillAlpha, float strokeAlpha) const
    {
        const auto area = plotArea();
        const double sr = juce::jmax (8000.0, getSampleRate());
        const float binHz = (float) sr / (float) fftSize;
        juce::Path p;
        constexpr int steps = 200;
        for (int i = 0; i <= steps; ++i)
        {
            const float x = area.getX() + area.getWidth() * (float) i / steps;
            const float f = hzForX (x);
            // ~1/6 octave average so the trace isn't jagged at bin resolution.
            int i0 = juce::jmax (1, (int) (f * 0.944f / binHz));
            int i1 = juce::jmin ((int) spec.size() - 1, (int) (f * 1.059f / binHz));
            if (i1 < i0) i1 = i0;
            float sum = 0.0f;
            for (int k = i0; k <= i1; ++k) sum += spec[(size_t) k];
            // A +4.5 dB/oct tilt so a mix reads level across the range, as
            // in most multiband analyzers.
            const float db = sum / (float) (i1 - i0 + 1) + 4.5f * std::log2 (f / 1000.0f);
            const float y = yForDb (db);
            if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
        }
        if (fillAlpha > 0.0f)
        {
            auto fill = p;
            fill.lineTo (area.getRight(), area.getBottom());
            fill.lineTo (area.getX(), area.getBottom());
            fill.closeSubPath();
            g.setColour (colour.withAlpha (fillAlpha));
            g.fillPath (fill);
        }
        g.setColour (colour.withAlpha (strokeAlpha));
        g.strokePath (p, juce::PathStrokeType (1.2f));
    }

    static const std::array<float, fftSize>& hann()
    {
        static const auto table = []
        {
            std::array<float, fftSize> w {};
            for (int i = 0; i < fftSize; ++i)
                w[(size_t) i] = 0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi * (float) i / (float) (fftSize - 1));
            return w;
        }();
        return table;
    }

    void analyse (const std::array<std::atomic<float>, fftSize>& ring, const std::atomic<int>& writeIdx, Spectrum& out)
    {
        const int w = writeIdx.load (std::memory_order_acquire);
        for (int i = 0; i < fftSize; ++i)
            fftData[(size_t) i] = ring[(size_t) ((w + i) & (fftSize - 1))].load (std::memory_order_relaxed) * hann()[(size_t) i];
        std::fill (fftData.begin() + fftSize, fftData.end(), 0.0f);
        fft.performFrequencyOnlyForwardTransform (fftData.data());
        const float norm = 2.0f / (float) fftSize;
        for (size_t i = 0; i < out.size(); ++i)
        {
            const float db = juce::Decibels::gainToDecibels (fftData[i] * norm + 1.0e-9f);
            out[i] = db > out[i] ? db : out[i] * 0.85f + db * 0.15f;
        }
    }

    void timerCallback() override
    {
        if (! isLiveShowing (*this))
            return;
        analyse (telemetry.preScope, telemetry.preScopeWrite, spectrumIn);
        analyse (telemetry.scope, telemetry.scopeWrite, spectrumOut);
        const bool on = comp::isOn (apvts);
        for (int b = 0; b < comp::numBands; ++b)
        {
            // Snap toward a bigger excursion and ease back, so the fill reads
            // as movement rather than flicker at 30 fps.
            const auto target = on ? telemetry.compBandDb[(size_t) b].load (std::memory_order_relaxed) : 0.0f;
            auto& gr = shownGr[(size_t) b];
            gr = std::abs (target) > std::abs (gr) ? target : gr * 0.84f;
            const auto lvl = on ? telemetry.compBandLevelDb[(size_t) b].load (std::memory_order_relaxed) : -100.0f;
            auto& shown = shownLevel[(size_t) b];
            shown = lvl > shown ? lvl : juce::jmax (lvl, shown - 1.5f);
        }
        repaint();
    }

    juce::AudioProcessorValueTreeState& apvts;
    const dsp::Telemetry& telemetry;
    std::function<double()> getSampleRate;
    comp::Gesture gesture;

    juce::dsp::FFT fft { fftOrder };
    std::array<float, fftSize * 2> fftData {};
    Spectrum spectrumIn {}, spectrumOut {};
    std::array<float, comp::numBands> shownGr {};
    std::array<float, comp::numBands> shownLevel { -100.0f, -100.0f, -100.0f };

    int selected = 0;
    int dragXover = -1, dragBand = -1, hoverXover = -1, hoverBand = -1;
    float dragStartY = 0.0f, dragStartDb = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CompBandGraph)
};

//==============================================================================
// Input vs output for the selected band. Drag up / down to move its threshold.
class CompTransferCurve final : public juce::Component,
                                public juce::SettableTooltipClient,
                                private juce::Timer
{
public:
    CompTransferCurve (juce::AudioProcessorValueTreeState& state, const dsp::Telemetry& tel)
        : apvts (state), telemetry (tel)
    {
        setWantsKeyboardFocus (false);
        setTooltip ("Transfer curve of the selected band: input level across, output level up. "
                    "Above the threshold the curve flattens by the Down Ratio; below it, the Upward Ratio lifts quiet signal. "
                    "Drag up or down to move the threshold.");
        startTimerHz (30);
    }
    ~CompTransferCurve() override { stopTimer(); }

    void setSelectedBand (int b) { selected = b; repaint(); }

    void paint (juce::Graphics& g) override
    {
        namespace k = params::id::fx::compband;
        const auto& t = currentTheme();
        const bool on = comp::isOn (apvts);
        const auto live = on ? t.accentMod : t.textSecondary.withAlpha (0.5f);
        draw::displayWell (g, getLocalBounds().toFloat(), false);
        const auto r = plot();

        g.setFont (metrics::smallFont());
        for (float db = -12.0f; db > minDb; db -= 12.0f)
        {
            g.setColour (t.outline.withAlpha (0.25f));
            g.drawVerticalLine ((int) xFor (db), r.getY(), r.getBottom());
            g.drawHorizontalLine ((int) yFor (db), r.getX(), r.getRight());
        }
        g.setColour (t.outline.withAlpha (0.6f));
        g.drawLine (r.getX(), r.getBottom(), r.getRight(), r.getY(), 1.0f);   // 1:1

        const float thr = comp::band (apvts, selected, k::threshold);
        const float makeup = comp::band (apvts, selected, k::gain);
        g.setColour (t.textSecondary.withAlpha (0.45f));
        const float dashes[] { 3.0f, 3.0f };
        g.drawDashedLine ({ xFor (thr), r.getBottom(), xFor (thr), r.getY() }, dashes, 2);

        juce::Path curve;
        for (int i = 0; i <= 120; ++i)
        {
            const float in = minDb + (0.0f - minDb) * (float) i / 120.0f;
            const float out = in + comp::curveDb (apvts, selected, in) + makeup;
            const auto pt = juce::Point<float> (xFor (in), yFor (out));
            if (i == 0) curve.startNewSubPath (pt); else curve.lineTo (pt);
        }
        g.saveState();
        g.reduceClipRegion (r.toNearestInt());
        g.setColour (live);
        g.strokePath (curve, juce::PathStrokeType (2.0f));

        if (on && level > minDb && comp::band (apvts, selected, k::bypass) < 0.5f)
        {
            const float out = level + comp::curveDb (apvts, selected, level) + makeup;
            g.setColour (t.textPrimary);
            g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre ({ xFor (level), yFor (out) }));
        }
        g.restoreState();

        g.setColour (t.textSecondary.withAlpha (0.7f));
        g.setFont (metrics::smallFontBold());
        g.drawText (juce::String (comp::bandNames[selected]) + " CURVE", r.withHeight (12.0f).translated (4.0f, 2.0f),
                    juce::Justification::centredLeft);
        g.setFont (metrics::smallFont());
        g.drawText ("IN", r.withTrimmedTop (r.getHeight() - 11.0f).withTrimmedRight (3.0f), juce::Justification::centredRight);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        startY = e.position.y;
        startDb = comp::band (apvts, selected, params::id::fx::compband::threshold);
        gesture.begin (apvts, params::id::compBand (selected, params::id::fx::compband::threshold));
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        const float pxPerDb = plot().getHeight() / -minDb;
        comp::set (apvts, params::id::compBand (selected, params::id::fx::compband::threshold),
                   juce::jlimit (-60.0f, 0.0f, startDb - (e.position.y - startY) / pxPerDb));
        repaint();
    }
    void mouseUp (const juce::MouseEvent&) override { gesture.end(); }

private:
    static constexpr float minDb = -60.0f;

    juce::Rectangle<float> plot() const { return getLocalBounds().toFloat().reduced (3.0f); }
    float xFor (float db) const { auto r = plot(); return juce::jmap (db, minDb, 0.0f, r.getX(), r.getRight()); }
    float yFor (float db) const { auto r = plot(); return juce::jmap (db, minDb, 0.0f, r.getBottom(), r.getY()); }

    void timerCallback() override
    {
        if (! isLiveShowing (*this)) return;
        const float target = telemetry.compBandLevelDb[(size_t) selected].load (std::memory_order_relaxed);
        level = target > level ? target : juce::jmax (target, level - 1.5f);
        repaint();
    }

    juce::AudioProcessorValueTreeState& apvts;
    const dsp::Telemetry& telemetry;
    comp::Gesture gesture;
    int selected = 0;
    float level = -100.0f, startY = 0.0f, startDb = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CompTransferCurve)
};

//==============================================================================
// IN | GR | OUT bars: the module's own levels and the selected band's change.
class CompMeters final : public juce::Component,
                         public juce::SettableTooltipClient,
                         private juce::Timer
{
public:
    explicit CompMeters (const dsp::Telemetry& tel) : telemetry (tel)
    {
        setTooltip ("IN / OUT: level into and out of the compressor.  GR: the selected band's gain change "
                    "(down = reduction, up = upward lift).");
        startTimerHz (30);
    }
    ~CompMeters() override { stopTimer(); }

    void setSelectedBand (int b) { selected = b; }

    void paint (juce::Graphics& g) override
    {
        const auto& t = currentTheme();
        auto area = getLocalBounds().toFloat();
        auto labels = area.removeFromBottom (12.0f);
        const float w = area.getWidth() / 3.0f;
        const char* names[3] { "IN", "GR", "OUT" };
        for (int i = 0; i < 3; ++i)
        {
            auto lane = area.removeFromLeft (w).reduced (3.0f, 0.0f);
            g.setColour (t.meterLane);
            g.fillRoundedRectangle (lane, 2.0f);
            if (i == 1)
            {
                // Signed, centred: reduction hangs down, lift rises.
                const float cy = lane.getCentreY();
                const float h = lane.getHeight() * 0.5f * juce::jlimit (0.0f, 1.0f, std::abs (gr) / 24.0f);
                g.setColour (gr < 0.0f ? t.accentMod : t.accent);
                g.fillRect (gr < 0.0f ? lane.withTop (cy).withHeight (h) : lane.withTop (cy - h).withHeight (h));
                g.setColour (t.outline);
                g.drawHorizontalLine ((int) cy, lane.getX(), lane.getRight());
            }
            else
            {
                const float db = juce::Decibels::gainToDecibels (i == 0 ? inLevel : outLevel, -60.0f);
                const float frac = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
                g.setColour (db > -0.1f ? t.meterRed : (db > -12.0f ? t.meterYellow : t.meterGreen));
                g.fillRoundedRectangle (lane.withTop (lane.getBottom() - lane.getHeight() * frac), 2.0f);
            }
            g.setColour (t.textSecondary);
            g.setFont (metrics::smallFont());
            g.drawText (names[i], labels.removeFromLeft (w), juce::Justification::centred);
        }
    }

private:
    void timerCallback() override
    {
        if (! isLiveShowing (*this)) return;
        const auto ease = [] (float& shown, float target, float fall)
        { shown = target > shown ? target : juce::jmax (target, shown * fall); };
        ease (inLevel, telemetry.compInPeak.load (std::memory_order_relaxed), 0.86f);
        ease (outLevel, telemetry.compOutPeak.load (std::memory_order_relaxed), 0.86f);
        const float target = telemetry.compBandDb[(size_t) selected].load (std::memory_order_relaxed);
        gr = std::abs (target) > std::abs (gr) ? target : gr * 0.84f;
        repaint();
    }

    const dsp::Telemetry& telemetry;
    int selected = 0;
    float inLevel = 0.0f, outLevel = 0.0f, gr = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CompMeters)
};

//==============================================================================
class CompPanel final : public juce::Component
{
public:
    static constexpr int numBandKnobs = 7;

    CompPanel (juce::AudioProcessorValueTreeState& state, const dsp::Telemetry& tel,
               std::function<double()> sampleRateFn = [] { return 48000.0; })
        : graph (state, tel, std::move (sampleRateFn)),
          curve (state, tel),
          meters (tel),
          header (state, { { "Multi-band compressor", params::id::fx::compEnable } }),
          xoverLow (state, params::id::fx::compXoverLow, "Low X"),
          xoverHigh (state, params::id::fx::compXoverHigh, "High X"),
          mix (state, params::id::fx::compMix, "Mix")
    {
        addAndMakeVisible (graph);
        addAndMakeVisible (curve);
        addAndMakeVisible (meters);
        addAndMakeVisible (header);
        addAndMakeVisible (xoverLow);
        addAndMakeVisible (xoverHigh);
        addAndMakeVisible (mix);
        graph.onBandSelected = [this] (int b) { selectBand (b); };

        namespace k = params::id::fx::compband;
        static const char* keys[numBandKnobs] { k::threshold, k::ratio, k::upRatio, k::knee,
                                                k::attack, k::release, k::gain };
        static const char* captions[numBandKnobs] { "Threshold", "Down Ratio", "Up Ratio", "Knee",
                                                    "Attack", "Release", "Makeup" };

        for (int b = 0; b < comp::numBands; ++b)
        {
            auto button = std::make_unique<BandButton> (comp::bandNames[b]);
            button->onClick = [this, b] { selectBand (b); };
            addAndMakeVisible (*button);
            bandButtons[(size_t) b] = std::move (button);

            for (int i = 0; i < numBandKnobs; ++i)
            {
                auto knob = std::make_unique<Knob> (state, params::id::compBand (b, keys[i]), captions[i]);
                addChildComponent (*knob);
                knobs[(size_t) b][(size_t) i] = std::move (knob);
            }
            solo[(size_t) b] = std::make_unique<Toggle> (state, params::id::compBand (b, k::solo), "SOLO");
            bypass[(size_t) b] = std::make_unique<Toggle> (state, params::id::compBand (b, k::bypass), "BYPASS");
            solo[(size_t) b]->button.setTooltip ("Solo: hear only this band (other soloed bands too).");
            bypass[(size_t) b]->button.setTooltip ("Bypass: pass this band through uncompressed.");
            addChildComponent (*solo[(size_t) b]);
            addChildComponent (*bypass[(size_t) b]);
        }

        powerTracker = std::make_unique<TabEngagementTracker> (state,
            std::vector<std::pair<juce::String, std::vector<juce::String>>> {
                { "on", { params::id::fx::compEnable } } }, *this);
        selectBand (0);
    }

    void paint (juce::Graphics&) override {}

    void resized() override
    {
        header.setBounds (getLocalBounds().removeFromTop (metrics::sectionHeaderHeight));
        auto area = getLocalBounds().withTrimmedTop (metrics::sectionHeaderHeight).reduced (7, 3);

        // Bottom: the controls row. Top: graph | transfer curve | meters.
        auto controls = area.removeFromBottom (juce::jlimit (78, 96, area.getHeight() / 4));
        area.removeFromBottom (6);

        meters.setBounds (area.removeFromRight (66));
        area.removeFromRight (6);
        const int curveSize = juce::jmin (area.getHeight(), juce::roundToInt ((float) area.getWidth() * 0.3f));
        curve.setBounds (area.removeFromRight (curveSize).withSizeKeepingCentre (curveSize, curveSize));
        area.removeFromRight (6);
        graph.setBounds (area);

        // Band selector + its Solo / Bypass.
        auto selector = controls.removeFromLeft (juce::jmin (210, controls.getWidth() / 5));
        auto tabs = selector.removeFromTop (22);
        const auto tabWidth = tabs.getWidth() / comp::numBands;
        for (int b = 0; b < comp::numBands; ++b)
            bandButtons[(size_t) b]->setBounds (tabs.removeFromLeft (tabWidth).reduced (2, 0));
        selector.removeFromTop (8);
        auto toggles = selector.removeFromTop (22);
        const auto half = toggles.getWidth() / 2;
        for (int b = 0; b < comp::numBands; ++b)
        {
            solo[(size_t) b]->setBounds (toggles.withWidth (half).reduced (4, 0));
            bypass[(size_t) b]->setBounds (toggles.withTrimmedLeft (half).reduced (4, 0));
        }
        controls.removeFromLeft (10);

        // Seven band knobs, a gap, then the module-wide crossovers and Mix.
        const auto cell = controls.getWidth() / (numBandKnobs + 3 + 1);
        for (int i = 0; i < numBandKnobs; ++i)
            knobs[(size_t) selected][(size_t) i]->setBounds (controls.removeFromLeft (cell));
        controls.removeFromLeft (cell);
        xoverLow.setBounds (controls.removeFromLeft (cell));
        xoverHigh.setBounds (controls.removeFromLeft (cell));
        mix.setBounds (controls.removeFromLeft (cell));
    }

    // Test hooks.
    CompBandGraph& getGraph() { return graph; }
    Knob* getBandKnob (int b, int index) { return knobs[(size_t) b][(size_t) index].get(); }
    Knob& getMixKnob() { return mix; }
    juce::Component* getBandButton (int b) { return bandButtons[(size_t) b].get(); }
    int getSelectedBand() const { return selected; }

    void selectBand (int b)
    {
        selected = juce::jlimit (0, comp::numBands - 1, b);
        graph.setSelectedBand (selected);
        curve.setSelectedBand (selected);
        meters.setSelectedBand (selected);
        for (int i = 0; i < comp::numBands; ++i)
        {
            if (auto* button = dynamic_cast<BandButton*> (bandButtons[(size_t) i].get()))
                button->setSelected (i == selected);
            for (auto& knob : knobs[(size_t) i])
                knob->setVisible (i == selected);
            solo[(size_t) i]->setVisible (i == selected);
            bypass[(size_t) i]->setVisible (i == selected);
        }
        resized();
    }

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

    CompBandGraph graph;
    CompTransferCurve curve;
    CompMeters meters;
    FxPanelHeader header;
    Knob xoverLow, xoverHigh, mix;
    std::array<std::unique_ptr<juce::Button>, comp::numBands> bandButtons;
    std::array<std::array<std::unique_ptr<Knob>, numBandKnobs>, comp::numBands> knobs;
    std::array<std::unique_ptr<Toggle>, comp::numBands> solo, bypass;
    std::unique_ptr<TabEngagementTracker> powerTracker;
    int selected = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CompPanel)
};

} // namespace spa::ui
