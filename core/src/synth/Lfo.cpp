#include "vocalsynth/synth/Lfo.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace vocalsynth::synth
{

// Global LFO, one value per control tick (docs/SYNTH-DESIGN.md 8.2; shapes and retrigger rules in the
// header). The phase is a double advanced by rate * tickSamples / fs per tick, so it depends only on the
// number of ticks since reset() (block-size invariant).

void Lfo::prepare (double newSampleRate, uint32_t newSeed) noexcept
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    seed = newSeed != 0 ? newSeed : 1u;
    reset();
}

void Lfo::reset() noexcept
{
    random = seed;
    phase = 0.0;
    fadeSamples = 0.0;
    fading = false;
    retriggerPending = false;
    phaseResetPending = false;
    holdValue = nextRandom();
    nextHoldValue = nextRandom();
    value = 0.0f;
}

float Lfo::nextRandom() noexcept
{
    // xorshift32 -> [-1, 1]
    random ^= random << 13;
    random ^= random >> 17;
    random ^= random << 5;
    return static_cast<float> (static_cast<double> (random) * (2.0 / 4294967295.0) - 1.0);
}

void Lfo::newCycle() noexcept
{
    holdValue = nextHoldValue;
    nextHoldValue = nextRandom();
}

void Lfo::retrigger (const LfoSettings& settings) noexcept
{
    retriggerPending = true;
    phaseResetPending = phaseResetPending || settings.retrigger;
}

float Lfo::shapeValue (LfoShape shape) const noexcept
{
    const auto p = phase;

    switch (shape)
    {
        case LfoShape::sine:
            return static_cast<float> (std::sin (2.0 * std::numbers::pi * p));
        case LfoShape::triangle:
        {
            auto q = p + 0.25;
            q -= std::floor (q);
            return static_cast<float> (1.0 - 4.0 * std::abs (q - 0.5));
        }
        case LfoShape::sawUp:
            return static_cast<float> (2.0 * p - 1.0);
        case LfoShape::sawDown:
            return static_cast<float> (1.0 - 2.0 * p);
        case LfoShape::square:
            return p < 0.5 ? 1.0f : -1.0f;
        case LfoShape::sampleHold:
            return holdValue;
        case LfoShape::smoothRandom:
        {
            const auto w = 0.5 - 0.5 * std::cos (std::numbers::pi * p);
            return static_cast<float> (static_cast<double> (holdValue)
                                       + static_cast<double> (nextHoldValue - holdValue) * w);
        }
    }

    return 0.0f;
}

float Lfo::advanceTick (const LfoSettings& settings, const SynthTransport& transport, float rateOctaves,
                        int tickSamples) noexcept
{
    const auto samples = static_cast<double> (std::max (0, tickSamples));

    // Rate in Hz: free or synced to the host tempo (bars follow the host time signature)
    double rate = std::isfinite (settings.rateHz) ? static_cast<double> (settings.rateHz) : 1.0;

    if (settings.sync != TempoDivision::off)
    {
        const auto bpm = std::isfinite (transport.bpm) && transport.bpm > 0.0 ? transport.bpm : 120.0;
        const auto quarters = tempoDivisionQuarterNotes (settings.sync, transport.quarterNotesPerBar);

        if (quarters > 0.0)
            rate = bpm / 60.0 / quarters;
    }

    if (std::isfinite (rateOctaves) && rateOctaves != 0.0f)
        rate *= std::exp2 (static_cast<double> (rateOctaves));

    rate = std::clamp (rate, 0.001, 100.0);

    if (retriggerPending && phaseResetPending)
    {
        // Retrigger quantised to this tick: the value at phase 0 (a new S&H cycle)
        phase = 0.0;
        newCycle();
    }
    else
    {
        phase += rate * samples / sampleRate;

        if (phase >= 1.0)
        {
            phase -= std::floor (phase);
            newCycle();
        }
    }

    if (retriggerPending)
    {
        fading = true;
        fadeSamples = 0.0;
    }
    else if (fading)
    {
        fadeSamples += samples;
    }

    retriggerPending = false;
    phaseResetPending = false;

    auto out = shapeValue (settings.shape);

    if (fading)
    {
        const auto fadeLength = std::isfinite (settings.fadeMs) ? static_cast<double> (settings.fadeMs) * 0.001 * sampleRate
                                                                : 0.0;

        if (fadeSamples >= fadeLength)
            fading = false;
        else
            out = static_cast<float> (static_cast<double> (out) * (fadeSamples / fadeLength));
    }

    value = out;
    return value;
}

} // namespace vocalsynth::synth
