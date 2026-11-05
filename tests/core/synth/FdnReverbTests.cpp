// FdnReverb: RT60 vs size (+-20 %), damping, mix law, on/off fade, bypass, stereo decorrelation, tail end in
// exact 0, NaN guard, block-size invariance, benchmark. Test plan: docs/SYNTH-DESIGN.md 21. Owner: agent C.

#include "vocalsynth/synth/fx/FdnReverb.h"

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

template <typename SettingsAt>
st::Stereo run (FdnReverb& reverb, st::Stereo signal, int block, SettingsAt&& settingsAt, float mixMod = 0.0f)
{
    const std::array<int, 1> blocks { block };
    st::runTicks (static_cast<int> (signal.size()), blocks,
                  [&] (int t) { reverb.beginTick (settingsAt (t), mixMod, controlTickSamples); },
                  [&] (int offset, int n) { reverb.process (signal.left.data() + offset, signal.right.data() + offset, n); });
    return signal;
}

st::Stereo noise (std::size_t n, uint32_t seed = 1, float amplitude = 0.5f)
{
    st::Stereo s (n);

    for (std::size_t i = 0; i < n; ++i)
    {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        s.left[i] = s.right[i] = amplitude * static_cast<float> (static_cast<double> (seed) / 2147483648.0 - 1.0);
    }

    return s;
}

ReverbSettings wetOnly (float size, float damping = 0.0f)
{
    ReverbSettings s;
    s.on = true;
    s.sizePercent = size;
    s.dampingPercent = damping;
    s.mixPercent = 100.0f;
    return s;
}

/** RT60 from the Schroeder backward integral of the impulse response energy (T30: -5 .. -35 dB, x 2). */
double measureRt60 (const st::Stereo& response, double sampleRate)
{
    const auto n = response.size();
    std::vector<double> integral (n + 1, 0.0);

    for (std::size_t i = n; i-- > 0;)
        integral[i] = integral[i + 1] + static_cast<double> (response.left[i]) * response.left[i]
                      + static_cast<double> (response.right[i]) * response.right[i];

    const auto total = integral[0];
    double t5 = -1.0, t35 = -1.0;

    for (std::size_t i = 0; i < n; ++i)
    {
        const auto db = 10.0 * std::log10 (std::max (integral[i] / total, 1e-300));

        if (t5 < 0.0 && db <= -5.0)
            t5 = static_cast<double> (i) / sampleRate;

        if (t35 < 0.0 && db <= -35.0)
        {
            t35 = static_cast<double> (i) / sampleRate;
            break;
        }
    }

    return t5 >= 0.0 && t35 > t5 ? 2.0 * (t35 - t5) : -1.0;
}

double expectedRt60 (float sizePercent)
{
    return 0.3 * std::pow (10.0 / 0.3, static_cast<double> (sizePercent) / 100.0);
}
} // namespace

TEST_CASE ("FdnReverb: off after reset is bypassed and leaves the signal untouched", "[synth][fx]")
{
    FdnReverb reverb;
    reverb.prepare (48000.0);
    CHECK (reverb.isBypassed());
    const auto in = noise (5000);
    const auto out = run (reverb, in, 64, [] (int) { return ReverbSettings {}; });
    CHECK (out.left == in.left);
    CHECK (out.right == in.right);
}

TEST_CASE ("FdnReverb: RT60 follows 0.3 * (10 / 0.3)^size within 20 %", "[synth][fx]")
{
    const auto sr = GENERATE (44100.0, 48000.0, 96000.0);
    const auto size = GENERATE (0.0f, 30.0f, 60.0f, 100.0f);
    CAPTURE (sr, size);
    const auto expected = expectedRt60 (size);

    FdnReverb reverb;
    reverb.prepare (sr);
    st::Stereo in (static_cast<std::size_t> ((1.1 * expected + 0.3) * sr));
    in.left[0] = in.right[0] = 1.0f;
    const auto out = run (reverb, in, 512, [&] (int) { return wetOnly (size); });
    const auto rt60 = measureRt60 (out, sr);
    CAPTURE (expected, rt60);
    CHECK (rt60 > 0.8 * expected);
    CHECK (rt60 < 1.2 * expected);
}

TEST_CASE ("FdnReverb: damping shortens the high-frequency decay", "[synth][fx]")
{
    const double sr = 48000.0;
    const auto highShare = [sr] (float damping)
    {
        FdnReverb reverb;
        reverb.prepare (sr);
        st::Stereo in (static_cast<std::size_t> (1.5 * sr));
        in.left[0] = in.right[0] = 1.0f;
        const auto out = run (reverb, in, 512, [&] (int) { return wetOnly (50.0f, damping); });
        const auto start = static_cast<std::size_t> (0.5 * sr);
        const auto p = st::powerSpectrum (std::span<const float> (out.left.data() + start, 32768));
        double low = 0.0, high = 0.0;

        for (std::size_t k = 1; k < p.size(); ++k)
            (static_cast<double> (k) * sr / 32768.0 < 2000.0 ? low : high) += p[k];

        return high / low;
    };

    CHECK (highShare (100.0f) < 0.1 * highShare (0.0f));
}

TEST_CASE ("FdnReverb: dual-linear mix law, mix modulation", "[synth][fx]")
{
    const auto mix = GENERATE (0.0f, 25.0f, 50.0f, 75.0f, 100.0f);
    CAPTURE (mix);
    FdnReverb reverb;
    reverb.prepare (48000.0);
    auto s = wetOnly (50.0f);
    s.mixPercent = mix;
    st::Stereo in (4000);
    in.left[0] = in.right[0] = 1.0f;
    const auto out = run (reverb, in, 64, [&] (int) { return s; });
    const auto m = mix / 100.0f;
    CHECK_THAT (out.left[0], WithinAbs (std::min (1.0f, 2.0f * (1.0f - m)), 1e-6)); // the wet path is >= 10 ms late

    FdnReverb modulated;
    modulated.prepare (48000.0);
    auto half = wetOnly (50.0f);
    half.mixPercent = 50.0f;
    const auto out2 = run (modulated, in, 64, [&] (int) { return half; }, 0.25f); // 0.75 -> dry 0.5
    CHECK_THAT (out2.left[0], WithinAbs (0.5, 1e-6));
}

TEST_CASE ("FdnReverb: stereo output of a mono input is decorrelated", "[synth][fx]")
{
    FdnReverb reverb;
    reverb.prepare (48000.0);
    const auto out = run (reverb, noise (96000), 512, [] (int) { return wetOnly (60.0f, 30.0f); });
    double lr = 0.0, ll = 0.0, rr = 0.0;

    for (std::size_t i = 24000; i < out.size(); ++i)
    {
        lr += static_cast<double> (out.left[i]) * out.right[i];
        ll += static_cast<double> (out.left[i]) * out.left[i];
        rr += static_cast<double> (out.right[i]) * out.right[i];
    }

    CHECK (std::abs (lr / std::sqrt (ll * rr)) < 0.3);
    CHECK (ll > 0.0);
}

TEST_CASE ("FdnReverb: switching off fades over 10 ms, then bypassed with exactly the dry signal", "[synth][fx]")
{
    const double sr = 48000.0;
    FdnReverb reverb;
    reverb.prepare (sr);
    auto on = wetOnly (80.0f);
    auto off = on;
    off.on = false;
    const auto in = noise (static_cast<std::size_t> (0.4 * sr));
    const int switchTick = 600;
    const auto out = run (reverb, in, 480, [&] (int t) { return t < switchTick ? on : off; });

    for (std::size_t i = static_cast<std::size_t> ((switchTick + 31) * controlTickSamples); i < in.size(); ++i)
        REQUIRE (out.left[i] == in.left[i]);

    CHECK (reverb.isBypassed());
}

TEST_CASE ("FdnReverb: the tail ends in exact 0 after the input stops", "[synth][fx]")
{
    const double sr = 48000.0;
    FdnReverb reverb;
    reverb.prepare (sr);
    auto s = wetOnly (0.0f, 50.0f);
    s.mixPercent = 40.0f;
    auto in = noise (static_cast<std::size_t> (2.5 * sr));
    std::fill (in.left.begin() + 4800, in.left.end(), 0.0f);
    std::fill (in.right.begin() + 4800, in.right.end(), 0.0f);
    const auto out = run (reverb, in, 512, [&] (int) { return s; });

    for (std::size_t i = static_cast<std::size_t> (1.6 * sr); i < out.size(); ++i)
    {
        REQUIRE (out.left[i] == 0.0f);
        REQUIRE (out.right[i] == 0.0f);
    }

    // Wakes up again on the next input
    st::Stereo ping (4800);
    ping.left[10] = ping.right[10] = 1.0f;
    const auto again = run (reverb, ping, 7, [&] (int) { return s; });
    CHECK (again.left[10] == 1.0f);
    CHECK (st::rms (std::span<const float> (again.left.data() + 1000, 3000)) > 1e-4);
}

TEST_CASE ("FdnReverb: a NaN input resets the network at the next tick", "[synth][fx]")
{
    FdnReverb reverb;
    reverb.prepare (48000.0);
    auto in = noise (48000);
    in.left[3000] = std::numeric_limits<float>::quiet_NaN();
    const auto out = run (reverb, in, 64, [] (int) { return wetOnly (90.0f); });
    CHECK (reverb.getNanResetCount() >= 1);

    for (std::size_t i = 12000; i < out.size(); ++i)
    {
        REQUIRE (std::isfinite (out.left[i]));
        REQUIRE (std::isfinite (out.right[i]));
    }
}

TEST_CASE ("FdnReverb: bit-identical for every host block split", "[synth][fx]")
{
    const auto sr = GENERATE (44100.0, 48000.0, 96000.0);
    CAPTURE (sr);
    auto in = noise (static_cast<std::size_t> (1.2 * sr), 9);
    std::fill (in.left.begin() + static_cast<std::ptrdiff_t> (0.5 * sr), in.left.end(), 0.0f);
    std::fill (in.right.begin() + static_cast<std::ptrdiff_t> (0.5 * sr), in.right.end(), 0.0f);
    const auto settingsAt = [] (int t)
    {
        ReverbSettings s;
        s.on = (t / 1500) % 5 != 4;
        s.sizePercent = static_cast<float> ((t / 40) % 101);
        s.dampingPercent = static_cast<float> ((t / 25) % 101);
        s.mixPercent = static_cast<float> ((t / 9) % 101);
        return s;
    };

    FdnReverb reference;
    reference.prepare (sr);
    const auto expected = run (reference, in, 4096, settingsAt);

    for (const auto block : hostBlocks)
    {
        CAPTURE (block);
        FdnReverb reverb;
        reverb.prepare (sr);
        const auto out = run (reverb, in, block, settingsAt);
        CHECK (out.left == expected.left);
        CHECK (out.right == expected.right);
    }
}

TEST_CASE ("FdnReverb: benchmark", "[synth][fx][!benchmark]")
{
    FdnReverb reverb;
    reverb.prepare (48000.0);
    auto s = wetOnly (50.0f, 50.0f);
    s.mixPercent = 25.0f;
    auto in = noise (4096);

    BENCHMARK ("reverb 4096 samples (budget 60 ns / sample = 246 us)")
    {
        run (reverb, in, 128, [&] (int) { return s; });
        return reverb.isBypassed();
    };
}
