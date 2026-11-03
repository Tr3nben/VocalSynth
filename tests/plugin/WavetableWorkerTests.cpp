// WavetableWorker: factory / import / embedded requests, newest request wins, errors, Unicode paths.
// Test plan: docs/SYNTH-DESIGN.md 21. Owner: agent D (Plugin data).

#include "WavetableWorker.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE ("WavetableWorker: scaffold compiles", "[plugin][synth][worker]")
{
    SUCCEED();
}
