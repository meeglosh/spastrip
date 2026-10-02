#pragma once

#include <atomic>
#include <functional>
#include <vector>

#include "../SPAStripProcessor.h"

namespace spa::preset
{

// Preset save / load / browse for SPAStrip (message thread only, except where
// noted). Modelled on SPASynth's library::PresetManager (XML wrapping the state
// tree, user bank folders, the edited flag as "state != what was loaded/saved"),
// without its sample-library / factory-recipe coupling.
//
// FILE FORMAT  (*.spastrip)
//   <SPAStripPreset name="Fire Pad" version="1">
//     <PARAMS fxOrder="..." irSource="none|embedded|factory:<id>" convIRName="..."
//             modSlot1Target="fxChorus.rate" ... stateVersion="1">
//       <PARAM id="fxDist.drive" value="0.3"/> ...
//       <IR format="flac24" .../>        (only for an embedded user IR: the audio
//     </PARAMS>                           travels INSIDE the preset, base64 FLAC)
//   </SPAStripPreset>
//
// A preset does NOT carry, and loading one does NOT change:
//   * the FX lock mask and WILD (workflow settings),
//   * global.oversampling (a quality / CPU setting of this session -- the
//     session's current value is kept),
//   * UI properties of the session.
// Loading a preset is ONE undo step and resets the FX chain state (under the
// processor's callback lock), as SPASynth's preset load does.
//
// LOCATION
//   <userApplicationDataDirectory>/Silverplatter Audio/SPAStrip/Presets/User[/<bank>]/<name>.spastrip
//   -- exactly SPASynth's convention (<same>/Silverplatter Audio/SPASynth/Presets):
//   macOS ~/Library/..., Windows %APPDATA%\... (per-user, no admin rights, the
//   place JUCE's own userApplicationDataDirectory maps to on both). One level of
//   bank sub-folders; deeper folders are ignored.
//   A read-only "Factory" bank can later be fed from embedded data via
//   setFactoryPresets() (no content ships yet).
class PresetManager : public juce::ChangeBroadcaster
{
public:
    static constexpr const char* presetExtension = ".spastrip";
    static constexpr const char* presetTag = "SPAStripPreset";
    static constexpr int presetFormatVersion = 1;
    static constexpr juce::int64 maxPresetFileBytes = 256ll * 1024 * 1024;   // refuse absurd files

    struct PresetInfo
    {
        juce::String name;          // display name (the file's base name)
        juce::String bank;          // "" = User root; user banks: the folder name
        juce::File file;            // invalid for an embedded factory preset
        bool isFactory = false;     // read-only: cannot be saved over, renamed or deleted
        juce::String embeddedXml;   // factory presets: the preset document
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
    // Delete moves to the system Trash (recoverable) by default; tests turn it off.
    void setUseTrash (bool shouldUseTrash) { useTrash = shouldUseTrash; }

    //==========================================================================
    // Browse.
    void rescan();
    const std::vector<PresetInfo>& getPresets() const { return presets; }
    juce::StringArray getBanks() const { return banks; }   // user bank folders (incl. empty ones), sorted
    void setFactoryPresets (std::vector<FactoryPreset> factory);

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
    // Save / delete / rename (user presets only).
    struct SaveResult
    {
        bool ok = false;
        bool clash = false;         // name taken and replace == false: nothing written
        bool overwritten = false;   // an existing preset was replaced
        juce::String error;
        juce::File file;
    };
    // bank "" = User root; otherwise a one-level bank folder (created on demand).
    // The preset's name (and file name) is sanitiseFileName(name).
    SaveResult save (const juce::String& name, const juce::String& bank, bool replace = false);
    bool deletePreset (const juce::File& file, juce::String* error = nullptr);
    struct RenameResult { bool ok = false; juce::String error; juce::File newFile; };
    RenameResult rename (const juce::File& file, const juce::String& newName);

    //==========================================================================
    // Current preset + edited flag.
    juce::String getCurrentName() const;   // any thread
    juce::File getCurrentFile() const { return currentFile; }
    int getCurrentIndex() const { return currentIndex; }
    // True when the patch (parameters except oversampling, chain order, mod slot
    // targets, IR source) differs from what the current preset had when it was
    // loaded or last saved -- or when there is nothing to compare with (an
    // imported SPASynth preset). Lock mask / WILD / oversampling never count.
    bool isEdited() const;                 // exact; message thread
    bool isEditedCached() const { return editedFlag.load (std::memory_order_relaxed); }   // any thread, may lag
    // Re-evaluates the flag; broadcasts a change message when it flipped.
    // Returns the new value. (The processor calls it after discrete edits and
    // from its 150 ms timer after any parameter movement.)
    bool refreshEditedState();

    //==========================================================================
    // SPASynth import.
    struct ImportResult
    {
        enum class Order { notInFile, applied, migratedLegacy, invalid };
        enum class IR { notInFile, resolved, unresolved };

        bool ok = false;
        juce::String error;
        juce::String presetName;
        int applied = 0;              // FX parameters taken from the file (incl. the migrated spreadPitch)
        int defaulted = 0;            // FX parameters the file lacks: set to their registry default
        juce::StringArray skippedIds; // PARAM ids NOT applied (synth-only parameters, non-FX, unreadable values)
        Order order = Order::notInFile;   // notInFile -> default chain order; invalid -> natural order (FXChain rule)
        IR ir = IR::notInFile;        // unresolved: the IR is left unchanged
        juce::String irPath;          // as written in the file
        bool grainSpreadMigrated = false;   // 1.0.29 -> 1.0.30: spreadPitch = 12 x spread
    };
    // Reads a .spasynth preset (root <SPASynthPreset>, PARAM id/value children,
    // fxOrder property, convIR path property) and applies ONLY the FX parameters
    // and the chain order -- global.*, sc.*, mod.* depths, mod slot targets,
    // locks, WILD, oversampling are untouched. One undo step; resets FX state.
    // convIR is resolved only when it is an absolute path to an existing audio
    // file ("$LIB$..." is relative to the synth's sample library, which SPAStrip
    // has no access to); otherwise the IR is left as it is.
    ImportResult importSPASynthPreset (const juce::File& file);

    //==========================================================================
    // Processor hooks (undo / host-state restore).
    PresetContext captureContext() const;
    void restoreContext (const PresetContext&);
    void sessionRestored (const juce::String& name, bool edited);

private:
    LoadResult loadDocument (const juce::XmlElement& root, const PresetInfo* info,
                             const juce::String& fallbackName, const juce::File& file, bool isFactory,
                             const juce::String& bank);
    LoadResult loadInfo (const PresetInfo&);
    bool writePresetFile (const juce::File& file, const juce::String& name, const juce::ValueTree& state,
                          juce::String& error) const;
    void setIdentity (const juce::String& name, const juce::File& file, const juce::String& bank,
                      bool isFactory, bool captureBaseline);
    void resolveCurrentIndex();
    void captureBaselineNow();

    SPAStripProcessor& processor;
    juce::File presetsRoot;
    bool useTrash = true;
    int oversamplingParamIndex = -1;

    std::vector<PresetInfo> presets;
    std::vector<FactoryPreset> factoryPresets;
    juce::StringArray banks;

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
