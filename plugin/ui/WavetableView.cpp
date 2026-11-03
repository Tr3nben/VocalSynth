#include "ui/WavetableView.h"

#include "ui/Theme.h"

namespace vocalsynth::ui
{

// Scaffold (docs/SYNTH-DESIGN.md 20.4): background and name only. Agent E implements the frame path.

WavetableView::WavetableView() = default;
WavetableView::~WavetableView() = default;

void WavetableView::setTable (std::shared_ptr<const synth::Wavetable> newTable)
{
    table = std::move (newTable);
    repaint();
}

void WavetableView::setPosition (float newPosition)
{
    position = juce::jlimit (0.0f, 1.0f, newPosition);
    repaint();
}

void WavetableView::paint (juce::Graphics& g)
{
    g.fillAll (theme::background);
    g.setColour (theme::textDim);
    g.setFont (theme::font (theme::fontSmall));
    g.drawText (table != nullptr ? juce::String (table->getName()) : juce::String ("Loading..."), getLocalBounds(),
                juce::Justification::centred);
}

} // namespace vocalsynth::ui
