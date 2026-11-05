// SvfFilter: -3 dB at fc, slopes, BP / HP / notch shape, stability at max resonance + drive + cutoff jumps, NaN reset.
// Test plan: docs/SYNTH-DESIGN.md 21. Owner: agent B (Voice).

#include "vocalsynth/synth/SvfFilter.h"

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <vector>

using namespace vocalsynth::synth;
using Catch::Matchers::WithinAbs;

namespace
{

constexpr int tick = controlTickSamples;

/** Runs the filter with constant targets over a stereo copy of the input (beginTick every 16 samples). */
std::vector<float> runFilter (SvfFilter& f, const FilterTick& target, const std::vector<float>& input)
{
    std::vector<float> left = input, right = input;

    for (std::size_t pos = 0; pos < input.size(); pos += tick)
    {
        const auto n = static_cast<int> (std::min<std::size_t> (tick, input.size() - pos));
        f.beginTick (target, tick);
        f.process (left.data() + pos, right.data() + pos, n);
    }

    REQUIRE (left == right);
    return left;
}

std::vector<float> sine (double freq, double sampleRate, int numSamples, double amplitude = 0.5)
{
    std::vector<float> s (static_cast<std::size_t> (numSamples));

    for (int i = 0; i < numSamples; ++i)
        s[static_cast<std::size_t> (i)] = static_cast<float> (amplitude * std::sin (2.0 * std::numbers::pi * freq * i / sampleRate));

    return s;
}

/** Amplitude of the component at freq (Hann-windowed correlation) over [from, end). */
double amplitudeAt (const std::vector<float>& x, double freq, double sampleRate, std::size_t from)
{
    double re = 0.0, im = 0.0, wsum = 0.0;
    const auto n = x.size() - from;

    for (std::size_t i = 0; i < n; ++i)
    {
        const auto w = 0.5 - 0.5 * std::cos (2.0 * std::numbers::pi * static_cast<double> (i) / static_cast<double> (n));
        const auto ph = 2.0 * std::numbers::pi * freq * static_cast<double> (from + i) / sampleRate;
        re += w * x[from + i] * std::cos (ph);
        im += w * x[from + i] * std::sin (ph);
        wsum += w;
    }

    return 2.0 * std::sqrt (re * re + im * im) / wsum;
}

/** Gain in dB of the filter at freq (steady state). */
double gainDb (FilterMode mode, double cutoff, double freq, double sampleRate, float resonance = 0.0f)
{
    SvfFilter f;
    f.prepare (sampleRate);
    const auto settle = static_cast<int> (std::max (0.2 * sampleRate, 40.0 * sampleRate / std::min (freq, cutoff)));
    const auto measure = static_cast<int> (std::max (0.2 * sampleRate, 40.0 * sampleRate / freq));
    const auto in = sine (freq, sampleRate, settle + measure);
    const auto out = runFilter (f, { mode, static_cast<float> (cutoff), resonance, 0.0f }, in);
    const auto a = amplitudeAt (out, freq, sampleRate, static_cast<std::size_t> (settle));
    const auto b = amplitudeAt (in, freq, sampleRate, static_cast<std::size_t> (settle));
    return 20.0 * std::log10 (std::max (a, 1e-30) / b);
}

uint32_t xorshift (uint32_t& s)
{
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

float noiseSample (uint32_t& s)
{
    return static_cast<float> (static_cast<double> (xorshift (s)) / 4294967295.0 * 2.0 - 1.0);
}

} // namespace

TEST_CASE ("SvfFilter: LP12 and LP24 are -3 dB at the cutoff", "[synth][filter]")
{
    const auto rate = GENERATE (44100.0, 48000.0, 96000.0);
    const auto cutoff = GENERATE (100.0, 1000.0, 10000.0);
    const auto mode = GENERATE (FilterMode::lp12, FilterMode::lp24);
    CAPTURE (rate, cutoff, static_cast<int> (mode));

    CHECK_THAT (gainDb (mode, cutoff, cutoff, rate), WithinAbs (-3.0103, 0.5));
    // Pass band flat (Butterworth)
    CHECK_THAT (gainDb (mode, cutoff, cutoff / 8.0, rate), WithinAbs (0.0, 0.1));
}

TEST_CASE ("SvfFilter: slopes are 12 and 24 dB per octave", "[synth][filter]")
{
    const auto rate = GENERATE (44100.0, 48000.0, 96000.0);
    const auto cutoff = GENERATE (100.0, 1000.0);
    CAPTURE (rate, cutoff);

    // Octaves of the prewarped (analog) frequency: the TPT filter is the bilinear image of the analog one
    const auto warpedOctaves = [rate] (double f1, double f2)
    { return std::log2 (std::tan (std::numbers::pi * f2 / rate) / std::tan (std::numbers::pi * f1 / rate)); };

    const auto up = warpedOctaves (8.0 * cutoff, 16.0 * cutoff);
    const auto lp12 = (gainDb (FilterMode::lp12, cutoff, 8.0 * cutoff, rate) - gainDb (FilterMode::lp12, cutoff, 16.0 * cutoff, rate)) / up;
    const auto lp24 = (gainDb (FilterMode::lp24, cutoff, 8.0 * cutoff, rate) - gainDb (FilterMode::lp24, cutoff, 16.0 * cutoff, rate)) / up;
    CHECK_THAT (lp12, WithinAbs (12.0, 1.0));
    CHECK_THAT (lp24, WithinAbs (24.0, 1.0));
    const auto down = warpedOctaves (cutoff / 16.0, cutoff / 8.0);
    const auto hp = (gainDb (FilterMode::hp, cutoff, cutoff / 8.0, rate) - gainDb (FilterMode::hp, cutoff, cutoff / 16.0, rate)) / down;
    CHECK_THAT (hp, WithinAbs (12.0, 1.0));
}

TEST_CASE ("SvfFilter: band pass, high pass and notch shapes", "[synth][filter]")
{
    const auto rate = GENERATE (44100.0, 48000.0, 96000.0);
    const auto cutoff = GENERATE (100.0, 1000.0, 5000.0);
    CAPTURE (rate, cutoff);

    // BP: unity peak at fc, falling on both sides
    CHECK_THAT (gainDb (FilterMode::bp, cutoff, cutoff, rate), WithinAbs (0.0, 0.5));
    CHECK (gainDb (FilterMode::bp, cutoff, cutoff / 8.0, rate) < -12.0);
    CHECK (gainDb (FilterMode::bp, cutoff, std::min (8.0 * cutoff, 0.4 * rate), rate) < -9.0);

    // HP: -3 dB at fc, flat above, steep below
    CHECK_THAT (gainDb (FilterMode::hp, cutoff, cutoff, rate), WithinAbs (-3.0103, 0.5));
    CHECK_THAT (gainDb (FilterMode::hp, cutoff, std::min (8.0 * cutoff, 0.3 * rate), rate), WithinAbs (0.0, 0.5));
    CHECK (gainDb (FilterMode::hp, cutoff, cutoff / 8.0, rate) < -30.0);

    // Notch: deep at fc, flat away from it
    CHECK (gainDb (FilterMode::notch, cutoff, cutoff, rate) < -40.0);
    CHECK_THAT (gainDb (FilterMode::notch, cutoff, cutoff / 8.0, rate), WithinAbs (0.0, 0.5));
    CHECK_THAT (gainDb (FilterMode::notch, cutoff, std::min (8.0 * cutoff, 0.3 * rate), rate), WithinAbs (0.0, 0.7));
}

TEST_CASE ("SvfFilter: resonance raises the peak to Q", "[synth][filter]")
{
    const auto rate = 48000.0;
    // LP12 at fc: |H| = Q; Q = 0.7071 (r = 0) .. 25 (r = 1)
    CHECK_THAT (gainDb (FilterMode::lp12, 1000.0, 1000.0, rate, 1.0f), WithinAbs (20.0 * std::log10 (25.0), 0.5));
    CHECK_THAT (gainDb (FilterMode::lp12, 1000.0, 1000.0, rate, 0.5f),
                WithinAbs (20.0 * std::log10 (0.70710678 * std::sqrt (25.0 / 0.70710678)), 0.5));
    // LP24: stage 1 Q 0.5412 at fc gives |H1| = 0.5412, stage 2 Q up to 25
    CHECK_THAT (gainDb (FilterMode::lp24, 1000.0, 1000.0, rate, 1.0f), WithinAbs (20.0 * std::log10 (0.5412 * 25.0), 0.5));
    // BP keeps a unity peak at any resonance
    CHECK_THAT (gainDb (FilterMode::bp, 1000.0, 1000.0, rate, 1.0f), WithinAbs (0.0, 0.5));
}

TEST_CASE ("SvfFilter: drive 0 dB is exactly linear, drive > 0 saturates and stays bounded", "[synth][filter]")
{
    const auto in = sine (220.0, 48000.0, 4800, 0.4);
    std::vector<float> twice (in.size());
    std::transform (in.begin(), in.end(), twice.begin(), [] (float x) { return 2.0f * x; });

    for (const auto mode : { FilterMode::lp12, FilterMode::lp24, FilterMode::bp, FilterMode::hp, FilterMode::notch })
    {
        SvfFilter a, b;
        a.prepare (48000.0);
        b.prepare (48000.0);
        const FilterTick target { mode, 2000.0f, 0.3f, 0.0f };
        const auto ya = runFilter (a, target, in);
        const auto yb = runFilter (b, target, twice);

        for (std::size_t i = 0; i < ya.size(); ++i)
            REQUIRE (yb[i] == 2.0f * ya[i]); // exact homogeneity: no saturation stage at all
    }

    SvfFilter d;
    d.prepare (48000.0);
    const auto loud = sine (220.0, 48000.0, 9600, 4.0);
    const auto y = runFilter (d, { FilterMode::lp12, 20000.0f, 0.0f, 24.0f }, loud);
    float peak = 0.0f;

    for (auto v : y)
        peak = std::max (peak, std::abs (v));

    // s (G x) / sqrt (G) <= 1 / sqrt (G) = 0.251 at 24 dB (a clipped sine; the LP12 overshoots it slightly)
    CHECK (peak < 0.3f);
    CHECK (peak > 0.2f);
}

TEST_CASE ("SvfFilter: drive switched on and off fades over one tick", "[synth][filter]")
{
    SvfFilter f;
    f.prepare (48000.0);
    std::vector<float> l (tick * 40, 0.5f), r (tick * 40, 0.5f);
    float maxStep = 0.0f;
    float previous = 0.0f;

    for (int t = 0; t < 40; ++t)
    {
        const float drive = (t / 10) % 2 == 1 ? 12.0f : 0.0f;
        f.beginTick ({ FilterMode::lp12, 20000.0f, 0.0f, drive }, tick);
        f.process (l.data() + t * tick, r.data() + t * tick, tick);

        for (int i = 0; i < tick; ++i)
        {
            const auto v = l[static_cast<std::size_t> (t * tick + i)];

            if (t > 2)
                maxStep = std::max (maxStep, std::abs (v - previous));

            previous = v;
        }
    }

    // DC 0.5 -> saturated about 0.25: the change is spread over 16 samples (no step of the whole 0.25)
    CHECK (maxStep < 0.03f);
}

TEST_CASE ("SvfFilter: stable at max resonance + drive with cutoff jumps every tick for 10 s", "[synth][filter]")
{
    const auto mode = GENERATE (FilterMode::lp12, FilterMode::lp24, FilterMode::bp, FilterMode::hp, FilterMode::notch);
    const auto rate = GENERATE (44100.0, 96000.0);
    CAPTURE (static_cast<int> (mode), rate);

    SvfFilter f;
    f.prepare (rate);
    uint32_t seed = 12345;
    const auto total = static_cast<int> (10.0 * rate);
    std::array<float, tick> l {}, r {};
    float peak = 0.0f;
    bool finite = true;

    for (int pos = 0, t = 0; pos < total; pos += tick, ++t)
    {
        const float cutoff = (t % 2 == 0) ? 20.0f : 20000.0f;
        f.beginTick ({ mode, cutoff, 1.0f, 24.0f }, tick);

        for (int i = 0; i < tick; ++i)
        {
            l[static_cast<std::size_t> (i)] = noiseSample (seed);
            r[static_cast<std::size_t> (i)] = (i % 8) < 4 ? 1.0f : -1.0f;
        }

        f.process (l.data(), r.data(), tick);

        for (int i = 0; i < tick; ++i)
        {
            finite = finite && std::isfinite (l[static_cast<std::size_t> (i)]) && std::isfinite (r[static_cast<std::size_t> (i)]);
            peak = std::max ({ peak, std::abs (l[static_cast<std::size_t> (i)]), std::abs (r[static_cast<std::size_t> (i)]) });
        }
    }

    CHECK (finite);
    CHECK (peak < 50.0f); // input <= 1 / sqrt (G) after the drive; Q 25 at most -> well below this bound
    CHECK (f.getNanResetCount() == 0);
}

TEST_CASE ("SvfFilter: random cutoff / resonance / mode modulation stays finite and bounded", "[synth][filter]")
{
    SvfFilter f;
    f.prepare (48000.0);
    uint32_t seed = 777;
    std::array<float, tick> l {}, r {};
    float peak = 0.0f;

    for (int t = 0; t < 30000; ++t)
    {
        FilterTick target;
        target.mode = static_cast<FilterMode> (xorshift (seed) % numFilterModes);
        target.cutoffHz = 20.0f * std::pow (1000.0f, noiseSample (seed) * 0.5f + 0.5f);
        target.resonance = noiseSample (seed) * 0.5f + 0.5f;
        target.driveDb = std::max (0.0f, noiseSample (seed) * 24.0f);
        f.beginTick (target, 1 + static_cast<int> (xorshift (seed) % tick));

        for (auto& v : l)
            v = noiseSample (seed);

        r = l;
        f.process (l.data(), r.data(), tick);

        for (auto v : l)
            peak = std::max (peak, std::abs (v));
    }

    CHECK (std::isfinite (peak));
    CHECK (peak < 50.0f);
}

TEST_CASE ("SvfFilter: NaN input resets the states at the next tick and counts", "[synth][filter]")
{
    SvfFilter f;
    f.prepare (48000.0);
    std::array<float, tick> l {}, r {};
    l.fill (0.1f);
    r.fill (0.1f);
    f.beginTick ({ FilterMode::lp24, 1000.0f, 0.5f, 0.0f }, tick);
    f.process (l.data(), r.data(), tick);
    CHECK (f.getNanResetCount() == 0);

    l.fill (0.1f);
    r.fill (0.1f);
    l[3] = std::numeric_limits<float>::quiet_NaN();
    r[5] = std::numeric_limits<float>::infinity();
    f.process (l.data(), r.data(), tick);
    CHECK (! std::isfinite (l[15]));

    f.beginTick ({ FilterMode::lp24, 1000.0f, 0.5f, 0.0f }, tick);
    CHECK (f.getNanResetCount() == 1);
    l.fill (0.1f);
    r.fill (0.1f);
    f.process (l.data(), r.data(), tick);

    for (int i = 0; i < tick; ++i)
    {
        CHECK (std::isfinite (l[static_cast<std::size_t> (i)]));
        CHECK (std::isfinite (r[static_cast<std::size_t> (i)]));
    }

    // NaN targets do not break the filter either
    f.beginTick ({ FilterMode::lp12, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::quiet_NaN(),
                   std::numeric_limits<float>::quiet_NaN() },
                 tick);
    f.process (l.data(), r.data(), tick);
    CHECK (std::isfinite (l[15]));
    CHECK (f.getNanResetCount() == 1);
}

TEST_CASE ("SvfFilter: states decay to exactly 0 in silence (denormal flush at ticks)", "[synth][filter]")
{
    SvfFilter f;
    f.prepare (48000.0);
    std::array<float, tick> l {}, r {};
    l.fill (1.0f);
    r.fill (-1.0f);
    f.beginTick ({ FilterMode::lp24, 200.0f, 0.9f, 6.0f }, tick);
    f.process (l.data(), r.data(), tick);

    bool zero = false;

    for (int t = 0; t < 48000 && ! zero; ++t)
    {
        l.fill (0.0f);
        r.fill (0.0f);
        f.beginTick ({ FilterMode::lp24, 200.0f, 0.9f, 6.0f }, tick);
        f.process (l.data(), r.data(), tick);
        zero = std::all_of (l.begin(), l.end(), [] (float v) { return v == 0.0f; })
               && std::all_of (r.begin(), r.end(), [] (float v) { return v == 0.0f; }) && t > 100;
    }

    CHECK (zero);
}

TEST_CASE ("SvfFilter: any split of a tick into process() calls is bit-identical", "[synth][filter]")
{
    const auto in = sine (330.0, 48000.0, 16 * 600, 0.7);
    std::vector<std::vector<float>> results;

    for (const int split : { 16, 1, 7, 5, 3 })
    {
        SvfFilter f;
        f.prepare (48000.0);
        std::vector<float> l = in, r = in;

        for (std::size_t pos = 0, t = 0; pos < in.size(); pos += tick, ++t)
        {
            // The same targets for every split (a mid-tick re-target after 5 samples every 3rd tick)
            const auto mode = static_cast<FilterMode> ((t / 50) % numFilterModes);
            const auto cutoff = 200.0f + 5000.0f * static_cast<float> ((t * 37) % 100) / 100.0f;
            const float drive = (t / 25) % 2 == 0 ? 0.0f : 9.0f;
            f.beginTick ({ mode, cutoff, 0.6f, drive }, tick);
            int done = 0;

            const auto runTo = [&] (int end)
            {
                while (done < end)
                {
                    const auto n = std::min (split, end - done);
                    f.process (l.data() + pos + static_cast<std::size_t> (done), r.data() + pos + static_cast<std::size_t> (done), n);
                    done += n;
                }
            };

            if (t % 3 == 0)
            {
                runTo (5);
                f.beginTick ({ mode, cutoff * 0.5f, 0.2f, drive }, tick - 5);
            }

            runTo (tick);
        }

        results.push_back (l);
    }

    for (std::size_t i = 1; i < results.size(); ++i)
        CHECK (results[i] == results[0]);
}

TEST_CASE ("SvfFilter: benchmark LP24 stereo", "[synth][filter][!benchmark]")
{
    SvfFilter f;
    f.prepare (48000.0);
    std::vector<float> l (4096), r (4096);
    uint32_t seed = 5;

    for (std::size_t i = 0; i < l.size(); ++i)
        l[i] = r[i] = noiseSample (seed) * 0.5f;

    float cutoff = 500.0f;

    // Modulated cutoff (ramp active in every tick): the expensive path
    BENCHMARK ("LP24 stereo, ramping, 4096 samples")
    {
        for (int pos = 0; pos < 4096; pos += tick)
        {
            cutoff = cutoff > 8000.0f ? 500.0f : cutoff * 1.01f;
            f.beginTick ({ FilterMode::lp24, cutoff, 0.5f, 0.0f }, tick);
            f.process (l.data() + pos, r.data() + pos, tick);
        }

        return l[100];
    };

    BENCHMARK ("LP24 stereo, ramping + drive, 4096 samples")
    {
        for (int pos = 0; pos < 4096; pos += tick)
        {
            cutoff = cutoff > 8000.0f ? 500.0f : cutoff * 1.01f;
            f.beginTick ({ FilterMode::lp24, cutoff, 0.5f, 6.0f }, tick);
            f.process (l.data() + pos, r.data() + pos, tick);
        }

        return l[100];
    };

    BENCHMARK ("LP24 stereo, constant, 4096 samples")
    {
        for (int pos = 0; pos < 4096; pos += tick)
        {
            f.beginTick ({ FilterMode::lp24, 1000.0f, 0.5f, 0.0f }, tick);
            f.process (l.data() + pos, r.data() + pos, tick);
        }

        return l[100];
    };
}
