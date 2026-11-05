// VoicePipeline expression overload (docs/SYNTH-DESIGN.md 16.3): events + timings bit-identical with and without
// the expression buffer, one frame per hop at the hop's event offset, frame positions and values block-size
// invariant, plausible values for a sung melody. Test plan: docs/SYNTH-DESIGN.md 21. Owner: agent C.

#include "vocalsynth/VoicePipeline.h"

#include "TestSignals.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace vocalsynth;

namespace
{
constexpr std::array<int, 7> blockSizes { 1, 7, 64, 480, 512, 4096, 0 }; // 0 = varying sizes

struct TimedEvent
{
    int64_t position = 0;
    VsEvent event;
    bool operator== (const TimedEvent&) const noexcept = default;
};

struct TimedFrame
{
    int64_t position = 0;
    ExpressionFrame frame;
    bool operator== (const TimedFrame&) const noexcept = default;
};

struct Run
{
    std::vector<TimedEvent> events;
    std::vector<VsEventTiming> timings;
    std::vector<TimedFrame> frames;
    std::vector<int64_t> hopSamples;
    int offsetViolations = 0;
    int unmatchedTimings = 0;
};

void observe (void* context, const AnalysisFrameInfo& info) noexcept
{
    static_cast<Run*> (context)->hopSamples.push_back (info.hopSample);
}

int blockAt (int block, std::size_t index)
{
    constexpr std::array<int, 9> varying { 1, 300, 17, 4096, 64, 2, 511, 1000, 7 };
    return block > 0 ? block : varying[index % varying.size()];
}

Run runPipeline (const test::Signal& input, double sr, const VoiceSettings& s, int block, bool withExpression)
{
    Run run;
    VoicePipeline pipeline;
    pipeline.prepare (sr, 4096);
    pipeline.setFrameObserver (&observe, &run);
    VsEventBuffer<1024> events;
    VsEventTimingBuffer<1024> timings;
    ExpressionBuffer frames;
    std::size_t pos = 0, index = 0;

    while (pos < input.size())
    {
        const auto n = static_cast<int> (std::min<std::size_t> (static_cast<std::size_t> (blockAt (block, index++)), input.size() - pos));
        events.clear();
        timings.clear();
        frames.clear();

        if (withExpression)
            pipeline.process (input.data() + pos, n, s, events, timings, frames);
        else
            pipeline.process (input.data() + pos, n, s, events, timings);

        for (std::size_t i = 0; i < events.size(); ++i)
        {
            auto e = events[i];
            e.sampleOffset = 0; // compared by absolute position
            run.events.push_back ({ static_cast<int64_t> (pos) + events[i].sampleOffset, e });
        }

        run.unmatchedTimings += timings.size() == events.size() ? 0 : 1;

        for (std::size_t i = 0; i < timings.size(); ++i)
            run.timings.push_back (timings[i]);

        for (std::size_t i = 0; i < frames.size(); ++i)
        {
            const auto& f = frames[i];
            run.offsetViolations += f.sampleOffset >= 0 && f.sampleOffset < n ? 0 : 1;
            auto frame = f;
            frame.sampleOffset = 0;
            run.frames.push_back ({ static_cast<int64_t> (pos) + f.sampleOffset, frame });
        }

        pos += static_cast<std::size_t> (n);
    }

    return run;
}

/** A sung phrase: rest, a note, a legato step, a vibrato note, breath noise, a detuned note, rest. */
test::Signal phrase (double sr)
{
    auto a = test::toneSequence (sr, { { 0.0, 0.15 }, { test::midiToHz (57), 0.35 }, { test::midiToHz (60), 0.3 }, { 0.0, 0.2 } }, 0.3f,
                                 test::sawShape);
    const auto vib = test::vibrato (sr, test::midiToHz (64), 40.0, 5.5, 0.6, 0.25f);
    const auto breath = test::whiteNoise (sr, 0.25, 0.05f, 11);
    const auto detuned = test::sine (sr, test::midiToHz (62.3), 0.5, 0.3f);
    const auto rest = test::silence (sr, 0.3);
    return test::concat ({ a, vib, rest, breath, detuned, rest });
}

/** Expression frame nearest to an absolute position. */
const TimedFrame& frameAt (const Run& run, int64_t position)
{
    std::size_t best = 0;

    for (std::size_t i = 1; i < run.frames.size(); ++i)
        if (std::llabs (run.frames[i].position - position) < std::llabs (run.frames[best].position - position))
            best = i;

    return run.frames[best];
}
} // namespace

TEST_CASE ("VoicePipeline expression: events and timings bit-identical with and without the buffer", "[expression][pipeline]")
{
    const auto sr = GENERATE (44100.0, 48000.0, 96000.0);
    const auto config = GENERATE (0, 1);
    CAPTURE (sr, config);
    VoiceSettings s;
    s.algorithm = config == 0 ? PitchAlgorithm::yin : PitchAlgorithm::mpm;
    s.bendMode = config == 0 ? BendMode::trueBend : BendMode::anchored;
    s.legato = config == 1;
    const auto input = phrase (sr);

    for (const auto block : { 64, 512, 4096, 1 })
    {
        CAPTURE (block);
        const auto plain = runPipeline (input, sr, s, block, false);
        const auto expressive = runPipeline (input, sr, s, block, true);
        CHECK (plain.events.size() >= 4);
        CHECK (plain.events == expressive.events);
        CHECK (plain.timings == expressive.timings);
        CHECK (plain.hopSamples == expressive.hopSamples);
        CHECK (plain.frames.empty());
        CHECK (expressive.unmatchedTimings == 0);
    }
}

TEST_CASE ("VoicePipeline expression: one frame per hop at the hop's event offset", "[expression][pipeline]")
{
    const auto sr = GENERATE (44100.0, 48000.0, 96000.0);
    CAPTURE (sr);
    const auto input = phrase (sr);
    const auto run = runPipeline (input, sr, VoiceSettings {}, 480, true);
    CHECK (run.offsetViolations == 0);
    REQUIRE (run.frames.size() == run.hopSamples.size());
    REQUIRE (! run.frames.empty());

    for (std::size_t i = 0; i < run.frames.size(); ++i)
        REQUIRE (run.frames[i].position == run.hopSamples[i]);

    // 2 ms hops
    const auto spacing = static_cast<double> (run.frames.back().position - run.frames.front().position)
                         / static_cast<double> (run.frames.size() - 1);
    CHECK (std::abs (spacing / sr - 0.002) < 0.0002);
}

TEST_CASE ("VoicePipeline expression: frame positions and values are block-size invariant", "[expression][pipeline]")
{
    const auto sr = GENERATE (44100.0, 48000.0, 96000.0);
    CAPTURE (sr);
    const auto input = phrase (sr);
    const auto reference = runPipeline (input, sr, VoiceSettings {}, 4096, true);

    for (const auto block : blockSizes)
    {
        CAPTURE (block);
        const auto run = runPipeline (input, sr, VoiceSettings {}, block, true);
        CHECK (run.offsetViolations == 0);
        CHECK (run.frames == reference.frames);
        CHECK (run.events == reference.events);
    }
}

TEST_CASE ("VoicePipeline expression: plausible values for a sung phrase, reset restarts the tracker", "[expression][pipeline]")
{
    const double sr = 48000.0;
    const auto input = phrase (sr);
    const auto run = runPipeline (input, sr, VoiceSettings {}, 512, true);
    const auto at = [&] (double seconds) { return frameAt (run, static_cast<int64_t> (seconds * sr)).frame; };

    CHECK (at (0.05).level == 0.0f);          // rest before the first note
    CHECK (at (0.40).level > 0.5f);           // the saw note, -10 dBFS
    CHECK (at (0.40).brightness > at (1.20).brightness); // saw brighter than the vibrato sine
    CHECK (at (0.40).noisiness < 0.3f);
    CHECK (at (1.88).level < 0.02f);          // rest after the vibrato (50 ms release)
    CHECK (std::abs (at (2.30).pitchCents - 30.0f) < 8.0f); // the note sung 30 cents sharp of D4

    // Every value in range
    for (const auto& f : run.frames)
    {
        REQUIRE (f.frame.level >= 0.0f);
        REQUIRE (f.frame.level <= 1.0f);
        REQUIRE (f.frame.brightness >= 0.0f);
        REQUIRE (f.frame.brightness <= 1.0f);
        REQUIRE (f.frame.noisiness >= 0.0f);
        REQUIRE (f.frame.noisiness <= 1.0f);
        REQUIRE (std::abs (f.frame.pitchCents) <= 100.0f);
    }

    // reset(): the same input gives the same frames again
    VoicePipeline pipeline;
    pipeline.prepare (sr, 4096);
    VsEventBuffer<1024> events;
    VsEventTimingBuffer<1024> timings;
    ExpressionBuffer first, second;
    pipeline.process (input.data(), 4096, VoiceSettings {}, events, timings, first);
    pipeline.reset();
    events.clear();
    timings.clear();
    pipeline.process (input.data(), 4096, VoiceSettings {}, events, timings, second);
    REQUIRE (first.size() == second.size());

    for (std::size_t i = 0; i < first.size(); ++i)
        REQUIRE (first[i] == second[i]);
}
