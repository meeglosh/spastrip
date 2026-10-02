#pragma once

#include "Theme.h"
#include "../params/ParameterRegistry.h"

namespace spa::ui
{

// A titled panel that builds its controls straight from the parameter
// registry for one section: rotary knobs for floats/ints, combo boxes for
// choices, pill toggles for bools. Every section of the synth gets a
// consistent, professional look with zero per-panel layout code — and new
// registry parameters appear automatically.
class SectionPanel : public juce::Component
{
public:
    // excludeKeys: parameter IDs to skip (when a bespoke control elsewhere
    // covers them). title empty = use the section name.
    // dense: OPT-IN, FXPanel only (1.0.25 FX-tab layout round) -- packs
    // controls at their own natural pixel width instead of a fixed 2-cell
    // grid (toggles pair up two-high in one column when consecutive;
    // combos take only the width their longest entry needs, per
    // comboTextFitsCellTest's own formula; knobs use a narrower fixed
    // column), so a section needs fewer rows and its FXPanel-owned display
    // gets a bigger share. false (the default, and the ONLY mode every
    // other caller of this class uses -- there are none today, but the
    // point is future ones stay unaffected) is the original fixed-grid
    // layout, untouched, verified pixel-identical by a whole-editor
    // snapshot diff outside the FX tab area.
    SectionPanel (juce::AudioProcessorValueTreeState& apvts, params::Section section,
                  const juce::String& title = {},
                  const juce::StringArray& excludeIDs = {},
                  bool drawFrame = true,
                  bool dense = false);

    void paint (juce::Graphics&) override;
    void resized() override;

    // Height needed for a given width (grid wraps).
    int heightForWidth (int width) const;

    // The control component for a registry paramID, plus its sibling label
    // if it has one (toggles draw their own text and have none) -- for
    // callers that need to wire up cross-param behaviour like dependent
    // dimming after the auto-built grid is constructed. Empty if not found.
    std::vector<juce::Component*> findControlComponents (const juce::String& paramID) const;

private:
    struct Control
    {
        std::unique_ptr<juce::Component> component;
        std::unique_ptr<juce::Label> label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> sliderAttachment;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> comboAttachment;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> buttonAttachment;
        bool wide = false;    // fixed-grid mode: combos + toggles span two cells
        bool isToggle = false;
        bool isCombo = false;
    };

    static constexpr int cellWidth = 66;
public:
    // Row height of a bare (FX tab) panel: knob + its caption.
    static constexpr int bareCellHeight = 78;
private:
    // Shares draw::sectionHeader()'s reserved title-row height (Theme.h) --
    // this panel is only ever built with drawFrame=false today (FXPanel owns
    // its own header instead), but keep the constant in sync in case a
    // future framed use appears.
    static constexpr int headerHeight = metrics::sectionHeaderHeight;

    int cellHeight = 72;   // compacted in bare (embedded) mode
    juce::String panelTitle;
    bool framed = true;
    bool dense = false;
    std::vector<Control> controls;

    // One control's placement within the dense packer's row/column plan.
    // x/width are real pixels (not cell multiples); stackHalf is 0 for a
    // full-height item, 1/2 for a toggle paired to share one column with
    // another toggle. Computed fresh from the CURRENT controls for a given
    // width -- controls never move, so nothing needs to be cached.
    struct DensePlacement
    {
        int controlIndex = -1;
        int row = 0;
        int x = 0;
        int width = 0;
        int stackHalf = 0;   // 0 = full height, 1 = top half, 2 = bottom half
    };

    // Shared by heightForWidth() and resized() in dense mode so the row
    // count they agree on can never drift apart. Pure function of the
    // controls + width; independent of the final row height (that's decided
    // afterwards from whatever vertical space the caller actually has, same
    // "shrink toward a floor, never below the label" rule as fixed-grid
    // mode).
    std::vector<DensePlacement> computeDensePlacements (int width) const;
    int denseToggleWidth (const juce::String& text) const;
    int denseComboWidth (const juce::String& longestChoice) const;
    int denseCaptionWidth (const juce::String& captionText) const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SectionPanel)
};

} // namespace spa::ui
