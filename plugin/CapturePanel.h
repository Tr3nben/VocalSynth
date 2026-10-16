#pragma once

#include "FileDragSource.h"
#include "vocalsynth/Take.h"
#include "vocalsynth/TakeRenderer.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <vector>

namespace vocalsynth
{

class VocalSynthProcessor;

/** The "Capture" tab (third page beside "Voice" and "Test & Routing", docs/M3-DESIGN.md 14):
    status line, take list (newest first: number, start bar / time, length, note count, Grab / incomplete
    badges, mini piano roll of the selected version, waveform strip with audio), drag handles "MIDI" and
    "WAV", "Save as...", "New take", "Grab last [N bars / N s]", "Clear" (with confirmation), export options,
    capture timing offset (mode + ms, Auto value in ms), audio on/off, and the hint
    "Takes are not saved with the project - drag or save what you want to keep."

    Message thread only. Reads the processor's capture worker from a Timer at <= 10 Hz and repaints only
    when the take-list version or the status text changed. Never touches the audio thread's data. */
class CapturePanel final : public juce::Component,
                           private juce::Timer
{
public:
    static constexpr int timerHz = 10;

    explicit CapturePanel (VocalSynthProcessor& processor);
    ~CapturePanel() override;

    void paint (juce::Graphics& g) override;
    void resized() override;

    /** Status line as shown, e.g. "Synced 128.0 BPM 4/4 - recording take 3 (bar 17)" (for the tests). */
    juce::String getStatusText() const;

    /** Number of take rows shown (for the tests). */
    int getNumTakeRows() const noexcept { return static_cast<int> (shownTakes.size()); }

    /** Runs one timer tick now (for the tests). */
    void runTimerTickForTests() { timerCallback(); }

private:
    void timerCallback() override;

    VocalSynthProcessor& vsProcessor;
    std::vector<std::shared_ptr<const Take>> shownTakes;
    uint32_t shownVersion = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CapturePanel)
};

} // namespace vocalsynth
