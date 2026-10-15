#include "vocalsynth/Framer.h"

#include "vocalsynth/VoiceSettings.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

using namespace vocalsynth;

namespace
{

/** Value of the n-th pushed sample (1-based push number), exactly representable in float. */
float sampleValue (int64_t pushNumber) { return static_cast<float> (pushNumber % 100000) + 0.5f; }

/** Pushes `total` samples in blocks of `blockSize`, returns the push numbers that completed a hop and
    checks every frame's contents (newest W samples, oldest first, zeros before the first push). */
std::vector<int64_t> runFramer (Framer& framer, int64_t total, int blockSize)
{
    std::vector<int64_t> hops;
    int64_t pushed = 0;

    while (pushed < total)
    {
        const auto len = std::min<int64_t> (blockSize, total - pushed);

        for (int64_t i = 0; i < len; ++i)
        {
            ++pushed;

            if (! framer.pushSample (sampleValue (pushed)))
                continue;

            hops.push_back (pushed);
            const auto window = framer.getWindow();
            const auto w = static_cast<int64_t> (framer.getWindowSamples());
            REQUIRE (static_cast<int64_t> (window.size()) == w);

            for (int64_t j = 0; j < w; ++j)
            {
                const auto number = pushed - w + 1 + j; // push number of window[j]
                REQUIRE (window[static_cast<std::size_t> (j)] == (number >= 1 ? sampleValue (number) : 0.0f));
            }
        }
    }

    return hops;
}

} // namespace

TEST_CASE ("Framer hop and window sizes", "[framer]")
{
    // 2 ms hop (docs/M2-DESIGN.md 3.4)
    CHECK (Framer::hopSamplesFor (16000.0) == 32);
    CHECK (Framer::hopSamplesFor (22050.0) == 44);
    CHECK (Framer::hopSamplesFor (17640.0) == 35);
    CHECK (Framer::hopSamplesFor (0.0) == 1);
    CHECK (Framer::hopSamplesFor (100.0) == 1);

    // W = 2 * (ceil (fa / f0min) + 1): 2 periods of f0 min plus the guard lag of the edge rule
    CHECK (Framer::maxLagFor (16000.0, 110.0) == 147);
    CHECK (Framer::windowSamplesFor (16000.0, 110.0) == 294);
    CHECK (Framer::windowSamplesFor (16000.0, 65.0) == 496);
    CHECK (Framer::windowSamplesFor (22050.0, 65.0) == 682);
    CHECK (Framer::maxLagFor (16000.0, 0.0) == 2);

    // Design table (Bass/Wide, Baritone, Tenor, Alto, Soprano, Whistle at 16 and 22.05 kHz)
    const int expected16[] = { 496, 394, 294, 220, 166, 66, 496 };
    const int expected22[] = { 682, 540, 404, 302, 228, 92, 682 };

    for (int r = 0; r < numVoiceRanges; ++r)
    {
        const auto f0Min = voiceRangeTable[static_cast<std::size_t> (r)].f0MinHz;
        CHECK (Framer::windowSamplesFor (16000.0, f0Min) == expected16[r]);
        CHECK (Framer::windowSamplesFor (22050.0, f0Min) == expected22[r]);
    }

    for (const auto fa : { 16000.0, 17640.0, 22050.0, 24000.0 })
        for (const auto f0 : { 65.0, 82.0, 110.0, 147.0, 196.0, 500.0 })
        {
            const auto maxLag = Framer::maxLagFor (fa, f0);
            CHECK (Framer::windowSamplesFor (fa, f0) == 2 * maxLag);
            CHECK (static_cast<double> (maxLag - 1) >= fa / f0); // the f0-min period is an interior lag
            CHECK (Framer::windowSamplesFor (fa, f0) >= 2.0 * fa / f0); // >= 2 periods (spec)
        }
}

TEST_CASE ("Framer starts zero-filled and frames hold the newest W samples, oldest first", "[framer]")
{
    const auto fa = GENERATE (16000.0, 17640.0, 22050.0);
    CAPTURE (fa);

    Framer framer;
    framer.prepare (fa, Framer::windowSamplesFor (fa, lowestF0MinHz));
    CHECK (framer.getWindowSamples() == framer.getMaxWindowSamples());

    // Before any push the window is all zeros
    const auto initial = framer.getWindow();
    REQUIRE (static_cast<int> (initial.size()) == framer.getWindowSamples());
    CHECK (std::all_of (initial.begin(), initial.end(), [] (float x) { return x == 0.0f; }));

    // Several ring wraps, with every range's window length
    for (const auto& range : voiceRangeTable)
    {
        CAPTURE (range.name);
        framer.reset();
        framer.setWindowSamples (Framer::windowSamplesFor (fa, range.f0MinHz));
        const auto hops = runFramer (framer, 5 * framer.getMaxWindowSamples() + 17, 64);
        REQUIRE (! hops.empty());
        CHECK (hops.front() == framer.getHopSamples()); // the first frame is not skipped (leading zeros)
    }
}

TEST_CASE ("Framer hop clock is independent of the push pattern", "[framer]")
{
    const auto fa = GENERATE (16000.0, 17640.0, 22050.0);
    CAPTURE (fa);

    const auto maxWindow = Framer::windowSamplesFor (fa, lowestF0MinHz);
    const int64_t total = 20000;

    Framer reference;
    reference.prepare (fa, maxWindow);
    const auto expected = runFramer (reference, total, static_cast<int> (total));

    // Hops complete on push numbers m * hop, m = 1, 2, ...
    const auto hop = reference.getHopSamples();
    REQUIRE (expected.size() == static_cast<std::size_t> (total / hop));

    for (std::size_t m = 0; m < expected.size(); ++m)
        REQUIRE (expected[m] == static_cast<int64_t> (m + 1) * hop);

    for (const int blockSize : { 1, 7, 64, 480, 512, 4096 })
    {
        CAPTURE (blockSize);
        Framer framer;
        framer.prepare (fa, maxWindow);
        CHECK (runFramer (framer, total, blockSize) == expected);
        CHECK (framer.getNumSamplesPushed() == total);
    }
}

TEST_CASE ("Framer range change keeps the ring contents and the hop phase", "[framer]")
{
    const auto fa = 16000.0;
    Framer framer;
    framer.prepare (fa, Framer::windowSamplesFor (fa, lowestF0MinHz));
    framer.setWindowSamples (Framer::windowSamplesFor (fa, 110.0)); // Tenor
    const auto hop = framer.getHopSamples();

    int64_t pushed = 0;
    std::vector<int64_t> hops;

    // Switch the window length at awkward moments (mid-hop) and check every frame
    const int windows[] = { 294, 496, 66, 166, 496, 394 };

    for (const auto w : windows)
    {
        framer.setWindowSamples (w);
        CHECK (framer.getWindowSamples() == w);

        for (int i = 0; i < 3 * hop + 5; ++i)
        {
            ++pushed;

            if (! framer.pushSample (sampleValue (pushed)))
                continue;

            hops.push_back (pushed);
            const auto window = framer.getWindow();
            REQUIRE (static_cast<int> (window.size()) == w);
            CHECK (window.back() == sampleValue (pushed));

            const auto oldest = pushed - w + 1;
            CHECK (window.front() == (oldest >= 1 ? sampleValue (oldest) : 0.0f));
        }
    }

    for (const auto h : hops)
        CHECK (h % hop == 0);

    // Out-of-range lengths are clamped
    framer.setWindowSamples (100000);
    CHECK (framer.getWindowSamples() == framer.getMaxWindowSamples());
    framer.setWindowSamples (0);
    CHECK (framer.getWindowSamples() == 1);
}

TEST_CASE ("Framer reset restarts the clock and zeroes the ring", "[framer]")
{
    Framer framer;
    framer.prepare (22050.0, 682);

    for (int i = 0; i < 1000; ++i)
        framer.pushSample (1.0f);

    framer.reset();
    CHECK (framer.getNumSamplesPushed() == 0);
    const auto window = framer.getWindow();
    CHECK (std::all_of (window.begin(), window.end(), [] (float x) { return x == 0.0f; }));

    const auto hops = runFramer (framer, 1000, 33);
    REQUIRE (! hops.empty());
    CHECK (hops.front() == framer.getHopSamples());

    // An unprepared framer never crashes and returns an empty window
    Framer unprepared;
    unprepared.pushSample (1.0f);
    CHECK (unprepared.getWindow().empty());
}
