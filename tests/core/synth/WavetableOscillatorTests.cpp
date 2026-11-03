// WavetableOscillator: frequency +-0.1 ct, alias test, unison detune / spread, morph and mip switch without jumps, table crossfade, benchmark.
// Test plan: docs/SYNTH-DESIGN.md 21. Owner: agent A (Wavetable).

#include "vocalsynth/synth/WavetableOscillator.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE ("WavetableOscillator: scaffold compiles", "[synth][oscillator]")
{
    SUCCEED();
}
