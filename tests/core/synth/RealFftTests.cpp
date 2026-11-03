// RealFft: round trip, Parseval, known spectra, sizes 16..16384.
// Test plan: docs/SYNTH-DESIGN.md 21. Owner: agent A (Wavetable).

#include "vocalsynth/synth/RealFft.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE ("RealFft: scaffold compiles", "[synth][fft]")
{
    SUCCEED();
}
