#include "vocalsynth/synth/Envelope.h"

#include <algorithm>
#include <cmath>

namespace vocalsynth::synth
{

// Incremental exponential ADSR (docs/SYNTH-DESIGN.md 8.1, formulas in the header). The value is carried in
// double precision: a 10 s segment is about 30 000 multiplications by the per-tick factor, which in float
// would drift by more than the 1e-5 the closed-form test allows.

namespace
{

constexpr double attackTarget = 1.5;
constexpr double ln10000 = 9.210340371976184; // decay / release reach -80 dB of their distance

double toUnit (float percent) noexcept
{
    return std::isfinite (percent) ? std::clamp (static_cast<double> (percent) * 0.01, 0.0, 1.0) : 0.0;
}

} // namespace

void Envelope::prepare (double newSampleRate) noexcept
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    reset();
}

void Envelope::reset() noexcept
{
    stage = Stage::idle;
    value = 0.0f;
    segmentStart = 0.0f;
    segmentTime = 0.0;
    level = 0.0;
    cachedTimeMs = -1.0f;
}

void Envelope::noteOn (float startValue) noexcept
{
    const auto v = std::isfinite (startValue) ? std::clamp (startValue, 0.0f, 1.0f) : 0.0f;
    stage = Stage::attack;
    segmentStart = v;
    segmentTime = 0.0;
    level = v;
    value = v;
    cachedTimeMs = -1.0f;
}

void Envelope::noteOff (float startValue) noexcept
{
    if (stage == Stage::idle)
        return;

    const auto v = std::isfinite (startValue) ? std::clamp (startValue, 0.0f, 1.0f) : 0.0f;
    segmentStart = v;
    segmentTime = 0.0;
    level = v;
    value = v;
    cachedTimeMs = -1.0f;
    stage = v > 0.0f ? Stage::release : Stage::idle;
}

double Envelope::decayFactor (double rate, float timeMs, int n) noexcept
{
    if (n == controlTickSamples)
    {
        if (cachedTimeMs != timeMs)
        {
            cachedFactor = std::exp (-static_cast<double> (n) * rate);
            cachedTimeMs = timeMs;
        }

        return cachedFactor;
    }

    return std::exp (-static_cast<double> (n) * rate);
}

float Envelope::advance (const EnvelopeSettings& settings, int numSamples) noexcept
{
    // A segment ends where its value reaches the end point (attack: 1; decay / release: -80 dB of the
    // distance the segment started with), computed from the current state and the CURRENT time. With
    // constant settings that is exactly the sample ceil (time) (the test checks it for every split); when a
    // time is changed mid-segment the value never jumps (a "t >= time" rule would snap early or overshoot).
    auto n = std::max (0, numSamples);
    const auto sustain = toUnit (settings.sustainPercent);
    constexpr double tolerance = 1.0 + 1.0e-9;

    const auto samplesOf = [this] (float ms)
    { return std::isfinite (ms) && ms > 0.0f ? static_cast<double> (ms) * 0.001 * sampleRate : 0.0; };

    const auto finishSegment = [this, sustain]
    {
        const auto next = stage == Stage::attack ? Stage::decay : (stage == Stage::decay ? Stage::sustain : Stage::idle);
        stage = next;
        level = next == Stage::decay ? 1.0 : (next == Stage::sustain ? sustain : 0.0);
        segmentStart = static_cast<float> (level);
        segmentTime = 0.0;
        cachedTimeMs = -1.0f;
    };

    for (;;)
    {
        if (stage == Stage::idle)
        {
            level = 0.0;
            break;
        }

        if (stage == Stage::sustain)
        {
            level = sustain;
            break;
        }

        const auto timeMs = stage == Stage::attack ? settings.attackMs
                                                   : (stage == Stage::decay ? settings.decayMs : settings.releaseMs);
        const auto time = samplesOf (timeMs);
        const auto start = static_cast<double> (segmentStart);

        if (time <= 0.0 || (stage == Stage::attack && start >= 1.0))
        {
            finishSegment(); // time 0 = jump
            continue;
        }

        // Distance to the asymptote, the distance at which the segment ends, and the rate per sample
        const auto target = stage == Stage::attack ? attackTarget : (stage == Stage::decay ? sustain : 0.0);
        const auto distance = std::abs (level - target);
        const auto endDistance = stage == Stage::attack ? attackTarget - 1.0 : std::max (std::abs (start - target) * 1.0e-4, 1.0e-8);
        const auto rate = stage == Stage::attack ? std::log ((attackTarget - start) / (attackTarget - 1.0)) / time
                                                 : ln10000 / time;

        if (distance <= endDistance * tolerance)
        {
            finishSegment();
            continue;
        }

        if (n == 0)
            break;

        const auto factor = decayFactor (rate, timeMs, n);

        if (distance * factor > endDistance * tolerance)
        {
            level = target + (level - target) * factor;
            segmentTime += static_cast<double> (n);
            break;
        }

        // The end lies inside this advance: the remaining samples continue in the next segment
        const auto m = std::clamp (static_cast<int> (std::ceil (std::log (distance / endDistance) / rate - 1.0e-6)), 0, n);
        n -= m;
        finishSegment();
    }

    value = static_cast<float> (level);
    return value;
}

} // namespace vocalsynth::synth
