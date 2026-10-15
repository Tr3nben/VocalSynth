#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace vocalsynth::params
{

// Parameter IDs are PERMANENT: renaming one breaks saved projects and automation.
// Add new parameters (with a new version hint) instead of repurposing old ones.
inline const juce::ParameterID muteDry { "muteDry", 1 };

// M1 routing test build (docs/M1-DESIGN.md)
inline const juce::ParameterID testPattern { "testPattern", 1 };
inline const juce::ParameterID midiChannel { "midiChannel", 1 };
inline const juce::ParameterID testTempo { "testTempo", 1 };
inline const juce::ParameterID ccNumber { "ccNumber", 1 };
inline const juce::ParameterID monitorTone { "monitorTone", 1 };
inline const juce::ParameterID virtualMidiOut { "virtualMidiOut", 1 };

/** Choice index == static_cast<int> (vocalsynth::TestPattern). Only append new entries at the end. */
inline const juce::StringArray testPatternChoices { "Off", "Notes", "Pitch Bend", "CC Sweep", "All" };

inline constexpr float minTestTempo = 60.0f;
inline constexpr float maxTestTempo = 200.0f;
inline constexpr float defaultTestTempo = 120.0f;

inline juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<juce::AudioParameterBool> (
        muteDry, "Mute Dry", false, juce::AudioParameterBoolAttributes().withLabel ("")));

    layout.add (std::make_unique<juce::AudioParameterChoice> (testPattern, "Test Pattern", testPatternChoices, 0));

    layout.add (std::make_unique<juce::AudioParameterInt> (midiChannel, "MIDI Channel", 1, 16, 1));

    // Continuous on purpose (interval 0): also gives clap-validator a non-stepped parameter
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        testTempo, "Test Tempo", juce::NormalisableRange<float> (minTestTempo, maxTestTempo), defaultTestTempo,
        juce::AudioParameterFloatAttributes()
            .withLabel ("BPM")
            .withStringFromValueFunction ([] (float v, int) { return juce::String (v, 1); })));

    layout.add (std::make_unique<juce::AudioParameterInt> (ccNumber, "CC Number", 1, 119, 1));

    layout.add (std::make_unique<juce::AudioParameterBool> (monitorTone, "Monitor Tone", false));

    layout.add (std::make_unique<juce::AudioParameterBool> (virtualMidiOut, "Virtual MIDI Out", false));

    return layout;
}

} // namespace vocalsynth::params
