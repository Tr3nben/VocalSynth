#include "vocalsynth/PitchPostProcessor.h"

#include <algorithm>
#include <cmath>

namespace vocalsynth
{

void PitchPostProcessor::prepare (double frameRateHz) noexcept
{
    const auto toFrames = [frameRateHz] (double ms) noexcept
    {
        if (! (frameRateHz > 0.0))
            return 1;

        return std::max (1, static_cast<int> (std::ceil (ms * 0.001 * frameRateHz)));
    };

    octaveConfirmFrames = toFrames (octaveConfirmMs);
    octaveMemoryFrames = toFrames (octaveMemoryMs);
    reset();
}

void PitchPostProcessor::reset() noexcept
{
    history.fill (PitchFrame {});
    historyPos = 0;
    referenceHz = 0.0f;
    framesSinceVoiced = 0;
    suspectDirection = 0;
    suspectFrames = 0;
}

void PitchPostProcessor::setMedianLength (int length) noexcept
{
    length = std::clamp (length, 1, maxMedianLength);
    medianLength = length % 2 == 0 ? length - 1 : length;
    reset();
}

namespace
{
/** Median of the first `count` (odd, <= 5) values; sorts the local copy. */
float medianOf (std::array<float, PitchPostProcessor::maxMedianLength> values, int count) noexcept
{
    const auto end = values.begin() + count;
    std::sort (values.begin(), end);
    return values[static_cast<std::size_t> (count / 2)];
}
} // namespace

PitchFrame PitchPostProcessor::process (const PitchFrame& input) noexcept
{
    // 1. Median (unvoiced = 0 Hz, so it is also a voicing majority vote); rmsDb of the newest frame
    history[static_cast<std::size_t> (historyPos)] = input;
    historyPos = historyPos + 1 < medianLength ? historyPos + 1 : 0;

    std::array<float, maxMedianLength> frequencies {}, confidences {};

    for (int i = 0; i < medianLength; ++i)
    {
        const auto& h = history[static_cast<std::size_t> (i)];
        const auto voiced = h.frequencyHz > 0.0f && std::isfinite (h.frequencyHz);
        frequencies[static_cast<std::size_t> (i)] = voiced ? h.frequencyHz : 0.0f;
        confidences[static_cast<std::size_t> (i)] = std::isfinite (h.confidence) ? h.confidence : 0.0f;
    }

    PitchFrame out;
    out.frequencyHz = medianOf (frequencies, medianLength);
    out.confidence = medianOf (confidences, medianLength);
    out.rmsDb = input.rmsDb;

    // 2. Octave guard
    if (! (out.frequencyHz > 0.0f) || out.confidence < guardMinConfidence)
    {
        suspectDirection = 0;
        suspectFrames = 0;

        if (framesSinceVoiced < octaveMemoryFrames)
            ++framesSinceVoiced;

        if (framesSinceVoiced >= octaveMemoryFrames)
            referenceHz = 0.0f;

        return out;
    }

    framesSinceVoiced = 0;

    if (! (referenceHz > 0.0f)) // onset: no reference, never delayed or corrected
    {
        referenceHz = out.frequencyHz;
        return out;
    }

    const auto semitones = 12.0 * std::log2 (static_cast<double> (out.frequencyHz) / static_cast<double> (referenceHz));
    int direction = 0;

    if (std::abs (semitones - 12.0) <= octaveToleranceSemitones)
        direction = 1;
    else if (std::abs (semitones + 12.0) <= octaveToleranceSemitones)
        direction = -1;

    if (direction == 0)
    {
        suspectDirection = 0;
        suspectFrames = 0;
        referenceHz = out.frequencyHz;
        return out;
    }

    if (direction == suspectDirection)
        ++suspectFrames;
    else
    {
        suspectDirection = direction;
        suspectFrames = 1;
    }

    if (suspectFrames >= octaveConfirmFrames) // sustained: accept the new octave
    {
        suspectDirection = 0;
        suspectFrames = 0;
        referenceHz = out.frequencyHz;
        return out;
    }

    out.frequencyHz = direction > 0 ? 0.5f * out.frequencyHz : 2.0f * out.frequencyHz;
    return out;
}

} // namespace vocalsynth
