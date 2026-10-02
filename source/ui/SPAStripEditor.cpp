#include "SPAStripEditor.h"

#include "../SPAStripProcessor.h"
#include "../mod/ModTargets.h"
#include "CompPanel.h"
#include "EqEditor.h"
#include "FilterPanel.h"
#include "LimiterDisplay.h"
#include "SPAStripBrandingData.h"

namespace spa::ui
{

namespace
{
    constexpr const char* kStateFxTab = "uiFxTab";
    constexpr const char* kStateDrawer = "uiPresetDrawerOpen";

    // Fixed-seed per-pixel noise tile for the faceplate grain (generated once per editor).
    juce::Image makeFaceplateNoiseTexture()
    {
        constexpr int size = 96;
        juce::Image img (juce::Image::ARGB, size, size, false);
        juce::Random rng (0x5b0a5adeu);
        juce::Image::BitmapData bd (img, juce::Image::BitmapData::writeOnly);
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x)
            {
                const auto v = (juce::uint8) rng.nextInt (256);
                bd.setPixelColour (x, y, juce::Colour (v, v, v));
            }
        return img;
    }

    // Truncates `text` so text + suffix fits, keeping the suffix (the edited marker) visible.
    juce::String truncateKeepingSuffix (const juce::String& text, const juce::String& suffix,
                                        const juce::Font& font, int maxWidth)
    {
        const auto full = text + suffix;
        if (maxWidth <= 0 || juce::GlyphArrangement::getStringWidth (font, full) <= (float) maxWidth || text.isEmpty())
            return full;
        const auto budget = (float) maxWidth - juce::GlyphArrangement::getStringWidth (font, suffix)
                          - juce::GlyphArrangement::getStringWidth (font, "...");
        auto truncated = text;
        while (truncated.isNotEmpty() && juce::GlyphArrangement::getStringWidth (font, truncated) > budget)
            truncated = truncated.dropLastCharacters (1);
        return truncated + "..." + suffix;
    }

    const char* const kTabNames[dsp::FXChain::numModules] = { "DIST", "CHORUS", "DELAY", "REVERB", "EQ",
                                                              "MOD", "TREM/VIB", "LIMIT", "CONV", "COMP", "GRAIN",
                                                              "FILTER" };
}

//==============================================================================
void ContentComponent::AccentButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    const auto& t = currentTheme();
    auto bounds = getLocalBounds().toFloat();
    const auto d = juce::jmin (bounds.getWidth(), bounds.getHeight()) - 4.0f;
    const auto circle = bounds.withSizeKeepingCentre (d, d);
    g.setColour (t.accent);
    g.fillEllipse (circle);
    g.setColour (t.outline);
    g.drawEllipse (circle, 1.0f);
    if (highlighted || down)
    {
        g.setColour (juce::Colours::white.withAlpha (down ? 0.25f : 0.12f));
        g.fillEllipse (circle);
    }
}

void ContentComponent::LogoButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    if (auto* owner = findParentComponentOfClass<ContentComponent>())
        if (owner->logo != nullptr)
        {
            const auto a = highlighted || down ? 1.0f : 0.9f;
            owner->logo->drawWithin (g, getLocalBounds().reduced (10).toFloat(), juce::RectanglePlacement::centred, a);
        }
}

//==============================================================================
ContentComponent::ContentComponent (SPAStripProcessor& p)
    : processor (p), modViz (p, *this),
      oversampling (p.getAPVTS(), params::id::oversampling),
      sidechainPanel (p), modPanel (p), ioPanel (p)
{
    logo = juce::Drawable::createFromImageData (spa_brand::SPAudio_logo_white_svg, spa_brand::SPAudio_logo_white_svgSize);
    noiseTexture = makeFaceplateNoiseTexture();
    lastAccent = currentTheme().accent;

    // Opaque: paint() fills the full bounds first (matters for hosts that clear a non-opaque root).
    setOpaque (true);
    setWantsKeyboardFocus (true);   // owns Cmd/Ctrl+Z when the editor has focus

    //--- Header ---------------------------------------------------------------
    logoButton.setTooltip ("SPAStrip menu: about, accent colour");
    logoButton.onClick = [this] { showLogoMenu(); };
    addAndMakeVisible (logoButton);

    prevPresetButton.setComponentID ("navPrev");
    prevPresetButton.setTooltip ("Previous preset");
    prevPresetButton.onClick = [this] { processor.getPresetManager().loadPrevious(); refreshAll(); };
    addAndMakeVisible (prevPresetButton);
    nextPresetButton.setComponentID ("navNext");
    nextPresetButton.setTooltip ("Next preset");
    nextPresetButton.onClick = [this] { processor.getPresetManager().loadNext(); refreshAll(); };
    addAndMakeVisible (nextPresetButton);
    presetNameButton.setComponentID ("presetName");
    presetNameButton.setTooltip ("Browse presets");
    presetNameButton.onClick = [this] { setPresetBrowserOpen (! presetBrowserOpen, true); };
    addAndMakeVisible (presetNameButton);
    savePresetButton.setComponentID ("savePreset");
    savePresetButton.onClick = [this] { onSaveClicked(); };
    addAndMakeVisible (savePresetButton);
    initButton.setTooltip ("Reset every parameter, the effect order, the mod slots and the impulse to their defaults");
    initButton.onClick = [this] { processor.getPresetManager().init(); refreshAll(); };
    addAndMakeVisible (initButton);

    undoButton.setComponentID ("undoButton");
    undoButton.onClick = [this] { processor.undo(); refreshAll(); };
    addAndMakeVisible (undoButton);
    redoButton.setComponentID ("redoButton");
    redoButton.onClick = [this] { processor.redo(); refreshAll(); };
    addAndMakeVisible (redoButton);

    randomizeButton.setComponentID ("primary");
    randomizeButton.setTooltip ("Randomize every unlocked effect (one undo step)");
    randomizeButton.onClick = [this] { processor.randomizeAll(); refreshAll(); };
    addAndMakeVisible (randomizeButton);

    wildSlider.setComponentID ("wild");   // value ring heats toward red with amount
    wildSlider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    wildSlider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    wildSlider.setWantsKeyboardFocus (false);
    wildSlider.setRange (0.0, 1.0, 0.0);
    wildSlider.setDoubleClickReturnValue (true, 0.5);
    wildSlider.setValue (processor.getRandomWildness(), juce::dontSendNotification);
    wildSlider.onValueChange = [this]
    {
        processor.setRandomWildness ((float) wildSlider.getValue());
        if (wildSlider.isMouseButtonDown())
            wildLabel.setText (juce::String (juce::roundToInt (wildSlider.getValue() * 100.0)) + "%", juce::dontSendNotification);
    };
    wildSlider.onDragEnd = [this] { wildLabel.setText ("WILD", juce::dontSendNotification); };
    wildSlider.setTooltip ("Chaos amount: how wild RANDOMIZE ALL rolls");
    addAndMakeVisible (wildSlider);
    wildLabel.setText ("WILD", juce::dontSendNotification);
    wildLabel.setFont (metrics::smallFont());
    wildLabel.setJustificationType (juce::Justification::centred);
    wildLabel.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (wildLabel);

    oversampling.combo.setTooltip ("Oversampling of the whole effect chain (1x / 2x / 4x)");
    addAndMakeVisible (oversampling);
    accentButton.setTooltip ("Accent colour");
    accentButton.onClick = [this] { showAccentPicker(); };
    addAndMakeVisible (accentButton);

    //--- FX tabs ----------------------------------------------------------------
    fxTabs.setTabBarDepth (metrics::sectionHeaderHeight);
    const auto tabBg = juce::Colours::transparentBlack;
    auto& tel = processor.getTelemetry();
    auto& apvts = processor.getAPVTS();
    namespace fx = params::id::fx;
    // Tabs are added in MODULE-ID order (DIST=0 .. FILTER=11) and then reordered to the chain order.
    fxTabs.addTab ("DIST", tabBg, new FXPanel (apvts, FXDisplay::Kind::distortion, params::Section::fxDist, "Distortion",
                                               juce::StringArray { fx::distEnable }, &tel), true);
    fxTabs.addTab ("CHORUS", tabBg, new FXPanel (apvts, FXDisplay::Kind::chorus, params::Section::fxChorus, "Chorus",
                                                 juce::StringArray { fx::chorusEnable }, &tel), true);
    fxTabs.addTab ("DELAY", tabBg, new FXPanel (apvts, FXDisplay::Kind::delay, params::Section::fxDelay, "Delay",
                                                juce::StringArray { fx::delayEnable }, &tel), true);
    fxTabs.addTab ("REVERB", tabBg, new FXPanel (apvts, FXDisplay::Kind::reverb, params::Section::fxReverb, "Reverb",
                                                 juce::StringArray { fx::reverbEnable }, &tel), true);
    fxTabs.addTab ("EQ", tabBg, new EqEditor (processor), true);
    fxTabs.addTab ("MOD", tabBg, new FXPanel (apvts, FXDisplay::Kind::mod, params::Section::fxMod, "Modulation",
                                              juce::StringArray { fx::modEnable }, &tel), true);
    fxTabs.addTab ("TREM/VIB", tabBg, new FXPanel (apvts, FXDisplay::Kind::tremVib, params::Section::fxTremVib, "Trem / Vib",
                                                   juce::StringArray { fx::tremEnable, fx::vibEnable }, &tel), true);
    fxTabs.addTab ("LIMIT", tabBg, new LimiterPanel (apvts, tel), true);
    convolvePanel = new ConvolvePanel (processor);
    fxTabs.addTab ("CONV", tabBg, convolvePanel, true);
    fxTabs.addTab ("COMP", tabBg, new CompPanel (apvts, tel), true);
    fxTabs.addTab ("GRAIN", tabBg, new FXPanel (apvts, FXDisplay::Kind::grain, params::Section::fxGrain, "Grain",
                                                juce::StringArray { fx::grainEnable }, &tel), true);
    fxTabs.addTab ("FILTER", tabBg, new FilterPanel (apvts, [this] { return processor.getSampleRate(); }), true);
    addAndMakeVisible (fxTabs);

    {
        juce::StringArray names;
        for (auto* n : kTabNames)
            names.add (n);
        fxTabs.setModuleNames (names);
    }
    fxTabs.applyOrder (processor.getFxOrder());

    fxTabs.onOrderChanged = [this]
    {
        // Every swap of one drag lands in ONE undo step (closed on mouse-up).
        if (tabDragStep == nullptr)
            tabDragStep = std::make_unique<SPAStripProcessor::UndoStep> (processor, "FX ORDER");
        processor.setFxOrder (fxTabs.currentOrder());
    };
    fxTabs.onDragEnd = [this] { tabDragStep.reset(); };
    fxTabs.isTabLocked = [this] (const juce::String& n) { return isTabLocked (n); };
    fxTabs.onLockClicked = [this] (const juce::String& n) { setLockedFromTab (n); };

    {
        const auto saved = apvts.state.getProperty (kStateFxTab, "EQ").toString();
        const int idx = fxTabs.getTabNames().indexOf (saved);
        fxTabs.setCurrentTabIndex (idx >= 0 ? idx : fxTabs.getTabNames().indexOf ("EQ"));
    }

    tabEngagement = std::make_unique<TabEngagementTracker> (apvts,
        std::vector<std::pair<juce::String, std::vector<juce::String>>> {
            { "DIST",     { fx::distEnable } },   { "CHORUS", { fx::chorusEnable } },
            { "DELAY",    { fx::delayEnable } },  { "REVERB", { fx::reverbEnable } },
            { "EQ",       { fx::eqEnable } },     { "MOD",    { fx::modEnable } },
            { "TREM/VIB", { fx::tremEnable, fx::vibEnable } },
            { "LIMIT",    { fx::limEnable } },    { "CONV",   { fx::convEnable } },
            { "COMP",     { fx::compEnable } },   { "GRAIN",  { fx::grainEnable } },
            { "FILTER",   { fx::filterEnable, fx::filter2Enable } } },
        fxTabs);
    fxTabs.isTabEngaged = [this] (const juce::String& n) { return tabEngagement != nullptr && tabEngagement->isEngaged (n); };

    //--- Bottom row ----------------------------------------------------------
    addAndMakeVisible (sidechainPanel);
    addAndMakeVisible (modPanel);
    addAndMakeVisible (ioPanel);

    //--- Preset drawer (topmost but dialogs) -------------------------------------
    PresetBrowser::Hooks hooks;
    hooks.onClose = [this] { setPresetBrowserOpen (false, true); };
    hooks.promptRename = [this] (const juce::File& f) { showRenameDialog (f); };
    hooks.promptText = [this] (const juce::String& t, const juce::String& pr, const juce::String& init,
                               const juce::String& ok, std::function<void (const juce::String&)> cb)
    { promptText (t, pr, init, ok, std::move (cb)); };
    hooks.confirm = [this] (const juce::String& t, const juce::String& b, const juce::String& ok, std::function<void()> cb)
    { showConfirm (t, b, ok, std::move (cb)); };
    hooks.askClash = [this] (const juce::String& name, std::function<void (preset::PresetManager::ImportClash, bool)> cb)
    { showClashDialog (name, std::move (cb)); };
    hooks.showMessage = [this] (const juce::String& t, const juce::String& m) { showMessage (t, m); };
    presetBrowser = std::make_unique<PresetBrowser> (processor, std::move (hooks));
    addChildComponent (*presetBrowser);

    //--- Wiring ----------------------------------------------------------------
    fxTabs.getTabbedButtonBar().addChangeListener (this);
    processor.getUndoBroadcaster().addChangeListener (this);
    processor.getPresetManager().addChangeListener (this);

    {
        std::vector<juce::Component*> panels;
        for (int i = 0; i < fxTabs.getNumTabs(); ++i)
            panels.push_back (fxTabs.getTabContentComponent (i));
        modViz.setExtraRoots (std::move (panels));
    }
    modViz.rescan();
    refreshAll();
    updateUndoButtons();
    startTimerHz (10);
    modViz.setActive (true);
}

ContentComponent::~ContentComponent()
{
    stopTimer();
    modViz.setActive (false);
    fxTabs.getTabbedButtonBar().removeChangeListener (this);
    processor.getUndoBroadcaster().removeChangeListener (this);
    processor.getPresetManager().removeChangeListener (this);
    tabDragStep.reset();
    if (animator != nullptr)
        animator->cancelAllAnimations (false);
}

//==============================================================================
void ContentComponent::updateActive()
{
    const bool nowActive = isLiveShowing (*this);
    if (nowActive == active)
        return;
    active = nowActive;
    modViz.setActive (active);
    if (active)
    {
        startTimerHz (10);
        refreshAll();   // state may have changed while hidden
    }
    else
        stopTimer();
}

void ContentComponent::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (source == &fxTabs.getTabbedButtonBar())
    {
        tabChanged();
        return;
    }
    if (source == &processor.getUndoBroadcaster())
        updateUndoButtons();
    refreshAll();
}

void ContentComponent::timerCallback()
{
    // Everything the processor can change without a broadcast: host state restore,
    // randomize, locks, WILD, the accent changed by another editor instance.
    syncTabOrder();

    const auto mask = processor.getFxLockMask();
    if (mask != lastLockMask)
    {
        lastLockMask = mask;
        fxTabs.repaintTabs();
    }

    if (! wildSlider.isMouseButtonDown())
    {
        const double w = processor.getRandomWildness();
        if (std::abs (wildSlider.getValue() - w) > 1.0e-6)
            wildSlider.setValue (w, juce::dontSendNotification);
    }

    if (currentTheme().accent != lastAccent)
    {
        lastAccent = currentTheme().accent;
        applyTheme();
    }

    const auto edited = processor.getPresetManager().isEditedCached() ? 1 : 0;
    if (edited != lastShownEdited)
        refreshAll();
    updateUndoButtons();
}

void ContentComponent::syncTabOrder()
{
    const auto want = processor.getFxOrder();
    if (fxTabs.currentOrder() != want)
    {
        fxTabs.applyOrder (want);
        fxTabs.repaintTabs();
    }
}

void ContentComponent::updateUndoButtons()
{
    const auto canUndo = processor.canUndo();
    const auto canRedo = processor.canRedo();
    if (undoButton.isEnabled() != canUndo) undoButton.setEnabled (canUndo);
    if (redoButton.isEnabled() != canRedo) redoButton.setEnabled (canRedo);
    undoButton.setTooltip (canUndo ? "Undo " + processor.getUndoLabel() + " (Cmd/Ctrl+Z)" : juce::String ("Nothing to undo"));
    redoButton.setTooltip (canRedo ? "Redo " + processor.getRedoLabel() + " (Shift+Cmd/Ctrl+Z)" : juce::String ("Nothing to redo"));
}

void ContentComponent::refreshAll()
{
    syncTabOrder();
    const auto& t = currentTheme();

    wildSlider.setValue (processor.getRandomWildness(), juce::dontSendNotification);
    wildLabel.setColour (juce::Label::textColourId, t.textSecondary);
    randomizeButton.setColour (juce::TextButton::buttonColourId, t.accent);
    randomizeButton.setColour (juce::TextButton::textColourOffId, t.display);

    auto& pm = processor.getPresetManager();
    const bool edited = pm.isEditedCached();
    lastShownEdited = edited ? 1 : 0;
    const auto font = getLookAndFeel().getTextButtonFont (presetNameButton, presetNameButton.getHeight());
    const auto text = truncateKeepingSuffix (pm.getCurrentName(), edited ? juce::String (" *") : juce::String(),
                                             font, presetNameButton.getWidth() - 12);
    if (text != presetNameButton.getButtonText())
        presetNameButton.setButtonText (text);
    presetNameButton.setTooltip (edited ? "Browse presets (edited since it was loaded or saved)" : "Browse presets");

    modPanel.refreshTargets();
    lastLockMask = processor.getFxLockMask();
    fxTabs.repaintTabs();
    updateUndoButtons();
    repaint();
}

void ContentComponent::applyTheme()
{
    if (onThemeChanged)
        onThemeChanged();
    refreshAll();
    if (presetBrowser != nullptr)
        presetBrowser->refresh();
    repaint();
}

//==============================================================================
bool ContentComponent::isTabLocked (const juce::String& name) const
{
    const int id = fxTabs.getModuleNames().indexOf (name);
    return id >= 0 && processor.isLocked ((dsp::FXChain::Module) id);
}

void ContentComponent::setLockedFromTab (const juce::String& name)
{
    const int id = fxTabs.getModuleNames().indexOf (name);
    if (id < 0)
        return;
    const auto m = (dsp::FXChain::Module) id;
    processor.setLocked (m, ! processor.isLocked (m));
    lastLockMask = processor.getFxLockMask();
    fxTabs.repaintTabs();
}

void ContentComponent::selectTabForModule (int moduleId)
{
    if (juce::isPositiveAndBelow (moduleId, fxTabs.getModuleNames().size()))
    {
        const int idx = fxTabs.getTabNames().indexOf (fxTabs.getModuleNames()[moduleId]);
        if (idx >= 0)
            fxTabs.setCurrentTabIndex (idx);
    }
}

int ContentComponent::getSelectedModule() const
{
    const int idx = fxTabs.getCurrentTabIndex();
    const auto names = fxTabs.getTabNames();
    return juce::isPositiveAndBelow (idx, names.size()) ? fxTabs.getModuleNames().indexOf (names[idx]) : -1;
}

void ContentComponent::tabChanged()
{
    // Remembered by effect NAME (a drag reorder never changes which effect is remembered).
    const int module = getSelectedModule();
    if (module >= 0)
        processor.getAPVTS().state.setProperty (kStateFxTab, fxTabs.getModuleNames()[module], nullptr);
    // A TabbedComponent only parents the CURRENT tab's panel, so the knobs the mod
    // visualisation drives are a different set after every switch.
    modViz.rescan();
    modViz.pollNow();
}

//==============================================================================
void ContentComponent::showModAssignMenu (juce::Slider& slider, const juce::String& paramID)
{
    juce::ignoreUnused (slider);
    if (mod::indexOf (paramID) < 0)
        return;   // not a modulation target: no menu

    juce::PopupMenu menu;
    menu.addSectionHeader ("MODULATE  " + modmenu::shortName (paramID).toUpperCase());
    for (int s = 0; s < SPAStripProcessor::numModSlots; ++s)
    {
        const auto current = processor.getModSlotTarget (s);
        if (current == paramID)
            menu.addItem (100 + s, "Remove from slot " + juce::String (s + 1));
        else
        {
            juce::String text = "Assign to mod slot " + juce::String (s + 1);
            if (current.isNotEmpty())
                text += "  (replaces " + modmenu::shortName (current) + ")";
            menu.addItem (1 + s, text);
        }
    }
    menu.showMenuAsync (juce::PopupMenu::Options().withMousePosition(),
                        [safe = juce::Component::SafePointer<ContentComponent> (this), paramID] (int result)
                        {
                            if (safe == nullptr || result <= 0)
                                return;
                            safe->toggleModAssignment (paramID, result >= 100 ? result - 100 : result - 1);
                        });
}

void ContentComponent::toggleModAssignment (const juce::String& paramID, int slot)
{
    if (! juce::isPositiveAndBelow (slot, SPAStripProcessor::numModSlots) || mod::indexOf (paramID) < 0)
        return;
    SPAStripProcessor::UndoStep step (processor, "MOD ASSIGN");
    if (processor.getModSlotTarget (slot) == paramID)
        processor.setModSlotTarget (slot, {});
    else
    {
        processor.setModSlotTarget (slot, paramID);
        // A slot at depth 0 does nothing: give a fresh assignment a visible default.
        if (auto* depth = processor.getAPVTS().getParameter (params::id::modSlotDepth (slot)))
            if (std::abs (processor.getAPVTS().getRawParameterValue (params::id::modSlotDepth (slot))->load()) < 0.001f)
            {
                depth->beginChangeGesture();
                depth->setValueNotifyingHost (depth->convertTo0to1 (0.5f));
                depth->endChangeGesture();
            }
    }
    modPanel.refreshTargets();
    modViz.pollNow();
}

//==============================================================================
void ContentComponent::showLogoMenu()
{
    juce::PopupMenu m;
    m.addItem (1, "About SPAStrip...");
    m.addItem (2, "Accent colour...");
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&logoButton),
                     [safe = juce::Component::SafePointer<ContentComponent> (this)] (int r)
                     {
                         if (safe == nullptr)
                             return;
                         if (r == 1) safe->showAboutPanel();
                         else if (r == 2) safe->showAccentPicker();
                     });
}

// SAVE: a loaded user preset whose file still exists offers "Save" (rewrite it in
// place, keeping its name, folder and type) next to "Save As..."; anything else (Init,
// a factory preset, a deleted file) goes straight to Save As -- as in SPASynth.
void ContentComponent::onSaveClicked()
{
    auto& pm = processor.getPresetManager();
    const auto file = pm.getCurrentFile();
    if (! file.existsAsFile())
    {
        showSaveDialog (true);
        return;
    }
    juce::PopupMenu m;
    m.addItem (1, "Save \"" + pm.getCurrentName() + "\"");
    m.addItem (2, "Save As...");
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&savePresetButton),
                     [safe = juce::Component::SafePointer<ContentComponent> (this)] (int r)
                     {
                         if (safe == nullptr || r <= 0)
                             return;
                         if (r == 2)
                             safe->showSaveDialog (true);
                         else
                             safe->saveCurrentInPlace();
                     });
}

void ContentComponent::saveCurrentInPlace()
{
    auto& pm = processor.getPresetManager();
    const auto res = pm.saveInPlace (pm.getCurrentFile());
    if (! res.ok)
        showMessage ("Save failed", res.error);
    refreshAll();
}

// SAVE AS / first save of an unsaved patch. Default folder = the loaded user preset's
// folder (else the User root), never Auto unless Auto was used for the previous save.
// Default NAME: "<Type> <n>" until the user types one; a patch that came from a preset
// starts from that preset's name ("Warm Pad", or "Warm Pad 2" if taken) and that name
// is the user's, so changing TYPE leaves it alone.
void ContentComponent::showSaveDialog (bool)
{
    auto& pm = processor.getPresetManager();
    SaveDialog::Init init;
    init.userFolders = pm.getUserFolders();
    init.startAuto = saveFolderAutoSticky;

    juce::String presetName;
    {
        const auto idx = pm.getCurrentIndex();
        const auto& list = pm.getPresets();
        if (idx >= 0 && idx < (int) list.size())
        {
            const auto& p = list[(size_t) idx];
            if (! p.isFactory)
                init.defaultFolder = p.folder;
            presetName = p.name;
            init.initialType = p.type;
        }
        else if (pm.getCurrentName() != "Init")
            presetName = pm.getCurrentName();
    }

    // The folder a name is checked against: Auto only applies once a type exists.
    const auto initialFolder = init.startAuto ? init.initialType : init.defaultFolder;
    if (presetName.isNotEmpty())
    {
        init.nameIsOwned = true;
        init.initialName = pm.suggestName (presetName, initialFolder, false);
    }
    else
        init.initialName = pm.suggestName (init.initialType.isEmpty() ? juce::String ("Preset") : init.initialType,
                                           initialFolder, true);

    juce::Component::SafePointer<ContentComponent> safe (this);
    auto d = std::make_unique<SaveDialog> (
        init,
        [safe] (const SaveRequest& req)
        {
            preset::PresetManager::SaveResult res;
            if (safe == nullptr)
                return res;
            res = safe->processor.getPresetManager().save (req.name, req.folder, req.replace, req.type, req.createFolder);
            if (res.ok)
                safe->saveFolderAutoSticky = req.autoFolder;
            return res;
        },
        [safe] (const juce::String& type, const juce::String& folder)
        {
            return safe == nullptr ? type
                                   : safe->processor.getPresetManager().suggestName (type.isEmpty() ? juce::String ("Preset") : type,
                                                                                    folder, true);
        },
        [safe] (const juce::String& name, const juce::String& folder)
        { return safe != nullptr && safe->processor.getPresetManager().userPresetExists (name, folder); },
        [safe] { if (safe != nullptr) safe->refreshAll(); });
    auto* raw = d.get();
    showDialog (std::move (d));
    raw->getNameEditor().grabKeyboardFocus();
    raw->getNameEditor().selectAll();
}

void ContentComponent::promptText (const juce::String& title, const juce::String& prompt, const juce::String& initial,
                                   const juce::String& okLabel, std::function<void (const juce::String&)> onOk)
{
    juce::Component::SafePointer<ContentComponent> safe (this);
    showDialog (std::make_unique<TextPromptDialog> (title, prompt, initial, okLabel,
        [safe, answer = std::move (onOk)] (const juce::String& text)
        {
            if (safe == nullptr)
                return;
            // A turn later: the answer may open another dialog, which would destroy this
            // one while its own button handler is still running.
            if (safe->dialog != nullptr && safe->dialog->onDismiss)
                safe->dialog->onDismiss();
            juce::MessageManager::callAsync ([answer, text] { answer (text); });
        }));
    if (auto* d = dynamic_cast<TextPromptDialog*> (dialog.get()))
        d->getEditor().grabKeyboardFocus();
}

void ContentComponent::showConfirm (const juce::String& title, const juce::String& body, const juce::String& okLabel,
                                    std::function<void()> onOk)
{
    showDialog (std::make_unique<ConfirmDialog> (title, body, okLabel, std::move (onOk)));
}

void ContentComponent::showClashDialog (const juce::String& presetName,
                                        std::function<void (preset::PresetManager::ImportClash, bool)> decide)
{
    showDialog (std::make_unique<ClashDialog> (presetName, std::move (decide)));
}

void ContentComponent::showRenameDialog (const juce::File& file)
{
    promptText ("Rename preset", "NAME", file.getFileNameWithoutExtension(), "Rename",
                [safe = juce::Component::SafePointer<ContentComponent> (this), file] (const juce::String& newName)
                {
                    if (safe == nullptr)
                        return;
                    const auto r = safe->processor.getPresetManager().rename (file, newName);
                    if (! r.ok)
                        safe->showMessage ("Rename failed", r.error);
                    safe->refreshAll();
                });
}

void ContentComponent::showAboutPanel() { showDialog (std::make_unique<AboutDialog> (processor)); }

void ContentComponent::showAccentPicker()
{
    showDialog (std::make_unique<AccentDialog> ([this] { applyTheme(); }));
}

void ContentComponent::showMessage (const juce::String& title, const juce::String& body)
{
    showDialog (std::make_unique<MessageDialog> (title, body, juce::Point<int> (480, 256)));
}

void ContentComponent::showDialog (std::unique_ptr<DialogOverlay> d)
{
    dismissDialog();
    dialog = std::move (d);
    dialog->onDismiss = [safe = juce::Component::SafePointer<ContentComponent> (this), raw = dialog.get()]
    {
        if (safe == nullptr)
            return;
        raw->setVisible (false);   // gone at once; destroyed after the callback that asked unwinds
        juce::MessageManager::callAsync ([safe, raw]
                                         { if (safe != nullptr && safe->dialog.get() == raw) safe->dismissDialog(); });
    };
    addAndMakeVisible (*dialog);
    dialog->setBounds (getLocalBounds());
    dialog->toFront (true);
    dialog->grabKeyboardFocus();
}

void ContentComponent::dismissDialog()
{
    if (dialog != nullptr)
    {
        removeChildComponent (dialog.get());
        dialog.reset();
        grabKeyboardFocus();
    }
}

//==============================================================================
void ContentComponent::setPresetBrowserOpen (bool open, bool animate)
{
    presetBrowserOpen = open;
    processor.getAPVTS().state.setProperty (kStateDrawer, open, nullptr);
    layoutDrawer();
    const auto openBounds = presetBrowser->getOpenBounds();
    const auto closedBounds = openBounds.translated (-openBounds.getWidth() - 12, 0);
    if (open)
    {
        processor.getPresetManager().rescan();   // files may have changed on disk since it was last open
        presetBrowser->scrollToCurrent();
        presetBrowser->setVisible (true);
        presetBrowser->toFront (false);
        if (dialog != nullptr)
            dialog->toFront (false);
    }
    if (! animate || ! isShowing())
    {
        if (animator != nullptr)
            animator->cancelAllAnimations (false);
        presetBrowser->setBounds (open ? openBounds : closedBounds);
        presetBrowser->setVisible (open);
        if (open)
            presetBrowser->grabKeyboardFocus();
        else
            grabKeyboardFocus();
        return;
    }
    if (animator == nullptr)
        animator = std::make_unique<juce::ComponentAnimator>();
    if (open)
        presetBrowser->setBounds (closedBounds);
    animator->animateComponent (presetBrowser.get(), open ? openBounds : closedBounds, 1.0f, 160, false, 1.0, 1.0);
    juce::Component::SafePointer<ContentComponent> safe (this);
    juce::Timer::callAfterDelay (190, [safe, open]
    {
        if (safe == nullptr || safe->presetBrowserOpen != open)
            return;
        safe->presetBrowser->setVisible (open);
        if (open)
            safe->presetBrowser->grabKeyboardFocus();
        else
            safe->grabKeyboardFocus();
    });
}

void ContentComponent::layoutDrawer()
{
    const auto area = getLocalBounds().withTrimmedTop (metrics::brandBandHeight + metrics::headerHeight)
                                      .withTrimmedBottom (metrics::footerHeight)
                                      .removeFromLeft (metrics::presetBrowserWidth);
    presetBrowser->setOpenBounds (area);
}

//==============================================================================
bool ContentComponent::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey && presetBrowserOpen)
    {
        setPresetBrowserOpen (false, true);
        return true;
    }
    const auto mods = key.getModifiers();
    const int code = key.getKeyCode();
    if (mods.isCommandDown() && ! mods.isAltDown() && (code == 'Z' || code == 'z'))
    {
        if (mods.isShiftDown()) processor.redo(); else processor.undo();
        refreshAll();
        return true;
    }
    if (mods.isCommandDown() && ! mods.isAltDown() && ! mods.isShiftDown() && (code == 'Y' || code == 'y'))
    {
        processor.redo();   // Windows convention
        refreshAll();
        return true;
    }
    return false;
}

juce::StringArray ContentComponent::collectControlledParameterIDs() const
{
    juce::StringArray ids;
    std::function<void (const juce::Component&)> walk = [&] (const juce::Component& c)
    {
        if (c.getProperties().contains ("paramID"))
            ids.addIfNotAlreadyThere (c.getProperties()["paramID"].toString());
        for (auto* child : c.getChildren())
            walk (*child);
    };
    walk (*this);
    // Hidden tabs' panels have no parent (TabbedComponent parents only the current one).
    for (int i = 0; i < fxTabs.getNumTabs(); ++i)
        if (auto* panel = fxTabs.getTabContentComponent (i))
            walk (*panel);
    return ids;
}

//==============================================================================
void ContentComponent::paint (juce::Graphics& g)
{
    const auto& t = currentTheme();
    g.fillAll (t.background);

    // Faceplate grain: a tiled low-opacity noise texture over the whole surface.
    if (noiseTexture.isValid())
    {
        g.setTiledImageFill (noiseTexture, 0, 0, 0.03f);
        g.fillRect (getLocalBounds());
    }

    auto area = getLocalBounds();

    // Brand band: the big tracked wordmark, centred.
    auto band = area.removeFromTop (metrics::brandBandHeight);
    g.setColour (t.header.darker (0.25f));
    g.fillRect (band);
    draw::trackedCentredText (g, metrics::wordmarkFont(), "SPASTRIP", band.withTrimmedBottom (9), juce::Colour (0xffe7ecef));
    draw::trackedCentredText (g, metrics::brandSubFont(), "SILVERPLATTER AUDIO", band.withTrimmedTop (21), juce::Colour (0xff7f8d97));

    // Header strip.
    auto header = area.removeFromTop (metrics::headerHeight);
    g.setColour (t.header);
    g.fillRect (header);

    g.setColour (t.textSecondary);
    g.setFont (metrics::smallFont());
    g.drawText ("OVERSAMPLING", osCaptionRect, juce::Justification::centred);

    // Footer strip.
    const auto footer = juce::Rectangle<int> (0, getHeight() - metrics::footerHeight, getWidth(), metrics::footerHeight);
    g.setColour (t.header);
    g.fillRect (footer);
    g.setColour (t.textSecondary);
    g.setFont (metrics::smallFont());
    g.drawText ("v" SPASTRIP_VERSION, footer.reduced (12, 0), juce::Justification::centredLeft);
    // Leave room for the editor's resize-corner grip (fixed ~20 editor px, so it
    // covers 20 / scale content px).
    const int gripRoom = juce::roundToInt (22.0f / juce::jmax (0.25f, getTransform().mat00));
    g.drawText ("Silverplatter Audio", footer.withTrimmedLeft (12).withTrimmedRight (juce::jmax (12, gripRoom)),
                juce::Justification::centredRight);
    g.setColour (juce::Colour (0xffe7ecef).withAlpha (0.8f));
    g.setFont (metrics::labelFont());
    g.drawText ("SPAStrip", footer, juce::Justification::centred);

    // Hint in the empty right end of the tab strip.
    if (! tabHintRect.isEmpty())
    {
        g.setColour (t.textSecondary.withAlpha (0.55f));
        g.setFont (metrics::smallFont());
        const juce::StringArray options { "DRAG TABS TO REORDER   -   PADLOCK KEEPS AN EFFECT THROUGH RANDOMIZE ALL",
                                          "DRAG TO REORDER   -   PADLOCK = KEEP ON RANDOMIZE",
                                          "DRAG TO REORDER   -   PADLOCK = KEEP", "DRAG TO REORDER" };
        for (const auto& o : options)
            if (o == options[options.size() - 1]
                || juce::GlyphArrangement::getStringWidth (metrics::smallFont(), o) <= (float) tabHintRect.getWidth())
            {
                g.drawText (o, tabHintRect, juce::Justification::centredRight, true);
                break;
            }
    }

    // Recessed seams + soft shadow bands (the faceplate "overhang" language).
    constexpr float edgeLineAlpha = 0.44f;
    constexpr float shadowSoftLength = 27.0f;
    const auto seam = [&] (int y)
    {
        if (y <= 0)
            return;
        const float x = 0.0f, w = (float) getWidth();
        g.setColour (juce::Colours::black.withAlpha (edgeLineAlpha));
        g.fillRect (juce::Rectangle<float> (x, (float) y, w, 1.0f));
        g.setGradientFill (draw::easedShadowGradient ({ x, (float) y + 1.0f }, { x, (float) y + 1.0f + shadowSoftLength },
                                                      draw::shadowStartAlpha));
        g.fillRect (juce::Rectangle<float> (x, (float) y + 1.0f, w, shadowSoftLength));
    };
    seam (seamHeaderY);
    seam (seamBottomY);
    seam (seamFooterY);

    g.setColour (t.seam);
    for (int x : gutterXs)
        g.drawVerticalLine (x, (float) seamBottomY, (float) seamFooterY);
}

void ContentComponent::resized()
{
    auto bounds = getLocalBounds();
    bounds.removeFromTop (metrics::brandBandHeight);
    auto header = bounds.removeFromTop (metrics::headerHeight);
    auto footer = bounds.removeFromBottom (metrics::footerHeight);

    // --- Header -------------------------------------------------------------
    logoButton.setBounds (header.removeFromLeft (52));

    auto right = header.removeFromRight (400).reduced (0, 9);
    right.removeFromRight (12);
    accentButton.setBounds (right.removeFromRight (32).reduced (2, 6));
    right.removeFromRight (6);
    auto os = right.removeFromRight (74);
    osCaptionRect = os.removeFromTop (12).translated (0, -1);
    oversampling.setBounds (os.reduced (0, 0).withHeight (22).withY (os.getY() + 1));
    right.removeFromRight (14);
    randomizeButton.setBounds (right.removeFromRight (150).reduced (0, 3));
    right.removeFromRight (6);
    auto wild = right.removeFromRight (44);
    wildLabel.setBounds (wild.removeFromBottom (11));
    wildSlider.setBounds (wild);

    auto presetArea = header.reduced (metrics::unit, 12);
    prevPresetButton.setBounds (presetArea.removeFromLeft (26));
    redoButton.setBounds (presetArea.removeFromRight (24));
    presetArea.removeFromRight (2);
    undoButton.setBounds (presetArea.removeFromRight (24));
    presetArea.removeFromRight (8);
    initButton.setBounds (presetArea.removeFromRight (46));
    presetArea.removeFromRight (4);
    savePresetButton.setBounds (presetArea.removeFromRight (52));
    presetArea.removeFromRight (8);
    nextPresetButton.setBounds (presetArea.removeFromRight (26));
    presetNameButton.setBounds (presetArea.reduced (3, 0));

    // --- Main area ----------------------------------------------------------
    seamHeaderY = bounds.getY();
    seamFooterY = footer.getY();
    auto main = bounds.reduced (metrics::unit, 4);
    constexpr int gap = 6;
    constexpr int bottomHeight = 214;
    auto bottom = main.removeFromBottom (bottomHeight);
    main.removeFromBottom (gap);
    seamBottomY = bottom.getY() - 3;
    fxTabs.setBounds (main);

    gutterXs.clear();
    constexpr int sidechainW = 316, ioW = 232;
    sidechainPanel.setBounds (bottom.removeFromLeft (sidechainW));
    gutterXs.push_back (bottom.getX() + gap / 2);
    bottom.removeFromLeft (gap);
    ioPanel.setBounds (bottom.removeFromRight (ioW));
    gutterXs.push_back (bottom.getRight() + gap / 2);
    bottom.removeFromRight (gap);
    modPanel.setBounds (bottom);

    // Tab-strip hint: right of the last tab.
    {
        auto& bar = fxTabs.getTabbedButtonBar();
        int lastRight = 0;
        for (int i = 0; i < bar.getNumTabs(); ++i)
            if (auto* b = bar.getTabButton (i))
                lastRight = juce::jmax (lastRight, b->getRight());
        tabHintRect = juce::Rectangle<int> (fxTabs.getX() + lastRight + 14, fxTabs.getY(),
                                            juce::jmax (0, fxTabs.getWidth() - lastRight - 24), metrics::sectionHeaderHeight);
    }

    layoutDrawer();
    presetBrowser->setBounds (presetBrowserOpen ? presetBrowser->getOpenBounds()
                                                : presetBrowser->getOpenBounds().translated (-metrics::presetBrowserWidth - 12, 0));
    if (dialog != nullptr)
        dialog->setBounds (getLocalBounds());
}

} // namespace spa::ui

//==============================================================================
namespace spa
{

namespace
{
    const juce::Displays::Display* displayForFit (juce::Component& editor)
    {
        auto& displays = juce::Desktop::getInstance().getDisplays();
        if (auto* peer = editor.getPeer())
            if (auto* d = displays.getDisplayForRect (peer->getBounds()))
                return d;
        if (auto* d = displays.getDisplayForPoint (juce::Desktop::getMousePosition().toFloat()))
            return d;
        return displays.getPrimaryDisplay();
    }
}

float SPAStripEditor::scaleThatFits (juce::Rectangle<int> userArea, int baseW, int baseH)
{
    constexpr double hostChromeHeight = 140.0, hostChromeWidth = 40.0;   // DAW window chrome allowance
    const auto availableH = (double) userArea.getHeight() - hostChromeHeight;
    const auto availableW = (double) userArea.getWidth() - hostChromeWidth;
    auto scale = juce::jmin (1.0, availableH / (double) baseH, availableW / (double) baseW);
    scale = std::floor (juce::jmax (scale, 0.0) * 20.0) / 20.0;   // quantise down to 0.05
    return (float) juce::jmax ((double) minScale, scale);
}

SPAStripEditor::SPAStripEditor (SPAStripProcessor& p)
    : juce::AudioProcessorEditor (p), stripProcessor (p)
{
    setLookAndFeel (&lookAndFeel);
    setOpaque (true);

    content = std::make_unique<ui::ContentComponent> (p);
    content->onThemeChanged = [this]
    {
        lookAndFeel.refreshPalette();
        sendLookAndFeelChange();
        repaint();
    };
    addAndMakeVisible (*content);

    const int baseW = ui::metrics::baseWidth, baseH = ui::metrics::baseHeight;
    // The content's one real layout pass happens here, AFTER it is parented, so every
    // descendant resolves this editor's LookAndFeel (tab widths depend on it).
    content->setSize (baseW, baseH);

    setResizable (true, true);
    if (auto* c = getConstrainer())
    {
        c->setFixedAspectRatio ((double) baseW / baseH);
        c->setSizeLimits (juce::roundToInt (baseW * minScale), juce::roundToInt (baseH * minScale),
                          juce::roundToInt (baseW * maxScale), juce::roundToInt (baseH * maxScale));
    }

    // Restore the drawer.
    if ((bool) p.getAPVTS().state.getProperty ("uiPresetDrawerOpen", false))
        content->setPresetBrowserOpen (true, false);

    // Window size: a remembered scale that still fits this display is used as is; otherwise
    // (first open, or a smaller display) the largest scale that fits.
    const auto* display = displayForFit (*this);
    const auto userArea = display != nullptr ? display->userBounds.getSmallestIntegerContainer()
                                             : juce::Rectangle<int> (0, 0, 1920, 1080);
    const auto rawMaxScale = juce::jmin (1.0, (double) (userArea.getHeight() - 140) / baseH,
                                         (double) (userArea.getWidth() - 40) / baseW);
    const auto& state = p.getAPVTS().state;
    const auto hasRemembered = state.hasProperty ("uiScale");
    const auto saved = (double) state.getProperty ("uiScale", 1.0);
    const auto scale = (hasRemembered && (saved <= rawMaxScale + 0.001 || saved > 1.0))
                           ? juce::jlimit ((double) minScale, (double) maxScale, saved)
                           : (double) scaleThatFits (userArea, baseW, baseH);

    suppressScaleSave = true;   // an automatic fit is never mistaken for the user's chosen size
    setSize (juce::roundToInt (baseW * scale), juce::roundToInt (baseH * scale));
    suppressScaleSave = false;
}

SPAStripEditor::~SPAStripEditor()
{
    setLookAndFeel (nullptr);
}

float SPAStripEditor::getScale() const
{
    return (float) getWidth() / (float) ui::metrics::baseWidth;
}

void SPAStripEditor::paint (juce::Graphics& g)
{
    g.fillAll (ui::currentTheme().background);
}

void SPAStripEditor::resized()
{
    if (content == nullptr)
        return;
    ++resizedCalls;
    const auto scale = (float) getWidth() / (float) ui::metrics::baseWidth;
    content->setTransform (juce::AffineTransform::scale (scale));
    content->setTopLeftPosition (0, 0);
    if (! suppressScaleSave)
        stripProcessor.getAPVTS().state.setProperty ("uiScale", (double) scale, nullptr);
}

void SPAStripEditor::parentHierarchyChanged()
{
    // One-shot re-check once there is a real peer: the host may have created the editor
    // before it was on screen, so the constructor's fit could have used the wrong display.
    if (! screenFitCheckDone && getPeer() != nullptr && isShowing())
    {
        screenFitCheckDone = true;
        if (const auto* display = displayForFit (*this))
        {
            const auto userArea = display->userBounds.getSmallestIntegerContainer();
            if (getWidth() > userArea.getWidth() - 40 || getHeight() > userArea.getHeight() - 140)
            {
                const auto scale = (double) scaleThatFits (userArea, ui::metrics::baseWidth, ui::metrics::baseHeight);
                suppressScaleSave = true;
                setSize (juce::roundToInt (ui::metrics::baseWidth * scale), juce::roundToInt (ui::metrics::baseHeight * scale));
                suppressScaleSave = false;
            }
        }
    }

   #if JUCE_MAC
    // macOS Tahoe's AUHostingService (Logic / GarageBand) can open an editor with a stale
    // hit-test region until the host renegotiates the view (a known Apple bug, hits non-JUCE
    // plugins too): nudging the size by one pixel and back, once, once the editor is on
    // screen, makes it rebuild its geometry. A resize is automation-safe.
    if (hostViewWakeupDone || stripProcessor.wrapperType != juce::AudioProcessor::wrapperType_AudioUnit || ! isShowing())
        return;
    hostViewWakeupDone = true;
    juce::Component::SafePointer<SPAStripEditor> safe (this);
    juce::Timer::callAfterDelay (150, [safe]
    {
        if (safe == nullptr)
            return;
        const auto w = safe->getWidth(), h = safe->getHeight();
        safe->suppressScaleSave = true;
        safe->setSize (w + 1, h);
        juce::Timer::callAfterDelay (60, [safe, w, h]
        {
            if (safe == nullptr)
                return;
            safe->setSize (w, h);
            safe->suppressScaleSave = false;
            safe->repaint();
        });
    });
   #endif
}

} // namespace spa
