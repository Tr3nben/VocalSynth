#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace vocalsynth::params
{

// Parameter IDs are PERMANENT: renaming one breaks saved projects and automation.
// Add new parameters (with a new version hint) instead of repurposing old ones.
inline const juce::ParameterID muteDry { "muteDry", 1 };

inline juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<juce::AudioParameterBool> (
        muteDry, "Mute Dry", false, juce::AudioParameterBoolAttributes().withLabel ("")));

    return layout;
}

} // namespace vocalsynth::params
