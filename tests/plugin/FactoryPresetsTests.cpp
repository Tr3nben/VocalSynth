// FactoryPresets: 24 presets, categories, IDs valid, values in range, >= 8 use voice sources, tables exist.
// Test plan: docs/SYNTH-DESIGN.md 21. Owner: agent F (Sound / presets).

#include "FactoryPresets.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE ("FactoryPresets: scaffold compiles", "[plugin][synth][factorypresets]")
{
    SUCCEED();
}
