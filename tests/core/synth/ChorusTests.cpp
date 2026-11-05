// Chorus: mix law, on/off fade, bypass, tap delays, block-size invariance, tail end, NaN guard, benchmark.
// Test plan: docs/SYNTH-DESIGN.md 21. Owner: agent C (Engine + FX + Expression).

#include "vocalsynth/synth/fx/Chorus.h"

#include "SynthTestUtils.h"

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <vector>

using namespace vocalsynth::synth;
using Catch::Matchers::WithinAbs;
namespace st = vocalsynth::synth::test;

namespace
{
constexpr std::array<int, 6> hostBlocks { 1, 7, 64, 480, 512, 4096 };

/** Runs the chorus over the input in place: beginTick at every absolute multiple of 16 with the settings of
    that tick, process() between control positions and host block ends. */
template <typename SettingsAt>
st::Stereo run (Chorus& chorus, st::Stereo signal, int block, SettingsAt&& settingsAt, float mixMod = 0.0f)
{
    const std::array<int, 1> blocks { block };
    st::runTicks (static_cast<int> (signal.size()), blocks,
                  [&] (int t) { chorus.beginTick (settingsAt (t), mixMod, controlTickSamples); },
                  [&] (int offset, int n) { chorus.process (signal.left.data() + offset, signal.right.data() + offset, n); });
    return signal;
}

st::Stereo noise (std::size_t n, uint32_t seed = 1)
{
    st::Stereo s (n);

    for (std::size_t i = 0; i < n; ++i)
    {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        s.left[i] = static_cast<float> (static_cast<double> (seed) / 4294967296.0 - 0.5);
        s.right[i] = static_cast<float> (std::sin (0.01 * static_cast<double> (i)) * 0.5);
    }

    return s;
}

st::Stereo impulse (std::size_t n, std::size_t at = 0)
{
    st::Stereo s (n);
    s.left[at] = 1.0f;
    s.right[at] = 1.0f;
    return s;
}
} // namespace

TEST_CASE ("Chorus: off after reset is bypassed and leaves the signal untouched", "[synth][fx]")
{
    Chorus chorus;
    chorus.prepare (48000.0);
    CHECK (chorus.isBypassed());
    const auto in = noise (4000);
    const auto out = run (chorus, in, 64, [] (int) { return ChorusSettings {}; });
    CHECK (out.left == in.left);
    CHECK (out.right == in.right);
    CHECK (chorus.isBypassed());
}

TEST_CASE ("Chorus: dual-linear mix law and the tap delays (7 ms left, 11 ms right)", "[synth][fx]")
{
    const auto mix = GENERATE (0.0f, 25.0f, 50.0f, 75.0f, 100.0f);
    CAPTURE (mix);
    Chorus chorus;
    chorus.prepare (48000.0);
    ChorusSettings s;
    s.on = true;
    s.depthPercent = 0.0f; // static taps
    s.mixPercent = mix;
    const auto out = run (chorus, impulse (2000), 64, [&] (int) { return s; });
    const auto m = mix / 100.0f;
    const auto dry = std::min (1.0f, 2.0f * (1.0f - m));
    const auto wet = std::min (1.0f, 2.0f * m);
    CHECK_THAT (out.left[0], WithinAbs (dry, 1e-6));
    CHECK_THAT (out.right[0], WithinAbs (dry, 1e-6));
    CHECK_THAT (out.left[336], WithinAbs (wet, 1e-6));  // 7 ms at 48 kHz
    CHECK_THAT (out.right[528], WithinAbs (wet, 1e-6)); // 11 ms
    CHECK_THAT (out.left[528], WithinAbs (0.0, 1e-6));
    CHECK_THAT (out.right[336], WithinAbs (0.0, 1e-6));
}

TEST_CASE ("Chorus: the mix modulation adds to the mix fraction, clamped", "[synth][fx]")
{
    Chorus chorus;
    chorus.prepare (48000.0);
    ChorusSettings s;
    s.on = true;
    s.mixPercent = 30.0f;
    const auto out = run (chorus, impulse (100), 64, [&] (int) { return s; }, 0.6f); // 0.9 -> dry 0.2
    CHECK_THAT (out.left[0], WithinAbs (0.2, 1e-5));
    Chorus clamped;
    clamped.prepare (48000.0);
    const auto full = run (clamped, impulse (100), 64, [&] (int) { return s; }, 5.0f);
    CHECK_THAT (full.left[0], WithinAbs (0.0, 1e-6));
}

TEST_CASE ("Chorus: depth modulates the delay between base - 4 ms and base + 4 ms", "[synth][fx]")
{
    // A 1 kHz tone through the left tap only (mix 100 %): the phase modulation shows as sidebands at
    // 1 kHz +- rate; depth 0 gives none.
    const double sr = 48000.0;
    const auto sidebandDb = [sr] (float depth)
    {
        Chorus chorus;
        chorus.prepare (sr);
        ChorusSettings s;
        s.on = true;
        s.depthPercent = depth;
        s.rateHz = 5.0f;
        s.mixPercent = 100.0f;
        const std::size_t n = 65536;
        st::Stereo in (n);

        for (std::size_t i = 0; i < n; ++i)
            in.left[i] = in.right[i] = static_cast<float> (0.5 * std::sin (2.0 * std::numbers::pi * 1000.0 * static_cast<double> (i) / sr));

        const auto out = run (chorus, in, 512, [&] (int) { return s; });
        const auto window = st::hannWindow (32768);
        const auto p = st::powerSpectrum (std::span<const float> (out.left.data() + 32768, 32768), std::span<const double> (window));
        const auto bin = [&] (double hz) { return static_cast<std::size_t> (std::lround (hz * 32768.0 / sr)); };
        double side = 0.0;

        for (std::size_t k = bin (1015); k <= bin (1060); ++k)
            side += p[k];

        return st::toDb (side / p[bin (1000)]);
    };

    CHECK (sidebandDb (0.0f) < -60.0);
    CHECK (sidebandDb (100.0f) > -20.0);
}

TEST_CASE ("Chorus: switching off fades over 10 ms, then bypassed with exactly the dry signal", "[synth][fx]")
{
    const auto sr = GENERATE (44100.0, 48000.0, 96000.0);
    CAPTURE (sr);
    Chorus chorus;
    chorus.prepare (sr);
    ChorusSettings on;
    on.on = true;
    on.mixPercent = 100.0f;
    auto off = on;
    off.on = false;
    const int switchTick = 200;
    const auto in = noise (static_cast<std::size_t> (0.2 * sr));
    const auto out = run (chorus, in, 64, [&] (int t) { return t < switchTick ? on : off; });
    const auto fadeTicks = static_cast<int> (std::ceil (0.010 * sr / controlTickSamples));
    const auto fadeEnd = static_cast<std::size_t> ((switchTick + fadeTicks) * controlTickSamples);

    // Before the switch the dry part is 0 (mix 100 %); after the fade the output is exactly the input
    CHECK (out.left[static_cast<std::size_t> (switchTick * controlTickSamples - 1)] != in.left[static_cast<std::size_t> (switchTick * controlTickSamples - 1)]);

    for (std::size_t i = fadeEnd; i < in.size(); ++i)
    {
        REQUIRE (out.left[i] == in.left[i]);
        REQUIRE (out.right[i] == in.right[i]);
    }

    CHECK (chorus.isBypassed());

    // The fade is gradual: the distance to the dry signal shrinks tick by tick
    double previous = 1.0e9;
    int growing = 0;

    for (int t = switchTick; t < switchTick + fadeTicks; t += 4)
    {
        double e = 0.0;

        for (int i = 0; i < 4 * controlTickSamples; ++i)
        {
            const auto k = static_cast<std::size_t> (t * controlTickSamples + i);
            e += std::abs (static_cast<double> (out.left[k] - in.left[k]));
        }

        growing += e > previous * 1.5 ? 1 : 0;
        previous = e;
    }

    CHECK (growing == 0);
}

TEST_CASE ("Chorus: bit-identical for every host block split", "[synth][fx]")
{
    const auto sr = GENERATE (44100.0, 48000.0, 96000.0);
    CAPTURE (sr);
    const auto in = noise (static_cast<std::size_t> (0.5 * sr), 7);
    const auto settingsAt = [] (int t)
    {
        ChorusSettings s;
        s.on = (t / 700) % 3 != 2; // on / off / on: fades, bypass and the restart from cleared lines
        s.rateHz = 0.05f + 4.0f * static_cast<float> ((t / 50) % 7) / 6.0f;
        s.depthPercent = static_cast<float> ((t * 3) % 101);
        s.mixPercent = static_cast<float> ((t / 3) % 101);
        return s;
    };

    Chorus reference;
    reference.prepare (sr);
    const auto expected = run (reference, in, 4096, settingsAt);

    for (const auto block : hostBlocks)
    {
        CAPTURE (block);
        Chorus chorus;
        chorus.prepare (sr);
        const auto out = run (chorus, in, block, settingsAt);
        CHECK (out.left == expected.left);
        CHECK (out.right == expected.right);
    }
}

TEST_CASE ("Chorus: the tail ends in exact 0 after the input stops", "[synth][fx]")
{
    Chorus chorus;
    chorus.prepare (48000.0);
    ChorusSettings s;
    s.on = true;
    s.mixPercent = 60.0f;
    s.depthPercent = 0.0f;
    auto in = noise (20000);
    std::fill (in.left.begin() + 8000, in.left.end(), 0.0f);
    std::fill (in.right.begin() + 8000, in.right.end(), 0.0f);
    const auto out = run (chorus, in, 480, [&] (int) { return s; });

    for (std::size_t i = 8000 + 16 * 48; i < out.size(); ++i)
    {
        REQUIRE (out.left[i] == 0.0f);
        REQUIRE (out.right[i] == 0.0f);
    }

    // And it runs again when the input returns (the wet tap appears 7 ms later)
    auto again = impulse (2000, 100);
    const auto out2 = run (chorus, again, 7, [&] (int) { return s; });
    CHECK (out2.left[100] != 0.0f);
    CHECK (out2.left[436] != 0.0f);
}

TEST_CASE ("Chorus: a NaN input is cleared at the next tick", "[synth][fx]")
{
    Chorus chorus;
    chorus.prepare (48000.0);
    ChorusSettings s;
    s.on = true;
    s.mixPercent = 100.0f;
    auto in = noise (10000);
    in.left[1000] = std::numeric_limits<float>::quiet_NaN();
    const auto out = run (chorus, in, 64, [&] (int) { return s; });
    CHECK (chorus.getNanResetCount() >= 1);

    for (std::size_t i = 3000; i < out.size(); ++i)
        REQUIRE (std::isfinite (out.left[i]));
}

TEST_CASE ("Chorus: benchmark", "[synth][fx][!benchmark]")
{
    Chorus chorus;
    chorus.prepare (48000.0);
    ChorusSettings s;
    s.on = true;
    auto in = noise (4096);

    BENCHMARK ("chorus 4096 samples (budget 15 ns / sample = 61 us)")
    {
        run (chorus, in, 128, [&] (int) { return s; });
        return chorus.isBypassed();
    };
}
