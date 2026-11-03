// Processor with the synth (docs/SYNTH-DESIGN.md 17, 21): synth audible in FX and Instrument, dryLevel / muteDry,
// 0 dB dry bit-identical to 0.3.1, synth part block-size invariant through the processor, no allocation in
// processBlock, nothing hangs on bypass / flush / source change, tables published when setStateInformation
// returns, state round trip with an embedded user table, preset load sets no voice parameter.
// Test plan: docs/SYNTH-DESIGN.md 21. Owner: integration.

#include "PluginProcessor.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE ("SynthProcessor: scaffold compiles", "[plugin][synth][processor]")
{
    SUCCEED();
}
