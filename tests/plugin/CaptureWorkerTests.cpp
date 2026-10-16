// CaptureWorker (docs/M3-DESIGN.md 5, 16). Owner from the implementation phase on: agent C.
// Planned cases: start / suspend / resume / stop (quick join, no thread before start), drain step with a
// fed CaptureTap, take list (newest first, max 32, versions), open-take snapshot, WAV cache states
// (preparing -> ready -> persisted), re-render on new WAV options, heartbeat, 10-minute simulated take
// (Voice and Test Pattern, runStepForTests) with overflow counters 0, makeTransportInfo.

#include "CaptureExporter.h"
#include "CaptureWorker.h"

#include <catch2/catch_test_macros.hpp>

using namespace vocalsynth;

TEST_CASE ("CaptureWorker: scaffold compiles", "[plugin][captureworker]")
{
    CaptureTap tap;
    tap.prepare (48000.0, 512);
    CaptureWorker worker (tap, CaptureExporter::getInstanceFolder ("test-scaffold"));
    CHECK_FALSE (worker.isRunning());
    CHECK (worker.getTakes().empty());
    worker.stop();
}
