// Chorus: modulation depth, mix law, on/off fade, bypass when off.
// Test plan: docs/SYNTH-DESIGN.md 21. Owner: agent C (Engine + FX + Expression).

#include "vocalsynth/synth/fx/Chorus.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE ("Chorus: scaffold compiles", "[synth][fx][chorus]")
{
    SUCCEED();
}
