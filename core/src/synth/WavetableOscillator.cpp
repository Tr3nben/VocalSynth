#include "vocalsynth/synth/WavetableOscillator.h"

namespace vocalsynth::synth
{

// Scaffold (docs/SYNTH-DESIGN.md 6.1): silent. Agent A implements.

void WavetableOscillator::prepare (double newSampleRate) noexcept
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    reset();
}

void WavetableOscillator::reset() noexcept
{
    previousTable = nullptr;
    mipLevel = 0;
    unison = 1;
    phase.fill (0.0);
    increment.fill (0.0);
    gainLeft.fill (0.0f);
    gainRight.fill (0.0f);
}

void WavetableOscillator::setTable (const Wavetable* newTable) noexcept
{
    pendingTable = newTable;
    table = newTable;
}

TablesInUse WavetableOscillator::getTablesInUse() const noexcept
{
    return { table, previousTable };
}

void WavetableOscillator::restart (uint32_t, float) noexcept
{
    phase.fill (0.0);
}

void WavetableOscillator::beginTick (const OscillatorTick& target, int) noexcept
{
    unison = target.unison;
}

void WavetableOscillator::render (float*, float*, int) noexcept {}

} // namespace vocalsynth::synth
