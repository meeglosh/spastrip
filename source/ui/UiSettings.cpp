#include "UiSettings.h"

namespace spa::ui::settings
{

namespace
{
    juce::CriticalSection& lock()
    {
        static juce::CriticalSection cs;
        return cs;
    }

    juce::File& overrideFile()
    {
        static juce::File f;
        return f;
    }

    std::unique_ptr<juce::PropertiesFile> open()
    {
        juce::PropertiesFile::Options o;
        o.millisecondsBeforeSaving = 0;
        o.storageFormat = juce::PropertiesFile::storeAsXML;
        o.doNotSave = false;
        return std::make_unique<juce::PropertiesFile> (getSettingsFile(), o);
    }
}

juce::File getSettingsFile()
{
    const juce::ScopedLock sl (lock());
    if (overrideFile() != juce::File())
        return overrideFile();
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("Silverplatter Audio").getChildFile ("SPAStrip").getChildFile ("SPAStrip.settings");
}

void setSettingsFileOverride (const juce::File& f)
{
    const juce::ScopedLock sl (lock());
    overrideFile() = f;
}

juce::Colour getAccentColor (juce::Colour fallback)
{
    const juce::ScopedLock sl (lock());
    if (! getSettingsFile().existsAsFile())
        return fallback;
    auto p = open();
    if (! p->containsKey ("accentColour"))
        return fallback;
    return juce::Colour::fromString (p->getValue ("accentColour")).withAlpha (1.0f);
}

void setAccentColor (juce::Colour c)
{
    const juce::ScopedLock sl (lock());
    getSettingsFile().getParentDirectory().createDirectory();
    auto p = open();
    p->setValue ("accentColour", c.withAlpha (1.0f).toString());
    p->saveIfNeeded();
}

void clearAccentColor()
{
    const juce::ScopedLock sl (lock());
    if (! getSettingsFile().existsAsFile())
        return;
    auto p = open();
    p->removeValue ("accentColour");
    p->saveIfNeeded();
}

juce::File getLastIRFolder()
{
    const juce::ScopedLock sl (lock());
    if (! getSettingsFile().existsAsFile())
        return {};
    auto p = open();
    return juce::File (p->getValue ("lastIRFolder"));
}

void setLastIRFolder (const juce::File& f)
{
    const juce::ScopedLock sl (lock());
    getSettingsFile().getParentDirectory().createDirectory();
    auto p = open();
    p->setValue ("lastIRFolder", (f.isDirectory() ? f : f.getParentDirectory()).getFullPathName());
    p->saveIfNeeded();
}

} // namespace spa::ui::settings
