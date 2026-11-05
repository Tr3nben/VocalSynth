// StereoDelay: delay time + tempo sync (host time signature), mix law, feedback bounded at 95 %, ping-pong,
// time glide, on/off fade, bypass, tail end in exact 0, NaN guard, block-size invariance, benchmark.
// Test plan: docs/SYNTH-DESIGN.md 21. Owner: agent C (Engine + FX + Expression).

#include "vocalsynth/synth/fx/StereoDelay.h"

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
st::Stereo run (StereoDelay& delay, st::Stereo signal, int block, SettingsAt&& settingsAt,
                SynthTransport transport = {}, float mixMod = 0.0f)
{
    const std::array<int, 1> blocks { block };
    st::runTicks (static_cast<int> (signal.size()), blocks,
                  [&] (int t) { delay.beginTick (settingsAt (t), transport, mixMod, controlTickSamples); },
                  [&] (int offset, int n) { delay.process (signal.left.data() + offset, signal.right.data() + offset, n); });
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
        s.left[i] = amplitude * static_cast<float> (static_cast<double> (seed) / 2147483648.0 - 1.0);
        s.right[i] = amplitude * static_cast<float> (std::sin (0.013 * static_cast<double> (i)));
    }

    return s;
}

st::Stereo impulse (std::size_t n, bool left = true, bool right = true)
{
    st::Stereo s (n);
    s.left[0] = left ? 1.0f : 0.0f;
    s.right[0] = right ? 1.0f : 0.0f;
    return s;
}

DelaySettings wetOnly (float timeMs, float feedback = 0.0f)
{
    DelaySettings s;
    s.on = true;
    s.timeMs = timeMs;
    s.feedbackPercent = feedback;
    s.mixPercent = 100.0f;
    return s;
}

std::size_t peakIndex (const std::vector<float>& x)
{
    std::size_t best = 0;

    for (std::size_t i = 1; i < x.size(); ++i)
        if (std::abs (x[i]) > std::abs (x[best]))
            best = i;

    return best;
}

double energy (const std::vector<float>& x, std::size_t from, std::size_t to)
{
    double e = 0.0;

    for (std::size_t i = from; i < std::min (to, x.size()); ++i)
        e += static_cast<double> (x[i]) * static_cast<double> (x[i]);

    return e;
}
} // namespace

TEST_CASE ("StereoDelay: off after reset is bypassed and leaves the signal untouched", "[synth][fx]")
{
    StereoDelay delay;
    delay.prepare (48000.0);
    CHECK (delay.isBypassed());
    const auto in = noise (5000);
    const auto out = run (delay, in, 64, [] (int) { return DelaySettings {}; });
    CHECK (out.left == in.left);
    CHECK (out.right == in.right);
}

TEST_CASE ("StereoDelay: delay time in ms", "[synth][fx]")
{
    const auto sr = GENERATE (44100.0, 48000.0, 96000.0);
    const auto ms = GENERATE (1.0f, 10.0f, 375.0f, 1999.0f);
    CAPTURE (sr, ms);
    StereoDelay delay;
    delay.prepare (sr);
    const auto n = static_cast<std::size_t> (2.1 * sr);
    const auto out = run (delay, impulse (n), 512, [&] (int) { return wetOnly (ms); });
    const auto expected = static_cast<double> (ms) * 0.001 * sr;
    CHECK (std::abs (static_cast<double> (peakIndex (out.left)) - expected) <= 1.0);
    CHECK (std::abs (static_cast<double> (peakIndex (out.right)) - expected) <= 1.0);
    CHECK_THAT (out.left[0], WithinAbs (0.0, 1e-9)); // mix 100 %: no dry
}

TEST_CASE ("StereoDelay: tempo sync follows bpm and the host time signature", "[synth][fx]")
{
    struct Case
    {
        TempoDivision division;
        double bpm, quarterNotesPerBar, expectedSeconds;
    };

    const auto c = GENERATE (Case { TempoDivision::d1_4, 120.0, 4.0, 0.5 }, Case { TempoDivision::d1_8D, 90.0, 4.0, 0.5 },
                             Case { TempoDivision::d1_16T, 174.0, 4.0, 60.0 / 174.0 / 6.0 },
                             Case { TempoDivision::bar1, 120.0, 3.0, 1.5 },   // 3/4
                             Case { TempoDivision::bar1, 160.0, 3.5, 1.3125 }, // 7/8
                             Case { TempoDivision::bar1, 120.0, 0.0, 2.0 },   // no signature: 4 quarter notes
                             Case { TempoDivision::d1_4, 0.0, 4.0, 0.5 },     // no tempo: 120 BPM
                             Case { TempoDivision::bars4, 120.0, 4.0, 2.0 },  // 8 s clamped to 2000 ms
                             Case { TempoDivision::d1_32, 300.0, 4.0, 0.025 });
    CAPTURE (static_cast<int> (c.division), c.bpm, c.quarterNotesPerBar);
    const double sr = 48000.0;
    StereoDelay delay;
    delay.prepare (sr);
    auto s = wetOnly (375.0f);
    s.sync = c.division;
    const auto out = run (delay, impulse (static_cast<std::size_t> (2.1 * sr)), 480, [&] (int) { return s; },
                          SynthTransport { c.bpm, c.quarterNotesPerBar });
    CHECK (std::abs (static_cast<double> (peakIndex (out.left)) - c.expectedSeconds * sr) <= 1.0);
}

TEST_CASE ("StereoDelay: dual-linear mix law", "[synth][fx]")
{
    const auto mix = GENERATE (0.0f, 25.0f, 50.0f, 75.0f, 100.0f);
    CAPTURE (mix);
    StereoDelay delay;
    delay.prepare (48000.0);
    auto s = wetOnly (10.0f);
    s.mixPercent = mix;
    const auto out = run (delay, impulse (1000), 64, [&] (int) { return s; });
    const auto m = mix / 100.0f;
    CHECK_THAT (out.left[0], WithinAbs (std::min (1.0f, 2.0f * (1.0f - m)), 1e-6));
    CHECK_THAT (out.left[480], WithinAbs (std::min (1.0f, 2.0f * m), 1e-6));
}

TEST_CASE ("StereoDelay: feedback repeats through the filters, bounded at 95 %", "[synth][fx]")
{
    const double sr = 48000.0;

    SECTION ("each repeat is feedback x the previous one (1 kHz burst, filters and limiter ~ -0.5 dB)")
    {
        StereoDelay delay;
        delay.prepare (sr);
        st::Stereo in (static_cast<std::size_t> (0.5 * sr));

        for (std::size_t i = 0; i < 960; ++i)
            in.left[i] = in.right[i] = static_cast<float> (0.5 * std::sin (2.0 * std::numbers::pi * 1000.0 * static_cast<double> (i) / sr));

        const auto out = run (delay, in, 512, [] (int) { return wetOnly (100.0f, 50.0f); });
        const auto e1 = energy (out.left, 4800, 4800 + 960);
        const auto e2 = energy (out.left, 9600, 9600 + 960);
        const auto e3 = energy (out.left, 14400, 14400 + 960);
        const auto r1 = std::sqrt (e2 / e1), r2 = std::sqrt (e3 / e2);
        CHECK (r1 > 0.42);
        CHECK (r1 < 0.51);
        CHECK (r2 > 0.42);
        CHECK (r2 < 0.51);
    }

    SECTION ("95 % with loud noise for 10 s stays finite and bounded")
    {
        StereoDelay delay;
        delay.prepare (sr);
        auto s = wetOnly (37.0f, 95.0f);
        s.pingPong = GENERATE (false, true);
        const auto out = run (delay, noise (static_cast<std::size_t> (10.0 * sr), 3, 1.0f), 512, [&] (int) { return s; });
        const auto peak = std::max (st::peakAbs (std::span<const float> (out.left)), st::peakAbs (std::span<const float> (out.right)));
        CHECK (std::isfinite (peak));
        CHECK (peak < 3.0);
    }
}

TEST_CASE ("StereoDelay: ping-pong: mono sum into the left line, L -> R -> L; else L -> L, R -> R", "[synth][fx]")
{
    const double sr = 48000.0;
    const std::size_t d = 4800;

    SECTION ("ping-pong")
    {
        StereoDelay delay;
        delay.prepare (sr);
        auto s = wetOnly (100.0f, 50.0f);
        s.pingPong = true;
        const auto out = run (delay, impulse (5 * d, true, false), 64, [&] (int) { return s; });
        const auto near = [&] (const std::vector<float>& x, std::size_t at) { return energy (x, at - 20, at + 20); };
        CHECK (near (out.left, d) > 0.2);      // the mono sum (0.5) arrives left
        CHECK (near (out.right, d) < 1e-12);
        CHECK (near (out.right, 2 * d) > 0.01); // then right
        CHECK (near (out.left, 2 * d) < 1e-12);
        CHECK (near (out.left, 3 * d) > 0.001); // then left again
        CHECK (near (out.right, 3 * d) < 1e-12);
    }

    SECTION ("stereo: a left impulse never reaches the right channel")
    {
        StereoDelay delay;
        delay.prepare (sr);
        const auto out = run (delay, impulse (5 * d, true, false), 64, [] (int) { return wetOnly (100.0f, 50.0f); });
        CHECK (energy (out.left, d - 20, d + 20) > 0.5);
        CHECK (energy (out.right, 0, out.size()) == 0.0);
    }
}

TEST_CASE ("StereoDelay: a time change glides (tape-like), no jump in the output", "[synth][fx]")
{
    const double sr = 48000.0;
    const auto n = static_cast<std::size_t> (1.5 * sr);
    st::Stereo in (n);

    for (std::size_t i = 0; i < n; ++i)
        in.left[i] = in.right[i] = static_cast<float> (0.5 * std::sin (2.0 * std::numbers::pi * 440.0 * static_cast<double> (i) / sr));

    const auto from = GENERATE (100.0f, 300.0f);
    const auto to = from == 100.0f ? 300.0f : 100.0f;
    StereoDelay delay;
    delay.prepare (sr);
    const int switchTick = static_cast<int> (0.5 * sr) / controlTickSamples;
    const auto out = run (delay, in, 128, [&] (int t) { return wetOnly (t < switchTick ? from : to); });

    double steady = 0.0, during = 0.0;

    for (std::size_t i = static_cast<std::size_t> (0.35 * sr); i < static_cast<std::size_t> (0.5 * sr); ++i)
        steady = std::max (steady, static_cast<double> (std::abs (out.left[i] - out.left[i - 1])));

    for (std::size_t i = static_cast<std::size_t> (0.5 * sr); i < n; ++i)
        during = std::max (during, static_cast<double> (std::abs (out.left[i] - out.left[i - 1])));

    // Read speed between 0 (delay growing by 1 sample per sample) and 2 (shrinking): no step larger than that
    CHECK (during <= 2.05 * steady);
}

TEST_CASE ("StereoDelay: switching off fades over 10 ms, then bypassed with exactly the dry signal", "[synth][fx]")
{
    const double sr = 48000.0;
    StereoDelay delay;
    delay.prepare (sr);
    auto on = wetOnly (50.0f, 60.0f);
    auto off = on;
    off.on = false;
    const auto in = noise (static_cast<std::size_t> (0.3 * sr));
    const int switchTick = 400;
    const auto out = run (delay, in, 7, [&] (int t) { return t < switchTick ? on : off; });
    const auto fadeEnd = static_cast<std::size_t> ((switchTick + 31) * controlTickSamples);

    for (std::size_t i = fadeEnd; i < in.size(); ++i)
        REQUIRE (out.left[i] == in.left[i]);

    CHECK (delay.isBypassed());

    // On again: starts from cleared lines (no stale echo of the time before)
    auto quiet = st::Stereo (static_cast<std::size_t> (0.2 * sr));
    const auto again = run (delay, quiet, 64, [&] (int) { return on; });
    CHECK (energy (again.left, 0, again.size()) == 0.0);
}

TEST_CASE ("StereoDelay: the tail ends in exact 0 after the input stops", "[synth][fx]")
{
    const double sr = 48000.0;
    StereoDelay delay;
    delay.prepare (sr);
    auto s = wetOnly (50.0f, 35.0f);
    s.mixPercent = 50.0f;
    s.pingPong = GENERATE (false, true);
    auto in = noise (static_cast<std::size_t> (3.0 * sr));
    std::fill (in.left.begin() + static_cast<std::ptrdiff_t> (0.2 * sr), in.left.end(), 0.0f);
    std::fill (in.right.begin() + static_cast<std::ptrdiff_t> (0.2 * sr), in.right.end(), 0.0f);
    const auto out = run (delay, in, 512, [&] (int) { return s; });

    for (std::size_t i = static_cast<std::size_t> (2.0 * sr); i < out.size(); ++i)
    {
        REQUIRE (out.left[i] == 0.0f);
        REQUIRE (out.right[i] == 0.0f);
    }

    CHECK_FALSE (delay.isBypassed());

    // It wakes up at the first non-zero sample
    auto ping = impulse (static_cast<std::size_t> (0.1 * sr));
    const auto again = run (delay, ping, 1, [&] (int) { return s; });
    CHECK (again.left[0] == 1.0f);
    CHECK (energy (again.left, 2390, 2410) > 0.1);
}

TEST_CASE ("StereoDelay: a NaN input is cleared at the next tick", "[synth][fx]")
{
    StereoDelay delay;
    delay.prepare (48000.0);
    auto in = noise (48000);
    in.left[1000] = std::numeric_limits<float>::infinity();
    in.right[2000] = std::numeric_limits<float>::quiet_NaN();
    const auto out = run (delay, in, 64, [] (int) { return wetOnly (20.0f, 90.0f); });
    CHECK (delay.getNanResetCount() >= 1);

    for (std::size_t i = 4000; i < out.size(); ++i)
    {
        REQUIRE (std::isfinite (out.left[i]));
        REQUIRE (std::isfinite (out.right[i]));
    }
}

TEST_CASE ("StereoDelay: bit-identical for every host block split", "[synth][fx]")
{
    const auto sr = GENERATE (44100.0, 48000.0, 96000.0);
    CAPTURE (sr);
    auto in = noise (static_cast<std::size_t> (1.5 * sr), 5);
    std::fill (in.left.begin() + static_cast<std::ptrdiff_t> (0.9 * sr), in.left.end(), 0.0f); // tail end included
    std::fill (in.right.begin() + static_cast<std::ptrdiff_t> (0.9 * sr), in.right.end(), 0.0f);
    const auto settingsAt = [] (int t)
    {
        DelaySettings s;
        s.on = (t / 2000) % 4 != 3;
        s.timeMs = 5.0f + static_cast<float> ((t / 300) % 5) * 40.0f;
        s.sync = (t / 1500) % 2 == 1 ? TempoDivision::d1_32 : TempoDivision::off;
        s.feedbackPercent = 20.0f + static_cast<float> (t % 70);
        s.pingPong = (t / 900) % 2 == 0;
        s.mixPercent = static_cast<float> ((t / 7) % 101);
        return s;
    };
    const SynthTransport transport { 140.0, 3.0 };

    StereoDelay reference;
    reference.prepare (sr);
    const auto expected = run (reference, in, 4096, settingsAt, transport);

    for (const auto block : hostBlocks)
    {
        CAPTURE (block);
        StereoDelay delay;
        delay.prepare (sr);
        const auto out = run (delay, in, block, settingsAt, transport);
        CHECK (out.left == expected.left);
        CHECK (out.right == expected.right);
    }
}

TEST_CASE ("StereoDelay: benchmark", "[synth][fx][!benchmark]")
{
    StereoDelay delay;
    delay.prepare (48000.0);
    auto s = wetOnly (375.0f, 35.0f);
    s.mixPercent = 25.0f;
    auto in = noise (4096);

    BENCHMARK ("delay 4096 samples (budget 10 ns / sample = 41 us)")
    {
        run (delay, in, 128, [&] (int) { return s; });
        return delay.isBypassed();
    };
}
