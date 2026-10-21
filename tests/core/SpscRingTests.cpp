// SpscRing (docs/M3-DESIGN.md 3, 16). Owner: agent A.

#include "vocalsynth/CaptureRecord.h"
#include "vocalsynth/SpscRing.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <numeric>
#include <thread>
#include <vector>

using namespace vocalsynth;

TEST_CASE ("SpscRing: capacity is a power of two, an unprepared ring rejects everything", "[spscring]")
{
    SpscRing<int> ring;
    CHECK (ring.getCapacity() == 0);
    CHECK_FALSE (ring.push (1));
    CHECK (ring.getOverflowCount() == 1);
    int x = 0;
    CHECK_FALSE (ring.pop (x));

    ring.prepare (0);
    CHECK (ring.getCapacity() == 1);
    ring.prepare (1000);
    CHECK (ring.getCapacity() == 1024);
    CHECK (ring.getOverflowCount() == 0); // prepare clears the counter
    ring.prepare (1024);
    CHECK (ring.getCapacity() == 1024);
    ring.prepare (1025);
    CHECK (ring.getCapacity() == 2048);
}

TEST_CASE ("SpscRing: full / empty without a wasted slot, overflow counts dropped items", "[spscring]")
{
    SpscRing<int> ring;
    ring.prepare (8);

    CHECK (ring.size() == 0);
    CHECK (ring.freeSpace() == 8);

    for (int i = 0; i < 8; ++i)
        CHECK (ring.push (i));

    CHECK (ring.size() == 8);
    CHECK (ring.freeSpace() == 0);
    CHECK_FALSE (ring.push (99));
    CHECK_FALSE (ring.push (99));
    CHECK (ring.getOverflowCount() == 2);

    int x = -1;
    for (int i = 0; i < 8; ++i)
    {
        REQUIRE (ring.pop (x));
        CHECK (x == i);
    }

    CHECK_FALSE (ring.pop (x));
    CHECK (ring.size() == 0);
    CHECK (ring.freeSpace() == 8);
    CHECK (ring.getOverflowCount() == 2);
}

TEST_CASE ("SpscRing: wrap-around keeps FIFO order for single and bulk operations", "[spscring]")
{
    SpscRing<int> ring;
    ring.prepare (8);

    int next = 0;
    int expected = 0;

    for (int round = 0; round < 50; ++round)
    {
        // Push 5 (bulk), pop 3 (single) + rest (bulk): indices wrap many times
        std::array<int, 5> in {};
        for (auto& v : in)
            v = next++;

        if (ring.freeSpace() >= in.size())
            REQUIRE (ring.tryPushAll (std::span<const int> { in }));
        else
            next -= 5;

        for (int i = 0; i < 3 && ring.size() > 0; ++i)
        {
            int x = 0;
            REQUIRE (ring.pop (x));
            CHECK (x == expected++);
        }

        if (round % 3 == 2)
        {
            std::array<int, 8> out {};
            const auto n = ring.popBulk (std::span<int> { out });

            for (std::size_t i = 0; i < n; ++i)
                CHECK (out[i] == expected++);
        }
    }

    std::array<int, 16> rest {};
    const auto n = ring.popBulk (std::span<int> { rest });
    for (std::size_t i = 0; i < n; ++i)
        CHECK (rest[i] == expected++);

    CHECK (expected == next);
}

TEST_CASE ("SpscRing: tryPushAll is all or nothing", "[spscring]")
{
    SpscRing<float> ring;
    ring.prepare (16);

    std::vector<float> ten (10, 1.0f);
    REQUIRE (ring.tryPushAll (std::span<const float> { ten }));
    CHECK (ring.size() == 10);

    // 10 more do not fit into the 6 free slots: nothing is stored, 10 counted
    CHECK_FALSE (ring.tryPushAll (std::span<const float> { ten }));
    CHECK (ring.size() == 10);
    CHECK (ring.getOverflowCount() == 10);

    std::vector<float> six (6, 2.0f);
    CHECK (ring.tryPushAll (std::span<const float> { six }));
    CHECK (ring.freeSpace() == 0);
    CHECK (ring.tryPushAll ({})); // empty always fits

    std::vector<float> out (32, 0.0f);
    CHECK (ring.popBulk (std::span<float> { out }) == 16);
    CHECK (out[9] == 1.0f);
    CHECK (out[10] == 2.0f);
    CHECK (out[15] == 2.0f);
    CHECK (ring.popBulk (std::span<float> { out }) == 0);
}

TEST_CASE ("SpscRing: reset empties but keeps storage and counter; prepare with the same size clears", "[spscring]")
{
    SpscRing<int> ring;
    ring.prepare (4);

    for (int i = 0; i < 6; ++i)
        ring.push (i);

    CHECK (ring.getOverflowCount() == 2);
    ring.reset();
    CHECK (ring.size() == 0);
    CHECK (ring.getCapacity() == 4);
    CHECK (ring.getOverflowCount() == 2);

    ring.push (7);
    int x = 0;
    REQUIRE (ring.pop (x));
    CHECK (x == 7);

    ring.push (8);
    ring.prepare (4);
    CHECK (ring.size() == 0);
    CHECK (ring.getOverflowCount() == 0);
}

TEST_CASE ("SpscRing: two-thread stress, 10^7 items, order and checksum", "[spscring]")
{
    constexpr uint64_t total = 10'000'000;
    SpscRing<uint64_t> ring;
    ring.prepare (4096);

    uint64_t producedSum = 0;
    std::thread producer ([&]
    {
        uint64_t next = 0;
        std::array<uint64_t, 37> block {};

        while (next < total)
        {
            if ((next & 1) == 0)
            {
                // Bulk push of a block that wraps at arbitrary places
                const auto n = std::min<uint64_t> (block.size(), total - next);
                for (uint64_t i = 0; i < n; ++i)
                    block[i] = next + i;

                if (ring.tryPushAll (std::span<const uint64_t> { block.data(), static_cast<std::size_t> (n) }))
                {
                    for (uint64_t i = 0; i < n; ++i)
                        producedSum += (next + i) * 2654435761u;
                    next += n;
                }
                else
                {
                    std::this_thread::yield();
                }
            }
            else if (ring.push (next))
            {
                producedSum += next * 2654435761u;
                ++next;
            }
            else
            {
                std::this_thread::yield();
            }
        }
    });

    uint64_t expected = 0;
    uint64_t consumedSum = 0;
    bool inOrder = true;
    std::array<uint64_t, 61> out {};

    while (expected < total)
    {
        std::size_t n = 0;

        if ((expected % 3) == 0)
        {
            uint64_t v = 0;
            if (ring.pop (v))
            {
                out[0] = v;
                n = 1;
            }
        }
        else
        {
            n = ring.popBulk (std::span<uint64_t> { out });
        }

        if (n == 0)
        {
            std::this_thread::yield();
            continue;
        }

        for (std::size_t i = 0; i < n; ++i)
        {
            inOrder = inOrder && out[i] == expected;
            consumedSum += out[i] * 2654435761u;
            ++expected;
        }
    }

    producer.join();
    CHECK (inOrder);
    CHECK (consumedSum == producedSum);
    CHECK (ring.size() == 0);
}

TEST_CASE ("SpscRing: two-thread stress with 32-byte records", "[spscring]")
{
    constexpr int64_t total = 2'000'000;
    SpscRing<CaptureRecord> ring;
    ring.prepare (1024);

    std::thread producer ([&]
    {
        for (int64_t i = 0; i < total;)
        {
            if (ring.push (CaptureRecord::makeGap (i, { static_cast<uint64_t> (i) * 3u, static_cast<uint64_t> (~i) })))
                ++i;
            else
                std::this_thread::yield();
        }
    });

    bool ok = true;
    std::array<CaptureRecord, 100> out {};

    for (int64_t next = 0; next < total;)
    {
        const auto n = ring.popBulk (std::span<CaptureRecord> { out });

        if (n == 0)
            std::this_thread::yield();

        for (std::size_t i = 0; i < n; ++i, ++next)
        {
            const auto g = out[i].asGap();
            ok = ok && out[i].sample == next && g.droppedRecords == static_cast<uint64_t> (next) * 3u
                 && g.droppedAudioSamples == static_cast<uint64_t> (~next);
        }
    }

    producer.join();
    CHECK (ok);
}
