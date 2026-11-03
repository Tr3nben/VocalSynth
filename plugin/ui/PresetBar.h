#pragma once

#include "PresetManager.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace vocalsynth::ui
{

/** Preset bar (docs/SYNTH-DESIGN.md 20.5): [<] [name (* when modified) - click: menu by category] [>]
    [Save]. The menu lists the factory presets grouped by category, then "User" with the user presets, then
    "Init". Save asks for a name and category (AlertWindow, async) and calls
    PresetManager::saveUserPreset. Refreshes the name with a 10 Hz timer (modified flag). Component IDs:
    "presetPrev", "presetName", "presetNext", "presetSave". Message thread. Owner: agent E. */
class PresetBar : public juce::Component, private juce::Timer
{
public:
    explicit PresetBar (PresetManager& presets);
    ~PresetBar() override;

    void resized() override;

private:
    void timerCallback() override;

    PresetManager& presets;
    juce::TextButton previousButton { "<" };
    juce::TextButton nameButton;
    juce::TextButton nextButton { ">" };
    juce::TextButton saveButton { "Save" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PresetBar)
};

} // namespace vocalsynth::ui
