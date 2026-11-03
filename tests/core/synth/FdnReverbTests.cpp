// FdnReverb: RT60 vs size, damping, stability, tail ends in exact silence, NaN guard.
// Test plan: docs/SYNTH-DESIGN.md 21. Owner: agent C (Engine + FX + Expression).

#include "vocalsynth/synth/fx/FdnReverb.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE ("FdnReverb: scaffold compiles", "[synth][fx][reverb]")
{
    SUCCEED();
}
