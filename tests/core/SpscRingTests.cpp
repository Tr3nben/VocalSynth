// SpscRing (docs/M3-DESIGN.md 16). Owner from the implementation phase on: agent A.
// Planned cases: wrap-around, full/empty, bulk push/pop, overflow count, two-thread stress (>= 1e7 items, checksum).

#include "vocalsynth/SpscRing.h"

#include <catch2/catch_test_macros.hpp>

using namespace vocalsynth;

TEST_CASE ("SpscRing: scaffold compiles", "[spscring]")
{
    SpscRing<float> ring;
    CHECK (ring.getOverflowCount() == 0);
}
