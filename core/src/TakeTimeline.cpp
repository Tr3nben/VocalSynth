#include "vocalsynth/TakeTimeline.h"

namespace vocalsynth
{

// TODO(A): implement (docs/M3-DESIGN.md 6). Stubs: an empty timeline at fallbackBpm.

void TakeTimeline::reset (double newSampleRate)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    segments.clear();
}

void TakeTimeline::addTransport (int64_t, const TransportInfo&)
{
}

void TakeTimeline::evictBefore (int64_t)
{
}

TakeTimeline TakeTimeline::slice (int64_t, int64_t) const
{
    return *this;
}

const TimelineSegment* TakeTimeline::segmentAt (double) const noexcept
{
    return segments.empty() ? nullptr : &segments.front();
}

double TakeTimeline::beatsAt (double sample) const noexcept
{
    return sample / sampleRate * fallbackBpm / 60.0;
}

double TakeTimeline::sampleAtBeats (double beats) const noexcept
{
    return beats * 60.0 / fallbackBpm * sampleRate;
}

double TakeTimeline::bpmAt (double) const noexcept
{
    return fallbackBpm;
}

bool TakeTimeline::isPlayingAt (double) const noexcept
{
    return false;
}

BarPosition TakeTimeline::barAt (double) const noexcept
{
    return {};
}

std::vector<TempoPoint> TakeTimeline::tempoMap (double fromBeats, double) const
{
    return { TempoPoint { fromBeats, fallbackBpm } };
}

std::vector<TimeSignaturePoint> TakeTimeline::timeSignatureMap (double fromBeats, double) const
{
    return { TimeSignaturePoint { fromBeats, 4, 4 } };
}

} // namespace vocalsynth
