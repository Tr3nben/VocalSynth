// ExpressionTracker: level mapping vs rmsDb and smoothing times, brightness of a sine sweep monotonic,
// noise > tone in noisiness, pitch cents of a detuned tone, robustness, CPU.
// Test plan: docs/SYNTH-DESIGN.md 21. Owner: agent C (Engine + FX + Expression).

#include "vocalsynth/ExpressionTracker.h"

#include "TestSignals.h"

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <chrono>
#include <cmath>
#include <limits>
#include <vector>

using namespace vocalsynth;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr double analysisRate = 16000.0;
constexpr int hop = 32; // 2 ms
constexpr double hopSeconds = hop / analysisRate;

struct Feeder
{
    ExpressionTracker tracker;
    test::Signal signal;
    std::size_t position = 0;
    std::size_t windowSize = 512;

    explicit Feeder (test::Signal s, std::size_t window = 512) : signal (std::move (s)), position (window), windowSize (window)
    {
        tracker.prepare (analysisRate, hop);
    }

    /** One hop: the newest windowSize samples of the signal (wrapping), with the given frame values. */
    ExpressionFrame step (float rmsDb, float confidence = 0.9f, float f0 = 0.0f, int note = -1, float gate = -40.0f)
    {
        ExpressionTrackerInput in;
        std::span<const float> all (signal);
        position = std::min (position + hop, signal.size());

        if (position == signal.size())
            position = windowSize; // loop the signal

        in.window = all.subspan (position - windowSize, windowSize);
        in.rmsDb = rmsDb;
        in.confidence = confidence;
        in.frequencyHz = f0;
        in.sungNote = note;
        in.gateThresholdDb = gate;
        return tracker.processHop (in);
    }

    ExpressionFrame settle (int hops, float rmsDb, float confidence = 0.9f, float f0 = 0.0f, int note = -1)
    {
        ExpressionFrame f;

        for (int i = 0; i < hops; ++i)
            f = step (rmsDb, confidence, f0, note);

        return f;
    }
};

test::Signal sine (double hz, double seconds = 0.5) { return test::sine (analysisRate, hz, seconds, 0.5f); }
} // namespace

TEST_CASE ("ExpressionTracker: level maps rmsDb between the gate (0) and -6 dBFS (1)", "[expression]")
{
    const float gate = -40.0f;
    const auto fraction = GENERATE (0.0f, 0.25f, 0.5f, 0.8f, 1.0f);
    CAPTURE (fraction);
    Feeder f (sine (440.0));
    const auto out = f.settle (300, gate + fraction * (-6.0f - gate));
    CHECK_THAT (out.level, WithinAbs (fraction, 1e-4));

    Feeder above (sine (440.0));
    CHECK_THAT (above.settle (300, 0.0f).level, WithinAbs (1.0, 1e-6));
    Feeder below (sine (440.0));
    CHECK (below.settle (300, -80.0f).level == 0.0f);
}

TEST_CASE ("ExpressionTracker: level attack 5 ms, release 50 ms (one-pole at the hop rate)", "[expression]")
{
    Feeder f (sine (440.0));

    for (int k = 1; k <= 20; ++k)
    {
        const auto level = f.step (-6.0f).level;
        CHECK_THAT (level, WithinAbs (1.0 - std::exp (-k * hopSeconds / 0.005), 1e-4));
    }

    f.settle (200, -6.0f);

    for (int k = 1; k <= 100; ++k)
    {
        const auto level = f.step (-90.0f).level;
        CHECK_THAT (level, WithinAbs (std::exp (-k * hopSeconds / 0.050), 1e-4));
    }

    // It reaches exactly 0 (no endless tail)
    CHECK (f.settle (500, -90.0f).level == 0.0f);
}

TEST_CASE ("ExpressionTracker: brightness of a sine sweep is monotonic, log-normalised 200 Hz .. 5 kHz", "[expression]")
{
    std::vector<float> values;
    std::vector<double> frequencies;

    for (double hz = 150.0; hz <= 6000.0; hz *= 1.15)
    {
        Feeder f (sine (hz));
        values.push_back (f.settle (60, -10.0f).brightness);
        frequencies.push_back (hz);
    }

    for (std::size_t i = 1; i < values.size(); ++i)
    {
        CAPTURE (frequencies[i]);
        CHECK (values[i] >= values[i - 1]);
    }

    const auto at = [] (double hz)
    {
        Feeder f (sine (hz));
        return f.settle (60, -10.0f).brightness;
    };

    CHECK (at (200.0) < 0.05f);
    CHECK_THAT (at (1000.0), WithinAbs (0.5, 0.03));       // log2 (5) / log2 (25)
    CHECK_THAT (at (2000.0), WithinAbs (0.7153, 0.03));    // log2 (10) / log2 (25)
    CHECK (at (5000.0) > 0.95f);
}

TEST_CASE ("ExpressionTracker: noise is noisier than a tone", "[expression]")
{
    Feeder tone (sine (300.0));
    Feeder noise (test::whiteNoise (analysisRate, 0.5, 0.3f, 99));
    const auto t = tone.settle (60, -10.0f, 0.95f).noisiness;
    const auto n = noise.settle (60, -10.0f, 0.1f).noisiness;
    CAPTURE (t, n);
    CHECK (t < 0.1f);
    CHECK (n > 0.8f);

    // Each half of the measure counts: noise with a confident pitch, a tone without confidence
    Feeder noiseConfident (test::whiteNoise (analysisRate, 0.5, 0.3f, 7));
    const auto nc = noiseConfident.settle (60, -10.0f, 1.0f).noisiness;
    Feeder toneUnsure (sine (300.0));
    const auto tu = toneUnsure.settle (60, -10.0f, 0.0f).noisiness;
    CHECK (nc > 0.35f);
    CHECK (nc < 0.55f);
    CHECK (tu > 0.45f);
    CHECK (tu < 0.6f);
}

TEST_CASE ("ExpressionTracker: brightness and noisiness are 0 without a voice", "[expression]")
{
    Feeder f (test::whiteNoise (analysisRate, 0.5, 0.5f, 3));
    const auto loud = f.settle (60, -10.0f, 0.0f);
    CHECK (loud.brightness > 0.5f);
    CHECK (loud.noisiness > 0.5f);
    const auto gone = f.settle (300, -100.0f, 0.0f);
    CHECK (gone.level == 0.0f);
    CHECK (gone.brightness == 0.0f);
    CHECK (gone.noisiness == 0.0f);
}

TEST_CASE ("ExpressionTracker: pitch cents of a detuned tone, clamped, 0 without a note", "[expression]")
{
    const auto cents = GENERATE (-75.0, -30.0, 0.0, 12.5, 49.0);
    CAPTURE (cents);
    Feeder f (sine (440.0));
    const auto hz = static_cast<float> (440.0 * std::exp2 (cents / 1200.0));
    CHECK_THAT (f.settle (100, -10.0f, 0.9f, hz, 69).pitchCents, WithinAbs (cents, 0.01));

    // 10 ms smoothing: after one hop 1 - exp (-2 / 10) of the way
    Feeder g (sine (440.0));
    CHECK_THAT (g.step (-10.0f, 0.9f, hz, 69).pitchCents, WithinAbs (cents * (1.0 - std::exp (-0.2)), 0.01));

    Feeder far (sine (440.0));
    CHECK_THAT (far.settle (100, -10.0f, 0.9f, 440.0f * 1.25f, 69).pitchCents, WithinAbs (100.0, 1e-4));
    CHECK_THAT (far.settle (100, -10.0f, 0.9f, 0.0f, 69).pitchCents, WithinAbs (0.0, 1e-4)); // unvoiced
    CHECK_THAT (far.settle (100, -10.0f, 0.9f, 440.0f * 1.01f, -1).pitchCents, WithinAbs (0.0, 1e-4)); // no note
}

TEST_CASE ("ExpressionTracker: short windows are zero-padded, reset restarts, non-finite inputs stay finite",
           "[expression]")
{
    Feeder shortWindow (sine (1000.0), 300);
    CHECK_THAT (shortWindow.settle (60, -10.0f).brightness, WithinAbs (0.5, 0.05));

    Feeder f (sine (1000.0));
    const auto a = f.settle (40, -12.0f, 0.7f, 1000.0f, 83);
    f.tracker.reset();
    f.position = f.windowSize;
    const auto b = f.settle (40, -12.0f, 0.7f, 1000.0f, 83);
    CHECK (a == b);

    auto bad = sine (440.0);
    bad[700] = std::numeric_limits<float>::quiet_NaN();
    Feeder n (bad);

    for (int i = 0; i < 200; ++i)
    {
        const auto out = n.step (std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                                 std::numeric_limits<float>::quiet_NaN(), 69);
        REQUIRE (std::isfinite (out.level));
        REQUIRE (std::isfinite (out.brightness));
        REQUIRE (std::isfinite (out.noisiness));
        REQUIRE (std::isfinite (out.pitchCents));
    }
}

TEST_CASE ("ExpressionTracker: CPU per hop", "[expression][cpu]")
{
    // Budget (docs/SYNTH-DESIGN.md 18): <= 0.5 % at 48 kHz = 10 us per 2 ms hop. Generous gate (debug-free
    // CI machines vary); the benchmark below gives the number.
    Feeder f (test::whiteNoise (analysisRate, 1.0, 0.3f, 5));
    f.settle (10, -10.0f, 0.5f, 220.0f, 57);
    double best = 1.0e9;

    for (int run = 0; run < 9; ++run)
    {
        const auto start = std::chrono::steady_clock::now();
        f.settle (500, -10.0f, 0.5f, 220.0f, 57);
        const std::chrono::duration<double, std::micro> us = std::chrono::steady_clock::now() - start;
        best = std::min (best, us.count() / 500.0);
    }

    UNSCOPED_INFO ("ExpressionTracker: " << best << " us per hop (budget 10 us)");
    CHECK (best < 10.0);
}

TEST_CASE ("ExpressionTracker: benchmark", "[expression][!benchmark]")
{
    Feeder f (test::whiteNoise (analysisRate, 1.0, 0.3f, 5));

    BENCHMARK ("100 hops (= 200 ms of voice)")
    {
        return f.settle (100, -10.0f, 0.5f, 220.0f, 57).level;
    };
}
