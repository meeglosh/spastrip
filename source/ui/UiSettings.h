#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace spa::ui::settings
{

// Machine-wide UI preferences (shared by every SPAStrip instance and host on
// this account): <userApplicationData>/Silverplatter Audio/SPAStrip/SPAStrip.settings
// (macOS ~/Library/Application Support, Windows %APPDATA%). Everything here is
// message-thread, low-frequency; each call opens the file fresh so another
// host process' change is seen.
juce::File getSettingsFile();

// Test / snapshot seam: redirect the file (a hermetic temp location). Pass
// juce::File() to restore the default.
void setSettingsFileOverride (const juce::File&);

// The user's accent colour, or `fallback` when none has been chosen.
juce::Colour getAccentColor (juce::Colour fallback);
void setAccentColor (juce::Colour);
void clearAccentColor();

// Last folder the "Load file..." impulse chooser used.
juce::File getLastIRFolder();
void setLastIRFolder (const juce::File& fileOrFolder);

// Preset browser preferences (machine-wide, like SPASynth's): the starred presets
// (keys are "<bank or User>/<name>", see PresetManager::favouriteKey), the groups
// the user collapsed (keys "U:<folder>" / "F:<bank>" / "T:<type>") and the list
// grouping (0 = by folder, 1 = by type).
juce::StringArray getFavoritePresets();
void setPresetFavorite (const juce::String& key, bool favorite);
juce::StringArray getCollapsedPresetGroups();
void setPresetGroupCollapsed (const juce::String& key, bool collapsed);
int getPresetGroupMode();
void setPresetGroupMode (int mode);

} // namespace spa::ui::settings
