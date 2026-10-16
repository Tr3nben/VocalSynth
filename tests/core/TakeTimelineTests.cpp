// TakeTimeline (docs/M3-DESIGN.md 16). Owner from the implementation phase on: agent A.
// Planned cases: beats at 60 / 97.5 / 120 / 174 BPM, tempo changes, stop/start keeps beats, loop wrap unwraps, small ppq differences re-sync, bar grid from the host bar start / from ppq 0 / without ppq, 3/4 and 7/8, barSnapToleranceBeats, slice, tempo / time-signature maps, sampleAtBeats inverse.

#include "vocalsynth/TakeTimeline.h"

#include <catch2/catch_test_macros.hpp>

using namespace vocalsynth;

TEST_CASE ("TakeTimeline: scaffold compiles", "[taketimeline]")
{
    TakeTimeline timeline;
    timeline.reset (48000.0);
    CHECK (timeline.empty());
}
