// Envelope: segment times exact to +-1 sample for any split, closed forms, time changes mid-segment, retrigger.
// Test plan: docs/SYNTH-DESIGN.md 21. Owner: agent B (Voice).

#include "vocalsynth/synth/Envelope.h"

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace vocalsynth::synth;
using Catch::Matchers::WithinAbs;

namespace
{

constexpr double ln10000 = 9.210340371976184;

/** Advances the envelope by exactly total samples: a first chunk of `first`, then chunks of `step`. */
void advanceBy (Envelope& env, const EnvelopeSettings& s, int total, int first, int step)
{
    int done = 0;
    int chunk = first > 0 ? first : step;

    while (done < total)
    {
        const auto n = std::min (chunk, total - done);
        env.advance (s, n);
        done += n;
        chunk = step;
    }
}

int samplesOf (double ms, double rate)
{
    return static_cast<int> (std::ceil (ms * 0.001 * rate - 1e-6));
}

} // namespace

TEST_CASE ("Envelope: idle at 0 after prepare / reset, note-off on idle is ignored", "[synth][envelope]")
{
    Envelope env;
    env.prepare (48000.0);
    CHECK_FALSE (env.isActive());
    CHECK (env.advance ({}, 100) == 0.0f);
    env.noteOff (0.5f);
    CHECK_FALSE (env.isActive());
    CHECK (env.getValue() == 0.0f);

    env.noteOn (0.0f);
    env.advance ({}, 1000);
    CHECK (env.isActive());
    env.reset();
    CHECK_FALSE (env.isActive());
    CHECK (env.getValue() == 0.0f);
}

TEST_CASE ("Envelope: segment times exact to the sample for any split and offset", "[synth][envelope]")
{
    const auto rate = GENERATE (44100.0, 48000.0, 96000.0);
    const EnvelopeSettings s { 7.3f, 21.9f, 40.0f, 13.1f };
    const auto attack = samplesOf (s.attackMs, rate);
    const auto decay = samplesOf (s.decayMs, rate);
    const auto release = samplesOf (s.releaseMs, rate);

    for (int step = 1; step <= controlTickSamples; ++step)
    {
        for (int first = 0; first < controlTickSamples; first += 3)
        {
            CAPTURE (rate, step, first);
            Envelope env;
            env.prepare (rate);

            // Attack ends exactly at sample `attack`
            env.noteOn (0.0f);
            advanceBy (env, s, attack - 1, first, step);
            CHECK (env.getStage() == Envelope::Stage::attack);
            CHECK (env.getValue() < 1.0f);
            env.advance (s, 1);
            CHECK (env.getStage() == Envelope::Stage::decay);
            CHECK (env.getValue() == 1.0f);

            // Decay ends exactly at `decay` samples later, snapping to the sustain level
            advanceBy (env, s, decay - 1, first, step);
            CHECK (env.getStage() == Envelope::Stage::decay);
            env.advance (s, 1);
            CHECK (env.getStage() == Envelope::Stage::sustain);
            CHECK (env.getValue() == 0.4f);

            advanceBy (env, s, 1000, first, step);
            CHECK (env.getValue() == 0.4f);

            // Release ends exactly at `release`
            env.noteOff (env.getValue());
            advanceBy (env, s, release - 1, first, step);
            CHECK (env.getStage() == Envelope::Stage::release);
            CHECK (env.getValue() > 0.0f);
            env.advance (s, 1);
            CHECK_FALSE (env.isActive());
            CHECK (env.getValue() == 0.0f);
        }
    }
}

TEST_CASE ("Envelope: a segment end inside an advance continues in the next segment", "[synth][envelope]")
{
    // One advance across attack -> decay -> sustain equals the chained closed forms
    Envelope env;
    env.prepare (48000.0);
    const EnvelopeSettings s { 1.0f, 1.0f, 50.0f, 1.0f }; // 48 samples each
    env.noteOn (0.0f);
    const auto v = env.advance (s, 48 + 10);
    CHECK (env.getStage() == Envelope::Stage::decay);
    CHECK_THAT (v, WithinAbs (0.5 + 0.5 * std::exp (-10.0 * ln10000 / 48.0), 1e-6));
    env.advance (s, 38 + 5);
    CHECK (env.getStage() == Envelope::Stage::sustain);
    CHECK (env.getValue() == 0.5f);
}

TEST_CASE ("Envelope: constant settings match the closed forms within 1e-5", "[synth][envelope]")
{
    const auto rate = GENERATE (44100.0, 96000.0);
    const auto start = GENERATE (0.0f, 0.3f, 0.9f);
    const EnvelopeSettings s { 2000.0f, 10000.0f, 25.0f, 10000.0f };
    CAPTURE (rate, start);

    Envelope env;
    env.prepare (rate);
    env.noteOn (start);

    const auto a = s.attackMs * 0.001 * rate;
    const auto tau = a / std::log ((1.5 - start) / 0.5);
    double maxError = 0.0;
    int t = 0;

    while (env.getStage() == Envelope::Stage::attack)
    {
        const auto v = env.advance (s, controlTickSamples);
        t += controlTickSamples;

        if (env.getStage() == Envelope::Stage::attack)
            maxError = std::max (maxError, std::abs (v - (1.5 - (1.5 - start) * std::exp (-t / tau))));
    }

    CHECK (maxError < 1e-5);
    CHECK (std::abs (t - samplesOf (s.attackMs, rate)) < controlTickSamples);

    // Decay from an exact start at a tick boundary: restart the check from the decay start
    Envelope d;
    d.prepare (rate);
    d.noteOn (1.0f); // attack done at once
    d.advance (s, 0);
    REQUIRE (d.getStage() == Envelope::Stage::decay);
    const auto dl = s.decayMs * 0.001 * rate;
    maxError = 0.0;
    t = 0;

    while (d.getStage() == Envelope::Stage::decay)
    {
        const auto v = d.advance (s, controlTickSamples);
        t += controlTickSamples;

        if (d.getStage() == Envelope::Stage::decay)
            maxError = std::max (maxError, std::abs (v - (0.25 + 0.75 * std::exp (-t * ln10000 / dl))));
    }

    CHECK (maxError < 1e-5);

    // Release from the sustain level
    d.noteOff (d.getValue());
    const auto rl = s.releaseMs * 0.001 * rate;
    maxError = 0.0;
    t = 0;

    while (d.isActive())
    {
        const auto v = d.advance (s, controlTickSamples);
        t += controlTickSamples;

        if (d.isActive())
            maxError = std::max (maxError, std::abs (v - 0.25 * std::exp (-t * ln10000 / rl)));
    }

    CHECK (maxError < 1e-5);
}

TEST_CASE ("Envelope: decay / release time changed during the segment changes the value continuously", "[synth][envelope]")
{
    const auto lengthen = GENERATE (true, false);
    CAPTURE (lengthen);

    for (const bool release : { false, true })
    {
        Envelope env;
        env.prepare (48000.0);
        EnvelopeSettings s { 0.0f, 500.0f, 0.0f, 500.0f };
        env.noteOn (1.0f);
        env.advance (s, 0);

        if (release)
            env.noteOff (1.0f);

        // 100 ticks at 500 ms, then the time is ramped to 2000 / 50 ms over 50 ticks (smoothing), then held
        float previous = env.advance (s, controlTickSamples);
        float worst = 0.0f;
        const float target = lengthen ? 2000.0f : 50.0f;

        for (int t = 1; t < 400 && env.isActive(); ++t)
        {
            const float oldTime = release ? s.releaseMs : s.decayMs;

            if (t > 100 && t <= 150)
            {
                const float time = 500.0f + (target - 500.0f) * static_cast<float> (t - 100) / 50.0f;
                (release ? s.releaseMs : s.decayMs) = time;
            }

            const auto v = env.advance (s, controlTickSamples);

            if (! env.isActive() || (! release && env.getStage() == Envelope::Stage::sustain))
                break;

            // The step this tick may not exceed the step that the faster of the two times would take from
            // the current distance (a closed form in "time since start" would jump here)
            const auto fastest = std::min (oldTime, release ? s.releaseMs : s.decayMs);
            const auto maxStep = previous * (1.0 - std::exp (-controlTickSamples * ln10000 / (fastest * 48.0)));
            worst = std::max (worst, static_cast<float> (std::abs (previous - v) - maxStep));
            previous = v;
        }

        CHECK (worst <= 1e-6f);
    }
}

TEST_CASE ("Envelope: retrigger and release from mid-segment start from the given value", "[synth][envelope]")
{
    Envelope env;
    env.prepare (48000.0);
    const EnvelopeSettings s { 20.0f, 100.0f, 60.0f, 300.0f };

    // Release from mid-attack
    env.noteOn (0.0f);
    env.advance (s, 400);
    const auto midAttack = env.getValue();
    REQUIRE (midAttack > 0.2f);
    REQUIRE (midAttack < 1.0f);
    env.noteOff (midAttack);
    CHECK (env.getValue() == midAttack);
    const auto r1 = env.advance (s, 1);
    CHECK (r1 < midAttack);
    CHECK (midAttack - r1 < 0.001f);

    // Retrigger from mid-release: the attack continues from the current value, rising, no jump
    env.advance (s, 2000);
    const auto midRelease = env.getValue();
    env.noteOn (midRelease);
    CHECK (env.getValue() == midRelease);
    const auto a1 = env.advance (s, 1);
    CHECK (a1 > midRelease);
    CHECK (a1 - midRelease < 0.01f);

    // The attack from a non-zero start still ends exactly at A with value 1
    Envelope e2;
    e2.prepare (48000.0);
    e2.noteOn (0.5f);
    e2.advance (s, 959);
    CHECK (e2.getStage() == Envelope::Stage::attack);
    e2.advance (s, 1);
    CHECK (e2.getStage() == Envelope::Stage::decay);
    CHECK (e2.getValue() == 1.0f);

    // Sample-to-sample steps of a retriggered envelope stay small (click-free)
    Envelope e3;
    e3.prepare (48000.0);
    e3.noteOn (0.0f);
    float prev = 0.0f, maxStep = 0.0f;

    for (int i = 0; i < 20000; ++i)
    {
        if (i == 3000 || i == 7000)
            e3.noteOff (e3.getValue());

        if (i == 5000 || i == 9000)
            e3.noteOn (e3.getValue());

        const auto v = e3.advance (s, 1);
        maxStep = std::max (maxStep, std::abs (v - prev));
        prev = v;
    }

    // Steepest part: the start of a 20 ms attack, 1.5 * (1 - exp (-1 / tau)) ~ 1.5 / (960 / ln 3) per sample
    CHECK (maxStep < 0.002f);
}

TEST_CASE ("Envelope: zero times jump", "[synth][envelope]")
{
    Envelope env;
    env.prepare (48000.0);
    const EnvelopeSettings s { 0.0f, 0.0f, 70.0f, 0.0f };
    env.noteOn (0.0f);
    CHECK (env.advance (s, 0) == 0.7f);
    CHECK (env.getStage() == Envelope::Stage::sustain);
    env.noteOff (env.getValue());
    CHECK (env.advance (s, 0) == 0.0f);
    CHECK_FALSE (env.isActive());

    // A = 0, D > 0: the decay starts at 1
    const EnvelopeSettings s2 { 0.0f, 10.0f, 0.0f, 10.0f };
    env.noteOn (0.0f);
    CHECK (env.advance (s2, 0) == 1.0f);
    CHECK (env.getStage() == Envelope::Stage::decay);

    // Sustain 0, decay finished: a release from 0 ends at once
    env.advance (s2, 1000);
    CHECK (env.getStage() == Envelope::Stage::sustain);
    CHECK (env.getValue() == 0.0f);
    env.noteOff (env.getValue());
    CHECK_FALSE (env.isActive());
}

TEST_CASE ("Envelope: sustain follows the setting, NaN settings are harmless", "[synth][envelope]")
{
    Envelope env;
    env.prepare (48000.0);
    EnvelopeSettings s { 0.0f, 0.0f, 50.0f, 100.0f };
    env.noteOn (0.0f);
    CHECK (env.advance (s, 16) == 0.5f);
    s.sustainPercent = 80.0f;
    CHECK (env.advance (s, 16) == 0.8f);

    s.releaseMs = std::nanf ("");
    s.sustainPercent = std::nanf ("");
    const auto v = env.advance (s, 16);
    CHECK (std::isfinite (v));
    env.noteOff (v);
    env.advance (s, 16);
    CHECK_FALSE (env.isActive()); // NaN time = 0
}

TEST_CASE ("Envelope: benchmark per tick", "[synth][envelope][!benchmark]")
{
    Envelope env;
    env.prepare (48000.0);
    const EnvelopeSettings s { 5.0f, 300.0f, 50.0f, 200.0f };

    BENCHMARK ("3 envelopes x 1000 ticks (cached factors)")
    {
        float sum = 0.0f;

        for (int t = 0; t < 1000; ++t)
        {
            if (t % 200 == 0)
                env.noteOn (env.getValue());

            for (int e = 0; e < 3; ++e)
                sum += env.advance (s, controlTickSamples);
        }

        return sum;
    };
}
