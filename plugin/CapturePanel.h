#pragma once

#include "CaptureSettings.h"
#include "CaptureWorker.h"
#include "FileDragSource.h"
#include "vocalsynth/CaptureMode.h"
#include "vocalsynth/Take.h"
#include "vocalsynth/TakeRenderer.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace vocalsynth
{

class VocalSynthProcessor;

/** The "Capture" tab (third page beside "Voice" and "Test & Routing", docs/M3-DESIGN.md 14):
    status line (+ memory use), take list (newest first: number, start bar / time, length, note count,
    Grab / incomplete / audio truncated badges), a mini piano roll of the selected take in the selected
    version with its waveform strip, drag handles "MIDI" and "WAV", "Save as...", "New take",
    "Grab last [N bars / N s]", "Clear" (with confirmation), export options (version, quantize + strength +
    ends, bends, CC), capture timing offset (mode, Manual ms, fine ms, the Auto value in ms), capture and
    audio on/off, the free-mode gap, and the hint
    "Takes are not saved with the project - drag or save what you want to keep."

    Message thread only. While visible, a 10 Hz timer reads the processor's capture worker; the list and
    the preview repaint only when the take-list version, the selected take's rendering (options, offset)
    or the status text changed (the user found 30 Hz updates hectic). Never touches the audio thread's
    data: takes come from the worker's lock (immutable shared_ptr<const Take>). */
class CapturePanel final : public juce::Component,
                           private juce::Timer,
                           private juce::ListBoxModel
{
public:
    static constexpr int timerHz = 10;
    static constexpr int rowHeight = 26;

    explicit CapturePanel (VocalSynthProcessor& processor);
    ~CapturePanel() override;

    void paint (juce::Graphics& g) override;
    void resized() override;
    void visibilityChanged() override;

    //==============================================================================
    /** Status line, e.g. "Synced 128.0 BPM 4/4 - recording take 3 (bar 17)" (en dash), "Free - waiting for a
        note", "Free - recording take 4", "Capture off"; overflow counters appended only when non-zero.
        M3.1 (docs/M3.1-DESIGN.md 7): with a take limit "Synced 128.0 BPM 4/4 - take 3, bar 2/16 (bar 17)" /
        "- take 3 waiting for a note", "Free - recording take 4 (max 16 bars)"; Record mode: "Record mode -
        press Record (8 bars)" (recordBars), "Armed - starts when FL plays (8 bars)", "Starts at bar 17 -
        3 beats", "Recording bar 3/8 (take 5)". */
    static juce::String formatStatus (const CaptureStatus& status, bool captureEnabled, int openTakeNotes,
                                      int recordBars = recordBarsForIndex (defaultRecordBarsIndex));

    /** "Audio history 34.5 MB  ·  takes 1.2 MB (5)". */
    static juce::String formatMemory (const CaptureStatus& status);

    /** Row text: "Take 3 · Bar 17 · 4 bars · 12 notes" (synced) / "Take 4 · 14:03:22 · 7.5 s · 5 notes"
        (free), then the badges " · Rec" (Record take), " · Grab", " · incomplete", " · audio truncated". */
    static juce::String describeTake (const Take& take, const RenderedTake& rendered);

    /** "Grab last 8 bars" (synced) / "Grab last 30 s" (otherwise). */
    static juce::String grabButtonText (bool synced, int bars, int seconds);

    //==============================================================================
    // For the tests (and used by the controls)

    juce::String getStatusText() const;
    juce::String getMemoryText() const;
    juce::String getAutoOffsetText() const;
    juce::String getHintText() const;

    /** Number of take rows shown. */
    int getNumTakeRows() const noexcept { return static_cast<int> (shownTakes.size()); }
    juce::String getRowText (int row);

    /** Take number of the selected row, 0 = none. */
    int getSelectedTakeNumber() const noexcept { return selectedNumber; }
    void selectTake (int takeNumber);

    /** Drag actions: write the selected take as .mid (current options) / persist its WAV. */
    juce::File exportSelectedMidi();
    juce::File exportSelectedWav();

    FileDragSource& getMidiDragSource() noexcept { return midiDrag; }
    FileDragSource& getWavDragSource() noexcept { return wavDrag; }

    void pressNewTake();
    void pressGrab();
    void pressClear(); ///< asks first (confirmClear)
    /** M3.1 Record / Cancel button: Record (recordBars) when nothing is armed, else Cancel. */
    void pressRecord();
    juce::String getRecordButtonText() const { return recordButton.getButtonText(); }

    /** Clear confirmation: called with a callback that gets true for "Clear". Default: an async AlertWindow.
        Tests replace it. */
    std::function<void (std::function<void (bool)>)> confirmClear;

    /** Number of list / preview repaints the timer requested so far. */
    int getContentRepaintCount() const noexcept { return contentRepaints; }

    /** Runs one timer tick now (also while hidden). */
    void runTimerTickForTests() { update(); }

    /** Applies the CaptureState to the export option controls (also after a state load). */
    void refreshOptionControls();

private:
    // ListBoxModel
    int getNumRows() override { return getNumTakeRows(); }
    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override;
    void selectedRowsChanged (int lastRowSelected) override;

    void timerCallback() override { update(); }
    void update();
    void updateDragHandles();
    void updateGrabButton();
    void updateModeControls (const CaptureStatus& status);
    void changeState (const std::function<void (CaptureState&)>& change);
    void showSaveMenu();
    void saveMidiAs();
    void saveWavAs();

    std::shared_ptr<const Take> getSelectedTake() const;

    /** Rendering of a take with the current options (cached; re-rendered when options / offset change). */
    const RenderedTake& renderedFor (const Take& take);

    void paintPreview (juce::Graphics& g);

    VocalSynthProcessor& vsProcessor;
    CaptureWorker& worker;

    std::vector<std::shared_ptr<const Take>> shownTakes;
    uint32_t shownVersion = 0;
    bool hasShownVersion = false;
    int selectedNumber = 0;
    int contentRepaints = 0;
    CaptureStatus shownStatus;
    CaptureState shownState;
    bool shownSynced = false;
    bool shownRecordMode = false;
    bool hasShownMode = false;
    bool recordActive = false;            ///< the Record button shows Cancel
    double recordRequestedMs = -1.0e9;    ///< Record pressed, not yet in the worker status
    double cancelRequestedMs = -1.0e9;    ///< Cancel pressed, not yet in the worker status
    std::optional<RenderOptions> previewOptions; ///< options the preview was painted with

    struct CachedRender
    {
        int number = 0;
        const Take* take = nullptr;
        RenderOptions options;
        RenderedTake rendered;
    };

    std::vector<CachedRender> renderCache;

    // Left: status, list, preview, hint
    juce::Label statusLabel;
    juce::Label memoryLabel;
    juce::ListBox takeList { "Takes", this };
    juce::Rectangle<int> previewBounds;
    juce::Label hintLabel;

    // Right: drag handles, actions, options
    FileDragSource midiDrag;
    FileDragSource wavDrag;
    juce::TextButton saveAsButton { "Save as..." };
    juce::TextButton newTakeButton { "New take" };
    juce::TextButton grabButton { "Grab last 8 bars" };
    juce::ComboBox grabBarsBox;
    juce::ComboBox grabSecondsBox;
    juce::TextButton clearButton { "Clear" };

    // M3.1 take length / Record mode (docs/M3.1-DESIGN.md 7)
    juce::Label modeLabel { {}, "Mode" };
    juce::ComboBox modeBox;
    juce::Label maxLengthLabel { {}, "Max length" };
    juce::ComboBox maxLengthBox;
    juce::Label recordLengthLabel { {}, "Length" };
    juce::ComboBox recordLengthBox;
    juce::TextButton recordButton { "Record" };

    juce::Label versionLabel { {}, "Version" };
    juce::ComboBox versionBox;
    juce::Label bendsLabel { {}, "Bends" };
    juce::ComboBox bendsBox;
    juce::ToggleButton ccButton { "CC" };
    juce::Label quantizeLabel { {}, "Quantize" };
    juce::ComboBox quantizeBox;
    juce::Slider strengthSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxLeft };
    juce::ToggleButton quantizeEndsButton { "Ends" };

    juce::Label offsetLabel { {}, "Offset" };
    juce::ComboBox offsetModeBox;
    juce::Label autoOffsetLabel;
    juce::Label manualLabel { {}, "Manual" };
    juce::Slider offsetMsSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxLeft };
    juce::Label fineLabel { {}, "Fine" };
    juce::Slider fineOffsetSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxLeft };

    juce::ToggleButton captureButton { "Capture" };
    juce::ToggleButton audioButton { "Capture audio" };
    juce::Label gapLabel { {}, "Free gap" };
    juce::Slider gapSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxLeft };

    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> offsetModeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> offsetMsAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> fineOffsetAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> captureAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> audioAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> modeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> maxLengthAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> recordLengthAttachment;

    std::unique_ptr<juce::FileChooser> fileChooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CapturePanel)
};

} // namespace vocalsynth
