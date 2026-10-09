#pragma once

#include <spa_presets/PresetBrowser.h>

#include "../SPAStripProcessor.h"
#include "../presets/PresetManager.h"
#include "Controls.h"

namespace spa::ui
{

// SPAStrip's preset drawer: the shared spa-presets browser (see
// <spa_presets/PresetBrowser.h> for what it does -- live search with a clear "x",
// TYPE / folder / star filters, folder or type grouping, rename / trash / export /
// move / set type, multi-select batch export, import, drag and drop) with SPAStrip's
// theme, effect types (Drums, Vocals, Mixbus ...) and old static helper signatures.
// The dialogs and the licence gate live in the editor (hooks) and the processor.
class PresetBrowser : public spa::presets::PresetBrowser
{
public:
    using Base = spa::presets::PresetBrowser;

    PresetBrowser (SPAStripProcessor&, Hooks);

    //--- pure filtering / grouping (testable without a UI), with SPAStrip's types ----
    // The label a preset is listed under: its type, or "Other".
    static juce::String typeLabelOf (const Info& p) { return Base::typeLabelOf (p, preset::PresetManager::otherTypeLabel); }
    static std::vector<int> filterIndices (const std::vector<Info>& presets, const Filter& f, const juce::StringArray& favouriteKeys)
    {
        return Base::filterIndices (presets, f, favouriteKeys, preset::PresetManager::otherTypeLabel);
    }
    static juce::StringArray availableTypes (const std::vector<Info>& presets, const Filter& excludingType,
                                             const juce::StringArray& favouriteKeys)
    {
        return Base::availableTypes (presets, excludingType, favouriteKeys, preset::PresetManager::presetTypes(),
                                     preset::PresetManager::otherTypeLabel);
    }
    static std::vector<DisplayRow> buildRows (const std::vector<Info>& presets, const std::vector<int>& filteredIndices,
                                              const juce::StringArray& userFolders, GroupBy groupBy,
                                              const juce::StringArray& collapsedKeys, bool searchActive, bool anyFilterActive)
    {
        return Base::buildRows (presets, filteredIndices, userFolders, groupBy, collapsedKeys, searchActive, anyFilterActive,
                                preset::PresetManager::presetTypes(), preset::PresetManager::otherTypeLabel);
    }
};

} // namespace spa::ui
