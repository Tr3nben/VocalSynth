// 0.4.0 synth parameters (docs/SYNTH-DESIGN.md 15): 102 parameters appended after the M3.1 ones, version hint 4,
// unique IDs, defaults = the core SynthSettings defaults, choice lists = the core enums, value texts.
// Written by the design agent; owner from the implementation phase on: agent D.

#include "Parameters.h"
#include "PluginProcessor.h"
#include "SynthParameters.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

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

float getPlain (VocalSynthProcessor& p, const juce::ParameterID& id)
{
    auto& param = getParameter (p, id);
    return param.convertFrom0to1 (param.getValue());
}

juce::StringArray layoutIds (VocalSynthProcessor& p)
{
    juce::StringArray ids;

    for (auto* parameter : p.getParameters().processor.getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (parameter))
            ids.add (ranged->paramID);

    return ids;
}
} // namespace

TEST_CASE ("Synth parameters are appended after the M3.1 parameters with version hint 4", "[plugin][parameters][synth]")
{
    using namespace vocalsynth::params;
    VocalSynthProcessor p (GENERATE (Variant::fx, Variant::instrument));

    const auto ids = layoutIds (p);
    const auto synthIds = getSynthParameterIds();

    REQUIRE (synthIds.size() == numSynthParameters);
    REQUIRE (ids.size() == 28 + numSynthParameters);
    CHECK (ids.indexOf ("recordBars") == 27);

    for (int i = 0; i < synthIds.size(); ++i)
    {
        CAPTURE (synthIds[i]);
        CHECK (ids[28 + i] == synthIds[i]);

        auto* param = dynamic_cast<juce::RangedAudioParameter*> (p.getParameters().getParameter (synthIds[i]));
        REQUIRE (param != nullptr);
        CHECK (param->getVersionHint() == synthParameterVersionHint);
    }

    auto unique = ids;
    unique.removeDuplicates (false);
    CHECK (unique.size() == ids.size());

    CHECK (synthIds[0] == "synthOn");
    CHECK (synthIds[synthIds.size() - 1] == "reverbMix");
}

TEST_CASE ("Synth parameter defaults are the core SynthSettings defaults", "[plugin][parameters][synth]")
{
    using namespace vocalsynth::params;
    VocalSynthProcessor p (Variant::fx);

    // synthOn: the parameter default is the value of a new instance (on, decision E4; integration 0.4 - a
    // different initial value fails clap-validator's param-default-values); old projects get it off through
    // migrateState (StateFixtureTests), docs/SYNTH-DESIGN.md 13.3, 24. The core default stays off.
    CHECK (getParameter (p, synthOn).getDefaultValue() == Catch::Approx (synthOnForNewInstances ? 1.0 : 0.0));
    CHECK_FALSE (vocalsynth::synth::SynthSettings {}.synthOn);
    CHECK (getPlain (p, synthVolume) == Catch::Approx (-6.0f));
    CHECK (getPlain (p, dryLevel) == Catch::Approx (0.0f));
    CHECK (getPlain (p, playMode) == Catch::Approx (1.0f)); // Legato (E6)
    CHECK (getPlain (p, glideMode) == Catch::Approx (1.0f)); // Legato Only
    CHECK (getPlain (p, ampGain) == Catch::Approx (0.0f));
    CHECK (getPlain (p, oscParameters[0].on) == Catch::Approx (1.0f));
    CHECK (getPlain (p, oscParameters[1].on) == Catch::Approx (0.0f));
    CHECK (getPlain (p, oscParameters[0].position) == Catch::Approx (50.0f));
    CHECK (getPlain (p, oscParameters[0].unison) == Catch::Approx (1.0f));
    CHECK (getPlain (p, filterMode) == Catch::Approx (1.0f)); // LP24
    CHECK (getPlain (p, filterCutoff) == Catch::Approx (20000.0f));
    CHECK (getPlain (p, envelopeParameters[0].sustain) == Catch::Approx (100.0f));
    CHECK (getPlain (p, envelopeParameters[1].sustain) == Catch::Approx (0.0f));
    CHECK (getPlain (p, lfoParameters[0].rate) == Catch::Approx (5.0f));
    CHECK (getPlain (p, lfoParameters[1].rate) == Catch::Approx (0.5f));
    CHECK (getPlain (p, modParameters[7].amount) == Catch::Approx (0.0f));
    CHECK (getPlain (p, delayTime) == Catch::Approx (375.0f));
    CHECK (getPlain (p, reverbMix) == Catch::Approx (25.0f));
}

TEST_CASE ("Synth choice lists match the core enums", "[plugin][parameters][synth]")
{
    using namespace vocalsynth;
    using namespace vocalsynth::params;

    CHECK (playModeChoices.size() == synth::numPlayModes);
    CHECK (glideModeChoices.size() == synth::numGlideModes);
    CHECK (subOctaveChoices.size() == synth::numSubOctaves);
    CHECK (subShapeChoices.size() == synth::numSubShapes);
    CHECK (filterModeChoices.size() == synth::numFilterModes);
    CHECK (lfoShapeChoices.size() == synth::numLfoShapes);
    CHECK (tempoDivisionChoices.size() == synth::numTempoDivisions);
    CHECK (modSourceChoices.size() == synth::numModSources);
    CHECK (modDestChoices.size() == synth::numModDests);

    CHECK (modSourceChoices[static_cast<int> (synth::ModSource::voicePitch)] == "Voice Pitch");
    CHECK (modDestChoices[static_cast<int> (synth::ModDest::reverbMix)] == "Reverb Mix");
    CHECK (modDestChoices[static_cast<int> (synth::ModDest::pitch)] == "Pitch");
    CHECK (filterModeChoices[static_cast<int> (synth::FilterMode::notch)] == "Notch");
    CHECK (tempoDivisionChoices[static_cast<int> (synth::TempoDivision::bars4)] == "4 Bars");
}

TEST_CASE ("Synth choice lists have their final length from 0.4.0 (saved automation, review 0.4 R2-1)",
           "[plugin][parameters][synth]")
{
    // Host automation stores index / (n - 1): a list that grows later moves every automation point. The lengths
    // below are frozen (docs/SYNTH-DESIGN.md 15, 24.4); later additions need a new parameter.
    using namespace vocalsynth::params;

    CHECK (playModeChoices.size() == 3);
    CHECK (glideModeChoices.size() == 2);
    CHECK (subOctaveChoices.size() == 2);
    CHECK (subShapeChoices.size() == 3);
    CHECK (filterModeChoices.size() == 5);
    CHECK (lfoShapeChoices.size() == 7);
    CHECK (tempoDivisionChoices.size() == 15);
    CHECK (modSourceChoices.size() == 17);
    CHECK (modDestChoices.size() == 22);

    // The planned sources are reserved now (0.5: vowel tracker; 0.6: calibrated vowel weights)
    CHECK (modSourceChoices[12] == "Vowel X");
    CHECK (modSourceChoices[13] == "Vowel Y");
    CHECK (modSourceChoices[14] == "Vowel Weight 1");
    CHECK (modSourceChoices[15] == "Vowel Weight 2");
    CHECK (modSourceChoices[16] == "Vowel Weight 3");

    // The real parameters carry the full lists
    VocalSynthProcessor p (Variant::fx);

    for (const auto& ids : modParameters)
    {
        auto* choice = dynamic_cast<juce::AudioParameterChoice*> (p.getParameters().getParameter (ids.source.getParamID()));
        REQUIRE (choice != nullptr);
        CHECK (choice->choices.size() == 17);
    }
}

TEST_CASE ("Presets set every synth parameter except the mix", "[plugin][parameters][synth]")
{
    using namespace vocalsynth::params;

    CHECK_FALSE (isPresetParameter (synthOn.getParamID()));
    CHECK_FALSE (isPresetParameter (synthVolume.getParamID()));
    CHECK_FALSE (isPresetParameter (dryLevel.getParamID()));
    CHECK_FALSE (isPresetParameter (gateThreshold.getParamID()));
    CHECK_FALSE (isPresetParameter (captureEnabled.getParamID()));
    CHECK (isPresetParameter (filterCutoff.getParamID()));
    CHECK (isPresetParameter (ampGain.getParamID())); // the patch level is part of the preset
    CHECK (isPresetParameter (modParameters[3].dest.getParamID()));

    int count = 0;

    for (const auto& id : getSynthParameterIds())
        if (isPresetParameter (id))
            ++count;

    CHECK (count == numSynthParameters - 3);
}

TEST_CASE ("Synth value texts carry no unit and parse back", "[plugin][parameters][synth]")
{
    using namespace vocalsynth::params;
    VocalSynthProcessor p (Variant::fx);

    auto& cutoff = getParameter (p, filterCutoff);
    CHECK (cutoff.getCurrentValueAsText() == "20000");
    CHECK (cutoff.getLabel() == "Hz");
    CHECK (cutoff.convertFrom0to1 (cutoff.getValueForText ("1000")) == Catch::Approx (1000.0f).epsilon (1e-4));
    // Logarithmic: the knob's middle is the geometric mean
    CHECK (cutoff.convertFrom0to1 (0.5f) == Catch::Approx (std::sqrt (20.0f * 20000.0f)).epsilon (1e-4));

    CHECK (getParameter (p, synthVolume).getCurrentValueAsText() == "-6.0");
    CHECK (getParameter (p, ampGain).getCurrentValueAsText() == "0.0");
    CHECK (getParameter (p, ampGain).getLabel() == "dB");
    CHECK (getParameter (p, lfoParameters[1].rate).getCurrentValueAsText() == "0.50");
    CHECK (getParameter (p, envelopeParameters[0].attack).getCurrentValueAsText() == "5.0");
    CHECK (getParameter (p, envelopeParameters[0].decay).getCurrentValueAsText() == "300");
}
