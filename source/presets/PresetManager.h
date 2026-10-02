#pragma once

#include <atomic>
#include <functional>
#include <vector>

#include "../SPAStripProcessor.h"

namespace spa::preset
{

// Preset save / load / browse for SPAStrip (message thread only, except where
// noted). Managed the way SPASynth's library::PresetManager manages its presets
// (XML wrapping the state tree, user folders, a stored "type", the edited flag as
// "state != what was loaded/saved", favourites, export / import), with effect
// types instead of sound types and without the synth's sample-library coupling.
//
// FILE FORMAT  (*.spastrip)
//   <SPAStripPreset name="Fire Pad" version="1" type="Mixbus">
//     <PARAMS fxOrder="..." irSource="none|embedded|factory:<id>" convIRName="..."
//             modSlot1Target="fxChorus.rate" ... stateVersion="1">
//       <PARAM id="fxDist.drive" value="0.3"/> ...
//       <IR format="flac24" .../>        (only for an embedded user IR: the audio
//     </PARAMS>                           travels INSIDE the preset, base64 FLAC)
//   </SPAStripPreset>
//
//   type="" / absent / not one of presetTypes() -> the preset is untyped: the
//   browser lists it under "Other". The attribute is the user's explicit choice
//   (made in the save panel or via "Set type"); it is never guessed from the name.
//
// A preset does NOT carry, and loading one does NOT change:
//   * the FX lock mask and WILD (workflow settings),
//   * global.oversampling (a quality / CPU setting of this session -- the
//     session's current value is kept),
//   * UI properties of the session.
// Loading a preset is ONE undo step and resets the FX chain state (under the
// processor's callback lock), as SPASynth's preset load does.
//
// LOCATION  (SPASynth's convention, with its types kept OUT of the layout: a
// type is an attribute, a folder is the user's own filing -- the two are
// independent axes, exactly as in the synth)
//   <userApplicationDataDirectory>/Silverplatter Audio/SPAStrip/Presets/User[/<folder>[/<folder>]]/<name>.spastrip
//   macOS ~/Library/Application Support/..., Windows %APPDATA%\... (per-user, no
//   admin rights; where JUCE's userApplicationDataDirectory maps to on both).
//   Folders nest maxFolderDepth levels (what "New folder" can create); deeper
//   folders already on disk are still scanned and shown. A "bank" is a top-level
//   folder. The save panel's "Auto (by type)" files into User/<Type>/.
//   A read-only "Factory" group can later be fed from embedded data via
//   setFactoryPresets() (no content ships yet).
class PresetManager : public juce::ChangeBroadcaster
{
public:
    static constexpr const char* presetExtension = ".spastrip";
    static constexpr const char* presetTag = "SPAStripPreset";
    static constexpr int presetFormatVersion = 1;
    static constexpr juce::int64 maxPresetFileBytes = 256ll * 1024 * 1024;   // refuse absurd files
    static constexpr int maxFolderDepth = 2;   // levels below User/ that "New folder" may create

    // The effect-oriented types, in display order. One per preset.
    static const juce::StringArray& presetTypes();
    // What an untyped preset (no / unknown type) is listed as.
    static constexpr const char* otherTypeLabel = "Other";
    // The canonical spelling of a stored type (case-insensitive, trimmed), or ""
    // when it is empty or not one of presetTypes().
    static juce::String canonicalType (const juce::String& stored);

    struct PresetInfo
    {
        juce::String name;          // display name (the file's base name)
        juce::String bank;          // "" = User root; user presets: the TOP-LEVEL folder name
        juce::File file;            // invalid for an embedded factory preset
        bool isFactory = false;     // read-only: cannot be saved over, renamed or deleted
        juce::String embeddedXml;   // factory presets: the preset document
        juce::String folder;        // user presets: folder path below User/ with '/' ("" = root, "Drums", "Drums/Kicks")
        juce::String type;          // one of presetTypes(), or "" (untyped -> "Other")
        juce::String storedType;    // the raw attribute (an unknown value is kept on rename / save, never shown)
    };

    // Embedded read-only presets (the seam for a future "Factory" bank; empty today).
    struct FactoryPreset { juce::String name, bank, xml; };

    explicit PresetManager (SPAStripProcessor&);

    static juce::File defaultPresetsRoot();
    // Test seam: used by every PresetManager constructed afterwards (a hermetic
    // temp folder instead of the user's real one). Pass juce::File() to clear.
    static void setPresetsRootOverride (const juce::File& root);

    juce::File getPresetsRoot() const { return presetsRoot; }
    juce::File getUserFolder() const { return presetsRoot.getChildFile ("User"); }
    void setPresetsRoot (const juce::File& root);   // rescans
    // Delete / replace move to the system Trash (recoverable) by default; tests turn it off.
    void setUseTrash (bool shouldUseTrash) { useTrash = shouldUseTrash; }

    //==========================================================================
    // Browse.
    void rescan();
    const std::vector<PresetInfo>& getPresets() const { return presets; }
    juce::StringArray getBanks() const { return banks; }   // top-level user folders (incl. empty ones), sorted
    // Every user folder on disk as a relative path ("Drums", "Drums/Kicks"),
    // sorted, INCLUDING empty ones (so a fresh folder can be seen and dropped into).
    juce::StringArray getUserFolders() const { return userFolders; }
    int countPresetsInFolder (const juce::String& relFolder) const;   // recursive
    void setFactoryPresets (std::vector<FactoryPreset> factory);

    // Identity of a list entry across rescans: the file path, or "F:<bank>/<name>".
    static juce::String keyOf (const PresetInfo&);
    // The favourite key (what the settings file stores): "<bank or User>/<name>".
    static juce::String favouriteKey (const PresetInfo&);
    // The browser pushes its list order (every preset, as listed) so the prev / next
    // arrows step through the list as shown, ignoring filters and collapsed groups.
    // Empty = scan order. Presets missing from it are appended in scan order.
    void setNavigationOrder (std::vector<juce::String> keys) { navOrder = std::move (keys); }

    // Turns a user-typed name into a safe file base name: illegal characters
    // removed (JUCE's legal-file-name rules), leading / trailing dots and spaces
    // stripped (no hidden files, no Windows trailing-dot trap), Windows device
    // names (CON, NUL, COM1 ...) prefixed with '_', length capped in UTF-8 bytes.
    // Empty when nothing usable is left.
    static juce::String sanitiseFileName (const juce::String& name);

    //==========================================================================
    // Load.
    struct LoadResult { bool ok = false; juce::String error; };
    LoadResult load (const juce::File& file);
    LoadResult loadPreset (int index);
    LoadResult loadNext();
    LoadResult loadPrevious();
    // Restores registry defaults, the default chain order, no mod slots and no
    // IR. Keeps the session's lock mask, WILD and oversampling. One undo step.
    void init();

    //==========================================================================
    // Folders (user only -- every entry point refuses anything outside User/).
    struct OpResult
    {
        bool ok = false;
        juce::String error;   // human-readable reason when !ok
        juce::File file;      // the resulting file / folder on success
        int count = 0;        // presets inside (folder trash)
    };
    // parentRel "" = directly under User/. The name goes through sanitiseFileName;
    // empty / clashing / too deep -> refused.
    OpResult createUserFolder (const juce::String& parentRel, const juce::String& name);
    OpResult renameUserFolder (const juce::String& rel, const juce::String& newName);
    // Trash (recoverable) on the directory; favourites inside are removed. The
    // loaded SOUND is left as it is.
    OpResult trashUserFolder (const juce::String& rel);
    // Moves ONE user preset into destRel ("" = User root). A clash is refused, both
    // files untouched. The favourite follows and the loaded preset stays current.
    OpResult moveUserPreset (const juce::File& file, const juce::String& destRel);

    //==========================================================================
    // Save / delete / rename / type (user presets only).
    struct SaveResult
    {
        bool ok = false;
        bool clash = false;         // name taken and replace == false: nothing written
        bool overwritten = false;   // an existing preset was replaced
        juce::String error;
        juce::File file;
    };
    // folder: "" = User root, else a '/' path below User/ (each part goes through
    // sanitiseFileName; a missing folder is created when createFolder, within
    // maxFolderDepth). type: "" or one of presetTypes() (anything else is refused).
    // The preset's name (and file name) is sanitiseFileName(name). A replaced
    // preset goes to the Trash first (the new file is fully written beforehand, so
    // a failed write never damages the old one).
    SaveResult save (const juce::String& name, const juce::String& folder, bool replace = false,
                     const juce::String& type = {}, bool createFolder = true);
    // True when User/<folder>/<sanitised name>.spastrip exists.
    bool userPresetExists (const juce::String& name, const juce::String& folder) const;
    // The save panel's naming rule. startAtOne: "<base> 1", "<base> 2"... ; otherwise
    // "<base>" itself if free, else "<base> 2", "<base> 3"...
    juce::String suggestName (const juce::String& base, const juce::String& folder, bool startAtOne) const;
    // Rewrites a USER preset IN PLACE with the current state, keeping its name, folder
    // and stored type (the SAVE menu's "Save"). The previous version goes to the Trash.
    SaveResult saveInPlace (const juce::File& file);

    bool deletePreset (const juce::File& file, juce::String* error = nullptr);
    struct RenameResult { bool ok = false; juce::String error; juce::File newFile; };
    // Keeps the folder, the stored type and every other attribute of the file.
    RenameResult rename (const juce::File& file, const juce::String& newName);
    // Rewrites the "type" attribute (atomic). "" clears it. Refused for a factory /
    // unscanned file and for a type that is not in presetTypes().
    bool setPresetType (const juce::File& file, const juce::String& type, juce::String* error = nullptr);

    //==========================================================================
    // Export / import of .spastrip files (never of another product's format).
    bool exportPreset (const PresetInfo&, const juce::File& destFile, juce::String* error = nullptr) const;
    // Zips every preset under User/<rel> (recursively), paths relative to that folder.
    bool exportFolder (const juce::String& rel, const juce::File& destZip, juce::String* error = nullptr) const;

    enum class ImportClash { replace, keepBoth, skip };
    struct ImportResult
    {
        int imported = 0;
        juce::StringArray malformed;         // sources skipped as unreadable / not a SPAStrip preset
        juce::StringArray rejectedZipSlip;   // zip entries refused for escaping the target folder
    };

    // paths: any mix of .spastrip files (-> User/ root), folders (-> a folder named
    // after it) and .zip files (-> a folder named after the zip, zip-slip safe).
    // Fully async-friendly: advance() runs until finished or until the next name
    // clash (awaitingDecision); the caller resolves the clash out of band (an
    // in-editor dialog), calls decide(), then advance() again. finish() rescans once.
    class ImportSession
    {
    public:
        struct StepResult
        {
            bool finished = false;
            bool awaitingDecision = false;
            juce::String clashName;   // valid only when awaitingDecision
        };

        StepResult advance();
        // applyToRest: remember the action for every later clash of this session.
        void decide (ImportClash action, bool applyToRest);
        ImportResult finish();

    private:
        friend class PresetManager;
        struct Item
        {
            enum class Kind { file, zipEntry } kind = Kind::file;
            juce::File source;        // the preset file, or the zip
            juce::File destFolder;    // resolved destination folder
            int zipEntry = -1;
            juce::String label;       // for messages
        };
        ImportSession (PresetManager&, const juce::Array<juce::File>& paths);
        PresetManager& owner;
        std::vector<Item> items;
        size_t cursor = 0;
        ImportResult result;
        bool haveApplyToAll = false, havePending = false;
        ImportClash applyToAll = ImportClash::skip, pending = ImportClash::skip;
    };
    std::unique_ptr<ImportSession> beginImport (const juce::Array<juce::File>& paths);

    //==========================================================================
    // Current preset + edited flag.
    juce::String getCurrentName() const;   // any thread
    juce::File getCurrentFile() const { return currentFile; }
    int getCurrentIndex() const { return currentIndex; }
    // True when the patch (parameters except oversampling, chain order, mod slot
    // targets, IR source) differs from what the current preset had when it was
    // loaded or last saved -- or when there is nothing to compare with. Lock mask /
    // WILD / oversampling never count.
    bool isEdited() const;                 // exact; message thread
    bool isEditedCached() const { return editedFlag.load (std::memory_order_relaxed); }   // any thread, may lag
    // Re-evaluates the flag; broadcasts a change message when it flipped.
    // Returns the new value. (The processor calls it after discrete edits and
    // from its 150 ms timer after any parameter movement.)
    bool refreshEditedState();

    //==========================================================================
    // Processor hooks (undo / host-state restore).
    PresetContext captureContext() const;
    void restoreContext (const PresetContext&);
    void sessionRestored (const juce::String& name, bool edited);

private:
    LoadResult loadDocument (const juce::XmlElement& root, const juce::String& fallbackName, const juce::File& file,
                             bool isFactory, const juce::String& bank);
    LoadResult loadInfo (const PresetInfo&);
    // Writes name / version / type + the state to `file` atomically. A previous file is
    // trashed first when trashExisting (best effort; the swap still lands if that fails).
    bool writePresetFile (const juce::File& file, const juce::String& name, const juce::String& storedType,
                          const juce::ValueTree& state, bool trashExisting, juce::String& error) const;
    void setIdentity (const juce::String& name, const juce::File& file, const juce::String& bank,
                      bool isFactory, bool captureBaseline);
    void resolveCurrentIndex();
    void captureBaselineNow();
    const PresetInfo* findUser (const juce::File&) const;
    bool resolveFolder (const juce::String& rel, juce::File& out) const;   // an EXISTING folder below User/
    // Splits + sanitises a folder path; false (error set) when nothing usable remains.
    bool cleanFolderPath (const juce::String& raw, juce::StringArray& parts, juce::String& error) const;
    std::vector<int> navigationIndices() const;
    void moveFavourite (const juce::String& oldKey, const juce::String& newKey) const;

    SPAStripProcessor& processor;
    juce::File presetsRoot;
    bool useTrash = true;
    int oversamplingParamIndex = -1;

    std::vector<PresetInfo> presets;
    std::vector<FactoryPreset> factoryPresets;
    juce::StringArray banks, userFolders;
    std::vector<juce::String> navOrder;

    mutable juce::CriticalSection identityLock;   // guards currentName (read off-thread by getStateInformation)
    juce::String currentName { "Init" };
    juce::File currentFile;
    juce::String currentBank;
    bool currentIsFactory = false;
    int currentIndex = -1;
    std::shared_ptr<const PatchSnapshot> baseline;   // null = always edited
    std::atomic<bool> editedFlag { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PresetManager)
};

} // namespace spa::preset
