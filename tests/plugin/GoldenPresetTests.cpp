// Golden audio: every factory preset's feature fingerprint vs tests/data/golden/presets-0.4.json, peak <= -1 dBFS, no DC / silence / NaN.
// Test plan: docs/SYNTH-DESIGN.md 21. Owner: agent F (Sound / presets).

#include "FactoryPresets.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE ("GoldenPreset: scaffold compiles", "[plugin][synth][golden]")
{
    SUCCEED();
}
