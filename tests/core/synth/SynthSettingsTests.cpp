// SynthSettings: defaults, sanitised() clamps every field, NaN -> default, enums.
// Test plan: docs/SYNTH-DESIGN.md 21. Owner: agent B (Voice).

#include "vocalsynth/synth/SynthSettings.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE ("SynthSettings: scaffold compiles", "[synth][settings]")
{
    SUCCEED();
}
