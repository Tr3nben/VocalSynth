#include "ui/SynthPanel.h"

#include "SynthParameters.h"
#include "ui/Theme.h"

namespace vocalsynth::ui
{

// Scaffold (docs/SYNTH-DESIGN.md 20.6): the preset bar and no controls yet. Agent E implements the page.

SynthPanel::SynthPanel (const SynthPanelContext& c)
    : context (c),
      presetBar (c.presets)
{
    addAndMakeVisible (presetBar);
}

SynthPanel::~SynthPanel() = default;

juce::Component* SynthPanel::findControlFor (const juce::String& parameterId)
{
    for (auto& control : controls)
        if (control->getComponentID() == parameterId)
            return control.get();

    return nullptr;
}

void SynthPanel::paint (juce::Graphics& g)
{
    g.fillAll (theme::background);
}

void SynthPanel::resized()
{
    presetBar.setBounds (getLocalBounds().removeFromTop (theme::presetBarHeight));
}

void SynthPanel::timerCallback()
{
    juce::ignoreUnused (context.expression.read());
}

} // namespace vocalsynth::ui
