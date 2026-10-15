#pragma once

#include "PluginProcessor.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <vector>

namespace vocalsynth
{

/** M2 UI (docs/M2-DESIGN.md 4.3), for FX and Instrument alike. Two pages:

    - "Voice" (default): tuner (note name in FL convention C5 = 60, cents needle, Hz, confidence bar +
      voiced LED), sounding note, analysis latency in ms, the voice settings (Source, Voice Range,
      Algorithm, Key, Scale, Bend Mode, Bend Range, Stickiness, Gate, Min Note, Velocity Sens., Octave,
      Legato) and an input meter with the gate line.
    - "Test & Routing": the M1 routing test build controls and diagnostics (docs/M1-DESIGN.md 4.6):
      test pattern, MIDI channel, tempo, CC, monitor tone, virtual MIDI, mute dry, sent counters,
      block-size histogram, drag sources for the test clips.

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

    static constexpr int defaultWidth = 940;
    static constexpr int defaultHeight = 470;

    enum class Page
    {
        voice,
        testAndRouting
    };

    /** Shows one page and hides the other's controls (also used by the tests). */
    void showPage (Page page);
    Page getPage() const noexcept { return currentPage; }

    /** "VST3", "CLAP", ... as shown in the header (wrapperType_Undefined means CLAP, see M1-DESIGN 4.6). */
    static juce::String getFormatName (juce::AudioProcessor::WrapperType wrapperType);

    /** Note name as FL Studio's piano roll shows it (MIDI note 60 = C5, 69 = A5), "-" for an invalid note. */
    static juce::String getNoteName (int midiNote);

    /** "24.2 ms" etc. for the latency display. */
    static juce::String formatLatency (double milliseconds);

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

    /** Tuner: note name, Hz, cents needle (-50..+50), confidence bar and voiced LED. Painted from the
        last snapshot the editor's timer handed it. */
    class TunerDisplay final : public juce::Component
    {
    public:
        /** active = false while source = Test Pattern (the analysis does not run, the tuner is greyed). */
        void setSnapshot (const VoiceAnalysisSnapshot& snapshot, bool active);
        void paint (juce::Graphics& g) override;

    private:
        VoiceAnalysisSnapshot shown;
        bool isActive = true;
    };

    void timerCallback() override;
    void updateStatusTexts();
    void updateVoiceTexts (const VoiceAnalysisSnapshot& snapshot);
    void layoutVoicePage (juce::Rectangle<int> area);
    void layoutTestPage (juce::Rectangle<int> area);
    void paintInputMeter (juce::Graphics& g);
    void paintHistogram (juce::Graphics& g);

    VocalSynthProcessor& vsProcessor;
    const juce::String formatName;
    Page currentPage = Page::voice;

    // Page switch
    juce::TextButton voicePageButton { "Voice" };
    juce::TextButton testPageButton { "Test & Routing" };

    //==============================================================================
    // Voice page
    TunerDisplay tuner;
    juce::Label sourceLabel { {}, "Source" };
    juce::ComboBox sourceBox;
    juce::Label voiceRangeLabel { {}, "Voice Range" };
    juce::ComboBox voiceRangeBox;
    juce::Label algorithmLabel { {}, "Algorithm" };
    juce::ComboBox algorithmBox;
    juce::Label keyLabel { {}, "Key" };
    juce::ComboBox keyBox;
    juce::Label scaleLabel { {}, "Scale" };
    juce::ComboBox scaleBox;
    juce::Label bendModeLabel { {}, "Bend Mode" };
    juce::ComboBox bendModeBox;
    juce::Label bendRangeLabel { {}, "Bend Range" };
    juce::ComboBox bendRangeBox;

    juce::Label stickinessLabel { {}, "Stickiness" };
    juce::Slider stickinessSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxLeft };
    juce::Label gateLabel { {}, "Gate" };
    juce::Slider gateSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxLeft };
    juce::Label minNoteLabel { {}, "Min Note" };
    juce::Slider minNoteSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxLeft };
    juce::Label velocityLabel { {}, "Velocity Sens." };
    juce::Slider velocitySlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxLeft };
    juce::Label octaveLabel { {}, "Octave" };
    juce::Slider octaveSlider { juce::Slider::IncDecButtons, juce::Slider::TextBoxLeft };
    juce::ToggleButton legatoButton { "Legato" };

    juce::Label soundingNoteLabel;
    juce::Label latencyLabel;
    juce::Label voiceHintLabel;
    juce::Label meterCaptionLabel;

    //==============================================================================
    // Test & Routing page (M1 controls)
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

    juce::Label inputChannelsLabel;
    juce::Label testNoteLabel;
    juce::Label sampleRateLabel;
    juce::Label sentCountsLabel;
    juce::Label histogramSummaryLabel;

    FileDragSource midiDragSource;
    FileDragSource wavDragSource;

    /** Components of each page (shown/hidden by showPage()). */
    std::vector<juce::Component*> voicePageComponents;
    std::vector<juce::Component*> testPageComponents;

    // Created after the combo boxes have their items, so the attachments can select the current one
    using ComboBoxAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;
    std::vector<std::unique_ptr<ComboBoxAttachment>> comboBoxAttachments;
    std::vector<std::unique_ptr<SliderAttachment>> sliderAttachments;
    std::vector<std::unique_ptr<ButtonAttachment>> buttonAttachments;

    juce::TooltipWindow tooltipWindow { this, 600 };

    juce::Rectangle<int> voiceMeterBounds; ///< input meter with gate line (Voice page)
    juce::Rectangle<int> testMeterBounds;  ///< input peak meter (Test & Routing page)
    juce::Rectangle<int> histogramBounds;
    float displayedLevel = 0.0f; // linear peak gain, decays smoothly
    float meterRmsDb = -100.0f;  // analysis RMS (gate input) of the newest snapshot
    float meterGateDb = -45.0f;  // gateThreshold
    bool meterGateOpen = false;
    BlockSizeHistogram::Snapshot histogram;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VocalSynthEditor)
};

} // namespace vocalsynth
