#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace vocalsynth
{

/** A labelled box that, when dragged, produces a file and starts an external file drag (drop it into an FL
    pattern / the playlist). M1 behaviour (formerly VocalSynthEditor::FileDragSource), shared by the
    Test & Routing page and the Capture tab (docs/M3-DESIGN.md 14):
    - the file is produced once per drag gesture (file I/O), then handed to
      juce::DragAndDropContainer::performExternalDragDropOfFiles;
    - setAvailable (false, "preparing...") greys the box and shows the reason instead of dragging;
    - the tooltip text is settable (Capture: mentions Shift = skip FL's import dialog).
    Message thread only. */
class FileDragSource final : public juce::Component,
                             public juce::SettableTooltipClient
{
public:
    static constexpr int dragThresholdPixels = 4;

    FileDragSource (juce::String text, std::function<juce::File()> produceFile);

    /** available = false: no drag; reason is shown as the status text. Repaints only on a change. */
    void setAvailable (bool available, const juce::String& reason = {});
    bool isAvailable() const noexcept { return available; }

    /** Second line when available and nothing was dragged yet (default "drag me into FL Studio"). */
    void setIdleText (const juce::String& text);

    /** The status line as painted (for the tests). */
    juce::String getStatusText() const;

    void paint (juce::Graphics& g) override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;

private:
    juce::String text;
    std::function<juce::File()> produceFile;
    juce::String statusText;      ///< result of the last drag (file name / failure)
    juce::String unavailableText; ///< reason while not available
    juce::String idleText { "drag me into FL Studio" };
    bool available = true;
    bool dragStartedThisGesture = false;
    bool isPressed = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FileDragSource)
};

} // namespace vocalsynth
