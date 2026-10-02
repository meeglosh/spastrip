#pragma once

#include "../SPAStripProcessor.h"
#include "../presets/PresetManager.h"
#include "Controls.h"

namespace spa::ui
{

// Preset drawer: slides in from the left over the effects area. Live search,
// banks as collapsible groups (the User root first, then each bank folder),
// click a row to load it, right-click for Rename / Delete (to the Trash) /
// Reveal. Footer: INIT, IMPORT SPASYNTH PRESET..., and the last import's
// result summary.
class PresetBrowser : public juce::Component,
                      private juce::ChangeListener,
                      private juce::ListBoxModel
{
public:
    // Hooks the editor provides (dialogs / lifetime live in the editor).
    struct Hooks
    {
        std::function<void()> onClose;
        std::function<void (const preset::PresetManager::ImportResult&)> onImported;   // show the details dialog
        std::function<void (const juce::File&)> promptRename;
        std::function<void (const juce::String& message)> showMessage;
    };

    PresetBrowser (SPAStripProcessor&, Hooks);
    ~PresetBrowser() override;

    struct Row
    {
        bool isGroup = false;
        juce::String label;       // group: bank name ("User" for the root); preset: its name
        int presetIndex = -1;     // index into PresetManager::getPresets()
        int count = 0;
        bool expanded = true;
    };

    void refresh();                     // re-pull presets, rebuild rows
    int getNumRows() override { return (int) rows.size(); }
    const Row& getRow (int i) const { return rows[(size_t) i]; }
    void setSearchText (const juce::String&);

    void setOpenBounds (juce::Rectangle<int> b) { openBounds = b; }
    juce::Rectangle<int> getOpenBounds() const { return openBounds; }

    // Last import summary line ("" until something was imported).
    void setImportSummary (const juce::String& s);
    juce::String getImportSummary() const { return importSummary; }
    void startImport();                 // opens the file chooser
    void importFile (const juce::File&);

    static juce::String describeImport (const preset::PresetManager::ImportResult&);

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }
    void paintListBoxItem (int row, juce::Graphics&, int w, int h, bool selected) override;
    void listBoxItemClicked (int row, const juce::MouseEvent&) override;
    void showRowMenu (int row);
    void rebuildRows();

    SPAStripProcessor& processor;
    Hooks hooks;
    juce::TextButton closeButton { juce::String::fromUTF8 ("\xc3\x97") };
    juce::TextEditor searchBox;
    juce::ListBox list { {}, this };
    juce::TextButton initButton { "INIT" }, importButton { "IMPORT SPASYNTH PRESET..." };
    juce::Label countLabel, summaryLabel;
    std::unique_ptr<juce::FileChooser> chooser;
    std::vector<Row> rows;
    juce::StringArray collapsed;
    juce::String importSummary;
    juce::Rectangle<int> openBounds, titleArea, listWell;

    static constexpr int shadowWidth = 10;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PresetBrowser)
};

} // namespace spa::ui
