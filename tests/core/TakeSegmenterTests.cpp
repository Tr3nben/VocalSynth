// TakeSegmenter (docs/M3-DESIGN.md 16). Owner from the implementation phase on: agent A.
// Planned cases: transport start/stop, loop wrap keeps one take, free-mode gap just below / above takeGapSeconds, manual new take inside a held note, empty takes discarded, reset closes the take, clear, history eviction, maxTakeEvents split, grab N bars (synced) and N seconds (free).

#include "vocalsynth/TakeSegmenter.h"

#include <catch2/catch_test_macros.hpp>

using namespace vocalsynth;

TEST_CASE ("TakeSegmenter: scaffold compiles", "[takesegmenter]")
{
    TakeSegmenter segmenter;
    CHECK (segmenter.getOpenTake() == nullptr);
    CHECK (segmenter.getNextTakeNumber() == 1);
}
