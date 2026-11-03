// Wavetable: mip layout, memory bound, level access.
// Test plan: docs/SYNTH-DESIGN.md 21. Owner: agent A (Wavetable).

#include "vocalsynth/synth/Wavetable.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE ("Wavetable: scaffold compiles", "[synth][wavetable]")
{
    SUCCEED();
}
