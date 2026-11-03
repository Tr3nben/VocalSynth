#include "ui/ChoiceChip.h"

namespace vocalsynth::ui
{

// Scaffold (docs/SYNTH-DESIGN.md 20.3): a plain ComboBox / ToggleButton. Agent E implements the chip look.

ChoiceChip::ChoiceChip (juce::AudioProcessorValueTreeState& apvts, const juce::String& id)
    : parameterId (id)
{
    setComponentID (id);
    auto* parameter = apvts.getParameter (id);

    if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (parameter))
    {
        combo = std::make_unique<juce::ComboBox>();
        combo->addItemList (choice->choices, 1);
        combo->setComponentID (id);
        addAndMakeVisible (*combo);
        comboAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (apvts, id, *combo);
    }
    else if (parameter != nullptr)
    {
        toggle = std::make_unique<juce::ToggleButton> (parameter->getName (32));
        toggle->setComponentID (id);
        addAndMakeVisible (*toggle);
        buttonAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (apvts, id, *toggle);
    }
}

ChoiceChip::~ChoiceChip() = default;

void ChoiceChip::resized()
{
    if (combo != nullptr)
        combo->setBounds (getLocalBounds());

    if (toggle != nullptr)
        toggle->setBounds (getLocalBounds());
}

} // namespace vocalsynth::ui
