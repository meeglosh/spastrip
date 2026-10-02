#pragma once

#include "../SPAStripProcessor.h"
#include "Controls.h"
#include "FxPanelHeader.h"

namespace spa::ui
{

// The impulse's shaped envelope (as the engine will convolve it: decay,
// damping and start applied), with the pre-delay gap and START trim marked.
// Polls the processor on a timer while showing.
class ConvolveDisplay : public juce::Component,
                        private juce::Timer
{
public:
    explicit ConvolveDisplay (SPAStripProcessor&);
    ~ConvolveDisplay() override;

    void paint (juce::Graphics&) override;

private:
    void timerCallback() override;

    SPAStripProcessor& processor;
    // What was last painted (envelope bytes + the two parameters drawn from
    // the apvts), so the timer only repaints when something moved.
    std::array<float, dsp::FXChain::convEnvPoints> shown {};
    float shownPre = -1.0f, shownTrim = -1.0f;
    bool shownHas = false;
};

// Convolution tab: the factory IR picker (grouped by category), a file
// chooser + drag-and-drop for the user's own impulse, the shaped-envelope
// display and the shaping knobs.
class ConvolvePanel : public juce::Component,
                      public juce::FileDragAndDropTarget,
                      private juce::Timer
{
public:
    explicit ConvolvePanel (SPAStripProcessor&);
    ~ConvolvePanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray&, int, int) override;
    void fileDragExit (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray& files, int, int) override;

    static bool isAcceptedAudioFile (const juce::String& path);

    // "Factory" / "File" / "" -- what the IR label says about the source.
    juce::String getSourceTagForTest() const { return sourceTag; }
    juce::String getNameForTest() const { return irName; }
    // The factory picker's menu (ids 1.. = list() index + 1) and its handler,
    // exposed so the tests can drive it without a modal popup.
    juce::PopupMenu buildFactoryMenu() const;
    void chooseFactoryIR (int listIndex);
    void clearIR();
    bool loadFile (const juce::File&);

private:
    void timerCallback() override;
    void refreshLabels();
    void showFactoryMenu();
    void showFileChooser();

    SPAStripProcessor& processor;
    FxPanelHeader header;
    ConvolveDisplay display;
    juce::TextButton factoryButton { "Factory IR" }, fileButton { "Load file..." }, clearButton { "Clear" };
    Knob mix, predelay, start, decay, damping, width;
    std::unique_ptr<juce::FileChooser> fileChooser;
    juce::String irName, sourceTag, lastSource;
    juce::Rectangle<int> nameRect;
    bool dragHighlight = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ConvolvePanel)
};

} // namespace spa::ui
