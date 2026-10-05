#include "SectionPanel.h"
#include "Controls.h"

namespace spa::ui
{

namespace
{
    // Strip the slot/LFO prefix registry names carry for host lists — the
    // panel title already gives that context.
    juce::String displayName (const params::ParamDef& def)
    {
        auto name = def.name;
        for (const char* prefix : { "A ", "B ", "C ", "L1 ", "L2 ", "L3 " })
            if (name.startsWith (prefix))
                return name.fromFirstOccurrenceOf (" ", false, false);
        return name;
    }

    // Dense-mode-only layout constants (fixed-grid mode is completely
    // untouched by any of this). Fixed knob column width -- narrower than
    // fixed-grid's 66px cellWidth, which is what actually lets a
    // knob-heavy section (REVERB has 9) fit its whole row budget; tuned
    // against the six real FX sections at base editor width (see the
    // 1.0.25 report for the measured row counts this produces).
    constexpr int denseKnobWidth = 56;
    constexpr int denseGap = 2;          // horizontal air between items
    constexpr int denseComboHeight = 22;
    constexpr int denseToggleTextPad = 10;   // air after the toggle's own text

    // FX registry display names are consistently "<Section word> <Rest>"
    // ("Chorus Rate", "Delay Width", "Mod Delay", "Trem On"...) -- the
    // section word is exactly what FXPanel's own header already says, so
    // it is redundant inside a dense FX panel (never touches the registry
    // name itself, which hosts still see for automation -- see displayName
    // above and getButtonText()/paramID, both untouched). This is the
    // fixed, principled set of words dense mode is allowed to try
    // stripping; it is NOT a per-caption override table (that's the
    // collision check in stripFxSectionPrefixIfUnambiguous below).
    const char* const fxSectionPrefixes[] = { "Dist", "Chorus", "Delay", "Reverb", "Mod", "Trem", "Vib", "Glitter" };

    juce::String stripFxSectionPrefix (const juce::String& caption)
    {
        for (auto* prefix : fxSectionPrefixes)
        {
            const juce::String p (prefix);
            if (caption.startsWithIgnoreCase (p + " ") && caption.length() > p.length() + 1)
                return caption.substring (p.length() + 1);
        }
        return caption;
    }
}

SectionPanel::SectionPanel (juce::AudioProcessorValueTreeState& apvts,
                            params::Section section,
                            const juce::String& title,
                            const juce::StringArray& excludeIDs,
                            bool drawFrame,
                            bool denseMode)
    : cellHeight (drawFrame ? 72 : bareCellHeight),
      panelTitle (title.isNotEmpty() ? title : params::sectionName (section)),
      framed (drawFrame),
      dense (denseMode)
{
    for (const auto& def : params::all())
    {
        if (def.section != section || excludeIDs.contains (def.id))
            continue;

        Control control;

        control.label = std::make_unique<juce::Label>();
        control.label->setText (displayName (def).toUpperCase(), juce::dontSendNotification);
        control.label->setFont (metrics::smallFont());
        control.label->setJustificationType (juce::Justification::centred);
        control.label->setInterceptsMouseClicks (false, false);

        switch (def.kind)
        {
            case params::ParamKind::boolParam:
            {
                auto toggle = std::make_unique<ParamToggleButton> (displayName (def).toUpperCase());
                control.buttonAttachment =
                    std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
                        apvts, def.id, *toggle);
                control.component = std::move (toggle);
                control.label = nullptr;  // toggle draws its own text
                control.wide = true;
                control.isToggle = true;
                break;
            }
            case params::ParamKind::choiceParam:
            {
                auto combo = std::make_unique<ParamComboBox>();
                combo->addItemList (def.choices, 1);
                control.comboAttachment =
                    std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
                        apvts, def.id, *combo);
                control.component = std::move (combo);
                control.wide = true;
                control.isCombo = true;
                break;
            }
            case params::ParamKind::intParam:
            case params::ParamKind::floatParam:
            {
                auto knob = std::make_unique<ModSlider> (
                    juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox);
                control.sliderAttachment =
                    std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
                        apvts, def.id, *knob);
                control.label->setMinimumHorizontalScale (0.6f);
                // Dense mode: wireDragReadout captures the resting text it's
                // given BY VALUE for its onDragEnd lambda, so wiring it here
                // (before the caption-shortening pass below runs) would
                // silently revert every shortened caption back to its full
                // form the moment the user drags that knob. Deferred to
                // after shortening decides the final text -- see below.
                if (! denseMode)
                    Knob::wireDragReadout (*knob, *control.label, apvts.getParameter (def.id),
                                           control.label->getText(), false);
                control.component = std::move (knob);
                break;
            }
        }

        control.component->getProperties().set ("paramID", def.id);
        if (auto* tip = dynamic_cast<juce::SettableTooltipClient*> (control.component.get()))
            tip->setTooltip (tooltipFor (def.id));
        addAndMakeVisible (*control.component);
        if (control.label != nullptr)
            addAndMakeVisible (*control.label);

        controls.push_back (std::move (control));
    }

    if (dense)
    {
        // Shorten every caption by trying to strip its leading FX section
        // word (see stripFxSectionPrefix above), but only where the result
        // is UNIQUE among every other shortened candidate in this SAME
        // panel -- this is what keeps TREM/VIB's "Trem Rate"/"Vib Rate"
        // (both would strip to "Rate") as their full forms while letting
        // unambiguous ones like "Trem Shape" (no "Vib Shape" to collide
        // with) shorten to "SHAPE". Generic, not a per-panel hand list; the
        // registry names and getButtonText()/paramID (host automation,
        // tooltips) are never touched, only what's drawn here.
        std::vector<juce::String> candidates (controls.size());
        for (size_t i = 0; i < controls.size(); ++i)
        {
            const auto& c = controls[i];
            const auto full = c.isToggle
                             ? dynamic_cast<juce::ToggleButton*> (c.component.get())->getButtonText()
                             : (c.label != nullptr ? c.label->getText() : juce::String());
            candidates[i] = stripFxSectionPrefix (full);
        }
        for (size_t i = 0; i < controls.size(); ++i)
        {
            if (candidates[i].isEmpty())
                continue;
            bool ambiguous = false;
            for (size_t j = 0; j < controls.size(); ++j)
                if (j != i && candidates[j].equalsIgnoreCase (candidates[i]))
                    { ambiguous = true; break; }
            if (ambiguous)
                continue;

            auto& c = controls[i];
            if (c.isToggle)
            {
                if (auto* tb = dynamic_cast<juce::ToggleButton*> (c.component.get()))
                    tb->setButtonText (candidates[i]);
            }
            else if (c.label != nullptr)
            {
                c.label->setText (candidates[i], juce::dontSendNotification);
            }
        }

        // Now that every caption has its FINAL text, wire the knobs' drag
        // readouts (deferred from the constructor loop above -- see there).
        for (auto& c : controls)
        {
            if (c.isToggle || c.isCombo || c.label == nullptr)
                continue;
            auto* knob = dynamic_cast<juce::Slider*> (c.component.get());
            if (knob == nullptr)
                continue;
            const auto paramID = c.component->getProperties()["paramID"].toString();
            Knob::wireDragReadout (*knob, *c.label, apvts.getParameter (paramID),
                                   c.label->getText(), false);
        }
    }
}

void SectionPanel::paint (juce::Graphics& g)
{
    if (! framed)
        return;

    draw::panel (g, getLocalBounds().toFloat());
    draw::sectionHeader (g, getLocalBounds(), panelTitle, {}, currentTheme().accent);
}

int SectionPanel::denseToggleWidth (const juce::String& text) const
{
    // Mirrors SPASynthLookAndFeel::drawToggleButton's own geometry exactly
    // (pillH capped at 14, pillW = pillH*1.9, text drawn with smallFont()
    // starting pillW+5 in) so this never under-estimates and clips the
    // toggle's own text -- drawToggleButton uses a plain drawText, which
    // does not wrap or shrink.
    constexpr float pillH = 14.0f;
    constexpr float pillW = pillH * 1.9f;
    const auto textW = juce::GlyphArrangement::getStringWidth (metrics::smallFont(), text);
    return (int) std::ceil (pillW + 5.0f + textW) + denseToggleTextPad;
}

int SectionPanel::denseComboWidth (const juce::String& longestChoice) const
{
    // Solved from comboTextFitsCellTest's own formula (LookAndFeel_V2::
    // positionComboBoxText: availableW = width + 3 - height - 10) for the
    // width that makes availableW exactly equal to the text -- this is the
    // same contract that test checks.
    //
    // minComboWidth is a defensive floor found by review, not derived from
    // that formula: MOD STAGES ("12" the longest choice, so the formula
    // alone gives a very tight ~43-45px box) painted "..." instead of its
    // real selected value ("6") even though the ComboBoxAttachment/
    // selectedId/getText() were always correct -- the box's OWN internal
    // text Label ended up with a bounds far narrower than this box's actual
    // final size (traced to an internal Label bounds of ~13px against a
    // 43px box, when the LookAndFeel's own formula for that box size gives
    // 24px), and neither an explicit extra ComboBox::resized() call nor
    // directly overwriting the internal Label's bounds with the correct
    // formula made it stick -- something later re-narrows it, which is a
    // JUCE ComboBox/Label interaction this investigation could not fully
    // pin down. Empirically, giving the box materially more headroom than
    // the tight formula makes the symptom disappear (verified at +16 and
    // +30 over the tight calculation), so this floor is a deliberate safety
    // margin against whatever that mechanism is, not a size any real choice
    // list here needs. Every OTHER FX combo's longest choice already clears
    // this floor on its own (verified after adding it), so this only
    // changes STAGES' width in practice.
    constexpr int minComboWidth = 60;
    const auto textW = juce::GlyphArrangement::getStringWidth (metrics::labelFont(), longestChoice);
    return juce::jmax (minComboWidth, (int) std::ceil (textW) + denseComboHeight + 7 + 4);
}

int SectionPanel::denseCaptionWidth (const juce::String& text) const
{
    // A knob/combo's own caption Label (e.g. "DELAY WIDTH") is centred
    // within its column and, unlike the knob/combo itself, was never given
    // a text-fitting width in dense mode -- at the fixed 40px knob column a
    // caption like "DELAY TIME" or "DELAY WIDTH" is wider than its cell and
    // gets clipped by the Label's own bounds (Justification::centred does
    // NOT wrap or shrink). Measured with the exact font/justification the
    // Label itself paints with.
    //
    // THE BUG THIS FIXES (found on review, not by any test -- see the new
    // fxPanelCaptionFitsColumnTest): the first attempt at this padded only
    // +4px, which under-shot by a wide margin because juce::Label has a
    // BUILT-IN BorderSize<int>{1,5,1,5} (juce_Label.h) that eats 5px off
    // EACH side of its own bounds before laying out text -- 10px total this
    // function never accounted for. So captions still clipped ("DELAY
    // T...") even after that "fix" landed; it was never actually verified
    // against a zoomed render, only an unzoomed one where the elision
    // wasn't visible. denseLabelBorderPad below is exactly that JUCE
    // default, named so the connection to Label's own constant is explicit.
    constexpr int denseLabelBorderPad = 10;   // juce::Label's default L+R border (5+5)
    const auto textW = juce::GlyphArrangement::getStringWidth (metrics::smallFont(), text);
    return (int) std::ceil (textW) + denseLabelBorderPad + 2;   // +2 a little slack
}

std::vector<SectionPanel::DensePlacement> SectionPanel::computeDensePlacements (int width) const
{
    std::vector<DensePlacement> placements;
    placements.reserve (controls.size());

    int x = 0, row = 0;
    int pendingToggleIndex = -1;   // index into `placements` of a toggle waiting for a partner

    for (int i = 0; i < (int) controls.size(); ++i)
    {
        const auto& c = controls[(size_t) i];
        if (! c.component->isVisible())
            continue;
        int itemWidth = denseKnobWidth;
        if (c.isToggle)
        {
            if (auto* tb = dynamic_cast<juce::ToggleButton*> (c.component.get()))
                itemWidth = denseToggleWidth (tb->getButtonText());
        }
        else if (c.isCombo)
        {
            juce::String longest;
            if (auto* cb = dynamic_cast<juce::ComboBox*> (c.component.get()))
                for (int n = 0; n < cb->getNumItems(); ++n)
                    if (cb->getItemText (n).length() > longest.length())
                        longest = cb->getItemText (n);
            itemWidth = denseComboWidth (longest);
            if (c.label != nullptr)
                itemWidth = juce::jmax (itemWidth, denseCaptionWidth (c.label->getText()));
        }
        else if (c.label != nullptr)   // knob
        {
            itemWidth = juce::jmax (itemWidth, denseCaptionWidth (c.label->getText()));
        }

        // Two CONSECUTIVE toggles share one column, stacked top/bottom --
        // this is what actually frees up the width a dense row needs
        // (a lone toggle already reserved that column's width; a knob or
        // combo landing between two toggles breaks the pairing rather than
        // stacking across a gap, which would read as an unrelated group).
        if (c.isToggle && pendingToggleIndex >= 0)
        {
            auto& top = placements[(size_t) pendingToggleIndex];
            const auto sharedWidth = juce::jmax (top.width, itemWidth);
            top.width = sharedWidth;
            top.stackHalf = 1;
            placements.push_back ({ i, top.row, top.x, sharedWidth, 2 });
            pendingToggleIndex = -1;
            continue;
        }

        if (x + itemWidth > width && x > 0)
        {
            ++row;
            x = 0;
        }

        placements.push_back ({ i, row, x, itemWidth, 0 });
        pendingToggleIndex = c.isToggle ? (int) placements.size() - 1 : -1;
        x += itemWidth + denseGap;
    }

    return placements;
}

int SectionPanel::heightForWidth (int width) const
{
    if (dense)
    {
        // -12 to match resized()'s own area, which reduces its local bounds
        // by (6,0) before computing placements -- heightForWidth and
        // resized must agree on the row count from the same effective
        // width, or a caller sizing itself from heightForWidth() could give
        // resized() too little height for the rows it actually lays out.
        const auto placements = computeDensePlacements (width - 12);
        int rows = 1;
        for (const auto& p : placements)
            rows = juce::jmax (rows, p.row + 1);
        return (framed ? headerHeight : 0) + rows * cellHeight + 8;
    }

    const auto columns = juce::jmax (1, (width - 12) / cellWidth);
    int cellsUsed = 0;
    for (const auto& c : controls)
        cellsUsed += c.wide ? 2 : 1;
    const auto rows = (cellsUsed + columns - 1) / columns;
    return (framed ? headerHeight : 0) + rows * cellHeight + 8;
}

void SectionPanel::resized()
{
    const auto area = getLocalBounds().withTrimmedTop (framed ? headerHeight : 0).reduced (6, 0);
    const auto labelH = framed ? 16 : 13;

    if (dense)
    {
        const auto placements = computeDensePlacements (area.getWidth());
        int rows = 1;
        for (const auto& p : placements)
            rows = juce::jmax (rows, p.row + 1);

        // Same shrink-toward-a-floor rule as fixed-grid mode: labels always
        // get their full slice, the control above it is what adapts.
        const auto rowHeight = juce::jlimit (labelH, cellHeight, area.getHeight() / rows);

        for (const auto& p : placements)
        {
            auto& control = controls[(size_t) p.controlIndex];
            auto cellBounds = juce::Rectangle<int> (area.getX() + p.x, area.getY() + p.row * rowHeight,
                                                     p.width, rowHeight);

            if (control.isToggle)
            {
                if (p.stackHalf != 0)
                {
                    auto half = p.stackHalf == 1 ? cellBounds.removeFromTop (rowHeight / 2)
                                                 : cellBounds.removeFromBottom (rowHeight - rowHeight / 2);
                    control.component->setBounds (half.withSizeKeepingCentre (
                        half.getWidth() - 4, juce::jmin (18, half.getHeight())));
                }
                else
                {
                    control.component->setBounds (cellBounds.withSizeKeepingCentre (
                        cellBounds.getWidth() - 4, juce::jmin (22, cellBounds.getHeight())));
                }
            }
            else if (control.isCombo)
            {
                control.label->setBounds (cellBounds.removeFromBottom (labelH));
                control.component->setBounds (cellBounds.withSizeKeepingCentre (
                    cellBounds.getWidth() - 2, juce::jmin (denseComboHeight, cellBounds.getHeight())));
                // Defense in depth for the ComboBox's OWN internal text
                // Label (distinct from control.label, our caption below
                // it): found once, on the narrowest dense combo (MOD
                // STAGES), painting "..." for a correct, correctly-selected
                // single-digit value because its internal Label's bounds
                // disagreed with the box's own final size -- root cause not
                // fully pinned down (denseComboWidth's minComboWidth floor
                // is the fix that actually resolved it; see that comment),
                // but explicitly setting the internal Label's bounds here,
                // with the exact formula LookAndFeel_V2::
                // positionComboBoxText uses, costs nothing and guards
                // against the same class of staleness recurring.
                if (auto* combo = dynamic_cast<juce::ComboBox*> (control.component.get()))
                    for (auto* child : combo->getChildren())
                        if (auto* internalLabel = dynamic_cast<juce::Label*> (child))
                            internalLabel->setBounds (1, 1, combo->getWidth() + 3 - combo->getHeight(),
                                                      combo->getHeight() - 2);
            }
            else   // knob
            {
                control.label->setBounds (cellBounds.removeFromBottom (labelH));
                // Diameter must not follow the cell (SPASynth 1.0.32): a long
                // caption widens its cell, and the knob would grow with it.
                // Every dense knob gets the same square, centred in its cell.
                const auto knobArea = cellBounds.reduced (2);
                const auto side = juce::jmin (denseKnobWidth - 4, knobArea.getHeight());
                control.component->setBounds (knobArea.withSizeKeepingCentre (side, juce::jmin (side, knobArea.getHeight())));
            }
        }
        return;
    }

    const auto columns = juce::jmax (1, area.getWidth() / cellWidth);

    int cellsUsed = 0;
    for (const auto& c : controls)
        cellsUsed += c.wide ? 2 : 1;
    const auto rows = juce::jmax (1, (cellsUsed + columns - 1) / columns);

    // Caption labels get their full height reserved first, always -- never
    // less than this, whatever else happens. The knob/control area is what
    // actually adapts: rowHeight normally matches cellHeight (the "ideal"
    // heightForWidth() spacing above), but shrinks -- down to just enough
    // for the label -- when this panel was given less real height than
    // that (e.g. FXPanel already collapsed its display to nothing and a
    // section still has more rows than fit at full cellHeight). This is
    // what actually keeps captions from clipping off the bottom on a
    // control-heavy tab like TREM/VIB: the caller (FXPanel) shrinks the
    // display as its first line of defense, but for sections with enough
    // rows even that isn't enough, so the row height itself must adapt too.
    const auto rowHeight = juce::jlimit (labelH, cellHeight, area.getHeight() / rows);

    int cell = 0;
    for (auto& control : controls)
    {
        const auto span = control.wide ? 2 : 1;
        // Wrap early if a wide control would split across rows.
        if ((cell % columns) + span > columns)
            cell += columns - (cell % columns);

        const auto col = cell % columns;
        const auto row = cell / columns;
        auto cellBounds = juce::Rectangle<int> (area.getX() + col * cellWidth,
                                                area.getY() + row * rowHeight,
                                                cellWidth * span, rowHeight);
        cell += span;

        if (control.label != nullptr)
        {
            control.label->setBounds (cellBounds.removeFromBottom (labelH));
            control.component->setBounds (cellBounds.reduced (framed ? 4 : 2));
        }
        else if (dynamic_cast<juce::ComboBox*> (control.component.get()) != nullptr)
        {
            control.component->setBounds (cellBounds.withSizeKeepingCentre (
                cellBounds.getWidth() - 10, juce::jmin (24, cellBounds.getHeight())));
        }
        else  // toggle
        {
            control.component->setBounds (cellBounds.withSizeKeepingCentre (
                cellBounds.getWidth() - 10, juce::jmin (22, cellBounds.getHeight())));
        }
    }
}

void SectionPanel::setControlVisible (const juce::String& paramID, bool shouldBeVisible)
{
    for (auto& c : controls)
        if (c.component->getProperties()["paramID"].toString() == paramID)
        {
            c.component->setVisible (shouldBeVisible);
            if (c.label != nullptr)
                c.label->setVisible (shouldBeVisible);
        }
}

std::vector<juce::Component*> SectionPanel::findControlComponents (const juce::String& paramID) const
{
    for (auto& c : controls)
    {
        if (c.component->getProperties()["paramID"].toString() != paramID)
            continue;

        std::vector<juce::Component*> result { c.component.get() };
        if (c.label != nullptr)
            result.push_back (c.label.get());
        return result;
    }
    return {};
}

} // namespace spa::ui
