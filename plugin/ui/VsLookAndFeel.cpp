#include "ui/VsLookAndFeel.h"

namespace vocalsynth::ui
{

// Scaffold (docs/SYNTH-DESIGN.md 20.2): theme colours on the V4 look, default drawing. Agent E implements.

VsLookAndFeel::VsLookAndFeel()
{
    setColour (juce::ResizableWindow::backgroundColourId, theme::background);
    setColour (juce::Slider::rotarySliderFillColourId, theme::accent);
    setColour (juce::Slider::rotarySliderOutlineColourId, theme::knobTrack);
    setColour (juce::Label::textColourId, theme::text);
}

VsLookAndFeel::~VsLookAndFeel() = default;

void VsLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                                      float sliderPosProportional, float rotaryStartAngle, float rotaryEndAngle,
                                      juce::Slider& slider)
{
    LookAndFeel_V4::drawRotarySlider (g, x, y, width, height, sliderPosProportional, rotaryStartAngle,
                                      rotaryEndAngle, slider);
}

} // namespace vocalsynth::ui
