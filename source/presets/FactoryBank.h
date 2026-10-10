#pragma once

#include <juce_data_structures/juce_data_structures.h>

#include <spa_presets/PresetManager.h>

#include <vector>

namespace spa::preset
{

// SPAStrip's factory bank: 12 types x 6 presets, written as short text recipes (the way
// SPASynth's SynthBank is) and delivered through the shared preset module's embedded
// factory list (PresetManager::setFactoryPresets), so they sit under "Factory" in the
// browser, are read-only, and need nothing on disk.
//
// Bump kFactoryRecipeVersion whenever the table in FactoryBankTable.cpp changes. Embedded
// presets are rebuilt from the table on every launch, so an install always shows the
// current bank; the stamp is written on each preset (root attribute "factoryRecipe") so
// a test, or a support question, can tell which table a document came from.
inline constexpr int kFactoryRecipeVersion = 1;

struct FactoryEntry
{
    const char* name;     // "<Type word> <Descriptor>"
    const char* type;     // one of PresetManager::presetTypes()
    const char* recipe;   // grammar: see FactoryBank.cpp
};

const std::vector<FactoryEntry>& factoryEntries();

// The patch state of one recipe (a "PARAMS" tree, the shape capturePresetState writes).
// Unknown parameters / values are reported into *errors (the tests require none).
juce::ValueTree buildFactoryState (const FactoryEntry&, juce::String* errors = nullptr);

// Every entry as an embedded <SPAStripPreset> document for setFactoryPresets.
std::vector<spa::presets::PresetManager::FactoryPreset> buildFactoryBank();

} // namespace spa::preset
