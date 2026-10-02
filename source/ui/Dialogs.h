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
    juce::Point<int> getCardSize() const { return cardSize; }

protected:
    // For dialogs whose content changes height (the save panel's optional rows).
    void setCardSize (juce::Point<int> newSize);
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
// Asks before something is moved to the Trash / replaced. OK runs `onOk` (deferred
// by the owner); Esc / Cancel / a click outside only dismiss.
class ConfirmDialog : public DialogOverlay
{
public:
    ConfirmDialog (juce::String title, juce::String body, juce::String okLabel, std::function<void()> onOk,
                   juce::Point<int> size = { 420, 150 });

    void accept() { okButton.onClick(); }   // the OK button (also the test entry point)
    juce::String getBodyForTest() const { return text.getText(); }

private:
    void layoutCard (juce::Rectangle<int>) override;
    juce::String getCardTitle() const override { return title; }

    juce::String title;
    juce::TextEditor text;
    juce::TextButton okButton, cancelButton { "Cancel" };
};

//==============================================================================
// Importing a preset whose name is already taken: Replace / Keep Both / Skip, with
// "do this for every clash in this import".
class ClashDialog : public DialogOverlay
{
public:
    using Decide = std::function<void (preset::PresetManager::ImportClash, bool applyToRest)>;
    ClashDialog (const juce::String& presetName, Decide decide);

    void choose (preset::PresetManager::ImportClash);   // also the test entry point
    void setApplyToRest (bool on) { applyToRest.setToggleState (on, juce::dontSendNotification); }

private:
    void layoutCard (juce::Rectangle<int>) override;
    juce::String getCardTitle() const override { return "Preset already exists"; }

    Decide decide;
    juce::Label message;
    juce::ToggleButton applyToRest { "Do this for every clash in this import" };
    juce::TextButton replaceButton { "Replace" }, keepButton { "Keep both" }, skipButton { "Skip" };
};

//==============================================================================
// SAVE / SAVE AS (the synth's save panel, in-editor): NAME, TYPE (the preset types, or
// none), FOLDER (unfiled, the user's folders, "Auto (by type)", "New folder...") with a
// hint saying where the preset will show under the type grouping, Cancel / Save.
// A clash shows an inline warning with Replace (the old one goes to the Trash).
// The default name follows the TYPE ("Drums 1", "Drums 2" ...) until the user types
// one of their own; a patch that came from a preset starts from that preset's name.
struct SaveRequest
{
    juce::String name, type, folder;
    bool replace = false, createFolder = false, autoFolder = false;
};

class SaveDialog : public DialogOverlay
{
public:
    struct Init
    {
        juce::StringArray userFolders;
        juce::String defaultFolder;    // pre-selected folder ("" = User root)
        juce::String initialType;      // "" = (none)
        juce::String initialName;
        bool nameIsOwned = false;      // a preset's own name: TYPE changes never replace it
        bool startAuto = false;        // "Auto (by type)" kept from the last save
    };
    using SaveFn = std::function<preset::PresetManager::SaveResult (const SaveRequest&)>;
    using DefaultNameFn = std::function<juce::String (const juce::String& type, const juce::String& folder)>;
    using NameTakenFn = std::function<bool (const juce::String& name, const juce::String& folder)>;

    SaveDialog (const Init&, SaveFn save, DefaultNameFn defaultName, NameTakenFn nameTaken,
                std::function<void()> onSaved);

    void submit (bool replace);
    juce::TextEditor& getNameEditor() { return nameEditor; }
    juce::ComboBox& getTypeBox() { return typeBox; }
    juce::ComboBox& getFolderBox() { return folderBox; }
    juce::TextEditor& getNewFolderEditor() { return newFolderEditor; }
    juce::String getWarningForTest() const { return warning.getText(); }
    juce::String getHintForTest() const { return hint.getText(); }
    bool isReplaceVisibleForTest() const { return replaceButton.isVisible(); }
    bool isNameOwnedForTest() const { return nameOwned; }
    int getAutoItemId() const { return autoItemId; }
    int getNewFolderItemId() const { return newFolderItemId; }

private:
    void layoutCard (juce::Rectangle<int>) override;
    juce::String getCardTitle() const override { return "Save preset"; }
    int contentHeight() const;
    void fit();
    juce::String currentType() const;
    juce::String currentFolder() const;
    void refreshDefaultName();
    void updateHint();
    void hideWarning();
    void showWarning (const juce::String& text, bool offerReplace);

    juce::StringArray folders;
    juce::Label nameLabel, typeLabel, folderLabel, hint, warning;
    juce::TextEditor nameEditor, newFolderEditor;
    juce::ComboBox typeBox, folderBox;
    juce::TextButton saveButton { "Save" }, replaceButton { "Replace" }, cancelButton { "Cancel" };
    int autoItemId = 0, newFolderItemId = 0;
    bool nameOwned = false;
    SaveFn saveFn;
    DefaultNameFn defaultNameFn;
    NameTakenFn nameTakenFn;
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
