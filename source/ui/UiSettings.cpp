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

namespace
{
    // A newline-separated list stored under one key (preset names cannot contain a newline).
    juce::StringArray readList (const char* key)
    {
        const juce::ScopedLock sl (lock());
        if (! getSettingsFile().existsAsFile())
            return {};
        auto p = open();
        auto list = juce::StringArray::fromLines (p->getValue (key));
        list.removeEmptyStrings();
        return list;
    }

    void writeListEntry (const char* key, const juce::String& entry, bool present)
    {
        const juce::ScopedLock sl (lock());
        if (! present && ! getSettingsFile().existsAsFile())
            return;
        getSettingsFile().getParentDirectory().createDirectory();
        auto p = open();
        auto list = juce::StringArray::fromLines (p->getValue (key));
        list.removeEmptyStrings();
        if (present)
            list.addIfNotAlreadyThere (entry);
        else
            list.removeString (entry);
        p->setValue (key, list.joinIntoString ("\n"));
        p->saveIfNeeded();
    }
}

juce::StringArray getFavoritePresets() { return readList ("favoritePresets"); }

void setPresetFavorite (const juce::String& key, bool favorite)
{
    if (key.isNotEmpty())
        writeListEntry ("favoritePresets", key, favorite);
}

juce::StringArray getCollapsedPresetGroups() { return readList ("collapsedPresetGroups"); }

void setPresetGroupCollapsed (const juce::String& key, bool collapsed)
{
    if (key.isNotEmpty())
        writeListEntry ("collapsedPresetGroups", key, collapsed);
}

int getPresetGroupMode()
{
    const juce::ScopedLock sl (lock());
    if (! getSettingsFile().existsAsFile())
        return 0;
    return open()->getIntValue ("presetGroupMode", 0) == 1 ? 1 : 0;
}

void setPresetGroupMode (int mode)
{
    const juce::ScopedLock sl (lock());
    getSettingsFile().getParentDirectory().createDirectory();
    auto p = open();
    p->setValue ("presetGroupMode", mode == 1 ? 1 : 0);
    p->saveIfNeeded();
}

} // namespace spa::ui::settings
