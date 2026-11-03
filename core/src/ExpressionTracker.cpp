#include "vocalsynth/ExpressionTracker.h"

namespace vocalsynth
{

// Scaffold (docs/SYNTH-DESIGN.md 16): all values 0. Agent C implements.

void ExpressionTracker::prepare (double analysisSampleRate, int hop)
{
    analysisRate = analysisSampleRate > 0.0 ? analysisSampleRate : 16000.0;
    hopSamples = hop > 0 ? hop : 1;
    fft.prepare (fftOrder);
    windowed.assign (static_cast<std::size_t> (fftSize), 0.0f);
    power.assign (static_cast<std::size_t> (fftSize / 2 + 1), 0.0f);
    hann.assign (static_cast<std::size_t> (fftSize), 0.0f);
    reset();
}

void ExpressionTracker::reset() noexcept
{
    hopCounter = 0;
    current = {};
}

ExpressionFrame ExpressionTracker::processHop (const ExpressionTrackerInput&) noexcept
{
    return current;
}

} // namespace vocalsynth
