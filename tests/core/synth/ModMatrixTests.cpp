// ModMatrix: scaling per destination, summing, off slots, global / per-voice split.
// Test plan: docs/SYNTH-DESIGN.md 21. Owner: agent B (Voice).

#include "vocalsynth/synth/ModMatrix.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE ("ModMatrix: scaffold compiles", "[synth][modmatrix]")
{
    SUCCEED();
}
