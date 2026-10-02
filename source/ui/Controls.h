#pragma once

#include "Theme.h"
#include "../params/ParameterRegistry.h"

namespace spa::ui
{

// The editor content implements this so any modulatable knob can ask for the
// "Assign to mod slot N" context menu without knowing the editor's type.
class ModAssignHost
{
public:
    virtual ~ModAssignHost() = default;
    virtual void showModAssignMenu (juce::Slider&, const juce::String& paramID) = 0;
};

// A rotary juce::Slider that turns a right-click (context-menu click) into a
// callback to the nearest ModAssignHost ancestor instead of a value drag.
// Knobs whose parameter is not a modulation target (the host decides) simply
// get no menu.
class ModSlider : public juce::Slider
{
public:
    ModSlider() = default;
    ModSlider (SliderStyle style, TextEntryBoxPosition pos) : juce::Slider (style, pos) {}

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
        {
            contextClick = true;
            if (auto* host = findParentComponentOfClass<ModAssignHost>())
                host->showModAssignMenu (*this, getProperties()["paramID"].toString());
            return;
        }
        contextClick = false;
        juce::Slider::mouseDown (e);
    }
    void mouseDrag (const juce::MouseEvent& e) override { if (! contextClick) juce::Slider::mouseDrag (e); }
    void mouseUp (const juce::MouseEvent& e) override   { if (! contextClick) juce::Slider::mouseUp (e); contextClick = false; }
    void mouseDoubleClick (const juce::MouseEvent& e) override { if (! e.mods.isPopupMenu()) juce::Slider::mouseDoubleClick (e); }

private:
    bool contextClick = false;
};

// A thin-ring knob with its label underneath — the atomic control of the UI.
// Set modColoured for modulation-domain knobs (cyan ring).
class Knob : public juce::Component
{
public:
    Knob (juce::AudioProcessorValueTreeState& apvts, const juce::String& paramID,
          const juce::String& labelText, bool modColoured = false)
        : parameter (apvts.getParameter (paramID)),
          originalParamID (paramID),
          restingText (labelText.toUpperCase()),
          modAccent (modColoured)
    {
        slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        // Controls never take keyboard focus themselves; a click bubbles up to
        // the editor's content component, which owns Cmd/Ctrl+Z.
        slider.setWantsKeyboardFocus (false);
        slider.getProperties().set ("paramID", paramID);      // mod-viz driver / assign menu look it up
        if (modColoured)
            slider.setComponentID ("mod");
        attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
            apvts, paramID, slider);
        addAndMakeVisible (slider);

        label.setText (restingText, juce::dontSendNotification);
        label.setFont (metrics::smallFont());
        label.setJustificationType (juce::Justification::centred);
        label.setInterceptsMouseClicks (false, false);
        label.setMinimumHorizontalScale (0.6f);
        addAndMakeVisible (label);

        // Press state: the live value readout.
        wireDragReadout (slider, label, parameter, restingText, modAccent);
    }

    ~Knob() override = default;

    // Shared wiring: while dragging, `label` shows the parameter's value in
    // the accent colour; on release it reverts. Guarded so programmatic
    // value changes never hijack the label.
    static void wireDragReadout (juce::Slider& s, juce::Label& l,
                                 juce::RangedAudioParameter* param,
                                 const juce::String& restingText, bool modAccent)
    {
        const auto showValue = [&l, param, modAccent]
        {
            if (param == nullptr)
                return;
            const auto& t = currentTheme();
            l.setColour (juce::Label::textColourId, modAccent ? t.accentMod : t.accent);
            auto text = param->getCurrentValueAsText();
            if (param->getLabel().isNotEmpty())
                text << " " << param->getLabel();
            l.setText (text, juce::dontSendNotification);
        };

        s.onDragStart = showValue;
        s.onValueChange = [&s, showValue]
        {
            if (s.isMouseButtonDown())
                showValue();
        };
        s.onDragEnd = [&l, restingText]
        {
            l.removeColour (juce::Label::textColourId);
            l.setText (restingText, juce::dontSendNotification);
        };
    }

    void resized() override
    {
        auto area = getLocalBounds();
        label.setBounds (area.removeFromBottom (13));
        slider.setBounds (area);
    }

    ModSlider slider;
    juce::Label label;

    // Drop the parameter attachment early, for controls that can outlive
    // the editor (the VOICE call-out's panel, owned by JUCE's modal manager
    // and deleted asynchronously) and so must not touch the processor's
    // APVTS from a destructor that may run after the processor is gone.
    void detach() { attachment.reset(); }

    // Test-only introspection: true once the attachment has been released.
    bool isDetached() const { return attachment == nullptr; }

    // Overrides the resting-state caption text (1.0.27, the tall wavetable
    // 3D layout's abbreviated captions -- e.g. "POSITION" -> "POS"). Re-wires
    // wireDragReadout's onDragEnd, which otherwise reverts to the ORIGINAL
    // restingText captured at construction rather than this new one. A no-op
    // call with the same text leaves the label alone rather than clobbering
    // a live drag-value readout.
    void setCaption (const juce::String& text)
    {
        const auto upper = text.toUpperCase();
        if (upper == restingText)
            return;
        restingText = upper;
        if (! slider.isMouseButtonDown())
            label.setText (restingText, juce::dontSendNotification);
        wireDragReadout (slider, label, parameter, restingText, modAccent);
    }
    juce::String getCaptionForTest() const { return restingText; }

private:
    juce::RangedAudioParameter* parameter = nullptr;
    juce::String originalParamID;
    juce::String restingText;
    bool modAccent = false;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
};

// Labelled combo box row.
class Choice : public juce::Component
{
public:
    Choice (juce::AudioProcessorValueTreeState& apvts, const juce::String& paramID)
        : originalParamID (paramID)
    {
        combo.setWantsKeyboardFocus (false);
        combo.getProperties().set ("paramID", paramID);
        if (const auto* def = params::find (paramID))
            combo.addItemList (def->choices, 1);
        attachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
            apvts, paramID, combo);
        addAndMakeVisible (combo);
    }

    void resized() override { combo.setBounds (getLocalBounds()); }

    juce::ComboBox combo;

    void detach() { attachment.reset(); }   // see Knob::detach

    // Test-only introspection: true once the attachment has been released.
    bool isDetached() const { return attachment == nullptr; }

private:
    juce::String originalParamID;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> attachment;
};

// Pill toggle bound to a bool parameter.
class Toggle : public juce::Component
{
public:
    Toggle (juce::AudioProcessorValueTreeState& apvts, const juce::String& paramID,
            const juce::String& text)
        : button (text), originalParamID (paramID)
    {
        button.setWantsKeyboardFocus (false);
        button.getProperties().set ("paramID", paramID);
        attachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
            apvts, paramID, button);
        addAndMakeVisible (button);
    }

    void resized() override { button.setBounds (getLocalBounds()); }

    juce::ToggleButton button;

    void detach() { attachment.reset(); }   // see Knob::detach
    bool isDetached() const { return attachment == nullptr; }

private:
    juce::String originalParamID;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> attachment;
};

// Grays out (and disables interaction on) a set of target components
// whenever another parameter's value makes them irrelevant -- e.g. the LFO
// rate knob while sync is on, or the delay time knob while delay sync is on.
// APVTS listener callbacks can fire off the message thread, so this defers
// through AsyncUpdater before touching any Component, same pattern OscStrip
// already uses for its mode-driven visibility.
//
// setEnabled(false) on a target propagates through JUCE's parent-enablement
// chain (Component::isEnabled() walks up to its parent and ANDs them), so
// passing a Knob/Choice/Toggle wrapper as the single target is enough -- its
// inner slider/combo/button (and the Knob's caption label) dim themselves
// via the LookAndFeel's isEnabled() checks. For SectionPanel-built controls,
// where the label is a sibling rather than a child, pass both explicitly.
class DependentEnable : private juce::AudioProcessorValueTreeState::Listener,
                        private juce::AsyncUpdater
{
public:
    DependentEnable (juce::AudioProcessorValueTreeState& apvtsIn, const juce::String& gateParamID,
                     std::function<bool (float)> predicateIn,
                     std::vector<juce::Component*> targetsIn)
        : apvts (apvtsIn), gateID (gateParamID), predicate (std::move (predicateIn)),
          targets (std::move (targetsIn))
    {
        if (auto* raw = apvts.getRawParameterValue (gateID))
            lastValue.store (raw->load());
        applyState();                          // initial state, before the first repaint
        apvts.addParameterListener (gateID, this);
    }

    ~DependentEnable() override
    {
        apvts.removeParameterListener (gateID, this);
    }

private:
    void parameterChanged (const juce::String&, float newValue) override
    {
        lastValue.store (newValue);
        triggerAsyncUpdate();
    }

    void handleAsyncUpdate() override { applyState(); }

    void applyState()
    {
        const auto enabled = predicate (lastValue.load());
        for (auto* c : targets)
            if (c != nullptr)
                c->setEnabled (enabled);
    }

    juce::AudioProcessorValueTreeState& apvts;
    juce::String gateID;
    std::function<bool (float)> predicate;
    std::vector<juce::Component*> targets;
    std::atomic<float> lastValue { 0.0f };
};

// Tracks a set of "engaged" gate parameters (e.g. FX enable toggles) keyed by
// a tab/label name, and repaints a target component whenever any of them
// changes -- same listener+AsyncUpdater idiom as DependentEnable above, so UI
// never gets touched off the parameter-callback thread. isEngaged() is a
// plain synchronous read (juce::AudioProcessorValueTreeState::getRawParameterValue
// is safe to read from any thread at any time), so it can be queried directly
// from paint(). A tab can map to more than one param id (e.g. one tab
// covering two effects); it reads as engaged if ANY of them is on.
class TabEngagementTracker : private juce::AudioProcessorValueTreeState::Listener,
                             private juce::AsyncUpdater
{
public:
    TabEngagementTracker (juce::AudioProcessorValueTreeState& apvtsIn,
                          std::vector<std::pair<juce::String, std::vector<juce::String>>> paramsByTabIn,
                          juce::Component& repaintTargetIn)
        : apvts (apvtsIn), paramsByTab (std::move (paramsByTabIn)), repaintTarget (repaintTargetIn)
    {
        for (auto& entry : paramsByTab)
            for (auto& id : entry.second)
            {
                allIds.push_back (id);
                apvts.addParameterListener (id, this);
            }
    }

    ~TabEngagementTracker() override
    {
        for (auto& id : allIds)
            apvts.removeParameterListener (id, this);
    }

    bool isEngaged (const juce::String& tabName) const
    {
        for (auto& entry : paramsByTab)
        {
            if (entry.first != tabName)
                continue;
            for (auto& id : entry.second)
                if (auto* raw = apvts.getRawParameterValue (id))
                    if (raw->load() > 0.5f)
                        return true;
            return false;
        }
        return false;
    }

private:
    void parameterChanged (const juce::String&, float) override { triggerAsyncUpdate(); }
    void handleAsyncUpdate() override { repaintTarget.repaint(); }

    juce::AudioProcessorValueTreeState& apvts;
    std::vector<std::pair<juce::String, std::vector<juce::String>>> paramsByTab;
    std::vector<juce::String> allIds;
    juce::Component& repaintTarget;
};

// Popup menus all go through here so a single place decides how they are shown.
inline void showPopupAnchored (juce::Component& /*anchor*/, juce::PopupMenu& menu,
                               const juce::PopupMenu::Options& options,
                               std::function<void (int)> callback)
{
    menu.showMenuAsync (options, std::move (callback));
}

// A combo-box-looking button that opens a popup menu built by the owner (the
// mod slot target picker, the factory IR picker). Shows `text` left-aligned
// and elided, a chevron on the right; `placeholder` style when `dimmed`.
class PickerButton : public juce::Button
{
public:
    PickerButton() : juce::Button ("picker") { setWantsKeyboardFocus (false); }

    void setText (const juce::String& t, bool isPlaceholder)
    {
        if (t != text || isPlaceholder != placeholder)
        {
            text = t;
            placeholder = isPlaceholder;
            repaint();
        }
    }
    juce::String getText() const { return text; }

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        const auto& t = currentTheme();
        const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
        const auto enabled = isEnabled();
        g.setColour (t.display.brighter (down ? 0.10f : (over ? 0.05f : 0.0f)).withMultipliedAlpha (enabled ? 1.0f : 0.5f));
        g.fillRoundedRectangle (bounds, metrics::cornerRadius);
        g.setColour (t.outline);
        g.drawRoundedRectangle (bounds, metrics::cornerRadius, 1.0f);

        juce::Path chevron;
        const auto cx = (float) getWidth() - 11.0f;
        const auto cy = (float) getHeight() * 0.5f;
        chevron.startNewSubPath (cx - 3.5f, cy - 1.8f);
        chevron.lineTo (cx, cy + 2.2f);
        chevron.lineTo (cx + 3.5f, cy - 1.8f);
        g.setColour (t.textSecondary.withAlpha (enabled ? 1.0f : 0.4f));
        g.strokePath (chevron, juce::PathStrokeType (1.4f));

        g.setColour (placeholder ? t.textSecondary.withAlpha (0.7f) : t.textPrimary);
        g.setFont (metrics::labelFont());
        g.drawText (text, getLocalBounds().withTrimmedLeft (9).withTrimmedRight (20),
                    juce::Justification::centredLeft, true);
    }

private:
    juce::String text;
    bool placeholder = false;
};

} // namespace spa::ui
