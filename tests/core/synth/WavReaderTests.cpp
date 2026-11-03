// WavReader: formats 8/16/24/32/32f/64f, extensible, stereo -> mid, clm chunk, broken files, fuzz.
// Test plan: docs/SYNTH-DESIGN.md 21. Owner: agent A (Wavetable).

#include "vocalsynth/synth/WavReader.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE ("WavReader: scaffold compiles", "[synth][wavreader]")
{
    SUCCEED();
}
