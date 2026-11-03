#include "ui/PresetBar.h"

#include "ui/Theme.h"

namespace vocalsynth::ui
{

// Scaffold (docs/SYNTH-DESIGN.md 20.5): buttons and the name, no menu / save dialog. Agent E implements.

PresetBar::PresetBar (PresetManager& p)
    : presets (p)
{
    previousButton.setComponentID ("presetPrev");
    nameButton.setComponentID ("presetName");
    nextButton.setComponentID ("presetNext");
    saveButton.setComponentID ("presetSave");
    previousButton.onClick = [this] { presets.loadPrevious(); };
    nextButton.onClick = [this] { presets.loadNext(); };

    for (auto* b : { &previousButton, &nameButton, &nextButton, &saveButton })
        addAndMakeVisible (*b);

    timerCallback();
}

PresetBar::~PresetBar() = default;

void PresetBar::resized()
{
    auto bounds = getLocalBounds();
    previousButton.setBounds (bounds.removeFromLeft (theme::presetBarHeight));
    saveButton.setBounds (bounds.removeFromRight (60));
    nextButton.setBounds (bounds.removeFromRight (theme::presetBarHeight));
    nameButton.setBounds (bounds);
}

void PresetBar::timerCallback()
{
    nameButton.setButtonText (presets.getCurrentName() + (presets.isModified() ? " *" : ""));
}

} // namespace vocalsynth::ui
