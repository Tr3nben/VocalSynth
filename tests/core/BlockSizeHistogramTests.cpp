#include "vocalsynth/BlockSizeHistogram.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cstring>

using namespace vocalsynth;

TEST_CASE ("BlockSizeHistogram bin boundaries", "[histogram]")
{
    using H = BlockSizeHistogram;

    STATIC_REQUIRE (H::numBins == 9);
    STATIC_REQUIRE (H::binIndex (1) == 0);
    STATIC_REQUIRE (H::binIndex (4096) == 8);

    struct Case
    {
        int numSamples;
        std::size_t bin;
    };

    const auto c = GENERATE (Case { 1, 0 },    Case { 2, 1 },    Case { 16, 1 },   Case { 17, 2 },
                             Case { 32, 2 },   Case { 33, 3 },   Case { 64, 3 },   Case { 65, 4 },
                             Case { 128, 4 },  Case { 129, 5 },  Case { 256, 5 },  Case { 257, 6 },
                             Case { 512, 6 },  Case { 513, 7 },  Case { 1024, 7 }, Case { 1025, 8 },
                             Case { 4096, 8 }, Case { 1 << 20, 8 });

    CAPTURE (c.numSamples);
    CHECK (H::binIndex (c.numSamples) == c.bin);

    H histogram;
    histogram.record (c.numSamples);
    const auto s = histogram.snapshot();

    for (std::size_t i = 0; i < H::numBins; ++i)
        CHECK (s.counts[i] == (i == c.bin ? 1u : 0u));
}

TEST_CASE ("BlockSizeHistogram bin labels", "[histogram]")
{
    using H = BlockSizeHistogram;

    CHECK (std::strcmp (H::binLabel (0), "1") == 0);
    CHECK (std::strcmp (H::binLabel (1), "2-16") == 0);
    CHECK (std::strcmp (H::binLabel (7), "513-1024") == 0);
    CHECK (std::strcmp (H::binLabel (8), ">1024") == 0);
    CHECK (std::strcmp (H::binLabel (9), "") == 0);
}

TEST_CASE ("BlockSizeHistogram starts empty", "[histogram]")
{
    BlockSizeHistogram histogram;
    const auto s = histogram.snapshot();

    for (auto count : s.counts)
        CHECK (count == 0);

    CHECK (s.totalBlocks == 0);
    CHECK (s.emptyBlocks == 0);
    CHECK (s.minBlockSize == 0);
    CHECK (s.maxBlockSize == 0);
    CHECK (s.lastBlockSize == 0);
}

TEST_CASE ("BlockSizeHistogram tracks min, max, last and total", "[histogram]")
{
    BlockSizeHistogram histogram;

    for (int n : { 512, 7, 4096, 1, 480, 64 })
        histogram.record (n);

    const auto s = histogram.snapshot();
    CHECK (s.totalBlocks == 6);
    CHECK (s.emptyBlocks == 0);
    CHECK (s.minBlockSize == 1);
    CHECK (s.maxBlockSize == 4096);
    CHECK (s.lastBlockSize == 64);

    CHECK (s.counts[0] == 1); // 1
    CHECK (s.counts[1] == 1); // 7
    CHECK (s.counts[3] == 1); // 64
    CHECK (s.counts[6] == 2); // 480, 512
    CHECK (s.counts[8] == 1); // 4096

    uint64_t sum = 0;
    for (auto count : s.counts)
        sum += count;
    CHECK (sum == s.totalBlocks);
}

TEST_CASE ("BlockSizeHistogram min is the first block when it is the largest", "[histogram]")
{
    BlockSizeHistogram histogram;
    histogram.record (1024);
    histogram.record (2048);

    const auto s = histogram.snapshot();
    CHECK (s.minBlockSize == 1024);
    CHECK (s.maxBlockSize == 2048);
    CHECK (s.lastBlockSize == 2048);
}

TEST_CASE ("BlockSizeHistogram counts empty blocks separately", "[histogram]")
{
    BlockSizeHistogram histogram;
    histogram.record (0);
    histogram.record (-5);
    histogram.record (256);
    histogram.record (0);

    const auto s = histogram.snapshot();
    CHECK (s.emptyBlocks == 3);
    CHECK (s.totalBlocks == 1);
    CHECK (s.minBlockSize == 256);
    CHECK (s.maxBlockSize == 256);
    CHECK (s.lastBlockSize == 256);
    CHECK (s.counts[5] == 1);
}

TEST_CASE ("BlockSizeHistogram reset clears everything", "[histogram]")
{
    BlockSizeHistogram histogram;

    for (int n : { 0, 1, 33, 1025 })
        histogram.record (n);

    histogram.reset();
    const auto cleared = histogram.snapshot();

    for (auto count : cleared.counts)
        CHECK (count == 0);

    CHECK (cleared.totalBlocks == 0);
    CHECK (cleared.emptyBlocks == 0);
    CHECK (cleared.minBlockSize == 0);
    CHECK (cleared.maxBlockSize == 0);
    CHECK (cleared.lastBlockSize == 0);

    // After a reset, min/max start fresh.
    histogram.record (100);
    const auto s = histogram.snapshot();
    CHECK (s.minBlockSize == 100);
    CHECK (s.maxBlockSize == 100);
    CHECK (s.totalBlocks == 1);
    CHECK (s.counts[4] == 1);
}
