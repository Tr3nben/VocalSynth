// SynthState: round trip with embedded user table, data once per hash, broken / missing children, stateVersion.
// Test plan: docs/SYNTH-DESIGN.md 21. Owner: agent D (Plugin data).

#include "SynthState.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE ("SynthState: scaffold compiles", "[plugin][synth][state]")
{
    SUCCEED();
}
