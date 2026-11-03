#include "ui/Knob.h"

#include "ui/Theme.h"

namespace vocalsynth::ui
{

// Scaffold (docs/SYNTH-DESIGN.md 20.3): slider + label + attachment, simple layout. Agent E implements.

Knob::Knob (juce::AudioProcessorValueTreeState& apvts, const juce::String& id, const juce::String& labelText)
    : parameterId (id)
{
    setComponentID (id);
    slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, theme::knobSize, theme::knobValueHeight);
    slider.setComponentID (id);
    label.setText (labelText, juce::dontSendNotification);
    label.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (label);
    addAndMakeVisible (slider);

    if (apvts.getParameter (id) != nullptr)
        attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (apvts, id, slider);
}

Knob::~Knob() = default;

void Knob::setModulationRange (juce::Range<double> normalisedRange)
{
    slider.getProperties().set ("modStart", normalisedRange.getStart());
    slider.getProperties().set ("modEnd", normalisedRange.getEnd());
    repaint();
}

void Knob::resized()
{
    auto bounds = getLocalBounds();
    label.setBounds (bounds.removeFromTop (theme::knobLabelHeight));
    slider.setBounds (bounds);
}

} // namespace vocalsynth::ui
