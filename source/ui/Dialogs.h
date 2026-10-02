#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "../presets/PresetManager.h"
#include "Controls.h"

namespace spa::ui
{

// In-editor modal dialogs. Each is a full-size child of the editor content (so
// it scales with the window): a dimmed backdrop, a centred card, Esc or a click
// outside the card dismisses. No native windows, no modal loops.
class DialogOverlay : public juce::Component
{
public:
    explicit DialogOverlay (juce::Point<int> cardSizeIn);

    std::function<void()> onDismiss;   // the owner removes the dialog (deferred)

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;
    juce::Rectangle<int> getCardBounds() const;

protected:
    virtual void layoutCard (juce::Rectangle<int> card) = 0;
    virtual juce::String getCardTitle() const { return {}; }
    void dismiss() { if (onDismiss) onDismiss(); }

private:
    juce::Point<int> cardSize;
};

//==============================================================================
// Version / build, the factory IR credits (scrollable) and the website link.
class AboutDialog : public DialogOverlay
{
public:
    explicit AboutDialog (juce::AudioProcessor&);
    void paint (juce::Graphics&) override;

    juce::String getCreditsTextForTest() const { return credits.getText(); }
    juce::String getInfoTextForTest() const { return info.getText(); }
    juce::String buildClipboardText() const;

private:
    void layoutCard (juce::Rectangle<int>) override;
    juce::String formatString() const;

    juce::AudioProcessor& processor;
    juce::Rectangle<int> wordmarkArea;
    juce::Label byline, info, creditsHeading;
    juce::TextEditor credits;
    juce::HyperlinkButton link;
    juce::TextButton copyButton { "Copy Info" }, closeButton { "Close" };
};

//==============================================================================
class MessageDialog : public DialogOverlay
{
public:
    MessageDialog (juce::String title, juce::String body, juce::Point<int> size = { 460, 260 });
    juce::String getBodyForTest() const { return text.getText(); }

private:
    void layoutCard (juce::Rectangle<int>) override;
    juce::String getCardTitle() const override { return title; }

    juce::String title;
    juce::TextEditor text;
    juce::TextButton okButton { "OK" };
};

//==============================================================================
class TextPromptDialog : public DialogOverlay
{
public:
    TextPromptDialog (juce::String title, juce::String prompt, juce::String initial, juce::String okLabel,
                      std::function<void (const juce::String&)> onOk);
    juce::TextEditor& getEditor() { return editor; }

private:
    void layoutCard (juce::Rectangle<int>) override;
    juce::String getCardTitle() const override { return title; }

    juce::String title;
    juce::Label promptLabel;
    juce::TextEditor editor;
    juce::TextButton okButton, cancelButton { "Cancel" };
    std::function<void (const juce::String&)> accept;
};

//==============================================================================
// SAVE / SAVE AS: name + bank (the user root, an existing bank folder or a new one).
class SaveDialog : public DialogOverlay
{
public:
    // save(name, bank, replace) performs the save and returns the manager's result.
    using SaveFn = std::function<preset::PresetManager::SaveResult (const juce::String&, const juce::String&, bool)>;
    SaveDialog (juce::StringArray banks, juce::String initialName, juce::String initialBank, SaveFn save,
                std::function<void()> onSaved);

    void submit (bool replace);
    juce::TextEditor& getNameEditor() { return nameEditor; }
    juce::String getWarningForTest() const { return warning.getText(); }

private:
    void layoutCard (juce::Rectangle<int>) override;
    juce::String getCardTitle() const override { return "Save preset"; }
    juce::String selectedBank() const;

    juce::StringArray bankList;
    juce::Label nameLabel, bankLabel, warning;
    juce::TextEditor nameEditor, newBankEditor;
    juce::ComboBox bankBox;
    juce::TextButton saveButton { "Save" }, replaceButton { "Replace" }, cancelButton { "Cancel" };
    int newBankId = 0;
    SaveFn saveFn;
    std::function<void()> saved;
};

//==============================================================================
// Accent colour picker. Applies (and persists, machine-wide) live as the colour moves.
class AccentDialog : public DialogOverlay, private juce::ChangeListener
{
public:
    explicit AccentDialog (std::function<void()> onChanged);
    ~AccentDialog() override;
    juce::ColourSelector& getSelector() { return selector; }

private:
    void layoutCard (juce::Rectangle<int>) override;
    juce::String getCardTitle() const override { return "Accent colour"; }
    void changeListenerCallback (juce::ChangeBroadcaster*) override;

    std::function<void()> changed;
    juce::ColourSelector selector { juce::ColourSelector::showColourspace | juce::ColourSelector::showSliders };
    juce::TextButton resetButton { "Reset to default" }, closeButton { "Close" };
};

} // namespace spa::ui
