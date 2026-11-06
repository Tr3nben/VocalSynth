#include "ui/SectionPanel.h"

#include "ui/Theme.h"

namespace vocalsynth::ui
{

SectionPanel::SectionPanel (const juce::String& t)
    : title (t)
{
    setComponentID ("section:" + t);
}

SectionPanel::~SectionPanel() = default;

void SectionPanel::setSubtitle (const juce::String& s)
{
    subtitle = s;
    repaint();
}

void SectionPanel::setHeaderComponent (juce::Component* component)
{
    header = component;

    if (header != nullptr)
        addAndMakeVisible (*header);

    resized();
}

void SectionPanel::setHeaderWidth (int width)
{
    headerWidth = width;
    resized();
}

void SectionPanel::setPowerComponent (juce::Component* component)
{
    power = component;

    if (power != nullptr)
        addAndMakeVisible (*power);

    resized();
    repaint();
}

void SectionPanel::setActive (bool shouldBeActive)
{
    if (active != shouldBeActive)
    {
        active = shouldBeActive;
        repaint();
    }
}

juce::Rectangle<int> SectionPanel::getTitleRow() const
{
    return getLocalBounds().removeFromTop (titleBandHeight).reduced (theme::sectionPadding, 3);
}

juce::Rectangle<int> SectionPanel::getContentBounds() const
{
    return getLocalBounds().reduced (theme::sectionPadding).withTrimmedTop (titleBandHeight - 2);
}

void SectionPanel::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
    const auto corner = static_cast<float> (theme::cornerRadius);

    g.setColour (theme::panel);
    g.fillRoundedRectangle (bounds, corner);

    // Title band: lighter, rounded at the top only
    {
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (getLocalBounds().removeFromTop (titleBandHeight));
        g.setColour (theme::panelHeader);
        g.fillRoundedRectangle (bounds, corner);
    }

    g.setColour (theme::panelOutline);
    g.fillRect (juce::Rectangle<float> (bounds.getX(), static_cast<float> (titleBandHeight), bounds.getWidth(), 1.0f));
    g.drawRoundedRectangle (bounds, corner, 1.0f);

    auto row = getTitleRow();

    if (power != nullptr)
        row.removeFromLeft (row.getHeight() + 5);

    if (header != nullptr)
        row.removeFromRight (headerWidth + theme::gap);

    const auto titleFont = theme::titleFont();
    g.setFont (titleFont);
    g.setColour (active ? theme::text : theme::textDim);
    g.drawText (title, row, juce::Justification::centredLeft, true);

    if (subtitle.isNotEmpty())
    {
        const auto w = juce::GlyphArrangement::getStringWidthInt (titleFont, title);
        g.setFont (theme::font (theme::fontSmall).withExtraKerningFactor (0.06f));
        g.setColour (active ? theme::accent.withAlpha (0.8f) : theme::textDim.withAlpha (0.6f));
        g.drawText (subtitle, row.withTrimmedLeft (w + 8), juce::Justification::centredLeft, true);
    }
}

void SectionPanel::resized()
{
    const auto row = getTitleRow();

    if (power != nullptr)
        power->setBounds (row.withWidth (row.getHeight()));

    if (header != nullptr)
        header->setBounds (row.withTrimmedLeft (juce::jmax (0, row.getWidth() - headerWidth)));
}

} // namespace vocalsynth::ui
