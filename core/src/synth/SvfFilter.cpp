#include "vocalsynth/synth/SvfFilter.h"

namespace vocalsynth::synth
{

// Scaffold (docs/SYNTH-DESIGN.md 7): passes the signal through unchanged. Agent B implements.

void SvfFilter::prepare (double newSampleRate) noexcept
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    reset();
}

void SvfFilter::reset() noexcept
{
    for (auto& channel : state)
        channel.fill (0.0f);

    g = 0.0f;
    gStep = 0.0f;
    k = 1.4142f;
    kStep = 0.0f;
    k1 = 1.8478f;
    driveGain = 1.0f;
}

void SvfFilter::beginTick (const FilterTick& target, int) noexcept
{
    mode = target.mode;
}

void SvfFilter::process (float*, float*, int) noexcept {}

} // namespace vocalsynth::synth
