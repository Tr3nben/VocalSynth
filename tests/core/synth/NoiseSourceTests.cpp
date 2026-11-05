// NoiseSource: reproducible after reset, white flat / pink -3 dB per octave, RMS level, mono.
// Test plan: docs/SYNTH-DESIGN.md 21. Owner: agent A (Wavetable).

#include "vocalsynth/synth/NoiseSource.h"

#include "SynthTestUtils.h"

#include <catch2/catch_test_macros.hpp>

#include <type_traits>

using namespace vocalsynth::synth;
using namespace vocalsynth::synth::test;

namespace
{
Stereo renderNoise (NoiseSource& noise, float level, float colour, int numSamples, std::span<const int> blocks = {})
{
    Stereo out (static_cast<std::size_t> (numSamples));
    runTicks (
        numSamples, blocks, [&] (int) { noise.beginTick (level, colour, controlTickSamples); },
        [&] (int o, int n) { noise.render (out.left.data() + o, out.right.data() + o, n); });
    return out;
}

/** Welch power spectrum (Hann, 4096, hop 2048), bins 0..2048. */
std::vector<double> welch (const std::vector<float>& x)
{
    constexpr std::size_t n = 4096;
    const auto w = hannWindow (n);
    std::vector<double> sum (n / 2 + 1, 0.0);

    for (std::size_t start = 0; start + n <= x.size(); start += n / 2)
    {
        const auto p = powerSpectrum (std::span<const float> (x).subspan (start, n), std::span<const double> (w));

        for (std::size_t k = 0; k < p.size(); ++k)
            sum[k] += p[k];
    }

    return sum;
}

/** Mean of the Welch spectrum over the band [lo, hi) Hz (density per bin). */
double bandDensity (const std::vector<double>& psd, double fs, double lo, double hi)
{
    double band = 0.0;
    int bins = 0;

    for (std::size_t k = 0; k < psd.size(); ++k)
    {
        const double f = static_cast<double> (k) * fs / 4096.0;

        if (f >= lo && f < hi)
        {
            band += psd[k];
            ++bins;
        }
    }

    return band / bins;
}
} // namespace

TEST_CASE ("NoiseSource: real-time structure", "[synth][noise]")
{
    STATIC_REQUIRE (std::is_trivially_destructible_v<NoiseSource>);
    NoiseSource noise;
    float l = 0.0f, r = 0.0f;
    STATIC_REQUIRE (noexcept (noise.render (&l, &r, 1)));
    STATIC_REQUIRE (noexcept (noise.beginTick (1.0f, 0.0f, 16)));
    STATIC_REQUIRE (noexcept (noise.reset()));
    STATIC_REQUIRE (noexcept (noise.prepare (48000.0, 1u)));
}

TEST_CASE ("NoiseSource: reproducible after reset, seeded, mono", "[synth][noise]")
{
    NoiseSource a;
    a.prepare (48000.0, 5u);
    const auto first = renderNoise (a, 1.0f, 0.5f, 4096);
    a.reset();
    const auto again = renderNoise (a, 1.0f, 0.5f, 4096);
    CHECK (first.left == again.left);
    CHECK (first.left == first.right);

    NoiseSource b;
    b.prepare (48000.0, 6u);
    CHECK (renderNoise (b, 1.0f, 0.5f, 4096).left != first.left);

    NoiseSource c;
    c.prepare (48000.0, 5u);
    CHECK (renderNoise (c, 1.0f, 0.5f, 4096).left == first.left);

    // Seed 0 is valid (xorshift32 needs a non-zero state)
    NoiseSource zero;
    zero.prepare (48000.0, 0u);
    CHECK (rms (std::span<const float> (renderNoise (zero, 1.0f, 0.0f, 4096).left)) > 0.1);

    // Level 0: exact silence; the first tick ramps up from 0
    a.reset();
    CHECK (peakAbs (std::span<const float> (renderNoise (a, 0.0f, 0.3f, 1024).left)) == 0.0);
    a.reset();
    const auto ramp = renderNoise (a, 1.0f, 0.0f, 32);
    CHECK (ramp.left[0] == 0.0f);
}

TEST_CASE ("NoiseSource: RMS about -10 dBFS for white and pink, linear level", "[synth][noise]")
{
    for (const double fs : { 44100.0, 48000.0, 96000.0 })
        for (const float colour : { 0.0f, 1.0f })
        {
            NoiseSource noise;
            noise.prepare (fs, 1234u);
            const auto out = renderNoise (noise, 1.0f, colour, 1 << 19);
            const double db = 20.0 * std::log10 (rms (std::span<const float> (out.left).subspan (16)));
            CAPTURE (fs, colour, db);
            CHECK (std::abs (db + 10.0) < (colour == 0.0f ? 0.1 : 0.5));

            noise.reset();
            const auto half = renderNoise (noise, 0.5f, colour, 1 << 16);
            noise.reset();
            const auto full = renderNoise (noise, 1.0f, colour, 1 << 16);

            for (std::size_t i = 16; i < half.size(); i += 997)
                CHECK (std::abs (half.left[i] - 0.5f * full.left[i]) < 1e-6f);
        }
}

TEST_CASE ("NoiseSource: white is flat, pink falls 3 dB per octave", "[synth][noise]")
{
    for (const double fs : { 44100.0, 48000.0, 96000.0 })
    {
        NoiseSource noise;
        noise.prepare (fs, 77u);
        const auto white = welch (renderNoise (noise, 1.0f, 0.0f, 1 << 20).left);
        noise.reset();
        const auto pink = welch (renderNoise (noise, 1.0f, 1.0f, 1 << 20).left);

        // Octave bands 125 Hz .. 16 kHz (two-thirds of an octave wide around each centre)
        const double reference = bandDensity (white, fs, 1000.0 / std::pow (2.0, 1.0 / 3.0), 1000.0 * std::pow (2.0, 1.0 / 3.0));
        const double pinkReference = bandDensity (pink, fs, 1000.0 / std::pow (2.0, 1.0 / 3.0), 1000.0 * std::pow (2.0, 1.0 / 3.0));

        for (double centre = 125.0; centre <= 16000.0; centre *= 2.0)
        {
            const double lo = centre / std::pow (2.0, 1.0 / 3.0);
            const double hi = centre * std::pow (2.0, 1.0 / 3.0);
            const double octaves = std::log2 (centre / 1000.0);
            const double whiteDb = toDb (bandDensity (white, fs, lo, hi) / reference);
            const double pinkDb = toDb (bandDensity (pink, fs, lo, hi) / pinkReference);
            CAPTURE (fs, centre, whiteDb, pinkDb);
            CHECK (std::abs (whiteDb) < 1.0);
            CHECK (std::abs (pinkDb + 3.0 * octaves) < 1.0);
        }
    }
}

TEST_CASE ("NoiseSource: bit-identical for every block split", "[synth][noise]")
{
    const auto render = [] (int block)
    {
        NoiseSource noise;
        noise.prepare (48000.0, 99u);
        Stereo out (20000);
        const int blocks[] = { block };
        runTicks (
            20000, blocks, [&] (int tick) { noise.beginTick (0.5f + 0.4f * std::sin (0.01f * static_cast<float> (tick)), static_cast<float> (tick % 100) / 100.0f, 16); },
            [&] (int o, int n) { noise.render (out.left.data() + o, out.right.data() + o, n); });
        return out;
    };

    const auto reference = render (1);

    for (const int block : { 7, 64, 480, 512, 4096 })
        CHECK (render (block).left == reference.left);
}
