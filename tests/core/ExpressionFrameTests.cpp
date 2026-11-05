// ExpressionBuffer / ExpressionPublisher / restExpression.
// Test plan: docs/SYNTH-DESIGN.md 21. Owner: agent C (Engine + FX + Expression).

#include "vocalsynth/ExpressionFrame.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>

using namespace vocalsynth;

TEST_CASE ("ExpressionBuffer: fixed capacity, push order, clear", "[expression]")
{
    static_assert (ExpressionBuffer::capacity >= 257, "one frame per 2 ms hop of a 4096 chunk at 8 kHz, plus one");
    ExpressionBuffer buffer;
    CHECK (buffer.empty());

    for (std::size_t i = 0; i < ExpressionBuffer::capacity; ++i)
        REQUIRE (buffer.push ({ static_cast<int> (i), 0.5f, 0.25f, 0.125f, -3.0f }));

    CHECK (buffer.size() == ExpressionBuffer::capacity);
    CHECK_FALSE (buffer.push ({}));
    CHECK (buffer.size() == ExpressionBuffer::capacity);
    CHECK (buffer[0].sampleOffset == 0);
    CHECK (buffer[511].sampleOffset == 511);
    CHECK (buffer.view().size() == ExpressionBuffer::capacity);
    CHECK (buffer.view()[3] == ExpressionFrame { 3, 0.5f, 0.25f, 0.125f, -3.0f });

    buffer.clear();
    CHECK (buffer.empty());
    CHECK (buffer.view().empty());
    CHECK (buffer.push ({ 7, 1.0f, 0.0f, 0.0f, 0.0f }));
    CHECK (buffer.size() == 1);
}

TEST_CASE ("restExpression: level 1 while a note sounds, everything else 0", "[expression]")
{
    constexpr auto held = restExpression (true);
    constexpr auto silent = restExpression (false);
    STATIC_CHECK (held.level == 1.0f);
    STATIC_CHECK (held.brightness == 0.0f);
    STATIC_CHECK (held.noisiness == 0.0f);
    STATIC_CHECK (held.pitchCents == 0.0f);
    STATIC_CHECK (silent == ExpressionFrame {});
}

TEST_CASE ("ExpressionPublisher: round trip, readable from another thread", "[expression]")
{
    ExpressionPublisher publisher;
    CHECK (publisher.read() == ExpressionFrame {});
    publisher.publish ({ 123, 0.75f, 0.5f, 0.25f, -42.0f });
    const auto f = publisher.read();
    CHECK (f.sampleOffset == 0); // the offset is not published
    CHECK (f.level == 0.75f);
    CHECK (f.brightness == 0.5f);
    CHECK (f.noisiness == 0.25f);
    CHECK (f.pitchCents == -42.0f);

    // Single writer, concurrent reader: every value read is one that was written
    std::atomic<bool> done { false };
    std::atomic<int> bad { 0 };
    std::thread reader ([&]
                        {
                            while (! done.load())
                            {
                                const auto r = publisher.read();

                                if (r.level < 0.0f || r.level > 1.0f || r.pitchCents < -100.0f || r.pitchCents > 100.0f)
                                    bad.fetch_add (1);
                            }
                        });

    for (int i = 0; i < 100000; ++i)
        publisher.publish ({ 0, static_cast<float> (i % 101) / 100.0f, 0.0f, 0.0f, static_cast<float> (i % 201 - 100) });

    done.store (true);
    reader.join();
    CHECK (bad.load() == 0);
}
