#include "FxPanel.h"

namespace spa::ui
{

namespace id = params::id;


FXPanel::FXPanel (juce::AudioProcessorValueTreeState& apvts, FXDisplay::Kind kind,
                  params::Section section, const juce::String& title,
                  const juce::StringArray& enableParamIds,
                  const dsp::Telemetry* telemetry)
    : panelTitle (title),
      display (apvts, kind, telemetry),
      controls (apvts, section, title, enableParamIds, false, true)   // dense=true: FXPanel only; enable toggles live in the header
{
    addAndMakeVisible (display);
    addAndMakeVisible (controls);

    if (! enableParamIds.isEmpty())
    {
        std::vector<FxPanelHeader::Part> parts;
        if (enableParamIds.size() == 2)   // TREM/VIB: "Trem / Vib" -> two titled toggles
        {
            auto names = juce::StringArray::fromTokens (title, "/", "");
            names.trim();
            for (int i = 0; i < 2; ++i)
                parts.push_back ({ i < names.size() ? names[i] : title, enableParamIds[i] });
        }
        else
            parts.push_back ({ title, enableParamIds[0] });
        header = std::make_unique<FxPanelHeader> (apvts, std::move (parts));
        addAndMakeVisible (*header);
    }

    // Delay time only means anything free-running; division only means
    // anything synced -- same rule as the LFO panels, wired post-hoc here
    // since SectionPanel auto-builds its grid with no dependency concept.
    if (section == params::Section::fxDelay)
    {
        delayTimeEnable = std::make_unique<DependentEnable> (
            apvts, id::fx::delaySync, [] (float v) { return v < 0.5f; },
            controls.findControlComponents (id::fx::delayTime));
        delayDivisionEnable = std::make_unique<DependentEnable> (
            apvts, id::fx::delaySync, [] (float v) { return v >= 0.5f; },
            controls.findControlComponents (id::fx::delayDivision));
        // WIDTH only acts when ping-pong is on (see FXChain::processDelay).
        delayWidthEnable = std::make_unique<DependentEnable> (
            apvts, id::fx::delayPingPong, [] (float v) { return v >= 0.5f; },
            controls.findControlComponents (id::fx::delayWidth));
    }

    // GRAIN: density only means anything free-running, division only synced.
    if (section == params::Section::fxGrain)
    {
        delayTimeEnable = std::make_unique<DependentEnable> (
            apvts, id::fx::grainSync, [] (float v) { return v < 0.5f; },
            controls.findControlComponents (id::fx::grainDensity));
        delayDivisionEnable = std::make_unique<DependentEnable> (
            apvts, id::fx::grainSync, [] (float v) { return v >= 0.5f; },
            controls.findControlComponents (id::fx::grainDivision));

        auto tip = [this] (const juce::String& paramID, const juce::String& text)
        {
            for (auto* c : controls.findControlComponents (paramID))
                if (auto* t = dynamic_cast<juce::SettableTooltipClient*> (c))
                    t->setTooltip (text);
        };
        tip (id::fx::grainPosition, "How far back in the live input the grains start reading.");
        tip (id::fx::grainSpread, "Random start position, timing and stereo placement per grain.");
        tip (id::fx::grainSpreadPitch, "Random pitch per grain, up to this many semitones either way.");
        tip (id::fx::grainReverse, "How many grains play backwards.");
        tip (id::fx::grainFeedback, "Feeds the grains back into what they read, for clouds that build up.");
        tip (id::fx::grainFreeze, "Stops listening and keeps granulating what it already holds.");
    }

    // House-voice tooltips for the two controls the stereo chorus engine
    // added (1.0.22): the registry-built grid gives them knobs and a combo
    // but no explanation, and WIDTH in particular needs one -- it is the
    // control that stops the chorus imaging as mono.
    if (section == params::Section::fxChorus)
    {
        auto tip = [this] (const juce::String& paramID, const juce::String& text)
        {
            for (auto* c : controls.findControlComponents (paramID))
                if (auto* t = dynamic_cast<juce::SettableTooltipClient*> (c))
                    t->setTooltip (text);
        };
        tip (id::fx::chorusWidth,
             "Spreads the left and right sides apart. At zero both sides move "
             "together and the chorus sits in the middle; turn it up and the "
             "sides sweep against each other for a wide stereo image.");
        tip (id::fx::chorusMode,
             "Vintage is the warm, slightly dark bucket-brigade sound of a "
             "classic 80s polysynth. Modern is clean and digital, with a "
             "longer, deeper sweep.");
    }

    if (! enableParamIds.isEmpty())
        powerTracker = std::make_unique<TabEngagementTracker> (apvts,
            std::vector<std::pair<juce::String, std::vector<juce::String>>> {
                { "on", std::vector<juce::String> (enableParamIds.begin(), enableParamIds.end()) } },
            *this);
}

void FXPanel::paint (juce::Graphics& g)
{
    draw::panel (g, getLocalBounds().toFloat());
    // The title row ("[toggle] TITLE") is the FxPanelHeader child; recess=false
    // semantics preserved: fxTabs already casts the rule + recessed shadow
    // above it, so there is no second header band painted here.
}

void FXPanel::resized()
{
    if (header != nullptr)
        header->setBounds (getLocalBounds().removeFromTop (metrics::sectionHeaderHeight));
    auto area = getLocalBounds().withTrimmedTop (metrics::sectionHeaderHeight).reduced (7, 3);

    // 1.0.25's FX display redesign made the display worth actually seeing
    // (real echo timing, phaser/flanger sweeps, tremolo/vibrato shapes) --
    // but the OLD rule here ("controls always get their full ideal height,
    // the display gets whatever's left, even nothing") starved it: DELAY
    // was a ~30px sliver and MOD/TREM-VIB (many-row sections) were 0x0 at
    // base size, i.e. invisible. The rule now: the display gets first claim
    // on a guaranteed minimum share, and the control grid compresses into
    // what's left.
    //
    // SectionPanel::resized() already guarantees labels never clip on its
    // own (it reserves each label's fixed slice before the knob gets
    // whatever's left in that row -- see its comment), so shrinking the
    // control area only ever costs KNOB size, never a caption. What this
    // function has to protect is that the knob doesn't shrink into
    // uselessness. controlsIdeal is exactly rows*60+8 (SectionPanel::
    // heightForWidth, bare-mode cellHeight 60), so the row count is backed
    // out of it exactly rather than guessed.
    //
    // `controls` is built with dense=true (see the constructor call below):
    // a SectionPanel packing mode, opt-in for FXPanel only, that lays out
    // each control at its own natural pixel width (toggles pair up two-high
    // in one column when consecutive; combos AND knob captions take only
    // the width their own text needs -- a caption must never be narrower
    // than its own text, a hard constraint, so this is a floor, not a
    // choice) instead of a blanket 2-cell grid. That's what actually solves
    // the row-count problem this comment used to describe at length: DIST/
    // CHORUS/DELAY now fit their whole control set in ONE row (was 1/2/2),
    // and MOD/TREM-VIB fit in TWO (was 3) -- measured via controls.
    // heightForWidth() while tuning this. REVERB (9 knobs, the most of any
    // FX section, each with its own full caption) does NOT reach one row --
    // its dense content measures 746px against the 572px available, a
    // genuine content-volume limit rather than a layout inefficiency, so it
    // shares MOD/TREM-VIB's 2-row targets instead; see the 1.0.25 report.
    // With that fixed, the row-height floor below only ever matters for
    // these three 2-row sections in practice; every panel clears its target
    // (display >=70px / knob >=36px for the three single-row panels,
    // display >=48px / knob >=30px for MOD/TREM-VIB/REVERB) with room to
    // spare -- see fxPanelDisplayMinimumHeightTest and the
    // 1.0.25 report for the exact measured numbers.
    constexpr int minRowHeight = 56;      // floor for SectionPanel's own row height
    constexpr int barecellHeight = SectionPanel::bareCellHeight;   // SectionPanel's bare-mode cellHeight (drawFrame=false)
    constexpr int gap = 4;
    constexpr int minDisplayFloor = 48;
    // 0.37 rather than an even 0.38: at MOD/TREM-VIB's real numbers (155px
    // panel, 2 dense rows after the SectionPanel packing fix below) 0.38
    // rounds the leftover control height to an ODD 93px, and 93/2 floors to
    // a 46px row -- 1px short of the >=30px knob-diameter target. 0.37
    // lands on an even 94px, landing exactly on 47px rows / 30px knobs,
    // with the display still comfortably above its own 48px target (57px).
    constexpr float minDisplayFraction = 0.37f;

    const auto controlsIdeal = controls.heightForWidth (area.getWidth());
    const auto rows = juce::jmax (1, (controlsIdeal - 8) / barecellHeight);
    const auto minControlsH = rows * minRowHeight;
    const auto minDisplayH = juce::jmax (minDisplayFloor,
                                         (int) ((float) area.getHeight() * minDisplayFraction));

    auto controlsH = juce::jmin (controlsIdeal, area.getHeight());
    if (area.getHeight() - gap - controlsH < minDisplayH)
    {
        // Not enough room for both the ideal control grid AND the display's
        // minimum -- shrink the grid toward its own floor to free up the
        // difference. If even that floor doesn't leave room for the
        // display's minimum (MOD/TREM-VIB at base size -- see the comment
        // above and fxPanelDisplayMinimumHeightTest), the floor still wins:
        // a usable knob beats a taller picture.
        controlsH = juce::jmax (minControlsH, area.getHeight() - gap - minDisplayH);
        controlsH = juce::jmin (controlsH, area.getHeight());
    }

    controls.setBounds (area.removeFromBottom (controlsH));
    if (area.getHeight() > gap)
    {
        area.removeFromBottom (gap);
        display.setBounds (area);
    }
    else
    {
        display.setBounds ({});
    }
}


} // namespace spa::ui
