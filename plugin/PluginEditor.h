#pragma once

#include "PluginProcessor.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace vocalsynth
{

/** M1 routing test build UI (docs/M1-DESIGN.md 4.6): test-pattern controls, diagnostics for the
    FL Studio checks T1-T7 (sent counters, block-size histogram, virtual port status, input channels,
    plugin format) and drag sources for the test clips.
    Runs on the message thread; reads the processor only through atomics/accessors from a Timer. */
class VocalSynthEditor final : public juce::AudioProcessorEditor,
                               public juce::DragAndDropContainer,
                               private juce::Timer
{
public:
    explicit VocalSynthEditor (VocalSynthProcessor& processor);
    ~VocalSynthEditor() override;

    void paint (juce::Graphics& g) override;
    void resized() override;

    /** "VST3", "CLAP", ... as shown in the header (wrapperType_Undefined means CLAP, see 4.6). */
    static juce::String getFormatName (juce::AudioProcessor::WrapperType wrapperType);

private:
    /** A labelled box that, when dragged, exports a file and starts an external file drag
        (drop it into an FL pattern or the playlist). */
    class FileDragSource final : public juce::Component,
                                 public juce::SettableTooltipClient
    {
    public:
        FileDragSource (juce::String text, std::function<juce::File()> exportFile);

        void paint (juce::Graphics& g) override;
        void mouseDown (const juce::MouseEvent& e) override;
        void mouseDrag (const juce::MouseEvent& e) override;
        void mouseUp (const juce::MouseEvent& e) override;

    private:
        juce::String text;
        std::function<juce::File()> exportFile;
        juce::String statusText;
        bool dragStartedThisGesture = false;
        bool isPressed = false;
    };

    void timerCallback() override;
    void updateStatusTexts();

    VocalSynthProcessor& vsProcessor;
    const juce::String formatName;

    // Controls
    juce::Label patternLabel { {}, "Test pattern" };
    juce::ComboBox patternBox;
    juce::Label channelLabel { {}, "MIDI channel" };
    juce::Slider channelSlider { juce::Slider::IncDecButtons, juce::Slider::TextBoxLeft };
    juce::Label tempoLabel { {}, "Test tempo (BPM)" };
    juce::Slider tempoSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxLeft };
    juce::Label ccLabel { {}, "CC number" };
    juce::Slider ccSlider { juce::Slider::IncDecButtons, juce::Slider::TextBoxLeft };
    juce::ToggleButton monitorToneButton { "Monitor Tone" };
    juce::ToggleButton virtualMidiButton { "Virtual MIDI Out" };
    juce::Label virtualMidiStatusLabel;
    juce::ToggleButton muteDryButton { "Mute Dry" };
    juce::TextButton restartButton { "Restart pattern" };
    juce::TextButton resetHistogramButton { "Reset" };

    // Displays
    juce::Label inputChannelsLabel;
    juce::Label testNoteLabel;
    juce::Label sampleRateLabel;
    juce::Label sentCountsLabel;
    juce::Label histogramSummaryLabel;

    FileDragSource midiDragSource;
    FileDragSource wavDragSource;

    // Created after the combo box has its items, so the attachment can select the current one
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> patternAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> channelAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> tempoAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> ccAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> monitorToneAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> virtualMidiAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> muteDryAttachment;

    juce::TooltipWindow tooltipWindow { this, 600 };

    juce::Rectangle<int> meterBounds;
    juce::Rectangle<int> histogramBounds;
    float displayedLevel = 0.0f; // linear gain, decays smoothly
    BlockSizeHistogram::Snapshot histogram;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VocalSynthEditor)
};

} // namespace vocalsynth
