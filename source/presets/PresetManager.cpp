#include "PresetManager.h"

#include <algorithm>

namespace spa::preset
{

namespace
{
    juce::File& rootOverride()
    {
        static juce::File override;
        return override;
    }

    bool isWindowsDeviceName (const juce::String& baseName)
    {
        // The reserved names apply to the part before the first dot, any case.
        const auto stem = baseName.upToFirstOccurrenceOf (".", false, false).trim().toUpperCase();
        if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" || stem == "CLOCK$")
            return true;
        if ((stem.startsWith ("COM") || stem.startsWith ("LPT")) && stem.length() == 4)
            return stem[3] >= '1' && stem[3] <= '9';
        return false;
    }

    // Reads a preset document. Null + error on anything that is not one.
    std::unique_ptr<juce::XmlElement> parseDocument (const juce::File& file, juce::String& error)
    {
        if (! file.existsAsFile())
        {
            error = "The preset file does not exist.";
            return nullptr;
        }
        if (file.getSize() > PresetManager::maxPresetFileBytes)
        {
            error = "The preset file is too large.";
            return nullptr;
        }
        auto xml = juce::XmlDocument::parse (file);
        if (xml == nullptr || ! xml->hasTagName (PresetManager::presetTag) || xml->getFirstChildElement() == nullptr)
        {
            error = "That is not a SPAStrip preset.";
            return nullptr;
        }
        return xml;
    }
}

//==============================================================================
juce::File PresetManager::defaultPresetsRoot()
{
    if (rootOverride() != juce::File())
        return rootOverride();

    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("Silverplatter Audio").getChildFile ("SPAStrip").getChildFile ("Presets");
}

void PresetManager::setPresetsRootOverride (const juce::File& root)
{
    rootOverride() = root;
}

PresetManager::PresetManager (SPAStripProcessor& p)
    : processor (p), presetsRoot (defaultPresetsRoot())
{
    for (size_t i = 0; i < params::all().size(); ++i)
        if (params::all()[i].id == params::id::oversampling)
            oversamplingParamIndex = (int) i;

    captureBaselineNow();   // the pristine state is the "Init" patch
    rescan();
}

void PresetManager::setPresetsRoot (const juce::File& root)
{
    presetsRoot = root;
    rescan();
}

//==============================================================================
juce::String PresetManager::sanitiseFileName (const juce::String& name)
{
    // JUCE's legal-name rules drop the separators and wildcards but not control
    // characters (illegal on Windows, unpleasant everywhere): drop those first.
    juce::String printable;
    for (auto c : name.trim())
        if (c >= 32 && c != 127)
            printable += juce::String::charToString (c);
    auto n = juce::File::createLegalFileName (printable).trim();
    n = n.trimCharactersAtStart (". ").trimCharactersAtEnd (". ");
    // JUCE caps at 128 characters, but a file name limit is bytes (255 on most
    // file systems), and 128 four-byte characters overshoot it.
    while (n.getNumBytesAsUTF8() > 200)
        n = n.dropLastCharacters (1);
    n = n.trimCharactersAtEnd (". ");
    if (n.isEmpty())
        return {};
    if (isWindowsDeviceName (n))
        n = "_" + n;
    return n;
}

//==============================================================================
void PresetManager::setFactoryPresets (std::vector<FactoryPreset> factory)
{
    factoryPresets = std::move (factory);
    rescan();
}

void PresetManager::rescan()
{
    presets.clear();
    banks.clear();

    for (const auto& f : factoryPresets)
        presets.push_back ({ f.name, f.bank, juce::File(), true, f.xml });

    const auto userRoot = getUserFolder();
    const auto addFrom = [this] (const juce::File& folder, const juce::String& bank)
    {
        for (const auto& f : folder.findChildFiles (juce::File::findFiles | juce::File::ignoreHiddenFiles,
                                                    false, "*" + juce::String (presetExtension)))
            presets.push_back ({ f.getFileNameWithoutExtension(), bank, f, false, {} });
    };
    addFrom (userRoot, {});
    // One level of banks: each immediate sub-folder of User/ is a bank; anything
    // nested deeper is not looked at.
    for (const auto& dir : userRoot.findChildFiles (juce::File::findDirectories | juce::File::ignoreHiddenFiles, false))
    {
        banks.add (dir.getFileName());
        addFrom (dir, dir.getFileName());
    }
    banks.sortNatural();

    // Factory first, then User root, then banks; names case-insensitive.
    std::stable_sort (presets.begin(), presets.end(), [] (const PresetInfo& a, const PresetInfo& b)
    {
        if (a.isFactory != b.isFactory)
            return a.isFactory;
        const auto c = a.bank.compareIgnoreCase (b.bank);
        return c != 0 ? c < 0 : a.name.compareIgnoreCase (b.name) < 0;
    });

    resolveCurrentIndex();
    sendChangeMessage();
}

void PresetManager::resolveCurrentIndex()
{
    currentIndex = -1;
    for (size_t i = 0; i < presets.size(); ++i)
    {
        const auto& p = presets[i];
        const bool same = currentIsFactory ? (p.isFactory && p.bank == currentBank && p.name == currentName)
                                           : (! p.isFactory && currentFile != juce::File() && p.file == currentFile);
        if (same)
            currentIndex = (int) i;
    }
}

//==============================================================================
// Identity / edited flag.
juce::String PresetManager::getCurrentName() const
{
    const juce::ScopedLock sl (identityLock);
    return currentName;
}

void PresetManager::setIdentity (const juce::String& name, const juce::File& file, const juce::String& bank,
                                 bool isFactory, bool captureBaseline)
{
    {
        const juce::ScopedLock sl (identityLock);
        currentName = name;
    }
    currentFile = file;
    currentBank = bank;
    currentIsFactory = isFactory;
    resolveCurrentIndex();
    if (captureBaseline)
        captureBaselineNow();
    else
        baseline = nullptr;
    editedFlag.store (isEdited(), std::memory_order_relaxed);
    sendChangeMessage();
}

void PresetManager::captureBaselineNow()
{
    baseline = std::make_shared<const PatchSnapshot> (processor.capturePatchSnapshot());
    editedFlag.store (false, std::memory_order_relaxed);
}

bool PresetManager::isEdited() const
{
    if (baseline == nullptr)
        return true;
    return processor.capturePatchSnapshot().firstDifference (*baseline, oversamplingParamIndex).isNotEmpty();
}

bool PresetManager::refreshEditedState()
{
    const bool now = isEdited();
    if (editedFlag.exchange (now, std::memory_order_relaxed) != now)
        sendChangeMessage();
    return now;
}

PresetContext PresetManager::captureContext() const
{
    PresetContext c;
    c.file = currentFile;
    c.name = getCurrentName();
    c.bank = currentBank;
    c.isFactory = currentIsFactory;
    c.baseline = baseline;
    return c;
}

void PresetManager::restoreContext (const PresetContext& c)
{
    {
        const juce::ScopedLock sl (identityLock);
        currentName = c.name;
    }
    currentFile = c.file;
    currentBank = c.bank;
    currentIsFactory = c.isFactory;
    baseline = c.baseline;
    resolveCurrentIndex();
    editedFlag.store (isEdited(), std::memory_order_relaxed);
    sendChangeMessage();
}

void PresetManager::sessionRestored (const juce::String& name, bool edited)
{
    setIdentity (name.isEmpty() ? juce::String ("Init") : name, juce::File(), {}, false, ! edited);
}

//==============================================================================
// Load.
PresetManager::LoadResult PresetManager::loadDocument (const juce::XmlElement& root, const PresetInfo*,
                                                       const juce::String& fallbackName, const juce::File& file,
                                                       bool isFactory, const juce::String& bank)
{
    LoadResult r;
    const auto* stateXml = root.getFirstChildElement();
    const auto state = stateXml != nullptr ? juce::ValueTree::fromXml (*stateXml) : juce::ValueTree();
    if (! state.isValid())
    {
        r.error = "That preset's data could not be read.";
        return r;
    }

    if (! processor.applyPresetState (state, "LOAD PRESET"))
    {
        r.error = "That is not a SPAStrip preset.";
        return r;
    }

    // Identity is set AFTER the step closed: the undo entry captured the PREVIOUS
    // preset's identity, which is what undoing this load must bring back.
    // An embedded preset is known by its listed name; a file by its name attribute.
    const auto name = isFactory ? fallbackName : root.getStringAttribute ("name", fallbackName).trim();
    setIdentity (name.isEmpty() ? fallbackName : name, file, bank, isFactory, true);
    r.ok = true;
    return r;
}

PresetManager::LoadResult PresetManager::load (const juce::File& file)
{
    LoadResult r;
    auto xml = parseDocument (file, r.error);
    if (xml == nullptr)
        return r;

    // A file inside the scanned list keeps its bank; a loose file has none.
    juce::String bank;
    for (const auto& p : presets)
        if (! p.isFactory && p.file == file)
            bank = p.bank;
    return loadDocument (*xml, nullptr, file.getFileNameWithoutExtension(), file, false, bank);
}

PresetManager::LoadResult PresetManager::loadInfo (const PresetInfo& info)
{
    if (! info.isFactory)
        return load (info.file);

    LoadResult r;
    auto xml = juce::XmlDocument::parse (info.embeddedXml);
    if (xml == nullptr || ! xml->hasTagName (presetTag) || xml->getFirstChildElement() == nullptr)
    {
        r.error = "That factory preset is damaged.";
        return r;
    }
    return loadDocument (*xml, &info, info.name, juce::File(), true, info.bank);
}

PresetManager::LoadResult PresetManager::loadPreset (int index)
{
    if (index < 0 || index >= (int) presets.size())
        return { false, "No such preset." };
    const auto info = presets[(size_t) index];   // copy: a load may rescan
    return loadInfo (info);
}

PresetManager::LoadResult PresetManager::loadNext()
{
    const int n = (int) presets.size();
    if (n == 0)
        return { false, "There are no presets." };
    LoadResult first;
    // Step over presets that fail to load, or navigation would stick on a bad file.
    for (int attempt = 0, i = currentIndex; attempt < n; ++attempt)
    {
        i = (i + 1) % n;
        auto r = loadPreset (i);
        if (r.ok)
            return r;
        if (attempt == 0)
            first = r;
    }
    return first;
}

PresetManager::LoadResult PresetManager::loadPrevious()
{
    const int n = (int) presets.size();
    if (n == 0)
        return { false, "There are no presets." };
    LoadResult first;
    for (int attempt = 0, i = currentIndex < 0 ? 0 : currentIndex; attempt < n; ++attempt)
    {
        i = (i - 1 + n) % n;
        auto r = loadPreset (i);
        if (r.ok)
            return r;
        if (attempt == 0)
            first = r;
    }
    return first;
}

void PresetManager::init()
{
    // An empty state: restoreStateTree fills every parameter missing from it with
    // its registry default, and the absent chain order / IR / slot properties fall
    // back to the default order, no IR and unassigned slots.
    juce::ValueTree empty (processor.getAPVTS().state.getType());
    processor.applyPresetState (empty, "INIT");
    setIdentity ("Init", juce::File(), {}, false, true);
}

//==============================================================================
// Save / delete / rename.
bool PresetManager::writePresetFile (const juce::File& file, const juce::String& name,
                                     const juce::ValueTree& state, juce::String& error) const
{
    juce::XmlElement root (presetTag);
    root.setAttribute ("name", name);
    root.setAttribute ("version", presetFormatVersion);
    root.addChildElement (state.createXml().release());

    if (! file.getParentDirectory().createDirectory().wasOk())
    {
        error = "The preset folder could not be created.";
        return false;
    }

    // Write beside the target and swap, so a failed write never damages an
    // existing preset. ".tmp" keeps the half-written file out of the scan.
    juce::TemporaryFile temp (file, file.getSiblingFile (file.getFileName() + ".tmp"));
    if (! root.writeTo (temp.getFile()) || ! temp.overwriteTargetFileWithTemporary())
    {
        error = "The preset could not be written.";
        return false;
    }
    return true;
}

PresetManager::SaveResult PresetManager::save (const juce::String& name, const juce::String& bank, bool replace)
{
    SaveResult r;
    const auto base = sanitiseFileName (name);
    if (base.isEmpty())
    {
        r.error = name.trim().isEmpty() ? "Enter a name." : "That name can't be used for a file.";
        return r;
    }

    juce::File dir = getUserFolder();
    if (bank.trim().isNotEmpty())
    {
        const auto bankName = sanitiseFileName (bank);
        if (bankName.isEmpty())
        {
            r.error = "That bank name can't be used for a folder.";
            return r;
        }
        dir = dir.getChildFile (bankName);
    }

    const auto file = dir.getChildFile (base + presetExtension);
    if (file.existsAsFile())
    {
        if (! replace)
        {
            r.clash = true;
            r.error = "A preset called \"" + base + "\" already exists here.";
            return r;
        }
        r.overwritten = true;
    }

    if (! writePresetFile (file, base, processor.capturePresetState(), r.error))
    {
        r.overwritten = false;
        return r;
    }

    r.ok = true;
    r.file = file;
    rescan();
    // The saved patch is now the loaded one, and "unedited".
    juce::String savedBank;
    for (const auto& p : presets)
        if (! p.isFactory && p.file == file)
            savedBank = p.bank;
    setIdentity (base, file, savedBank, false, true);
    return r;
}

bool PresetManager::deletePreset (const juce::File& file, juce::String* error)
{
    const auto fail = [error] (const juce::String& why) { if (error != nullptr) *error = why; return false; };

    bool known = false;
    for (const auto& p : presets)
        known = known || (! p.isFactory && p.file == file);
    if (! known)
        return fail ("That preset can't be deleted.");   // factory, or not scanned by this manager

    const bool wasCurrent = ! currentIsFactory && currentFile == file;
    const bool removed = useTrash ? file.moveToTrash() : file.deleteFile();
    if (! removed)
        return fail ("The preset could not be moved to the Trash.");

    // The loaded SOUND is left exactly as it is (nothing is re-applied); only the
    // identity loses its file, so the next save cannot silently target a deleted one.
    if (wasCurrent)
    {
        currentFile = juce::File();
        currentBank = {};
    }
    rescan();
    return true;
}

PresetManager::RenameResult PresetManager::rename (const juce::File& file, const juce::String& newNameRaw)
{
    RenameResult r;

    const PresetInfo* entry = nullptr;
    for (const auto& p : presets)
        if (! p.isFactory && p.file == file)
            entry = &p;
    if (entry == nullptr)
    {
        r.error = "That preset can't be renamed.";
        return r;
    }

    const auto base = sanitiseFileName (newNameRaw);
    if (base.isEmpty())
    {
        r.error = newNameRaw.trim().isEmpty() ? "Enter a name." : "That name can't be used for a file.";
        return r;
    }

    const auto newFile = file.getSiblingFile (base + presetExtension);
    // Case-insensitive file systems: a case-only rename targets the file itself.
    const bool sameFile = newFile == file || newFile.getFileName().equalsIgnoreCase (file.getFileName());
    if (! sameFile && newFile.existsAsFile())
    {
        r.error = "\"" + base + "\" already exists in this folder.";
        return r;
    }

    juce::String error;
    auto xml = parseDocument (file, error);
    if (xml == nullptr)
    {
        r.error = error;
        return r;
    }
    xml->setAttribute ("name", base);

    juce::TemporaryFile temp (newFile, newFile.getSiblingFile (newFile.getFileName() + ".tmp"));
    if (! xml->writeTo (temp.getFile()))
    {
        r.error = "The rename could not be written.";
        return r;
    }

    const bool wasCurrent = ! currentIsFactory && currentFile == file;
    // A case-only change on a case-insensitive volume names the SAME file: rename
    // it in place (a plain overwrite would keep the old casing).
    if (sameFile && file.getFileName() != newFile.getFileName() && ! file.moveFileTo (newFile))
    {
        r.error = "The rename could not be written.";
        return r;
    }
    if (! temp.overwriteTargetFileWithTemporary())
    {
        r.error = "The rename could not be written.";
        return r;
    }
    // The old name is superseded by newFile, whose content was just written: not
    // data loss, so no Trash round trip.
    if (! sameFile)
        file.deleteFile();

    if (wasCurrent)
    {
        currentFile = newFile;
        const juce::ScopedLock sl (identityLock);
        currentName = base;
    }
    rescan();
    r.ok = true;
    r.newFile = newFile;
    return r;
}

} // namespace spa::preset
