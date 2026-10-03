#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "../params/ParameterRegistry.h"
#include "ConvolvePanel.h"
#include "Controls.h"
#include "Dialogs.h"
#include "DraggableTabs.h"
#include "FxPanel.h"
#include "ModViz.h"
#include "PresetBrowser.h"
#include "SPAStripLookAndFeel.h"
#include "LicenceUi.h"
#include "StripPanels.h"

namespace spa
{

class SPAStripProcessor;

namespace ui
{

// Everything inside the plugin window at base size (metrics::baseWidth x
// baseHeight); the editor shell scales this whole component.
//
//   brand band   wordmark + sub-line
//   header       logo/menu | < preset > SAVE INIT undo redo | WILD RANDOMIZE ALL | OS | accent
//   FX area      12 drag-reorderable tabs (grip, bold when engaged, padlock) + the tab's panel
//   bottom row   SIDECHAIN | MODULATION (8 slots) | INPUT / OUTPUT
//   footer       version / name / maker
class ContentComponent : public juce::Component,
                         public ModAssignHost,
                         private juce::ChangeListener,
                         private juce::Timer
{
public:
    explicit ContentComponent (SPAStripProcessor&);
    ~ContentComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;
    void visibilityChanged() override { updateActive(); }
    void parentHierarchyChanged() override { updateActive(); }

    // Re-reads everything the processor owns (preset name, tab order, locks,
    // WILD, undo state, mod slots). Called on every change broadcast and the 10 Hz tick.
    void refreshAll();
    void applyTheme();                       // palette + repaint after an accent change
    std::function<void()> onThemeChanged;    // the shell refreshes its LookAndFeel palette

    // Drawer.
    void setPresetBrowserOpen (bool open, bool animate);
    bool isPresetBrowserOpen() const { return presetBrowserOpen; }

    // Dialogs (one at a time).
    void showAboutPanel();
    void showAccentPicker();
    void showMessage (const juce::String& title, const juce::String& body);
    void showSaveDialog (bool saveAs);
    void showRenameDialog (const juce::File&);
    // Generic prompts the preset drawer uses (the answer is delivered a turn after the dialog closes).
    void promptText (const juce::String& title, const juce::String& prompt, const juce::String& initial,
                     const juce::String& okLabel, std::function<void (const juce::String&)> onOk);
    void showConfirm (const juce::String& title, const juce::String& body, const juce::String& okLabel,
                      std::function<void()> onOk);
    void showClashDialog (const juce::String& presetName,
                          std::function<void (preset::PresetManager::ImportClash, bool)> decide);
    void dismissDialog();
    juce::Component* getDialogForTest() const { return dialog.get(); }

#if SPASTRIP_HAS_SPA_LICENSING
    // Licence panel (logo menu "Licence...", the brand-band badge, or a blocked
    // preset save/export in demo mode). banner = optional one-line reason.
    void showLicencePanel (const juce::String& banner = {});
    LicenceDialog* getLicenceDialogForTest() const { return dynamic_cast<LicenceDialog*> (dialog.get()); }
    LicenceBadge& getLicenceBadgeForTest() { return licenceBadge; }
    juce::Button& getLogoButtonForTest() { return logoButton; }
    juce::PopupMenu createLogoMenuForTest() { return createLogoMenu(); }
#endif

    // ModAssignHost.
    void showModAssignMenu (juce::Slider&, const juce::String& paramID) override;
    // The menu's action (also driven by tests): assign (or, when already assigned, remove)
    // `paramID` to/from `slot` as ONE undo step.
    void toggleModAssignment (const juce::String& paramID, int slot);

    // Tab model (also the test seam).
    DraggableTabs& getFxTabs() { return fxTabs; }
    juce::Array<int> getTabOrder() const { return fxTabs.currentOrder(); }
    int getModuleForTabName (const juce::String& name) const { return fxTabs.getModuleNames().indexOf (name); }
    void setLockedFromTab (const juce::String& tabName);   // padlock click
    void selectTabForModule (int moduleId);
    int getSelectedModule() const;

    PresetBrowser& getPresetBrowser() { return *presetBrowser; }
    ModMatrixPanel& getModPanel() { return modPanel; }
    SidechainPanel& getSidechainPanel() { return sidechainPanel; }
    IoPanel& getIoPanel() { return ioPanel; }
    ConvolvePanel* getConvolvePanel() { return convolvePanel; }
    juce::TextButton& getUndoButton() { return undoButton; }
    juce::TextButton& getRedoButton() { return redoButton; }
    juce::TextButton& getRandomizeButton() { return randomizeButton; }
    juce::Slider& getWildSlider() { return wildSlider; }
    juce::String getPresetButtonText() const { return presetNameButton.getButtonText(); }
    ModVizDriver& getModViz() { return modViz; }
    // Runs one 10 Hz housekeeping tick synchronously.
    void tickForTest() { timerCallback(); }

    // Names of every parameter that has a control in this editor (APVTS attachments),
    // collected by walking the component tree. Used by the coverage test.
    juce::StringArray collectControlledParameterIDs() const;

private:
    struct AccentButton : juce::Button
    {
        AccentButton() : juce::Button ("accent") {}
        void paintButton (juce::Graphics&, bool highlighted, bool down) override;
    };
    struct LogoButton : juce::Button
    {
        LogoButton() : juce::Button ("logo") {}
        void paintButton (juce::Graphics&, bool highlighted, bool down) override;
    };

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void updateActive();
    void updateUndoButtons();
    void syncTabOrder();
    void showLogoMenu();
    juce::PopupMenu createLogoMenu();
#if SPASTRIP_HAS_SPA_LICENSING
    LicenceBadge licenceBadge;
    void refreshLicenceBadge();
    juce::String licenceStateLine();
#endif
    void onSaveClicked();
    void showDialog (std::unique_ptr<DialogOverlay>);
    void tabChanged();
    bool isTabLocked (const juce::String& name) const;
    void layoutDrawer();
    void saveCurrentInPlace();

    SPAStripProcessor& processor;
    ModVizDriver modViz;

    std::unique_ptr<juce::Drawable> logo;
    LogoButton logoButton;
    juce::Image noiseTexture;

    juce::TextButton prevPresetButton { "<" }, nextPresetButton { ">" };
    juce::TextButton presetNameButton, savePresetButton { "SAVE" }, initButton { "INIT" };
    juce::TextButton undoButton, redoButton;
    juce::TextButton randomizeButton { "RANDOMIZE ALL" };
    juce::Slider wildSlider;
    juce::Label wildLabel;
    Choice oversampling;
    AccentButton accentButton;
    juce::Rectangle<int> osCaptionRect;

    DraggableTabs fxTabs;
    std::unique_ptr<TabEngagementTracker> tabEngagement;
    ConvolvePanel* convolvePanel = nullptr;   // owned by fxTabs
    SidechainPanel sidechainPanel;
    ModMatrixPanel modPanel;
    IoPanel ioPanel;

    std::unique_ptr<PresetBrowser> presetBrowser;
    bool presetBrowserOpen = false;
    bool saveFolderAutoSticky = false;   // "Auto (by type)" stays selected for the next Save As
    std::unique_ptr<DialogOverlay> dialog;
    std::unique_ptr<juce::ComponentAnimator> animator;

    // Layout captured in resized() for paint().
    int seamHeaderY = 0, seamBottomY = 0, seamFooterY = 0;
    std::vector<int> gutterXs;
    juce::Rectangle<int> tabHintRect;

    // One undo step for a whole tab drag (the swaps share it).
    std::unique_ptr<SPAStripProcessor::UndoStep> tabDragStep;
    int lastShownEdited = -1;
    juce::Colour lastAccent;
    juce::uint32 lastLockMask = 0xffffffff;
    juce::String lastPresetText;
    bool active = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ContentComponent)
};

} // namespace ui

// The plugin editor: hosts the fixed-layout content at base size and scales it
// proportionally (fixed aspect, 50%..200%). The window scale is remembered in
// the plugin state ("uiScale").
class SPAStripEditor : public juce::AudioProcessorEditor
{
public:
    explicit SPAStripEditor (SPAStripProcessor&);
    ~SPAStripEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void parentHierarchyChanged() override;

    ui::ContentComponent& getContent() { return *content; }
    float getScale() const;

    // Largest scale <= 1.0 (quantised down to 0.05) at which baseW x baseH plus a
    // host-chrome allowance fits `userArea`; never below minScale.
    static float scaleThatFits (juce::Rectangle<int> userArea, int baseW, int baseH);
    static constexpr float minScale = 0.5f, maxScale = 2.0f;
    int getResizedCallCountForTest() const { return resizedCalls; }

private:
    SPAStripProcessor& stripProcessor;
    ui::SPAStripLookAndFeel lookAndFeel;
    juce::TooltipWindow tooltips { this };
    std::unique_ptr<ui::ContentComponent> content;
    int resizedCalls = 0;
    bool suppressScaleSave = false;
    bool screenFitCheckDone = false;
    bool hostViewWakeupDone = false;
    bool trialStartNoted = false;   // licensing builds only

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SPAStripEditor)
};

} // namespace spa
