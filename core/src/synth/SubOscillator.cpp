#include "vocalsynth/synth/SubOscillator.h"

#include "vocalsynth/synth/WavetableBuilder.h"

namespace vocalsynth::synth
{

// Scaffold (docs/SYNTH-DESIGN.md 6.2): silent. Agent A implements.

namespace
{
/** The process-wide shape tables (built on the first call, thread-safe static initialisation; never on the
    audio thread). Scaffold: three sine tables; agent A builds sine / triangle / square. */
const std::array<std::unique_ptr<Wavetable>, numSubShapes>& sharedTables()
{
    static const std::array<std::unique_ptr<Wavetable>, numSubShapes> shared {
        WavetableBuilder::makeSineTable(), WavetableBuilder::makeSineTable(), WavetableBuilder::makeSineTable()
    };
    return shared;
}
} // namespace

void SubOscillator::prepare (double newSampleRate)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    tables = &sharedTables();
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
