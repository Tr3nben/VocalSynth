#include "vocalsynth/synth/NoiseSource.h"

namespace vocalsynth::synth
{

// Scaffold (docs/SYNTH-DESIGN.md 6.3): silent. Agent A implements.

void NoiseSource::prepare (double, uint32_t newSeed) noexcept
{
    seed = newSeed != 0 ? newSeed : 1u;
    reset();
}

void NoiseSource::reset() noexcept
{
    state = seed;
    pinkState.fill (0.0f);
    gain = 0.0f;
    gainStep = 0.0f;
}

void NoiseSource::beginTick (float, float newColour, int) noexcept
{
    colour = newColour;
}

void NoiseSource::render (float*, float*, int) noexcept {}

} // namespace vocalsynth::synth
