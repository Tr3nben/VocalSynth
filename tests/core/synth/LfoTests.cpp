// Lfo: shapes, rate, tempo sync (incl. bars from the host time signature), retrigger at the next tick, fade-in, S&H.
// Test plan: docs/SYNTH-DESIGN.md 21. Owner: agent B (Voice).

#include "vocalsynth/synth/Lfo.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <limits>
#include <numbers>
#include <vector>

using namespace vocalsynth::synth;
using Catch::Matchers::WithinAbs;

namespace
{

constexpr int tick = controlTickSamples;

/** Measured period in samples: saw-up wraps, the exact wrap position interpolated from the tick values. */
double measurePeriodSamples (Lfo& lfo, LfoSettings s, const SynthTransport& transport, int numTicks)
{
    s.shape = LfoShape::sawUp;
    double previousPhase = (lfo.advanceTick (s, transport, 0.0f, tick) + 1.0) * 0.5;
    double firstWrap = -1.0, lastWrap = -1.0;
    int wraps = 0;

    for (int t = 1; t < numTicks; ++t)
    {
        const double phase = (lfo.advanceTick (s, transport, 0.0f, tick) + 1.0) * 0.5;

        if (phase < previousPhase)
        {
            const auto step = phase + 1.0 - previousPhase;
            const auto wrapAt = (t - 1) * tick + tick * (1.0 - previousPhase) / step;

            if (firstWrap < 0.0)
                firstWrap = wrapAt;
            else
                ++wraps;

            lastWrap = wrapAt;
        }

        previousPhase = phase;
    }

    REQUIRE (wraps > 0);
    return (lastWrap - firstWrap) / wraps;
}

} // namespace

TEST_CASE ("Lfo: shapes over one cycle", "[synth][lfo]")
{
    // 48000 / 16 / 64 = 46.875 Hz: 64 ticks per cycle, the phase advances exactly 1/64 per tick
    const auto shape = GENERATE (LfoShape::sine, LfoShape::triangle, LfoShape::sawUp, LfoShape::sawDown, LfoShape::square);
    CAPTURE (static_cast<int> (shape));
    Lfo lfo;
    lfo.prepare (48000.0, 7);
    LfoSettings s;
    s.shape = shape;
    s.rateHz = 46.875f;

    for (int t = 1; t <= 128; ++t)
    {
        const auto v = lfo.advanceTick (s, {}, 0.0f, tick);
        const double p = (t % 64) / 64.0;
        double expected = 0.0;

        switch (shape)
        {
            case LfoShape::sine:     expected = std::sin (2.0 * std::numbers::pi * p); break;
            case LfoShape::triangle: expected = p < 0.25 ? 4.0 * p : (p < 0.75 ? 2.0 - 4.0 * p : 4.0 * p - 4.0); break;
            case LfoShape::sawUp:    expected = 2.0 * p - 1.0; break;
            case LfoShape::sawDown:  expected = 1.0 - 2.0 * p; break;
            case LfoShape::square:   expected = p < 0.5 ? 1.0 : -1.0; break;
            default: break;
        }

        CAPTURE (t);
        CHECK_THAT (v, WithinAbs (expected, 1e-5));
        CHECK (lfo.getValue() == v);
    }
}

TEST_CASE ("Lfo: free rate and rate modulation", "[synth][lfo]")
{
    const auto rate = GENERATE (44100.0, 48000.0, 96000.0);
    const auto hz = GENERATE (0.5f, 5.0f, 30.0f);
    CAPTURE (rate, hz);
    Lfo lfo;
    lfo.prepare (rate, 3);
    LfoSettings s;
    s.rateHz = hz;
    const auto ticks = static_cast<int> (20.0 * rate / tick);
    CHECK_THAT (measurePeriodSamples (lfo, s, {}, ticks), WithinAbs (rate / hz, 0.01));

    // +1 octave of rate modulation doubles the frequency
    Lfo fast;
    fast.prepare (rate, 3);
    s.shape = LfoShape::sawUp;
    double prev = (fast.advanceTick (s, {}, 1.0f, tick) + 1.0) * 0.5;
    int wraps = 0;

    for (int t = 1; t < ticks; ++t)
    {
        const double ph = (fast.advanceTick (s, {}, 1.0f, tick) + 1.0) * 0.5;
        wraps += ph < prev ? 1 : 0;
        prev = ph;
    }

    CHECK (std::abs (wraps - 2.0 * hz * ticks * tick / rate) <= 1.0);
}

TEST_CASE ("Lfo: tempo sync follows the host tempo", "[synth][lfo]")
{
    const auto bpm = GENERATE (90.0, 120.0, 174.0);
    const auto division = GENERATE (TempoDivision::d1_16, TempoDivision::d1_8T, TempoDivision::d1_4, TempoDivision::d1_4D,
                                    TempoDivision::d1_2);
    const auto rate = GENERATE (44100.0, 48000.0);
    CAPTURE (bpm, static_cast<int> (division), rate);

    Lfo lfo;
    lfo.prepare (rate, 1);
    LfoSettings s;
    s.rateHz = 7.0f; // ignored while synced
    s.sync = division;
    const auto expected = tempoDivisionQuarterNotes (division) * 60.0 / bpm * rate;
    const auto period = measurePeriodSamples (lfo, s, { bpm, 4.0 }, static_cast<int> (30.0 * rate / tick));
    CHECK (std::abs (period - expected) <= 1.0); // within one sample per cycle
}

TEST_CASE ("Lfo: bar divisions follow the time signature, fallback 4 quarter notes", "[synth][lfo]")
{
    const auto quarters = GENERATE (4.0, 3.0, 3.5, 0.0, -1.0, std::numeric_limits<double>::quiet_NaN());
    CAPTURE (quarters);
    const auto bar = std::isfinite (quarters) && quarters > 0.0 ? quarters : 4.0;

    Lfo lfo;
    lfo.prepare (48000.0, 1);
    LfoSettings s;
    s.sync = TempoDivision::bar1;
    const auto period = measurePeriodSamples (lfo, s, { 120.0, quarters }, static_cast<int> (40.0 * 48000.0 / tick));
    CHECK (std::abs (period - bar * 0.5 * 48000.0) <= 1.0);

    // Unknown tempo (0 / NaN) -> 120 BPM
    Lfo l2;
    l2.prepare (48000.0, 1);
    s.sync = TempoDivision::d1_4;
    const auto p2 = measurePeriodSamples (l2, s, { std::numeric_limits<double>::quiet_NaN(), quarters },
                                          static_cast<int> (10.0 * 48000.0 / tick));
    CHECK (std::abs (p2 - 24000.0) <= 1.0);
}

TEST_CASE ("Lfo: retrigger takes effect at the next tick", "[synth][lfo]")
{
    Lfo lfo;
    lfo.prepare (48000.0, 1);
    LfoSettings s;
    s.shape = LfoShape::sawUp;
    s.rateHz = 3.0f;
    s.retrigger = true;

    for (int t = 0; t < 100; ++t)
        lfo.advanceTick (s, {}, 0.0f, tick);

    const auto before = lfo.getValue();
    REQUIRE (before > -0.9f);
    lfo.retrigger (s);
    CHECK (lfo.getValue() == before); // nothing changes until the tick
    CHECK (lfo.advanceTick (s, {}, 0.0f, tick) == -1.0f); // phase 0 at the tick
    const auto next = lfo.advanceTick (s, {}, 0.0f, tick);
    CHECK_THAT (next, WithinAbs (-1.0 + 2.0 * 3.0 * tick / 48000.0, 1e-6));

    // Retrigger off: the phase runs on, only the fade restarts (no fade time -> no effect)
    Lfo free;
    free.prepare (48000.0, 1);
    s.retrigger = false;
    Lfo ref;
    ref.prepare (48000.0, 1);

    for (int t = 0; t < 300; ++t)
    {
        if (t == 50)
            free.retrigger (s);

        CHECK (free.advanceTick (s, {}, 0.0f, tick) == ref.advanceTick (s, {}, 0.0f, tick));
    }
}

TEST_CASE ("Lfo: fade-in restarts at the tick after a retrigger", "[synth][lfo]")
{
    Lfo lfo;
    lfo.prepare (48000.0, 1);
    LfoSettings s;
    s.shape = LfoShape::square; // constant +1 for the first half cycle
    s.rateHz = 0.01f;
    s.retrigger = true;
    s.fadeMs = 100.0f; // 4800 samples = 300 ticks

    // After reset: not faded
    CHECK (lfo.advanceTick (s, {}, 0.0f, tick) == 1.0f);

    lfo.retrigger (s);
    CHECK (lfo.advanceTick (s, {}, 0.0f, tick) == 0.0f);

    for (int t = 1; t <= 320; ++t)
    {
        const auto v = lfo.advanceTick (s, {}, 0.0f, tick);
        CHECK_THAT (v, WithinAbs (std::min (1.0, t * tick / 4800.0), 1e-6));
    }

    // Fade 0: a retrigger does not fade
    s.fadeMs = 0.0f;
    lfo.retrigger (s);
    CHECK (lfo.advanceTick (s, {}, 0.0f, tick) == 1.0f);
}

TEST_CASE ("Lfo: S&H and smooth random are deterministic and in range", "[synth][lfo]")
{
    for (const auto shape : { LfoShape::sampleHold, LfoShape::smoothRandom })
    {
        Lfo a, b;
        a.prepare (48000.0, 42);
        b.prepare (48000.0, 42);
        LfoSettings s;
        s.shape = shape;
        s.rateHz = 10.0f;
        std::vector<float> first;
        int changes = 0;
        float previous = a.getValue();

        for (int t = 0; t < 20000; ++t)
        {
            const auto va = a.advanceTick (s, {}, 0.0f, tick);
            CHECK (va == b.advanceTick (s, {}, 0.0f, tick));
            CHECK (va >= -1.0f);
            CHECK (va <= 1.0f);
            changes += va != previous ? 1 : 0;
            previous = va;
            first.push_back (va);
        }

        if (shape == LfoShape::sampleHold)
            CHECK (std::abs (changes - 20000 * tick * 10.0 / 48000.0) <= 2.0); // one new value per cycle
        else
            CHECK (changes > 19000); // smooth: moves every tick

        // Reset gives the same sequence again
        a.reset();

        for (int t = 0; t < 20000; ++t)
            REQUIRE (a.advanceTick (s, {}, 0.0f, tick) == first[static_cast<std::size_t> (t)]);

        // Another seed differs
        Lfo c;
        c.prepare (48000.0, 43);
        int same = 0;

        for (int t = 0; t < 2000; ++t)
            same += c.advanceTick (s, {}, 0.0f, tick) == first[static_cast<std::size_t> (t)] ? 1 : 0;

        CHECK (same < 100);
    }
}

TEST_CASE ("Lfo: non-finite rate and modulation are harmless", "[synth][lfo]")
{
    Lfo lfo;
    lfo.prepare (48000.0, 1);
    LfoSettings s;
    s.rateHz = std::numeric_limits<float>::quiet_NaN();

    for (int t = 0; t < 1000; ++t)
    {
        const auto v = lfo.advanceTick (s, {}, std::numeric_limits<float>::infinity(), tick);
        REQUIRE (std::isfinite (v));
    }

    s.rateHz = 30.0f;

    for (int t = 0; t < 1000; ++t)
        REQUIRE (std::isfinite (lfo.advanceTick (s, {}, 100.0f, tick))); // clamped to 100 Hz
}
