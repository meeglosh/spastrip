#include "Dialogs.h"

#include "../SPAStripProcessor.h"
#include "SPAStripLookAndFeel.h"

namespace spa::ui
{

//==============================================================================
DialogOverlay::DialogOverlay (juce::Point<int> size) : cardSize (size)
{
    setWantsKeyboardFocus (true);
}

juce::Rectangle<int> DialogOverlay::getCardBounds() const
{
    return juce::Rectangle<int> (cardSize.x, cardSize.y).withCentre (getLocalBounds().getCentre());
}

void DialogOverlay::paint (juce::Graphics& g)
{
    const auto& t = currentTheme();
    g.fillAll (juce::Colours::black.withAlpha (0.55f));
    const auto card = getCardBounds().toFloat();

    // Soft shadow, then the card on the faceplate colour.
    for (int i = 6; i >= 1; --i)
    {
        g.setColour (juce::Colours::black.withAlpha (0.07f));
        g.fillRoundedRectangle (card.expanded ((float) i * 2.0f).translated (0.0f, (float) i), metrics::cornerRadius + (float) i * 2.0f);
    }
    g.setColour (t.panel.brighter (0.04f));
    g.fillRoundedRectangle (card, metrics::cornerRadius);
    g.setColour (t.outline);
    g.drawRoundedRectangle (card, metrics::cornerRadius, 1.0f);

    const auto title = getCardTitle();
    if (title.isNotEmpty())
    {
        g.setColour (t.accent);
        g.setFont (metrics::sectionFont());
        g.drawText (title.toUpperCase(), card.toNearestInt().withTrimmedTop (10).removeFromTop (20).reduced (16, 0),
                    juce::Justification::centredLeft);
    }
}

void DialogOverlay::resized() { layoutCard (getCardBounds()); }

void DialogOverlay::mouseDown (const juce::MouseEvent& e)
{
    if (! getCardBounds().contains (e.getPosition()))
        dismiss();
}

bool DialogOverlay::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::escapeKey)
    {
        dismiss();
        return true;
    }
    return true;   // modal: swallow everything else (the editor's Cmd+Z must not act behind a dialog)
}

//==============================================================================
AboutDialog::AboutDialog (juce::AudioProcessor& p) : DialogOverlay ({ 520, 500 }), processor (p)
{
    byline.setText ("by Silverplatter Audio", juce::dontSendNotification);
    byline.setFont (metrics::smallFont());
    byline.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (byline);

    juce::StringArray lines;
    lines.add ("Version " SPASTRIP_VERSION);
    lines.add ("Build " SPASTRIP_BUILD_DATE " (" SPASTRIP_GIT_COMMIT ")");
    lines.add ("Format: " + formatString());
    info.setText (lines.joinIntoString ("\n"), juce::dontSendNotification);
    info.setFont (metrics::labelFont());
    info.setJustificationType (juce::Justification::centredTop);
    info.setMinimumHorizontalScale (1.0f);
    addAndMakeVisible (info);

    creditsHeading.setText ("FACTORY IMPULSE RESPONSE CREDITS", juce::dontSendNotification);
    creditsHeading.setFont (metrics::smallFontBold());
    addAndMakeVisible (creditsHeading);

    credits.setMultiLine (true, true);
    credits.setReadOnly (true);
    credits.setCaretVisible (false);
    credits.setScrollbarsShown (true);
    credits.setFont (metrics::labelFont());
    credits.setColour (juce::TextEditor::backgroundColourId, currentTheme().display);
    credits.setColour (juce::TextEditor::textColourId, currentTheme().textPrimary);
    credits.setColour (juce::TextEditor::outlineColourId, currentTheme().outline);
    credits.setColour (juce::TextEditor::focusedOutlineColourId, currentTheme().outline);
    {
        // The credits ship as Markdown; show them as plain text (no heading hashes / bold markers).
        juce::StringArray creditLines;
        creditLines.addLines (SPAStripProcessor::getFactoryIRCredits());
        for (auto& line : creditLines)
        {
            line = line.replace ("**", "");
            while (line.startsWithChar ('#'))
                line = line.substring (1);
            line = line.trimStart();
        }
        credits.setText (creditLines.joinIntoString ("\n"), false);
    }
    credits.moveCaretToTop (false);
    addAndMakeVisible (credits);

    link.setButtonText ("silverplatteraudio.com");
    link.setURL (juce::URL ("https://www.silverplatteraudio.com"));
    link.setFont (metrics::smallFont(), false, juce::Justification::centred);
    link.setColour (juce::HyperlinkButton::textColourId, currentTheme().accent);
    addAndMakeVisible (link);

    copyButton.onClick = [this]
    {
        juce::SystemClipboard::copyTextToClipboard (buildClipboardText());
        copyButton.setButtonText ("Copied");
        juce::Timer::callAfterDelay (900, [safe = juce::Component::SafePointer<juce::TextButton> (&copyButton)]
                                     { if (safe != nullptr) safe->setButtonText ("Copy Info"); });
    };
    closeButton.onClick = [this] { dismiss(); };
    addAndMakeVisible (copyButton);
    addAndMakeVisible (closeButton);
}

juce::String AboutDialog::formatString() const
{
    if (processor.wrapperType == juce::AudioProcessor::wrapperType_AudioUnit)  return "AU";
    if (processor.wrapperType == juce::AudioProcessor::wrapperType_VST3)       return "VST3";
    if (processor.wrapperType == juce::AudioProcessor::wrapperType_Standalone) return "Standalone";
    return "Unknown";
}

juce::String AboutDialog::buildClipboardText() const
{
    juce::StringArray lines;
    lines.add ("SPAStrip " SPASTRIP_VERSION);
    lines.add ("Commit: " SPASTRIP_GIT_COMMIT);
    lines.add ("Build date: " SPASTRIP_BUILD_DATE);
    lines.add ("Format: " + formatString());
    lines.add ("OS: " + juce::SystemStats::getOperatingSystemName());
    lines.add ("Sample rate: " + juce::String (processor.getSampleRate(), 0) + " Hz");
    lines.add ("Block size: " + juce::String (processor.getBlockSize()));
    const juce::String host (juce::PluginHostType().getHostDescription());
    if (host.isNotEmpty() && host != "Unknown")
        lines.add ("Host: " + host);
    return lines.joinIntoString ("\n");
}

void AboutDialog::paint (juce::Graphics& g)
{
    DialogOverlay::paint (g);
    draw::trackedCentredText (g, metrics::wordmarkFont(), "SPASTRIP", wordmarkArea, currentTheme().textPrimary);
}

void AboutDialog::layoutCard (juce::Rectangle<int> card)
{
    auto r = card.reduced (18, 14);
    wordmarkArea = r.removeFromTop (30);
    byline.setBounds (r.removeFromTop (16));
    r.removeFromTop (6);
    info.setBounds (r.removeFromTop (50));
    link.setBounds (r.removeFromTop (20));
    r.removeFromTop (10);
    creditsHeading.setBounds (r.removeFromTop (16));
    auto buttons = r.removeFromBottom (28);
    r.removeFromBottom (8);
    credits.setBounds (r);
    copyButton.setBounds (buttons.removeFromLeft (buttons.getWidth() / 2).reduced (4, 0));
    closeButton.setBounds (buttons.reduced (4, 0));
}

//==============================================================================
MessageDialog::MessageDialog (juce::String t, juce::String body, juce::Point<int> size)
    : DialogOverlay (size), title (std::move (t))
{
    text.setMultiLine (true, true);
    text.setReadOnly (true);
    text.setCaretVisible (false);
    text.setFont (metrics::labelFont());
    text.setColour (juce::TextEditor::backgroundColourId, juce::Colours::transparentBlack);
    text.setColour (juce::TextEditor::textColourId, currentTheme().textPrimary);
    text.setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    text.setColour (juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
    text.setText (body, false);
    addAndMakeVisible (text);
    okButton.onClick = [this] { dismiss(); };
    addAndMakeVisible (okButton);
}

void MessageDialog::layoutCard (juce::Rectangle<int> card)
{
    auto r = card.reduced (16, 12);
    r.removeFromTop (24);
    auto bottom = r.removeFromBottom (28);
    okButton.setBounds (bottom.removeFromRight (90));
    r.removeFromBottom (8);
    text.setBounds (r);
}

//==============================================================================
TextPromptDialog::TextPromptDialog (juce::String t, juce::String prompt, juce::String initial, juce::String okLabel,
                                    std::function<void (const juce::String&)> onOk)
    : DialogOverlay ({ 380, 170 }), title (std::move (t)), accept (std::move (onOk))
{
    promptLabel.setText (prompt, juce::dontSendNotification);
    promptLabel.setFont (metrics::smallFont());
    addAndMakeVisible (promptLabel);
    editor.setText (initial, false);
    editor.setSelectAllWhenFocused (true);
    editor.setEscapeAndReturnKeysConsumed (false);
    editor.onReturnKey = [this] { if (accept) accept (editor.getText()); };
    editor.onEscapeKey = [this] { dismiss(); };
    addAndMakeVisible (editor);
    okButton.setButtonText (okLabel);
    okButton.onClick = [this] { if (accept) accept (editor.getText()); };
    cancelButton.onClick = [this] { dismiss(); };
    addAndMakeVisible (okButton);
    addAndMakeVisible (cancelButton);
}

void TextPromptDialog::layoutCard (juce::Rectangle<int> card)
{
    auto r = card.reduced (16, 12);
    r.removeFromTop (28);
    promptLabel.setBounds (r.removeFromTop (16));
    editor.setBounds (r.removeFromTop (26));
    r.removeFromTop (14);
    auto buttons = r.removeFromBottom (28);
    okButton.setBounds (buttons.removeFromRight (90));
    buttons.removeFromRight (8);
    cancelButton.setBounds (buttons.removeFromRight (90));
}

//==============================================================================
SaveDialog::SaveDialog (juce::StringArray banks, juce::String initialName, juce::String initialBank, SaveFn save,
                        std::function<void()> onSaved)
    : DialogOverlay ({ 400, 206 }), bankList (std::move (banks)), saveFn (std::move (save)), saved (std::move (onSaved))
{
    for (auto* l : { &nameLabel, &bankLabel })
    {
        l->setFont (metrics::smallFont());
        addAndMakeVisible (*l);
    }
    nameLabel.setText ("NAME", juce::dontSendNotification);
    bankLabel.setText ("BANK", juce::dontSendNotification);

    nameEditor.setText (initialName, false);
    nameEditor.setSelectAllWhenFocused (true);
    nameEditor.setEscapeAndReturnKeysConsumed (false);
    nameEditor.onReturnKey = [this] { submit (false); };
    nameEditor.onTextChange = [this] { warning.setVisible (false); replaceButton.setVisible (false); };
    addAndMakeVisible (nameEditor);

    bankBox.addItem ("User (no bank)", 1);
    int sel = 1, id = 2;
    for (const auto& b : bankList)
    {
        bankBox.addItem (b, id);
        if (b == initialBank)
            sel = id;
        ++id;
    }
    newBankId = id;
    bankBox.addItem ("New bank...", newBankId);
    bankBox.setSelectedId (sel, juce::dontSendNotification);
    bankBox.onChange = [this]
    {
        newBankEditor.setVisible (bankBox.getSelectedId() == newBankId);
        warning.setVisible (false);
        replaceButton.setVisible (false);
        if (newBankEditor.isVisible())
            newBankEditor.grabKeyboardFocus();
    };
    addAndMakeVisible (bankBox);
    newBankEditor.setTextToShowWhenEmpty ("New bank name", currentTheme().textSecondary);
    newBankEditor.setEscapeAndReturnKeysConsumed (false);
    newBankEditor.onReturnKey = [this] { submit (false); };
    addChildComponent (newBankEditor);

    warning.setFont (metrics::smallFont());
    warning.setColour (juce::Label::textColourId, currentTheme().meterYellow);
    addChildComponent (warning);

    replaceButton.onClick = [this] { submit (true); };
    addChildComponent (replaceButton);
    cancelButton.onClick = [this] { dismiss(); };
    saveButton.onClick = [this] { submit (false); };
    addAndMakeVisible (cancelButton);
    addAndMakeVisible (saveButton);
}

juce::String SaveDialog::selectedBank() const
{
    const int id = bankBox.getSelectedId();
    if (id == 1) return {};
    if (id == newBankId) return newBankEditor.getText().trim();
    return bankList[id - 2];
}

void SaveDialog::submit (bool replace)
{
    if (! saveFn)
        return;
    const auto r = saveFn (nameEditor.getText(), selectedBank(), replace);
    if (r.ok)
    {
        if (saved)
            saved();
        dismiss();
        return;
    }
    warning.setText (r.clash ? "A preset with that name already exists in this bank." : r.error,
                     juce::dontSendNotification);
    warning.setVisible (true);
    replaceButton.setVisible (r.clash);
    resized();
}

void SaveDialog::layoutCard (juce::Rectangle<int> card)
{
    auto r = card.reduced (16, 12);
    r.removeFromTop (28);
    nameLabel.setBounds (r.removeFromTop (14));
    nameEditor.setBounds (r.removeFromTop (26));
    r.removeFromTop (8);
    bankLabel.setBounds (r.removeFromTop (14));
    auto bankRow = r.removeFromTop (26);
    if (newBankEditor.isVisible())
    {
        bankBox.setBounds (bankRow.removeFromLeft (bankRow.getWidth() / 2 - 4));
        bankRow.removeFromLeft (8);
        newBankEditor.setBounds (bankRow);
    }
    else
        bankBox.setBounds (bankRow);
    r.removeFromTop (6);
    warning.setBounds (r.removeFromTop (16));
    auto buttons = r.removeFromBottom (28);
    saveButton.setBounds (buttons.removeFromRight (90));
    buttons.removeFromRight (8);
    cancelButton.setBounds (buttons.removeFromRight (90));
    buttons.removeFromRight (8);
    replaceButton.setBounds (buttons.removeFromRight (90));
}

//==============================================================================
AccentDialog::AccentDialog (std::function<void()> onChanged)
    : DialogOverlay ({ 360, 330 }), changed (std::move (onChanged))
{
    selector.setCurrentColour (currentTheme().accent, juce::dontSendNotification);
    selector.addChangeListener (this);
    addAndMakeVisible (selector);
    resetButton.onClick = [this]
    {
        resetAccentColor();
        selector.setCurrentColour (currentTheme().accent, juce::dontSendNotification);
        if (changed)
            changed();
    };
    closeButton.onClick = [this] { dismiss(); };
    addAndMakeVisible (resetButton);
    addAndMakeVisible (closeButton);
}

AccentDialog::~AccentDialog() { selector.removeChangeListener (this); }

void AccentDialog::changeListenerCallback (juce::ChangeBroadcaster*)
{
    setAccentColor (selector.getCurrentColour().withAlpha (1.0f));
    if (changed)
        changed();
}

void AccentDialog::layoutCard (juce::Rectangle<int> card)
{
    auto r = card.reduced (16, 12);
    r.removeFromTop (28);
    auto buttons = r.removeFromBottom (28);
    r.removeFromBottom (8);
    selector.setBounds (r);
    closeButton.setBounds (buttons.removeFromRight (90));
    buttons.removeFromRight (8);
    resetButton.setBounds (buttons.removeFromRight (130));
}

} // namespace spa::ui
