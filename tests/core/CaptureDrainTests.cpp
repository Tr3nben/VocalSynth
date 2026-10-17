// CaptureDrain (docs/M3-DESIGN.md 5.3). Owner from the implementation phase on: agent A.
// Planned cases (once SpscRing / CaptureTap / CaptureHistory are implemented): records and audio frames
// reach the history in stream order, audio frames are matched to their audio records, records pushed
// after the clock was loaded do not move advanceTo backwards, one call handles at most one ring capacity.

#include "vocalsynth/CaptureDrain.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace vocalsynth;

TEST_CASE ("CaptureDrain: an empty tap drains nothing and keeps its clock", "[capturedrain]")
{
    CaptureTap tap;
    tap.prepare (48000.0, 512);
    tap.processChunk (nullptr, 480, {}, {});

    CaptureHistory history;
    history.prepare (1.0);
    TakeSegmenter segmenter;
    std::vector<Take> closed;
    CaptureDrain drain;

    CHECK (drain.drain (tap, history, segmenter, closed) == 0);
    CHECK (drain.getLastClock() == tap.getClock());
    CHECK (closed.empty());
}
