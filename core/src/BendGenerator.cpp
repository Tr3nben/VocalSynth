#include "vocalsynth/BendGenerator.h"

#include <algorithm>
#include <cmath>

namespace vocalsynth
{

int16_t BendGenerator::centsToBend (double cents, int rangeSemitones) noexcept
{
    if (rangeSemitones <= 0 || ! std::isfinite (cents))
        return 0;

    const auto x = std::clamp (cents / (100.0 * static_cast<double> (rangeSemitones)), -1.0, 1.0);
    const auto value = x >= 0.0 ? std::lround (x * 8191.0) : std::lround (x * 8192.0);
    return static_cast<int16_t> (std::clamp (value, -8192L, 8191L));
}

double BendGenerator::shapeCents (BendMode bendMode, double residualCents) noexcept
{
    switch (bendMode)
    {
        case BendMode::off:
            return 0.0;

        case BendMode::anchored:
        {
            const auto magnitude = std::abs (residualCents);

            if (magnitude <= anchoredDeadZoneCents)
                return 0.0;

            return std::copysign (magnitude - anchoredDeadZoneCents, residualCents);
        }

        case BendMode::trueBend:
            return residualCents;
    }

    return 0.0;
}

void BendGenerator::prepare (double newSampleRate, double frameIntervalSeconds) noexcept
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    minIntervalSamples = std::max (1L, std::lround (sampleRate / maxUpdateRateHz));
    smoothingCoefficient = frameIntervalSeconds > 0.0
                               ? std::exp (-frameIntervalSeconds / (anchoredSmoothingMs * 0.001))
                               : 0.0;
    reset();
}

void BendGenerator::reset() noexcept
{
    smoothedCents = 0.0;
    lastValue = 0;
    lastEmitSample = 0;
    hasEmitted = false;
}

void BendGenerator::setMode (BendMode newMode, BendRange newRange) noexcept
{
    mode = newMode;
    rangeSemitones = bendRangeSemitones (newRange);
}

double BendGenerator::shapedOutputCents (double residualCents, bool restartSmoother) noexcept
{
    const auto target = shapeCents (mode, std::isfinite (residualCents) ? residualCents : 0.0);

    // The smoother always tracks the bend that is being produced, so a mode switch to anchored in the
    // middle of a note glides from the current bend instead of jumping.
    if (mode == BendMode::anchored && ! restartSmoother)
        smoothedCents = smoothingCoefficient * smoothedCents + (1.0 - smoothingCoefficient) * target;
    else
        smoothedCents = target;

    return smoothedCents;
}

void BendGenerator::emit (int16_t value, int64_t absoluteSample, int sampleOffset, uint8_t channel,
                          VsEventSink out) noexcept
{
    out.push (VsEvent::makePitchBend (sampleOffset, channel, value));
    lastValue = value;
    lastEmitSample = absoluteSample;
    hasEmitted = true;
}

void BendGenerator::noteStarted (double residualCents, int64_t absoluteSample, int sampleOffset, uint8_t channel,
                                 VsEventSink out) noexcept
{
    const auto value = centsToBend (shapedOutputCents (residualCents, true), rangeSemitones);

    if (value != lastValue)
        emit (value, absoluteSample, sampleOffset, channel, out);
}

void BendGenerator::update (double residualCents, int64_t absoluteSample, int sampleOffset, uint8_t channel,
                            VsEventSink out) noexcept
{
    const auto value = centsToBend (shapedOutputCents (residualCents, false), rangeSemitones);

    // Compared with the last EMITTED value: a change held back by the rate limit goes out at the next
    // allowed frame, even if the value has not moved since.
    if (value != lastValue && (! hasEmitted || absoluteSample - lastEmitSample >= minIntervalSamples))
        emit (value, absoluteSample, sampleOffset, channel, out);
}

void BendGenerator::noteEnded (int64_t absoluteSample, int sampleOffset, uint8_t channel, VsEventSink out) noexcept
{
    smoothedCents = 0.0;

    if (lastValue != 0)
        emit (0, absoluteSample, sampleOffset, channel, out);
}

} // namespace vocalsynth
