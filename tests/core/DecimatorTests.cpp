#include "vocalsynth/Decimator.h"

#include "TestSignals.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

using Catch::Matchers::WithinAbs;
using namespace vocalsynth;

namespace
{

/** Runs the input through a decimator (one push per sample, split into blocks of blockSize, which a
    per-sample state machine must not notice) and returns the outputs. */
std::vector<float> decimate (Decimator& decimator, const std::vector<float>& in, int blockSize = 512)
{
    std::vector<float> out;
    out.reserve (in.size() / static_cast<std::size_t> (decimator.getFactor()) + 1);

    for (std::size_t start = 0; start < in.size(); start += static_cast<std::size_t> (blockSize))
    {
        const auto end = std::min (in.size(), start + static_cast<std::size_t> (blockSize));

        for (auto i = start; i < end; ++i)
        {
            float y = 0.0f;

            if (decimator.pushSample (in[i], y))
                out.push_back (y);
        }
    }

    return out;
}

double rmsOf (const std::vector<float>& x, std::size_t from)
{
    double sum = 0.0;

    for (auto i = from; i < x.size(); ++i)
        sum += static_cast<double> (x[i]) * static_cast<double> (x[i]);

    return std::sqrt (sum / static_cast<double> (x.size() - from));
}

/** Gain in dB of decimating a sine at the input rate (steady state, after the FIR has filled). */
double toneGainDb (double sampleRate, double frequencyHz)
{
    Decimator decimator;
    decimator.prepare (sampleRate);
    const auto in = test::sine (sampleRate, frequencyHz, 0.25, 0.5f);
    const auto out = decimate (decimator, in);
    const auto skip = static_cast<std::size_t> (decimator.getNumTaps()); // in output samples: generous
    return 20.0 * std::log10 (rmsOf (out, skip) / (0.5 / std::sqrt (2.0)));
}

/** Impulse response h[0..N) reconstructed from the outputs: an impulse at input n = p gives
    y[k] = h[k D - p]. */
std::vector<float> measuredTaps (double sampleRate)
{
    Decimator decimator;
    decimator.prepare (sampleRate);
    const auto d = decimator.getFactor();
    const auto numTaps = decimator.getNumTaps();
    std::vector<float> h (static_cast<std::size_t> (numTaps), -1.0f);

    for (int p = 0; p < d; ++p)
    {
        decimator.reset();
        std::vector<float> in (static_cast<std::size_t> (numTaps + 2 * d), 0.0f);
        in[static_cast<std::size_t> (p)] = 1.0f;
        const auto out = decimate (decimator, in);

        for (std::size_t k = 0; k < out.size(); ++k)
        {
            const auto j = static_cast<int> (k) * d - p;

            if (j >= 0 && j < numTaps)
                h[static_cast<std::size_t> (j)] = out[k];
            else
                REQUIRE (out[k] == 0.0f); // causal and exactly N taps long
        }
    }

    return h;
}

} // namespace

TEST_CASE ("Decimation factor follows floor (sampleRate / 16000)", "[decimator]")
{
    CHECK (Decimator::factorFor (44100.0) == 2);
    CHECK (Decimator::factorFor (48000.0) == 3);
    CHECK (Decimator::factorFor (88200.0) == 5);
    CHECK (Decimator::factorFor (96000.0) == 6);
    CHECK (Decimator::factorFor (176400.0) == 11);
    CHECK (Decimator::factorFor (192000.0) == 12);
    CHECK (Decimator::factorFor (32000.0) == 2);
    CHECK (Decimator::factorFor (31999.0) == 1);
    CHECK (Decimator::factorFor (22050.0) == 1);
    CHECK (Decimator::factorFor (8000.0) == 1);
    CHECK (Decimator::factorFor (0.0) == 1);
    CHECK (Decimator::factorFor (-1.0) == 1);

    // Tap counts of the design table (docs/M2-DESIGN.md 3.3)
    CHECK (Decimator::numTapsFor (1) == 1);
    CHECK (Decimator::numTapsFor (2) == 27);
    CHECK (Decimator::numTapsFor (3) == 39);
    CHECK (Decimator::numTapsFor (5) == 63);
    CHECK (Decimator::numTapsFor (6) == 75);
    CHECK (Decimator::numTapsFor (12) == 147);

    for (int d = 2; d <= 13; ++d)
        CHECK (Decimator::numTapsFor (d) % 2 == 1);

    Decimator decimator;
    decimator.prepare (96000.0);
    CHECK (decimator.getFactor() == 6);
    CHECK (decimator.getNumTaps() == 75);
    CHECK (decimator.getGroupDelayInputSamples() == 37);
    CHECK (decimator.getOutputSampleRate() == 16000.0);
    CHECK (decimator.getInputSampleRate() == 96000.0);
}

TEST_CASE ("Decimator taps: symmetric, DC gain 1, peak at the group delay", "[decimator]")
{
    const auto sampleRate = GENERATE (44100.0, 48000.0, 88200.0, 96000.0, 192000.0);
    CAPTURE (sampleRate);

    const auto h = measuredTaps (sampleRate);
    const auto numTaps = static_cast<int> (h.size());
    REQUIRE (numTaps == Decimator::numTapsFor (Decimator::factorFor (sampleRate)));

    double sum = 0.0;

    for (int i = 0; i < numTaps; ++i)
    {
        REQUIRE (h[static_cast<std::size_t> (i)] == h[static_cast<std::size_t> (numTaps - 1 - i)]); // linear phase
        sum += static_cast<double> (h[static_cast<std::size_t> (i)]);
    }

    CHECK_THAT (sum, WithinAbs (1.0, 1.0e-6));

    const auto peak = std::max_element (h.begin(), h.end()) - h.begin();
    Decimator decimator;
    decimator.prepare (sampleRate);
    CHECK (peak == decimator.getGroupDelayInputSamples());

    // DC: a constant input settles to exactly the same constant (within float rounding)
    const auto out = decimate (decimator, std::vector<float> (4096, 0.25f));

    for (auto k = static_cast<std::size_t> (numTaps); k < out.size(); ++k)
        REQUIRE_THAT (out[k], WithinAbs (0.25, 1.0e-6));
}

TEST_CASE ("Decimator anti-alias attenuation >= 60 dB above 0.65 fa", "[decimator]")
{
    const auto sampleRate = GENERATE (44100.0, 48000.0, 88200.0, 96000.0, 192000.0);
    const auto fa = sampleRate / static_cast<double> (Decimator::factorFor (sampleRate));
    CAPTURE (sampleRate);

    // Tones from the stopband edge up to just below the input Nyquist, including frequencies that
    // would alias onto pitch-relevant frequencies (k fa +- 200 Hz) or onto DC (k fa)
    std::vector<double> tones;

    for (double f = Decimator::stopbandFraction * fa; f < 0.499 * sampleRate; f += 0.0731 * fa)
        tones.push_back (f);

    for (double k = 1.0; k * fa < 0.499 * sampleRate; k += 1.0)
        for (const auto offset : { -200.0, 0.0, 200.0 })
            if (k * fa + offset >= Decimator::stopbandFraction * fa && k * fa + offset < 0.499 * sampleRate)
                tones.push_back (k * fa + offset);

    for (const auto f : tones)
    {
        CAPTURE (f);
        CHECK (toneGainDb (sampleRate, f) <= -Decimator::stopbandAttenuationDb);
    }
}

TEST_CASE ("Decimator passband ripple < 0.1 dB up to 0.35 fa", "[decimator]")
{
    const auto sampleRate = GENERATE (44100.0, 48000.0, 96000.0);
    const auto fa = sampleRate / static_cast<double> (Decimator::factorFor (sampleRate));
    CAPTURE (sampleRate);

    for (double f = 50.0; f <= Decimator::passbandFraction * fa; f += 0.0317 * fa)
    {
        CAPTURE (f);
        CHECK_THAT (toneGainDb (sampleRate, f), WithinAbs (0.0, 0.1));
    }

    // The whole pitch band (Whistle f0 max 2.5 kHz) passes untouched
    CHECK_THAT (toneGainDb (sampleRate, 2500.0), WithinAbs (0.0, 0.1));
}

TEST_CASE ("Decimator emits exactly at n % D == 0, y[k] = sum h[j] x[kD - j]", "[decimator]")
{
    const auto sampleRate = GENERATE (44100.0, 48000.0, 96000.0, 22050.0);
    CAPTURE (sampleRate);

    Decimator decimator;
    decimator.prepare (sampleRate);
    const auto d = decimator.getFactor();
    const auto h = measuredTaps (sampleRate);

    auto in = test::whiteNoise (sampleRate, 0.05, 0.5f, 11);
    decimator.reset();
    std::size_t k = 0;

    for (std::size_t n = 0; n < in.size(); ++n)
    {
        float y = 0.0f;
        const auto produced = decimator.pushSample (in[n], y);
        REQUIRE (produced == (n % static_cast<std::size_t> (d) == 0));

        if (! produced)
            continue;

        double expected = 0.0;

        for (std::size_t j = 0; j < h.size() && j <= n; ++j)
            expected += static_cast<double> (h[j]) * static_cast<double> (in[n - j]);

        REQUIRE_THAT (y, WithinAbs (expected, 1.0e-5));
        ++k;
    }

    CHECK (k == (in.size() + static_cast<std::size_t> (d) - 1) / static_cast<std::size_t> (d));

    // reset() restarts the phase: the next push (n = 0) produces an output
    float y = 0.0f;
    decimator.pushSample (0.0f, y);
    decimator.reset();
    CHECK (decimator.pushSample (0.0f, y));
}

TEST_CASE ("Decimator with D == 1 is an exact pass-through", "[decimator]")
{
    Decimator decimator;
    decimator.prepare (24000.0);
    REQUIRE (decimator.getFactor() == 1);
    CHECK (decimator.getNumTaps() == 1);
    CHECK (decimator.getGroupDelayInputSamples() == 0);

    const auto in = test::whiteNoise (24000.0, 0.01, 0.9f, 3);
    CHECK (decimate (decimator, in) == in);
}

TEST_CASE ("Decimator is block-size invariant (bit-exact)", "[decimator]")
{
    const auto sampleRate = GENERATE (44100.0, 48000.0, 96000.0);
    CAPTURE (sampleRate);

    auto in = test::saw (sampleRate, 196.0, 0.3, 0.4f);
    test::mixInto (in, test::whiteNoise (sampleRate, 0.3, 0.05f, 5));

    Decimator reference;
    reference.prepare (sampleRate);
    const auto expected = decimate (reference, in, static_cast<int> (in.size()));

    for (const int blockSize : { 1, 7, 64, 480, 512, 4096 })
    {
        CAPTURE (blockSize);
        Decimator decimator;
        decimator.prepare (sampleRate);
        REQUIRE (decimate (decimator, in, blockSize) == expected);

        // ... and after a reset() the same again
        decimator.reset();
        REQUIRE (decimate (decimator, in, blockSize) == expected);
    }
}

TEST_CASE ("Decimator group delay is (N - 1) / 2 input samples", "[decimator]")
{
    const auto sampleRate = GENERATE (44100.0, 48000.0, 96000.0);
    CAPTURE (sampleRate);

    Decimator decimator;
    decimator.prepare (sampleRate);
    const auto d = decimator.getFactor();
    const auto delay = decimator.getGroupDelayInputSamples();
    CHECK (delay == (decimator.getNumTaps() - 1) / 2);

    // A slow in-band sine comes out delayed by exactly `delay` input samples: compare the decimated
    // output with the analytic sine at (kD - delay)
    const auto f = 300.0;
    const auto in = test::sine (sampleRate, f, 0.1, 0.5f);
    const auto out = decimate (decimator, in);

    for (auto k = static_cast<std::size_t> (decimator.getNumTaps()); k < out.size(); ++k)
    {
        const auto t = (static_cast<double> (k) * d - delay) / sampleRate;
        REQUIRE_THAT (out[k], WithinAbs (0.5 * std::sin (2.0 * std::numbers::pi * f * t), 2.0e-3));
    }
}
