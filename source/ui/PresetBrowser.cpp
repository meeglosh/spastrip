#include "PresetBrowser.h"

#include "UiSettings.h"

namespace spa::ui
{

namespace
{
    juce::Font browserFont()      { return juce::Font (juce::FontOptions (13.0f)); }
    juce::Font browserSmallFont() { return juce::Font (juce::FontOptions (10.5f)); }
}

PresetBrowser::PresetBrowser (SPAStripProcessor& p, Hooks h) : processor (p), hooks (std::move (h))
{
    setComponentID ("presetBrowser");
    setWantsKeyboardFocus (true);

    closeButton.setTooltip ("Close the preset browser (Esc)");
    closeButton.onClick = [this] { if (hooks.onClose) hooks.onClose(); };
    addAndMakeVisible (closeButton);

    searchBox.setTextToShowWhenEmpty ("Search presets", currentTheme().textSecondary.withAlpha (0.7f));
    searchBox.setSelectAllWhenFocused (true);
    searchBox.setEscapeAndReturnKeysConsumed (false);   // Esc bubbles up = close
    searchBox.onTextChange = [this] { rebuildRows(); };
    searchBox.onEscapeKey = [this] { if (hooks.onClose) hooks.onClose(); };
    addAndMakeVisible (searchBox);

    list.setRowHeight (30);
    list.setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    list.setRowSelectedOnMouseDown (false);
    addAndMakeVisible (list);

    initButton.setTooltip ("Reset every parameter, the chain order, the mod slots and the impulse to their defaults");
    initButton.onClick = [this] { processor.getPresetManager().init(); };
    addAndMakeVisible (initButton);
    importButton.setTooltip ("Import the effects of a SPASynth preset (.spasynth)");
    importButton.onClick = [this] { startImport(); };
    addAndMakeVisible (importButton);

    countLabel.setFont (metrics::smallFont());
    countLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (countLabel);
    summaryLabel.setFont (metrics::smallFont());
    summaryLabel.setJustificationType (juce::Justification::topLeft);
    summaryLabel.setMinimumHorizontalScale (1.0f);
    addAndMakeVisible (summaryLabel);

    processor.getPresetManager().addChangeListener (this);
    refresh();
}

PresetBrowser::~PresetBrowser()
{
    processor.getPresetManager().removeChangeListener (this);
}

void PresetBrowser::refresh()
{
    list.setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    searchBox.setColour (juce::TextEditor::backgroundColourId, currentTheme().display);
    searchBox.setColour (juce::TextEditor::textColourId, currentTheme().textPrimary);
    searchBox.setColour (juce::TextEditor::outlineColourId, currentTheme().outline);
    searchBox.setColour (juce::TextEditor::focusedOutlineColourId, currentTheme().accent.withAlpha (0.7f));
    summaryLabel.setColour (juce::Label::textColourId, currentTheme().textSecondary);
    countLabel.setColour (juce::Label::textColourId, currentTheme().textSecondary);
    rebuildRows();
}

void PresetBrowser::setSearchText (const juce::String& s)
{
    searchBox.setText (s, false);
    rebuildRows();
}

void PresetBrowser::rebuildRows()
{
    auto& pm = processor.getPresetManager();
    const auto& presets = pm.getPresets();
    const auto needle = searchBox.getText().trim();
    const bool searching = needle.isNotEmpty();

    // Group order: the User root, then banks (alphabetical, as the manager lists them), "Factory" last.
    juce::StringArray groups;
    groups.add ({});   // root
    for (const auto& b : pm.getBanks())
        groups.addIfNotAlreadyThere (b);
    for (const auto& p : presets)
        if (p.isFactory)
            groups.addIfNotAlreadyThere (p.bank.isEmpty() ? juce::String ("Factory") : p.bank);

    rows.clear();
    int shown = 0;
    for (const auto& g : groups)
    {
        std::vector<int> members;
        for (int i = 0; i < (int) presets.size(); ++i)
        {
            const auto& p = presets[(size_t) i];
            const auto bank = p.isFactory ? (p.bank.isEmpty() ? juce::String ("Factory") : p.bank) : p.bank;
            if (bank != g)
                continue;
            if (searching && ! p.name.containsIgnoreCase (needle) && ! bank.containsIgnoreCase (needle))
                continue;
            members.push_back (i);
        }
        if (g.isEmpty() && members.empty())
            continue;                                   // nothing in the root
        if (searching && members.empty())
            continue;                                   // hide empty groups while filtering

        const auto label = g.isEmpty() ? juce::String ("User") : g;
        Row group;
        group.isGroup = true;
        group.label = label;
        group.count = (int) members.size();
        group.expanded = searching || ! collapsed.contains (label);
        rows.push_back (group);
        if (group.expanded)
            for (int idx : members)
            {
                Row r;
                r.label = presets[(size_t) idx].name;
                r.presetIndex = idx;
                rows.push_back (r);
            }
        shown += (int) members.size();
    }

    countLabel.setText (juce::String (shown) + (shown == 1 ? " preset" : " presets"), juce::dontSendNotification);
    list.updateContent();
    list.repaint();
    repaint();
}

void PresetBrowser::paintListBoxItem (int rowIndex, juce::Graphics& g, int width, int height, bool)
{
    if (! juce::isPositiveAndBelow (rowIndex, (int) rows.size()))
        return;
    const auto& t = currentTheme();
    const auto& row = rows[(size_t) rowIndex];
    const juce::Rectangle<int> r (0, 0, width, height);

    if (row.isGroup)
    {
        g.setColour (t.outline.withAlpha (0.22f));
        g.fillRect (r);
        const float cy = (float) height * 0.5f, cx = 13.0f;
        juce::Path tri;
        if (row.expanded)
            tri.addTriangle (cx - 5.0f, cy - 3.0f, cx + 5.0f, cy - 3.0f, cx, cy + 4.0f);
        else
            tri.addTriangle (cx - 3.0f, cy - 5.0f, cx - 3.0f, cy + 5.0f, cx + 4.0f, cy);
        g.setColour (t.textSecondary);
        g.fillPath (tri);
        auto text = r.withTrimmedLeft (26).withTrimmedRight (10);
        g.setFont (browserSmallFont());
        g.drawText ("(" + juce::String (row.count) + ")", text.removeFromRight (36), juce::Justification::centredRight);
        g.setColour (t.textPrimary);
        g.setFont (browserFont().boldened());
        g.drawText (row.label.toUpperCase(), text, juce::Justification::centredLeft, true);
        g.setColour (t.outline.withAlpha (0.5f));
        g.fillRect (0, height - 1, width, 1);
        return;
    }

    const bool current = processor.getPresetManager().getCurrentIndex() == row.presetIndex;
    if (current)
    {
        g.setColour (t.accent.withAlpha (0.13f));
        g.fillRect (r);
        g.setColour (t.accent);
        g.fillRect (r.withWidth (2));
    }
    g.setColour (current ? t.textPrimary : t.textPrimary.withAlpha (0.88f));
    g.setFont (current ? browserFont().boldened() : browserFont());
    g.drawText (row.label, r.withTrimmedLeft (24).withTrimmedRight (10), juce::Justification::centredLeft, true);
    g.setColour (t.outline.withAlpha (0.35f));
    g.fillRect (0, height - 1, width, 1);
}

void PresetBrowser::listBoxItemClicked (int rowIndex, const juce::MouseEvent& e)
{
    if (! juce::isPositiveAndBelow (rowIndex, (int) rows.size()))
        return;
    const auto& row = rows[(size_t) rowIndex];
    if (row.isGroup)
    {
        if (collapsed.contains (row.label))
            collapsed.removeString (row.label);
        else
            collapsed.add (row.label);
        rebuildRows();
        return;
    }
    if (e.mods.isPopupMenu())
    {
        showRowMenu (rowIndex);
        return;
    }
    const auto res = processor.getPresetManager().loadPreset (row.presetIndex);
    if (! res.ok && hooks.showMessage)
        hooks.showMessage ("The preset could not be loaded:\n" + res.error);
    list.repaint();
}

void PresetBrowser::showRowMenu (int rowIndex)
{
    if (! juce::isPositiveAndBelow (rowIndex, (int) rows.size()) || rows[(size_t) rowIndex].isGroup)
        return;
    const auto idx = rows[(size_t) rowIndex].presetIndex;
    const auto& presets = processor.getPresetManager().getPresets();
    if (! juce::isPositiveAndBelow (idx, (int) presets.size()))
        return;
    const auto info = presets[(size_t) idx];

    juce::PopupMenu m;
    m.addItem (1, "Load");
    m.addSeparator();
    m.addItem (2, "Rename...", ! info.isFactory);
    m.addItem (3, "Move to Trash", ! info.isFactory);
    m.addItem (4, "Reveal in " + juce::String (juce::SystemStats::getOperatingSystemType() & juce::SystemStats::MacOSX ? "Finder" : "Explorer"),
               info.file != juce::File());
    m.showMenuAsync (juce::PopupMenu::Options(),
                     [safe = juce::Component::SafePointer<PresetBrowser> (this), info, idx] (int result)
                     {
                         if (safe == nullptr)
                             return;
                         auto& pm = safe->processor.getPresetManager();
                         if (result == 1)
                             pm.loadPreset (idx);
                         else if (result == 2 && safe->hooks.promptRename)
                             safe->hooks.promptRename (info.file);
                         else if (result == 3)
                         {
                             juce::String err;
                             if (! pm.deletePreset (info.file, &err) && safe->hooks.showMessage)
                                 safe->hooks.showMessage (err);
                         }
                         else if (result == 4)
                             info.file.revealToUser();
                     });
}

juce::String PresetBrowser::describeImport (const preset::PresetManager::ImportResult& r)
{
    using R = preset::PresetManager::ImportResult;
    if (! r.ok)
        return "Import failed: " + r.error;

    juce::StringArray lines;
    lines.add ("Imported \"" + r.presetName + "\"");
    lines.add (juce::String (r.applied) + " effect parameters applied, " + juce::String (r.defaulted)
               + " set to their defaults (not in the file).");
    if (r.grainSpreadMigrated)
        lines.add ("Grain spread migrated from the 1.0.29 format.");
    switch (r.order)
    {
        case R::Order::applied:         lines.add ("Effect order: applied."); break;
        case R::Order::migratedLegacy:  lines.add ("Effect order: applied (older order: COMP and GRAIN and FILTER added where it had none)."); break;
        case R::Order::invalid:         lines.add ("Effect order: invalid in the file, natural order used."); break;
        case R::Order::notInFile:       lines.add ("Effect order: not in the file, default order used."); break;
    }
    switch (r.ir)
    {
        case R::IR::resolved:    lines.add ("Impulse response: loaded from " + r.irPath); break;
        case R::IR::unresolved:  lines.add ("Impulse response: \"" + r.irPath + "\" could not be found here; the current impulse was kept."); break;
        case R::IR::notInFile:   lines.add ("Impulse response: none in the file; the current impulse was kept."); break;
    }
    if (! r.skippedIds.isEmpty())
        lines.add (juce::String (r.skippedIds.size()) + " synth-only parameters skipped: " + r.skippedIds.joinIntoString (", "));
    return lines.joinIntoString ("\n");
}

void PresetBrowser::setImportSummary (const juce::String& s)
{
    importSummary = s;
    summaryLabel.setText (s, juce::dontSendNotification);
    repaint();
}

void PresetBrowser::startImport()
{
    const auto last = settings::getLastIRFolder();
    chooser = std::make_unique<juce::FileChooser> ("Import a SPASynth preset",
                                                   juce::File::getSpecialLocation (juce::File::userHomeDirectory),
                                                   "*.spasynth");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [safe = juce::Component::SafePointer<PresetBrowser> (this)] (const juce::FileChooser& fc)
                          {
                              if (safe != nullptr && fc.getResult().existsAsFile())
                                  safe->importFile (fc.getResult());
                          });
    juce::ignoreUnused (last);
}

void PresetBrowser::importFile (const juce::File& f)
{
    const auto result = processor.getPresetManager().importSPASynthPreset (f);
    const auto text = describeImport (result);
    setImportSummary (result.ok ? "Imported \"" + result.presetName + "\": " + juce::String (result.applied) + " applied, "
                                      + juce::String (result.defaulted) + " defaulted, "
                                      + juce::String (result.skippedIds.size()) + " skipped."
                                : text);
    if (hooks.onImported)
        hooks.onImported (result);
}

bool PresetBrowser::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::escapeKey)
    {
        if (hooks.onClose)
            hooks.onClose();
        return true;
    }
    return false;
}

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
}

void PresetBrowser::resized()
{
    auto bounds = getLocalBounds();
    bounds.removeFromRight (shadowWidth);
    bounds.reduce (10, 10);

    auto header = bounds.removeFromTop (22);
    closeButton.setBounds (header.removeFromRight (22));
    titleArea = header;

    bounds.removeFromTop (8);
    searchBox.setBounds (bounds.removeFromTop (26));
    bounds.removeFromTop (8);

    auto footer = bounds.removeFromBottom (86);
    auto summary = footer.removeFromBottom (44);
    summaryLabel.setBounds (summary);
    auto buttons = footer.removeFromTop (24);
    initButton.setBounds (buttons.removeFromLeft (56));
    buttons.removeFromLeft (6);
    importButton.setBounds (buttons);
    footer.removeFromTop (4);
    countLabel.setBounds (footer.removeFromTop (14));

    bounds.removeFromBottom (6);
    listWell = bounds;
    list.setBounds (bounds.reduced (1));
}

} // namespace spa::ui
