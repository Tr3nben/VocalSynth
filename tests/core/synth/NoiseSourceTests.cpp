// NoiseSource: reproducible, white / pink slope, level.
// Test plan: docs/SYNTH-DESIGN.md 21. Owner: agent A (Wavetable).

#include "vocalsynth/synth/NoiseSource.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE ("NoiseSource: scaffold compiles", "[synth][noise]")
{
    SUCCEED();
}
