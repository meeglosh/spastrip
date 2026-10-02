#include "SPAStripLookAndFeel.h"
#include "UiSettings.h"
#include "DraggableTabs.h"

namespace spa::ui
{

namespace
{
    // The one theme instance (message thread only). Accents load from the
    // user's saved preference on first access.
    Theme& mutableTheme()
    {
        static Theme theme = []
        {
            auto t = Theme::dark();
            t.accent = settings::getAccentColor (t.accent);
            t.accentMod = t.accent;
            return t;
        }();
        return theme;
    }
}

const Theme& currentTheme()
{
    return mutableTheme();
}

void setAccentColor (juce::Colour accent)
{
    mutableTheme().accent = accent;
    mutableTheme().accentMod = accent;
    settings::setAccentColor (accent);
}

void resetAccentColor()
{
    const auto defaults = Theme::dark();
    mutableTheme().accent = defaults.accent;
    mutableTheme().accentMod = defaults.accentMod;
    settings::clearAccentColor();
}

namespace draw
{

void panel (juce::Graphics&, juce::Rectangle<float>)
{
    // Faceplate restyle: modules no longer paint their own card. The
    // continuous surface (fill + texture + seams + row shadows) is painted
    // once behind everything by ContentComponent::paint, so it shows
    // through unbroken. Kept as a no-op (rather than deleting every call
    // site) so draw::panel(...) stays the single place to reintroduce a
    // module fill if that's ever needed again.
}

void trackedCentredText (juce::Graphics& g, const juce::Font& font, const juce::String& text,
                         juce::Rectangle<int> area, juce::Colour colour)
{
    juce::GlyphArrangement glyphs;
    glyphs.addLineOfText (font, text, 0.0f, 0.0f);

    juce::Path ink;
    glyphs.createPath (ink);
    const auto box = ink.getBounds();
    ink.applyTransform (juce::AffineTransform::translation (
        (float) area.getCentreX() - box.getCentreX(),
        (float) area.getCentreY() - box.getCentreY()));

    g.setColour (colour);
    g.fillPath (ink);
}

juce::Rectangle<int> sectionHeader (juce::Graphics& g, juce::Rectangle<int> bounds,
                                    const juce::String& title, const juce::String& readout,
                                    juce::Colour titleColour, bool recess)
{
    const auto& t = currentTheme();
    const auto full = bounds.removeFromTop (metrics::sectionHeaderHeight);
    auto header = full;
    header.removeFromTop (metrics::sectionHeaderTopInset);
    auto text = header.reduced (metrics::sectionHeaderLeftInset, 0);

    if (titleColour == juce::Colour())
        titleColour = t.textPrimary;

    g.setFont (metrics::sectionFont());

    const auto titleText = title.toUpperCase();
    juce::GlyphArrangement glyphs;
    glyphs.addLineOfText (metrics::sectionFont(), titleText, 0.0f, 0.0f);
    const auto titleWidth = (int) std::ceil (glyphs.getBoundingBox (0, -1, true).getWidth());

    g.setColour (titleColour);
    g.drawText (titleText, text, juce::Justification::centredLeft);

    if (readout.isNotEmpty())
    {
        const auto stringWidth = [] (const juce::String& s)
        {
            juce::GlyphArrangement glyphArrangement;
            glyphArrangement.addLineOfText (metrics::labelFont(), s, 0.0f, 0.0f);
            return (int) std::ceil (glyphArrangement.getBoundingBox (0, -1, true).getWidth());
        };

        // Never run under the title: fit into the space after title + a
        // minimum gap, ellipsizing the tail of long content names. (Used to
        // measure against a rule drawn between title and readout; the rule
        // has since moved to the band's bottom edge, but the same margin
        // still keeps the readout from crowding the title.)
        const auto available = text.getWidth() - titleWidth - 8 - 14;
        auto fitted = readout;
        if (stringWidth (fitted) > available)
        {
            while (fitted.length() > 4 && stringWidth (fitted + "...") > available)
                fitted = fitted.dropLastCharacters (1).trimEnd();
            fitted += "...";
        }

        if (available > 20)
        {
            g.setColour (t.textSecondary);
            g.setFont (metrics::labelFont());
            g.drawText (fitted, text, juce::Justification::centredRight);
        }
    }

    // Faceplate restyle: the rule moves from beside the title (old: a thin
    // line between title and readout, at title mid-height) to beneath it --
    // full header width, at the band's own bottom edge -- with the same
    // eased inner shadow rising from it that SPAStripLookAndFeel::
    // drawTabAreaBehindFrontButton casts over a tab strip. Identical recipe
    // (same helper, same draw::shadowStartAlpha, same 13px cap) so every
    // title band and every tab strip read as one shadow language; the two
    // rule colours were unified onto t.outline the same session (see
    // SPAStripLookAndFeel::refreshPalette's tabOutlineColourId comment).
    //
    // recess=false (iteration: double-recess fix, then no-rule-under-a-tab-
    // strip fix) skips both the shadow AND the rule -- used by headers that
    // sit directly under a tab strip already casting that same recessed
    // channel + rule (FilterPanel/FXPanel), so the module doesn't stack two
    // rule/shadow tiers. Title (and readout) still paint either way.
    if (recess)
    {
        const float lineY = (float) full.getBottom() - 1.0f;
        const float shadowLength = juce::jmin (13.0f, (float) full.getHeight());

        g.setGradientFill (easedShadowGradient ({ (float) full.getX(), lineY },
                                                { (float) full.getX(), lineY - shadowLength },
                                                shadowStartAlpha));
        g.fillRect (juce::Rectangle<float> ((float) full.getX(), lineY - shadowLength,
                                            (float) full.getWidth(), shadowLength));

        g.setColour (t.outline);
        g.fillRect (juce::Rectangle<int> (full.getX(), full.getBottom() - 1, full.getWidth(), 1));
    }

    return bounds;
}

void displayWell (juce::Graphics& g, juce::Rectangle<float> bounds, bool centreLine)
{
    const auto& t = currentTheme();

    // Faceplate restyle: no LED-screen well any more — curves render
    // straight on the faceplate surface (glowStroke). Keep only an
    // extremely faint zero/centre reference line; several scopes (LFO,
    // bipolar wave, filter) are otherwise hard to read with no baseline.
    // Non-scope callers (e.g. a plain list container) can opt out — a
    // reference line has no meaning there and just bisects the content.
    if (! centreLine)
        return;

    g.setColour (t.outline.withAlpha (0.18f));
    g.drawHorizontalLine ((int) bounds.getCentreY(), bounds.getX() + 2.0f,
                          bounds.getRight() - 2.0f);
}

void glowStroke (juce::Graphics& g, const juce::Path& path, juce::Colour colour, float thickness)
{
    // Three feathered passes for the mock's neon glow.
    g.setColour (colour.withAlpha (0.10f));
    g.strokePath (path, juce::PathStrokeType (thickness * 6.0f,
                                              juce::PathStrokeType::curved,
                                              juce::PathStrokeType::rounded));
    g.setColour (colour.withAlpha (0.28f));
    g.strokePath (path, juce::PathStrokeType (thickness * 2.6f,
                                              juce::PathStrokeType::curved,
                                              juce::PathStrokeType::rounded));
    g.setColour (colour.brighter (0.05f));
    g.strokePath (path, juce::PathStrokeType (thickness,
                                              juce::PathStrokeType::curved,
                                              juce::PathStrokeType::rounded));
}

juce::ColourGradient easedShadowGradient (juce::Point<float> from, juce::Point<float> to,
                                          float startAlpha)
{
    juce::ColourGradient shadow (juce::Colours::black.withAlpha (startAlpha), from,
                                 juce::Colours::transparentBlack, to, false);
    shadow.addColour (0.30, juce::Colours::black.withAlpha (startAlpha * 0.50f));
    shadow.addColour (0.62, juce::Colours::black.withAlpha (startAlpha * 0.20f));
    shadow.addColour (0.85, juce::Colours::black.withAlpha (startAlpha * 0.07f));
    return shadow;
}

} // namespace draw

SPAStripLookAndFeel::SPAStripLookAndFeel()
{
    refreshPalette();
}

void SPAStripLookAndFeel::refreshPalette()
{
    const auto& t = currentTheme();

    setColour (juce::ResizableWindow::backgroundColourId, t.background);
    setColour (juce::Label::textColourId, t.textSecondary);
    setColour (juce::TextButton::buttonColourId, t.knobFace);
    setColour (juce::TextButton::buttonOnColourId, t.accent.withAlpha (0.85f));
    setColour (juce::TextButton::textColourOffId, t.textPrimary);
    setColour (juce::TextButton::textColourOnId, t.display);
    setColour (juce::ComboBox::backgroundColourId, t.display);
    setColour (juce::ComboBox::textColourId, t.textPrimary);
    setColour (juce::ComboBox::outlineColourId, t.outline);
    setColour (juce::ComboBox::arrowColourId, t.textSecondary);
    setColour (juce::PopupMenu::backgroundColourId, t.panel);
    setColour (juce::PopupMenu::textColourId, t.textPrimary);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, t.accent.withAlpha (0.30f));
    setColour (juce::PopupMenu::highlightedTextColourId, t.textPrimary);
    setColour (juce::Slider::textBoxTextColourId, t.textPrimary);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::thumbColourId, t.accent);
    setColour (juce::Slider::trackColourId, t.knobTrack);
    setColour (juce::Slider::backgroundColourId, t.knobTrack);
    setColour (juce::ScrollBar::thumbColourId, t.knobTrack);
    setColour (juce::TooltipWindow::backgroundColourId, t.panel);
    setColour (juce::TooltipWindow::textColourId, t.textPrimary);
    setColour (juce::TooltipWindow::outlineColourId, t.outline);
    setColour (juce::BubbleComponent::backgroundColourId, t.panel);
    setColour (juce::TabbedButtonBar::tabTextColourId, t.textSecondary);
    setColour (juce::TabbedButtonBar::frontTextColourId, t.textPrimary);
    // Tab-strip recess rule (drawTabAreaBehindFrontButton's bottom-edge
    // line). Never tokenized before -- it painted with LookAndFeel_V4's
    // built-in dark-scheme default (a light, ~50%-alpha grey, nothing to do
    // with this theme), while draw::sectionHeader's rule used t.outline
    // directly. Restyle unifies both header-band families onto one rule
    // colour so a tab strip and a title band read as the same milled
    // channel where they sit in the same row.
    setColour (juce::TabbedButtonBar::tabOutlineColourId, t.outline);
    // JUCE's TabbedComponent fills a 1px outline around its content area in
    // this colour whenever outlineThickness > 0 (the default); it was never
    // tokenized, so it painted with the stock LookAndFeel_V4 default rather
    // than any theme colour. Faceplate restyle has no card/frame around the
    // filter/env/lfo/fx tab content any more, so make it fully transparent
    // instead of chasing setOutline(0) on every TabbedComponent instance.
    setColour (juce::TabbedComponent::outlineColourId, juce::Colours::transparentBlack);
    setColour (juce::AlertWindow::backgroundColourId, t.panel);
    setColour (juce::AlertWindow::textColourId, t.textPrimary);
}

void SPAStripLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                                           float sliderPos, float rotaryStartAngle,
                                           float rotaryEndAngle, juce::Slider& slider)
{
    const auto& t = currentTheme();
    const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (2.0f);
    const auto radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto centre = bounds.getCentre();
    const auto angle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);
    // Flat, modern knob (per the Audio Damage reference): a thin track ring,
    // an accent value arc, and a small position dot. No disc, bevel, drop
    // shadow or needle. Sizes/placement are unchanged — only the paint.
    const auto lineW = juce::jlimit (1.6f, 2.6f, radius * 0.12f);
    const auto arcRadius = radius - lineW * 1.2f;   // inset so the dot stays in bounds

    // The WILD knob's ring heats from the accent (0%) up to bright red (100%)
    // so the chaos amount reads at a glance. Interpolate in HSV so the sweep
    // stays vivid (teal -> green -> amber -> red) rather than passing through
    // muddy greys, and land exactly on the accent at 0 and red at 1.
    const auto wildHeat = [&t] (float pos)
    {
        const juce::Colour red (0xffff2a24);
        const auto a = t.accent;
        pos = juce::jlimit (0.0f, 1.0f, pos);
        return juce::Colour::fromHSV (
            a.getHue()        + (red.getHue()        - a.getHue())        * pos,
            a.getSaturation() + (red.getSaturation() - a.getSaturation()) * pos,
            a.getBrightness() + (red.getBrightness() - a.getBrightness()) * pos,
            1.0f);
    };

    // A knob whose parameter is assigned to any mod matrix slot renders in
    // the fixed violet indicator colour everywhere it would otherwise use
    // the accent -- as soon as the assignment exists, whether or not audio
    // is playing and regardless of the route's depth. Controls.h's
    // Knob::pollModViz publishes this once per tick from the per-editor
    // ModAssignSource table (see Controls.h), so it's live even while
    // dragging a matrix depth slider with the transport stopped. This is
    // deliberately NOT the same colour as the live modActive overlay below
    // (t.accentMod, user-tintable) -- modAssignedColour() is fixed and
    // never touched by the accent picker.
    const auto modAssigned = slider.getProperties().getWithDefault ("modAssigned", false);
    const auto accent  = slider.getComponentID() == "wild" ? wildHeat (sliderPos)
                       : slider.getComponentID() == "mod"  ? t.accentMod
                       : (bool) modAssigned                ? modAssignedColour()
                                                           : t.accent;
    const auto enabled = slider.isEnabled();
    const auto hot     = slider.isMouseOverOrDragging() && enabled;

    // Track ring (full sweep).
    juce::Path track;
    track.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f,
                         rotaryStartAngle, rotaryEndAngle, true);
    g.setColour (hot ? t.knobTrack.brighter (0.15f)
                     : t.knobTrack.withAlpha (enabled ? 0.9f : 0.5f));
    g.strokePath (track, juce::PathStrokeType (lineW, juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));

    // Value arc: grows from the centre for bipolar params, else from the start.
    const auto isBipolar = slider.getMinimum() < 0.0 && slider.getMaximum() > 0.0;
    const auto fillStart = isBipolar
                         ? rotaryStartAngle + 0.5f * (rotaryEndAngle - rotaryStartAngle)
                         : rotaryStartAngle;
    const auto arcColour = enabled ? accent : t.textSecondary.withAlpha (0.35f);

    juce::Path value;
    value.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f,
                         fillStart, angle, true);
    if (enabled)   // faint bloom keeps a hint of the accent glow, subtly
    {
        g.setColour (accent.withAlpha (hot ? 0.28f : 0.16f));
        g.strokePath (value, juce::PathStrokeType (lineW * 2.6f,
                                                   juce::PathStrokeType::curved,
                                                   juce::PathStrokeType::rounded));
    }
    g.setColour (arcColour);
    g.strokePath (value, juce::PathStrokeType (lineW, juce::PathStrokeType::curved,
                                               juce::PathStrokeType::rounded));

    // Position dot riding the ring at the current angle.
    const auto dotR = lineW * (hot ? 1.35f : 1.15f);
    const auto dot  = centre.getPointOnCircumference (arcRadius, angle);
    g.setColour (enabled ? accent.brighter (0.15f) : t.textSecondary.withAlpha (0.4f));
    g.fillEllipse (dot.x - dotR, dot.y - dotR, dotR * 2.0f, dotR * 2.0f);

    // Static reachable-range arc for matrix-assigned knobs: how far the
    // assigned routes' combined depths (Controls.h's ModAssignInfo,
    // polarity-aware -- a unipolar source only pulls one way) can move this
    // knob from its current base setting, clamped to the knob's own travel.
    // Updates live as a matrix depth slider is dragged, with no audio
    // required (see ModAssignTable in SPASynthEditor.cpp) -- this answers
    // "is this wired" and "how far can it go", independent of the moving
    // dot below which answers "is it moving right now". Drawn BEFORE the
    // live overlay so that overlay always paints on top of it.
    if (enabled && (bool) modAssigned)
    {
        const auto negReach = (float) (double) slider.getProperties().getWithDefault ("modRangeNeg", 0.0);
        const auto posReach = (float) (double) slider.getProperties().getWithDefault ("modRangePos", 0.0);
        if (negReach > 0.0f || posReach > 0.0f)
        {
            const auto lo = juce::jlimit (0.0f, 1.0f, sliderPos - negReach);
            const auto hi = juce::jlimit (0.0f, 1.0f, sliderPos + posReach);
            const auto loAngle = rotaryStartAngle + lo * (rotaryEndAngle - rotaryStartAngle);
            const auto hiAngle = rotaryStartAngle + hi * (rotaryEndAngle - rotaryStartAngle);

            juce::Path reach;
            reach.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f,
                                 loAngle, hiAngle, true);
            g.setColour (modAssignedColour().withAlpha (0.32f));
            g.strokePath (reach, juce::PathStrokeType (lineW * 2.2f,
                                                       juce::PathStrokeType::curved,
                                                       juce::PathStrokeType::rounded));
        }
    }

    // Modulation-viz overlay (Controls.h's Knob::pollModViz publishes these
    // slider properties at 30 Hz for mod-destination knobs): a translucent
    // "modulation range" arc from the base value to the live modulated
    // value, plus a small bright dot at the modulated position just outside
    // the ring. The base pointer/arc above are untouched -- they must keep
    // showing the user's actual base setting, never the modulated one.
    if (enabled && slider.getProperties().contains ("modActive")
                && (bool) slider.getProperties()["modActive"])
    {
        const auto modNorm = juce::jlimit (0.0f, 1.0f,
            (float) (double) slider.getProperties()["modValue"]);
        const auto modAngle = rotaryStartAngle + modNorm * (rotaryEndAngle - rotaryStartAngle);
        const auto dimmed = (bool) slider.getProperties().getWithDefault ("modDim", false);
        const auto modAlphaScale = dimmed ? 0.5f : 1.0f;

        juce::Path modRange;
        modRange.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f,
                                juce::jmin (angle, modAngle), juce::jmax (angle, modAngle), true);
        g.setColour (t.accentMod.withAlpha (0.35f * modAlphaScale));
        g.strokePath (modRange, juce::PathStrokeType (lineW * 1.7f,
                                                       juce::PathStrokeType::curved,
                                                       juce::PathStrokeType::rounded));

        const auto modDotR = lineW * 0.9f;
        const auto modDot = centre.getPointOnCircumference (arcRadius + lineW * 1.6f, modAngle);
        g.setColour (t.accentMod.brighter (0.2f).withAlpha (0.95f * modAlphaScale));
        g.fillEllipse (modDot.x - modDotR, modDot.y - modDotR, modDotR * 2.0f, modDotR * 2.0f);
    }

    // Press state for label-less knobs that opt in: a value chip across the
    // knob (used by the header master volume).
    if (slider.isMouseButtonDown()
        && slider.getProperties().contains ("inlineValueSuffix"))
    {
        const auto magnitude = std::abs (slider.getValue());
        const auto text = juce::String (slider.getValue(),
                                        magnitude >= 1000.0 ? 0 : 1)
                        + slider.getProperties()["inlineValueSuffix"].toString();
        g.setFont (metrics::smallFont());

        juce::GlyphArrangement glyphs;
        glyphs.addLineOfText (metrics::smallFont(), text, 0.0f, 0.0f);
        const auto w = glyphs.getBoundingBox (0, -1, true).getWidth() + 10.0f;
        const auto chip = juce::Rectangle<float> (w, 14.0f).withCentre (centre);

        g.setColour (t.display.withAlpha (0.92f));
        g.fillRoundedRectangle (chip, 3.0f);
        g.setColour (t.outline);
        g.drawRoundedRectangle (chip, 3.0f, 1.0f);
        g.setColour (t.textPrimary);
        g.drawText (text, chip.toNearestInt(), juce::Justification::centred);
    }
}

void SPAStripLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                                           float sliderPos, float, float,
                                           juce::Slider::SliderStyle style, juce::Slider& slider)
{
    const auto& t = currentTheme();

    if (style == juce::Slider::LinearHorizontal || style == juce::Slider::LinearBar)
    {
        const auto track = juce::Rectangle<float> ((float) x, (float) y + (float) height * 0.5f - 1.5f,
                                                   (float) width, 3.0f);
        g.setColour (t.knobTrack);
        g.fillRoundedRectangle (track, 1.5f);

        const auto accent = slider.getComponentID() == "mod" ? t.accentMod : t.accent;
        const auto isBipolar = slider.getMinimum() < 0.0 && slider.getMaximum() > 0.0;
        const auto zeroX = isBipolar ? (float) x + (float) width * 0.5f : (float) x;
        g.setColour (accent);
        g.fillRoundedRectangle (juce::Rectangle<float> (juce::jmin (zeroX, sliderPos),
                                                        track.getY(),
                                                        std::abs (sliderPos - zeroX), 3.0f),
                                1.5f);

        g.setColour (t.textPrimary);
        g.fillEllipse (sliderPos - 4.0f, (float) y + (float) height * 0.5f - 4.0f, 8.0f, 8.0f);

        // Press state: numeric readout inside the slider, on whichever side
        // of the thumb has room.
        if (slider.isMouseButtonDown())
        {
            const auto text = juce::String (slider.getValue(), 2);
            g.setFont (metrics::smallFont());
            g.setColour (t.textPrimary);
            const auto onLeft = sliderPos > (float) x + (float) width * 0.5f;
            const auto textArea = onLeft
                ? juce::Rectangle<int> (x + 2, y, (int) (sliderPos - (float) x) - 10, height)
                : juce::Rectangle<int> ((int) sliderPos + 10, y,
                                        x + width - (int) sliderPos - 12, height);
            g.drawText (text, textArea,
                        onLeft ? juce::Justification::centredLeft
                               : juce::Justification::centredRight);
        }
        return;
    }

    LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos, 0, 0, style, slider);
}

void SPAStripLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button,
                                               const juce::Colour& backgroundColour,
                                               bool highlighted, bool down)
{
    const auto& t = currentTheme();
    auto bounds = button.getLocalBounds().toFloat().reduced (0.5f);

    // Primary action: glossy accent gradient + glow (RANDOMIZE ALL).
    if (button.getComponentID() == "primary")
    {
        g.setColour (t.accent.withAlpha (down ? 0.15f : 0.30f));
        g.fillRoundedRectangle (bounds.expanded (2.5f), metrics::cornerRadius + 2.0f);

        auto top = t.accent.brighter (highlighted ? 0.22f : 0.14f);
        auto bottom = t.accent.darker (down ? 0.35f : 0.18f);
        g.setGradientFill (juce::ColourGradient (top, bounds.getX(), bounds.getY(),
                                                 bottom, bounds.getX(), bounds.getBottom(),
                                                 false));
        g.fillRoundedRectangle (bounds, metrics::cornerRadius);

        g.setColour (juce::Colours::white.withAlpha (0.25f));
        g.drawLine (bounds.getX() + metrics::cornerRadius, bounds.getY() + 1.0f,
                    bounds.getRight() - metrics::cornerRadius, bounds.getY() + 1.0f, 1.0f);
        return;
    }

    auto colour = backgroundColour;
    if (button.getToggleState())
        colour = findColour (juce::TextButton::buttonOnColourId);
    if (down)
        colour = colour.darker (0.15f);
    else if (highlighted)
        colour = colour.brighter (0.07f);

    g.setColour (colour);
    g.fillRoundedRectangle (bounds, metrics::cornerRadius);
    g.setColour (t.outline);
    g.drawRoundedRectangle (bounds, metrics::cornerRadius, 1.0f);
}

// Minimal padlock glyph inside rect r: a stroked shackle (inverted U) over a
// filled rounded-rect body. Used to mark a locked section button.
void drawLockGlyph (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour c)
{
    const float stroke = juce::jmax (1.0f, r.getHeight() * 0.13f);
    const float bodyH  = r.getHeight() * 0.56f;
    const auto  body   = juce::Rectangle<float> (r.getX(), r.getBottom() - bodyH,
                                                 r.getWidth(), bodyH);
    const float sr     = r.getWidth() * 0.30f;          // shackle radius
    const float scy    = body.getY() - sr * 0.30f;      // shackle arc centre
    const float legB   = body.getY() + stroke * 0.4f;   // legs sink into the body

    juce::Path shackle;
    shackle.startNewSubPath (r.getCentreX() - sr, legB);
    shackle.lineTo          (r.getCentreX() - sr, scy);
    shackle.addCentredArc    (r.getCentreX(), scy, sr, sr, 0.0f,
                              -juce::MathConstants<float>::halfPi,
                               juce::MathConstants<float>::halfPi, false);
    shackle.lineTo          (r.getCentreX() + sr, legB);

    g.setColour (c);
    g.strokePath (shackle, juce::PathStrokeType (stroke, juce::PathStrokeType::curved,
                                                 juce::PathStrokeType::rounded));
    g.fillRoundedRectangle (body, r.getWidth() * 0.16f);
}

void SPAStripLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& button,
                                          bool highlighted, bool down)
{
    // Preset prev/next: draw a solid left/right triangle caret instead of a
    // "<"/">" glyph.
    const auto navId = button.getComponentID();
    if (navId == "navPrev" || navId == "navNext")
    {
        const auto c = button.getLocalBounds().toFloat().getCentre();
        constexpr float w = 3.6f, h = 9.0f;   // triangle half-width / height
        juce::Path tri;
        if (navId == "navPrev")   // points left
            tri.addTriangle (c.x - w, c.y, c.x + w, c.y - h * 0.5f, c.x + w, c.y + h * 0.5f);
        else                      // points right
            tri.addTriangle (c.x + w, c.y, c.x - w, c.y - h * 0.5f, c.x - w, c.y + h * 0.5f);

        g.setColour (button.findColour (juce::TextButton::textColourOffId)
                         .withAlpha (down ? 0.55f : (highlighted ? 1.0f : 0.85f)));
        g.fillPath (tri);
        return;
    }

    // Undo / redo (1.0.29): a curved arrow, drawn (not a font glyph, so it is
    // identical on every platform). Redo is the undo shape mirrored. Dim when
    // there is nothing to step to.
    if (navId == "undoButton" || navId == "redoButton")
    {
        const auto b = button.getLocalBounds().toFloat();
        const auto c = b.getCentre();
        const float r = juce::jmin (b.getWidth(), b.getHeight()) * 0.20f;
        const float stroke = 1.1f;
        juce::Path arc, head;
        // Body: from 9 o'clock over the top and down the right side to about 5.
        arc.addCentredArc (c.x, c.y + r * 0.1f, r, r, 0.0f,
                           -juce::MathConstants<float>::halfPi, juce::MathConstants<float>::pi * 0.8f, true);
        const float hs = r * 0.62f;
        const float hx = c.x - r, hy = c.y + r * 0.1f;
        head.addTriangle (hx - hs * 0.85f, hy, hx + hs * 0.85f, hy, hx, hy + hs * 1.15f);
        if (navId == "redoButton")
        {
            const auto flip = juce::AffineTransform::scale (-1.0f, 1.0f, c.x, c.y);
            arc.applyTransform (flip);
            head.applyTransform (flip);
        }
        const auto alpha = ! button.isEnabled() ? 0.28f : (down ? 0.55f : (highlighted ? 1.0f : 0.85f));
        g.setColour (button.findColour (juce::TextButton::textColourOffId).withAlpha (alpha));
        g.strokePath (arc, juce::PathStrokeType (stroke, juce::PathStrokeType::curved,
                                                 juce::PathStrokeType::butt));
        g.fillPath (head);
        return;
    }

    // The mod matrix's two ASSIGN buttons (SOURCE/DEST -- see MatrixPanel::
    // AssignKind) each show the same small padlock, beside the label, while
    // latched (MatrixPanel::AssignMode::latched -- see MatrixPanel::
    // applyMode, which is the only place that sets/clears the
    // "assignLatched" property). Matched by componentID prefix so whichever
    // one is latched ("matrixAssignSource" or "matrixAssignDest") shows it.
    // One-shot mode lights the button the same way ordinary toggle-on does,
    // with no glyph.
    const bool showAssignLock = button.getComponentID().startsWith ("matrixAssign") && button.getToggleState()
                               && (bool) button.getProperties().getWithDefault ("assignLatched", false);

    // Locked section buttons show a small padlock beside the label so the
    // locked state reads as "locked", not just a colour change. Everything
    // else uses the default text rendering.
    if ((button.getComponentID() != "lock" || ! button.getToggleState()) && ! showAssignLock)
    {
        LookAndFeel_V4::drawButtonText (g, button, highlighted, down);
        return;
    }

    const auto font = getTextButtonFont (button, button.getHeight());
    const auto text = button.getButtonText();

    juce::GlyphArrangement ga;
    ga.addLineOfText (font, text, 0.0f, 0.0f);
    const auto textW = ga.getBoundingBox (0, -1, true).getWidth();

    const auto h      = (float) button.getHeight();
    const auto iconH  = juce::jmin (h * 0.5f, font.getHeight() * 0.95f);
    const auto iconW  = iconH * 0.74f;
    const auto gap    = 4.0f;
    const auto groupW = iconW + gap + textW;
    const auto startX = juce::jmax (2.0f, ((float) button.getWidth() - groupW) * 0.5f);

    const auto colour = button.findColour (juce::TextButton::textColourOnId);
    drawLockGlyph (g, { startX, (h - iconH) * 0.5f, iconW, iconH }, colour);

    g.setColour (colour);
    g.setFont (font);
    g.drawText (text, juce::Rectangle<float> (startX + iconW + gap, 0.0f,
                                              (float) button.getWidth() - (startX + iconW + gap),
                                              h).toNearestInt(),
                juce::Justification::centredLeft, true);
}

void SPAStripLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& button,
                                           bool highlighted, bool)
{
    const auto& t = currentTheme();
    const auto bounds = button.getLocalBounds().toFloat();
    const auto pillH = juce::jmin (14.0f, bounds.getHeight() - 4.0f);
    const auto pillW = pillH * 1.9f;
    const auto pill = juce::Rectangle<float> (bounds.getX(),
                                              bounds.getCentreY() - pillH * 0.5f,
                                              pillW, pillH);
    const auto enabled = button.isEnabled();

    g.setColour ((button.getToggleState() ? t.accentMod
                                          : t.knobTrack.brighter (highlighted ? 0.08f : 0.0f))
                    .withMultipliedAlpha (enabled ? 1.0f : 0.5f));
    g.fillRoundedRectangle (pill, pillH * 0.5f);

    const auto knobX = button.getToggleState() ? pill.getRight() - pillH + 2.0f
                                               : pill.getX() + 2.0f;
    g.setColour (t.textPrimary.withAlpha (enabled ? 1.0f : 0.4f));
    g.fillEllipse (knobX, pill.getY() + 2.0f, pillH - 4.0f, pillH - 4.0f);

    g.setColour (t.textSecondary.withAlpha (enabled ? 1.0f : 0.4f));
    g.setFont (metrics::smallFont());
    g.drawText (button.getButtonText().toUpperCase(),
                bounds.withTrimmedLeft (pillW + 5.0f).toNearestInt(),
                juce::Justification::centredLeft);
}

void SPAStripLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height, bool,
                                       int, int, int, int, juce::ComboBox& box)
{
    const auto& t = currentTheme();
    const auto bounds = juce::Rectangle<float> (0.0f, 0.0f, (float) width, (float) height)
                            .reduced (0.5f);
    const auto enabled = box.isEnabled();

    g.setColour (findColour (juce::ComboBox::backgroundColourId).withMultipliedAlpha (enabled ? 1.0f : 0.5f));
    g.fillRoundedRectangle (bounds, metrics::cornerRadius);
    g.setColour (t.outline.withAlpha (enabled ? 1.0f : 0.5f));
    g.drawRoundedRectangle (bounds, metrics::cornerRadius, 1.0f);

    juce::Path chevron;
    const auto cx = (float) width - 11.0f;
    const auto cy = (float) height * 0.5f;
    chevron.startNewSubPath (cx - 3.5f, cy - 1.8f);
    chevron.lineTo (cx, cy + 2.2f);
    chevron.lineTo (cx + 3.5f, cy - 1.8f);
    g.setColour (t.textSecondary.withAlpha (enabled ? 1.0f : 0.4f));
    g.strokePath (chevron, juce::PathStrokeType (1.4f));
}

juce::Label* SPAStripLookAndFeel::createComboBoxTextBox (juce::ComboBox&)
{
    // Matches LookAndFeel_V2's default (juce_LookAndFeel_V2.cpp) except for
    // the focus flag -- see this override's declaration comment.
    auto* label = new juce::Label();
    label->setMouseClickGrabsKeyboardFocus (false);
    return label;
}

// Draggable FX tabs draw a grip-dots handle at the left; reserve room for it so
// a re-laid-out (tight) bar never slides the centred text onto the grip.

// Tabs bold their label when the thing they represent is "engaged" (e.g. an
// enabled FX). Queried generically by name via DraggableTabs::isTabEngaged so
// the LnF stays FX-agnostic; the mapping lives in ContentComponent.
static bool isTabEngaged (const juce::TabBarButton& button)
{
    if (auto* tabs = dynamic_cast<DraggableTabs*> (button.getTabbedButtonBar().getParentComponent()))
        if (tabs->isTabEngaged)
            return tabs->isTabEngaged (button.getButtonText());
    return false;
}

int SPAStripLookAndFeel::getTabButtonBestWidth (juce::TabBarButton& button, int tabDepth)
{
    juce::GlyphArrangement glyphs;
    // Always measure with the bold variant so a tab's width never changes
    // when it gains/loses the bold "engaged" weight (tabLayoutInvarianceTest
    // asserts stable bounds; a width jump on toggle would look broken).
    glyphs.addLineOfText (metrics::smallFontBold(), button.getButtonText(), 0.0f, 0.0f);
    const int grip = dynamic_cast<DraggableTabButton*> (&button) != nullptr
                         ? DraggableTabButton::gripReserve + DraggableTabButton::lockReserve : 0;
    // Floor of 2x the tab-bar depth deliberately matches JUCE's own
    // LookAndFeel_V2 default (see its getTabButtonBestWidth) -- short tab
    // names (AMP, ENV 2, LFO 1...) need a floor at all, and this is the one
    // that was already baked into the generous/evenly-spaced look every FX
    // and ENV/LFO/Filter tab bar shipped with, since every one of those bars
    // gets its real width computed only once (see SPASynthEditor's
    // constructor) and, before that constructor fix, briefly fell back to
    // LookAndFeel_V2's formula for that one pass. Longer names (FILTER 1,
    // CHORUS...) are unaffected -- their text width already clears this.
    return juce::jmax (tabDepth * 2, (int) std::ceil (glyphs.getBoundingBox (0, -1, true).getWidth())
                              + 16 + grip);
}

void SPAStripLookAndFeel::drawTabButton (juce::TabBarButton& button, juce::Graphics& g,
                                        bool isMouseOver, bool)
{
    const auto& t = currentTheme();
    const auto bounds = button.getLocalBounds().toFloat().reduced (1.0f, 2.0f);
    const auto front = button.isFrontTab();

    const auto* dtb = dynamic_cast<DraggableTabButton*> (&button);
    const bool locked = dtb != nullptr && dtb->isLockedNow();

    if (locked)
    {
        // A locked effect (kept through Randomize All): a faint amber wash
        // behind the whole tab, on top of the padlock itself.
        g.setColour (t.assignSelected.withAlpha (front ? 0.14f : 0.09f));
        g.fillRoundedRectangle (bounds, 3.0f);
    }

    if (front)
    {
        // Faceplate restyle: no display-black pill behind the front tab —
        // just the accent underline against the continuous surface, with
        // brighter text (below) carrying the "selected" read.
        auto underline = bounds;
        g.setColour (t.accent);
        g.fillRect (underline.removeFromBottom (2.0f).reduced (4.0f, 0.0f));
    }
    else if (isMouseOver)
    {
        g.setColour (t.seam.withAlpha (0.6f));
        g.fillRoundedRectangle (bounds, 2.0f);
    }

    const bool engaged = isTabEngaged (button);
    g.setColour (front || engaged ? t.textPrimary : t.textSecondary);
    g.setFont (engaged ? metrics::smallFontBold() : metrics::smallFont());
    auto textArea = button.getLocalBounds();
    if (dtb != nullptr)
    {
        textArea.removeFromLeft (DraggableTabButton::gripReserve);    // clear of the grip
        textArea.removeFromRight (DraggableTabButton::lockReserve);   // and of the padlock
    }
    g.drawText (button.getButtonText().toUpperCase(), textArea, juce::Justification::centred);
}

void drawPadlockGlyph (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour c, bool locked)
{
    if (locked)
    {
        drawLockGlyph (g, r, c);
        return;
    }
    // Unlocked: the same body with the shackle swung open on the right.
    const float stroke = juce::jmax (1.0f, r.getHeight() * 0.13f);
    const float bodyH  = r.getHeight() * 0.56f;
    const auto  body   = juce::Rectangle<float> (r.getX(), r.getBottom() - bodyH, r.getWidth(), bodyH);
    const float sr     = r.getWidth() * 0.30f;
    const float scy    = body.getY() - sr * 0.30f;
    const float legB   = body.getY() + stroke * 0.4f;

    juce::Path shackle;
    shackle.startNewSubPath (r.getCentreX() - sr, legB);
    shackle.lineTo          (r.getCentreX() - sr, scy);
    shackle.addCentredArc    (r.getCentreX(), scy, sr, sr, 0.0f,
                              -juce::MathConstants<float>::halfPi,
                               juce::MathConstants<float>::halfPi * 0.25f, false);
    g.setColour (c);
    g.strokePath (shackle, juce::PathStrokeType (stroke, juce::PathStrokeType::curved,
                                                 juce::PathStrokeType::rounded));
    g.fillRoundedRectangle (body, r.getWidth() * 0.16f);
}

void DraggableTabButton::paintButton (juce::Graphics& g, bool over, bool down)
{
    juce::TabBarButton::paintButton (g, over, down);   // the LookAndFeel's tab

    const auto& t = currentTheme();
    const auto b = getLocalBounds().toFloat();

    // Grip-dots handle at the left, so the drag is discoverable.
    {
        const float x = b.getX() + 6.0f;
        const float cy = b.getCentreY();
        constexpr float d = 1.5f, gap = 3.0f;
        g.setColour (juce::Colours::white.withAlpha (over || down ? 0.55f : 0.30f));
        for (int col = 0; col < 2; ++col)
            for (int row = -1; row <= 1; ++row)
                g.fillEllipse (x + (float) col * gap, cy + (float) row * gap - d * 0.5f, d, d);
    }

    // Padlock at the right: bright amber + closed when locked, a dim open
    // padlock when not (brighter while hovered).
    const bool locked = isLockedNow();
    const auto lr = getLockRect().toFloat();
    const float h = juce::jmin (10.0f, lr.getHeight() - 2.0f);   // glyph is smaller than the (unchanged) click target
    const auto glyph = juce::Rectangle<float> (h * 0.78f, h).withCentre (lr.getCentre());
    const auto colour = locked ? t.assignSelected
                               : t.textSecondary.withAlpha (lockHovered() ? 0.9f : 0.34f);
    drawPadlockGlyph (g, glyph, colour, locked);
}

void SPAStripLookAndFeel::drawTabbedButtonBarBackground (juce::TabbedButtonBar&, juce::Graphics&)
{
}

void SPAStripLookAndFeel::drawTabAreaBehindFrontButton (juce::TabbedButtonBar& bar, juce::Graphics& g,
                                                        int w, int h)
{
    // This is where the light rule under every tab strip actually comes
    // from: it's JUCE's stock LookAndFeel_V3::drawTabAreaBehindFrontButton
    // (never overridden before now -- drawTabbedButtonBarBackground above is
    // a no-op, and drawTabButton only paints each button itself), which
    // draws a 1px TabbedButtonBar::tabOutlineColourId line along the bar's
    // own bottom edge (the bar component's height IS the tab-strip depth --
    // just the tab-label row itself, not the panel below). It also paints
    // its own very faint
    // built-in shadow -- replaced here with a real one matched to the
    // faceplate's shadow language.
    //
    // Faceplate restyle: recess the whole strip -- an inner shadow cast
    // UPWARDS from that line, darkest right above it and fading out as it
    // rises through the tab-strip area, so the selector reads as milled
    // into the plate rather than floating on it. Same family as
    // ContentComponent::paint's row-overhang shadows (crisp edge + eased
    // falloff), same alphas -- only the geometry is mirrored (shadow rises
    // from the strip's own bottom edge instead of falling from a row
    // boundary) and the falloff length is capped to the strip's actual
    // depth so it never bleeds into the tab labels' own row above.
    if (bar.getOrientation() != juce::TabbedButtonBar::TabsAtTop)
    {
        // Not used anywhere in this codebase (every tab bar runs TabsAtTop),
        // but keep other orientations correct via the stock JUCE look.
        juce::LookAndFeel_V3::drawTabAreaBehindFrontButton (bar, g, w, h);
        return;
    }

    // Literally the same recipe as the row-overhang shadow (same startAlpha,
    // same eased stop shape -- via draw::easedShadowGradient) so the two
    // read as one shadow language. An earlier iteration boosted this strip's
    // alpha to 0.62, reasoning that its near-black surface (no lighter card
    // underneath, unlike the module rows) would wash the recipe out --
    // pixel-sampling proved that reasoning wrong, and Mike decided he
    // prefers the subtler read anyway. Both call sites share
    // draw::shadowStartAlpha (Theme.h) so they can't drift apart; lightened
    // from 0.42f to 0.30f (2026-08-31, Mike: "a little dark"). Geometry
    // stays mirrored (the shadow rises from the strip's own bottom rule
    // rather than falling from a row boundary) and the falloff stays capped
    // to the strip's actual depth so it never bleeds into the tab labels'
    // own row above.
    const float shadowLength = juce::jmin (13.0f, (float) h);
    const float lineY = (float) h - 1.0f;

    g.setGradientFill (draw::easedShadowGradient ({ 0.0f, lineY }, { 0.0f, lineY - shadowLength },
                                                  draw::shadowStartAlpha));
    g.fillRect (juce::Rectangle<float> (0.0f, lineY - shadowLength, (float) w, shadowLength));

    // The rule itself -- kept exactly as stock JUCE draws it (same colour,
    // same 1px bottom edge) so the tab bar's contract with the rest of the
    // look and feel (tabOutlineColourId) is unchanged; it's now the lip the
    // recess reads against instead of a bare divider.
    g.setColour (bar.findColour (juce::TabbedButtonBar::tabOutlineColourId));
    g.fillRect (juce::Rectangle<int> (0, h - 1, w, 1));
}

// Adapted from LookAndFeel_V4::drawPopupMenuItem (juce_LookAndFeel_V4.cpp).
// The only deliberate change is the tick-sizing block, marked below: V4
// scales the tick to fill nearly the whole icon column (icon column height
// == maxFontHeight, derived from the row height), which against our
// labelFont() metrics reads as an oversized checkmark. Everything else --
// separator, highlight fill, icon drawable path, submenu arrow, shortcut
// text, enabled/disabled alpha -- is kept identical to V4 so no other menu
// behaviour changes. getTickShape() itself is untouched (shared with
// drawTickBox() elsewhere); only the rectangle its path is scaled into here
// is smaller.
void SPAStripLookAndFeel::drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle<int>& area,
                                             const bool isSeparator, const bool isActive,
                                             const bool isHighlighted, const bool isTicked,
                                             const bool hasSubMenu, const juce::String& text,
                                             const juce::String& shortcutKeyText,
                                             const juce::Drawable* icon,
                                             const juce::Colour* const textColourToUse)
{
    if (isSeparator)
    {
        auto r = area.reduced (5, 0);
        r.removeFromTop (juce::roundToInt (((float) r.getHeight() * 0.5f) - 0.5f));

        g.setColour (findColour (juce::PopupMenu::textColourId).withAlpha (0.3f));
        g.fillRect (r.removeFromTop (1));
        return;
    }

    auto textColour = (textColourToUse == nullptr ? findColour (juce::PopupMenu::textColourId)
                                                   : *textColourToUse);

    auto r = area.reduced (1);

    if (isHighlighted && isActive)
    {
        g.setColour (findColour (juce::PopupMenu::highlightedBackgroundColourId));
        g.fillRect (r);

        g.setColour (findColour (juce::PopupMenu::highlightedTextColourId));
    }
    else
    {
        g.setColour (textColour.withMultipliedAlpha (isActive ? 1.0f : 0.5f));
    }

    r.reduce (juce::jmin (5, area.getWidth() / 20), 0);

    auto font = getPopupMenuFont();

    auto maxFontHeight = (float) r.getHeight() / 1.3f;

    if (font.getHeight() > maxFontHeight)
        font.setHeight (maxFontHeight);

    g.setFont (font);

    auto iconArea = r.removeFromLeft (juce::roundToInt (maxFontHeight)).toFloat();

    if (icon != nullptr)
    {
        icon->drawWithin (g, iconArea, juce::RectanglePlacement::centred
                                          | juce::RectanglePlacement::onlyReduceInSize, 1.0f);
        r.removeFromLeft (juce::roundToInt (maxFontHeight * 0.5f));
    }
    else if (isTicked)
    {
        // --- tick-sizing change starts here ---
        // V4: tick.getTransformToScaleToFit (iconArea.reduced (iconArea.getWidth() / 5, 0), true)
        // scales the tick to the icon column's FULL height, only trimming
        // width -- that's the oversized checkmark. Shrink the target rect
        // further, evenly about its own centre, so the tick reads as a
        // modest mark beside the label rather than filling the column.
        // getTransformToScaleToFit(..., true) keeps proportions and centres
        // within whatever rect it's given, so shrinking-about-centre here
        // is all that's needed to shrink and re-centre the mark together.
        constexpr float tickSizeFactor = 0.5f;    // ~50% of the V4 drawn size
        auto tickArea = iconArea.reduced (iconArea.getWidth() / 5, 0.0f);
        tickArea = tickArea.withSizeKeepingCentre (tickArea.getWidth() * tickSizeFactor,
                                                    tickArea.getHeight() * tickSizeFactor);

        auto tick = getTickShape (1.0f);
        g.fillPath (tick, tick.getTransformToScaleToFit (tickArea, true));
        // --- tick-sizing change ends here ---
    }

    if (hasSubMenu)
    {
        auto arrowH = 0.6f * getPopupMenuFont().getAscent();

        auto x = static_cast<float> (r.removeFromRight ((int) arrowH).getX());
        auto halfH = static_cast<float> (r.getCentreY());

        juce::Path path;
        path.startNewSubPath (x, halfH - arrowH * 0.5f);
        path.lineTo (x + arrowH * 0.6f, halfH);
        path.lineTo (x, halfH + arrowH * 0.5f);

        g.strokePath (path, juce::PathStrokeType (2.0f));
    }

    r.removeFromRight (3);
    g.drawFittedText (text, r, juce::Justification::centredLeft, 1);

    if (shortcutKeyText.isNotEmpty())
    {
        auto f2 = font;
        f2.setHeight (f2.getHeight() * 0.75f);
        f2.setHorizontalScale (0.95f);
        g.setFont (f2);

        g.drawText (shortcutKeyText, r, juce::Justification::centredRight, true);
    }
}

} // namespace spa::ui
