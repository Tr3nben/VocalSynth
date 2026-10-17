// TakeSegmenter (docs/M3-DESIGN.md 16). Owner from the implementation phase on: agent A.
// Planned cases: transport start/stop, capture tail after stop (a note sung just before Stop arrives after it and stays in the synced take; no free take starts in the tail), loop wrap keeps one take, free-mode gap just below / above takeGapSeconds, manual new take inside a held note, empty takes discarded, reset closes the take, clear, history eviction, maxTakeEvents split, grab N bars (synced) and N seconds (free).

#include "vocalsynth/TakeSegmenter.h"

#include <catch2/catch_test_macros.hpp>

using namespace vocalsynth;

TEST_CASE ("TakeSegmenter: scaffold compiles", "[takesegmenter]")
{
    TakeSegmenter segmenter;
    CHECK (segmenter.getOpenTake() == nullptr);
    CHECK (segmenter.getNextTakeNumber() == 1);
}

TEST_CASE ("TakeSegmenter: capture tail covers the largest round trip plus the detection allowance",
           "[takesegmenter]")
{
    // ASIO 2048 at 48 kHz: max (4096 + 4800, 14400) + 0.25 s = 14400 + 12000
    CHECK (TakeSegmenter::compensationTailSamples (48000.0, 2048) == 26400);
    CHECK (TakeSegmenter::compensationTailSamples (0.0, 2048) == 0);
}
