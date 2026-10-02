#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_dsp/juce_dsp.h>
#include "Theme.h"
#include "FxPanelHeader.h"
#include "../SPAStripProcessor.h"
#include "Controls.h"
#include "../dsp/ParametricEQ.h"
#include "../dsp/Telemetry.h"
#include "../params/ParameterRegistry.h"
#include <array>
#include <cmath>

namespace spa::ui
{

// Pro-Q-style interactive EQ: a log-frequency response graph with a live FFT
// spectrum analyzer behind it, 8 draggable band nodes (X = freq, Y = gain,
// wheel = Q), double-click to add/remove a band, plus an on/off toggle and a
// character selector. Nodes write straight to the APVTS band params; the curve
// is drawn by the same magnitude function the DSP uses, so it never lies.
class EqEditor : public juce::Component,
                 public juce::SettableTooltipClient,
                 private juce::Timer
{
public:
    explicit EqEditor (SPAStripProcessor& p)
        : processor (p), apvts (p.getAPVTS()), telemetry (p.getTelemetry()),
          getSampleRate ([&p] { return p.getSampleRate(); }),
          header (p.getAPVTS(), { { "EQ", params::id::fx::eqEnable } }),
          character (p.getAPVTS(), params::id::fx::eqCharacter)
    {
        refreshSampleRate();
        analyzerSpeed = (AnalyzerSpeed) juce::jlimit (0, 2,
            (int) apvts.state.getProperty ("uiEqAnalyzerSpeed", (int) AnalyzerSpeed::medium));
        addAndMakeVisible (header);
        addAndMakeVisible (character);
        setWantsKeyboardFocus (false);
        modName = (juce::SystemStats::getOperatingSystemType() & juce::SystemStats::MacOSX)
                      ? "Cmd" : "Ctrl";
        setTooltip ("Double-click empty space to add a Bell (near the left/right edges: a "
                    "Low/High Cut). Double-click a node to remove it. Drag a node to move it; "
                    + modName + "-drag vertically or use the mouse wheel to set its Q. "
                    "Right-click a node to change its type or (for cuts) its slope.");
        wheelHoldTimer.onTimer = [this] { closeUndoHold(); };
        startTimerHz (30);
    }

    ~EqEditor() override
    {
        stopTimer();
        closeUndoHold();
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto top = r.removeFromTop (metrics::sectionHeaderHeight);
        header.setBounds (top);
        top.removeFromTop (metrics::sectionHeaderTopInset);
        character.setBounds (top.removeFromRight (130).reduced (4, 2).withSizeKeepingCentre (122, 20));
        graph = r.reduced (8, 6).toFloat();
        // A row above the frequency-label row (graph.getBottom()-11..-1), so
        // it never collides with those labels or the legend (top-left).
        speedLabelRect = juce::Rectangle<float> (graph.getRight() - 46.0f, graph.getBottom() - 23.0f, 42.0f, 11.0f);
    }

    void paint (juce::Graphics& g) override
    {
        const auto& t = currentTheme();
        const bool on = value (params::id::fx::eqEnable) >= 0.5f;

        // Faceplate restyle: no display-well fill/border behind the graph —
        // the grid + spectrum + curve draw straight on the faceplate surface.
        drawGrid (g, t);
        drawSpectrum (g, t);

        const auto bands = readBands();
        const auto colour = on ? t.accentMod : t.textSecondary.withAlpha (0.5f);

        // Response curve + fill.
        juce::Path curve;
        constexpr int steps = 220;
        for (int i = 0; i <= steps; ++i)
        {
            const float x = graph.getX() + graph.getWidth() * (float) i / steps;
            const float db = dsp::ParametricEQ::magnitudeDb (bands, xToFreq (x), sampleRate);
            const float y = dbToY (juce::jlimit (-dbRange, dbRange, db));
            if (i == 0) curve.startNewSubPath (x, y); else curve.lineTo (x, y);
        }
        auto fill = curve;
        fill.lineTo (graph.getRight(), dbToY (0.0f));
        fill.lineTo (graph.getX(), dbToY (0.0f));
        fill.closeSubPath();
        g.setColour (colour.withAlpha (0.14f));
        g.fillPath (fill);
        draw::glowStroke (g, curve, colour, 1.8f);

        // Band nodes.
        for (int b = 0; b < numBands; ++b)
        {
            if (! bandEnabled (b)) continue;
            const auto c = nodeCentre (b);
            const bool hot = (b == dragBand || b == hoverBand || b == selectedBand);
            const float rad = hot ? nodeRadius + 2.0f : nodeRadius;
            g.setColour (t.display.withAlpha (0.9f));
            g.fillEllipse (c.x - rad - 1.0f, c.y - rad - 1.0f, (rad + 1.0f) * 2.0f, (rad + 1.0f) * 2.0f);
            g.setColour (on ? t.accentMod : t.textSecondary);
            g.fillEllipse (c.x - rad, c.y - rad, rad * 2.0f, rad * 2.0f);
            if (b == selectedBand)   // selection ring
            {
                g.setColour (t.textPrimary);
                g.drawEllipse (c.x - rad - 2.0f, c.y - rad - 2.0f,
                               (rad + 2.0f) * 2.0f, (rad + 2.0f) * 2.0f, 1.5f);
            }
            g.setColour (t.background);
            g.setFont (juce::Font (juce::FontOptions (10.0f)));
            g.drawText (juce::String (b + 1),
                        juce::Rectangle<float> (c.x - rad, c.y - rad, rad * 2.0f, rad * 2.0f),
                        juce::Justification::centred);

            // Type badge next to hot nodes ("LC 24", "HS", "BP", "TILT"...) so a
            // cut's slope is visible without opening the menu.
            if (hot)
            {
                const int type = (int) rawBand (b, params::id::fx::eqband::type);
                const juce::String badge = badgeText (type, (int) rawBand (b, params::id::fx::eqband::slope));
                g.setColour (t.textPrimary.withAlpha (0.85f));
                g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
                g.drawText (badge, juce::Rectangle<float> (c.x + rad + 4.0f, c.y - 7.0f, 60.0f, 14.0f),
                            juce::Justification::centredLeft);
            }
        }

        // Readout for the selected node (or the one under the pointer): frequency,
        // gain, and Q, so the Q wheel has a visible target.
        const int info = hoverBand >= 0 ? hoverBand : selectedBand;
        if (info >= 0 && bandEnabled (info))
        {
            const float f = rawBand (info, params::id::fx::eqband::freq);
            const float gainDb = rawBand (info, params::id::fx::eqband::gain);
            const float q = rawBand (info, params::id::fx::eqband::q);
            const int type = (int) rawBand (info, params::id::fx::eqband::type);
            const int slope = (int) rawBand (info, params::id::fx::eqband::slope);
            juce::String txt = "B" + juce::String (info + 1) + "  " + badgeText (type, slope) + "   "
                             + (f >= 1000.0f ? juce::String (f / 1000.0f, 2) + " kHz"
                                             : juce::String (juce::roundToInt (f)) + " Hz");
            if (isGainType (type)) txt += "   " + juce::String (gainDb, 1) + " dB";
            txt += "   Q " + juce::String (q, 2);
            g.setColour (t.textSecondary);
            g.setFont (juce::Font (juce::FontOptions (11.0f)));
            g.drawText (txt, graph.reduced (8.0f, 5.0f).removeFromTop (14.0f),
                        juce::Justification::topLeft);
        }

        // Subtle usage hint, top-right (full detail is in the tooltip).
        g.setColour (t.textSecondary.withAlpha (0.45f));
        g.setFont (juce::Font (juce::FontOptions (10.0f)));
        g.drawText ("double-click: add / remove (edges: cut)    right-click: type    "
                    + modName + "-drag or wheel: Q",
                    graph.reduced (8.0f, 5.0f).removeFromTop (13.0f),
                    juce::Justification::topRight);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const int b = bandAt (e.position);
        if (e.mods.isPopupMenu())   // right-click / Ctrl-click: type + slope menu
        {
            if (b >= 0)
            {
                selectedBand = b;
                repaint();
                showTypeMenu (b);
            }
            else if (speedLabelRect.toNearestInt().contains (e.getPosition()))
            {
                showSpeedMenu();
            }
            return;
        }
        // Left-click on the SLOW/MED/FAST label cycles it (right-click also
        // opens the same choice as a menu, above).
        if (speedLabelRect.toNearestInt().contains (e.getPosition()))
        {
            analyzerSpeed = (AnalyzerSpeed) (((int) analyzerSpeed + 1) % 3);
            apvts.state.setProperty ("uiEqAnalyzerSpeed", (int) analyzerSpeed, nullptr);
            repaint();
            return;
        }
        // Click selects and grabs the node under the pointer (no accidental
        // creation); clicking empty space deselects. Nodes are added/removed by
        // double-click.
        selectedBand = b;
        dragBand = b;
        qDragActive = false;
        if (b >= 0)
            openUndoHold();
        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragBand < 0) return;
        if (e.mods.isCommandDown())   // Cmd/Ctrl-drag = Q (Pro-Q style), vertical
        {
            if (! qDragActive)        // anchor when the Q gesture begins
            {
                qDragActive = true;
                qRefY = e.position.y;
                qRefQ = rawBand (dragBand, params::id::fx::eqband::q);
            }
            const float dy = qRefY - e.position.y;   // up = narrower (higher Q)
            setBandRaw (dragBand, params::id::fx::eqband::q,
                        juce::jlimit (0.1f, 18.0f, qRefQ * std::exp (dy * 0.012f)));
        }
        else
        {
            qDragActive = false;
            applyDrag (e.position);   // freq (x) + gain (y)
        }
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        dragBand = -1;
        qDragActive = false;
        closeUndoHold();
        repaint();
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        // Double-click a node to remove it, or empty graph space to add one:
        // near the left edge -> Low Cut, near the right edge -> High Cut,
        // otherwise a Bell (matches the on-panel hint).
        const int b = bandAt (e.position);
        if (b >= 0)
        {
            setBand (b, params::id::fx::eqband::enable, 0.0f);
            if (selectedBand == b) selectedBand = -1;
        }
        else
        {
            const float frac = graph.getWidth() > 0.0f
                              ? (e.position.x - graph.getX()) / graph.getWidth() : 0.5f;
            const int edgeType = frac < 0.12f ? (int) dsp::ParametricEQ::Type::lowCut
                               : frac > 0.88f ? (int) dsp::ParametricEQ::Type::highCut
                                              : (int) dsp::ParametricEQ::Type::bell;
            const int n = createBandAt (e.position, edgeType);
            if (n >= 0) selectedBand = n;
        }
        repaint();
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const int b = bandAt (e.position);
        if (b != hoverBand) { hoverBand = b; repaint(); }
        setMouseCursor (b >= 0 ? juce::MouseCursor::DraggingHandCursor
                               : juce::MouseCursor::NormalCursor);
    }

    void mouseExit (const juce::MouseEvent&) override { hoverBand = -1; repaint(); }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        // Wheel sets Q: the hovered node if the pointer is over one, otherwise
        // the currently selected node (so you can dial Q after picking a node).
        int b = bandAt (e.position);
        if (b < 0) b = selectedBand;
        if (b < 0 || ! bandEnabled (b)) return;
        const float q = rawBand (b, params::id::fx::eqband::q);
        // A burst of wheel ticks is ONE undo step: the hold closes 500 ms after the last tick.
        openUndoHold();
        wheelHoldTimer.startTimer (500);
        setBandRaw (b, params::id::fx::eqband::q,
                    juce::jlimit (0.1f, 18.0f, q * (1.0f + w.deltaY * 0.6f)));
        repaint();
    }

    void timerCallback() override
    {
        refreshSampleRate();
        // CPU gate only -- isShowing(), never an EQ-enable gate: the
        // analyzer must keep running while the EQ module is switched off
        // (Phil's request) so it stays useful as a general-purpose pre/post
        // visualizer, not just an EQ-tuning aid.
        if (! isLiveShowing (*this)) return;
        computeSpectrum();
        // Repaint only when the drawn analyzer data moved. With nothing
        // playing the spectra settle on the floor and every 30 Hz repaint of
        // this large panel was redundant, yet each one forced a full window
        // commit (measured: ~40% of a core per open editor while idle).
        if (analyzerChangedSincePaint())
            repaint();
    }

private:
    static constexpr int numBands = dsp::ParametricEQ::numBands;
    static constexpr float minF = 20.0f, maxF = 20000.0f, dbRange = 24.0f, nodeRadius = 6.0f;
    enum class AnalyzerSpeed { slow = 0, medium = 1, fast = 2 };

    float value (const juce::String& id) const
    {
        if (auto* v = apvts.getRawParameterValue (id)) return v->load();
        return 0.0f;
    }
    float rawBand (int b, const char* key) const
    {
        return value (params::id::eqBand (b, key));
    }
    bool bandEnabled (int b) const { return rawBand (b, params::id::fx::eqband::enable) >= 0.5f; }

    void setBandRaw (int b, const char* key, float realValue)
    {
        const auto id = params::id::eqBand (b, key);
        // A node drag / wheel holds one undo step open (undoHold) so all of its
        // per-tick writes fold into ONE step; a discrete edit (type menu,
        // double-click add/remove) opens its own.
        SPAStripProcessor::UndoStep undoScope (processor, "EQ");
        if (auto* p = apvts.getParameter (id))
            p->setValueNotifyingHost (p->convertTo0to1 (realValue));
    }
    void setBand (int b, const char* key, float realValue) { setBandRaw (b, key, realValue); }

    std::array<dsp::ParametricEQ::Band, numBands> readBands() const
    {
        std::array<dsp::ParametricEQ::Band, numBands> bands {};
        for (int b = 0; b < numBands; ++b)
        {
            auto& bd = bands[(size_t) b];
            bd.enabled = bandEnabled (b);
            bd.type    = (int) rawBand (b, params::id::fx::eqband::type);
            bd.slope   = (int) rawBand (b, params::id::fx::eqband::slope);
            bd.freq    = rawBand (b, params::id::fx::eqband::freq);
            bd.gainDb  = rawBand (b, params::id::fx::eqband::gain);
            bd.q       = rawBand (b, params::id::fx::eqband::q);
        }
        return bands;
    }

    float freqToX (float f) const
    {
        return graph.getX() + graph.getWidth()
             * std::log (f / minF) / std::log (maxF / minF);
    }
    float xToFreq (float x) const
    {
        return minF * std::pow (maxF / minF,
                                (x - graph.getX()) / juce::jmax (1.0f, graph.getWidth()));
    }
    float dbToY (float db) const
    {
        return graph.getY() + graph.getHeight() * (0.5f - db / (2.0f * dbRange));
    }
    float yToDb (float y) const
    {
        return (0.5f - (y - graph.getY()) / juce::jmax (1.0f, graph.getHeight())) * 2.0f * dbRange;
    }

    // Bell / Low Shelf / High Shelf / Tilt Shelf carry a gain; cuts, Notch and
    // Band Pass don't (their node sits on the 0 dB line).
    static bool isGainType (int type)
    {
        using T = dsp::ParametricEQ::Type;
        return (T) type == T::bell || (T) type == T::lowShelf
            || (T) type == T::highShelf || (T) type == T::tiltShelf;
    }

public:
    // Public for eqEditorTypeMenuTest (drives the exact string the badge and
    // readout draw) and because the right-click menu's callback uses it too.
    static juce::String badgeText (int type, int slope)
    {
        using T = dsp::ParametricEQ::Type;
        switch ((T) type)
        {
            case T::bell:      return "BELL";
            case T::lowShelf:  return "LS";
            case T::highShelf: return "HS";
            case T::notch:     return "NOTCH";
            case T::bandPass:  return "BP";
            case T::tiltShelf: return "TILT";
            case T::lowCut:
                return "LC " + juce::String (dsp::ParametricEQ::slopeDbPerOct (slope));
            case T::highCut:
                return "HC " + juce::String (dsp::ParametricEQ::slopeDbPerOct (slope));
        }
        return {};
    }

    void showTypeMenu (int b)
    {
        using T = dsp::ParametricEQ::Type;
        const int curType = (int) rawBand (b, params::id::fx::eqband::type);
        const int curSlope = (int) rawBand (b, params::id::fx::eqband::slope);

        juce::PopupMenu menu;
        const struct { const char* name; T type; } types[] = {
            { "Bell", T::bell }, { "Low Shelf", T::lowShelf }, { "High Shelf", T::highShelf },
            { "Low Cut", T::lowCut }, { "High Cut", T::highCut }, { "Notch", T::notch },
            { "Band Pass", T::bandPass }, { "Tilt Shelf", T::tiltShelf },
        };
        for (auto& t : types)
        {
            const int typeIdx = (int) t.type;
            if (t.type == T::lowCut || t.type == T::highCut)
            {
                juce::PopupMenu slopeMenu;
                const char* slopeNames[6] = { "6 dB/oct", "12 dB/oct", "18 dB/oct",
                                               "24 dB/oct", "36 dB/oct", "48 dB/oct" };
                for (int s = 0; s < 6; ++s)
                    slopeMenu.addItem (10000 + typeIdx * 100 + s, slopeNames[s], true,
                                       curType == typeIdx && curSlope == s);
                menu.addSubMenu (t.name, slopeMenu, true, nullptr, curType == typeIdx);
            }
            else
            {
                menu.addItem (typeIdx + 1, t.name, true, curType == typeIdx);
            }
        }

        // Anchor to the NODE, not the whole editor. Bug (Mike, screenshot):
        // withTargetComponent(this) targeted the whole EqEditor's screen
        // bounds; combined with the editor content's scale transform
        // (SPASynthEditor::content->setTransform), the menu could land far
        // from the clicked node (observed: top-left, over the AMP panel).
        // localAreaToGlobal walks the full transform chain explicitly, so
        // build the target rect from the node's own local position rather
        // than relying on a large component's screen bounds.
        showPopupAnchored (*this, menu, juce::PopupMenu::Options().withTargetScreenArea (nodeScreenArea (b)),
            [this, b] (int result)
            {
                if (result <= 0) return;
                if (result >= 10000)
                {
                    const int r = result - 10000;
                    setTypeAndSlope (b, r / 100, r % 100);
                }
                else
                {
                    setTypeAndSlope (b, result - 1, -1);
                }
            });
    }

    // The one code path both the menu and tests use to change a band's type
    // (and, for cuts, its slope) so the two never drift apart.
    void setTypeAndSlope (int b, int type, int slope)
    {
        setBand (b, params::id::fx::eqband::type, (float) type);
        if (slope >= 0)
            setBand (b, params::id::fx::eqband::slope, (float) slope);
        repaint();
    }

    // Test-only: real screen-space centre of band b's node, so a test can
    // synthesize a right-click through the real peer at the exact spot
    // mouseDown()/bandAt() would hit (nodeCentre() itself stays private --
    // production code never needs a node's position from outside).
    juce::Point<float> nodeCentreForTest (int b) const { return nodeCentre (b); }

    // Test-only: the exact screen-space target area the right-click menu is
    // anchored to (see showTypeMenu/nodeScreenArea). Used to prove it
    // contains the node's real global centre at scale 1.0 and under a
    // scale transform on an ancestor.
    juce::Rectangle<int> nodeScreenAreaForTest (int b) const { return nodeScreenArea (b); }

    // Test-only: drive the analyzer's FFT step directly (the 30Hz timer
    // can't be relied on in a headless test) and read back its outputs, so
    // tests can prove the PRE/POST split, the analyzer running while EQ is
    // disabled, and peak-hold decay without any pixel inspection.
    void computeSpectrumForTest() { computeSpectrum(); }
    bool analyzerChangedForTest() { return analyzerChangedSincePaint(); }
    float postDbAtFreqForTest (float freq) const { return logSmoothedDb (spectrum, sampleRate, freq); }
    float preDbAtFreqForTest (float freq) const { return logSmoothedDb (preSpectrum, sampleRate, freq); }
    float peakHoldDbAtFreqForTest (float freq) const { return logSmoothedDb (peakHold, sampleRate, freq); }

private:
    juce::Point<float> nodeCentre (int b) const
    {
        const int type = (int) rawBand (b, params::id::fx::eqband::type);
        const float gain = isGainType (type) ? rawBand (b, params::id::fx::eqband::gain) : 0.0f;
        return { freqToX (rawBand (b, params::id::fx::eqband::freq)),
                 dbToY (juce::jlimit (-dbRange, dbRange, gain)) };
    }

    juce::Rectangle<int> nodeScreenArea (int b) const
    {
        const auto localRect = juce::Rectangle<float> (nodeRadius * 2.0f, nodeRadius * 2.0f)
                                    .withCentre (nodeCentre (b));
        return localAreaToGlobal (localRect.toNearestIntEdges());
    }

    int bandAt (juce::Point<float> p) const
    {
        for (int b = 0; b < numBands; ++b)
        {
            if (! bandEnabled (b)) continue;
            if (nodeCentre (b).getDistanceFrom (p) <= nodeRadius + 4.0f) return b;
        }
        return -1;
    }

    void applyDrag (juce::Point<float> p)
    {
        setBandRaw (dragBand, params::id::fx::eqband::freq,
                    juce::jlimit (minF, maxF, xToFreq (p.x)));
        const int type = (int) rawBand (dragBand, params::id::fx::eqband::type);
        if (isGainType (type))
            setBandRaw (dragBand, params::id::fx::eqband::gain,
                        juce::jlimit (-dbRange, dbRange, yToDb (p.y)));
    }

    // Enable the first free band at the click point with the given type
    // (default Bell), or -1 if all 8 are in use or the click is outside the
    // graph. Cuts/Notch/Band Pass ignore the click's vertical position (no
    // gain); their node sits on the 0 dB line.
    int createBandAt (juce::Point<float> p, int type = 0 /* Bell */)
    {
        if (! graph.contains (p)) return -1;
        for (int i = 0; i < numBands; ++i)
            if (! bandEnabled (i))
            {
                setBand (i, params::id::fx::eqband::type, (float) type);
                setBandRaw (i, params::id::fx::eqband::freq, juce::jlimit (minF, maxF, xToFreq (p.x)));
                if (isGainType (type))
                    setBandRaw (i, params::id::fx::eqband::gain,
                                juce::jlimit (-dbRange, dbRange, yToDb (p.y)));
                setBand (i, params::id::fx::eqband::enable, 1.0f);
                return i;
            }
        return -1;
    }

    void drawGrid (juce::Graphics& g, const Theme& t) const
    {
        g.setColour (t.outline.withAlpha (0.6f));
        for (float f : gridFreqs)
        {
            const float x = freqToX (f);
            g.drawVerticalLine ((int) x, graph.getY(), graph.getBottom());
        }
        for (float db : { 12.0f, 0.0f, -12.0f })
        {
            const float y = dbToY (db);
            g.setColour (t.outline.withAlpha (db == 0.0f ? 0.8f : 0.4f));
            g.drawHorizontalLine ((int) y, graph.getX(), graph.getRight());
            // dB labels along the LEFT edge, clear of the node readout (top
            // area) and the usage hint (top-right).
            g.setColour (t.textSecondary.withAlpha (0.5f));
            g.setFont (juce::Font (juce::FontOptions (9.0f)));
            g.drawText ((db > 0.0f ? "+" : "") + juce::String (juce::roundToInt (db)),
                        juce::Rectangle<float> (graph.getX() + 2.0f, y - 6.0f, 24.0f, 12.0f),
                        juce::Justification::centredLeft);
        }
        // Frequency labels along the bottom -- 20/50/100/200/500/1k/2k/5k/
        // 10k/20k, per Phil's request for clearer grid labels. Skipped where
        // two labels would overlap at this width (adjacent centres < 22px).
        g.setColour (t.textSecondary.withAlpha (0.6f));
        g.setFont (juce::Font (juce::FontOptions (9.0f)));
        float lastX = -1000.0f;
        for (float f : gridFreqs)
        {
            const float x = freqToX (f);
            if (x - lastX < 22.0f) continue;
            lastX = x;
            const juce::String label = f >= 1000.0f
                ? juce::String (f / 1000.0f, f >= 10000.0f ? 0 : 1) + "k"
                : juce::String (juce::roundToInt (f));
            g.drawText (label, juce::Rectangle<float> (x - 14.0f, graph.getBottom() - 11.0f, 28.0f, 10.0f),
                        juce::Justification::centred);
        }
    }

    // One bin's dB smoothed over a 1/6-octave-wide window (+/- 1/12 octave)
    // around targetFreq, so the drawn curve doesn't look jagged at FFT bin
    // resolution -- done per drawn pixel column (a couple hundred), not per
    // FFT bin, so it's cheap even though it's O(window) per column.
    static float logSmoothedDb (const std::array<float, dsp::Telemetry::scopeSize / 2>& spec,
                                 double sampleRate, float targetFreq)
    {
        const float lo = targetFreq * 0.9439f;   // -1/12 octave (2^(-1/12))
        const float hi = targetFreq * 1.0595f;   //  +1/12 octave
        const float binHz = (float) sampleRate / (float) dsp::Telemetry::scopeSize;
        int i0 = juce::jmax (1, (int) (lo / binHz));
        int i1 = juce::jmin ((int) spec.size() - 1, (int) (hi / binHz));
        if (i1 < i0) i1 = i0;
        float sum = 0.0f; int n = 0;
        for (int i = i0; i <= i1; ++i) { sum += spec[(size_t) i]; ++n; }
        return n > 0 ? sum / (float) n : -100.0f;
    }

    void drawSpectrumTrace (juce::Graphics& g, const std::array<float, dsp::Telemetry::scopeSize / 2>& spec,
                            juce::Colour colour, float fillAlpha, float strokeAlpha, bool gradient) const
    {
        juce::Path p;
        bool started = false;
        constexpr int steps = 240;
        for (int i = 0; i <= steps; ++i)
        {
            const float x = graph.getX() + graph.getWidth() * (float) i / steps;
            const float freq = xToFreq (x);
            if (freq < minF || freq > maxF) continue;
            const float db = juce::jlimit (-90.0f, 0.0f, logSmoothedDb (spec, sampleRate, freq));
            const float y = graph.getBottom() - (db + 90.0f) / 90.0f * graph.getHeight();
            if (! started) { p.startNewSubPath (x, y); started = true; }
            else p.lineTo (x, y);
        }
        if (! started) return;

        if (fillAlpha > 0.0f)
        {
            auto fill = p;
            fill.lineTo (graph.getRight(), graph.getBottom());
            fill.lineTo (graph.getX(), graph.getBottom());
            fill.closeSubPath();
            if (gradient)
            {
                juce::ColourGradient grad (colour.withAlpha (fillAlpha), graph.getX(), graph.getY(),
                                            colour.withAlpha (0.0f), graph.getX(), graph.getBottom(), false);
                g.setGradientFill (grad);
            }
            else
            {
                g.setColour (colour.withAlpha (fillAlpha));
            }
            g.fillPath (fill);
        }
        if (strokeAlpha > 0.0f)
        {
            g.setColour (colour.withAlpha (strokeAlpha));
            g.strokePath (p, juce::PathStrokeType (1.2f));
        }
    }

    void drawPeakHold (juce::Graphics& g, const Theme& t) const
    {
        juce::Path p;
        bool started = false;
        constexpr int steps = 240;
        for (int i = 0; i <= steps; ++i)
        {
            const float x = graph.getX() + graph.getWidth() * (float) i / steps;
            const float freq = xToFreq (x);
            if (freq < minF || freq > maxF) continue;
            const float db = juce::jlimit (-90.0f, 0.0f, logSmoothedDb (peakHold, sampleRate, freq));
            const float y = graph.getBottom() - (db + 90.0f) / 90.0f * graph.getHeight();
            if (! started) { p.startNewSubPath (x, y); started = true; } else p.lineTo (x, y);
        }
        if (started)
        {
            g.setColour (t.textPrimary.withAlpha (0.5f));
            g.strokePath (p, juce::PathStrokeType (0.8f));
        }
    }

    void drawSpectrum (juce::Graphics& g, const Theme& t) const
    {
        // PRE: faint, thin, no gradient -- "the dry synth before effects".
        drawSpectrumTrace (g, preSpectrum, t.textSecondary, 0.08f, 0.35f, false);
        // POST: bright, gradient fill under it, on top of PRE.
        drawSpectrumTrace (g, spectrum, t.accentMod, 0.22f, 0.75f, true);
        drawPeakHold (g, t);

        // Legend, bottom-left corner (clear of the freq/dB labels and the
        // top-area readout/hint text).
        g.setFont (juce::Font (juce::FontOptions (9.0f, juce::Font::bold)));
        auto legendR = juce::Rectangle<float> (graph.getX() + 4.0f, graph.getY() + 4.0f, 34.0f, 11.0f);
        g.setColour (t.textSecondary.withAlpha (0.6f));
        g.drawText ("PRE", legendR, juce::Justification::centredLeft);
        g.setColour (t.accentMod.withAlpha (0.85f));
        g.drawText ("POST", legendR.translated (0.0f, 12.0f), juce::Justification::centredLeft);

        // SLOW/MED/FAST speed control, clickable/right-clickable.
        g.setColour (t.textSecondary.withAlpha (0.55f));
        g.setFont (juce::Font (juce::FontOptions (9.0f)));
        g.drawText (speedName (analyzerSpeed), speedLabelRect, juce::Justification::centredRight);
    }

    static const char* speedName (AnalyzerSpeed s)
    {
        switch (s) { case AnalyzerSpeed::slow: return "SLOW"; case AnalyzerSpeed::fast: return "FAST";
                     case AnalyzerSpeed::medium: return "MED"; }
        return "MED";
    }

    void showSpeedMenu()
    {
        juce::PopupMenu menu;
        menu.addItem (1, "Slow", true, analyzerSpeed == AnalyzerSpeed::slow);
        menu.addItem (2, "Medium", true, analyzerSpeed == AnalyzerSpeed::medium);
        menu.addItem (3, "Fast", true, analyzerSpeed == AnalyzerSpeed::fast);
        showPopupAnchored (*this, menu,
            juce::PopupMenu::Options().withTargetScreenArea (localAreaToGlobal (speedLabelRect.toNearestIntEdges())),
            [this] (int result)
            {
                if (result <= 0) return;
                analyzerSpeed = (AnalyzerSpeed) (result - 1);
                apvts.state.setProperty ("uiEqAnalyzerSpeed", (int) analyzerSpeed, nullptr);
                repaint();
            });
    }

    // Release-coefficient pair (POST smoothing, peak-hold fall) per speed.
    // Peak-hold fall is expressed as dB/s, converted to a per-frame (30Hz)
    // subtraction in computeSpectrum().
    struct SpeedCoeffs { float release; float peakFallDbPerSec; };
    static SpeedCoeffs speedCoeffs (AnalyzerSpeed s)
    {
        switch (s)
        {
            case AnalyzerSpeed::slow:   return { 0.93f, 12.0f };
            case AnalyzerSpeed::fast:   return { 0.55f, 20.0f };
            case AnalyzerSpeed::medium: return { 0.85f, 16.0f };
        }
        return { 0.85f, 16.0f };
    }

    // Hann window, computed once (it was 8192 std::cos calls per 30 Hz tick).
    static const std::array<float, dsp::Telemetry::scopeSize>& hannWindow()
    {
        static const auto table = []
        {
            std::array<float, dsp::Telemetry::scopeSize> w {};
            const int n = dsp::Telemetry::scopeSize;
            for (int i = 0; i < n; ++i)
                w[(size_t) i] = 0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi
                                                        * (float) i / (float) (n - 1));
            return w;
        }();
        return table;
    }

    void computeSpectrumInto (const std::array<std::atomic<float>, dsp::Telemetry::scopeSize>& ring,
                              const std::atomic<int>& writeIdx,
                              std::array<float, dsp::Telemetry::scopeSize / 2>& outSpectrum,
                              float release)
    {
        const int size = dsp::Telemetry::scopeSize;
        const int w = writeIdx.load (std::memory_order_acquire);
        for (int i = 0; i < size; ++i)
        {
            const int idx = (w + i) & (size - 1);   // oldest -> newest
            fftData[(size_t) i] = ring[(size_t) idx].load (std::memory_order_relaxed) * hannWindow()[(size_t) i];
        }
        std::fill (fftData.begin() + size, fftData.end(), 0.0f);
        fft.performFrequencyOnlyForwardTransform (fftData.data());

        const int bins = size / 2;
        const float norm = 2.0f / (float) size;
        for (int i = 0; i < bins; ++i)
        {
            const float mag = fftData[(size_t) i] * norm;
            const float db = juce::Decibels::gainToDecibels (mag + 1.0e-9f);
            // Temporal smoothing; fast attack, release set by analyzer speed.
            float& s = outSpectrum[(size_t) i];
            s = db > s ? db : s * release + db * (1.0f - release);
        }
    }

    void computeSpectrum()
    {
        const auto coeffs = speedCoeffs (analyzerSpeed);
        computeSpectrumInto (telemetry.scope, telemetry.scopeWrite, spectrum, coeffs.release);
        computeSpectrumInto (telemetry.preScope, telemetry.preScopeWrite, preSpectrum, coeffs.release);

        // Peak-hold on POST: hold each bin's recent max, falling slowly
        // (dB/s -> per-frame at the 30Hz timer).
        const float fallPerFrame = coeffs.peakFallDbPerSec / 30.0f;
        for (size_t i = 0; i < peakHold.size(); ++i)
        {
            peakHold[i] = juce::jmax (spectrum[i], peakHold[i] - fallPerFrame);
        }
    }

    // True (and records the new baseline) when any bin of the three drawn
    // curves moved by more than ~0.2 dB (a fraction of a pixel) since the last
    // repaint this asked for. The first call always reports a change.
    bool analyzerChangedSincePaint()
    {
        // Band / enable / character parameters too: they can change with no
        // mouse involvement (preset load, undo, automation) and the curve and
        // nodes must follow, which the unconditional 30 Hz repaint used to do.
        std::array<float, paramSigSize> sig {};
        {
            size_t k = 0;
            for (int b = 0; b < numBands; ++b)
                for (const char* key : { params::id::fx::eqband::enable, params::id::fx::eqband::type,
                                         params::id::fx::eqband::slope, params::id::fx::eqband::freq,
                                         params::id::fx::eqband::gain, params::id::fx::eqband::q })
                    sig[k++] = rawBand (b, key);
            sig[k++] = value (params::id::fx::eqEnable);
            sig[k++] = value (params::id::fx::eqCharacter);
        }
        bool changed = ! paintedBaselineValid || sig != paintedParams;
        paintedParams = sig;
        if (! changed)
            for (size_t i = 0; i < spectrum.size(); ++i)
                if (std::abs (spectrum[i] - paintedSpectrum[i]) > 0.2f
                    || std::abs (preSpectrum[i] - paintedPre[i]) > 0.2f
                    || std::abs (peakHold[i] - paintedPeak[i]) > 0.2f)
                {
                    changed = true;
                    break;
                }
        if (changed)
        {
            paintedSpectrum = spectrum;
            paintedPre = preSpectrum;
            paintedPeak = peakHold;
            paintedBaselineValid = true;
        }
        return changed;
    }

    void refreshSampleRate()
    {
        const double sr = getSampleRate ? getSampleRate() : 0.0;
        if (sr > 0.0) sampleRate = sr;
    }

    void openUndoHold()
    {
        if (undoHold == nullptr)
            undoHold = std::make_unique<SPAStripProcessor::UndoStep> (processor, "EQ");
    }
    void closeUndoHold()
    {
        wheelHoldTimer.stopTimer();
        undoHold.reset();
    }
    struct HoldTimer : juce::Timer
    {
        std::function<void()> onTimer;
        void timerCallback() override { stopTimer(); if (onTimer) onTimer(); }
    } wheelHoldTimer;

    SPAStripProcessor& processor;
    juce::AudioProcessorValueTreeState& apvts;
    const dsp::Telemetry& telemetry;
    std::function<double()> getSampleRate;
    std::unique_ptr<SPAStripProcessor::UndoStep> undoHold;
    double sampleRate = 48000.0;

    FxPanelHeader header;
    Choice character;

    juce::Rectangle<float> graph;
    int dragBand = -1, hoverBand = -1, selectedBand = -1;
    bool qDragActive = false;      // Cmd/Ctrl-drag Q gesture in progress
    float qRefY = 0.0f, qRefQ = 1.0f;
    juce::String modName { "Ctrl" };

    // 20/50/100/200/500/1k/2k/5k/10k/20k -- Phil's requested grid labels.
    static constexpr std::array<float, 10> gridFreqs { 20.0f, 50.0f, 100.0f, 200.0f, 500.0f,
                                                        1000.0f, 2000.0f, 5000.0f, 10000.0f, 20000.0f };

    AnalyzerSpeed analyzerSpeed = AnalyzerSpeed::medium;
    juce::Rectangle<float> speedLabelRect;

    juce::dsp::FFT fft { 12 };   // 2^12 = 4096 = scopeSize
    std::array<float, dsp::Telemetry::scopeSize * 2> fftData {};
    std::array<float, dsp::Telemetry::scopeSize / 2> spectrum {};       // POST (smoothed)
    std::array<float, dsp::Telemetry::scopeSize / 2> preSpectrum {};    // PRE  (smoothed)
    std::array<float, dsp::Telemetry::scopeSize / 2> peakHold {};       // POST peak-hold, slow fall
    std::array<float, dsp::Telemetry::scopeSize / 2> paintedSpectrum {}, paintedPre {}, paintedPeak {};
    static constexpr size_t paramSigSize = (size_t) (numBands * 6 + 2);
    std::array<float, paramSigSize> paintedParams {};
    bool paintedBaselineValid = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EqEditor)
};

} // namespace spa::ui
