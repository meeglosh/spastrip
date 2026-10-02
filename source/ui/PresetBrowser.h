#pragma once

#include "../SPAStripProcessor.h"
#include "../presets/PresetManager.h"
#include "Controls.h"

namespace spa::ui
{

// Preset drawer: slides in from the left over the effects area. It manages
// presets the way SPASynth's drawer does, with effect types (Drums, Vocals,
// Mixbus ...) where the synth has sound types:
//   * live search (name, bank / folder, type), a TYPE dropdown that lists only the
//     types present, a bank dropdown and a favourites star filter;
//   * the list is GROUPED -- by folder (User, its folders and nested folders, then
//     Factory) or by type (Drums ... Creative, then Other for presets with no type);
//     group rows collapse (remembered), search forces them open;
//   * click a row to load it, the star toggles a favourite, right-click for Rename /
//     Move to Trash / Export / Move to Folder / Set type; right-click a folder row for
//     New / Rename / Trash / Export folder; drag rows onto folders to move them;
//   * IMPORT takes .spastrip files, folders and .zip packs (also dropped on the drawer).
// Factory rows are read-only everywhere. Esc closes the drawer; Up / Down move the
// highlight and Return loads it.
class PresetBrowser : public juce::Component,
                      public juce::DragAndDropContainer,
                      public juce::FileDragAndDropTarget,
                      private juce::ChangeListener,
                      private juce::ListBoxModel
{
public:
    // Hooks the editor provides (dialogs / lifetime live in the editor: no native
    // windows, no modal loops).
    struct Hooks
    {
        std::function<void()> onClose;
        std::function<void (const juce::File&)> promptRename;
        std::function<void (const juce::String& title, const juce::String& prompt, const juce::String& initial,
                            const juce::String& okLabel, std::function<void (const juce::String&)> onOk)> promptText;
        std::function<void (const juce::String& title, const juce::String& body, const juce::String& okLabel,
                            std::function<void()> onOk)> confirm;
        std::function<void (const juce::String& presetName,
                            std::function<void (preset::PresetManager::ImportClash, bool applyToRest)> decide)> askClash;
        std::function<void (const juce::String& title, const juce::String& message)> showMessage;
    };

    PresetBrowser (SPAStripProcessor&, Hooks);
    ~PresetBrowser() override;

    //--- pure filtering / grouping (testable without a UI) -----------------------
    using Info = preset::PresetManager::PresetInfo;

    struct Filter
    {
        juce::String search;        // case-insensitive substring of name / bank / folder / type
        juce::String category;      // "" = all; "User" (unfiled), a bank name, or a factory bank
        juce::String type;          // "" = all types; a preset type; or "Other" for untyped presets
        bool favoritesOnly = false;
    };

    // The label a preset is listed under: its type, or "Other".
    static juce::String typeLabelOf (const Info&);
    // Top-level grouping name: "User" for unfiled presets, else the bank.
    static juce::String categoryOf (const Info&);
    static std::vector<int> filterIndices (const std::vector<Info>&, const Filter&, const juce::StringArray& favouriteKeys);
    // The types actually present among presets matching every filter field EXCEPT
    // type (its own value is ignored): presetTypes() order, then "Other".
    static juce::StringArray availableTypes (const std::vector<Info>&, const Filter& excludingType,
                                             const juce::StringArray& favouriteKeys);

    enum class GroupBy { folders = 0, type = 1 };

    struct DisplayRow
    {
        enum class Kind { preset, userRoot, userFolder, factoryRoot, factoryBank, type };
        Kind kind = Kind::preset;
        int presetIndex = -1;       // preset rows: index into the manager's list
        juce::String key;           // group rows: persisted collapse key
        juce::String label;
        juce::String relFolder;     // userRoot ("") / userFolder: path below User/
        int depth = 0;
        int count = 0;              // group rows: matching presets inside
        bool expanded = true;
        bool isGroup() const { return kind != Kind::preset; }
    };

    // Pure: the display rows for the given (already filtered) preset indices.
    // collapsedKeys = persisted collapsed groups; searchActive forces every group
    // open; anyFilterActive hides groups with nothing matching (with NO filter,
    // empty user folders still show so they can be dropped into).
    static std::vector<DisplayRow> buildRows (const std::vector<Info>&, const std::vector<int>& filteredIndices,
                                              const juce::StringArray& userFolders, GroupBy,
                                              const juce::StringArray& collapsedKeys, bool searchActive,
                                              bool anyFilterActive);

    //--- list ---------------------------------------------------------------------
    void refresh();                     // re-pull presets, rebuild rows (keeps filters)
    void scrollToCurrent();             // bring the loaded preset's row into view
    int getNumRows() override { return (int) rows.size(); }
    const DisplayRow& getRow (int i) const { return rows[(size_t) i]; }
    const Info* presetAtRow (int row) const;          // null for group rows
    int findGroupRow (const juce::String& key) const; // -1 if not shown
    int findVisibleRow (const juce::String& presetName) const;   // -1 if hidden (collapsed / filtered)
    void toggleGroupRow (int row);
    GroupBy getGroupBy() const { return groupBy; }
    void setGroupBy (GroupBy);
    void setSearchText (const juce::String&);
    // Set the dropdown filters by label ("" = All). false when that entry is not offered.
    bool setTypeFilter (const juce::String& type);
    bool setCategoryFilter (const juce::String& category);
    void setFavouritesOnly (bool);
    juce::StringArray getOfferedTypes() const;       // what the TYPE dropdown lists (without "All Types")
    juce::String getCountText() const { return countLabel.getText(); }
    // The preset list as shown (ignores collapsing and filters): what the prev / next
    // arrows step through. Pushed to the manager on every rebuild.
    std::vector<juce::String> currentNavigationOrder() const;

    void setOpenBounds (juce::Rectangle<int> b) { openBounds = b; }
    juce::Rectangle<int> getOpenBounds() const { return openBounds; }

    //--- preset actions -----------------------------------------------------------
    static constexpr int renameMenuItemId = 1;
    static constexpr int deleteMenuItemId = 2;
    static constexpr int exportMenuItemId = 3;
    static constexpr int newFolderMenuItemId = 4;
    static constexpr int renameFolderMenuItemId = 5;
    static constexpr int trashFolderMenuItemId = 6;
    static constexpr int exportFolderMenuItemId = 7;
    static constexpr int moveToRootMenuItemId = 8;
    static constexpr int moveNewFolderMenuItemId = 9;
    static constexpr int clearTypeMenuItemId = 10;
    static constexpr int revealMenuItemId = 11;
    static constexpr int firstTypeMenuItemId = 1000;     // one id per preset type
    static constexpr int firstMoveFolderMenuItemId = 3000;   // one id per user folder

    bool canModifyRow (int row) const;               // a user preset (not factory / group)
    // typeNamesOut / moveFoldersOut: filled in the order the submenu items were added.
    juce::PopupMenu buildRowMenu (int row, juce::StringArray* typeNamesOut = nullptr,
                                  juce::StringArray* moveFoldersOut = nullptr) const;
    juce::PopupMenu buildGroupMenu (int row) const;    // empty when it makes no sense (Type grouping)
    juce::PopupMenu buildBackgroundMenu() const;
    bool deleteRow (int row);                          // trashes + cleans the favourite
    bool applyTypeToRow (int row, const juce::String& type);   // "" / "Other" clears it
    void toggleFavouriteRow (int row);

    preset::PresetManager::OpResult createFolder (const juce::String& parentRel, const juce::String& name);
    preset::PresetManager::OpResult renameFolder (const juce::String& rel, const juce::String& newName);
    preset::PresetManager::OpResult trashFolder (const juce::String& rel);
    void promptNewFolder (const juce::String& parentRel, std::function<void (const juce::String&)> onCreated = {});
    void promptRenameFolder (const juce::String& rel);
    void confirmTrashFolder (const juce::String& rel);

    struct MoveSummary { int moved = 0; juce::StringArray refused; };
    // Moves user preset files into destRel ("" = User root). Refused files (clash,
    // factory ...) are listed with the reason and left in place; the others still move.
    MoveSummary moveFilesToFolder (const juce::Array<juce::File>& files, const juce::String& destRel, bool notify = true);
    // The user folder a drop / the menu on this row would target; false when the row
    // is not a valid destination (factory, Type grouping ...).
    bool dropTargetFolderForRow (int row, juce::String& relFolderOut) const;

    void startImport();                                // opens the file chooser
    void importFromPaths (const juce::Array<juce::File>& paths);
    void continueImport (std::shared_ptr<preset::PresetManager::ImportSession> session);
    void exportRow (int row);
    void exportFolderRel (const juce::String& rel);

    //--- Component ----------------------------------------------------------------
    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;
    void fileDragEnter (const juce::StringArray&, int, int) override { draggingOver = true; repaint(); }
    void fileDragExit (const juce::StringArray&) override { draggingOver = false; repaint(); }

    // Public (a ListBoxModel override) so tests can ask what a drag of these rows would carry.
    juce::var getDragSourceDescription (const juce::SparseSet<int>&) override;
    bool dragInterested (const juce::DragAndDropTarget::SourceDetails&) const;
    void dragMoved (const juce::DragAndDropTarget::SourceDetails&);
    void dragExited();
    void dragDropped (const juce::DragAndDropTarget::SourceDetails&);

    // Test access.
    juce::TextEditor& getSearchBoxForTest() { return searchBox; }
    juce::ComboBox& getTypeBoxForTest() { return typeBox; }
    juce::ComboBox& getCategoryBoxForTest() { return categoryBox; }
    juce::ListBox& getListForTest() { return list; }
    void selectRowForTest (int row) { list.selectRow (row); }
    bool returnKeyPressedForTest (int row) { returnKeyPressed (row); return true; }

private:
    class StarToggle;
    class DropList : public juce::ListBox, public juce::DragAndDropTarget
    {
    public:
        using juce::ListBox::ListBox;
        PresetBrowser* owner = nullptr;
        bool isInterestedInDragSource (const SourceDetails& d) override { return owner != nullptr && owner->dragInterested (d); }
        void itemDragEnter (const SourceDetails& d) override { if (owner) owner->dragMoved (d); }
        void itemDragMove (const SourceDetails& d) override { if (owner) owner->dragMoved (d); }
        void itemDragExit (const SourceDetails&) override { if (owner) owner->dragExited(); }
        void itemDropped (const SourceDetails& d) override { if (owner) owner->dragDropped (d); }
    };

    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }
    void paintListBoxItem (int row, juce::Graphics&, int w, int h, bool selected) override;
    void listBoxItemClicked (int row, const juce::MouseEvent&) override;
    void returnKeyPressed (int row) override;
    void backgroundClicked (const juce::MouseEvent&) override;
    void applyFilter();
    void rebuildCombos (const Filter& filterWithoutType);
    void showRowMenu (int row);
    void showGroupMenu (int row);
    void showBackgroundMenu();
    void applyMoveFromMenu (int row, int result, const juce::StringArray& folders);
    void message (const juce::String& title, const juce::String& body);
    Filter currentFilter() const;

    SPAStripProcessor& processor;
    Hooks hooks;
    juce::TextButton closeButton { juce::String::fromUTF8 ("\xc3\x97") };
    juce::TextButton groupFoldersButton { "FOLDERS" }, groupTypeButton { "TYPE" };
    juce::TextEditor searchBox;
    juce::ComboBox typeBox, categoryBox;
    std::unique_ptr<StarToggle> favouritesChip;
    DropList list { {}, this };
    juce::Label countLabel;
    juce::TextButton initButton { "INIT" }, newFolderButton { "NEW FOLDER" }, importButton { "IMPORT..." };
    std::unique_ptr<juce::FileChooser> chooser;

    GroupBy groupBy = GroupBy::folders;
    std::vector<Info> presets;               // snapshot
    std::vector<int> filtered;               // indices into presets
    std::vector<DisplayRow> rows;            // what the ListBox shows
    juce::StringArray userFolderList, favouriteKeys, offeredTypes;
    juce::String lastRevealed;
    int dropRow = -1;
    bool draggingOver = false;
    juce::Rectangle<int> openBounds, titleArea, listWell;

    static constexpr int shadowWidth = 10;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PresetBrowser)
};

} // namespace spa::ui
