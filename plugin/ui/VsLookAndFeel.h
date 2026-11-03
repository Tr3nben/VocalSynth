#pragma once

#include "ui/Theme.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace vocalsynth::ui
{

/** The VocalSynth look (docs/SYNTH-DESIGN.md 20.2): vector-drawn, theme colours, no bitmaps (HiDPI-sharp at
    any scale). Set on the whole editor by the integration; the old pages keep their layout and only get
    these colours / shapes. Draws rotary sliders (arc track + value arc + pointer, modulation ring from the
    slider properties "modStart" / "modEnd" = doubles in normalised units 0..1, see Knob), linear sliders,
    buttons, toggle buttons, combo boxes, popup menus, tab buttons, labels. Owner: agent E. */
class VsLookAndFeel : public juce::LookAndFeel_V4
{
public:
    VsLookAndFeel();
    ~VsLookAndFeel() override;

    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPosProportional,
                           float rotaryStartAngle, float rotaryEndAngle, juce::Slider& slider) override;
};

} // namespace vocalsynth::ui
