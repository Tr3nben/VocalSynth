#include "vocalsynth/InputConditioner.h"

#include "TestSignals.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <vector>

using Catch::Matchers::WithinAbs;
using namespace vocalsynth;

namespace
{

test::Signal condition (double sampleRate, const test::Signal& in)
{
    InputConditioner conditioner;
    conditioner.prepare (sampleRate);
    test::Signal out (in.size());
    conditioner.process (in.data(), out.data(), static_cast<int> (in.size()));
    return out;
}

double rmsOf (const float* x, std::size_t n)
{
    double sum = 0.0;

    for (std::size_t i = 0; i < n; ++i)
        sum += static_cast<double> (x[i]) * static_cast<double> (x[i]);

    return n > 0 ? std::sqrt (sum / static_cast<double> (n)) : 0.0;
}

/** Steady-state gain in dB of the conditioner for a sine (after 1 s of settling, measured over 1 s). */
double gainDb (double sampleRate, double frequencyHz)
{
    const auto in = test::sine (sampleRate, frequencyHz, 2.0, 0.5f);
    const auto out = condition (sampleRate, in);
    const auto start = test::numSamplesFor (1.0, sampleRate);
    const auto n = in.size() - start;
    return 20.0 * std::log10 (rmsOf (out.data() + start, n) / rmsOf (in.data() + start, n));
}

/** Energy centroid sum t x^2 / sum x^2 in samples. */
double energyCentroid (const test::Signal& x)
{
    double weighted = 0.0, total = 0.0;

    for (std::size_t i = 0; i < x.size(); ++i)
    {
        const auto e = static_cast<double> (x[i]) * static_cast<double> (x[i]);
        weighted += static_cast<double> (i) * e;
        total += e;
    }

    return weighted / total;
}

} // namespace

TEST_CASE ("InputConditioner removes DC", "[conditioner]")
{
    const auto sampleRate = GENERATE (44100.0, 48000.0, 96000.0);
    const auto out = condition (sampleRate, test::Signal (test::numSamplesFor (2.0, sampleRate), 0.5f));

    // A DC step passes the first sample(s) and then decays: after 1 s nothing measurable is left
    const auto settled = test::numSamplesFor (1.0, sampleRate);

    for (std::size_t i = settled; i < out.size(); ++i)
        REQUIRE (std::abs (out[i]) < 1.0e-4f);
}

TEST_CASE ("InputConditioner high-pass: -3 dB at 60 Hz, flat from 150 Hz", "[conditioner]")
{
    const auto sampleRate = GENERATE (44100.0, 48000.0, 96000.0);
    CAPTURE (sampleRate);

    CHECK_THAT (gainDb (sampleRate, 60.0), WithinAbs (-3.0, 0.5));

    for (const auto f : { 150.0, 220.0, 440.0, 1000.0, 2500.0, 5000.0 })
    {
        CAPTURE (f);
        CHECK_THAT (gainDb (sampleRate, f), WithinAbs (0.0, 0.5));
    }

    // Rumble is strongly attenuated (2nd order: about -12 dB per octave below the cutoff)
    CHECK (gainDb (sampleRate, 20.0) < -18.0);

    // Bass f0 min passes with a moderate loss (docs/M2-DESIGN.md 3.2: about 2.4 dB)
    CHECK_THAT (gainDb (sampleRate, 65.0), WithinAbs (-2.4, 0.5));
}

TEST_CASE ("InputConditioner group delay matches a measured envelope delay", "[conditioner]")
{
    const auto sampleRate = GENERATE (44100.0, 48000.0, 96000.0);

    for (const auto f : { 65.0, 82.0, 110.0, 220.0 })
    {
        CAPTURE (sampleRate, f);

        // Narrow-band tone burst: Gaussian envelope (sigma 100 ms -> ~1.6 Hz bandwidth) around 1 s.
        // The energy centroid of the output minus that of the input is the envelope (group) delay.
        const auto n = test::numSamplesFor (2.0, sampleRate);
        test::Signal in (n);
        const auto centre = 1.0, sigma = 0.1;

        for (std::size_t i = 0; i < n; ++i)
        {
            const auto t = static_cast<double> (i) / sampleRate;
            const auto env = std::exp (-0.5 * (t - centre) * (t - centre) / (sigma * sigma));
            in[i] = static_cast<float> (0.5 * env * std::sin (2.0 * std::numbers::pi * f * t));
        }

        const auto out = condition (sampleRate, in);
        const auto measuredMs = 1000.0 * (energyCentroid (out) - energyCentroid (in)) / sampleRate;
        const auto modelMs = 1000.0 * InputConditioner::groupDelaySeconds (sampleRate, f);
        CHECK_THAT (measuredMs, WithinAbs (modelMs, 0.3));
    }

    // Values the latency budget is built on (docs/M2-DESIGN.md 3.2 / 3.11)
    CHECK_THAT (1000.0 * InputConditioner::groupDelaySeconds (48000.0, 65.0), WithinAbs (3.62, 0.02));
    CHECK_THAT (1000.0 * InputConditioner::groupDelaySeconds (48000.0, 110.0), WithinAbs (1.40, 0.02));
    CHECK_THAT (1000.0 * InputConditioner::groupDelaySeconds (44100.0, 65.0), WithinAbs (3.62, 0.02));
    CHECK_THAT (1000.0 * InputConditioner::groupDelaySeconds (96000.0, 65.0), WithinAbs (3.62, 0.02));

    // Degenerate arguments
    CHECK (InputConditioner::groupDelaySeconds (0.0, 100.0) == 0.0);
    CHECK (InputConditioner::groupDelaySeconds (48000.0, 0.0) == 0.0);
    CHECK (InputConditioner::groupDelaySeconds (48000.0, 30000.0) == 0.0);
}

TEST_CASE ("InputConditioner is block-size invariant (bit-exact)", "[conditioner]")
{
    const auto sampleRate = 48000.0;
    auto in = test::saw (sampleRate, 110.0, 0.5, 0.4f);
    test::mixInto (in, test::whiteNoise (sampleRate, 0.5, 0.1f, 7));

    const auto reference = condition (sampleRate, in);

    for (const int blockSize : { 1, 7, 64, 480, 512, 4096 })
    {
        CAPTURE (blockSize);
        InputConditioner conditioner;
        conditioner.prepare (sampleRate);
        test::Signal out (in.size());

        for (std::size_t start = 0; start < in.size(); start += static_cast<std::size_t> (blockSize))
        {
            const auto len = std::min (static_cast<std::size_t> (blockSize), in.size() - start);

            if (blockSize == 7) // also mix process() with processSample()
                for (std::size_t i = 0; i < len; ++i)
                    out[start + i] = conditioner.processSample (in[start + i]);
            else
                conditioner.process (in.data() + start, out.data() + start, static_cast<int> (len));
        }

        REQUIRE (out == reference);
    }

    // In-place processing gives the same result
    auto inPlace = in;
    InputConditioner conditioner;
    conditioner.prepare (sampleRate);
    conditioner.process (inPlace.data(), inPlace.data(), static_cast<int> (inPlace.size()));
    CHECK (inPlace == reference);
}

TEST_CASE ("InputConditioner treats non-finite input as 0", "[conditioner]")
{
    InputConditioner conditioner;
    conditioner.prepare (44100.0);
    const auto in = test::sine (44100.0, 220.0, 0.1, 0.5f);

    for (std::size_t i = 0; i < in.size(); ++i)
    {
        auto x = in[i];

        if (i % 97 == 3)
            x = std::numeric_limits<float>::quiet_NaN();
        else if (i % 101 == 5)
            x = std::numeric_limits<float>::infinity();
        else if (i % 103 == 7)
            x = -std::numeric_limits<float>::infinity();

        const auto y = conditioner.processSample (x);
        REQUIRE (std::isfinite (y));
        REQUIRE (std::abs (y) < 4.0f);
    }

    // A NaN gives the same output as a 0 at that position
    InputConditioner a, b;
    a.prepare (48000.0);
    b.prepare (48000.0);

    for (std::size_t i = 0; i < in.size(); ++i)
    {
        const auto ya = a.processSample (i == 100 ? std::numeric_limits<float>::quiet_NaN() : in[i]);
        const auto yb = b.processSample (i == 100 ? 0.0f : in[i]);
        REQUIRE (ya == yb);
    }
}

TEST_CASE ("InputConditioner reset restores the initial state", "[conditioner]")
{
    const auto in = test::saw (48000.0, 147.0, 0.2, 0.5f);

    InputConditioner conditioner;
    conditioner.prepare (48000.0);
    test::Signal first (in.size()), second (in.size());
    conditioner.process (in.data(), first.data(), static_cast<int> (in.size()));
    conditioner.reset();
    conditioner.process (in.data(), second.data(), static_cast<int> (in.size()));
    CHECK (first == second);

    // prepare() also resets, and silence stays exactly silent
    conditioner.prepare (96000.0);

    for (int i = 0; i < 64; ++i)
        CHECK (conditioner.processSample (0.0f) == 0.0f);
}

TEST_CASE ("InputConditioner flushes to exact zeros after a long silence", "[conditioner]")
{
    const auto sampleRate = GENERATE (44100.0, 48000.0, 96000.0);
    CAPTURE (sampleRate);

    InputConditioner conditioner;
    conditioner.prepare (sampleRate);

    for (const auto x : test::saw (sampleRate, 110.0, 0.3, 0.8f))
        conditioner.processSample (x);

    // The DC blocker (5 Hz) decays slowest; 8 s are far more than its ~70 time constants to 1e-30
    const auto silentSamples = test::numSamplesFor (8.0, sampleRate);
    std::size_t firstExactZero = silentSamples;
    float y = 1.0f;

    for (std::size_t i = 0; i < silentSamples; ++i)
    {
        y = conditioner.processSample (0.0f);

        if (y != 0.0f)
            firstExactZero = silentSamples;
        else if (firstExactZero == silentSamples)
            firstExactZero = i;
    }

    CHECK (y == 0.0f);
    CHECK (firstExactZero < silentSamples);

    // Exactly zero, not merely tiny: the next samples stay exactly 0
    for (int i = 0; i < 1000; ++i)
        REQUIRE (conditioner.processSample (0.0f) == 0.0f);
}
