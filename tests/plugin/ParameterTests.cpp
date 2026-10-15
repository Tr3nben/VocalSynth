// M2 parameters (docs/M2-SPEC.md "Parameters", docs/M2-DESIGN.md 4.1): IDs, version hints, types,
// ranges, defaults, choice lists, and the mapping of choice indices onto the core enums.
// Owner from the implementation phase on: agent D.

#include "Parameters.h"
#include "PluginProcessor.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <vector>

using vocalsynth::Variant;
using vocalsynth::VocalSynthProcessor;

namespace
{

juce::RangedAudioParameter& getParameter (VocalSynthProcessor& p, const juce::ParameterID& id)
{
    auto* param = p.getParameters().getParameter (id.getParamID());
    REQUIRE (param != nullptr);
    return *param;
}

float getPlainValue (VocalSynthProcessor& p, const juce::ParameterID& id)
{
    auto& param = getParameter (p, id);
    return param.convertFrom0to1 (param.getValue());
}

void setPlainValue (VocalSynthProcessor& p, const juce::ParameterID& id, float plainValue)
{
    auto& param = getParameter (p, id);
    param.setValueNotifyingHost (param.convertTo0to1 (plainValue));
}

void checkChoice (VocalSynthProcessor& p, const juce::ParameterID& id, const std::vector<juce::String>& expected,
                  int defaultIndex)
{
    auto* choice = dynamic_cast<juce::AudioParameterChoice*> (&getParameter (p, id));
    REQUIRE (choice != nullptr);
    CHECK (id.getVersionHint() == 1);
    REQUIRE (choice->choices.size() == static_cast<int> (expected.size()));

    for (int i = 0; i < choice->choices.size(); ++i)
        CHECK (choice->choices[i] == expected[static_cast<std::size_t> (i)]);

    CHECK (choice->getIndex() == defaultIndex);
}

void checkFloat (VocalSynthProcessor& p, const juce::ParameterID& id, float min, float max, float defaultValue)
{
    auto* param = dynamic_cast<juce::AudioParameterFloat*> (&getParameter (p, id));
    REQUIRE (param != nullptr);
    CHECK (id.getVersionHint() == 1);
    CHECK (param->range.start == Catch::Approx (min));
    CHECK (param->range.end == Catch::Approx (max));
    CHECK (param->get() == Catch::Approx (defaultValue));
}

} // namespace

TEST_CASE ("M2 parameters exist with the spec IDs, ranges and defaults", "[plugin][parameters]")
{
    using namespace vocalsynth::params;
    VocalSynthProcessor p (GENERATE (Variant::fx, Variant::instrument));

    const juce::String plusMinus (juce::CharPointer_UTF8 ("\xc2\xb1"));

    checkChoice (p, source, { "Voice", "Test Pattern" }, 0);
    checkChoice (p, voiceRange, { "Bass", "Baritone", "Tenor", "Alto", "Soprano", "Whistle", "Wide" }, 2);
    checkChoice (p, pitchAlgorithm, { "YIN", "MPM" }, 0);
    checkChoice (p, key, { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" }, 0);
    checkChoice (p, scale,
                 { "Chromatic", "Major", "Natural Minor", "Harmonic Minor", "Major Pentatonic", "Minor Pentatonic",
                   "Blues", "Dorian", "Mixolydian" },
                 0);
    checkChoice (p, bendMode, { "Off", "Anchored", "True" }, 0);
    checkChoice (p, bendRange, { plusMinus + "2", plusMinus + "12", plusMinus + "24", plusMinus + "48" }, 0);

    checkFloat (p, stickiness, 0.0f, 100.0f, 50.0f);
    checkFloat (p, gateThreshold, -70.0f, -10.0f, -45.0f);
    checkFloat (p, minNoteLength, 20.0f, 250.0f, 60.0f);
    checkFloat (p, velocitySensitivity, 0.0f, 100.0f, 70.0f);

    auto* octave = dynamic_cast<juce::AudioParameterInt*> (&getParameter (p, octaveShift));
    REQUIRE (octave != nullptr);
    CHECK (octaveShift.getVersionHint() == 1);
    CHECK (octave->getRange().getStart() == -3);
    CHECK (octave->getRange().getEnd() == 3);
    CHECK (octave->get() == 0);

    auto* legatoParam = dynamic_cast<juce::AudioParameterBool*> (&getParameter (p, legato));
    REQUIRE (legatoParam != nullptr);
    CHECK (legato.getVersionHint() == 1);
    CHECK_FALSE (legatoParam->get());
}

TEST_CASE ("M2 choice indices equal the core enum values", "[plugin][parameters]")
{
    using namespace vocalsynth;
    using namespace vocalsynth::params;

    CHECK (voiceRangeChoices.size() == numVoiceRanges);
    CHECK (pitchAlgorithmChoices.size() == numPitchAlgorithms);
    CHECK (scaleChoices.size() == numScales);
    CHECK (bendModeChoices.size() == numBendModes);
    CHECK (bendRangeChoices.size() == numBendRanges);
    CHECK (sourceChoices.size() == numSources);
    CHECK (keyChoices.size() == 12);

    CHECK (voiceRangeChoices[static_cast<int> (VoiceRange::wide)] == "Wide");
    CHECK (voiceRangeChoices[static_cast<int> (VoiceRange::whistle)] == "Whistle");
    CHECK (scaleChoices[static_cast<int> (Scale::mixolydian)] == "Mixolydian");
    CHECK (bendModeChoices[static_cast<int> (BendMode::trueBend)] == "True");
    CHECK (pitchAlgorithmChoices[static_cast<int> (PitchAlgorithm::mpm)] == "MPM");
    CHECK (sourceChoices[static_cast<int> (Source::testPattern)] == "Test Pattern");

    for (int i = 0; i < numBendRanges; ++i)
        CHECK (bendRangeChoices[i].endsWith (juce::String (bendRangeSemitones (static_cast<BendRange> (i)))));
}

TEST_CASE ("M2 value texts carry no unit (the label does)", "[plugin][parameters]")
{
    using namespace vocalsynth::params;
    VocalSynthProcessor p (Variant::fx);

    CHECK (getParameter (p, stickiness).getCurrentValueAsText() == "50");
    CHECK (getParameter (p, stickiness).getLabel() == "%");
    CHECK (getParameter (p, gateThreshold).getCurrentValueAsText() == "-45.0");
    CHECK (getParameter (p, gateThreshold).getLabel() == "dB");
    CHECK (getParameter (p, minNoteLength).getCurrentValueAsText() == "60");
    CHECK (getParameter (p, minNoteLength).getLabel() == "ms");
    CHECK (getParameter (p, velocitySensitivity).getCurrentValueAsText() == "70");

    // Text -> value parses the number (clap-validator converts in both directions)
    auto& gate = getParameter (p, gateThreshold);
    CHECK (gate.convertFrom0to1 (gate.getValueForText ("-30.0")) == Catch::Approx (-30.0f));
}

TEST_CASE ("State of the M2 parameters survives save and restore", "[plugin][parameters][state]")
{
    using namespace vocalsynth::params;
    const auto variant = GENERATE (Variant::fx, Variant::instrument);

    juce::MemoryBlock state;
    {
        VocalSynthProcessor p (variant);
        setPlainValue (p, source, 1.0f);
        setPlainValue (p, voiceRange, 6.0f);
        setPlainValue (p, pitchAlgorithm, 1.0f);
        setPlainValue (p, key, 9.0f);
        setPlainValue (p, scale, 8.0f);
        setPlainValue (p, stickiness, 12.5f);
        setPlainValue (p, gateThreshold, -33.5f);
        setPlainValue (p, minNoteLength, 120.0f);
        setPlainValue (p, octaveShift, -2.0f);
        setPlainValue (p, bendMode, 2.0f);
        setPlainValue (p, bendRange, 3.0f);
        setPlainValue (p, velocitySensitivity, 0.0f);
        setPlainValue (p, legato, 1.0f);
        p.getStateInformation (state);
    }

    VocalSynthProcessor restored (variant);
    restored.setStateInformation (state.getData(), (int) state.getSize());

    CHECK (getPlainValue (restored, source) == Catch::Approx (1.0f));
    CHECK (getPlainValue (restored, voiceRange) == Catch::Approx (6.0f));
    CHECK (getPlainValue (restored, pitchAlgorithm) == Catch::Approx (1.0f));
    CHECK (getPlainValue (restored, key) == Catch::Approx (9.0f));
    CHECK (getPlainValue (restored, scale) == Catch::Approx (8.0f));
    CHECK (getPlainValue (restored, stickiness) == Catch::Approx (12.5f).margin (0.001));
    CHECK (getPlainValue (restored, gateThreshold) == Catch::Approx (-33.5f).margin (0.001));
    CHECK (getPlainValue (restored, minNoteLength) == Catch::Approx (120.0f).margin (0.001));
    CHECK (getPlainValue (restored, octaveShift) == Catch::Approx (-2.0f));
    CHECK (getPlainValue (restored, bendMode) == Catch::Approx (2.0f));
    CHECK (getPlainValue (restored, bendRange) == Catch::Approx (3.0f));
    CHECK (getPlainValue (restored, velocitySensitivity) == Catch::Approx (0.0f).margin (0.001));
    CHECK (getPlainValue (restored, legato) == Catch::Approx (1.0f));
}
