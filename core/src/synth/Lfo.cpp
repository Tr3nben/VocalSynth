#include "vocalsynth/synth/Lfo.h"

namespace vocalsynth::synth
{

// Scaffold (docs/SYNTH-DESIGN.md 8.2): constant 0. Agent B implements.

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
    holdValue = 0.0f;
    nextHoldValue = 0.0f;
    value = 0.0f;
}

void Lfo::retrigger (const LfoSettings&) noexcept
{
    fadeSamples = 0.0;
}

float Lfo::advanceTick (const LfoSettings&, double, float, int) noexcept
{
    return value;
}

} // namespace vocalsynth::synth
