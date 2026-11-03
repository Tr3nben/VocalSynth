#include "vocalsynth/synth/fx/FdnReverb.h"

#include <algorithm>
#include <cmath>

namespace vocalsynth::synth
{

// Scaffold (docs/SYNTH-DESIGN.md 11.3): bypassed. Agent C implements.

void FdnReverb::prepare (double newSampleRate)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    // Longest line (79.1 ms) at the largest size scale (1.5) plus the modulation depth
    const auto length = static_cast<std::size_t> (std::ceil ((0.0791 * 1.5 + 0.001) * sampleRate)) + 4u;

    for (auto& line : lines)
        line.assign (length, 0.0f);

    reset();
}

void FdnReverb::reset() noexcept
{
    for (auto& line : lines)
        std::fill (line.begin(), line.end(), 0.0f);

    damping.fill (0.0f);
    writeIndex = 0;
    onGain = 0.0f;
}

void FdnReverb::beginTick (const ReverbSettings&, float, int) noexcept {}

void FdnReverb::process (float*, float*, int) noexcept {}

bool FdnReverb::isBypassed() const noexcept
{
    return true;
}

} // namespace vocalsynth::synth
