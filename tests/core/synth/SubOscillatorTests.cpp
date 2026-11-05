// SubOscillator: shapes and octaves (FFT), band limit, shared tables, level, mono.
// Test plan: docs/SYNTH-DESIGN.md 21. Owner: agent A (Wavetable).

#include "vocalsynth/synth/SubOscillator.h"

#include "SynthTestUtils.h"

#include <catch2/catch_test_macros.hpp>

#include "vocalsynth/synth/NoiseSource.h"

#include <chrono>
#include <cstdio>
#include <type_traits>

using namespace vocalsynth::synth;
using namespace vocalsynth::synth::test;

namespace
{
constexpr std::size_t fftSize = 65536;

Stereo renderSub (double fs, double increment, float level, SubShape shape, int numSamples, std::span<const int> blocks = {})
{
    SubOscillator sub;
    sub.prepare (fs);
    sub.restart();
    Stereo out (static_cast<std::size_t> (numSamples));
    runTicks (
        numSamples, blocks, [&] (int) { sub.beginTick (increment, level, shape, controlTickSamples); },
        [&] (int o, int n) { sub.render (out.left.data() + o, out.right.data() + o, n); });
    return out;
}

/** Power of harmonic k relative to the fundamental (dB), f0 on bin m. */
struct HarmonicSpectrum
{
    std::vector<double> power;
    std::size_t m = 1;

    double harmonicDb (std::size_t k) const { return toDb (power[k * m] / power[m]); }
};

HarmonicSpectrum analyse (double fs, double f0, SubShape shape, float level = 1.0f)
{
    HarmonicSpectrum h;
    h.m = static_cast<std::size_t> (std::llround (f0 * fftSize / fs)) | 1u;
    const auto out = renderSub (fs, static_cast<double> (h.m) / fftSize, level, shape, static_cast<int> (fftSize) + 16);
    h.power = powerSpectrum (std::span<const float> (out.left).subspan (16, fftSize));
    return h;
}
} // namespace

TEST_CASE ("SubOscillator: real-time structure", "[synth][sub]")
{
    STATIC_REQUIRE (std::is_trivially_destructible_v<SubOscillator>); // the tables are shared, not owned
    SubOscillator sub;
    float l = 0.0f, r = 0.0f;
    STATIC_REQUIRE (noexcept (sub.render (&l, &r, 1)));
    STATIC_REQUIRE (noexcept (sub.beginTick (0.01, 1.0f, SubShape::sine, 16)));
    STATIC_REQUIRE (noexcept (sub.restart()));
    STATIC_REQUIRE (noexcept (sub.reset()));
}

TEST_CASE ("SubOscillator: shapes have their harmonic series", "[synth][sub]")
{
    for (const double fs : { 44100.0, 48000.0, 96000.0 })
    {
        const auto sine = analyse (fs, 55.0, SubShape::sine);
        const auto triangle = analyse (fs, 55.0, SubShape::triangle);
        const auto square = analyse (fs, 55.0, SubShape::square);
        CAPTURE (fs);

        for (std::size_t k = 2; k <= 9; ++k)
            CHECK (sine.harmonicDb (k) < -100.0);

        for (const std::size_t k : { 3u, 5u, 7u, 9u })
        {
            const double kd = static_cast<double> (k);
            CHECK (std::abs (triangle.harmonicDb (k) - toDb (1.0 / (kd * kd * kd * kd))) < 0.05); // 1 / k^2
            CHECK (std::abs (square.harmonicDb (k) - toDb (1.0 / (kd * kd))) < 0.05);              // 1 / k
        }

        for (const std::size_t k : { 2u, 4u, 6u, 8u })
        {
            CHECK (triangle.harmonicDb (k) < -100.0);
            CHECK (square.harmonicDb (k) < -100.0);
        }

        // Non-harmonic energy (alias gate measure) far below the fundamental
        CHECK (nonHarmonicDb (sine.power, sine.m) < -100.0);
        CHECK (nonHarmonicDb (triangle.power, triangle.m) < -70.0);
        CHECK (nonHarmonicDb (square.power, square.m) < -70.0);
    }
}

TEST_CASE ("SubOscillator: band-limited at high pitches", "[synth][sub]")
{
    // A square at 3 / 5 kHz: only harmonics below 0.45 fs, nothing else above -60 dB.
    for (const double fs : { 44100.0, 48000.0, 96000.0 })
        for (const double f0 : { 1000.0, 3000.0, 5000.0 })
        {
            const auto h = analyse (fs, f0, SubShape::square);
            const double f = static_cast<double> (h.m) * fs / fftSize;
            CAPTURE (fs, f0);
            CHECK (nonHarmonicDb (h.power, h.m) < (f0 <= 1000.0 ? -70.0 : -60.0));

            for (std::size_t k = 3; k * h.m < h.power.size(); k += 2)
                if (static_cast<double> (k) * f > 0.45 * fs)
                    CHECK (h.harmonicDb (k) < -100.0);
        }
}

TEST_CASE ("SubOscillator: octave pitch, level, mono and restart", "[synth][sub]")
{
    constexpr double fs = 48000.0;

    for (const double note : { 440.0, 110.0, 1760.0 })
        for (const double octave : { 0.5, 0.25 })
        {
            const auto out = renderSub (fs, note * octave / fs, 1.0f, SubShape::sine, 65536 + 1024 + 16);
            CHECK (std::abs (cents (measureFrequency (std::span<const float> (out.left).subspan (16), fs), note * octave)) < 0.1);
        }

    // Level is linear, clamped to 0..1, the same on both channels; restart starts at phase 0 with a gain ramp
    const auto full = renderSub (fs, 0.01, 1.0f, SubShape::triangle, 4096);
    const auto half = renderSub (fs, 0.01, 0.5f, SubShape::triangle, 4096);
    const auto over = renderSub (fs, 0.01, 3.0f, SubShape::triangle, 4096);
    const auto none = renderSub (fs, 0.01, 0.0f, SubShape::triangle, 4096);
    CHECK (full.left == full.right);
    CHECK (over.left == full.left);
    CHECK (peakAbs (std::span<const float> (none.left)) == 0.0);
    CHECK (std::abs (rms (std::span<const float> (half.left).subspan (16)) / rms (std::span<const float> (full.left).subspan (16)) - 0.5) < 1e-6);
    CHECK (full.left[0] == 0.0f);
    // Peak-normalised table (a triangle with 32 harmonics at this pitch has slightly rounded corners)
    CHECK (peakAbs (std::span<const float> (full.left).subspan (16)) > 0.97);
    CHECK (peakAbs (std::span<const float> (full.left).subspan (16)) <= 1.0);

    // Shape switch at the tick, phase continuous (no restart): sine up to sample 47, square from 48 on
    SubOscillator sub;
    sub.prepare (fs);
    sub.restart();
    Stereo out (64);

    for (int tick = 0; tick < 4; ++tick)
    {
        sub.beginTick (0.01, 1.0f, tick < 3 ? SubShape::sine : SubShape::square, 16);
        sub.render (out.left.data() + tick * 16, out.right.data() + tick * 16, 16);
    }

    CHECK (std::abs (out.left[47] - static_cast<float> (std::sin (2.0 * std::numbers::pi * 0.47))) < 1e-3f);
    CHECK (out.left[48] > 0.8f);  // square plateau at phase 0.48 (a restarted phase would be at its edge)
    CHECK (out.left[52] < -0.8f); // past the edge at phase 0.5
}

TEST_CASE ("SubOscillator: bit-identical for every block split, mip changes during a glide", "[synth][sub]")
{
    const auto render = [] (double fs, int block)
    {
        SubOscillator sub;
        sub.prepare (fs);
        sub.restart();
        const int numSamples = static_cast<int> (0.3 * fs);
        Stereo out (static_cast<std::size_t> (numSamples));
        const int blocks[] = { block };
        runTicks (
            numSamples, blocks,
            [&] (int tick)
            {
                const double t = static_cast<double> (tick * 16) / fs;
                sub.beginTick (20.0 * std::pow (500.0, t / 0.3) / fs, 0.7f, tick % 400 < 200 ? SubShape::square : SubShape::triangle, 16);
            },
            [&] (int o, int n) { sub.render (out.left.data() + o, out.right.data() + o, n); });
        return out;
    };

    for (const double fs : { 44100.0, 48000.0, 96000.0 })
    {
        const auto reference = render (fs, 1);

        for (const int block : { 7, 64, 480, 512, 4096 })
            CHECK (render (fs, block).left == reference.left);
    }
}

TEST_CASE ("SubOscillator + NoiseSource: benchmark", "[synth][sub][noise][!benchmark]")
{
    // ns per voice per sample for sub + noise together (budget 3 ns, docs/SYNTH-DESIGN.md 18), incl. ticks.
    SubOscillator sub;
    sub.prepare (48000.0);
    sub.restart();
    NoiseSource noise;
    noise.prepare (48000.0, 3u);
    std::array<float, 128> l {}, r {};
    double best = 1e9;

    for (int run = 0; run < 9; ++run)
    {
        const int blocks = 4000;
        const auto start = std::chrono::steady_clock::now();

        for (int b = 0; b < blocks; ++b)
            for (int s = 0; s < 128; s += 16)
            {
                sub.beginTick (55.0 / 48000.0, 0.5f, SubShape::triangle, 16);
                noise.beginTick (0.3f, 0.5f, 16);
                sub.render (l.data() + s, r.data() + s, 16);
                noise.render (l.data() + s, r.data() + s, 16);
            }

        const double seconds = std::chrono::duration<double> (std::chrono::steady_clock::now() - start).count();
        best = std::min (best, seconds * 1e9 / (blocks * 128.0));
    }

    std::printf ("Sub + Noise: %.2f ns per voice per sample, checksum %g\n", best, static_cast<double> (l[3] + r[100]));
    CHECK (best > 0.0);
}
