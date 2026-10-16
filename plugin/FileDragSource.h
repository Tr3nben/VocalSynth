#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace vocalsynth
{

/** A labelled box that, when dragged, produces a file and starts an external file drag (drop it into an FL
    pattern / the playlist). TODO(C): move VocalSynthEditor::FileDragSource (M1, PluginEditor.*) here so the
    Test & Routing page and the Capture tab share it (docs/M3-DESIGN.md 14), and add:
    - setAvailable (false, "preparing...") greys the box and shows the reason instead of dragging;
    - the tooltip text is settable (Capture: mentions Shift = skip FL's import dialog).
    Message thread only. */
class FileDragSource final : public juce::Component,
                             public juce::SettableTooltipClient
{
public:
    FileDragSource (juce::String text, std::function<juce::File()> produceFile);

    /** available = false: no drag; reason is shown as the status text. */
    void setAvailable (bool available, const juce::String& reason = {});
    bool isAvailable() const noexcept { return available; }

    void paint (juce::Graphics& g) override;
    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;

private:
    juce::String text;
    std::function<juce::File()> produceFile;
    juce::String statusText;
    bool available = true;
    bool dragStartedThisGesture = false;
    bool isPressed = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FileDragSource)
};

} // namespace vocalsynth
