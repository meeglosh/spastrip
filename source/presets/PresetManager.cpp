#include "PresetManager.h"

#include <algorithm>

#include "../ui/UiSettings.h"

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

    // The root element's start tag from the first bytes of a document, as a
    // self-closed element, parsed on its own. Presets can embed megabytes of IR
    // audio: the scan must not parse all of that just to read one attribute.
    std::unique_ptr<juce::XmlElement> parseRootTag (const juce::String& head)
    {
        const auto open = head.indexOf ("<" + juce::String (PresetManager::presetTag));
        if (open < 0)
            return nullptr;
        const auto close = head.indexOfChar (open, '>');
        if (close < 0)
            return nullptr;
        auto tag = head.substring (open, close + 1);
        if (! tag.endsWith ("/>"))
            tag = tag.dropLastCharacters (1) + "/>";
        auto xml = juce::XmlDocument::parse (tag);
        return xml != nullptr && xml->hasTagName (PresetManager::presetTag) ? std::move (xml) : nullptr;
    }

    juce::String readStoredType (const juce::File& file)
    {
        juce::FileInputStream in (file);
        if (! in.openedOk())
            return {};
        char buffer[4096];
        const auto n = in.read (buffer, (int) sizeof (buffer));
        if (n <= 0)
            return {};
        if (auto root = parseRootTag (juce::String::fromUTF8 (buffer, n)))
            return root->getStringAttribute ("type");
        // The start tag was longer than the window (or not well-formed there): read it properly.
        if (auto xml = juce::XmlDocument::parse (file); xml != nullptr && xml->hasTagName (PresetManager::presetTag))
            return xml->getStringAttribute ("type");
        return {};
    }

    juce::String readStoredType (const juce::String& xmlText)
    {
        if (auto root = parseRootTag (xmlText.substring (0, 4096)))
            return root->getStringAttribute ("type");
        return {};
    }

    // True if the entry, or any folder between it and `root`, is hidden. JUCE's File::isHidden()
    // only checks FILE_ATTRIBUTE_HIDDEN on Windows but also catches dot-names on macOS/Linux, so
    // dot-prefixed names (and in-flight ".tmp" files) are skipped explicitly for the same
    // behaviour on every OS.
    bool isHiddenOrTemp (const juce::File& root, const juce::File& f)
    {
        if (f.hasFileExtension ("tmp"))
            return true;
        for (auto cur = f; cur != root && cur != cur.getParentDirectory(); cur = cur.getParentDirectory())
            if (cur.getFileName().startsWithChar ('.') || cur.isHidden())
                return true;
        return false;
    }

    juce::Array<juce::File> findVisible (const juce::File& root, int what, bool recursive, const juce::String& pattern = "*")
    {
        juce::Array<juce::File> result;
        for (const auto& f : root.findChildFiles (what | juce::File::ignoreHiddenFiles, recursive, pattern))
            if (! isHiddenOrTemp (root, f))
                result.add (f);
        return result;
    }

    // Recursively lists the presets and folders below User/. The depth cap keeps a
    // symlink loop from running away.
    void scanFolder (const juce::File& dir, const juce::String& rel, int depth,
                     std::vector<PresetManager::PresetInfo>& out, juce::StringArray& folders)
    {
        const auto bank = rel.upToFirstOccurrenceOf ("/", false, false);
        for (const auto& f : findVisible (dir, juce::File::findFiles, false, "*" + juce::String (PresetManager::presetExtension)))
        {
            auto stored = readStoredType (f);
            auto type = PresetManager::canonicalType (stored);
            out.push_back ({ f.getFileNameWithoutExtension(), bank, f, false, {}, rel, std::move (type), std::move (stored) });
        }
        if (depth >= 8)
            return;
        for (const auto& d : findVisible (dir, juce::File::findDirectories, false))
        {
            const auto childRel = rel.isEmpty() ? d.getFileName() : rel + "/" + d.getFileName();
            folders.add (childRel);
            scanFolder (d, childRel, depth + 1, out, folders);
        }
    }
}

//==============================================================================
const juce::StringArray& PresetManager::presetTypes()
{
    static const juce::StringArray types { "Drums", "Bass", "Vocals", "Guitar", "Keys",
                                           "Synth", "FX", "Mixbus", "Mastering", "Creative" };
    return types;
}

juce::String PresetManager::canonicalType (const juce::String& stored)
{
    const auto t = stored.trim();
    if (t.isEmpty())
        return {};
    for (const auto& known : presetTypes())
        if (known.equalsIgnoreCase (t))
            return known;
    return {};
}

juce::String PresetManager::keyOf (const PresetInfo& p)
{
    return p.isFactory ? "F:" + p.bank + "/" + p.name : p.file.getFullPathName();
}

juce::String PresetManager::favouriteKey (const PresetInfo& p)
{
    if (p.isFactory)
        return "Factory:" + p.bank + "/" + p.name;
    return (p.bank.isEmpty() ? juce::String ("User") : p.bank) + "/" + p.name;
}

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
    userFolders.clear();

    for (const auto& f : factoryPresets)
    {
        auto stored = readStoredType (f.xml);
        auto type = canonicalType (stored);
        presets.push_back ({ f.name, f.bank, juce::File(), true, f.xml, {}, std::move (type), std::move (stored) });
    }

    scanFolder (getUserFolder(), {}, 0, presets, userFolders);
    userFolders.sort (true);
    for (const auto& f : userFolders)
        if (! f.contains ("/"))
            banks.add (f);
    banks.sortNatural();

    // Factory first, then the User root, then banks / folders; names case-insensitive.
    std::stable_sort (presets.begin(), presets.end(), [] (const PresetInfo& a, const PresetInfo& b)
    {
        if (a.isFactory != b.isFactory)
            return a.isFactory;
        if (auto c = a.bank.compareIgnoreCase (b.bank); c != 0)
            return c < 0;
        if (auto c = a.folder.compareIgnoreCase (b.folder); c != 0)
            return c < 0;
        return a.name.compareIgnoreCase (b.name) < 0;
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

int PresetManager::countPresetsInFolder (const juce::String& relFolder) const
{
    int n = 0;
    for (const auto& p : presets)
        if (! p.isFactory && (p.folder == relFolder || p.folder.startsWith (relFolder + "/")))
            ++n;
    return n;
}

const PresetManager::PresetInfo* PresetManager::findUser (const juce::File& file) const
{
    if (file == juce::File())
        return nullptr;
    for (const auto& p : presets)
        if (! p.isFactory && p.file == file)
            return &p;
    return nullptr;
}

void PresetManager::moveFavourite (const juce::String& oldKey, const juce::String& newKey) const
{
    if (oldKey == newKey || ! ui::settings::getFavoritePresets().contains (oldKey))
        return;
    ui::settings::setPresetFavorite (oldKey, false);
    ui::settings::setPresetFavorite (newKey, true);
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
PresetManager::LoadResult PresetManager::loadDocument (const juce::XmlElement& root, const juce::String& fallbackName,
                                                       const juce::File& file, bool isFactory, const juce::String& bank)
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
    if (const auto* known = findUser (file))
        bank = known->bank;
    return loadDocument (*xml, file.getFileNameWithoutExtension(), file, false, bank);
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
    return loadDocument (*xml, info.name, juce::File(), true, info.bank);
}

PresetManager::LoadResult PresetManager::loadPreset (int index)
{
    if (index < 0 || index >= (int) presets.size())
        return { false, "No such preset." };
    const auto info = presets[(size_t) index];   // copy: a load may rescan
    return loadInfo (info);
}

// The browser's list order (when it has pushed one), else the scan order.
std::vector<int> PresetManager::navigationIndices() const
{
    std::vector<int> order;
    std::vector<bool> used (presets.size(), false);
    for (const auto& key : navOrder)
        for (size_t i = 0; i < presets.size(); ++i)
            if (! used[i] && keyOf (presets[i]) == key)
            {
                used[i] = true;
                order.push_back ((int) i);
                break;
            }
    for (size_t i = 0; i < presets.size(); ++i)
        if (! used[i])
            order.push_back ((int) i);
    return order;
}

PresetManager::LoadResult PresetManager::loadNext()
{
    const auto order = navigationIndices();
    const int n = (int) order.size();
    if (n == 0)
        return { false, "There are no presets." };
    int pos = -1;
    for (int i = 0; i < n; ++i)
        if (order[(size_t) i] == currentIndex)
            pos = i;

    LoadResult first;
    // Step over presets that fail to load, or navigation would stick on a bad file.
    for (int attempt = 0, i = pos; attempt < n; ++attempt)
    {
        i = (i + 1) % n;
        auto r = loadPreset (order[(size_t) i]);
        if (r.ok)
            return r;
        if (attempt == 0)
            first = r;
    }
    return first;
}

PresetManager::LoadResult PresetManager::loadPrevious()
{
    const auto order = navigationIndices();
    const int n = (int) order.size();
    if (n == 0)
        return { false, "There are no presets." };
    int pos = -1;
    for (int i = 0; i < n; ++i)
        if (order[(size_t) i] == currentIndex)
            pos = i;

    LoadResult first;
    for (int attempt = 0, i = pos < 0 ? 0 : pos; attempt < n; ++attempt)
    {
        i = (i - 1 + n) % n;
        auto r = loadPreset (order[(size_t) i]);
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
// Folders.
bool PresetManager::resolveFolder (const juce::String& rel, juce::File& out) const
{
    const auto base = getUserFolder();
    out = base;
    if (rel.isEmpty())
        return true;
    for (const auto& part : juce::StringArray::fromTokens (rel, "/", ""))
    {
        if (part.isEmpty() || part == "." || part == ".." || juce::File::createLegalFileName (part) != part)
            return false;
        out = out.getChildFile (part);
    }
    return out.isDirectory() && out.isAChildOf (base);
}

bool PresetManager::cleanFolderPath (const juce::String& raw, juce::StringArray& parts, juce::String& error) const
{
    parts.clear();
    if (raw.trim().isEmpty())
        return true;
    // Segments that sanitise to nothing (".", "..", blanks) are dropped, never followed.
    for (const auto& segment : juce::StringArray::fromTokens (raw, "/\\", ""))
        if (const auto clean = sanitiseFileName (segment); clean.isNotEmpty())
            parts.add (clean);
    if (parts.isEmpty())
    {
        error = "That folder name can't be used.";
        return false;
    }
    return true;
}

PresetManager::OpResult PresetManager::createUserFolder (const juce::String& parentRel, const juce::String& name)
{
    OpResult r;
    juce::File parent;
    if (! resolveFolder (parentRel, parent))
    {
        r.error = "That folder can't be used.";
        return r;
    }
    const int depth = parentRel.isEmpty() ? 1 : juce::StringArray::fromTokens (parentRel, "/", "").size() + 1;
    if (depth > maxFolderDepth)
    {
        r.error = "Folders can only be nested " + juce::String (maxFolderDepth) + " levels deep.";
        return r;
    }
    if (name.trim().isEmpty())
    {
        r.error = "Enter a name.";
        return r;
    }
    const auto legal = sanitiseFileName (name);
    if (legal.isEmpty())
    {
        r.error = "That name can't be used for a folder.";
        return r;
    }
    const auto target = parent.getChildFile (legal);
    if (target.exists())
    {
        r.error = "\"" + legal + "\" already exists here.";
        return r;
    }
    if (! target.createDirectory().wasOk())
    {
        r.error = "The folder could not be created.";
        return r;
    }
    rescan();
    r.ok = true;
    r.file = target;
    return r;
}

PresetManager::OpResult PresetManager::renameUserFolder (const juce::String& rel, const juce::String& newName)
{
    OpResult r;
    juce::File folder;
    if (rel.isEmpty() || ! resolveFolder (rel, folder))
    {
        r.error = "That folder can't be renamed.";
        return r;
    }
    if (newName.trim().isEmpty())
    {
        r.error = "Enter a name.";
        return r;
    }
    const auto legal = sanitiseFileName (newName);
    if (legal.isEmpty())
    {
        r.error = "That name can't be used for a folder.";
        return r;
    }
    const auto target = folder.getSiblingFile (legal);
    const bool sameFolder = target == folder || target.getFileName().equalsIgnoreCase (folder.getFileName());
    if (! sameFolder && target.exists())
    {
        r.error = "\"" + legal + "\" already exists here.";
        return r;
    }

    const auto loaded = currentIsFactory ? juce::File() : currentFile;
    const bool topLevel = ! rel.contains ("/");

    // Favourites inside follow when the top-level name (their key's first part) changes.
    std::vector<std::pair<juce::String, juce::String>> favMoves;
    if (topLevel)
        for (const auto& p : presets)
            if (! p.isFactory && p.bank == rel)
                favMoves.push_back ({ favouriteKey (p), legal + "/" + p.name });

    if (target.getFileName() != folder.getFileName() && ! folder.moveFileTo (target))
    {
        r.error = "The folder could not be renamed.";
        return r;
    }
    for (const auto& m : favMoves)
        moveFavourite (m.first, m.second);

    r.count = countPresetsInFolder (rel);
    if (loaded != juce::File() && loaded.isAChildOf (folder))
        currentFile = target.getChildFile (loaded.getRelativePathFrom (folder));
    rescan();
    r.ok = true;
    r.file = target;
    return r;
}

PresetManager::OpResult PresetManager::trashUserFolder (const juce::String& rel)
{
    OpResult r;
    juce::File folder;
    if (rel.isEmpty() || ! resolveFolder (rel, folder))
    {
        r.error = "That folder can't be removed.";
        return r;
    }

    const bool loadedInside = ! currentIsFactory && currentFile != juce::File() && currentFile.isAChildOf (folder);
    std::vector<juce::String> favKeys;
    for (const auto& p : presets)
        if (! p.isFactory && (p.folder == rel || p.folder.startsWith (rel + "/")))
            favKeys.push_back (favouriteKey (p));
    r.count = (int) favKeys.size();

    if (! (useTrash ? folder.moveToTrash() : folder.deleteRecursively()))
    {
        r.error = "The folder could not be moved to the Trash.";
        r.count = 0;
        return r;
    }
    for (const auto& k : favKeys)
        ui::settings::setPresetFavorite (k, false);

    // The sound stays as it is; the identity only loses the file that went.
    if (loadedInside)
    {
        currentFile = juce::File();
        currentBank = {};
    }
    rescan();
    r.ok = true;
    return r;
}

PresetManager::OpResult PresetManager::moveUserPreset (const juce::File& file, const juce::String& destRel)
{
    OpResult r;
    const auto* entry = findUser (file);
    if (entry == nullptr)
    {
        r.error = "That preset can't be moved.";
        return r;
    }
    juce::File dest;
    if (! resolveFolder (destRel, dest))
    {
        r.error = "That folder can't be used.";
        return r;
    }
    if (dest == file.getParentDirectory())
    {
        r.ok = true;   // already there
        r.file = file;
        return r;
    }
    const auto target = dest.getChildFile (file.getFileName());
    if (target.exists())
    {
        r.error = "\"" + entry->name + "\" already exists in that folder.";
        return r;
    }

    const auto oldKey = favouriteKey (*entry);
    const auto name = entry->name;
    const bool wasCurrent = ! currentIsFactory && currentFile == file;
    if (! file.moveFileTo (target))
    {
        r.error = "\"" + name + "\" could not be moved.";
        return r;
    }
    moveFavourite (oldKey, (destRel.isEmpty() ? juce::String ("User") : destRel.upToFirstOccurrenceOf ("/", false, false))
                               + "/" + name);
    if (wasCurrent)
        currentFile = target;
    rescan();
    if (wasCurrent)
        if (const auto* now = findUser (target))
            currentBank = now->bank;
    r.ok = true;
    r.file = target;
    return r;
}

//==============================================================================
// Save / delete / rename / type.
bool PresetManager::writePresetFile (const juce::File& file, const juce::String& name, const juce::String& storedType,
                                     const juce::ValueTree& state, bool trashExisting, juce::String& error) const
{
    juce::XmlElement root (presetTag);
    root.setAttribute ("name", name);
    root.setAttribute ("version", presetFormatVersion);
    if (storedType.isNotEmpty())
        root.setAttribute ("type", storedType);
    root.addChildElement (state.createXml().release());

    if (! file.getParentDirectory().createDirectory().wasOk())
    {
        error = "The preset folder could not be created.";
        return false;
    }

    // Write beside the target and swap, so a failed write never damages an
    // existing preset. ".tmp" keeps the half-written file out of the scan.
    juce::TemporaryFile temp (file, file.getSiblingFile (file.getFileName() + ".tmp"));
    if (! root.writeTo (temp.getFile()))
    {
        error = "The preset could not be written.";
        return false;
    }
    // The new version is complete on disk: now the old one may go to the Trash
    // (recoverable). Best effort -- the swap below replaces it either way.
    if (trashExisting && useTrash && file.existsAsFile())
        file.moveToTrash();
    if (! temp.overwriteTargetFileWithTemporary())
    {
        error = "The preset could not be written.";
        return false;
    }
    return true;
}

bool PresetManager::userPresetExists (const juce::String& name, const juce::String& folder) const
{
    const auto base = sanitiseFileName (name);
    juce::StringArray parts;
    juce::String ignored;
    if (base.isEmpty() || ! cleanFolderPath (folder, parts, ignored))
        return false;
    auto dir = getUserFolder();
    for (const auto& p : parts)
        dir = dir.getChildFile (p);
    return dir.getChildFile (base + presetExtension).existsAsFile();
}

juce::String PresetManager::suggestName (const juce::String& base, const juce::String& folder, bool startAtOne) const
{
    if (! startAtOne && ! userPresetExists (base, folder))
        return base;
    for (int n = startAtOne ? 1 : 2; n < 10000; ++n)
    {
        const auto candidate = base + " " + juce::String (n);
        if (! userPresetExists (candidate, folder))
            return candidate;
    }
    return base;
}

PresetManager::SaveResult PresetManager::save (const juce::String& name, const juce::String& folder, bool replace,
                                               const juce::String& type, bool createFolder)
{
    SaveResult r;
    if (! checkSaveAllowed()) { r.error = saveBlockedMessage; return r; }
    const auto base = sanitiseFileName (name);
    if (base.isEmpty())
    {
        r.error = name.trim().isEmpty() ? "Enter a name." : "That name can't be used for a file.";
        return r;
    }

    juce::String storedType;
    if (type.trim().isNotEmpty())
    {
        storedType = canonicalType (type);
        if (storedType.isEmpty())
        {
            r.error = "\"" + type.trim() + "\" is not a preset type.";
            return r;
        }
    }

    juce::StringArray parts;
    if (! cleanFolderPath (folder, parts, r.error))
        return r;
    auto dir = getUserFolder();
    for (const auto& p : parts)
        dir = dir.getChildFile (p);
    if (! parts.isEmpty() && ! dir.isDirectory())
    {
        if (! createFolder)
        {
            r.error = "That folder doesn't exist.";
            return r;
        }
        if (parts.size() > maxFolderDepth)
        {
            r.error = "Folders can only be nested " + juce::String (maxFolderDepth) + " levels deep.";
            return r;
        }
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

    if (! writePresetFile (file, base, storedType, processor.capturePresetState(), r.overwritten, r.error))
    {
        r.overwritten = false;
        return r;
    }

    r.ok = true;
    r.file = file;
    rescan();
    // The saved patch is now the loaded one, and "unedited".
    const auto* saved = findUser (file);
    setIdentity (base, file, saved != nullptr ? saved->bank : juce::String(), false, true);
    return r;
}

PresetManager::SaveResult PresetManager::saveInPlace (const juce::File& file)
{
    SaveResult r;
    if (! checkSaveAllowed()) { r.error = saveBlockedMessage; return r; }
    const auto* entry = findUser (file);
    if (entry == nullptr)
    {
        r.error = "That preset can't be saved over.";
        return r;
    }
    const auto storedType = entry->storedType;   // kept as it is, even if unknown
    auto old = parseDocument (file, r.error);
    if (old == nullptr)
        return r;
    const auto name = old->getStringAttribute ("name", file.getFileNameWithoutExtension()).trim();

    if (! writePresetFile (file, name.isEmpty() ? file.getFileNameWithoutExtension() : name, storedType,
                           processor.capturePresetState(), true, r.error))
        return r;

    r.ok = true;
    r.overwritten = true;
    r.file = file;
    rescan();
    const auto* saved = findUser (file);
    setIdentity (name.isEmpty() ? file.getFileNameWithoutExtension() : name, file,
                 saved != nullptr ? saved->bank : juce::String(), false, true);
    return r;
}

bool PresetManager::deletePreset (const juce::File& file, juce::String* error)
{
    const auto fail = [error] (const juce::String& why) { if (error != nullptr) *error = why; return false; };

    const auto* entry = findUser (file);
    if (entry == nullptr)
        return fail ("That preset can't be deleted.");   // factory, or not scanned by this manager
    const auto key = favouriteKey (*entry);

    const bool wasCurrent = ! currentIsFactory && currentFile == file;
    const bool removed = useTrash ? file.moveToTrash() : file.deleteFile();
    if (! removed)
        return fail ("The preset could not be moved to the Trash.");

    // A star left behind would re-attach itself to a later preset of the same name.
    ui::settings::setPresetFavorite (key, false);

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

    const auto* entry = findUser (file);
    if (entry == nullptr)
    {
        r.error = "That preset can't be renamed.";
        return r;
    }
    const auto oldKey = favouriteKey (*entry);
    const auto bank = entry->bank;

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
    xml->setAttribute ("name", base);   // every other attribute (the type, ...) stays

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

    moveFavourite (oldKey, (bank.isEmpty() ? juce::String ("User") : bank) + "/" + base);
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

bool PresetManager::setPresetType (const juce::File& file, const juce::String& type, juce::String* error)
{
    const auto fail = [error] (const juce::String& why) { if (error != nullptr) *error = why; return false; };

    if (findUser (file) == nullptr)
        return fail ("That preset's type can't be changed.");
    juce::String canonical;
    if (type.trim().isNotEmpty())
    {
        canonical = canonicalType (type);
        if (canonical.isEmpty())
            return fail ("\"" + type.trim() + "\" is not a preset type.");
    }

    juce::String parseError;
    auto xml = parseDocument (file, parseError);
    if (xml == nullptr)
        return fail (parseError);
    if (canonical.isEmpty())
        xml->removeAttribute ("type");
    else
        xml->setAttribute ("type", canonical);

    juce::TemporaryFile temp (file, file.getSiblingFile (file.getFileName() + ".tmp"));
    if (! xml->writeTo (temp.getFile()) || ! temp.overwriteTargetFileWithTemporary())
        return fail ("The preset could not be written.");

    rescan();
    return true;
}

//==============================================================================
// Export.
bool PresetManager::exportPreset (const PresetInfo& info, const juce::File& destFile, juce::String* error) const
{
    const auto fail = [error] (const juce::String& why) { if (error != nullptr) *error = why; return false; };
    if (! checkSaveAllowed()) return fail (saveBlockedMessage);

    if (destFile == juce::File() || ! destFile.getParentDirectory().createDirectory().wasOk())
        return fail ("The destination folder could not be created.");
    juce::TemporaryFile temp (destFile, destFile.getSiblingFile (destFile.getFileName() + ".tmp"));
    if (info.isFactory)
    {
        if (! temp.getFile().replaceWithText (info.embeddedXml))
            return fail ("The preset could not be written.");
    }
    else
    {
        juce::String parseError;
        if (parseDocument (info.file, parseError) == nullptr)
            return fail (parseError);
        if (! info.file.copyFileTo (temp.getFile()))   // verbatim: nothing is rewritten
            return fail ("The preset could not be written.");
    }
    return temp.overwriteTargetFileWithTemporary() ? true : fail ("The preset could not be written.");
}

bool PresetManager::exportFolder (const juce::String& rel, const juce::File& destZip, juce::String* error) const
{
    const auto fail = [error] (const juce::String& why) { if (error != nullptr) *error = why; return false; };
    if (! checkSaveAllowed()) return fail (saveBlockedMessage);

    juce::File folder;
    if (rel.isEmpty() || ! resolveFolder (rel, folder))
        return fail ("That folder can't be exported.");

    juce::ZipFile::Builder builder;
    bool any = false;
    for (const auto& f : findVisible (folder, juce::File::findFiles, true, "*" + juce::String (presetExtension)))
    {
        any = true;
        builder.addFile (f, 9, f.getRelativePathFrom (folder).replaceCharacter ('\\', '/'));
    }
    if (! any)
        return fail ("That folder holds no presets.");

    if (! destZip.getParentDirectory().createDirectory().wasOk())
        return fail ("The destination folder could not be created.");
    juce::TemporaryFile temp (destZip, destZip.getSiblingFile (destZip.getFileName() + ".tmp"));
    {
        juce::FileOutputStream out (temp.getFile());
        if (! out.openedOk() || ! out.setPosition (0) || ! out.truncate().wasOk() || ! builder.writeToStream (out, nullptr))
            return fail ("The zip could not be written.");
    }
    return temp.overwriteTargetFileWithTemporary() ? true : fail ("The zip could not be written.");
}

//==============================================================================
// Import.
namespace
{
    // A preset document is a SPAStripPreset root with a state child. Anything else
    // (another product's file, a stray XML) is not importable.
    bool isPresetText (const juce::String& text, juce::String& nameOut, const juce::String& fallbackName)
    {
        auto xml = juce::XmlDocument::parse (text);
        if (xml == nullptr || ! xml->hasTagName (PresetManager::presetTag) || xml->getFirstChildElement() == nullptr)
            return false;
        nameOut = xml->getStringAttribute ("name", fallbackName).trim();
        if (nameOut.isEmpty())
            nameOut = fallbackName;
        return true;
    }

    // `parts` below a bank folder, clamped so an import never creates more levels
    // than "New folder" may (the deeper ones are flattened into the last level).
    juce::File clampedFolder (const juce::File& bankFolder, const juce::StringArray& relParts)
    {
        auto dir = bankFolder;
        for (int i = 0; i < relParts.size() && i < PresetManager::maxFolderDepth - 1; ++i)
            dir = dir.getChildFile (relParts[i]);
        return dir;
    }
}

PresetManager::ImportSession::ImportSession (PresetManager& o, const juce::Array<juce::File>& paths) : owner (o)
{
    const auto userRoot = owner.getUserFolder();
    const juce::String ext (presetExtension);

    for (const auto& path : paths)
    {
        if (path.existsAsFile() && path.hasFileExtension (ext))
        {
            items.push_back ({ Item::Kind::file, path, userRoot, -1, path.getFileName() });
        }
        else if (path.isDirectory())
        {
            auto bankName = sanitiseFileName (path.getFileName());
            if (bankName.isEmpty())
                bankName = "Imported";
            const auto bankFolder = userRoot.getChildFile (bankName);
            for (const auto& f : findVisible (path, juce::File::findFiles, true, "*" + ext))
            {
                juce::StringArray relParts;
                for (const auto& seg : juce::StringArray::fromTokens (f.getParentDirectory().getRelativePathFrom (path), "/\\", ""))
                    if (const auto clean = sanitiseFileName (seg); clean.isNotEmpty())
                        relParts.add (clean);
                items.push_back ({ Item::Kind::file, f, clampedFolder (bankFolder, relParts), -1, f.getFileName() });
            }
        }
        else if (path.existsAsFile() && path.hasFileExtension ("zip"))
        {
            auto bankName = sanitiseFileName (path.getFileNameWithoutExtension());
            if (bankName.isEmpty())
                bankName = "Imported";
            const auto bankFolder = userRoot.getChildFile (bankName);

            juce::ZipFile zip (path);
            for (int i = 0; i < zip.getNumEntries(); ++i)
            {
                const auto* entry = zip.getEntry (i);
                if (entry == nullptr || entry->filename.endsWithChar ('/') || ! entry->filename.endsWithIgnoreCase (ext))
                    continue;   // directories and non-preset content are ignored silently

                // Zip-slip guard: an entry may not climb out of its folder or be absolute.
                const auto segments = juce::StringArray::fromTokens (entry->filename.replaceCharacter ('\\', '/'), "/", "");
                bool escapes = entry->filename.startsWithChar ('/') || entry->filename.startsWithChar ('\\')
                               || entry->filename.containsChar (':');
                for (const auto& seg : segments)
                    escapes = escapes || seg == "..";
                if (escapes)
                {
                    result.rejectedZipSlip.add (entry->filename);
                    continue;
                }
                if (entry->uncompressedSize > (juce::int64) maxPresetFileBytes)
                {
                    result.malformed.add (entry->filename);
                    continue;
                }
                juce::StringArray relParts;
                for (int s = 0; s < segments.size() - 1; ++s)
                    if (const auto clean = sanitiseFileName (segments[s]); clean.isNotEmpty())
                        relParts.add (clean);
                items.push_back ({ Item::Kind::zipEntry, path, clampedFolder (bankFolder, relParts), i, entry->filename });
            }
        }
        else
        {
            result.malformed.add (path.getFileName());   // includes another product's preset files: never accepted
        }
    }
}

PresetManager::ImportSession::StepResult PresetManager::ImportSession::advance()
{
    const juce::String ext (presetExtension);
    while (cursor < items.size())
    {
        const auto& item = items[cursor];

        // Read + validate the source.
        juce::MemoryBlock bytes;
        juce::String fallbackName = item.source.getFileNameWithoutExtension();
        bool readOk = false;
        if (item.kind == Item::Kind::file)
        {
            readOk = item.source.getSize() <= maxPresetFileBytes && item.source.loadFileAsData (bytes);
        }
        else
        {
            juce::ZipFile zip (item.source);
            if (const auto* entry = zip.getEntry (item.zipEntry))
            {
                fallbackName = juce::File::createFileWithoutCheckingPath (entry->filename.replaceCharacter ('\\', '/'))
                                   .getFileNameWithoutExtension();
                if (std::unique_ptr<juce::InputStream> in { zip.createStreamForEntry (item.zipEntry) })
                {
                    in->readIntoMemoryBlock (bytes);   // the entry's size was capped when the session was planned
                    readOk = (juce::int64) bytes.getSize() <= maxPresetFileBytes;
                }
            }
        }
        juce::String name;
        if (! readOk || ! isPresetText (bytes.toString(), name, fallbackName))
        {
            result.malformed.add (item.label);
            ++cursor;
            continue;
        }

        const auto base = sanitiseFileName (name);
        if (base.isEmpty())
        {
            result.malformed.add (item.label);
            ++cursor;
            continue;
        }
        auto target = item.destFolder.getChildFile (base + ext);
        if (target.existsAsFile())
        {
            ImportClash action;
            if (haveApplyToAll)
                action = applyToAll;
            else if (havePending)
            {
                action = pending;
                havePending = false;
            }
            else
                return { false, true, base };   // pause WITHOUT consuming the item; decide() then advance() again

            if (action == ImportClash::skip)
            {
                ++cursor;
                continue;
            }
            if (action == ImportClash::keepBoth)
            {
                int n = 2;
                do
                    target = item.destFolder.getChildFile (base + " " + juce::String (n++) + ext);
                while (target.existsAsFile());
            }
        }

        bool ok = item.destFolder.createDirectory().wasOk();
        if (ok)
        {
            juce::TemporaryFile temp (target, target.getSiblingFile (target.getFileName() + ".tmp"));
            ok = temp.getFile().replaceWithData (bytes.getData(), bytes.getSize()) && temp.overwriteTargetFileWithTemporary();
        }
        if (ok)
            ++result.imported;
        else
            result.malformed.add (item.label);
        ++cursor;
    }
    return { true, false, {} };
}

void PresetManager::ImportSession::decide (ImportClash action, bool applyToRest)
{
    if (applyToRest)
    {
        haveApplyToAll = true;
        applyToAll = action;
    }
    else
    {
        havePending = true;
        pending = action;
    }
}

PresetManager::ImportResult PresetManager::ImportSession::finish()
{
    if (result.imported > 0)
        owner.rescan();
    return result;
}

std::unique_ptr<PresetManager::ImportSession> PresetManager::beginImport (const juce::Array<juce::File>& paths)
{
    return std::unique_ptr<ImportSession> (new ImportSession (*this, paths));
}

} // namespace spa::preset
