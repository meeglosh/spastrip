#include "ConvolvePanel.h"

#include "UiSettings.h"

namespace spa::ui
{

//==============================================================================
ConvolveDisplay::ConvolveDisplay (SPAStripProcessor& p) : processor (p)
{
    setInterceptsMouseClicks (false, false);
    startTimerHz (20);
}

ConvolveDisplay::~ConvolveDisplay() { stopTimer(); }

void ConvolveDisplay::timerCallback()
{
    if (! isLiveShowing (*this))
        return;
    const bool has = processor.hasConvolutionIR();
    const auto& env = processor.getConvolutionEnvelope();
    const float pre = processor.getAPVTS().getRawParameterValue (params::id::fx::convPreDelay)->load();
    const float trim = processor.getConvolutionStartTrim();
    if (has != shownHas || ! juce::exactlyEqual (pre, shownPre) || ! juce::exactlyEqual (trim, shownTrim) || env != shown)
    {
        shownHas = has;
        shownPre = pre;
        shownTrim = trim;
        shown = env;
        repaint();
    }
}

void ConvolveDisplay::paint (juce::Graphics& g)
{
    const auto& t = currentTheme();
    auto area = getLocalBounds().toFloat().reduced (1.0f);
    g.setColour (t.outline.withAlpha (0.18f));
    g.drawHorizontalLine ((int) area.getCentreY(), area.getX(), area.getRight());

    if (! processor.hasConvolutionIR())
    {
        g.setColour (t.textSecondary.withAlpha (0.6f));
        g.setFont (metrics::labelFont());
        g.drawText ("No impulse loaded - pick a factory IR or load a file", area, juce::Justification::centred);
        return;
    }

    const auto& env = shown;
    float peak = 1.0e-6f;
    for (auto v : env)
        peak = juce::jmax (peak, v);

    const float gapFrac = juce::jlimit (0.0f, 0.4f, shownPre / 400.0f);
    const float x0 = area.getX() + gapFrac * area.getWidth();
    const float w = area.getRight() - x0;
    const float cy = area.getCentreY();
    const float halfH = area.getHeight() * 0.45f;
    const int N = (int) env.size();

    juce::Path top;
    for (int i = 0; i < N; ++i)
    {
        const float a = env[(size_t) i] / peak * halfH;
        const float x = x0 + w * (float) i / (float) (N - 1);
        if (i == 0) top.startNewSubPath (x, cy - a); else top.lineTo (x, cy - a);
    }
    juce::Path fill = top;
    for (int i = N - 1; i >= 0; --i)
    {
        const float a = env[(size_t) i] / peak * halfH;
        fill.lineTo (x0 + w * (float) i / (float) (N - 1), cy + a);
    }
    fill.closeSubPath();
    g.setColour (t.accentMod.withAlpha (0.22f));
    g.fillPath (fill);
    g.setColour (t.accentMod);
    g.strokePath (top, juce::PathStrokeType (1.2f));

    if (gapFrac > 0.001f)   // pre-delay marker: a gap of silence before the wet signal
    {
        g.setColour (t.textSecondary.withAlpha (0.5f));
        g.drawVerticalLine ((int) x0, area.getY(), area.getBottom());
    }

    // START marker: how much was trimmed off the FRONT of the impulse itself.
    if (shownTrim > 0.001f)
    {
        const float wedgeW = juce::jmin (w * 0.3f, 10.0f + shownTrim * 30.0f);
        {
            juce::Graphics::ScopedSaveState save (g);
            g.reduceClipRegion (juce::Rectangle<float> (x0, area.getY(), wedgeW, area.getHeight())
                                    .getSmallestIntegerContainer());
            g.setColour (t.textSecondary.withAlpha (0.28f));
            for (float hx = x0 - area.getHeight(); hx < x0 + wedgeW; hx += 5.0f)
                g.drawLine (hx, area.getBottom(), hx + area.getHeight(), area.getY(), 1.0f);
        }
        g.setColour (t.textSecondary.withAlpha (0.8f));
        g.setFont (metrics::smallFont());
        g.drawText ("START -" + juce::String (juce::roundToInt (shownTrim * 100.0f)) + "%",
                    juce::Rectangle<float> (x0 + 2.0f, area.getY(), 90.0f, 11.0f),
                    juce::Justification::left);
    }

    // Time scale hint (length of the loaded impulse, after shaping the engine's own cap).
    g.setColour (t.textSecondary.withAlpha (0.6f));
    g.setFont (metrics::smallFont());
    g.drawText (juce::String (processor.getConvolutionLengthSeconds(), 2) + " s",
                area.removeFromBottom (11.0f), juce::Justification::right);
}

//==============================================================================
ConvolvePanel::ConvolvePanel (SPAStripProcessor& p)
    : processor (p), header (p.getAPVTS(), { { "Convolution", params::id::fx::convEnable } }),
      display (p),
      mix      (p.getAPVTS(), params::id::fx::convMix, "Mix"),
      predelay (p.getAPVTS(), params::id::fx::convPreDelay, "Pre"),
      start    (p.getAPVTS(), params::id::fx::convStart, "Start"),
      decay    (p.getAPVTS(), params::id::fx::convDecay, "Decay"),
      damping  (p.getAPVTS(), params::id::fx::convDamping, "Damp"),
      width    (p.getAPVTS(), params::id::fx::convWidth, "Width")
{
    factoryButton.setTooltip ("Pick one of the built-in impulse responses");
    factoryButton.onClick = [this] { showFactoryMenu(); };
    fileButton.setTooltip ("Load a WAV / AIFF / FLAC file as the impulse (or drop one on this panel)");
    fileButton.onClick = [this] { showFileChooser(); };
    clearButton.setTooltip ("Remove the impulse");
    clearButton.onClick = [this] { clearIR(); };
    for (auto* c : std::initializer_list<juce::Component*> { &header, &factoryButton, &fileButton, &clearButton,
                                                              &display, &mix, &predelay, &start, &decay, &damping, &width })
        addAndMakeVisible (*c);
    refreshLabels();
    startTimerHz (10);
}

ConvolvePanel::~ConvolvePanel() { stopTimer(); }

bool ConvolvePanel::isAcceptedAudioFile (const juce::String& path)
{
    const auto ext = juce::File (path).getFileExtension().toLowerCase();
    return ext == ".wav" || ext == ".aif" || ext == ".aiff" || ext == ".flac";
}

void ConvolvePanel::timerCallback()
{
    if (! isLiveShowing (*this))
        return;
    // The IR also changes without this panel (undo, preset load, host restore).
    if (processor.getConvolutionIRSource() != lastSource || processor.getConvolutionIRName() != irName)
        refreshLabels();
}

void ConvolvePanel::refreshLabels()
{
    lastSource = processor.getConvolutionIRSource();
    irName = processor.getConvolutionIRName();
    if (lastSource.startsWith (factory::sourcePrefix))
        sourceTag = "FACTORY";
    else if (lastSource == "embedded")
        sourceTag = "FILE";
    else
        sourceTag = {};
    if (sourceTag.isEmpty())
        irName = {};
    clearButton.setEnabled (sourceTag.isNotEmpty());
    repaint();
}

juce::PopupMenu ConvolvePanel::buildFactoryMenu() const
{
    juce::PopupMenu menu;
    const auto& list = SPAStripProcessor::getFactoryIRs();
    const auto current = processor.getConvolutionIRSource();
    juce::String lastCategory;
    for (size_t i = 0; i < list.size(); ++i)
    {
        if (list[i].category != lastCategory)
        {
            lastCategory = list[i].category;
            menu.addSectionHeader (lastCategory);
        }
        menu.addItem ((int) i + 1, list[i].name, true,
                      current == juce::String (factory::sourcePrefix) + list[i].id);
    }
    return menu;
}

void ConvolvePanel::chooseFactoryIR (int listIndex)
{
    const auto& list = SPAStripProcessor::getFactoryIRs();
    if (juce::isPositiveAndBelow (listIndex, (int) list.size()))
    {
        processor.loadFactoryIR (list[(size_t) listIndex].id);
        refreshLabels();
    }
}

void ConvolvePanel::clearIR()
{
    processor.clearConvolutionIR();
    refreshLabels();
}

bool ConvolvePanel::loadFile (const juce::File& f)
{
    if (! f.existsAsFile())
        return false;
    settings::setLastIRFolder (f);
    const bool ok = processor.loadConvolutionIR (f);
    if (! ok)
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Impulse response",
                                                "Could not read \"" + f.getFileName() + "\" as an audio file.");
    refreshLabels();
    return ok;
}

void ConvolvePanel::showFactoryMenu()
{
    auto menu = buildFactoryMenu();
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&factoryButton),
                        [safe = juce::Component::SafePointer<ConvolvePanel> (this)] (int result)
                        {
                            if (safe != nullptr && result > 0)
                                safe->chooseFactoryIR (result - 1);
                        });
}

void ConvolvePanel::showFileChooser()
{
    const auto last = settings::getLastIRFolder();
    fileChooser = std::make_unique<juce::FileChooser> (
        "Choose an impulse response",
        last.isDirectory() ? last : juce::File::getSpecialLocation (juce::File::userHomeDirectory),
        "*.wav;*.aif;*.aiff;*.flac");
    fileChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [safe = juce::Component::SafePointer<ConvolvePanel> (this)] (const juce::FileChooser& fc)
                              {
                                  if (safe != nullptr && fc.getResult().existsAsFile())
                                      safe->loadFile (fc.getResult());
                              });
}

bool ConvolvePanel::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& f : files)
        if (isAcceptedAudioFile (f))
            return true;
    return false;
}

void ConvolvePanel::fileDragEnter (const juce::StringArray&, int, int) { dragHighlight = true; repaint(); }
void ConvolvePanel::fileDragExit (const juce::StringArray&) { dragHighlight = false; repaint(); }

void ConvolvePanel::filesDropped (const juce::StringArray& files, int, int)
{
    dragHighlight = false;
    repaint();
    for (auto& path : files)
        if (isAcceptedAudioFile (path))
        {
            loadFile (juce::File (path));
            return;
        }
}

void ConvolvePanel::paint (juce::Graphics& g)
{
    const auto& t = currentTheme();

    // Current IR: "<name>  FACTORY|FILE" beside the picker buttons, in the title row.
    if (! nameRect.isEmpty())
    {
        auto r = nameRect;
        if (sourceTag.isNotEmpty())
        {
            g.setFont (metrics::smallFontBold());
            const auto tagW = (int) std::ceil (juce::GlyphArrangement::getStringWidth (metrics::smallFontBold(), sourceTag)) + 12;
            auto chip = r.removeFromRight (tagW).withSizeKeepingCentre (tagW, 15).toFloat();
            g.setColour (t.accent.withAlpha (0.16f));
            g.fillRoundedRectangle (chip, 3.0f);
            g.setColour (t.accent);
            g.drawText (sourceTag, chip.toNearestInt(), juce::Justification::centred);
            r.removeFromRight (6);
        }
        g.setColour (sourceTag.isNotEmpty() ? t.textPrimary : t.textSecondary.withAlpha (0.6f));
        g.setFont (metrics::labelFont());
        g.drawText (sourceTag.isNotEmpty() ? irName : juce::String ("no impulse"), r,
                    juce::Justification::centredRight, true);
    }

    if (dragHighlight)
    {
        auto bounds = getLocalBounds().toFloat().reduced (1.0f);
        for (int i = 3; i >= 0; --i)
        {
            const float inflate = (float) i * 1.5f;
            g.setColour (t.assignGlow.withAlpha (i == 0 ? 0.9f : 0.14f));
            g.drawRoundedRectangle (bounds.expanded (inflate), metrics::cornerRadius + inflate, i == 0 ? 2.0f : 1.5f);
        }
    }
}

void ConvolvePanel::resized()
{
    header.setBounds (getLocalBounds().removeFromTop (metrics::sectionHeaderHeight));
    auto r = getLocalBounds().withTrimmedTop (metrics::sectionHeaderHeight).reduced (7, 3);

    // Title row (right of "[toggle] CONVOLUTION"): [name  TAG]  [Factory IR v] [Load file...] [Clear]
    {
        constexpr int btnH = 22, gap = 5, rightInset = 7;
        const int y = metrics::sectionHeaderTopInset + (metrics::sectionHeaderHeight - metrics::sectionHeaderTopInset - btnH) / 2;
        int x = getWidth() - rightInset;
        clearButton.setBounds (x - 54, y, 54, btnH);
        x -= 54 + gap;
        fileButton.setBounds (x - 92, y, 92, btnH);
        x -= 92 + gap;
        factoryButton.setBounds (x - 100, y, 100, btnH);
        x -= 100 + 12;
        const int left = header.getContentRight() + 20;
        nameRect = juce::Rectangle<int> (left, y, juce::jmax (0, x - left), btnH);
    }

    auto strip = r.removeFromBottom (58);
    display.setBounds (r.reduced (0, 2));

    constexpr int cell = 64;
    for (auto* k : { &mix, &predelay, &start, &decay, &damping, &width })
    {
        k->setBounds (strip.removeFromLeft (cell));
        strip.removeFromLeft (2);
    }
}

} // namespace spa::ui
