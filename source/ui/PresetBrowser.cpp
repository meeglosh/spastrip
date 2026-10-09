#include "PresetBrowser.h"

namespace spa::ui
{

namespace
{
    spa::presets::Palette stripPalette()
    {
        const auto& t = currentTheme();
        spa::presets::Palette p;
        p.panel = t.panel;
        p.display = t.display;
        p.outline = t.outline;
        p.textPrimary = t.textPrimary;
        p.textSecondary = t.textSecondary;
        p.accent = t.accent;
        p.smallFont = metrics::smallFont();
        p.sectionFont = metrics::sectionFont();
        p.drawListWell = [] (juce::Graphics& g, juce::Rectangle<float> r) { draw::displayWell (g, r, false); };
        return p;
    }
}

PresetBrowser::PresetBrowser (SPAStripProcessor& processor, Hooks hooks)
    : Base (processor.getPresetManager(), [] { return stripPalette(); }, std::move (hooks))
{
}

} // namespace spa::ui
