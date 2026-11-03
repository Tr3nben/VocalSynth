// VoicePipeline expression overload: same events as before (bit-identical), frames per hop, offsets block-size invariant.
// Test plan: docs/SYNTH-DESIGN.md 21. Owner: agent C (Engine + FX + Expression).

#include "vocalsynth/VoicePipeline.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE ("VoicePipelineExpression: scaffold compiles", "[expression][pipeline]")
{
    SUCCEED();
}
