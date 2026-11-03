#pragma once

#include "vocalsynth/synth/Wavetable.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace vocalsynth::ui
{

/** 2D view of one wavetable frame (docs/SYNTH-DESIGN.md 20.4): draws mip level 0 of the frame at the current
    position (interpolated like the oscillator, decimated to the view width) as a path, plus a position bar
    with a marker. The path is cached and rebuilt only when the table or the position changes by more than
    0.5 %; repaint at most 30 Hz (owner's timer). No table: a flat line and "Loading...". Message thread;
    the table is shared (std::shared_ptr from WavetableStore::getSlot), never the audio thread's pointer.
    Owner: agent E. */
class WavetableView : public juce::Component
{
public:
    WavetableView();
    ~WavetableView() override;

    void setTable (std::shared_ptr<const synth::Wavetable> table);
    void setPosition (float position0to1);

    float getPosition() const noexcept { return position; }

    void paint (juce::Graphics& g) override;

private:
    std::shared_ptr<const synth::Wavetable> table;
    float position = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WavetableView)
};

} // namespace vocalsynth::ui
