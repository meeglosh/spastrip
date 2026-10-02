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

void DialogOverlay::setCardSize (juce::Point<int> newSize)
{
    if (newSize == cardSize)
        return;
    cardSize = newSize;
    resized();
    repaint();
}

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
ConfirmDialog::ConfirmDialog (juce::String t, juce::String body, juce::String okLabel, std::function<void()> onOk,
                              juce::Point<int> size)
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
    okButton.setButtonText (okLabel);
    // The action runs a turn later: it may open another dialog, which would otherwise
    // destroy this one (and this very handler) while it is still executing.
    okButton.onClick = [this, action = std::move (onOk)]
    {
        dismiss();
        if (action)
            juce::MessageManager::callAsync (action);
    };
    cancelButton.onClick = [this] { dismiss(); };
    addAndMakeVisible (okButton);
    addAndMakeVisible (cancelButton);
}

void ConfirmDialog::layoutCard (juce::Rectangle<int> card)
{
    auto r = card.reduced (16, 12);
    r.removeFromTop (24);
    auto buttons = r.removeFromBottom (28);
    r.removeFromBottom (8);
    text.setBounds (r);
    okButton.setBounds (buttons.removeFromRight (120));
    buttons.removeFromRight (8);
    cancelButton.setBounds (buttons.removeFromRight (90));
}

//==============================================================================
ClashDialog::ClashDialog (const juce::String& presetName, Decide d)
    : DialogOverlay ({ 420, 150 }), decide (std::move (d))
{
    message.setText ("\"" + presetName + "\" already exists in the destination.", juce::dontSendNotification);
    message.setFont (metrics::labelFont());
    message.setMinimumHorizontalScale (1.0f);
    addAndMakeVisible (message);
    applyToRest.setColour (juce::ToggleButton::textColourId, currentTheme().textSecondary);
    addAndMakeVisible (applyToRest);
    replaceButton.onClick = [this] { choose (preset::PresetManager::ImportClash::replace); };
    keepButton.onClick = [this] { choose (preset::PresetManager::ImportClash::keepBoth); };
    skipButton.onClick = [this] { choose (preset::PresetManager::ImportClash::skip); };
    for (auto* b : { &replaceButton, &keepButton, &skipButton })
        addAndMakeVisible (*b);
}

void ClashDialog::choose (preset::PresetManager::ImportClash action)
{
    // The owner defers the dialog's destruction; the decision is delivered first.
    const auto all = applyToRest.getToggleState();
    auto cb = std::move (decide);
    decide = nullptr;
    dismiss();
    if (cb)
        juce::MessageManager::callAsync ([cb, action, all] { cb (action, all); });
}

void ClashDialog::layoutCard (juce::Rectangle<int> card)
{
    auto r = card.reduced (16, 12);
    r.removeFromTop (28);
    message.setBounds (r.removeFromTop (20));
    r.removeFromTop (6);
    applyToRest.setBounds (r.removeFromTop (24));
    auto buttons = r.removeFromBottom (28);
    skipButton.setBounds (buttons.removeFromRight (90));
    buttons.removeFromRight (8);
    keepButton.setBounds (buttons.removeFromRight (100));
    buttons.removeFromRight (8);
    replaceButton.setBounds (buttons.removeFromRight (90));
}

//==============================================================================
SaveDialog::SaveDialog (const Init& init, SaveFn save, DefaultNameFn defaultName, NameTakenFn nameTaken,
                        std::function<void()> onSaved)
    : DialogOverlay ({ 400, 300 }), folders (init.userFolders), nameOwned (init.nameIsOwned),
      saveFn (std::move (save)), defaultNameFn (std::move (defaultName)), nameTakenFn (std::move (nameTaken)),
      saved (std::move (onSaved))
{
    for (auto* l : { &nameLabel, &typeLabel, &folderLabel })
    {
        l->setFont (metrics::smallFont());
        addAndMakeVisible (*l);
    }
    nameLabel.setText ("NAME", juce::dontSendNotification);
    typeLabel.setText ("TYPE", juce::dontSendNotification);
    folderLabel.setText ("FOLDER", juce::dontSendNotification);

    nameEditor.setText (init.initialName, false);
    nameEditor.setSelectAllWhenFocused (true);
    nameEditor.setEscapeAndReturnKeysConsumed (false);
    nameEditor.onReturnKey = [this] { submit (false); };
    nameEditor.onTextChange = [this] { nameOwned = true; hideWarning(); };   // user typing only: setText below passes false
    addAndMakeVisible (nameEditor);

    typeBox.addItem ("(none)", 1);
    int id = 2, selType = 1;
    for (const auto& t : preset::PresetManager::presetTypes())
    {
        typeBox.addItem (t, id);
        if (t == init.initialType)
            selType = id;
        ++id;
    }
    typeBox.setSelectedId (selType, juce::dontSendNotification);
    typeBox.onChange = [this] { hideWarning(); refreshDefaultName(); updateHint(); };
    addAndMakeVisible (typeBox);

    hint.setFont (metrics::smallFont());
    hint.setColour (juce::Label::textColourId, currentTheme().textSecondary);
    hint.setJustificationType (juce::Justification::topLeft);
    hint.setMinimumHorizontalScale (1.0f);
    addAndMakeVisible (hint);

    folderBox.addItem ("User (unfiled)", 1);
    int fid = 2, selFolder = 1;
    for (const auto& f : folders)
    {
        folderBox.addItem (f.replace ("/", "  /  "), fid);
        if (f == init.defaultFolder)
            selFolder = fid;
        ++fid;
    }
    autoItemId = fid++;
    folderBox.addItem ("Auto (by type)", autoItemId);
    newFolderItemId = fid;
    folderBox.addItem ("New folder...", newFolderItemId);
    folderBox.setSelectedId (init.startAuto ? autoItemId : selFolder, juce::dontSendNotification);
    folderBox.onChange = [this]
    {
        const bool show = folderBox.getSelectedId() == newFolderItemId;
        newFolderEditor.setVisible (show);
        hideWarning();
        fit();
        refreshDefaultName();
        if (show)
            newFolderEditor.grabKeyboardFocus();
    };
    addAndMakeVisible (folderBox);
    newFolderEditor.setTextToShowWhenEmpty ("New folder name", currentTheme().textSecondary);
    newFolderEditor.setEscapeAndReturnKeysConsumed (false);
    newFolderEditor.onReturnKey = [this] { submit (false); };
    newFolderEditor.onTextChange = [this] { hideWarning(); refreshDefaultName(); };
    addChildComponent (newFolderEditor);

    warning.setFont (metrics::smallFont());
    warning.setColour (juce::Label::textColourId, currentTheme().meterYellow);
    warning.setJustificationType (juce::Justification::topLeft);
    warning.setMinimumHorizontalScale (1.0f);
    addChildComponent (warning);

    replaceButton.onClick = [this] { submit (true); };
    addChildComponent (replaceButton);
    cancelButton.onClick = [this] { dismiss(); };
    saveButton.onClick = [this] { submit (false); };
    addAndMakeVisible (cancelButton);
    addAndMakeVisible (saveButton);

    updateHint();
    fit();
}

juce::String SaveDialog::currentType() const
{
    return typeBox.getSelectedId() > 1 ? typeBox.getText() : juce::String();
}

juce::String SaveDialog::currentFolder() const
{
    const auto sel = folderBox.getSelectedId();
    if (sel == autoItemId)
        return currentType();   // User/<Type>/ (created on demand); no type -> unfiled
    if (sel == newFolderItemId)
        return preset::PresetManager::sanitiseFileName (newFolderEditor.getText());
    if (sel > 1)
        return folders[sel - 2];
    return {};
}

void SaveDialog::refreshDefaultName()
{
    if (nameOwned || ! defaultNameFn)
        return;
    nameEditor.setText (defaultNameFn (currentType(), currentFolder()), false);
    nameEditor.selectAll();
}

void SaveDialog::updateHint()
{
    const auto t = currentType();
    hint.setText ("Shows under " + (t.isEmpty() ? juce::String (preset::PresetManager::otherTypeLabel) : t).toUpperCase()
                      + " when the browser is grouped by type.",
                  juce::dontSendNotification);
}

void SaveDialog::hideWarning()
{
    if (! warning.isVisible() && ! replaceButton.isVisible())
        return;
    warning.setVisible (false);
    replaceButton.setVisible (false);
    fit();
}

void SaveDialog::showWarning (const juce::String& text, bool offerReplace)
{
    warning.setText (text, juce::dontSendNotification);
    warning.setVisible (true);
    replaceButton.setVisible (offerReplace);
    fit();
}

void SaveDialog::submit (bool replace)
{
    if (! saveFn)
        return;
    const auto sel = folderBox.getSelectedId();
    if (sel == newFolderItemId && currentFolder().isEmpty())
    {
        showWarning ("Enter a name for the new folder.", false);
        return;
    }

    SaveRequest req;
    req.name = nameEditor.getText().trim();
    req.type = currentType();
    req.folder = currentFolder();
    req.replace = replace;
    req.autoFolder = sel == autoItemId;
    req.createFolder = sel == autoItemId || sel == newFolderItemId;

    if (req.name.isEmpty())
    {
        showWarning ("Enter a name.", false);
        return;
    }
    const auto clash = [&] { return "A preset called \"" + req.name + "\" already exists in this folder. Change the name, or replace it (the old one goes to the Trash)."; };
    if (! replace && nameTakenFn && nameTakenFn (req.name, req.folder))
    {
        showWarning (clash(), true);
        return;
    }

    const auto res = saveFn (req);
    if (res.ok)
    {
        if (saved)
            saved();
        dismiss();
    }
    else if (res.clash)
        showWarning (clash(), true);
    else
        showWarning (res.error.isEmpty() ? juce::String ("The preset could not be saved.") : res.error, false);
}

int SaveDialog::contentHeight() const
{
    int h = 12 + 28;                 // card padding + title
    h += 14 + 26 + 8;                // NAME
    h += 14 + 26 + 4 + 28 + 8;       // TYPE + hint
    h += 14 + 26 + 8;                // FOLDER
    if (newFolderEditor.isVisible())
        h += 26 + 8;
    if (warning.isVisible())
    {
        h += 42 + 6;
        if (replaceButton.isVisible())
            h += 28 + 8;
    }
    h += 28 + 12;                    // buttons + card padding
    return h;
}

void SaveDialog::fit()
{
    setCardSize ({ 400, contentHeight() });
    resized();
}

void SaveDialog::layoutCard (juce::Rectangle<int> card)
{
    auto r = card.reduced (16, 12);
    r.removeFromTop (28);
    nameLabel.setBounds (r.removeFromTop (14));
    nameEditor.setBounds (r.removeFromTop (26));
    r.removeFromTop (8);
    typeLabel.setBounds (r.removeFromTop (14));
    typeBox.setBounds (r.removeFromTop (26));
    r.removeFromTop (4);
    hint.setBounds (r.removeFromTop (28));
    r.removeFromTop (8);
    folderLabel.setBounds (r.removeFromTop (14));
    folderBox.setBounds (r.removeFromTop (26));
    r.removeFromTop (8);
    if (newFolderEditor.isVisible())
    {
        newFolderEditor.setBounds (r.removeFromTop (26));
        r.removeFromTop (8);
    }
    if (warning.isVisible())
    {
        warning.setBounds (r.removeFromTop (42));
        r.removeFromTop (6);
        if (replaceButton.isVisible())
        {
            replaceButton.setBounds (r.removeFromTop (28).removeFromLeft (110));
            r.removeFromTop (8);
        }
    }
    auto buttons = r.removeFromBottom (28);
    saveButton.setBounds (buttons.removeFromRight (90));
    buttons.removeFromRight (8);
    cancelButton.setBounds (buttons.removeFromRight (90));
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
