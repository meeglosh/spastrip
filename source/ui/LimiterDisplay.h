#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Theme.h"
#include "Controls.h"
#include "FxPanelHeader.h"
#include "../dsp/Telemetry.h"
#include "../params/ParameterRegistry.h"
#include <cmath>

namespace spa::ui
{

// Scrolling limiter meter, a simplified Pro-L-style view: a centred output-level
// waveform with the gain reduction painted as an amber region descending from
// the top, both scrolling right to left in real time from the Telemetry ring.
class LimiterDisplay : public juce::Component,
                       private juce::Timer
{
public:
    explicit LimiterDisplay (const dsp::Telemetry& tel) : telemetry (tel)
    {
        setInterceptsMouseClicks (false, false);
        startTimerHz (30);
    }

    ~LimiterDisplay() override { stopTimer(); }

    int getRepaintRequestsForTest() const { return repaintRequests; }

    void paint (juce::Graphics& g) override
    {
        const auto& t = currentTheme();
        const auto area = getLocalBounds().toFloat().reduced (1.0f);

        // Faceplate restyle: no display-well fill/border — the meter traces
        // draw straight on the faceplate surface.
        const float cy = area.getCentreY();
        const float halfH = area.getHeight() * 0.46f;

        // Reference grid: centre line + gain-reduction ticks from the top.
        g.setColour (t.outline.withAlpha (0.5f));
        g.drawHorizontalLine ((int) cy, area.getX(), area.getRight());
        for (int db = 3; db <= 12; db += 3)
        {
            const float y = area.getY() + (float) db / grRange * area.getHeight() * 0.5f;
            g.setColour (t.outline.withAlpha (0.25f));
            g.drawHorizontalLine ((int) y, area.getX(), area.getRight());
        }

        const int N = dsp::Telemetry::limiterHistory;
        const int show = juce::jmin (N - 1, 480);
        const int wr = telemetry.limWrite.load (std::memory_order_acquire);
        const auto frameAt = [&] (int c)
        {
            int idx = (wr - show + c) % N;
            if (idx < 0) idx += N;
            return idx;
        };
        const auto xOf = [&] (int c)
        {
            return area.getX() + area.getWidth() * (float) c / (float) (show - 1);
        };

        // Output-level waveform (centred, filled).
        juce::Path top, bot;
        for (int c = 0; c < show; ++c)
        {
            const float lvl = juce::jlimit (0.0f, 1.0f,
                                            telemetry.limOut[(size_t) frameAt (c)].load (std::memory_order_relaxed));
            const float x = xOf (c);
            const float yt = cy - lvl * halfH, yb = cy + lvl * halfH;
            if (c == 0) { top.startNewSubPath (x, yt); bot.startNewSubPath (x, yb); }
            else        { top.lineTo (x, yt);          bot.lineTo (x, yb); }
        }
        juce::Path fill = top;
        for (int c = show - 1; c >= 0; --c)
        {
            const float lvl = juce::jlimit (0.0f, 1.0f,
                                            telemetry.limOut[(size_t) frameAt (c)].load (std::memory_order_relaxed));
            fill.lineTo (xOf (c), cy + lvl * halfH);
        }
        fill.closeSubPath();
        g.setColour (t.accentMod.withAlpha (0.22f));
        g.fillPath (fill);
        g.setColour (t.accentMod);
        g.strokePath (top, juce::PathStrokeType (1.2f));
        g.strokePath (bot, juce::PathStrokeType (1.2f));

        // Gain reduction: amber region from the top, depth proportional to GR.
        const juce::Colour amber (0xffe0a83a);
        juce::Path gr;
        gr.startNewSubPath (area.getX(), area.getY());
        float lastGr = 0.0f;
        for (int c = 0; c < show; ++c)
        {
            const float grDb = telemetry.limGrDb[(size_t) frameAt (c)].load (std::memory_order_relaxed);
            lastGr = grDb;
            const float amt = juce::jlimit (0.0f, 1.0f, -grDb / grRange);
            gr.lineTo (xOf (c), area.getY() + amt * area.getHeight() * 0.5f);
        }
        gr.lineTo (area.getRight(), area.getY());
        gr.closeSubPath();
        g.setColour (amber.withAlpha (0.45f));
        g.fillPath (gr);

        // Current GR readout.
        g.setColour (amber);
        g.setFont (juce::Font (juce::FontOptions (11.0f)));
        g.drawText (lastGr < -0.05f ? "GR " + juce::String (lastGr, 1) + " dB" : "GR 0.0 dB",
                    area.reduced (7.0f, 4.0f).removeFromTop (14.0f),
                    juce::Justification::topLeft);
    }

    // Repaint only while showing, only when the ring has advanced, and not at
    // all while the trace is flat (limiter off and silent): an idle plugin
    // must not repaint 30 times a second.
    void timerCallback() override
    {
        if (! isLiveShowing (*this)) return;
        const int wr = telemetry.limWrite.load (std::memory_order_acquire);
        if (wr == lastWrite) return;
        lastWrite = wr;
        const int N = dsp::Telemetry::limiterHistory;
        bool active = false;
        for (int c = 0; c < 480 && ! active; ++c)
        {
            const int idx = ((wr - 1 - c) % N + N) % N;
            active = telemetry.limOut[(size_t) idx].load (std::memory_order_relaxed) > 1.0e-4f
                  || telemetry.limGrDb[(size_t) idx].load (std::memory_order_relaxed) < -0.005f;
        }
        if (! active && ! wasActive) return;
        wasActive = active;
        ++repaintRequests;
        repaint();
    }

private:
    static constexpr float grRange = 18.0f;   // dB shown from the top to the centre

    const dsp::Telemetry& telemetry;
    int lastWrite = -1;
    int repaintRequests = 0;
    bool wasActive = true;   // paint the first frame

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LimiterDisplay)
};

//==============================================================================
namespace limiter
{
    // One shared vertical dB scale for the twin slider and the two meters.
    inline constexpr float scaleMinDb = -24.0f;
    inline constexpr int trackPad = 8;   // room for a handle at either end of the track

    // THRESHOLD is the stored DRIVE parameter shown the other way up:
    // threshold_dB = -drive_dB. The DSP multiplies the input by 10^(drive/20)
    // before the brickwall at the ceiling, so lowering the threshold by 1 dB is
    // exactly +1 dB of drive. Param id, range and automation are unchanged.
    inline float thresholdFromDrive (float driveDb) { return -driveDb; }
    inline float driveFromThreshold (float thresholdDb) { return -thresholdDb; }

    inline float dbToY (float db, int height)
    {
        const float span = (float) (height - 2 * trackPad);
        return (float) trackPad + juce::jlimit (0.0f, 1.0f, -db / -scaleMinDb) * span;
    }
    inline float yToDb (float y, int height)
    {
        const float span = (float) (height - 2 * trackPad);
        return -juce::jlimit (0.0f, 1.0f, (y - (float) trackPad) / span) * -scaleMinDb;
    }
    inline juce::String dbText (float db)
    {
        if (std::abs (db) < 0.05f) db = 0.0f;   // never "-0.0"
        return juce::String (db, 1) + " dB";
    }
}

// Vertical peak meter on the shared scale (linear peak in, easing fall).
class LimiterLevelMeter final : public juce::Component, private juce::Timer
{
public:
    explicit LimiterLevelMeter (const std::atomic<float>& source) : peak (source)
    {
        setInterceptsMouseClicks (false, false);
        startTimerHz (30);
    }
    ~LimiterLevelMeter() override { stopTimer(); }

    void paint (juce::Graphics& g) override
    {
        const auto& t = currentTheme();
        auto lane = getLocalBounds().toFloat().reduced (3.0f, (float) limiter::trackPad);
        g.setColour (t.meterLane);
        g.fillRoundedRectangle (lane, 2.0f);
        const float db = juce::Decibels::gainToDecibels (shown, limiter::scaleMinDb);
        const float frac = juce::jlimit (0.0f, 1.0f, (db - limiter::scaleMinDb) / -limiter::scaleMinDb);
        if (frac > 0.0f)
        {
            g.setColour (db > -0.1f ? t.meterRed : (db > -6.0f ? t.meterYellow : t.meterGreen));
            g.fillRoundedRectangle (lane.withTop (lane.getBottom() - lane.getHeight() * frac), 2.0f);
        }
    }

    float getShownForTest() const { return shown; }

private:
    void timerCallback() override
    {
        if (! isLiveShowing (*this)) return;
        const float before = shown;
        const float target = peak.load (std::memory_order_relaxed);
        shown = target > shown ? target : juce::jmax (target, shown * 0.86f);
        if (shown < 1.0e-4f) shown = 0.0f;
        if (std::abs (shown - before) > 1.0e-4f)
            repaint();
    }

    const std::atomic<float>& peak;
    float shown = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LimiterLevelMeter)
};

// Ozone-Maximizer-style twin slider: left handle THRESHOLD, right handle
// CEILING on one vertical dB scale, between the IN and OUT meters. With LINK
// on, dragging either handle moves both by the same number of dB.
class LimiterTwinSlider final : public juce::Component
{
public:
    LimiterTwinSlider (juce::AudioProcessorValueTreeState& state, const dsp::Telemetry& tel)
        : apvts (state),
          inMeter (tel.limInPeak), outMeter (tel.limOutPeak),
          threshold (*this, true, state.getParameter (params::id::fx::limDrive)),
          ceiling (*this, false, state.getParameter (params::id::fx::limCeiling))
    {
        setWantsKeyboardFocus (false);
        addAndMakeVisible (inMeter);
        addAndMakeVisible (outMeter);
        addAndMakeVisible (threshold);
        addAndMakeVisible (ceiling);

        linked = (bool) apvts.state.getProperty ("uiLimiterLink", false);
        link.setTooltip ("Link THRESHOLD and CEILING: move both together");
        link.setOn (linked);
        link.onClick = [this]
        {
            linked = link.isOn();
            apvts.state.setProperty ("uiLimiterLink", linked, nullptr);   // UI state: not a parameter
            repaint();
        };
        addAndMakeVisible (link);
    }

    void paint (juce::Graphics& g) override
    {
        const auto& t = currentTheme();
        g.setFont (metrics::smallFont());

        // Titles over each half; IN / OUT under the meters.
        g.setColour (t.textSecondary);
        g.drawText ("THRESHOLD", getLocalBounds().removeFromTop (captionH).withWidth (getWidth() / 2 + 6),
                    juce::Justification::centredLeft);
        g.drawText ("CEILING", getLocalBounds().removeFromTop (captionH).withLeft (getWidth() / 2 - 6),
                    juce::Justification::centredRight);

        // Divider lines run from each label to the link icon, tinted with its state.
        {
            const auto textW = [] (const juce::String& str)
            {
                juce::GlyphArrangement ga;
                ga.addLineOfText (metrics::smallFont(), str, 0.0f, 0.0f);
                return (int) std::ceil (ga.getBoundingBox (0, -1, true).getWidth());
            };
            const int y = captionH / 2;
            const int gap = 5;
            const auto ib = link.getBounds();
            const int iconL = ib.getCentreX() - 7, iconR = ib.getCentreX() + 7;
            const int thrEnd = textW ("THRESHOLD") + gap;
            const int ceilStart = getWidth() - textW ("CEILING") - gap;
            g.setColour (link.isOn() ? t.accent : t.textSecondary.withAlpha (0.45f));
            if (iconL - gap > thrEnd)    g.fillRect (thrEnd, y, iconL - gap - thrEnd, 1);
            if (ceilStart > iconR + gap) g.fillRect (iconR + gap, y, ceilStart - iconR - gap, 1);
        }

        // Scale: faint rules across the track and labels in the centre gutter.
        const auto track = trackBounds();
        for (int db = 0; db >= (int) limiter::scaleMinDb; db -= 6)
        {
            const float y = (float) track.getY() + limiter::dbToY ((float) db, track.getHeight());
            g.setColour (t.outline.withAlpha (db == 0 ? 0.55f : 0.3f));
            g.drawHorizontalLine ((int) y, (float) threshold.getX(), (float) (ceiling.getRight()));
            g.setColour (t.textSecondary);
            g.drawText (juce::String (db), gutterBounds().withY ((int) y - 6).withHeight (12),
                        juce::Justification::centred);
        }

        // Readouts under their handles' lanes.
        g.setColour (t.accent);
        g.setFont (metrics::labelFont());
        g.drawText (limiter::dbText (threshold.getDb()), readoutFor (threshold), juce::Justification::centred);
        g.setColour (t.accentMod);
        g.drawText (limiter::dbText (ceiling.getDb()), readoutFor (ceiling), juce::Justification::centred);
        g.setColour (t.textSecondary);
        g.setFont (metrics::smallFont());
        g.drawText ("IN", readoutBounds().withX (inMeter.getX() - 4).withWidth (inMeter.getWidth() + 8), juce::Justification::centred);
        g.drawText ("OUT", readoutBounds().withX (outMeter.getX() - 4).withWidth (outMeter.getWidth() + 8), juce::Justification::centred);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        r.removeFromTop (captionH);
        link.setBounds (juce::Rectangle<int> (getWidth() / 2 - 15, 0, 30, captionH));
        r.removeFromBottom (readoutH);

        const int meterW = 22, gutterW = 28;
        const int laneW = (r.getWidth() - 2 * meterW - gutterW) / 2;
        inMeter.setBounds (r.removeFromLeft (meterW));
        threshold.setBounds (r.removeFromLeft (laneW));
        gutter = r.removeFromLeft (gutterW);
        ceiling.setBounds (r.removeFromLeft (laneW));
        outMeter.setBounds (r.removeFromLeft (meterW));
    }

    // Test hooks.
    juce::Component& getThresholdLane() { return threshold; }
    juce::Component& getCeilingLane() { return ceiling; }
    juce::Component& getLinkToggle() { return link; }
    bool getLinkIsOn() const { return link.isOn(); }
    bool isLinked() const { return linked; }
    float getThresholdDb() const { return threshold.getDb(); }
    float getCeilingDb() const { return ceiling.getDb(); }
    static float handleY (const juce::Component& lane, float db) { return limiter::dbToY (db, lane.getHeight()); }
    const LimiterLevelMeter& getInMeter() const { return inMeter; }
    const LimiterLevelMeter& getOutMeter() const { return outMeter; }

private:
    static constexpr int captionH = 16, readoutH = 16;

    juce::Rectangle<int> trackBounds() const { return threshold.getBounds(); }
    juce::Rectangle<int> gutterBounds() const { return gutter; }
    juce::Rectangle<int> readoutBounds() const
    {
        return { 0, threshold.getBottom() + 1, getWidth(), readoutH };
    }
    juce::Rectangle<int> readoutFor (const juce::Component& lane) const
    {
        return readoutBounds().withX (lane.getX() - 4).withWidth (lane.getWidth() + 8);
    }

    //------------------------------------------------------------------------------
    class Lane final : public juce::Component, public juce::SettableTooltipClient
    {
    public:
        Lane (LimiterTwinSlider& o, bool isThreshold, juce::RangedAudioParameter* p)
            : owner (o), thr (isThreshold), param (p),
              attachment (*p, [this] (float) { repaint(); owner.repaint (owner.readoutFor (*this)); })
        {
            setWantsKeyboardFocus (false);
            setMouseClickGrabsKeyboardFocus (false);
            setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
            getProperties().set ("paramID", p->getParameterID());   // MIDI Learn
            setTooltip (thr ? "THRESHOLD: drag down to drive harder into the ceiling.  Double-click to reset."
                            : "CEILING: the level nothing passes.  Double-click to reset.");
            attachment.sendInitialUpdate();
        }

        float minDb() const { return thr ? -param->convertFrom0to1 (1.0f) : param->convertFrom0to1 (0.0f); }
        float getDb() const
        {
            const float v = param->convertFrom0to1 (param->getValue());
            return thr ? limiter::thresholdFromDrive (v) : v;
        }
        void setDb (float db)
        {
            const float v = thr ? limiter::driveFromThreshold (db) : db;
            param->setValueNotifyingHost (param->convertTo0to1 (v));
        }
        void beginGesture() { param->beginChangeGesture(); }
        void endGesture() { param->endChangeGesture(); }
        void reset() { param->setValueNotifyingHost (param->getDefaultValue()); }

        void paint (juce::Graphics& g) override
        {
            const auto& t = currentTheme();
            const auto col = thr ? t.accent : t.accentMod;
            const float cx = (float) getWidth() * 0.5f;
            const float y = limiter::dbToY (getDb(), getHeight());
            const float top = (float) limiter::trackPad, bottom = (float) (getHeight() - limiter::trackPad);

            g.setColour (t.meterLane);
            g.fillRoundedRectangle (cx - 2.0f, top, 4.0f, bottom - top, 2.0f);
            g.setColour (col.withAlpha (0.55f));
            g.fillRoundedRectangle (cx - 2.0f, top, 4.0f, juce::jmax (0.0f, y - top), 2.0f);

            // Handle: a pill with a grip line and a notch pointing at its lane.
            const juce::Rectangle<float> h (cx - 15.0f, y - 6.0f, 30.0f, 12.0f);
            g.setColour (dragging ? col.brighter (0.3f) : col);
            g.fillRoundedRectangle (h, 3.0f);
            g.setColour (t.background.withAlpha (0.7f));
            g.drawHorizontalLine ((int) y, h.getX() + 6.0f, h.getRight() - 6.0f);
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            if (! e.mods.isLeftButtonDown() || e.mods.isPopupMenu()) return;
            const float hy = limiter::dbToY (getDb(), getHeight());
            grabOffset = std::abs ((float) e.y - hy) <= 8.0f ? hy - (float) e.y : 0.0f;
            dragging = true;
            owner.beginDrag (*this);
            owner.dragTo (*this, limiter::yToDb ((float) e.y + grabOffset, getHeight()));
            repaint();
        }
        void mouseDrag (const juce::MouseEvent& e) override
        {
            if (! dragging) return;
            owner.dragTo (*this, limiter::yToDb ((float) e.y + grabOffset, getHeight()));
        }
        void mouseUp (const juce::MouseEvent&) override
        {
            if (! dragging) return;
            dragging = false;
            owner.endDrag (*this);
            repaint();
        }
        void mouseDoubleClick (const juce::MouseEvent& e) override
        {
            if (! e.mods.isLeftButtonDown()) return;
            beginGesture(); reset(); endGesture();
        }

    private:
        LimiterTwinSlider& owner;
        bool thr;
        juce::RangedAudioParameter* param;
        juce::ParameterAttachment attachment;
        float grabOffset = 0.0f;
        bool dragging = false;
    };

    void beginDrag (Lane& lane)
    {
        startThreshold = threshold.getDb();
        startCeiling = ceiling.getDb();
        dragBoth = linked;
        lane.beginGesture();
        if (dragBoth) (&lane == &threshold ? ceiling : threshold).beginGesture();
    }
    void dragTo (Lane& lane, float db)
    {
        db = juce::jlimit (lane.minDb(), 0.0f, db);
        if (! linked)
        {
            lane.setDb (db);
            return;
        }
        // Same dB step for both, clamped so neither leaves its own range.
        const float start = &lane == &threshold ? startThreshold : startCeiling;
        const float lo = juce::jmax (threshold.minDb() - startThreshold, ceiling.minDb() - startCeiling);
        const float hi = juce::jmin (0.0f - startThreshold, 0.0f - startCeiling);
        const float d = juce::jlimit (lo, hi, db - start);
        threshold.setDb (startThreshold + d);
        ceiling.setDb (startCeiling + d);
    }
    void endDrag (Lane& lane)
    {
        lane.endGesture();
        if (dragBoth) (&lane == &threshold ? ceiling : threshold).endGesture();
        dragBoth = false;
    }

    juce::AudioProcessorValueTreeState& apvts;
    LimiterLevelMeter inMeter, outMeter;
    Lane threshold, ceiling;
    // Chain-link icon on the caption row; click toggles LINK.
    class LinkIcon final : public juce::Component, public juce::SettableTooltipClient
    {
    public:
        LinkIcon()
        {
            setWantsKeyboardFocus (false);
            setMouseClickGrabsKeyboardFocus (false);
            setMouseCursor (juce::MouseCursor::PointingHandCursor);
        }
        bool isOn() const { return on; }
        void setOn (bool v) { on = v; repaint(); }
        std::function<void()> onClick;

        void paint (juce::Graphics& g) override
        {
            const auto& t = currentTheme();
            g.setColour (on ? t.accent : t.textSecondary.withAlpha (0.45f));
            const auto c = getLocalBounds().toFloat().getCentre();
            const float lw = 8.5f, lh = 5.5f, off = 2.6f;
            juce::Path p;
            p.addRoundedRectangle (c.x - off - lw * 0.5f, c.y - lh * 0.5f, lw, lh, lh * 0.5f);
            p.addRoundedRectangle (c.x + off - lw * 0.5f, c.y - lh * 0.5f, lw, lh, lh * 0.5f);
            g.strokePath (p, juce::PathStrokeType (1.3f));
        }
        void mouseUp (const juce::MouseEvent& e) override
        {
            if (contains (e.getPosition()) && ! e.mods.isPopupMenu())
            {
                on = ! on;
                repaint();
                if (onClick) onClick();
            }
        }
    private:
        bool on = false;
    } link;
    juce::Rectangle<int> gutter;
    bool linked = false, dragBoth = false;
    float startThreshold = 0.0f, startCeiling = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LimiterTwinSlider)
};

// Limiter tab: the scrolling meter (main display) with a control strip under
// it, and the THRESHOLD | CEILING twin slider between the IN and OUT meters.
class LimiterPanel : public juce::Component
{
public:
    LimiterPanel (juce::AudioProcessorValueTreeState& apvts, const dsp::Telemetry& tel)
        : display (tel),
          header  (apvts, { { "Limiter", params::id::fx::limEnable } }),
          maximizer (apvts, tel),
          release (apvts, params::id::fx::limRelease, "Release"),
          link    (apvts, params::id::fx::limStereoLink, "Stereo Link"),
          character (apvts, params::id::fx::limCharacter),
          autoRel (apvts, params::id::fx::limAutoRelease, "AUTO REL"),
          truePeak (apvts, params::id::fx::limTruePeak, "TRUE PK"),
          lookahead (apvts, params::id::fx::limLookahead, "LOOK AHEAD"),
          autoGain (apvts, params::id::fx::limAutoGain, "AUTO GAIN")
    {
        autoGain.button.setTooltip ("Compensate the drive at the output, so the threshold "
                                    "controls how hard it limits without raising the level");
        addAndMakeVisible (header);
        addAndMakeVisible (display);
        addAndMakeVisible (maximizer);
        for (auto* c : std::initializer_list<juce::Component*> {
                 &release, &link, &character, &autoRel, &truePeak, &lookahead, &autoGain })
            addAndMakeVisible (*c);
    }

    // Faceplate restyle: FX-chain tab content, no card fill.
    void paint (juce::Graphics&) override {}

    void resized() override
    {
        header.setBounds (getLocalBounds().removeFromTop (metrics::sectionHeaderHeight));
        auto r = getLocalBounds().withTrimmedTop (metrics::sectionHeaderHeight).reduced (7, 3);

        maximizer.setBounds (r.removeFromRight (juce::jlimit (176, 210, r.getWidth() / 3)));
        r.removeFromRight (8);

        // Compact control strip along the bottom of the left column; the
        // scrolling meter takes the rest.
        auto strip = r.removeFromBottom (58);
        display.setBounds (r.reduced (0, 2));

        for (auto* k : { &release, &link })
        {
            k->setBounds (strip.removeFromLeft (68));   // "STEREO LINK" needs ~55 px of caption + the Label's 10 px border
            strip.removeFromLeft (2);
        }
        strip.removeFromLeft (14);
        // 2 x 2 toggle grid, then the character selector, packed beside the knobs.
        auto toggles = strip.removeFromLeft (230);
        auto tTop = toggles.removeFromTop (toggles.getHeight() / 2);
        autoRel.setBounds  (tTop.removeFromLeft (tTop.getWidth() / 2).reduced (2, 1));
        truePeak.setBounds (tTop.reduced (2, 1));
        lookahead.setBounds (toggles.removeFromLeft (toggles.getWidth() / 2).reduced (2, 1));
        strip.removeFromLeft (14);
        character.setBounds (strip.removeFromLeft (110).reduced (2, 18));
        autoGain.setBounds (toggles.reduced (2, 1));
    }

    // Test hooks.
    LimiterTwinSlider& getMaximizer() { return maximizer; }
    LimiterDisplay& getDisplay() { return display; }
    Knob& getReleaseKnob() { return release; }
    Knob& getLinkKnob() { return link; }
    Choice& getCharacter() { return character; }
    Toggle& getAutoRel() { return autoRel; }
    Toggle& getTruePeak() { return truePeak; }
    Toggle& getLookahead() { return lookahead; }
    Toggle& getAutoGain() { return autoGain; }

private:
    LimiterDisplay display;
    FxPanelHeader header;
    LimiterTwinSlider maximizer;
    Knob release, link;
    Choice character;
    Toggle autoRel, truePeak, lookahead, autoGain;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LimiterPanel)
};

} // namespace spa::ui
