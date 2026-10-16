#include "vocalsynth/TakeSegmenter.h"

#include <algorithm>

namespace vocalsynth
{

// TODO(A): implement (docs/M3-DESIGN.md 5.2). Stubs: never opens a take.

void TakeSegmenter::reset()
{
    state = State::off;
    hasOpenTake = false;
    openTake = {};
    playing = false;
    settings = {};
    soundingNotes = {};
    soundingVelocity = {};
    lastNoteOffSample = 0;
    newestSample = 0;
}

void TakeSegmenter::setTakeGapSeconds (double seconds) noexcept
{
    takeGapSeconds = std::clamp (seconds, minTakeGapSeconds, maxTakeGapSeconds);
}

void TakeSegmenter::process (const CaptureRecord&, const CaptureHistory&, std::vector<Take>&)
{
}

void TakeSegmenter::advanceTo (int64_t sample, const CaptureHistory&, std::vector<Take>&)
{
    newestSample = std::max (newestSample, sample);
}

const Take* TakeSegmenter::getOpenTake() const noexcept
{
    return hasOpenTake ? &openTake : nullptr;
}

std::optional<Take> TakeSegmenter::makeGrab (const CaptureHistory&, int64_t, const MarkerRecord&, int)
{
    return std::nullopt;
}

void TakeSegmenter::closeTake (int64_t, const CaptureHistory&, std::vector<Take>&)
{
}

} // namespace vocalsynth
