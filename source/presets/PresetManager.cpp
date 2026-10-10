#include "PresetManager.h"

#include "../ui/UiSettings.h"
#include "FactoryBank.h"

namespace spa::preset
{

namespace
{
    juce::File& rootOverride()
    {
        static juce::File override;
        return override;
    }
}

const juce::StringArray& PresetManager::presetTypes()
{
    static const juce::StringArray types { "Drums", "Bass", "Vocals", "Guitar", "Keys",
                                           "Synth", "FX", "Mixbus", "Mastering", "Creative",
                                           // Appended (1.0.6): the order is the browser's group order and
                                           // stored types of existing user presets must keep resolving.
                                           "Ambient", "Lo-Fi" };
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

spa::presets::Adapter PresetManager::makeAdapter (SPAStripProcessor& p)
{
    namespace settings = spa::ui::settings;
    spa::presets::Adapter a;

    a.productName = "SPAStrip";
    a.fileExtension = presetExtension;
    a.rootTag = presetTag;
    a.formatVersion = presetFormatVersion;
    a.maxPresetFileBytes = maxPresetFileBytes;
    a.maxFolderDepth = maxFolderDepth;
    a.types = presetTypes();
    a.otherTypeLabel = otherTypeLabel;
    a.defaultPresetsRoot = [] { return defaultPresetsRoot(); };
    a.initTooltip = "Reset every parameter, the chain order, the mod slots and the impulse to their defaults";

    a.captureState = [&p] { return p.capturePresetState(); };
    a.applyState = [&p] (const juce::ValueTree& state, const juce::String& label) { return p.applyPresetState (state, label); };
    a.applyInit = [&p]
    {
        // An empty state: restoreStateTree fills every parameter missing from it with
        // its registry default, and the absent chain order / IR / slot properties fall
        // back to the default order, no IR and unassigned slots.
        juce::ValueTree empty (p.getAPVTS().state.getType());
        p.applyPresetState (empty, "INIT");
    };

    int oversamplingParamIndex = -1;
    for (size_t i = 0; i < params::all().size(); ++i)
        if (params::all()[i].id == params::id::oversampling)
            oversamplingParamIndex = (int) i;
    a.captureBaseline = [&p]
    {
        return std::shared_ptr<const void> (std::make_shared<const PatchSnapshot> (p.capturePatchSnapshot()));
    };
    a.differsFromBaseline = [&p, oversamplingParamIndex] (const void* baseline)
    {
        return p.capturePatchSnapshot()
                   .firstDifference (*static_cast<const PatchSnapshot*> (baseline), oversamplingParamIndex)
                   .isNotEmpty();
    };

    a.getFavourites = [] { return settings::getFavoritePresets(); };
    a.setFavourite = [] (const juce::String& key, bool on) { settings::setPresetFavorite (key, on); };
    a.getCollapsedGroups = [] { return settings::getCollapsedPresetGroups(); };
    a.setGroupCollapsed = [] (const juce::String& key, bool on) { settings::setPresetGroupCollapsed (key, on); };
    a.getGroupMode = [] { return settings::getPresetGroupMode(); };
    a.setGroupMode = [] (int mode) { settings::setPresetGroupMode (mode); };
    return a;
}

PresetManager::PresetManager (SPAStripProcessor& p) : spa::presets::PresetManager (makeAdapter (p)) {}

namespace
{
    bool& factoryBankEnabledFlag()
    {
        static bool enabled = true;
        return enabled;
    }
}

void PresetManager::setFactoryBankEnabled (bool enabled)
{
    factoryBankEnabledFlag() = enabled;
}

void PresetManager::installFactoryBank()
{
    setFactoryPresets (factoryBankEnabledFlag() ? buildFactoryBank() : std::vector<FactoryPreset>());   // rescans
}

PresetContext PresetManager::captureContext() const
{
    const auto id = captureIdentity();
    PresetContext c;
    c.file = id.file;
    c.name = id.name;
    c.bank = id.bank;
    c.isFactory = id.isFactory;
    c.baseline = std::static_pointer_cast<const PatchSnapshot> (id.baseline);
    return c;
}

void PresetManager::restoreContext (const PresetContext& c)
{
    Identity id;
    id.file = c.file;
    id.name = c.name;
    id.bank = c.bank;
    id.isFactory = c.isFactory;
    id.baseline = c.baseline;
    restoreIdentity (id);
}

} // namespace spa::preset
