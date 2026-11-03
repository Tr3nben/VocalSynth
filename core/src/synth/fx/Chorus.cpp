#include "vocalsynth/synth/fx/Chorus.h"

#include <cmath>

namespace vocalsynth::synth
{

// Scaffold (docs/SYNTH-DESIGN.md 11.1): bypassed. Agent C implements.

void Chorus::prepare (double newSampleRate)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    const auto length = static_cast<std::size_t> (std::ceil (maxDelaySeconds * sampleRate)) + 4u;

    for (auto& line : lines)
        line.assign (length, 0.0f);

    reset();
}

void Chorus::reset() noexcept
{
    for (auto& line : lines)
        std::fill (line.begin(), line.end(), 0.0f);

    writeIndex = 0;
    lfoPhase = 0.0;
    onGain = 0.0f;
}

void Chorus::beginTick (const ChorusSettings&, float, int) noexcept {}

void Chorus::process (float*, float*, int) noexcept {}

bool Chorus::isBypassed() const noexcept
{
    return true;
}

} // namespace vocalsynth::synth
