#pragma once

#include <spa_presets/PresetManager.h>

#include "../SPAStripProcessor.h"

namespace spa::preset
{

// SPAStrip's preset manager: the shared spa-presets manager (save / load / browse,
// user folders and banks, stored type, edited flag, favourites, export / import --
// see <spa_presets/PresetManager.h> for the behaviour and the file layout) with
// SPAStrip's adapter plugged in. What lives HERE is only what is SPAStrip's own:
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
// A preset does NOT carry, and loading one does NOT change:
//   * the FX lock mask and WILD (workflow settings),
//   * global.oversampling (a quality / CPU setting of this session -- the
//     session's current value is kept),
//   * UI properties of the session.
// Loading a preset is ONE undo step and resets the FX chain state (under the
// processor's callback lock), as SPASynth's preset load does.
//
// LOCATION   <userApplicationDataDirectory>/Silverplatter Audio/SPAStrip/Presets/User[/<folder>[/<folder>]]/<name>.spastrip
//   macOS ~/Library/Application Support/..., Windows %APPDATA%\... A type is an
//   attribute, a folder is the user's own filing -- the two are independent axes.
// The effect-oriented TYPES: Drums, Bass, Vocals, Guitar, Keys, Synth, FX, Mixbus,
// Mastering, Creative, Ambient, Lo-Fi (one per preset; "Other" lists the untyped).
class PresetManager : public spa::presets::PresetManager
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

    explicit PresetManager (SPAStripProcessor&);

    // The 72 factory presets (FactoryBank.h), listed read-only under "Factory". The processor
    // installs them once it has built this manager. Test seam: with the bank disabled
    // (tests main does this, so the browser-model tests keep their exact counts) the call
    // installs nothing; the factory-bank tests turn it back on.
    void installFactoryBank();
    static void setFactoryBankEnabled (bool enabled);

    static juce::File defaultPresetsRoot();
    // Test seam: used by every PresetManager constructed afterwards (a hermetic
    // temp folder instead of the user's real one). Pass juce::File() to clear.
    static void setPresetsRootOverride (const juce::File& root);

    //==========================================================================
    // Processor hooks (undo / host-state restore): the undo history carries a
    // PresetContext, the shared manager an Identity.
    PresetContext captureContext() const;
    void restoreContext (const PresetContext&);

private:
    static spa::presets::Adapter makeAdapter (SPAStripProcessor&);
};

} // namespace spa::preset
