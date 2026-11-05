// Wavetable: mip layout, memory bound, level access.
// Test plan: docs/SYNTH-DESIGN.md 21. Owner: agent A (Wavetable).

#include "vocalsynth/synth/Wavetable.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

using namespace vocalsynth::synth;

TEST_CASE ("Wavetable: layout constants", "[synth][wavetable]")
{
    STATIC_REQUIRE (Wavetable::frameSize == 2048);
    STATIC_REQUIRE (Wavetable::maxFrames == 256);
    STATIC_REQUIRE (Wavetable::numMipLevels == 11);
    STATIC_REQUIRE (Wavetable::frameStride == 22561);
    STATIC_REQUIRE (Wavetable::guardBefore == 1);
    STATIC_REQUIRE (Wavetable::guardAfter == 2);

    const int lengths[] = { 8192, 4096, 2048, 1024, 1024, 1024, 1024, 1024, 1024, 1024, 1024 };
    int total = 0;

    for (int level = 0; level < Wavetable::numMipLevels; ++level)
    {
        CHECK (Wavetable::maxHarmonic (level) == 1024 >> level);
        CHECK (Wavetable::mipLength (level) == lengths[level]);
        CHECK (Wavetable::mipLength (level) >= 8 * Wavetable::maxHarmonic (level)); // >= 8x oversampled
        CHECK (Wavetable::levelOffset (level) == total + Wavetable::guardBefore);
        total += Wavetable::mipLength (level) + Wavetable::guardBefore + Wavetable::guardAfter;
    }

    CHECK (total == Wavetable::frameStride);
}

TEST_CASE ("Wavetable: memory bound and frame count clamp", "[synth][wavetable]")
{
    const Wavetable big ("big", 256);
    CHECK (big.getNumFrames() == 256);
    CHECK (big.getMemoryBytes() == 256u * 22561u * sizeof (float));
    CHECK (static_cast<double> (big.getMemoryBytes()) <= 23.2e6);

    const Wavetable one ("one", 1);
    CHECK (one.getMemoryBytes() == 90244u); // 90.2 KB per frame

    CHECK (Wavetable ("x", 0).getNumFrames() == 1);
    CHECK (Wavetable ("x", -5).getNumFrames() == 1);
    CHECK (Wavetable ("x", 300).getNumFrames() == 256);
    CHECK (Wavetable ("name", 3).getName() == "name");
    CHECK (Wavetable ("name", 3).getContentHash() == 0);

    Wavetable hashed ("h", 1);
    hashed.setContentHash (0x0123456789abcdefull);
    CHECK (hashed.getContentHash() == 0x0123456789abcdefull);
}

TEST_CASE ("Wavetable: level pointers are disjoint, guard samples inside the frame", "[synth][wavetable]")
{
    Wavetable table ("t", 3);
    const float* begin = table.getLevel (0, 0) - Wavetable::guardBefore;
    const float* end = begin + 3 * static_cast<std::ptrdiff_t> (Wavetable::frameStride);

    for (int frame = 0; frame < 3; ++frame)
    {
        const float* frameBegin = begin + frame * static_cast<std::ptrdiff_t> (Wavetable::frameStride);
        const float* frameEnd = frameBegin + Wavetable::frameStride;
        const float* previousEnd = frameBegin;

        for (int level = 0; level < Wavetable::numMipLevels; ++level)
        {
            const float* p = table.getLevel (frame, level);
            const int n = Wavetable::mipLength (level);
            CHECK (p == table.getLevelForWriting (frame, level));
            // p[-1] .. p[n + 1] inside this frame, after the previous level's guards
            CHECK (p - 1 >= previousEnd);
            CHECK (p + n + 2 <= frameEnd);
            CHECK (p + n + 2 <= end);
            previousEnd = p + n + 2;
        }

        CHECK (previousEnd == frameEnd); // levels back to back, no gap
    }

    // Writing every level of every frame touches each float exactly once (no overlap)
    std::vector<int> touched (static_cast<std::size_t> (3 * Wavetable::frameStride), 0);

    for (int frame = 0; frame < 3; ++frame)
        for (int level = 0; level < Wavetable::numMipLevels; ++level)
        {
            const float* p = table.getLevel (frame, level);

            for (int i = -1; i < Wavetable::mipLength (level) + 2; ++i)
                ++touched[static_cast<std::size_t> (p + i - begin)];
        }

    for (const auto t : touched)
        REQUIRE (t == 1);

    // Zero-initialised
    for (const float* p = begin; p < end; ++p)
        REQUIRE (*p == 0.0f);
}
