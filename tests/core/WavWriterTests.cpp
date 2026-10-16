// WavWriter (docs/M3-DESIGN.md 16). Owner from the implementation phase on: agent B.
// Planned cases: pcm24 / float32 headers (sizes, fmt fields, fact chunk), pad byte, encode clamping / rounding / NaN, maxFrames, streaming in blocks == one block.

#include "vocalsynth/WavWriter.h"

#include <catch2/catch_test_macros.hpp>

using namespace vocalsynth;

TEST_CASE ("WavWriter: scaffold compiles", "[wavwriter]")
{
    CHECK (WavWriter::bytesPerSample (WavSampleFormat::pcm24) == 3);
    CHECK (WavWriter::bytesPerSample (WavSampleFormat::float32) == 4);
}
