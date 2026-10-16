// CaptureHistory (docs/M3-DESIGN.md 16). Owner from the implementation phase on: agent A.
// Planned cases: event ring eviction, audio chunks (lazy allocation, reuse, cap of maxAudioBytes), zero-filled holes, audio off ends the run, readAudio outside the range, clearing reset, settingsAt, timeline eviction.

#include "vocalsynth/CaptureHistory.h"

#include <catch2/catch_test_macros.hpp>

using namespace vocalsynth;

TEST_CASE ("CaptureHistory: scaffold compiles", "[capturehistory]")
{
    CaptureHistory history;
    history.prepare (CaptureHistory::defaultAudioMinutes);
    CHECK (history.getAudioRange().from == history.getAudioRange().to);
}
