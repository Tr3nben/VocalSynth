#include "vocalsynth/synth/SubOscillator.h"

namespace vocalsynth::synth
{

// Scaffold (docs/SYNTH-DESIGN.md 6.2): silent. Agent A implements.

void SubOscillator::prepare (double newSampleRate)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    reset();
}

void SubOscillator::reset() noexcept
{
    phase = 0.0;
    increment = 0.0;
    gain = 0.0f;
    gainStep = 0.0f;
}

void SubOscillator::restart() noexcept
{
    phase = 0.0;
}

void SubOscillator::beginTick (double newIncrement, float, SubShape newShape, int) noexcept
{
    increment = newIncrement;
    shape = newShape;
}

void SubOscillator::render (float*, float*, int) noexcept {}

} // namespace vocalsynth::synth
