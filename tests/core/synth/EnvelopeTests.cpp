// Envelope: segment times exact to the sample at any split, retrigger from current value, release mid-attack.
// Test plan: docs/SYNTH-DESIGN.md 21. Owner: agent B (Voice).

#include "vocalsynth/synth/Envelope.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE ("Envelope: scaffold compiles", "[synth][envelope]")
{
    SUCCEED();
}
