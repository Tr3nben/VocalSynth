#pragma once

#include "PluginProcessor.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

namespace vocalsynth
{

/** Placeholder UI for the M0 skeleton: product name, input level meter and the Mute Dry switch.
    Runs on the message thread and only reads atomics from the processor. */
class VocalSynthEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit VocalSynthEditor (VocalSynthProcessor& processor);
    ~VocalSynthEditor() override;

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;

    VocalSynthProcessor& vsProcessor;

    juce::ToggleButton muteDryButton { "Mute Dry" };
    juce::AudioProcessorValueTreeState::ButtonAttachment muteDryAttachment;

    juce::Rectangle<int> meterBounds;
    float displayedLevel = 0.0f; // linear gain, decays smoothly

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VocalSynthEditor)
};

} // namespace vocalsynth
