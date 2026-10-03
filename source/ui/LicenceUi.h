#pragma once

// Licensing UI for SPAStrip (licensing branch, only with the spa-licensing
// module). Mirrors SPASynth's LicenceOverlay, built on SPAStrip's own
// DialogOverlay (the one-at-a-time dimmed overlay every SPAStrip dialog uses,
// not a CallOutBox: the panel launches an async file chooser, which would
// dismiss a CallOutBox):
//  - LicenceDialog: hosts the shared spa::lic::LicensePanel, themed from Theme.h.
//  - LicenceBadge: the small "TRIAL: N DAYS" / "DEMO - ACTIVATE" chip at the
//    right end of the brand band; hidden when licensed.
#if SPASTRIP_HAS_SPA_LICENSING

#include <juce_gui_basics/juce_gui_basics.h>
#include <spa_licensing/spa_licensing.h>

#include "Dialogs.h"
#include "Theme.h"

namespace spa::ui
{
inline spa::lic::LicensePanel::Style licencePanelStyle()
{
    const auto& t = currentTheme();
    spa::lic::LicensePanel::Style s;
    s.background = t.panel.brighter (0.04f);   // the DialogOverlay card colour
    s.outline = t.outline;
    s.text = t.textPrimary;
    s.textDim = t.textSecondary;
    s.accent = t.accent;
    s.error = t.meterHot;
    s.good = t.meterGreen;
    s.field = t.display;
    s.accentSecondary = t.accentMod;
    s.buttonFace = t.knobFace;
    s.rule = t.outline;
    s.header = metrics::titleFont().withExtraKerningFactor (0.08f);
    s.cornerRadius = metrics::cornerRadius - 2.0f;
    return s;
}

class LicenceDialog : public DialogOverlay
{
public:
    LicenceDialog (spa::lic::LicenseController& controller, const juce::String& banner)
        : DialogOverlay ({ spa::lic::LicensePanel::preferredWidth, 400 }),
          panel (controller, licencePanelStyle())
    {
        // A plain look-and-feel for the panel: SPAStrip's own one sizes every
        // Label to the faceplate's caption font, which flattens the panel's
        // title/status hierarchy. Colours come from the Style + Theme.h.
        const auto& t = currentTheme();
        plain.setColour (juce::TextButton::buttonColourId, t.panel.brighter (0.12f));
        plain.setColour (juce::TextButton::textColourOffId, t.textPrimary);
        plain.setColour (juce::TextButton::textColourOnId, t.textPrimary);
        panel.setLookAndFeel (&plain);
        panel.setBanner (banner);
        panel.onCloseRequested = [this] { dismiss(); };
        for (auto* te : { &panel.getSerialEditor(), &panel.getMachineEditor(), &panel.getResponseEditor() })
            te->onEscapeKey = [this] { dismiss(); };
        addAndMakeVisible (panel);
        syncSize();
    }

    ~LicenceDialog() override { panel.setLookAndFeel (nullptr); }

    spa::lic::LicensePanel& getPanel() { return panel; }
    void setBanner (const juce::String& b) { panel.setBanner (b); syncSize(); }

    // The panel changes height with its state (offline page, messages).
    void childBoundsChanged (juce::Component* c) override
    {
        if (c == &panel && ! syncing) syncSize();
    }

private:
    void syncSize()
    {
        syncing = true;
        setCardSize ({ spa::lic::LicensePanel::preferredWidth, panel.getPreferredHeight() });
        layoutCard (getCardBounds());
        syncing = false;
    }
    void layoutCard (juce::Rectangle<int> card) override
    {
        if (panel.getBounds() != card)
            panel.setBounds (card);
    }

    juce::LookAndFeel_V4 plain;   // declared before the panel: outlives it
    spa::lic::LicensePanel panel;
    bool syncing = false;
};

class LicenceBadge : public juce::Button
{
public:
    LicenceBadge() : juce::Button ("licenceBadge")
    {
        setMouseClickGrabsKeyboardFocus (false);
        setWantsKeyboardFocus (false);
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    juce::String update (const spa::lic::LicenseState& st)
    {
        juce::String text;
        demo = st.demoActive();
        // Not started (editor never opened yet) reads as the full trial.
        if (! st.isLicensed())
        {
            const int d = st.trialDaysLeft();
            text = demo ? juce::String ("DEMO - ACTIVATE")
                        : "TRIAL: " + juce::String (d) + (d == 1 ? " DAY" : " DAYS");
        }
        setButtonText (text);
        setTooltip (text.isEmpty() ? juce::String() : juce::String ("Licence: activate SPAStrip or see your trial"));
        setVisible (text.isNotEmpty());
        repaint();
        return text;
    }

    static constexpr int width = 116, height = 18;

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        const auto& t = currentTheme();
        auto r = getLocalBounds().toFloat().reduced (0.5f);
        const auto ink = demo ? t.meterHot : t.accent;
        g.setColour (ink.withAlpha (down ? 0.30f : (over ? 0.22f : 0.12f)));
        g.fillRoundedRectangle (r, 3.0f);
        g.setColour (ink.withAlpha (0.75f));
        g.drawRoundedRectangle (r, 3.0f, 1.0f);
        g.setColour (ink.brighter (0.25f));
        g.setFont (metrics::smallFontBold());
        g.drawText (getButtonText(), getLocalBounds(), juce::Justification::centred, false);
    }

private:
    bool demo = false;
};
} // namespace spa::ui

#endif
