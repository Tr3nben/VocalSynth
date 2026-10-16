// CaptureTap (docs/M3-DESIGN.md 16). Owner from the implementation phase on: agent A.
// Planned cases: no allocation in process, transport records only on change, block-size invariance {1, 7, 64, 480, 512, 4096} x 44.1/48/96 kHz (constant tempo, tempo change, loop wrap, stop/start), overflow -> gap, markers, flushNotes, bypass, captureOff/audioOff, benchmark < 1 % of a 128-sample block.

#include "vocalsynth/CaptureTap.h"

#include <catch2/catch_test_macros.hpp>

using namespace vocalsynth;

TEST_CASE ("CaptureTap: scaffold compiles", "[capturetap]")
{
    CaptureTap tap;
    tap.prepare (48000.0, 512);
    CHECK (tap.getClock() == 0);
}
