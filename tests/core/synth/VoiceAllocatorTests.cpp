// VoiceAllocator: no hanging voices (random sequences), <= 16, steal order, last-note priority, deferred note-off, <= 40 ms correction rule.
// Test plan: docs/SYNTH-DESIGN.md 21. Owner: agent B (Voice).

#include "vocalsynth/synth/VoiceAllocator.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE ("VoiceAllocator: scaffold compiles", "[synth][allocator]")
{
    SUCCEED();
}
