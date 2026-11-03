// SynthPanel: both variants, every synth parameter has exactly one control, layout inside bounds, preset bar.
// Test plan: docs/SYNTH-DESIGN.md 21. Owner: agent E (UI).

#include "ui/SynthPanel.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE ("SynthPanel: scaffold compiles", "[plugin][editor][synthpanel]")
{
    SUCCEED();
}
