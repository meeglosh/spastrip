#include "StripPanels.h"

namespace spa::ui
{

//==============================================================================
namespace modmenu
{
    juce::String groupName (const mod::ModTarget& t)
    {
        return t.owner.startsWith ("FX ") ? t.owner.substring (3) : t.owner;
    }

    juce::String shortName (const juce::String& parameterID)
    {
        for (const auto& t : SPAStripProcessor::getModTargets())
            if (t.id == parameterID)
                return t.displayName;
        return {};
    }

    juce::PopupMenu build (const juce::String& currentTargetID)
    {
        juce::PopupMenu menu;
        menu.addItem (1, "None", true, currentTargetID.isEmpty());
        menu.addSeparator();

        // One submenu per effect, in registry order (first appearance).
        const auto& targets = SPAStripProcessor::getModTargets();
        juce::StringArray groups;
        for (const auto& t : targets)
            groups.addIfNotAlreadyThere (groupName (t));
        for (const auto& g : groups)
        {
            juce::PopupMenu sub;
            bool hasCurrent = false;
            for (size_t i = 0; i < targets.size(); ++i)
                if (groupName (targets[i]) == g)
                {
                    const bool isCurrent = targets[i].id == currentTargetID;
                    hasCurrent = hasCurrent || isCurrent;
                    sub.addItem ((int) i + 2, targets[i].displayName, true, isCurrent);
                }
            menu.addSubMenu (g, sub, true, nullptr, hasCurrent);
        }
        return menu;
    }

    juce::String resultToTarget (int result, bool& valid)
    {
        const auto& targets = SPAStripProcessor::getModTargets();
        valid = result == 1 || juce::isPositiveAndBelow (result - 2, (int) targets.size());
        if (! valid || result == 1)
            return {};
        return targets[(size_t) (result - 2)].id;
    }
}

//==============================================================================
LevelMeter::LevelMeter (juce::String cap, PeakFn fn) : caption (std::move (cap)), peaks (std::move (fn))
{
    startTimerHz (30);
}

LevelMeter::~LevelMeter() { stopTimer(); }

void LevelMeter::mouseDown (const juce::MouseEvent&)
{
    clipped = { false, false };
    hold = {};
    repaint();
}

void LevelMeter::timerCallback()
{
    if (! isLiveShowing (*this))
        return;
    float l = 0.0f, r = 0.0f;
    if (peaks)
        peaks (l, r);
    const float in[2] { l, r };
    for (size_t c = 0; c < 2; ++c)
    {
        // Instant attack, fast fall (0.82 per tick, as SPASynth's header meters).
        level[c] = juce::jmax (in[c], level[c] * 0.82f);
        if (level[c] < 1.0e-5f)
            level[c] = 0.0f;
        if (in[c] >= hold[c])
        {
            hold[c] = in[c];
            holdAge[c] = 0;
        }
        else if (++holdAge[c] > 45)   // ~1.5 s, then it falls with the bar
            hold[c] = juce::jmax (level[c], hold[c] * 0.93f);
        if (in[c] >= 1.0f)
            clipped[c] = true;
    }
    // Repaint only when a drawn value moved by about a pixel.
    bool changed = false;
    for (size_t c = 0; c < 2; ++c)
        changed = changed
               || std::abs (dbToFraction (juce::Decibels::gainToDecibels (level[c], -90.0f))
                            - dbToFraction (juce::Decibels::gainToDecibels (shownLevel[c], -90.0f))) > 0.004f
               || std::abs (dbToFraction (juce::Decibels::gainToDecibels (hold[c], -90.0f))
                            - dbToFraction (juce::Decibels::gainToDecibels (shownHold[c], -90.0f))) > 0.004f;
    if (changed)
    {
        shownLevel = level;
        shownHold = hold;
        repaint();
    }
}

void LevelMeter::paint (juce::Graphics& g)
{
    const auto& t = currentTheme();
    auto area = getLocalBounds().toFloat();

    g.setColour (t.textSecondary);
    g.setFont (metrics::smallFontBold());
    auto captionArea = area.removeFromLeft (30.0f);
    g.drawText (caption, captionArea, juce::Justification::centredLeft);

    if (showScale)
        area.removeFromBottom (11.0f);
    const float laneH = juce::jmin (9.0f, (area.getHeight() - 3.0f) * 0.5f);
    const float totalH = laneH * 2.0f + 3.0f;
    area = area.withSizeKeepingCentre (area.getWidth(), totalH);

    for (size_t c = 0; c < 2; ++c)
    {
        auto lane = juce::Rectangle<float> (area.getX(), area.getY() + (float) c * (laneH + 3.0f), area.getWidth(), laneH);
        g.setColour (t.meterLane);
        g.fillRoundedRectangle (lane, 2.0f);

        const float fill = dbToFraction (juce::Decibels::gainToDecibels (shownLevel[c], -90.0f));
        auto bar = lane.withWidth (lane.getWidth() * fill);
        if (bar.getWidth() > 0.5f)
        {
            juce::Graphics::ScopedSaveState save (g);
            g.reduceClipRegion (bar.getSmallestIntegerContainer());
            const auto zone = [&] (float db0, float db1, juce::Colour col)
            {
                const float x0 = lane.getX() + lane.getWidth() * dbToFraction (db0);
                const float x1 = lane.getX() + lane.getWidth() * dbToFraction (db1);
                g.setColour (col);
                g.fillRect (juce::Rectangle<float> (x0, lane.getY(), x1 - x0, lane.getHeight()));
            };
            zone (-60.0f, -12.0f, t.meterGreen);
            zone (-12.0f, -3.0f, t.meterYellow);
            zone (-3.0f, 0.0f, t.meterRed);
        }

        const float hx = lane.getX() + lane.getWidth() * dbToFraction (juce::Decibels::gainToDecibels (shownHold[c], -90.0f));
        if (shownHold[c] > 1.0e-4f)
        {
            g.setColour (t.textPrimary.withAlpha (0.85f));
            g.fillRect (juce::Rectangle<float> (hx - 1.0f, lane.getY(), 1.5f, lane.getHeight()));
        }
        if (clipped[c])
        {
            g.setColour (t.meterHot);
            g.fillRoundedRectangle (juce::Rectangle<float> (lane.getRight() - 4.0f, lane.getY(), 4.0f, lane.getHeight()), 1.5f);
        }
    }

    if (showScale)
    {
        g.setColour (t.textSecondary.withAlpha (0.8f));
        g.setFont (metrics::smallFont());
        const float scaleY = area.getBottom() + 1.0f;
        const auto& f = g.getCurrentFont();
        for (const float db : { -48.0f, -36.0f, -24.0f, -12.0f, 0.0f })
        {
            const float x = area.getX() + area.getWidth() * dbToFraction (db);
            const auto label = juce::String ((int) db);
            const float w = juce::GlyphArrangement::getStringWidth (f, label) + 2.0f;
            // 0 dBFS is the right end of the bar: right-align its label there so it
            // never collides with -12; the rest are centred on their true position.
            const float left = db >= 0.0f ? area.getRight() - w : x - w * 0.5f;
            g.drawText (label, juce::Rectangle<float> (left, scaleY, w, 11.0f), juce::Justification::centred);
        }
    }
}

//==============================================================================
EnvelopeDisplay::EnvelopeDisplay (SPAStripProcessor& p) : processor (p)
{
    setInterceptsMouseClicks (false, false);
    startTimerHz (30);
}

EnvelopeDisplay::~EnvelopeDisplay() { stopTimer(); }

bool EnvelopeDisplay::shouldShowNoSidechainNotice() const
{
    const bool external = processor.getAPVTS().getRawParameterValue (params::id::sc::source)->load() < 0.5f;
    return external && ! processor.getTelemetry().scPresent.load (std::memory_order_relaxed);
}

void EnvelopeDisplay::timerCallback()
{
    if (! isLiveShowing (*this))
        return;
    const float env = juce::jlimit (0.0f, 1.0f, processor.getTelemetry().scEnvelope.load (std::memory_order_relaxed));
    shownEnv = env;
    history[(size_t) head] = env;
    head = (head + 1) % historySize;
    repaint();
}

void EnvelopeDisplay::paint (juce::Graphics& g)
{
    const auto& t = currentTheme();
    auto area = getLocalBounds().toFloat().reduced (1.0f);

    // Recessed field.
    g.setColour (t.display);
    g.fillRoundedRectangle (area, 4.0f);
    g.setColour (t.outline);
    g.drawRoundedRectangle (area, 4.0f, 1.0f);
    area.reduce (4.0f, 4.0f);

    const bool notice = shouldShowNoSidechainNotice();
    const float barW = 10.0f;
    auto bar = area.removeFromRight (barW);
    area.removeFromRight (5.0f);

    // History curve.
    juce::Path curve;
    for (int i = 0; i < historySize; ++i)
    {
        const float v = history[(size_t) ((head + i) % historySize)];
        const float x = area.getX() + area.getWidth() * (float) i / (float) (historySize - 1);
        const float y = area.getBottom() - v * area.getHeight();
        if (i == 0) curve.startNewSubPath (x, y); else curve.lineTo (x, y);
    }
    auto fill = curve;
    fill.lineTo (area.getRight(), area.getBottom());
    fill.lineTo (area.getX(), area.getBottom());
    fill.closeSubPath();
    const auto colour = notice ? t.textSecondary.withAlpha (0.4f) : t.accentMod;
    g.setColour (colour.withAlpha (0.16f));
    g.fillPath (fill);
    draw::glowStroke (g, curve, colour, 1.4f);

    // Live bar.
    g.setColour (t.meterLane);
    g.fillRoundedRectangle (bar, 2.0f);
    g.setColour (notice ? t.textSecondary.withAlpha (0.4f) : t.accentMod);
    g.fillRoundedRectangle (bar.withTop (bar.getBottom() - bar.getHeight() * shownEnv), 2.0f);

    g.setColour (t.textSecondary.withAlpha (0.8f));
    g.setFont (metrics::smallFont());
    g.drawText ("ENVELOPE", area.reduced (3.0f, 1.0f).removeFromTop (11.0f), juce::Justification::topLeft);

    if (notice)
    {
        const auto text = juce::String ("NO SIDECHAIN INPUT ROUTED");
        g.setFont (metrics::smallFontBold());
        const float w = juce::GlyphArrangement::getStringWidth (metrics::smallFontBold(), text) + 18.0f;
        auto chip = juce::Rectangle<float> (w, 18.0f).withCentre (area.getCentre());
        g.setColour (t.display.withAlpha (0.92f));
        g.fillRoundedRectangle (chip, 4.0f);
        g.setColour (t.meterYellow.withAlpha (0.9f));
        g.drawRoundedRectangle (chip, 4.0f, 1.0f);
        g.drawText (text, chip.toNearestInt(), juce::Justification::centred);
    }
}

//==============================================================================
SidechainPanel::SidechainPanel (SPAStripProcessor& p)
    : source (p.getAPVTS(), params::id::sc::source),
      listen (p.getAPVTS(), params::id::sc::listen, "Listen"),
      gain (p.getAPVTS(), params::id::sc::gain, "Gain"),
      attack (p.getAPVTS(), params::id::sc::attack, "Attack"),
      release (p.getAPVTS(), params::id::sc::release, "Release"),
      hpf (p.getAPVTS(), params::id::sc::hpf, "HPF"),
      envelope (p)
{
    listen.button.setTooltip ("Replace the output with the detector signal (after its high-pass and gain)");
    source.combo.setTooltip ("External: the plugin's sidechain input. Input: the plugin's own main input.");
    for (auto* c : std::initializer_list<juce::Component*> { &source, &listen, &gain, &attack, &release, &hpf, &envelope })
        addAndMakeVisible (*c);
}

void SidechainPanel::paint (juce::Graphics& g)
{
    draw::sectionHeader (g, getLocalBounds(), "Sidechain", "ENVELOPE FOLLOWER", currentTheme().accent);
}

void SidechainPanel::resized()
{
    auto r = getLocalBounds().withTrimmedTop (metrics::sectionHeaderHeight).reduced (10, 2);
    auto row = r.removeFromTop (26);
    source.setBounds (row.removeFromLeft (112).reduced (0, 2));
    row.removeFromLeft (8);
    listen.setBounds (row.removeFromLeft (80));
    r.removeFromTop (4);
    auto knobs = r.removeFromTop (66);
    const int cell = knobs.getWidth() / 4;
    for (auto* k : { &gain, &attack, &release, &hpf })
        k->setBounds (knobs.removeFromLeft (cell).reduced (3, 0));
    r.removeFromTop (6);
    envelope.setBounds (r.removeFromTop (juce::jmax (40, r.getHeight())));
}

//==============================================================================
void ModMatrixPanel::ActivityBar::paint (juce::Graphics& g)
{
    const auto& t = currentTheme();
    auto b = getLocalBounds().toFloat();
    g.setColour (t.meterLane);
    g.fillRoundedRectangle (b, 2.0f);
    const float cx = b.getCentreX();
    const float w = b.getWidth() * 0.5f * juce::jlimit (-1.0f, 1.0f, value);
    g.setColour (t.accentMod);
    g.fillRect (juce::Rectangle<float> (juce::jmin (cx, cx + w), b.getY(), std::abs (w), b.getHeight()));
    g.setColour (t.textSecondary.withAlpha (0.5f));
    g.fillRect (juce::Rectangle<float> (cx - 0.5f, b.getY() - 1.0f, 1.0f, b.getHeight() + 2.0f));
}

ModMatrixPanel::Cell::Cell (SPAStripProcessor& p, int slot)
    : slotIndex (slot), depth (p.getAPVTS(), params::id::modSlotDepth (slot), "Depth")
{
    picker.setTooltip ("Parameter this slot modulates (envelope x depth)");
    addAndMakeVisible (picker);
    addAndMakeVisible (depth);
    activity.setInterceptsMouseClicks (false, false);
    activity.setTooltip ("Live modulation offset of this slot");
    addAndMakeVisible (activity);
}

void ModMatrixPanel::Cell::paint (juce::Graphics& g)
{
    const auto& t = currentTheme();
    // Slot number chip, left of the picker.
    auto chip = getLocalBounds().removeFromTop (24).removeFromLeft (20).toFloat().reduced (1.0f, 2.0f);
    const bool assigned = shownTarget.isNotEmpty() && shownTarget != "?";
    g.setColour (assigned ? t.accent.withAlpha (0.22f) : t.knobTrack.withAlpha (0.6f));
    g.fillRoundedRectangle (chip, 4.0f);
    g.setColour (assigned ? t.accent : t.textSecondary);
    g.setFont (metrics::smallFontBold());
    g.drawText (juce::String (slotIndex + 1), chip.toNearestInt(), juce::Justification::centred);
}

void ModMatrixPanel::Cell::resized()
{
    auto r = getLocalBounds();
    auto top = r.removeFromTop (24);
    top.removeFromLeft (24);
    picker.setBounds (top.reduced (0, 1));
    r.removeFromTop (3);
    depth.setBounds (r.removeFromLeft (62));
    r.removeFromLeft (6);
    activity.setBounds (r.reduced (0, 0).withSizeKeepingCentre (juce::jmax (0, r.getWidth() - 4), 8).translated (0, -6));
}

ModMatrixPanel::ModMatrixPanel (SPAStripProcessor& p) : processor (p)
{
    for (int s = 0; s < SPAStripProcessor::numModSlots; ++s)
    {
        auto cell = std::make_unique<Cell> (p, s);
        cell->picker.onClick = [this, s] { showPicker (s); };
        addAndMakeVisible (*cell);
        cells[(size_t) s] = std::move (cell);
    }
    refreshTargets();
    startTimerHz (30);
}

ModMatrixPanel::~ModMatrixPanel() { stopTimer(); }

void ModMatrixPanel::paint (juce::Graphics& g)
{
    draw::sectionHeader (g, getLocalBounds(), "Modulation", "SIDECHAIN ENVELOPE  >  8 SLOTS", currentTheme().accent);
    // Faint hairlines between the columns.
    g.setColour (currentTheme().outline.withAlpha (0.45f));
    for (int c = 1; c < 4; ++c)
    {
        const int x = cells[(size_t) c]->getX() - 7;
        g.drawVerticalLine (x, (float) cells[0]->getY() + 4.0f, (float) cells[7]->getBottom() - 4.0f);
    }
    g.drawHorizontalLine (cells[0]->getBottom() + 4, (float) cells[0]->getX(), (float) cells[3]->getRight());
}

void ModMatrixPanel::resized()
{
    auto r = getLocalBounds().withTrimmedTop (metrics::sectionHeaderHeight).reduced (10, 4);
    const int colW = r.getWidth() / 4;
    const int rowH = r.getHeight() / 2;
    for (int s = 0; s < SPAStripProcessor::numModSlots; ++s)
        cells[(size_t) s]->setBounds (juce::Rectangle<int> (r.getX() + (s % 4) * colW, r.getY() + (s / 4) * rowH, colW, rowH)
                                          .reduced (7, 2));
}

void ModMatrixPanel::refreshTargets()
{
    for (int s = 0; s < SPAStripProcessor::numModSlots; ++s)
    {
        auto& c = *cells[(size_t) s];
        const auto id = processor.getModSlotTarget (s);
        if (id != c.shownTarget)
        {
            c.shownTarget = id;
            c.picker.setText (id.isEmpty() ? juce::String ("- none -") : modmenu::shortName (id), id.isEmpty());
            c.repaint();
        }
    }
}

void ModMatrixPanel::chooseTarget (int slot, const juce::String& parameterID)
{
    processor.setModSlotTarget (slot, parameterID);
    refreshTargets();
}

void ModMatrixPanel::showPicker (int slot)
{
    auto menu = modmenu::build (processor.getModSlotTarget (slot));
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&cells[(size_t) slot]->picker)
                            .withMinimumWidth (cells[(size_t) slot]->picker.getWidth()),
                        [safe = juce::Component::SafePointer<ModMatrixPanel> (this), slot] (int result)
                        {
                            bool valid = false;
                            const auto id = modmenu::resultToTarget (result, valid);
                            if (safe != nullptr && valid)
                                safe->chooseTarget (slot, id);
                        });
}

void ModMatrixPanel::timerCallback()
{
    if (! isLiveShowing (*this))
        return;
    refreshTargets();   // undo / preset load / host restore change slot targets without us
    auto& tel = processor.getTelemetry();
    for (int s = 0; s < SPAStripProcessor::numModSlots; ++s)
    {
        auto& c = *cells[(size_t) s];
        const float v = c.shownTarget.isEmpty() ? 0.0f : tel.modSlotOffset[(size_t) s].load (std::memory_order_relaxed);
        if (std::abs (v - c.shownActivity) > 0.004f || (v == 0.0f && c.shownActivity != 0.0f))
        {
            c.shownActivity = v;
            c.activity.value = v;
            c.activity.repaint();
        }
    }
}

//==============================================================================
IoPanel::IoPanel (SPAStripProcessor& p)
    : inMeter ("IN", [&p] (float& l, float& r)
                     { l = p.getTelemetry().inPeakL.load (std::memory_order_relaxed);
                       r = p.getTelemetry().inPeakR.load (std::memory_order_relaxed); }),
      outMeter ("OUT", [&p] (float& l, float& r)
                       { l = p.getTelemetry().peakL.load (std::memory_order_relaxed);
                         r = p.getTelemetry().peakR.load (std::memory_order_relaxed); }),
      inGain (p.getAPVTS(), params::id::inputGain, "Input"),
      outGain (p.getAPVTS(), params::id::outputGain, "Output"),
      mix (p.getAPVTS(), params::id::mix, "Mix")
{
    outMeter.setShowScale (true);
    for (auto* c : std::initializer_list<juce::Component*> { &inMeter, &outMeter, &inGain, &outGain, &mix })
        addAndMakeVisible (*c);
}

void IoPanel::paint (juce::Graphics& g)
{
    draw::sectionHeader (g, getLocalBounds(), "Input / Output", {}, currentTheme().accent);
}

void IoPanel::resized()
{
    auto r = getLocalBounds().withTrimmedTop (metrics::sectionHeaderHeight).reduced (12, 4);
    auto knobs = r.removeFromBottom (74);
    const int cell = knobs.getWidth() / 3;
    for (auto* k : { &inGain, &outGain, &mix })
        k->setBounds (knobs.removeFromLeft (cell).reduced (2, 0));
    r.removeFromBottom (4);
    inMeter.setBounds (r.removeFromTop (r.getHeight() * 2 / 5));
    r.removeFromTop (2);
    outMeter.setBounds (r);
}

} // namespace spa::ui
