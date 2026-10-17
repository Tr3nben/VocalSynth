// M3 capture parameters and non-parameter state (docs/M3-DESIGN.md 13). Owner from the implementation
// phase on: agent C (add: CaptureState round trip through get/setStateInformation, old states load with
// defaults, grab lengths snap to the choices).

#include "CaptureSettings.h"
#include "Parameters.h"
#include "PluginProcessor.h"

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
} // namespace

TEST_CASE ("M3 capture parameters exist with the design IDs, ranges and defaults", "[plugin][parameters][capture]")
{
    using namespace vocalsynth::params;
    VocalSynthProcessor p (GENERATE (Variant::fx, Variant::instrument));

    for (const auto* id : { &captureEnabled, &captureAudio, &captureOffsetMode, &captureOffsetMs, &captureFineOffsetMs })
        CHECK (id->getVersionHint() == 2); // after the released M1 / M2 parameters (hint 1)

    auto* enabled = dynamic_cast<juce::AudioParameterBool*> (&getParameter (p, captureEnabled));
    REQUIRE (enabled != nullptr);
    CHECK (enabled->get());

    auto* audio = dynamic_cast<juce::AudioParameterBool*> (&getParameter (p, captureAudio));
    REQUIRE (audio != nullptr);
    CHECK (audio->get());

    auto* mode = dynamic_cast<juce::AudioParameterChoice*> (&getParameter (p, captureOffsetMode));
    REQUIRE (mode != nullptr);
    REQUIRE (mode->choices.size() == 3);
    CHECK (mode->choices[0] == "Auto");
    CHECK (mode->choices[1] == "Manual");
    CHECK (mode->choices[2] == "Off");
    CHECK (mode->getIndex() == 0);

    auto* offset = dynamic_cast<juce::AudioParameterFloat*> (&getParameter (p, captureOffsetMs));
    REQUIRE (offset != nullptr);
    CHECK (offset->range.start == Catch::Approx (0.0f));
    CHECK (offset->range.end == Catch::Approx (300.0f));
    CHECK (offset->get() == Catch::Approx (0.0f));
    CHECK (offset->getLabel() == "ms");

    auto* fine = dynamic_cast<juce::AudioParameterFloat*> (&getParameter (p, captureFineOffsetMs));
    REQUIRE (fine != nullptr);
    CHECK (fine->range.start == Catch::Approx (-100.0f));
    CHECK (fine->range.end == Catch::Approx (100.0f));
    CHECK (fine->get() == Catch::Approx (0.0f));
}

TEST_CASE ("CaptureState defaults follow the decisions", "[plugin][capture][state]")
{
    const vocalsynth::CaptureState state;
    CHECK (state.render.version == vocalsynth::ExportVersion::clean);
    CHECK (state.render.quantize == vocalsynth::QuantizeGrid::off);
    CHECK (state.render.bends == vocalsynth::BendExport::asPlugin);
    CHECK (state.takeGapSeconds == Catch::Approx (3.0));
    CHECK (state.audioHistoryMinutes == Catch::Approx (10.0));
    CHECK (state.grabBars == 8);
    CHECK (state.grabSeconds == 30);
    CHECK (state.toValueTree().hasType (vocalsynth::CaptureState::treeType));
}
