#include "vocalsynth/BendGenerator.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <vector>

using namespace vocalsynth;

namespace
{

/** A bend event with the absolute sample of the call that produced it. */
struct TimedBend
{
    int64_t position;
    int sampleOffset;
    uint8_t channel;
    int16_t value;
};

/** Drives a BendGenerator on a frame grid (hop host samples apart) like the NoteSegmenter does and
    records the emitted bends. Offsets are simulated for blocks of blockSize samples. */
struct BendRig
{
    double sampleRate = 48000.0;
    int hop = 96; // 2 ms at 48 kHz (docs/M2-DESIGN.md 3.4)
    int blockSize = 512;
    BendGenerator bend;
    int64_t position = 0;
    std::vector<TimedBend> events;

    BendRig (double sr, int hopSamples, BendMode mode, BendRange range = BendRange::semitones2, int block = 512)
        : sampleRate (sr), hop (hopSamples), blockSize (block)
    {
        bend.prepare (sampleRate, static_cast<double> (hop) / sampleRate);
        bend.setMode (mode, range);
    }

    int offset() const { return static_cast<int> (position % blockSize); }

    template <typename Fn>
    void call (Fn&& fn)
    {
        VsEventBuffer<8> buffer;
        fn (VsEventSink (buffer));

        for (const auto& e : buffer)
        {
            REQUIRE (e.type == VsEventType::pitchBend);
            REQUIRE (e.sampleOffset == offset());
            events.push_back ({ position, e.sampleOffset, e.channel, e.bend });
        }
    }

    void start (double cents, uint8_t channel = 1)
    {
        call ([&] (VsEventSink out) { bend.noteStarted (cents, position, offset(), channel, out); });
    }

    void update (double cents, uint8_t channel = 1)
    {
        call ([&] (VsEventSink out) { bend.update (cents, position, offset(), channel, out); });
    }

    void end (uint8_t channel = 1)
    {
        call ([&] (VsEventSink out) { bend.noteEnded (position, offset(), channel, out); });
    }

    void advance (int frames = 1) { position += static_cast<int64_t> (frames) * hop; }
};

} // namespace

TEST_CASE ("Bend value mapping", "[bendgenerator]")
{
    CHECK (BendGenerator::centsToBend (0.0, 2) == 0);
    CHECK (BendGenerator::centsToBend (200.0, 2) == 8191);
    CHECK (BendGenerator::centsToBend (-200.0, 2) == -8192);
    CHECK (BendGenerator::shapeCents (BendMode::anchored, 20.0) == 0.0);
}

TEST_CASE ("Bend value mapping for every range, 14-bit with centre 0 (= 8192 on the wire)", "[bendgenerator]")
{
    for (auto range : { BendRange::semitones2, BendRange::semitones12, BendRange::semitones24, BendRange::semitones48 })
    {
        const auto semis = bendRangeSemitones (range);
        const auto full = 100.0 * semis;
        CAPTURE (semis);

        CHECK (BendGenerator::centsToBend (0.0, semis) == 0);
        CHECK (BendGenerator::centsToBend (full, semis) == 8191);
        CHECK (BendGenerator::centsToBend (-full, semis) == -8192);
        CHECK (BendGenerator::centsToBend (full * 0.5, semis) == 4096);   // lround (4095.5)
        CHECK (BendGenerator::centsToBend (-full * 0.5, semis) == -4096);
        CHECK (BendGenerator::centsToBend (full * 3.0, semis) == 8191);   // clamped
        CHECK (BendGenerator::centsToBend (-full * 3.0, semis) == -8192);

        // Monotone and inside [-8192, 8191] over the whole range; the wire value 8192 + bend fits 14 bits
        int previous = -8193;

        for (int c = -2 * static_cast<int> (full); c <= 2 * static_cast<int> (full); ++c)
        {
            const auto v = BendGenerator::centsToBend (static_cast<double> (c), semis);
            REQUIRE (v >= previous);
            REQUIRE (v >= -8192);
            REQUIRE (v <= 8191);
            REQUIRE (8192 + v >= 0);
            REQUIRE (8192 + v <= 16383);
            previous = v;
        }
    }

    CHECK (bendRangeSemitones (BendRange::semitones2) == 2);
    CHECK (bendRangeSemitones (BendRange::semitones12) == 12);
    CHECK (bendRangeSemitones (BendRange::semitones24) == 24);
    CHECK (bendRangeSemitones (BendRange::semitones48) == 48);

    // Typical values: +-50 cents vibrato at +-2 st ~ +-2048, one semitone at +-12, one cent at +-48
    CHECK (BendGenerator::centsToBend (50.0, 2) == 2048);  // 8191 / 4 = 2047.75
    CHECK (BendGenerator::centsToBend (-50.0, 2) == -2048);
    CHECK (BendGenerator::centsToBend (100.0, 12) == 683); // 8191 / 12 = 682.58
    CHECK (BendGenerator::centsToBend (-100.0, 12) == -683);
    CHECK (BendGenerator::centsToBend (100.0, 24) == 341);
    CHECK (BendGenerator::centsToBend (1.0, 48) == 2);     // 8191 / 4800 = 1.71

    // Invalid input
    CHECK (BendGenerator::centsToBend (std::numeric_limits<double>::quiet_NaN(), 2) == 0);
    CHECK (BendGenerator::centsToBend (std::numeric_limits<double>::infinity(), 2) == 0);
    CHECK (BendGenerator::centsToBend (100.0, 0) == 0);
    CHECK (BendGenerator::centsToBend (100.0, -2) == 0);
}

TEST_CASE ("Bend shaping: off, anchored dead zone, true 1:1", "[bendgenerator]")
{
    for (double r = -300.0; r <= 300.0; r += 0.5)
    {
        CHECK (BendGenerator::shapeCents (BendMode::off, r) == 0.0);
        CHECK (BendGenerator::shapeCents (BendMode::trueBend, r) == r);
    }

    CHECK (BendGenerator::shapeCents (BendMode::anchored, 0.0) == 0.0);
    CHECK (BendGenerator::shapeCents (BendMode::anchored, 30.0) == 0.0);
    CHECK (BendGenerator::shapeCents (BendMode::anchored, -30.0) == 0.0);
    CHECK (BendGenerator::shapeCents (BendMode::anchored, 31.0) == 1.0);
    CHECK (BendGenerator::shapeCents (BendMode::anchored, -45.0) == -15.0);
    CHECK (BendGenerator::shapeCents (BendMode::anchored, 130.0) == 100.0);

    // Continuous at the dead-zone edge, odd symmetric, never larger than the residual
    double previous = BendGenerator::shapeCents (BendMode::anchored, -100.0);

    for (double r = -100.0; r <= 100.0; r += 0.01)
    {
        const auto c = BendGenerator::shapeCents (BendMode::anchored, r);
        CHECK (std::abs (c - previous) < 0.0101);
        CHECK (c == -BendGenerator::shapeCents (BendMode::anchored, -r));
        CHECK (std::abs (c) <= std::abs (r));
        previous = c;
    }
}

TEST_CASE ("Bend: note start emits only a changed value, note end returns to centre", "[bendgenerator]")
{
    SECTION ("off: never emits")
    {
        BendRig rig (48000.0, 96, BendMode::off);
        rig.start (80.0);

        for (int i = 0; i < 100; ++i)
        {
            rig.advance();
            rig.update (std::sin (i * 0.3) * 150.0);
        }

        rig.advance();
        rig.end();
        CHECK (rig.events.empty());
        CHECK (rig.bend.getLastValue() == 0);
    }

    SECTION ("true: start value, end centre, both not rate limited")
    {
        BendRig rig (48000.0, 96, BendMode::trueBend);
        rig.start (50.0, 3);
        REQUIRE (rig.events.size() == 1);
        CHECK (rig.events[0].value == 2048);
        CHECK (rig.events[0].channel == 3);
        CHECK (rig.events[0].position == 0);

        rig.end (3); // same sample: not rate limited
        REQUIRE (rig.events.size() == 2);
        CHECK (rig.events[1].value == 0);
        CHECK (rig.events[1].position == 0);
        CHECK (rig.bend.getLastValue() == 0);

        rig.end (3); // already centred: nothing
        CHECK (rig.events.size() == 2);

        rig.start (0.0); // centre at start: nothing to send
        CHECK (rig.events.size() == 2);
    }

    SECTION ("true: a note change sends the new residual immediately")
    {
        BendRig rig (48000.0, 96, BendMode::trueBend);
        rig.start (20.0);
        rig.advance();
        rig.start (-30.0); // change of note in the next frame: not limited by the 240-sample interval
        REQUIRE (rig.events.size() == 2);
        CHECK (rig.events[1].value == BendGenerator::centsToBend (-30.0, 2));
        CHECK (rig.events[1].position == 96);
        rig.start (-30.0); // same value: nothing
        CHECK (rig.events.size() == 2);
    }

    SECTION ("anchored: inside the dead zone nothing is sent, the smoother starts at the target")
    {
        BendRig rig (48000.0, 96, BendMode::anchored);
        rig.start (25.0);
        CHECK (rig.events.empty());

        rig.advance();
        rig.start (90.0); // shaped 60 cents, no glide-in
        REQUIRE (rig.events.size() == 1);
        CHECK (rig.events[0].value == BendGenerator::centsToBend (60.0, 2));

        for (int i = 0; i < 50; ++i)
        {
            rig.advance();
            rig.update (90.0); // steady: the smoother is already there
        }

        CHECK (rig.events.size() == 1);
        rig.advance();
        rig.end();
        REQUIRE (rig.events.size() == 2);
        CHECK (rig.events[1].value == 0);
    }
}

TEST_CASE ("Bend: true mode is 1:1 for every range", "[bendgenerator]")
{
    const auto range = GENERATE (BendRange::semitones2, BendRange::semitones12, BendRange::semitones24,
                                 BendRange::semitones48);
    const auto semis = bendRangeSemitones (range);
    BendRig rig (48000.0, 96, BendMode::trueBend, range);

    // 240-sample interval vs 96-sample hops: update every 3rd frame (288 samples) so none is limited
    rig.start (0.0);
    int expectedEvents = 0;

    for (int i = 1; i <= 200; ++i)
    {
        rig.advance (3);
        const auto cents = 100.0 * static_cast<double> (semis) * 0.9 * std::sin (static_cast<double> (i) * 0.37);
        const auto before = rig.events.size();
        rig.update (cents);
        const auto expected = BendGenerator::centsToBend (cents, semis);

        CHECK (rig.bend.getLastValue() == expected);

        if (rig.events.size() > before)
        {
            ++expectedEvents;
            CHECK (rig.events.back().value == expected);
        }
    }

    CHECK (static_cast<int> (rig.events.size()) == expectedEvents);
    CHECK (expectedEvents > 190);
}

TEST_CASE ("Bend: anchored mode smooths the shaped residual with a 30 ms one-pole", "[bendgenerator]")
{
    const double sr = GENERATE (44100.0, 48000.0, 96000.0);
    const int hop = sr == 44100.0 ? 88 : (sr == 48000.0 ? 96 : 192); // 2 ms hops (docs/M2-DESIGN.md 3.4)
    const auto a = std::exp (-(static_cast<double> (hop) / sr) / 0.030);
    BendRig rig (sr, hop, BendMode::anchored);
    CAPTURE (sr);

    rig.start (0.0);
    double smoothed = 0.0;
    int lastEmittedFrame = -1000;
    const auto minInterval = std::lround (sr / 200.0);

    // Step from 0 to 110 cents (shaped 80) at frame 0, then back to 10 cents (shaped 0) at frame 100;
    // 300 frames later the smoother is below half a bend step, i.e. exactly at the centre again
    for (int frame = 0; frame < 400; ++frame)
    {
        rig.advance();
        const auto residual = frame < 100 ? 110.0 : 10.0;
        const auto target = frame < 100 ? 80.0 : 0.0;
        smoothed = a * smoothed + (1.0 - a) * target;
        const auto expected = BendGenerator::centsToBend (smoothed, 2);
        const auto before = rig.events.size();
        const auto lastValue = rig.bend.getLastValue();
        rig.update (residual);

        const bool allowed = static_cast<long> (frame - lastEmittedFrame) * hop >= minInterval;

        if (expected != lastValue && allowed)
        {
            REQUIRE (rig.events.size() == before + 1);
            CHECK (rig.events.back().value == expected);
            lastEmittedFrame = frame;
        }
        else
        {
            REQUIRE (rig.events.size() == before);
        }
    }

    // Time constant: the bend crosses 1 - 1/e of the step 30 ms after the step (the first smoothed frame
    // is one hop after the step's start position 0); later only by the rate limit
    const auto crossValue = BendGenerator::centsToBend (80.0 * (1.0 - std::exp (-1.0)), 2);
    int64_t crossPosition = -1;

    for (const auto& e : rig.events)
    {
        if (e.value >= crossValue)
        {
            crossPosition = e.position;
            break;
        }
    }

    const auto tauSamples = 0.030 * sr;
    REQUIRE (crossPosition > 0);
    CHECK (static_cast<double> (crossPosition) >= tauSamples - static_cast<double> (hop));
    CHECK (static_cast<double> (crossPosition) <= tauSamples + static_cast<double> (minInterval + hop));

    // Smoothed values rise monotonically during the step and return to exactly centre eventually
    for (std::size_t i = 1; i < rig.events.size(); ++i)
        if (rig.events[i].position < 101 * hop)
            CHECK (rig.events[i].value > rig.events[i - 1].value);

    CHECK (rig.bend.getLastValue() == 0);
    CHECK (rig.events.back().value == 0);
}

TEST_CASE ("Bend: rate limit of ~200 Hz on absolute samples", "[bendgenerator]")
{
    const double sr = GENERATE (44100.0, 48000.0, 96000.0);
    const int hop = sr == 44100.0 ? 88 : (sr == 48000.0 ? 96 : 192);
    const auto minInterval = std::max (1L, std::lround (sr / 200.0));
    CAPTURE (sr);

    SECTION ("a value changing every frame is sent at most every sr / 200 samples")
    {
        BendRig rig (sr, hop, BendMode::trueBend);
        rig.start (0.0);

        for (int frame = 0; frame < 1000; ++frame)
        {
            rig.advance();
            rig.update (40.0 * std::sin (static_cast<double> (frame) * 0.05) + 1.0); // vibrato-like
        }

        REQUIRE (rig.events.size() > 100);

        for (std::size_t i = 1; i < rig.events.size(); ++i)
        {
            const auto gap = rig.events[i].position - rig.events[i - 1].position;
            CHECK (gap >= minInterval);
            CHECK (gap < minInterval + hop); // sent at the first allowed frame
        }

        // ~200 Hz nominal, at least 150 Hz effective with 2 ms hops
        const auto seconds = static_cast<double> (rig.events.back().position - rig.events.front().position) / sr;
        const auto rate = static_cast<double> (rig.events.size() - 1) / seconds;
        CHECK (rate <= 200.0);
        CHECK (rate >= 150.0);
    }

    SECTION ("only changes are sent, a limited change is sent at the next allowed frame")
    {
        BendRig rig (sr, hop, BendMode::trueBend);
        rig.start (0.0);
        rig.advance();
        rig.update (50.0); // first update after start (nothing emitted yet): not limited
        REQUIRE (rig.events.size() == 1);
        const auto firstPosition = rig.events[0].position;

        rig.advance();
        rig.update (60.0); // too early
        CHECK (rig.events.size() == 1);

        // The value stays at 60: it must go out as soon as the interval has passed
        while (rig.position - firstPosition < minInterval)
        {
            rig.update (60.0);
            CHECK (rig.events.size() == 1);
            rig.advance();
        }

        rig.update (60.0);
        REQUIRE (rig.events.size() == 2);
        CHECK (rig.events[1].value == BendGenerator::centsToBend (60.0, 2));
        CHECK (rig.events[1].position - firstPosition >= minInterval);

        // Unchanged values are never repeated
        for (int i = 0; i < 50; ++i)
        {
            rig.advance();
            rig.update (60.0);
        }

        CHECK (rig.events.size() == 2);
    }

    SECTION ("note start and end are not limited and restart the interval")
    {
        BendRig rig (sr, hop, BendMode::trueBend);
        rig.start (30.0);
        rig.advance();
        rig.end(); // one hop later: immediate
        REQUIRE (rig.events.size() == 2);
        CHECK (rig.events[1].value == 0);
        CHECK (rig.events[1].position == hop);

        rig.start (-40.0); // same sample: immediate
        REQUIRE (rig.events.size() == 3);
        const auto startPosition = rig.events[2].position;

        rig.advance();
        rig.update (-20.0); // limited relative to the note start
        CHECK (rig.events.size() == 3);

        while (rig.position - startPosition < minInterval)
            rig.advance();

        rig.update (-20.0);
        REQUIRE (rig.events.size() == 4);
        CHECK (rig.events[3].position - startPosition >= minInterval);
    }
}

TEST_CASE ("Bend: switching to off sends the centre once", "[bendgenerator]")
{
    BendRig rig (48000.0, 96, BendMode::trueBend);
    rig.start (70.0);
    REQUIRE (rig.events.size() == 1);

    rig.bend.setMode (BendMode::off, BendRange::semitones2);
    CHECK (rig.events.size() == 1); // setMode never emits

    for (int i = 0; i < 20; ++i)
    {
        rig.advance();
        rig.update (70.0 + i);
    }

    REQUIRE (rig.events.size() == 2);
    CHECK (rig.events[1].value == 0);
    CHECK (rig.events[1].position - rig.events[0].position >= 240); // rate limited like any update
    rig.end();
    CHECK (rig.events.size() == 2); // already centred
}

TEST_CASE ("Bend: switching off -> anchored mid-note glides from the current bend", "[bendgenerator]")
{
    BendRig rig (48000.0, 96, BendMode::off);
    rig.start (100.0);
    rig.bend.setMode (BendMode::anchored, BendRange::semitones2);

    for (int i = 0; i < 200; ++i)
    {
        rig.advance();
        rig.update (100.0); // shaped 70 cents
    }

    REQUIRE (rig.events.size() > 3);
    CHECK (rig.events.front().value < BendGenerator::centsToBend (70.0, 2) / 2); // no jump
    CHECK (rig.events.back().value == BendGenerator::centsToBend (70.0, 2));     // converged
}

TEST_CASE ("Bend: range change takes effect at the next call", "[bendgenerator]")
{
    BendRig rig (48000.0, 96, BendMode::trueBend, BendRange::semitones2);
    rig.start (100.0);
    REQUIRE (rig.events.size() == 1);
    CHECK (rig.events[0].value == 4096);

    rig.bend.setMode (BendMode::trueBend, BendRange::semitones12);
    CHECK (rig.events.size() == 1);
    rig.advance (3);
    rig.update (100.0);
    REQUIRE (rig.events.size() == 2);
    CHECK (rig.events[1].value == 683);

    rig.bend.setMode (BendMode::trueBend, BendRange::semitones48);
    rig.advance (3);
    rig.update (2400.0);
    REQUIRE (rig.events.size() == 3);
    CHECK (rig.events[2].value == 4096);
}

TEST_CASE ("Bend: reset forgets without emitting; non-finite residuals are 0 cents", "[bendgenerator]")
{
    BendRig rig (48000.0, 96, BendMode::trueBend);
    rig.start (80.0);
    REQUIRE (rig.bend.getLastValue() != 0);

    rig.bend.reset();
    CHECK (rig.bend.getLastValue() == 0);
    CHECK (rig.events.size() == 1);

    // After reset the first update is not rate limited
    rig.update (-80.0);
    REQUIRE (rig.events.size() == 2);
    CHECK (rig.events[1].value == BendGenerator::centsToBend (-80.0, 2));

    rig.advance (3);
    rig.update (std::numeric_limits<double>::quiet_NaN());
    REQUIRE (rig.events.size() == 3);
    CHECK (rig.events[2].value == 0);

    rig.bend.setMode (BendMode::anchored, BendRange::semitones2);
    rig.advance (3);
    rig.start (std::numeric_limits<double>::infinity());
    CHECK (rig.bend.getLastValue() == 0);
}

TEST_CASE ("Bend: output depends only on absolute samples, not on the block split", "[bendgenerator]")
{
    std::vector<std::vector<int16_t>> valueRuns;
    std::vector<std::vector<int64_t>> positionRuns;

    for (int blockSize : { 1, 7, 64, 480, 512, 4096 })
    {
        BendRig rig (48000.0, 96, BendMode::anchored, BendRange::semitones2, blockSize);
        rig.start (60.0);

        for (int i = 0; i < 500; ++i)
        {
            rig.advance();
            rig.update (50.0 + 60.0 * std::sin (2.0 * std::numbers::pi * 5.5 * static_cast<double> (i) * 0.002));
        }

        rig.end();

        std::vector<int16_t> values;
        std::vector<int64_t> positions;

        for (const auto& e : rig.events)
        {
            values.push_back (e.value);
            positions.push_back (e.position);
        }

        valueRuns.push_back (values);
        positionRuns.push_back (positions);
    }

    for (std::size_t i = 1; i < valueRuns.size(); ++i)
    {
        CHECK (valueRuns[i] == valueRuns[0]);
        CHECK (positionRuns[i] == positionRuns[0]);
    }
}
