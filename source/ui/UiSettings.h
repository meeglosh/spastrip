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

} // namespace spa::ui::settings
