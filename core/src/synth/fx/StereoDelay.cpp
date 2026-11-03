#include "vocalsynth/synth/fx/StereoDelay.h"

#include <algorithm>
#include <cmath>

namespace vocalsynth::synth
{

// Scaffold (docs/SYNTH-DESIGN.md 11.2): bypassed. Agent C implements.

void StereoDelay::prepare (double newSampleRate)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    const auto length = static_cast<std::size_t> (std::ceil (maxDelaySeconds * sampleRate)) + 4u;

    for (auto& line : lines)
        line.assign (length, 0.0f);

    reset();
}

void StereoDelay::reset() noexcept
{
    for (auto& line : lines)
        std::fill (line.begin(), line.end(), 0.0f);

    writeIndex = 0;
    onGain = 0.0f;
}

void StereoDelay::beginTick (const DelaySettings&, const SynthTransport&, float, int) noexcept {}

void StereoDelay::process (float*, float*, int) noexcept {}

bool StereoDelay::isBypassed() const noexcept
{
    return true;
}

} // namespace vocalsynth::synth
