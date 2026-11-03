// SynthVoice: control protocol, glide time, legato keeps envelopes, correction jumps, steal fade, silence after release.
// Test plan: docs/SYNTH-DESIGN.md 21. Owner: agent B (Voice).

#include "vocalsynth/synth/SynthVoice.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE ("SynthVoice: scaffold compiles", "[synth][voice]")
{
    SUCCEED();
}
