#include "vocalsynth/PitchPostProcessor.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <limits>
#include <vector>

using Catch::Matchers::WithinRel;
using namespace vocalsynth;

namespace
{

constexpr double frameRate = 500.0; // 2 ms hop (docs/M2-DESIGN.md 3.4)

PitchFrame voiced (float hz, float confidence = 0.95f, float rmsDb = -20.0f) { return { hz, confidence, rmsDb }; }
PitchFrame unvoiced (float rmsDb = -60.0f) { return { 0.0f, 0.0f, rmsDb }; }

std::vector<PitchFrame> run (PitchPostProcessor& post, const std::vector<PitchFrame>& in)
{
    std::vector<PitchFrame> out;

    for (const auto& f : in)
        out.push_back (post.process (f));

    return out;
}

std::vector<PitchFrame> repeat (const PitchFrame& f, int n) { return std::vector<PitchFrame> (static_cast<std::size_t> (n), f); }

std::vector<PitchFrame> operator+ (std::vector<PitchFrame> a, const std::vector<PitchFrame>& b)
{
    a.insert (a.end(), b.begin(), b.end());
    return a;
}

} // namespace

TEST_CASE ("PitchPostProcessor: median length is odd and bounded", "[postprocessor]")
{
    PitchPostProcessor post;
    post.prepare (frameRate);
    CHECK (post.getMedianLength() == 3);
    CHECK (post.getDelayFrames() == 1);
    post.setMedianLength (4);
    CHECK (post.getMedianLength() == 3);
    post.setMedianLength (9);
    CHECK (post.getMedianLength() == 5);
    CHECK (post.getDelayFrames() == 2);
    post.setMedianLength (0);
    CHECK (post.getMedianLength() == 1);
    CHECK (post.getDelayFrames() == 0);
}

TEST_CASE ("PitchPostProcessor: median of 3 removes single outliers and votes on voicing", "[postprocessor]")
{
    PitchPostProcessor post;
    post.prepare (frameRate);

    // Missing history counts as unvoiced: the first voiced frame alone is not enough (1 frame delay)
    auto out = run (post, { voiced (220.0f), voiced (220.0f), voiced (220.0f) });
    CHECK (out[0].frequencyHz == 0.0f);
    CHECK (out[1].frequencyHz == 220.0f);
    CHECK (out[2].frequencyHz == 220.0f);

    // A single outlier (not an octave) is removed, the pitch around it kept
    out = run (post, { voiced (220.0f), voiced (300.0f), voiced (221.0f), voiced (222.0f) });
    CHECK (out[1].frequencyHz == 220.0f);
    CHECK (out[2].frequencyHz == 221.0f);

    // A single unvoiced dropout is filled, two unvoiced frames win the vote
    post.reset();
    out = run (post, { voiced (223.0f), voiced (223.0f), voiced (223.0f), unvoiced(), voiced (223.0f), unvoiced(), unvoiced() });
    CHECK (out[0].frequencyHz == 0.0f);
    CHECK (out[1].frequencyHz == 223.0f);
    CHECK (out[3].frequencyHz == 223.0f);
    CHECK (out[4].frequencyHz == 223.0f); // dropout filled
    CHECK (out[5].frequencyHz == 0.0f);   // {0, 223, 0}
    CHECK (out[6].frequencyHz == 0.0f);

    // A single voiced blip in silence never gets through
    post.reset();
    out = run (post, repeat (unvoiced(), 5) + std::vector { voiced (440.0f) } + repeat (unvoiced(), 5));

    for (const auto& f : out)
        CHECK (f.frequencyHz == 0.0f);
}

TEST_CASE ("PitchPostProcessor: confidence median and rmsDb of the newest frame", "[postprocessor]")
{
    PitchPostProcessor post;
    post.prepare (frameRate);

    const auto out = run (post, { voiced (220.0f, 0.9f, -30.0f), voiced (220.0f, 0.2f, -25.0f), voiced (220.0f, 0.85f, -10.0f),
                                  voiced (220.0f, 0.95f, -12.0f) });
    CHECK (out[2].confidence == 0.85f); // median of 0.9, 0.2, 0.85
    CHECK (out[3].confidence == 0.85f); // median of 0.2, 0.85, 0.95

    for (std::size_t i = 0; i < out.size(); ++i)
        CHECK (out[i].rmsDb == std::vector { -30.0f, -25.0f, -10.0f, -12.0f }[i]);

    // NaN fields from a broken input never propagate
    post.reset();
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    const auto broken = run (post, { voiced (nan, nan), voiced (nan, nan), voiced (nan, nan) });

    for (const auto& f : broken)
    {
        CHECK (f.frequencyHz == 0.0f);
        CHECK (std::isfinite (f.confidence));
    }
}

TEST_CASE ("PitchPostProcessor: median of 5", "[postprocessor]")
{
    PitchPostProcessor post;
    post.prepare (frameRate);
    post.setMedianLength (5);

    // Two consecutive outliers are removed with 5 (not with 3)
    const auto out = run (post, { voiced (200.0f), voiced (201.0f), voiced (202.0f), voiced (300.0f), voiced (301.0f),
                                  voiced (203.0f), voiced (204.0f), voiced (205.0f) });
    CHECK (out[0].frequencyHz == 0.0f);
    CHECK (out[1].frequencyHz == 0.0f);
    CHECK (out[2].frequencyHz == 200.0f); // {0, 0, 200, 201, 202}
    CHECK (out[3].frequencyHz == 201.0f);
    CHECK (out[4].frequencyHz == 202.0f);
    CHECK (out[5].frequencyHz == 203.0f);
    CHECK (out[6].frequencyHz == 204.0f);
    CHECK (out[7].frequencyHz == 205.0f); // {300, 301, 203, 204, 205}
}

TEST_CASE ("PitchPostProcessor: octave guard corrects glitches shorter than 25 ms", "[postprocessor]")
{
    PitchPostProcessor post;
    post.prepare (frameRate);
    post.setMedianLength (1); // isolate the guard from the median
    const auto confirmFrames = static_cast<int> (std::ceil (PitchPostProcessor::octaveConfirmMs * 0.001 * frameRate));
    REQUIRE (confirmFrames == 13); // docs/M2-DESIGN.md 3.6: 13 frames at 2 ms

    for (const auto factor : { 2.0f, 0.5f, 2.05f, 0.49f })
    {
        CAPTURE (factor);
        post.reset();

        // 12 octave-off frames (24 ms) between stable frames: all output in the original octave
        const auto out = run (post, repeat (voiced (220.0f), 10) + repeat (voiced (220.0f * factor), confirmFrames - 1)
                                        + repeat (voiced (220.0f), 10));

        for (const auto& f : out)
            CHECK_THAT (f.frequencyHz, WithinRel (220.0f, 0.03f));
    }
}

TEST_CASE ("PitchPostProcessor: sustained octave leap accepted after 25 ms", "[postprocessor]")
{
    PitchPostProcessor post;
    post.prepare (frameRate);
    post.setMedianLength (1);
    const int confirmFrames = 13;

    for (const auto factor : { 2.0f, 0.5f })
    {
        CAPTURE (factor);
        post.reset();
        const auto out = run (post, repeat (voiced (220.0f), 10) + repeat (voiced (220.0f * factor), 40));

        for (int i = 10; i < 10 + confirmFrames - 1; ++i)
            CHECK (out[static_cast<std::size_t> (i)].frequencyHz == 220.0f); // corrected: f / 2 or f * 2

        for (int i = 10 + confirmFrames - 1; i < 50; ++i)
            CHECK (out[static_cast<std::size_t> (i)].frequencyHz == 220.0f * factor); // accepted, stays
    }

    // A non-octave jump is never touched
    post.reset();
    const auto out = run (post, repeat (voiced (220.0f), 5) + repeat (voiced (330.0f), 5));
    CHECK (out[5].frequencyHz == 330.0f);

    // Confidence and rmsDb of a corrected frame are unchanged
    post.reset();
    const auto corrected = run (post, { voiced (220.0f), voiced (440.0f, 0.83f, -17.0f) });
    CHECK (corrected[1].frequencyHz == 220.0f);
    CHECK (corrected[1].confidence == 0.83f);
    CHECK (corrected[1].rmsDb == -17.0f);
}

TEST_CASE ("PitchPostProcessor: suspect run must be consecutive and in one direction", "[postprocessor]")
{
    PitchPostProcessor post;
    post.prepare (frameRate);
    post.setMedianLength (1);

    // Alternating up/down glitches never accumulate into an accepted octave
    std::vector<PitchFrame> in = repeat (voiced (220.0f), 5);

    for (int i = 0; i < 30; ++i)
        in.push_back (voiced (i % 2 == 0 ? 440.0f : 110.0f));

    for (const auto& f : run (post, in))
        CHECK (f.frequencyHz == 220.0f);

    // A short unvoiced gap inside a glitch restarts the count
    post.reset();
    const auto out = run (post, repeat (voiced (220.0f), 5) + repeat (voiced (440.0f), 8) + repeat (unvoiced(), 3)
                                    + repeat (voiced (440.0f), 8));

    for (const auto& f : out)
        CHECK ((f.frequencyHz == 220.0f || f.frequencyHz == 0.0f));
}

TEST_CASE ("PitchPostProcessor: octave memory is forgotten after 60 ms without voice", "[postprocessor]")
{
    PitchPostProcessor post;
    post.prepare (frameRate);
    post.setMedianLength (1);
    const auto memoryFrames = static_cast<int> (std::ceil (PitchPostProcessor::octaveMemoryMs * 0.001 * frameRate));
    REQUIRE (memoryFrames == 30);

    // Gap shorter than the memory: the new phrase an octave up is still corrected
    auto out = run (post, repeat (voiced (220.0f), 10) + repeat (unvoiced(), memoryFrames - 1) + repeat (voiced (440.0f), 3));
    CHECK (out.back().frequencyHz == 220.0f);

    // Gap of the memory length: a new phrase may start in any octave, never corrected or delayed
    post.reset();
    out = run (post, repeat (voiced (220.0f), 10) + repeat (unvoiced(), memoryFrames) + repeat (voiced (440.0f), 3));
    CHECK (out[static_cast<std::size_t> (10 + memoryFrames)].frequencyHz == 440.0f);
    CHECK (out.back().frequencyHz == 440.0f);

    // Low-confidence "pitches" (YIN on noise) count as unvoiced: they neither keep nor set a reference
    post.reset();
    out = run (post, repeat (voiced (220.0f), 10) + repeat (voiced (447.0f, 0.3f), memoryFrames) + repeat (voiced (440.0f), 3));
    CHECK (out[12].frequencyHz == 447.0f); // passed through unchanged
    CHECK (out.back().frequencyHz == 440.0f);
}

TEST_CASE ("PitchPostProcessor: onset is never delayed by the guard; reset forgets everything", "[postprocessor]")
{
    PitchPostProcessor post;
    post.prepare (frameRate);

    // Median 3: an onset at frame 0 is out at frame 1, in whatever octave it starts
    auto out = run (post, repeat (voiced (523.0f), 4));
    CHECK (out[0].frequencyHz == 0.0f);
    CHECK (out[1].frequencyHz == 523.0f);

    // reset(): the previous reference (523 Hz) is gone, 261.5 Hz is a fresh onset
    post.reset();
    out = run (post, repeat (voiced (261.5f), 3));
    CHECK (out[1].frequencyHz == 261.5f);

    // Without reset it would have been corrected
    out = run (post, repeat (voiced (523.0f), 3));
    CHECK (out[2].frequencyHz == 261.5f);

    // prepare() converts the ms constants for other frame rates (at least 1 frame)
    post.prepare (1.0);
    post.setMedianLength (1);
    out = run (post, { voiced (220.0f), voiced (440.0f) });
    CHECK (out[1].frequencyHz == 440.0f); // confirm = 1 frame at 1 frame/s
}
