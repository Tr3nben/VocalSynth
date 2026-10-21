#include "vocalsynth/CaptureMode.h"

#include "vocalsynth/TakeTimeline.h"

#include <algorithm>
#include <cmath>

namespace vocalsynth
{

int64_t freeModeBarsToSamples (int bars, double sampleRate, double bpm) noexcept
{
    if (bars <= 0 || ! std::isfinite (sampleRate) || sampleRate <= 0.0)
        return 0;

    // Below 1 BPM (no host sends that) clamps to 1, so the result stays far inside int64
    const auto tempo = std::isfinite (bpm) && bpm > 0.0 ? std::max (bpm, 1.0) : TakeTimeline::fallbackBpm;
    return std::llround (static_cast<double> (bars) * freeModeBeatsPerBar * 60.0 * sampleRate / tempo);
}

} // namespace vocalsynth
