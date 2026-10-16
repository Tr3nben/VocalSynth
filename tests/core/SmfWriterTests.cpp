// SmfWriter (docs/M3-DESIGN.md 16). Owner from the implementation phase on: agent B.
// Planned cases: header (format 0/1, one track, division), meta order at tick 0, tempo / time-signature changes, RPN 0, delta times / VLQ edge values, note-off as 8n, end-of-track at endTick, byte-identical output, track name ASCII.

#include "vocalsynth/SmfWriter.h"

#include <catch2/catch_test_macros.hpp>

using namespace vocalsynth;

TEST_CASE ("SmfWriter: scaffold compiles", "[smfwriter]")
{
    std::vector<uint8_t> bytes;
    SmfWriter::appendVariableLength (bytes, 0);
    CHECK (bytes.size() <= 1);
}
