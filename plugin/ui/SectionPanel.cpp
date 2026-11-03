#include "ui/SectionPanel.h"

#include "ui/Theme.h"

namespace vocalsynth::ui
{

// Scaffold (docs/SYNTH-DESIGN.md 20.3): plain frame and title. Agent E implements the look.

SectionPanel::SectionPanel (const juce::String& t)
    : title (t)
{
}

SectionPanel::~SectionPanel() = default;

void SectionPanel::setHeaderComponent (juce::Component* component)
{
    header = component;

    if (header != nullptr)
        addAndMakeVisible (*header);

    resized();
}

juce::Rectangle<int> SectionPanel::getContentBounds() const
{
    return getLocalBounds().reduced (theme::sectionPadding).withTrimmedTop (theme::sectionTitleHeight);
}

void SectionPanel::paint (juce::Graphics& g)
{
    g.setColour (theme::panel);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), static_cast<float> (theme::cornerRadius));
    g.setColour (theme::text);
    g.setFont (theme::font (theme::fontTitle, true));
    g.drawText (title, getLocalBounds().reduced (theme::sectionPadding).removeFromTop (theme::sectionTitleHeight),
                juce::Justification::centredLeft);
}

void SectionPanel::resized()
{
    if (header != nullptr)
        header->setBounds (getLocalBounds()
                               .reduced (theme::sectionPadding)
                               .removeFromTop (theme::sectionTitleHeight)
                               .removeFromRight (80));
}

} // namespace vocalsynth::ui
