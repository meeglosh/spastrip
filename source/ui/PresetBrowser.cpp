#include "PresetBrowser.h"

#include "UiSettings.h"

namespace spa::ui
{

using PM = preset::PresetManager;

namespace
{
    juce::Font browserFont()      { return juce::Font (juce::FontOptions (13.0f)); }
    juce::Font browserSmallFont() { return juce::Font (juce::FontOptions (10.5f)); }
    constexpr int groupRadioId = 0x5752;
    constexpr int rowHeight = 36;
    constexpr int indentStep = 14;

    // A five-point star centred in `area`, drawn as a path (identical on every platform).
    juce::Path starPath (juce::Rectangle<float> area, float radius)
    {
        juce::Path p;
        p.addStar (area.getCentre().translated (0.0f, 0.5f), 5, radius * 0.45f, radius);
        return p;
    }

    juce::String folderLabelOf (const PresetBrowser::Info& p)
    {
        if (p.isFactory)
            return "Factory";
        return p.folder.isEmpty() ? juce::String ("User") : p.folder.replace ("/", "  /  ");
    }
}

//==============================================================================
// The favourites filter: a star that lights up.
class PresetBrowser::StarToggle : public juce::Button
{
public:
    StarToggle() : juce::Button ("favourites")
    {
        setClickingTogglesState (true);
        setTooltip ("Show favourites only");
    }

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        const auto& t = currentTheme();
        auto b = getLocalBounds().toFloat().reduced (0.5f);
        const bool on = getToggleState();
        g.setColour (on ? t.accent.withAlpha (0.16f) : t.display);
        g.fillRoundedRectangle (b, 3.0f);
        g.setColour (on ? t.accent.withAlpha (0.8f) : (over ? t.textSecondary : t.outline));
        g.drawRoundedRectangle (b, 3.0f, 1.0f);
        const auto star = starPath (b, 7.0f);
        if (on)
        {
            g.setColour (t.accent.withAlpha (down ? 0.7f : 1.0f));
            g.fillPath (star);
        }
        else
        {
            g.setColour (t.textSecondary.withAlpha (over ? 1.0f : 0.8f));
            g.strokePath (star, juce::PathStrokeType (1.1f));
        }
    }
};

//==============================================================================
// Pure helpers.
juce::String PresetBrowser::typeLabelOf (const Info& p)
{
    return p.type.isEmpty() ? juce::String (PM::otherTypeLabel) : p.type;
}

juce::String PresetBrowser::categoryOf (const Info& p)
{
    if (p.isFactory)
        return p.bank.isEmpty() ? juce::String ("Factory") : p.bank;
    return p.bank.isEmpty() ? juce::String ("User") : p.bank;
}

std::vector<int> PresetBrowser::filterIndices (const std::vector<Info>& presets, const Filter& f,
                                               const juce::StringArray& favouriteKeys)
{
    std::vector<int> out;
    for (size_t i = 0; i < presets.size(); ++i)
    {
        const auto& p = presets[i];
        if (f.category.isNotEmpty() && categoryOf (p) != f.category)
            continue;
        if (f.type.isNotEmpty() && typeLabelOf (p) != f.type)
            continue;
        if (f.favoritesOnly && ! favouriteKeys.contains (PM::favouriteKey (p)))
            continue;
        if (f.search.isNotEmpty()
            && ! p.name.containsIgnoreCase (f.search)
            && ! categoryOf (p).containsIgnoreCase (f.search)
            && ! p.folder.containsIgnoreCase (f.search)     // nested folder names too
            && ! p.type.containsIgnoreCase (f.search))
            continue;
        out.push_back ((int) i);
    }
    return out;
}

juce::StringArray PresetBrowser::availableTypes (const std::vector<Info>& presets, const Filter& excludingType,
                                                 const juce::StringArray& favouriteKeys)
{
    auto base = excludingType;
    base.type = {};   // the listing never depends on its own pick

    bool present[16] = {};
    bool other = false;
    for (auto i : filterIndices (presets, base, favouriteKeys))
    {
        const auto& t = presets[(size_t) i].type;
        if (t.isEmpty())
            other = true;
        else if (const auto at = PM::presetTypes().indexOf (t); at >= 0 && at < 16)
            present[at] = true;
    }
    juce::StringArray out;
    for (int i = 0; i < PM::presetTypes().size() && i < 16; ++i)
        if (present[i])
            out.add (PM::presetTypes()[i]);
    if (other)
        out.add (PM::otherTypeLabel);
    return out;
}

std::vector<PresetBrowser::DisplayRow> PresetBrowser::buildRows (const std::vector<Info>& presets,
                                                                 const std::vector<int>& filtered,
                                                                 const juce::StringArray& userFolders, GroupBy groupBy,
                                                                 const juce::StringArray& collapsed,
                                                                 bool searchActive, bool anyFilterActive)
{
    using Kind = DisplayRow::Kind;
    std::vector<DisplayRow> out;

    const auto isOpen = [&] (const juce::String& key) { return searchActive || ! collapsed.contains (key); };

    const auto sortedByName = [&] (std::vector<int> v)
    {
        std::stable_sort (v.begin(), v.end(), [&] (int a, int b)
                          { return presets[(size_t) a].name.compareIgnoreCase (presets[(size_t) b].name) < 0; });
        return v;
    };
    const auto addPresets = [&] (const std::vector<int>& idx, int depth)
    {
        for (auto i : sortedByName (idx))
        {
            DisplayRow r;
            r.presetIndex = i;
            r.depth = depth;
            out.push_back (std::move (r));
        }
    };
    const auto addGroup = [&] (Kind kind, const juce::String& key, const juce::String& label,
                               const juce::String& rel, int depth, int count)
    {
        DisplayRow g;
        g.kind = kind;
        g.key = key;
        g.label = label;
        g.relFolder = rel;
        g.depth = depth;
        g.count = count;
        g.expanded = isOpen (key);
        out.push_back (g);
        return g.expanded;
    };

    if (groupBy == GroupBy::type)
    {
        // Preset types in their fixed order; presets with no (or an unknown) type last, as "Other".
        juce::StringArray order = PM::presetTypes();
        order.add (PM::otherTypeLabel);
        for (const auto& label : order)
        {
            std::vector<int> members;
            for (auto i : filtered)
                if (typeLabelOf (presets[(size_t) i]) == label)
                    members.push_back (i);
            if (members.empty())
                continue;
            if (addGroup (Kind::type, "T:" + label, label, {}, 0, (int) members.size()))
                addPresets (members, 1);
        }
        return out;
    }

    // ---- Folders: User (root + folders, nested) then Factory ----
    std::vector<int> userIdx, factoryIdx;
    for (auto i : filtered)
        (presets[(size_t) i].isFactory ? factoryIdx : userIdx).push_back (i);

    const auto countUnder = [&] (const juce::String& rel)
    {
        int n = 0;
        for (auto i : userIdx)
        {
            const auto& f = presets[(size_t) i].folder;
            if (rel.isEmpty() || f == rel || f.startsWith (rel + "/"))
                ++n;
        }
        return n;
    };

    // Every folder to consider: those on disk plus any a preset reports (and their
    // ancestors, so a nested folder always hangs off a visible parent).
    juce::StringArray folderSet;
    const auto addWithAncestors = [&] (juce::String rel)
    {
        while (rel.isNotEmpty())
        {
            folderSet.addIfNotAlreadyThere (rel);
            rel = rel.contains ("/") ? rel.upToLastOccurrenceOf ("/", false, false) : juce::String();
        }
    };
    for (const auto& f : userFolders)
        addWithAncestors (f);
    for (auto i : userIdx)
        addWithAncestors (presets[(size_t) i].folder);
    folderSet.sort (true);

    const auto parentOf = [] (const juce::String& rel)
    {
        return rel.contains ("/") ? rel.upToLastOccurrenceOf ("/", false, false) : juce::String();
    };

    std::function<void (const juce::String&, int)> emitChildren = [&] (const juce::String& rel, int depth)
    {
        std::vector<int> direct;
        for (auto i : userIdx)
            if (presets[(size_t) i].folder == rel)
                direct.push_back (i);
        addPresets (direct, depth + 1);

        for (const auto& child : folderSet)
        {
            if (parentOf (child) != rel)
                continue;
            const auto n = countUnder (child);
            if (anyFilterActive && n == 0)
                continue;
            if (addGroup (Kind::userFolder, "U:" + child, child.fromLastOccurrenceOf ("/", false, false),
                          child, depth + 1, n))
                emitChildren (child, depth + 1);
        }
    };

    const auto userCount = (int) userIdx.size();
    if (! (anyFilterActive && userCount == 0))
        if (addGroup (Kind::userRoot, "U:", "User", {}, 0, userCount))
            emitChildren ({}, 0);

    if (! factoryIdx.empty())
    {
        if (addGroup (Kind::factoryRoot, "F:", "Factory", {}, 0, (int) factoryIdx.size()))
        {
            std::vector<int> direct;
            juce::StringArray bankNames;
            for (auto i : factoryIdx)
            {
                const auto& b = presets[(size_t) i].bank;
                if (b.isEmpty() || b == "Factory")
                    direct.push_back (i);
                else
                    bankNames.addIfNotAlreadyThere (b);
            }
            addPresets (direct, 1);
            bankNames.sort (true);
            for (const auto& bank : bankNames)
            {
                std::vector<int> members;
                for (auto i : factoryIdx)
                    if (presets[(size_t) i].bank == bank)
                        members.push_back (i);
                if (addGroup (Kind::factoryBank, "F:" + bank, bank, {}, 1, (int) members.size()))
                    addPresets (members, 2);
            }
        }
    }
    return out;
}

//==============================================================================
PresetBrowser::PresetBrowser (SPAStripProcessor& p, Hooks h) : processor (p), hooks (std::move (h))
{
    setComponentID ("presetBrowser");
    setWantsKeyboardFocus (true);

    closeButton.setTooltip ("Close the preset browser (Esc)");
    closeButton.onClick = [this] { if (hooks.onClose) hooks.onClose(); };
    addAndMakeVisible (closeButton);

    groupBy = settings::getPresetGroupMode() == 1 ? GroupBy::type : GroupBy::folders;
    for (auto* b : { &groupFoldersButton, &groupTypeButton })
    {
        b->setClickingTogglesState (true);
        b->setRadioGroupId (groupRadioId);
        b->setMouseClickGrabsKeyboardFocus (false);
        addAndMakeVisible (*b);
    }
    groupFoldersButton.setTooltip ("Group the list by folder (User folders, then Factory)");
    groupTypeButton.setTooltip ("Group the list by type (Drums, Vocals, Mixbus ...)");
    (groupBy == GroupBy::type ? groupTypeButton : groupFoldersButton).setToggleState (true, juce::dontSendNotification);
    groupFoldersButton.onClick = [this] { setGroupBy (GroupBy::folders); };
    groupTypeButton.onClick = [this] { setGroupBy (GroupBy::type); };

    searchBox.setTextToShowWhenEmpty ("Search presets", currentTheme().textSecondary.withAlpha (0.7f));
    searchBox.setSelectAllWhenFocused (true);
    // Esc / Return are consumed here: the first Esc leaves the field (the next one closes
    // the drawer), Return hands the keyboard to the list.
    searchBox.setEscapeAndReturnKeysConsumed (true);
    searchBox.onTextChange = [this] { applyFilter(); };
    searchBox.onEscapeKey = [this] { grabKeyboardFocus(); };
    searchBox.onReturnKey = [this]
    {
        list.grabKeyboardFocus();
        if (list.getSelectedRow() < 0)
            for (int i = 0; i < (int) rows.size(); ++i)
                if (! rows[(size_t) i].isGroup())
                {
                    list.selectRow (i);
                    break;
                }
    };
    addAndMakeVisible (searchBox);

    typeBox.setTextWhenNothingSelected ("All Types");
    typeBox.setWantsKeyboardFocus (false);
    typeBox.setTooltip ("Show one type only");
    typeBox.onChange = [this] { applyFilter(); };
    addAndMakeVisible (typeBox);
    categoryBox.setTextWhenNothingSelected ("All Folders");
    categoryBox.setWantsKeyboardFocus (false);
    categoryBox.setTooltip ("Show one top-level folder only");
    categoryBox.onChange = [this] { applyFilter(); };
    addAndMakeVisible (categoryBox);

    favouritesChip = std::make_unique<StarToggle>();
    favouritesChip->setMouseClickGrabsKeyboardFocus (false);
    favouritesChip->onClick = [this] { applyFilter(); };
    addAndMakeVisible (*favouritesChip);

    list.setRowHeight (rowHeight);
    list.setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    // Shift / Cmd-click multi-select (to drag several presets at once). Rows select on
    // mouse UP so that starting a drag on a row does not first load that preset.
    list.setMultipleSelectionEnabled (true);
    list.setRowSelectedOnMouseDown (false);
    list.owner = this;
    addAndMakeVisible (list);

    initButton.setTooltip ("Reset every parameter, the chain order, the mod slots and the impulse to their defaults");
    initButton.onClick = [this] { processor.getPresetManager().init(); };
    newFolderButton.setTooltip ("Make a new folder for presets");
    newFolderButton.onClick = [this] { promptNewFolder ({}); };
    importButton.setTooltip ("Import .spastrip presets, a folder of them, or a .zip of presets "
                             "(dropping them on this drawer works too)");
    importButton.onClick = [this] { startImport(); };
    for (auto* b : { &initButton, &newFolderButton, &importButton })
    {
        b->setMouseClickGrabsKeyboardFocus (false);
        addAndMakeVisible (*b);
    }

    countLabel.setFont (metrics::smallFont());
    countLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (countLabel);

    processor.getPresetManager().addChangeListener (this);
    refresh();
}

PresetBrowser::~PresetBrowser()
{
    processor.getPresetManager().removeChangeListener (this);
}

void PresetBrowser::message (const juce::String& title, const juce::String& body)
{
    if (hooks.showMessage && body.isNotEmpty())
        hooks.showMessage (title, body);
}

//==============================================================================
void PresetBrowser::refresh()
{
    const auto& t = currentTheme();
    auto& pm = processor.getPresetManager();

    presets = pm.getPresets();
    userFolderList = pm.getUserFolders();
    favouriteKeys = settings::getFavoritePresets();

    list.setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    searchBox.setColour (juce::TextEditor::backgroundColourId, t.display);
    searchBox.setColour (juce::TextEditor::textColourId, t.textPrimary);
    searchBox.setColour (juce::TextEditor::outlineColourId, t.outline);
    searchBox.setColour (juce::TextEditor::focusedOutlineColourId, t.accent.withAlpha (0.7f));
    countLabel.setColour (juce::Label::textColourId, t.textSecondary);

    // Rebuild the folder dropdown from the categories actually present (User, banks, Factory),
    // keeping the current pick when it still exists.
    const auto selectedCategory = categoryBox.getSelectedId() > 1 ? categoryBox.getText() : juce::String();
    juce::StringArray categories;
    for (const auto& p : presets)
        if (! p.isFactory && p.bank.isEmpty())
        {
            categories.add ("User");
            break;
        }
    {
        juce::StringArray userBanks, factoryBanks;
        for (const auto& p : presets)
            if (! p.bank.isEmpty() || p.isFactory)
                (p.isFactory ? factoryBanks : userBanks).addIfNotAlreadyThere (categoryOf (p));
        userBanks.sortNatural();
        factoryBanks.sortNatural();
        categories.addArray (userBanks);
        categories.addArray (factoryBanks);
    }
    categoryBox.clear (juce::dontSendNotification);
    categoryBox.addItem ("All Folders", 1);
    int id = 2;
    for (const auto& c : categories)
    {
        categoryBox.addItem (c, id);
        if (c == selectedCategory)
            categoryBox.setSelectedId (id, juce::dontSendNotification);
        ++id;
    }
    if (categoryBox.getSelectedId() == 0)
        categoryBox.setSelectedId (1, juce::dontSendNotification);

    applyFilter();
}

PresetBrowser::Filter PresetBrowser::currentFilter() const
{
    Filter f;
    f.search = searchBox.getText().trim();
    if (categoryBox.getSelectedId() > 1)
        f.category = categoryBox.getText();
    if (typeBox.getSelectedId() > 1)
        f.type = typeBox.getText();
    f.favoritesOnly = favouritesChip != nullptr && favouritesChip->getToggleState();
    return f;
}

void PresetBrowser::applyFilter()
{
    auto filter = currentFilter();

    // The TYPE dropdown offers the types present under every OTHER filter, keeping the
    // current pick when it is still offered, else falling back to "All Types".
    const auto selectedType = filter.type;
    offeredTypes = availableTypes (presets, filter, favouriteKeys);
    typeBox.clear (juce::dontSendNotification);
    typeBox.addItem ("All Types", 1);
    int id = 2;
    for (const auto& t : offeredTypes)
    {
        typeBox.addItem (t, id);
        if (t == selectedType)
            typeBox.setSelectedId (id, juce::dontSendNotification);
        ++id;
    }
    if (typeBox.getSelectedId() == 0)
        typeBox.setSelectedId (1, juce::dontSendNotification);
    filter.type = typeBox.getSelectedId() > 1 ? typeBox.getText() : juce::String();

    filtered = filterIndices (presets, filter, favouriteKeys);

    const bool searchActive = filter.search.isNotEmpty();
    const bool anyFilter = searchActive || filter.category.isNotEmpty() || filter.type.isNotEmpty() || filter.favoritesOnly;
    rows = buildRows (presets, filtered, userFolderList, groupBy, settings::getCollapsedPresetGroups(),
                      searchActive, anyFilter);
    processor.getPresetManager().setNavigationOrder (currentNavigationOrder());

    countLabel.setText (juce::String (filtered.size()) + " of " + juce::String (presets.size())
                            + (presets.size() == 1 ? " preset" : " presets"),
                        juce::dontSendNotification);
    list.updateContent();

    // Highlight the loaded preset when it is on a visible row; reveal it once per change.
    auto& pm = processor.getPresetManager();
    juce::String currentKey;
    if (pm.getCurrentIndex() >= 0 && pm.getCurrentIndex() < (int) presets.size())
        currentKey = PM::keyOf (presets[(size_t) pm.getCurrentIndex()]);
    int currentRow = -1;
    for (size_t r = 0; r < rows.size(); ++r)
        if (! rows[r].isGroup() && currentKey.isNotEmpty() && PM::keyOf (presets[(size_t) rows[r].presetIndex]) == currentKey)
            currentRow = (int) r;

    if (currentRow >= 0)
    {
        list.selectRow (currentRow, true);
        if (currentKey != lastRevealed)
        {
            list.scrollToEnsureRowIsOnscreen (currentRow);
            lastRevealed = currentKey;
        }
    }
    else
    {
        list.deselectAllRows();
        lastRevealed = {};
    }
    list.repaint();
    repaint();
}

void PresetBrowser::scrollToCurrent()
{
    lastRevealed = {};
    refresh();   // re-pull from the manager, then reveal the loaded preset
}

//==============================================================================
// Accessors.
const PresetBrowser::Info* PresetBrowser::presetAtRow (int row) const
{
    if (row < 0 || row >= (int) rows.size() || rows[(size_t) row].isGroup())
        return nullptr;
    return &presets[(size_t) rows[(size_t) row].presetIndex];
}

int PresetBrowser::findGroupRow (const juce::String& key) const
{
    for (size_t i = 0; i < rows.size(); ++i)
        if (rows[i].isGroup() && rows[i].key == key)
            return (int) i;
    return -1;
}

int PresetBrowser::findVisibleRow (const juce::String& presetName) const
{
    for (size_t i = 0; i < rows.size(); ++i)
        if (! rows[i].isGroup() && presets[(size_t) rows[i].presetIndex].name == presetName)
            return (int) i;
    return -1;
}

void PresetBrowser::toggleGroupRow (int row)
{
    if (row < 0 || row >= (int) rows.size() || ! rows[(size_t) row].isGroup())
        return;
    // While a search is active every group is forced open, so a click would change
    // nothing visible -- leave the persisted state alone.
    if (searchBox.getText().trim().isNotEmpty())
        return;
    const auto& g = rows[(size_t) row];
    settings::setPresetGroupCollapsed (g.key, g.expanded);   // expanded -> collapse
    applyFilter();
}

void PresetBrowser::setGroupBy (GroupBy g)
{
    if (g == groupBy)
        return;
    groupBy = g;
    settings::setPresetGroupMode (g == GroupBy::type ? 1 : 0);
    (g == GroupBy::type ? groupTypeButton : groupFoldersButton).setToggleState (true, juce::dontSendNotification);
    applyFilter();
}

void PresetBrowser::setSearchText (const juce::String& s)
{
    searchBox.setText (s, false);
    applyFilter();
}

bool PresetBrowser::setTypeFilter (const juce::String& type)
{
    if (type.isEmpty())
    {
        typeBox.setSelectedId (1, juce::dontSendNotification);
        applyFilter();
        return true;
    }
    for (int i = 0; i < typeBox.getNumItems(); ++i)
        if (typeBox.getItemText (i) == type)
        {
            typeBox.setSelectedId (typeBox.getItemId (i), juce::dontSendNotification);
            applyFilter();
            return true;
        }
    return false;
}

bool PresetBrowser::setCategoryFilter (const juce::String& category)
{
    if (category.isEmpty())
    {
        categoryBox.setSelectedId (1, juce::dontSendNotification);
        applyFilter();
        return true;
    }
    for (int i = 0; i < categoryBox.getNumItems(); ++i)
        if (categoryBox.getItemText (i) == category)
        {
            categoryBox.setSelectedId (categoryBox.getItemId (i), juce::dontSendNotification);
            applyFilter();
            return true;
        }
    return false;
}

void PresetBrowser::setFavouritesOnly (bool on)
{
    favouritesChip->setToggleState (on, juce::dontSendNotification);
    applyFilter();
}

juce::StringArray PresetBrowser::getOfferedTypes() const { return offeredTypes; }

std::vector<juce::String> PresetBrowser::currentNavigationOrder() const
{
    std::vector<int> all;
    for (size_t i = 0; i < presets.size(); ++i)
        all.push_back ((int) i);
    // Everything expanded, nothing filtered: the full list in the order it is grouped.
    const auto full = buildRows (presets, all, userFolderList, groupBy, {}, true, false);
    std::vector<juce::String> out;
    for (const auto& r : full)
        if (! r.isGroup())
            out.push_back (PM::keyOf (presets[(size_t) r.presetIndex]));
    return out;
}

//==============================================================================
// Painting + clicks.
void PresetBrowser::paintListBoxItem (int rowIndex, juce::Graphics& g, int width, int height, bool selected)
{
    if (! juce::isPositiveAndBelow (rowIndex, (int) rows.size()))
        return;
    const auto& t = currentTheme();
    const auto& dr = rows[(size_t) rowIndex];
    const juce::Rectangle<int> r (0, 0, width, height);

    if (dr.isGroup())
    {
        g.setColour (t.outline.withAlpha (0.22f));
        g.fillRect (r);
        if (selected)
        {
            g.setColour (t.accent.withAlpha (0.10f));
            g.fillRect (r);
        }
        const int x0 = 8 + dr.depth * indentStep;
        const float cy = (float) height * 0.5f, cx = (float) x0 + 5.0f;
        juce::Path tri;
        if (dr.expanded)
            tri.addTriangle (cx - 5.0f, cy - 3.0f, cx + 5.0f, cy - 3.0f, cx, cy + 4.0f);
        else
            tri.addTriangle (cx - 3.0f, cy - 5.0f, cx - 3.0f, cy + 5.0f, cx + 4.0f, cy);
        g.setColour (t.textSecondary);
        g.fillPath (tri);

        auto text = r.withTrimmedLeft (x0 + 18).withTrimmedRight (10);
        const auto countText = "(" + juce::String (dr.count) + ")";
        g.setFont (browserSmallFont());
        const auto countW = juce::GlyphArrangement::getStringWidthInt (browserSmallFont(), countText) + 4;
        g.drawText (countText, text.removeFromRight (countW), juce::Justification::centredRight);
        g.setColour (t.textPrimary);
        g.setFont (browserFont().boldened());
        g.drawText (dr.label.toUpperCase(), text, juce::Justification::centredLeft, true);
        if (rowIndex == dropRow)
        {
            g.setColour (t.accent);
            g.drawRect (r.reduced (1), 2);
        }
        g.setColour (t.outline.withAlpha (0.5f));
        g.fillRect (0, height - 1, width, 1);
        return;
    }

    const auto& p = presets[(size_t) dr.presetIndex];
    if (selected)
    {
        g.setColour (t.accent.withAlpha (0.13f));
        g.fillRect (r);
        g.setColour (t.accent);
        g.fillRect (r.withWidth (2));
    }
    if (rowIndex == dropRow)
    {
        g.setColour (t.accent);
        g.drawRect (r.reduced (1), 2);
    }

    auto body = r;
    const auto starZone = body.removeFromRight (30);
    const bool favourite = favouriteKeys.contains (PM::favouriteKey (p));
    const auto star = starPath (starZone.toFloat(), 6.5f);
    if (favourite)
    {
        g.setColour (t.accent);
        g.fillPath (star);
    }
    else
    {
        g.setColour (t.textSecondary.withAlpha (0.5f));
        g.strokePath (star, juce::PathStrokeType (1.0f));
    }

    auto text = body.reduced (8, 4);
    text.removeFromLeft (dr.depth * indentStep + 8);
    const bool current = processor.getPresetManager().getCurrentIndex() >= 0
                         && processor.getPresetManager().getCurrentIndex() < (int) presets.size()
                         && PM::keyOf (presets[(size_t) processor.getPresetManager().getCurrentIndex()]) == PM::keyOf (p);
    g.setColour (t.textPrimary);
    g.setFont (current ? browserFont().boldened() : browserFont());
    g.drawText (p.name, text.removeFromTop (text.getHeight() / 2), juce::Justification::centredLeft, true);
    // The group already names the folder in the Folders view, so the second line shows the
    // OTHER axis (the type); in the Type view it shows where the preset lives.
    g.setColour (t.textSecondary);
    g.setFont (browserSmallFont());
    g.drawText (groupBy == GroupBy::type ? folderLabelOf (p) : p.type, text, juce::Justification::centredLeft, true);

    g.setColour (t.outline.withAlpha (0.5f));
    g.fillRect (0, height - 1, width, 1);
}

void PresetBrowser::listBoxItemClicked (int rowIndex, const juce::MouseEvent& e)
{
    if (! juce::isPositiveAndBelow (rowIndex, (int) rows.size()))
        return;
    const auto& row = rows[(size_t) rowIndex];
    if (row.isGroup())
    {
        if (e.mods.isPopupMenu())
            showGroupMenu (rowIndex);
        else
            toggleGroupRow (rowIndex);
        return;
    }

    // Right-click opens the row menu and does nothing else (never loads, never stars).
    if (e.mods.isPopupMenu())
    {
        showRowMenu (rowIndex);
        return;
    }
    // Shift / Cmd-click only extends the selection (for multi-drag); it must not load.
    if (e.mods.isShiftDown() || e.mods.isCommandDown())
        return;

    // The star zone toggles the favourite instead of loading.
    if (e.getMouseDownX() > list.getWidth() - 34)
    {
        toggleFavouriteRow (rowIndex);
        return;
    }

    const auto res = processor.getPresetManager().loadPreset (row.presetIndex);
    if (! res.ok)
        message ("Presets", "The preset could not be loaded:\n" + res.error);
    list.repaint();
}

void PresetBrowser::returnKeyPressed (int row)
{
    if (! juce::isPositiveAndBelow (row, (int) rows.size()))
        return;
    if (rows[(size_t) row].isGroup())
    {
        toggleGroupRow (row);
        return;
    }
    const auto res = processor.getPresetManager().loadPreset (rows[(size_t) row].presetIndex);
    if (! res.ok)
        message ("Presets", "The preset could not be loaded:\n" + res.error);
}

void PresetBrowser::toggleFavouriteRow (int row)
{
    const auto* p = presetAtRow (row);
    if (p == nullptr)
        return;
    const auto key = PM::favouriteKey (*p);
    settings::setPresetFavorite (key, ! favouriteKeys.contains (key));
    favouriteKeys = settings::getFavoritePresets();
    if (favouritesChip->getToggleState())
        applyFilter();   // un-starring removes it from the list
    else
        list.repaintRow (row);
}

void PresetBrowser::backgroundClicked (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
        showBackgroundMenu();
}

bool PresetBrowser::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::escapeKey)
    {
        if (hooks.onClose)
            hooks.onClose();
        return true;
    }
    // Up / Down / Page / Home / End move the highlight, Return loads it: the list
    // does all of that when it has the keyboard; this forwards for when the drawer does.
    return list.keyPressed (k);
}

//==============================================================================
// Menus + preset actions.
bool PresetBrowser::canModifyRow (int row) const
{
    const auto* p = presetAtRow (row);
    return p != nullptr && ! p->isFactory;
}

juce::PopupMenu PresetBrowser::buildRowMenu (int row, juce::StringArray* typeNamesOut, juce::StringArray* moveFoldersOut) const
{
    juce::PopupMenu menu;
    const auto* pp = presetAtRow (row);
    if (pp == nullptr)
        return menu;
    const auto& p = *pp;
    const bool user = ! p.isFactory;

    menu.addSectionHeader (p.name);
    menu.addItem (renameMenuItemId, "Rename...", user);
    menu.addItem (deleteMenuItemId, "Move to Trash", user);
    menu.addItem (exportMenuItemId, "Export preset...");   // read-only, so factory presets too

    // "Move to Folder": user presets only, and only while the list shows real folders.
    {
        juce::PopupMenu moveMenu;
        const bool canMove = user && groupBy == GroupBy::folders;
        moveMenu.addItem (moveToRootMenuItemId, "User (unfiled)", canMove && p.folder.isNotEmpty(),
                          user && p.folder.isEmpty());
        int id = firstMoveFolderMenuItemId;
        juce::StringArray folders;
        for (const auto& f : userFolderList)
        {
            folders.add (f);
            moveMenu.addItem (id++, f.replace ("/", "  /  "), canMove && f != p.folder, user && f == p.folder);
        }
        moveMenu.addSeparator();
        moveMenu.addItem (moveNewFolderMenuItemId, "New Folder...", canMove);
        if (moveFoldersOut != nullptr)
            *moveFoldersOut = folders;
        menu.addSubMenu ("Move to Folder", moveMenu, canMove);
    }

    // "Set type": rewrites the file, so user presets only.
    {
        juce::PopupMenu typeMenu;
        const auto& names = PM::presetTypes();
        int id = firstTypeMenuItemId;
        for (const auto& n : names)
            typeMenu.addItem (id++, n, user, user && p.type == n);
        typeMenu.addSeparator();
        typeMenu.addItem (clearTypeMenuItemId, juce::String (PM::otherTypeLabel) + " (no type)", user, user && p.type.isEmpty());
        if (typeNamesOut != nullptr)
            *typeNamesOut = names;
        menu.addSubMenu ("Set type", typeMenu, user);
    }

    menu.addItem (revealMenuItemId,
                  juce::String ("Reveal in ") + ((juce::SystemStats::getOperatingSystemType() & juce::SystemStats::MacOSX) ? "Finder" : "Explorer"),
                  user && p.file != juce::File());
    return menu;
}

juce::PopupMenu PresetBrowser::buildGroupMenu (int row) const
{
    juce::PopupMenu menu;
    if (groupBy != GroupBy::folders || row < 0 || row >= (int) rows.size() || ! rows[(size_t) row].isGroup())
        return menu;
    using Kind = DisplayRow::Kind;
    const auto& g = rows[(size_t) row];
    const bool user = g.kind == Kind::userRoot || g.kind == Kind::userFolder;
    const int parentDepth = g.relFolder.isEmpty() ? 0 : juce::StringArray::fromTokens (g.relFolder, "/", "").size();
    const bool canNest = user && parentDepth < PM::maxFolderDepth;

    menu.addSectionHeader (g.label);
    // Factory groups carry the items DISABLED -- feedback beats a dead click.
    menu.addItem (newFolderMenuItemId, "New Folder...", canNest);
    menu.addItem (renameFolderMenuItemId, "Rename Folder...", g.kind == Kind::userFolder);
    menu.addItem (exportFolderMenuItemId, "Export Folder...", g.kind == Kind::userFolder && g.count > 0);
    menu.addItem (trashFolderMenuItemId, "Move Folder to Trash", g.kind == Kind::userFolder);
    return menu;
}

juce::PopupMenu PresetBrowser::buildBackgroundMenu() const
{
    juce::PopupMenu menu;
    if (groupBy == GroupBy::folders)
        menu.addItem (newFolderMenuItemId, "New Folder...");
    return menu;
}

void PresetBrowser::showRowMenu (int row)
{
    juce::StringArray typeNames, moveFolders;
    auto menu = buildRowMenu (row, &typeNames, &moveFolders);
    if (menu.getNumItems() == 0)
        return;
    const auto* p = presetAtRow (row);
    const auto file = p != nullptr ? p->file : juce::File();

    menu.showMenuAsync (juce::PopupMenu::Options().withMousePosition(),
                        [safe = juce::Component::SafePointer<PresetBrowser> (this), row, file, typeNames, moveFolders] (int result)
                        {
                            if (safe == nullptr || result == 0)
                                return;
                            if (result == deleteMenuItemId)
                                safe->deleteRow (row);
                            else if (result == renameMenuItemId)
                            {
                                if (safe->hooks.promptRename && safe->canModifyRow (row))
                                    safe->hooks.promptRename (file);
                            }
                            else if (result == exportMenuItemId)
                                safe->exportRow (row);
                            else if (result == revealMenuItemId)
                                file.revealToUser();
                            else if (result == clearTypeMenuItemId)
                                safe->applyTypeToRow (row, {});
                            else if (result == moveToRootMenuItemId || result == moveNewFolderMenuItemId
                                     || (result >= firstMoveFolderMenuItemId && result < firstMoveFolderMenuItemId + moveFolders.size()))
                                safe->applyMoveFromMenu (row, result, moveFolders);
                            else if (result >= firstTypeMenuItemId && result < firstTypeMenuItemId + typeNames.size())
                                safe->applyTypeToRow (row, typeNames[result - firstTypeMenuItemId]);
                        });
}

void PresetBrowser::showGroupMenu (int row)
{
    auto menu = buildGroupMenu (row);
    if (menu.getNumItems() == 0)
        return;
    const auto rel = rows[(size_t) row].relFolder;
    menu.showMenuAsync (juce::PopupMenu::Options().withMousePosition(),
                        [safe = juce::Component::SafePointer<PresetBrowser> (this), rel] (int result)
                        {
                            if (safe == nullptr || result == 0)
                                return;
                            if (result == newFolderMenuItemId)
                                safe->promptNewFolder (rel);
                            else if (result == renameFolderMenuItemId)
                                safe->promptRenameFolder (rel);
                            else if (result == exportFolderMenuItemId)
                                safe->exportFolderRel (rel);
                            else if (result == trashFolderMenuItemId)
                                safe->confirmTrashFolder (rel);
                        });
}

void PresetBrowser::showBackgroundMenu()
{
    auto menu = buildBackgroundMenu();
    if (menu.getNumItems() == 0)
        return;
    menu.showMenuAsync (juce::PopupMenu::Options().withMousePosition(),
                        [safe = juce::Component::SafePointer<PresetBrowser> (this)] (int result)
                        {
                            if (safe != nullptr && result == newFolderMenuItemId)
                                safe->promptNewFolder ({});
                        });
}

void PresetBrowser::applyMoveFromMenu (int row, int result, const juce::StringArray& folders)
{
    const auto* p = presetAtRow (row);
    if (p == nullptr || p->isFactory)
        return;
    const auto file = p->file;
    if (result == moveToRootMenuItemId)
        moveFilesToFolder ({ file }, {});
    else if (result == moveNewFolderMenuItemId)
        promptNewFolder ({}, [safe = juce::Component::SafePointer<PresetBrowser> (this), file] (const juce::String& newRel)
                         {
                             if (safe != nullptr)
                                 safe->moveFilesToFolder ({ file }, newRel);
                         });
    else if (const auto idx = result - firstMoveFolderMenuItemId; idx >= 0 && idx < folders.size())
        moveFilesToFolder ({ file }, folders[idx]);
}

bool PresetBrowser::deleteRow (int row)
{
    if (! canModifyRow (row))
        return false;
    const auto file = presetAtRow (row)->file;   // copied: refresh() below rebuilds `presets`
    juce::String err;
    if (! processor.getPresetManager().deletePreset (file, &err))
    {
        message ("Move to Trash failed", err);
        return false;
    }
    refresh();   // the manager's rescan broadcast is async; the row goes now, filters re-applied
    return true;
}

bool PresetBrowser::applyTypeToRow (int row, const juce::String& type)
{
    if (! canModifyRow (row))
        return false;
    const auto file = presetAtRow (row)->file;
    juce::String err;
    const auto newType = type == PM::otherTypeLabel ? juce::String() : type;
    if (! processor.getPresetManager().setPresetType (file, newType, &err))
    {
        message ("Set type failed", err);
        return false;
    }
    refresh();
    return true;
}

//==============================================================================
// Folders.
PM::OpResult PresetBrowser::createFolder (const juce::String& parentRel, const juce::String& name)
{
    auto r = processor.getPresetManager().createUserFolder (parentRel, name);
    if (r.ok)
        refresh();
    return r;
}

PM::OpResult PresetBrowser::renameFolder (const juce::String& rel, const juce::String& newName)
{
    // The collapse keys embed the folder path -- carry them across the rename.
    const auto collapsed = settings::getCollapsedPresetGroups();
    auto r = processor.getPresetManager().renameUserFolder (rel, newName);
    if (r.ok)
    {
        const auto parent = rel.contains ("/") ? rel.upToLastOccurrenceOf ("/", true, false) : juce::String();
        const auto newRel = parent + r.file.getFileName();
        for (const auto& k : collapsed)
            if (k == "U:" + rel || k.startsWith ("U:" + rel + "/"))
            {
                settings::setPresetGroupCollapsed (k, false);
                settings::setPresetGroupCollapsed ("U:" + newRel + k.substring (rel.length() + 2), true);
            }
        refresh();
    }
    return r;
}

PM::OpResult PresetBrowser::trashFolder (const juce::String& rel)
{
    const auto collapsed = settings::getCollapsedPresetGroups();
    auto r = processor.getPresetManager().trashUserFolder (rel);
    if (r.ok)
    {
        for (const auto& k : collapsed)
            if (k == "U:" + rel || k.startsWith ("U:" + rel + "/"))
                settings::setPresetGroupCollapsed (k, false);
        refresh();
    }
    return r;
}

void PresetBrowser::promptNewFolder (const juce::String& parentRel, std::function<void (const juce::String&)> onCreated)
{
    if (! hooks.promptText)
        return;
    hooks.promptText ("New folder", "NAME", {}, "Create",
                      [safe = juce::Component::SafePointer<PresetBrowser> (this), parentRel, onCreated] (const juce::String& name)
                      {
                          if (safe == nullptr)
                              return;
                          const auto r = safe->createFolder (parentRel, name);
                          if (! r.ok)
                              safe->message ("New folder failed", r.error);
                          else if (onCreated)
                              onCreated (r.file.getRelativePathFrom (safe->processor.getPresetManager().getUserFolder())
                                             .replaceCharacter ('\\', '/'));
                      });
}

void PresetBrowser::promptRenameFolder (const juce::String& rel)
{
    if (! hooks.promptText)
        return;
    hooks.promptText ("Rename folder", "NAME", rel.fromLastOccurrenceOf ("/", false, false), "Rename",
                      [safe = juce::Component::SafePointer<PresetBrowser> (this), rel] (const juce::String& name)
                      {
                          if (safe == nullptr)
                              return;
                          const auto r = safe->renameFolder (rel, name);
                          if (! r.ok)
                              safe->message ("Rename failed", r.error);
                      });
}

void PresetBrowser::confirmTrashFolder (const juce::String& rel)
{
    if (! hooks.confirm)
        return;
    const auto n = processor.getPresetManager().countPresetsInFolder (rel);
    const auto name = rel.fromLastOccurrenceOf ("/", false, false);
    hooks.confirm ("Move folder to Trash",
                   "Move the folder \"" + name + "\" and the " + juce::String (n) + " preset" + (n == 1 ? "" : "s")
                       + " in it to the Trash? You can recover them from the Trash.",
                   "Move to Trash",
                   [safe = juce::Component::SafePointer<PresetBrowser> (this), rel]
                   {
                       if (safe == nullptr)
                           return;
                       const auto r = safe->trashFolder (rel);
                       if (! r.ok)
                           safe->message ("Move to Trash failed", r.error);
                   });
}

PresetBrowser::MoveSummary PresetBrowser::moveFilesToFolder (const juce::Array<juce::File>& files,
                                                             const juce::String& destRel, bool notify)
{
    MoveSummary s;
    for (const auto& f : files)
    {
        const auto res = processor.getPresetManager().moveUserPreset (f, destRel);
        if (res.ok)
            ++s.moved;
        else
            s.refused.add (f.getFileNameWithoutExtension() + ": " + res.error);
    }
    refresh();
    if (notify && ! s.refused.isEmpty())
        message ("Some presets were not moved", s.refused.joinIntoString ("\n"));
    return s;
}

//==============================================================================
// Drag and drop of presets onto folder rows.
juce::var PresetBrowser::getDragSourceDescription (const juce::SparseSet<int>& selected)
{
    // Folders grouping only (the Type groups have no folder to drop on), and only
    // user presets travel -- factory rows are simply not draggable.
    if (groupBy != GroupBy::folders)
        return {};
    juce::Array<juce::var> files;
    for (int i = 0; i < selected.size(); ++i)
        if (const auto* p = presetAtRow (selected[i]); p != nullptr && ! p->isFactory)
            files.add (p->file.getFullPathName());
    if (files.isEmpty())
        return {};
    return juce::var (files);
}

bool PresetBrowser::dropTargetFolderForRow (int row, juce::String& rel) const
{
    if (groupBy != GroupBy::folders || row < 0 || row >= (int) rows.size())
        return false;
    const auto& r = rows[(size_t) row];
    using Kind = DisplayRow::Kind;
    if (r.kind == Kind::userRoot || r.kind == Kind::userFolder)
    {
        rel = r.relFolder;
        return true;
    }
    if (r.kind == Kind::preset)
        if (const auto& p = presets[(size_t) r.presetIndex]; ! p.isFactory)
        {
            rel = p.folder;   // dropping on a preset = its own folder
            return true;
        }
    return false;
}

bool PresetBrowser::dragInterested (const juce::DragAndDropTarget::SourceDetails& d) const
{
    return d.sourceComponent.get() == &list && d.description.isArray() && groupBy == GroupBy::folders;
}

void PresetBrowser::dragMoved (const juce::DragAndDropTarget::SourceDetails& d)
{
    const auto p = d.localPosition;
    list.getViewport()->autoScroll (p.x, p.y, 24, 12);
    juce::String rel;
    const auto row = list.getRowContainingPosition (p.x, p.y);
    const int newRow = dropTargetFolderForRow (row, rel) ? row : -1;
    if (newRow != dropRow)
    {
        dropRow = newRow;
        list.repaint();
    }
}

void PresetBrowser::dragExited()
{
    dropRow = -1;
    list.repaint();
}

void PresetBrowser::dragDropped (const juce::DragAndDropTarget::SourceDetails& d)
{
    const auto row = list.getRowContainingPosition (d.localPosition.x, d.localPosition.y);
    dropRow = -1;
    list.repaint();
    juce::String rel;
    if (! dropTargetFolderForRow (row, rel) || ! d.description.isArray())
        return;
    juce::Array<juce::File> files;
    for (const auto& v : *d.description.getArray())
        files.add (juce::File (v.toString()));
    moveFilesToFolder (files, rel);
}

//==============================================================================
// Export / import.
void PresetBrowser::exportRow (int row)
{
    const auto* p = presetAtRow (row);
    if (p == nullptr)
        return;
    const auto info = *p;
    chooser = std::make_unique<juce::FileChooser> ("Export preset",
                                                   juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                                                       .getChildFile (info.name + PM::presetExtension),
                                                   "*" + juce::String (PM::presetExtension));
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
                          [safe = juce::Component::SafePointer<PresetBrowser> (this), info] (const juce::FileChooser& fc)
                          {
                              auto dest = fc.getResult();
                              if (safe == nullptr || dest == juce::File())
                                  return;
                              if (! dest.hasFileExtension (PM::presetExtension))
                                  dest = juce::File (dest.getFullPathName() + PM::presetExtension);
                              juce::String err;
                              if (! safe->processor.getPresetManager().exportPreset (info, dest, &err))
                                  safe->message ("Export failed", err);
                          });
}

void PresetBrowser::exportFolderRel (const juce::String& rel)
{
    const auto name = rel.fromLastOccurrenceOf ("/", false, false);
    chooser = std::make_unique<juce::FileChooser> ("Export folder",
                                                   juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                                                       .getChildFile (name + ".zip"),
                                                   "*.zip");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles,
                          [safe = juce::Component::SafePointer<PresetBrowser> (this), rel] (const juce::FileChooser& fc)
                          {
                              auto dest = fc.getResult();
                              if (safe == nullptr || dest == juce::File())
                                  return;
                              if (! dest.hasFileExtension ("zip"))
                                  dest = juce::File (dest.getFullPathName() + ".zip");
                              juce::String err;
                              if (! safe->processor.getPresetManager().exportFolder (rel, dest, &err))
                                  safe->message ("Export failed", err);
                          });
}

void PresetBrowser::startImport()
{
    chooser = std::make_unique<juce::FileChooser> ("Import presets (.spastrip files, a folder, or a .zip)",
                                                   juce::File::getSpecialLocation (juce::File::userDocumentsDirectory));
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::canSelectDirectories
                              | juce::FileBrowserComponent::canSelectMultipleItems,
                          [safe = juce::Component::SafePointer<PresetBrowser> (this)] (const juce::FileChooser& fc)
                          {
                              if (safe != nullptr)
                                  safe->importFromPaths (fc.getResults());
                          });
}

bool PresetBrowser::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& f : files)
    {
        const juce::File file (f);
        if (file.isDirectory() || file.hasFileExtension (PM::presetExtension) || file.hasFileExtension ("zip"))
            return true;
    }
    return false;
}

void PresetBrowser::filesDropped (const juce::StringArray& files, int, int)
{
    draggingOver = false;
    repaint();
    juce::Array<juce::File> paths;
    for (const auto& f : files)
        paths.add (juce::File (f));
    importFromPaths (paths);
}

void PresetBrowser::importFromPaths (const juce::Array<juce::File>& paths)
{
    if (paths.isEmpty())
        return;
    // Fully async: the session is flattened up front (no writes yet) and driven one
    // file at a time; a name clash pauses it for an in-editor question. The shared_ptr
    // lets it outlive this drawer -- the manager (owned by the processor) does the writes.
    std::shared_ptr<PM::ImportSession> session (processor.getPresetManager().beginImport (paths).release());
    continueImport (session);
}

void PresetBrowser::continueImport (std::shared_ptr<PM::ImportSession> session)
{
    const auto step = session->advance();
    if (step.finished)
    {
        const auto result = session->finish();
        juce::StringArray lines;
        lines.add (juce::String (result.imported) + (result.imported == 1 ? " preset imported." : " presets imported."));
        if (! result.malformed.isEmpty())
            lines.add ("\nSkipped (not a SPAStrip preset, or unreadable):\n" + result.malformed.joinIntoString ("\n"));
        if (! result.rejectedZipSlip.isEmpty())
            lines.add ("\nRejected (an entry tried to write outside its folder):\n" + result.rejectedZipSlip.joinIntoString ("\n"));
        if (result.imported > 0 || ! result.malformed.isEmpty() || ! result.rejectedZipSlip.isEmpty())
            message("Import finished", lines.joinIntoString ("\n"));
        return;
    }

    // A name clash: ask (one decision, optionally for every later clash), then resume.
    if (! hooks.askClash)
    {
        session->decide (PM::ImportClash::skip, true);
        continueImport (session);
        return;
    }
    hooks.askClash (step.clashName,
                    [safe = juce::Component::SafePointer<PresetBrowser> (this), session]
                    (PM::ImportClash action, bool applyToRest)
                    {
                        session->decide (action, applyToRest);
                        if (safe != nullptr)
                            safe->continueImport (session);
                    });
}

//==============================================================================
void PresetBrowser::paint (juce::Graphics& g)
{
    const auto& t = currentTheme();
    auto bounds = getLocalBounds();
    const auto shadow = bounds.removeFromRight (shadowWidth);

    g.setGradientFill (juce::ColourGradient (juce::Colours::black.withAlpha (0.35f), (float) shadow.getX(), 0.0f,
                                             juce::Colours::transparentBlack, (float) shadow.getRight(), 0.0f, false));
    g.fillRect (shadow);

    g.setColour (t.panel);
    g.fillRect (bounds);
    g.setColour (t.outline);
    g.drawVerticalLine (bounds.getRight() - 1, 0.0f, (float) getHeight());

    g.setColour (t.textSecondary);
    g.setFont (metrics::sectionFont().withHeight (metrics::sectionFont().getHeight() + 2.0f));
    g.drawText ("PRESETS", titleArea, juce::Justification::centredLeft);

    draw::displayWell (g, listWell.toFloat().expanded (2.0f), false);

    if (filtered.empty())
    {
        // Below whatever group rows are still listed (the User root stays so it can be dropped into).
        g.setColour (t.textSecondary);
        g.setFont (browserSmallFont());
        const auto area = listWell.withTrimmedTop ((int) rows.size() * rowHeight + 14).reduced (16, 0).withHeight (64);
        g.drawFittedText (presets.empty() ? "No presets yet.\nPress SAVE to keep the current sound,\nor IMPORT presets from a file."
                                          : "No presets match.",
                          area, juce::Justification::centredTop, 4);
    }

    // Drop target highlight while a file / folder / zip drag hovers the drawer.
    if (draggingOver)
    {
        g.setColour (t.accent.withAlpha (0.5f));
        g.drawRect (getLocalBounds().withTrimmedRight (shadowWidth).reduced (1), 2);
    }
}

void PresetBrowser::resized()
{
    auto bounds = getLocalBounds();
    bounds.removeFromRight (shadowWidth);
    bounds.reduce (10, 10);

    auto header = bounds.removeFromTop (22);
    closeButton.setBounds (header.removeFromRight (22));
    header.removeFromRight (6);
    groupTypeButton.setBounds (header.removeFromRight (48));
    header.removeFromRight (3);
    groupFoldersButton.setBounds (header.removeFromRight (68));
    titleArea = header;

    bounds.removeFromTop (8);
    searchBox.setBounds (bounds.removeFromTop (26));

    bounds.removeFromTop (6);
    auto filterRow = bounds.removeFromTop (24);
    favouritesChip->setBounds (filterRow.removeFromRight (28));
    filterRow.removeFromRight (4);
    typeBox.setBounds (filterRow.removeFromLeft ((filterRow.getWidth() - 4) / 2));
    filterRow.removeFromLeft (4);
    categoryBox.setBounds (filterRow);

    auto footer = bounds.removeFromBottom (42);
    auto buttons = footer.removeFromBottom (24);
    initButton.setBounds (buttons.removeFromLeft (52));
    buttons.removeFromLeft (4);
    importButton.setBounds (buttons.removeFromRight (buttons.getWidth() / 2 - 2));
    buttons.removeFromRight (4);
    newFolderButton.setBounds (buttons);
    countLabel.setBounds (footer.withTrimmedBottom (2));

    bounds.removeFromBottom (6);
    bounds.removeFromTop (8);
    listWell = bounds;
    list.setBounds (bounds.reduced (1));
}

} // namespace spa::ui
