// Detection-delay compensation (docs/M3-DESIGN.md 16). Owner from the implementation phase on: agent B.
// Planned cases: VoicePipeline timing side output (one timing per event, same events as without it), synthetic tones (sines, saws, voice-like) with known onsets through VoicePipeline -> CaptureTap -> CaptureHistory -> TakeSegmenter -> TakeRenderer: median |MIDI - audio onset| <= 1 hop, p95 <= 2 hops, all ranges x YIN/MPM x 44.1/48/96 kHz; provisional corrections; legato changes; byte-identical .mid for blocks {1, 7, 64, 480, 512, 4096}.

#include "vocalsynth/VoicePipeline.h"

#include <catch2/catch_test_macros.hpp>

using namespace vocalsynth;

TEST_CASE ("Detection-delay compensation: scaffold compiles", "[capturecompensation]")
{
    VoicePipeline pipeline;
    CHECK (pipeline.getHopInputSamples() > 0);
}
