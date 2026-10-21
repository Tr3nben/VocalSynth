#pragma once

#include "CapturePanel.h"
#include "FileDragSource.h"
#include "PluginProcessor.h"
#include "TunerDisplayModel.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <memory>
#include <vector>

namespace vocalsynth
{

/** M2 UI (docs/M2-DESIGN.md 4.3) + M3 Capture tab (docs/M3-DESIGN.md 14), for FX and Instrument alike.
    Three pages:

    - "Voice" (default): tuner (note name in FL convention C5 = 60, cents needle, Hz, confidence bar +
      voiced LED; calmed by TunerDisplayModel), sounding note, the voice settings (Source, Voice Range,
      Algorithm, Key, Scale, Bend Mode, Bend Range, Stickiness, Gate, Min Note, Velocity Sens., Octave,
      Legato), an input meter with the gate line, and the latency / DSP load box (analysis latency,
      host buffer, estimated total, large-buffer warning, VocalSynth's own DSP load).
    - "Test & Routing": the M1 routing test build controls and diagnostics (docs/M1-DESIGN.md 4.6):
      test pattern, MIDI channel, tempo, CC, monitor tone, virtual MIDI, mute dry, sent counters,
      DSP load, block-size histogram, drag sources for the test clips.
    - "Capture" (M3): the CapturePanel (take list, drag handles for MIDI / WAV, export options).

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
        testAndRouting,
        capture
    };

    /** Shows one page and hides the others' controls (also used by the tests). */
    void showPage (Page page);
    Page getPage() const noexcept { return currentPage; }

    /** "VST3", "CLAP", ... as shown in the header (wrapperType_Undefined means CLAP, see M1-DESIGN 4.6). */
    static juce::String getFormatName (juce::AudioProcessor::WrapperType wrapperType);

    /** Note name as FL Studio's piano roll shows it (MIDI note 60 = C5, 69 = A5), "-" for an invalid note. */
    static juce::String getNoteName (int midiNote);

    /** "24.2 ms" etc. for the latency display. */
    static juce::String formatLatency (double milliseconds);

    /** Tuner frequency text: whole Hz below 1 kHz ("262 Hz"), else kHz with two decimals ("1.05 kHz"),
        "- Hz" for 0. */
    static juce::String formatTunerHz (float hz);

    /** Host buffers above this many samples get the amber large-buffer warning. */
    static constexpr int largeBufferSamples = 512;

    /** Buffer and total-latency texts of the Voice page. The total is an estimate of what the user hears:
        analysis + the audio interface's input buffer + its output buffer (one host block each; driver and
        converter extras not included). Routing through FL's MIDI port to another plugin can add about
        one more buffer, shown separately. bufferSamples = 0: no audio seen yet. */
    struct LatencyInfo
    {
        juce::String bufferText; ///< "Buffer 2048 smp = 46.4 ms"
        juce::String totalText;  ///< "Total ~ 139 ms (analysis + in/out buffer) ..." (U+2248, U+00B7 in the UI)
        bool largeBuffer = false; ///< bufferSamples > largeBufferSamples: show largeBufferHint()
    };

    static LatencyInfo describeLatency (double analysisMs, int bufferSamples, double sampleRate);

    /** "Large audio buffer adds latency - try 128 or 256 samples in FL's audio settings" */
    static juce::String largeBufferHint();

    /** "VocalSynth DSP: avg 2.1 % / peak 5.3 % of the block time" */
    static juce::String formatDspLoad (const DspLoadMeter::Reading& reading);

    /** Current calm tuner values (for the tests). */
    const TunerDisplayModel::State& getTunerState() const noexcept { return tunerModel.getState(); }

    /** Number of Voice-page meter repaints the timer requested so far (for the tests). */
    int getVoiceMeterRepaintCount() const noexcept { return voiceMeterRepaints; }

    /** Runs one display update tick now, as the timer does (for the tests). */
    void runTimerTickForTests() { timerCallback(); }

    /** The Capture tab (for the tests). */
    CapturePanel& getCapturePanel() noexcept { return capturePanel; }

    /** Header line under the plugin name: "Version 0.3.0 - M3 capture   |   Format: VST3" (en dash). */
    juce::String getVersionText() const;

private:
    /** Tuner: note name, Hz, cents needle (-50..+50), confidence bar and voiced LED. Painted from the
        TunerDisplayModel state the editor's timer hands it; repaints only the parts whose pixels change. */
    class TunerDisplay final : public juce::Component
    {
    public:
        /** active = false while source = Test Pattern (the analysis does not run, the tuner is greyed). */
        void setState (const TunerDisplayModel::State& state, bool active);
        void paint (juce::Graphics& g) override;
        void resized() override;

    private:
        /** What each region shows, quantised to what can be seen (pixels, fade steps). */
        struct NoteKey
        {
            int note = -1;
            bool segmented = false;
            int alphaStep = 0;
            bool operator== (const NoteKey&) const = default;
        };

        struct TextKey
        {
            int hzTenths = 0;
            int cents = 0;
            bool operator== (const TextKey&) const = default;
        };

        struct NeedleKey
        {
            int halfPixels = -1; ///< -1 = no needle
            int alphaStep = 0;
            bool inTune = false;
            bool operator== (const NeedleKey&) const = default;
        };

        struct BottomKey
        {
            bool led = false;
            int barPixels = 0;
            bool operator== (const BottomKey&) const = default;
        };

        NoteKey noteKey() const noexcept;
        TextKey textKey() const noexcept;
        NeedleKey needleKey() const noexcept;
        BottomKey bottomKey() const noexcept;
        float needleX (float cents) const noexcept;

        TunerDisplayModel::State shown;
        bool isActive = true;

        juce::Rectangle<int> noteArea, textArea, scaleArea, scaleLineArea, bottomArea, barArea;
    };

    void timerCallback() override;
    void updateStatusTexts();
    void updateVoiceTexts (const VoiceAnalysisSnapshot& snapshot, double elapsedMs);
    void updateLatencyTexts (const VoiceSettings& voiceSettings);
    void updateDspLoadTexts();
    std::array<int, 5> voiceMeterKey() const noexcept;
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
    juce::TextButton capturePageButton { "Capture" };

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
    juce::Label voiceHintLabel;
    juce::Label meterCaptionLabel;

    // Latency / DSP load box (below the dropdowns and sliders)
    juce::Label latencyLabel;       ///< "Analysis 21.9 ms (Tenor, 44.1 kHz) | Buffer 2048 smp = 46.4 ms"
    juce::Label totalLatencyLabel;  ///< "Total ~ ..." (amber with a large buffer)
    juce::Label bufferWarningLabel; ///< largeBufferHint() or empty
    juce::Label dspLoadVoiceLabel;  ///< formatDspLoad()
    juce::Rectangle<int> infoPanelBounds;

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
    juce::Label dspLoadTestLabel;
    juce::Label histogramSummaryLabel;

    FileDragSource midiDragSource;
    FileDragSource wavDragSource;

    //==============================================================================
    // Capture page (M3)
    CapturePanel capturePanel;

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
    float meterGateDb = params::defaultGateThresholdDb; // gateThreshold
    bool meterGateOpen = false;
    int voiceMeterRepaints = 0;
    BlockSizeHistogram::Snapshot histogram;

    TunerDisplayModel tunerModel;
    double lastTickMs = 0.0;     // juce::Time::getMillisecondCounterHiRes() of the previous update, 0 = none
    double sinceDspTextMs = 0.0; // the DSP load texts change at most every dspTextIntervalMs

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VocalSynthEditor)
};

} // namespace vocalsynth
