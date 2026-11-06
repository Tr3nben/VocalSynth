#include "ui/ChoiceChip.h"

#include "ui/Theme.h"

namespace vocalsynth::ui
{

ChoiceChip::ChoiceChip (juce::AudioProcessorValueTreeState& apvts, const juce::String& id)
    : ChoiceChip (apvts, id, {}, ToggleStyle::chip)
{
}

ChoiceChip::ChoiceChip (juce::AudioProcessorValueTreeState& apvts, const juce::String& id, const juce::String& text,
                        ToggleStyle toggleStyle)
    : parameterId (id)
{
    setComponentID (id);
    auto* parameter = apvts.getParameter (id);
    const auto name = parameter != nullptr ? parameter->getName (64) : id;

    if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (parameter))
    {
        combo = std::make_unique<juce::ComboBox>();
        combo->addItemList (choice->choices, 1);
        combo->setComponentID (id);
        combo->setScrollWheelEnabled (true);
        combo->setTooltip (name);
        addAndMakeVisible (*combo);
        comboAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (apvts, id, *combo);
        caption = text;
    }
    else if (parameter != nullptr)
    {
        toggle = std::make_unique<juce::ToggleButton> (text.isNotEmpty() ? text : name);
        toggle->setComponentID (id);
        toggle->getProperties().set ("vsStyle", toggleStyle == ToggleStyle::power ? "power" : "chip");
        toggle->setTooltip (name);
        addAndMakeVisible (*toggle);
        buttonAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (apvts, id, *toggle);
    }
}

ChoiceChip::~ChoiceChip() = default;

void ChoiceChip::setCaption (const juce::String& newCaption)
{
    caption = newCaption;
    resized();
    repaint();
}

void ChoiceChip::setTooltip (const juce::String& text)
{
    if (combo != nullptr)
        combo->setTooltip (text);

    if (toggle != nullptr)
        toggle->setTooltip (text);
}

juce::String ChoiceChip::getDisplayText() const
{
    if (combo != nullptr)
        return combo->getText();

    return toggle != nullptr ? toggle->getButtonText() : juce::String();
}

void ChoiceChip::paint (juce::Graphics& g)
{
    if (combo == nullptr || caption.isEmpty())
        return;

    g.setColour (theme::textDim);
    g.setFont (theme::font (theme::fontSmall));
    g.drawFittedText (caption, getLocalBounds().removeFromTop (theme::chipCaptionHeight).withTrimmedLeft (1),
                      juce::Justification::centredLeft, 1, 0.8f);
}

void ChoiceChip::resized()
{
    auto bounds = getLocalBounds();

    if (combo != nullptr)
    {
        if (caption.isNotEmpty())
            bounds.removeFromTop (theme::chipCaptionHeight);

        combo->setBounds (bounds);
    }

    if (toggle != nullptr)
        toggle->setBounds (bounds);
}

} // namespace vocalsynth::ui
