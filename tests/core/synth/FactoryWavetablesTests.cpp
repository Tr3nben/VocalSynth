// FactoryWavetables: ids permanent and unique, every table builds, deterministic, build time.
// Test plan: docs/SYNTH-DESIGN.md 21. Owner: agent A (Wavetable).

#include "vocalsynth/synth/FactoryWavetables.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE ("FactoryWavetables: scaffold compiles", "[synth][factory]")
{
    SUCCEED();
}
