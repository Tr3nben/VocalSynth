// ExpressionTracker: level mapping and smoothing, brightness of known spectra, noisiness of noise vs tone, pitch cents.
// Test plan: docs/SYNTH-DESIGN.md 21. Owner: agent C (Engine + FX + Expression).

#include "vocalsynth/ExpressionTracker.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE ("ExpressionTracker: scaffold compiles", "[expression]")
{
    SUCCEED();
}
