#pragma once

#include "vocalsynth/VoiceSettings.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <cstddef>

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

// M2 pitch -> MIDI (docs/M2-SPEC.md, docs/M2-DESIGN.md 4). All version 1.
inline const juce::ParameterID source { "source", 1 };
inline const juce::ParameterID voiceRange { "voiceRange", 1 };
inline const juce::ParameterID pitchAlgorithm { "pitchAlgorithm", 1 };
inline const juce::ParameterID key { "key", 1 };
inline const juce::ParameterID scale { "scale", 1 };
inline const juce::ParameterID stickiness { "stickiness", 1 };
inline const juce::ParameterID gateThreshold { "gateThreshold", 1 };
inline const juce::ParameterID minNoteLength { "minNoteLength", 1 };
inline const juce::ParameterID octaveShift { "octaveShift", 1 };
inline const juce::ParameterID bendMode { "bendMode", 1 };
inline const juce::ParameterID bendRange { "bendRange", 1 };
inline const juce::ParameterID velocitySensitivity { "velocitySensitivity", 1 };
inline const juce::ParameterID legato { "legato", 1 };

/** Choice index == static_cast<int> (vocalsynth::TestPattern). Only append new entries at the end. */
inline const juce::StringArray testPatternChoices { "Off", "Notes", "Pitch Bend", "CC Sweep", "All" };

/** "source" choice: what drives the MIDI output. Index == static_cast<int> (Source). */
enum class Source
{
    voice = 0,
    testPattern
};

inline constexpr int numSources = 2;

namespace detail
{
/** True if every entry of a fixed-size name table is set (catches a too-short initialiser list:
    std::array value-initialises the missing entries to nullptr). */
template <std::size_t N>
constexpr bool allNamed (const std::array<const char*, N>& names) noexcept
{
    for (const auto* name : names)
        if (name == nullptr)
            return false;

    return true;
}

/** Exact float equality for the default checks below without tripping -Wfloat-equal. */
constexpr bool sameValue (float a, float b) noexcept { return ! (a < b) && ! (b < a); }

/** Names are UTF-8 (the bend ranges contain U+00B1). */
template <std::size_t N>
juce::StringArray toStringArray (const std::array<const char*, N>& names)
{
    juce::StringArray result;

    for (const auto* name : names)
        result.add (juce::String (juce::CharPointer_UTF8 (name)));

    return result;
}
} // namespace detail

// Choice names. The array sizes are the core enum counts (vocalsynth/VoiceSettings.h), so a missing or
// extra name is a compile error: choice index == static_cast<int> of the core enum. Only append at the end.
inline constexpr std::array<const char*, numSources> sourceNames { "Voice", "Test Pattern" };
inline constexpr std::array<const char*, numVoiceRanges> voiceRangeNames { "Bass",    "Baritone", "Tenor", "Alto",
                                                                           "Soprano", "Whistle",  "Wide" };
inline constexpr std::array<const char*, numPitchAlgorithms> pitchAlgorithmNames { "YIN", "MPM" };
inline constexpr std::array<const char*, 12> keyNames { "C",  "C#", "D",  "D#", "E",  "F",
                                                        "F#", "G",  "G#", "A",  "A#", "B" };
inline constexpr std::array<const char*, numScales> scaleNames { "Chromatic",        "Major",
                                                                 "Natural Minor",    "Harmonic Minor",
                                                                 "Major Pentatonic", "Minor Pentatonic",
                                                                 "Blues",            "Dorian",
                                                                 "Mixolydian" };
inline constexpr std::array<const char*, numBendModes> bendModeNames { "Off", "Anchored", "True" };
// U+00B1 PLUS-MINUS SIGN as UTF-8; the literal is split so the hex escape does not swallow the digit
inline constexpr std::array<const char*, numBendRanges> bendRangeNames {
    "\xc2\xb1" "2", "\xc2\xb1" "12", "\xc2\xb1" "24", "\xc2\xb1" "48"
};

static_assert (detail::allNamed (sourceNames) && detail::allNamed (voiceRangeNames)
               && detail::allNamed (pitchAlgorithmNames) && detail::allNamed (keyNames)
               && detail::allNamed (scaleNames) && detail::allNamed (bendModeNames)
               && detail::allNamed (bendRangeNames));

inline const juce::StringArray sourceChoices = detail::toStringArray (sourceNames);
inline const juce::StringArray voiceRangeChoices = detail::toStringArray (voiceRangeNames);
inline const juce::StringArray pitchAlgorithmChoices = detail::toStringArray (pitchAlgorithmNames);
inline const juce::StringArray keyChoices = detail::toStringArray (keyNames);
inline const juce::StringArray scaleChoices = detail::toStringArray (scaleNames);
inline const juce::StringArray bendModeChoices = detail::toStringArray (bendModeNames);
inline const juce::StringArray bendRangeChoices = detail::toStringArray (bendRangeNames);

// Defaults (docs/M2-SPEC.md). They must equal the core VoiceSettings defaults, checked below.
inline constexpr int defaultSourceIndex = static_cast<int> (Source::voice);
inline constexpr int defaultVoiceRangeIndex = static_cast<int> (VoiceRange::tenor);
inline constexpr int defaultPitchAlgorithmIndex = static_cast<int> (PitchAlgorithm::mpm); // decided by vs-eval
inline constexpr int defaultKeyIndex = 0;                                                 // C
inline constexpr int defaultScaleIndex = static_cast<int> (Scale::chromatic);
inline constexpr float defaultStickinessPercent = 50.0f;
inline constexpr float defaultGateThresholdDb = -45.0f;
inline constexpr float defaultMinNoteLengthMs = 60.0f;
inline constexpr int defaultOctaveShift = 0;
inline constexpr int defaultBendModeIndex = static_cast<int> (BendMode::off);
inline constexpr int defaultBendRangeIndex = static_cast<int> (BendRange::semitones2);
inline constexpr float defaultVelocitySensitivityPercent = 70.0f;
inline constexpr bool defaultLegato = false;

static_assert (static_cast<int> (VoiceSettings {}.range) == defaultVoiceRangeIndex);
static_assert (static_cast<int> (VoiceSettings {}.algorithm) == defaultPitchAlgorithmIndex);
static_assert (VoiceSettings {}.key == defaultKeyIndex);
static_assert (static_cast<int> (VoiceSettings {}.scale) == defaultScaleIndex);
static_assert (detail::sameValue (VoiceSettings {}.stickinessPercent, defaultStickinessPercent));
static_assert (detail::sameValue (VoiceSettings {}.gateThresholdDb, defaultGateThresholdDb));
static_assert (detail::sameValue (VoiceSettings {}.minNoteLengthMs, defaultMinNoteLengthMs));
static_assert (VoiceSettings {}.octaveShift == defaultOctaveShift);
static_assert (static_cast<int> (VoiceSettings {}.bendMode) == defaultBendModeIndex);
static_assert (static_cast<int> (VoiceSettings {}.bendRange) == defaultBendRangeIndex);
static_assert (detail::sameValue (VoiceSettings {}.velocitySensitivityPercent, defaultVelocitySensitivityPercent));
static_assert (VoiceSettings {}.legato == defaultLegato);

/** Bend Range text -> choice index. Accepts the exact choice names and anything that carries the semitone
    number ("12", "+-12", the UTF-8 name read as Latin-1, ...): CLAP hosts pass UTF-8 text, which the
    clap-juce-extensions wrapper turns into a juce::String byte by byte, so U+00B1 "12" arrives as
    U+00C2 U+00B1 "12" and an exact match alone would map it to index 0 (found by clap-validator's
    param-conversions test). Unknown text -> 0 (+-2), as JUCE's default does. */
inline int bendRangeIndexFromText (const juce::String& text)
{
    if (const auto exact = bendRangeChoices.indexOf (text.trim()); exact >= 0)
        return exact;

    const auto digits = text.retainCharacters ("0123456789");

    if (digits.isEmpty())
        return 0;

    const auto semitones = digits.getIntValue();

    for (int i = 0; i < numBendRanges; ++i)
        if (bendRangeSemitones (static_cast<BendRange> (i)) == semitones)
            return i;

    return 0;
}

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

    // --- M2 pitch -> MIDI (appended after the M1 parameters, docs/M2-DESIGN.md 4) ---
    // Value texts carry no unit: the unit is the label (as testTempo), so hosts that show both do not
    // print it twice. Continuous floats (interval 0), like testTempo.
    const auto integerText = [] (float v, int) { return juce::String (juce::roundToInt (v)); };
    const auto oneDecimalText = [] (float v, int) { return juce::String (v, 1); };

    const auto floatParameter = [&layout] (const juce::ParameterID& id, const juce::String& name, float min,
                                           float max, float defaultValue, const juce::String& label,
                                           auto toText)
    {
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            id, name, juce::NormalisableRange<float> (min, max), defaultValue,
            juce::AudioParameterFloatAttributes().withLabel (label).withStringFromValueFunction (toText)));
    };

    layout.add (std::make_unique<juce::AudioParameterChoice> (source, "Source", sourceChoices, defaultSourceIndex));

    layout.add (std::make_unique<juce::AudioParameterChoice> (voiceRange, "Voice Range", voiceRangeChoices,
                                                              defaultVoiceRangeIndex));

    layout.add (std::make_unique<juce::AudioParameterChoice> (pitchAlgorithm, "Pitch Algorithm",
                                                              pitchAlgorithmChoices, defaultPitchAlgorithmIndex));

    layout.add (std::make_unique<juce::AudioParameterChoice> (key, "Key", keyChoices, defaultKeyIndex));

    layout.add (std::make_unique<juce::AudioParameterChoice> (scale, "Scale", scaleChoices, defaultScaleIndex));

    floatParameter (stickiness, "Stickiness", minStickinessPercent, maxStickinessPercent, defaultStickinessPercent,
                    "%", integerText);

    floatParameter (gateThreshold, "Gate Threshold", minGateThresholdDb, maxGateThresholdDb, defaultGateThresholdDb,
                    "dB", oneDecimalText);

    floatParameter (minNoteLength, "Min Note Length", minMinNoteLengthMs, maxMinNoteLengthMs, defaultMinNoteLengthMs,
                    "ms", integerText);

    layout.add (std::make_unique<juce::AudioParameterInt> (octaveShift, "Octave Shift", minOctaveShift,
                                                           maxOctaveShift, defaultOctaveShift));

    layout.add (std::make_unique<juce::AudioParameterChoice> (bendMode, "Bend Mode", bendModeChoices,
                                                              defaultBendModeIndex));

    layout.add (std::make_unique<juce::AudioParameterChoice> (
        bendRange, "Bend Range", bendRangeChoices, defaultBendRangeIndex,
        juce::AudioParameterChoiceAttributes().withValueFromStringFunction (bendRangeIndexFromText)));

    floatParameter (velocitySensitivity, "Velocity Sensitivity", minVelocitySensitivityPercent,
                    maxVelocitySensitivityPercent, defaultVelocitySensitivityPercent, "%", integerText);

    layout.add (std::make_unique<juce::AudioParameterBool> (legato, "Legato", defaultLegato));

    return layout;
}

} // namespace vocalsynth::params
