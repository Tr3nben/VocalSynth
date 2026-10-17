#include "vocalsynth/TakeTimeline.h"

#include <algorithm>
#include <cmath>

namespace vocalsynth
{

namespace
{
double barLengthOf (const TimelineSegment& s) noexcept
{
    return static_cast<double> (s.timeSigNumerator) * 4.0 / static_cast<double> (s.timeSigDenominator);
}

/** Beats per sample of a playing segment. */
double beatsPerSample (const TimelineSegment& s, double sampleRate) noexcept
{
    return s.bpm / (60.0 * sampleRate);
}
} // namespace

void TakeTimeline::reset (double newSampleRate)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    segments.clear();
}

void TakeTimeline::addTransport (int64_t sample, const TransportInfo& info)
{
    // Sanitise what the host reported (NaN / non-positive values count as "not reported")
    const bool hasPpq = info.hasPpq && std::isfinite (info.ppqPosition);
    const bool hasBpm = info.hasBpm && std::isfinite (info.bpm) && info.bpm > 0.0;
    const bool hasTimeSig = info.hasTimeSignature && info.timeSigNumerator >= 1 && info.timeSigDenominator >= 1;
    const bool hasBarStart = hasPpq && info.hasBarStart && std::isfinite (info.ppqPositionOfLastBarStart);

    TimelineSegment seg;
    seg.startSample = sample;
    seg.bpm = hasBpm ? info.bpm : fallbackBpm;
    seg.playing = info.isPlaying && hasPpq;
    seg.hasHostPosition = hasPpq;
    seg.timeSigNumerator = hasTimeSig ? info.timeSigNumerator : 4;
    seg.timeSigDenominator = hasTimeSig ? info.timeSigDenominator : 4;

    if (segments.empty())
    {
        seg.beats = hasPpq ? info.ppqPosition : 0.0;
    }
    else
    {
        const auto& prev = segments.back();

        if (sample <= prev.startSample)
        {
            // Same sample: replace the state, keep the unwrapped beats
            seg.startSample = prev.startSample;
            seg.beats = prev.beats;
        }
        else
        {
            const auto predictedBeats = beatsAt (static_cast<double> (sample));
            const auto predictedPpq = prev.hostPpq + (predictedBeats - prev.beats);
            seg.beats = predictedBeats;

            if (prev.playing && seg.playing)
            {
                const auto diff = info.ppqPosition - predictedPpq;

                // Re-sync (tempo change inside a block, rate-limited re-sync records); monotonic.
                // Larger differences (loop wrap, jump) keep U continuous and only re-anchor the bar grid.
                if (std::abs (diff) < jumpThresholdBeats)
                    seg.beats = std::max (predictedBeats + diff, prev.beats);
            }
        }

        if (sample <= prev.startSample)
            segments.pop_back();
    }

    seg.hostPpq = hasPpq ? info.ppqPosition : seg.beats;

    const auto barLength = barLengthOf (seg);
    seg.barAnchorPpq = hasBarStart ? info.ppqPositionOfLastBarStart : std::floor (seg.hostPpq / barLength) * barLength;
    seg.barAnchorNumber = static_cast<int64_t> (std::llround (seg.barAnchorPpq / barLength)) + 1;

    segments.push_back (seg);
}

void TakeTimeline::evictBefore (int64_t sample)
{
    if (segments.size() < 2)
        return;

    // Keep the segment containing sample (the last one starting at or before it)
    const auto it = std::upper_bound (segments.begin(), segments.end(), sample,
                                      [] (int64_t s, const TimelineSegment& seg) { return s < seg.startSample; });

    if (it == segments.begin())
        return;

    segments.erase (segments.begin(), it - 1);
}

TakeTimeline TakeTimeline::slice (int64_t fromSample, int64_t toSample) const
{
    TakeTimeline result;
    result.sampleRate = sampleRate;

    if (segments.empty())
        return result;

    auto it = std::upper_bound (segments.begin(), segments.end(), fromSample,
                                [] (int64_t s, const TimelineSegment& seg) { return s < seg.startSample; });

    if (it != segments.begin())
        --it;

    auto first = *it;

    if (fromSample > first.startSample)
    {
        // Cut the containing segment so that it starts at fromSample (same mapping)
        const auto beats = beatsAt (static_cast<double> (fromSample));
        first.hostPpq += beats - first.beats;
        first.beats = beats;
        first.startSample = fromSample;
    }

    result.segments.push_back (first);

    for (++it; it != segments.end() && it->startSample <= toSample; ++it)
        result.segments.push_back (*it);

    return result;
}

const TimelineSegment* TakeTimeline::segmentAt (double sample) const noexcept
{
    if (segments.empty())
        return nullptr;

    const auto it = std::upper_bound (segments.begin(), segments.end(), sample,
                                      [] (double s, const TimelineSegment& seg)
                                      { return s < static_cast<double> (seg.startSample); });

    return it == segments.begin() ? &segments.front() : &*(it - 1);
}

double TakeTimeline::beatsAt (double sample) const noexcept
{
    const auto* seg = segmentAt (sample);

    if (seg == nullptr)
        return sample / sampleRate * fallbackBpm / 60.0;

    if (! seg->playing)
        return seg->beats;

    return seg->beats + (sample - static_cast<double> (seg->startSample)) * beatsPerSample (*seg, sampleRate);
}

double TakeTimeline::sampleAtBeats (double beats) const noexcept
{
    if (segments.empty())
        return beats * 60.0 / fallbackBpm * sampleRate;

    for (std::size_t i = 0; i < segments.size(); ++i)
    {
        const auto& seg = segments[i];
        const auto start = static_cast<double> (seg.startSample);

        if (beats <= seg.beats)
        {
            // Before the first segment: extrapolate backwards if it is playing
            if (i == 0 && seg.playing)
                return start + (beats - seg.beats) / beatsPerSample (seg, sampleRate);

            return start;
        }

        if (! seg.playing)
            continue;

        const bool isLast = i + 1 == segments.size();
        const auto offset = (beats - seg.beats) / beatsPerSample (seg, sampleRate);

        if (isLast || start + offset < static_cast<double> (segments[i + 1].startSample))
            return start + offset;
    }

    return static_cast<double> (segments.back().startSample);
}

double TakeTimeline::bpmAt (double sample) const noexcept
{
    const auto* seg = segmentAt (sample);
    return seg != nullptr ? seg->bpm : fallbackBpm;
}

bool TakeTimeline::isPlayingAt (double sample) const noexcept
{
    const auto* seg = segmentAt (sample);
    return seg != nullptr && seg->playing && sample >= static_cast<double> (seg->startSample);
}

BarPosition TakeTimeline::barAt (double sample) const noexcept
{
    const auto* seg = segmentAt (sample);
    BarPosition bar;

    if (seg == nullptr)
    {
        // Empty timeline: 4/4 bars from sample 0 at fallbackBpm
        const auto beats = beatsAt (sample);
        const auto k = std::floor ((beats + barSnapToleranceBeats) / bar.lengthBeats);
        bar.number = static_cast<int64_t> (k) + 1;
        bar.startBeats = k * bar.lengthBeats;
        return bar;
    }

    const auto barLength = barLengthOf (*seg);
    const auto beats = beatsAt (sample);
    const auto hostPpq = seg->hostPpq + (beats - seg->beats);
    const auto k = std::floor ((hostPpq - seg->barAnchorPpq + barSnapToleranceBeats) / barLength);

    bar.number = seg->barAnchorNumber + static_cast<int64_t> (k);
    bar.startBeats = seg->beats + (seg->barAnchorPpq + k * barLength - seg->hostPpq);
    bar.lengthBeats = barLength;
    bar.timeSigNumerator = seg->timeSigNumerator;
    bar.timeSigDenominator = seg->timeSigDenominator;
    return bar;
}

std::vector<TempoPoint> TakeTimeline::tempoMap (double fromBeats, double toBeats) const
{
    std::vector<TempoPoint> map;

    if (segments.empty())
    {
        map.push_back ({ fromBeats, fallbackBpm });
        return map;
    }

    // Only playing segments define tempo (beats do not advance while stopped); a timeline without any
    // playing segment uses all of them.
    const bool anyPlaying = std::any_of (segments.begin(), segments.end(), [] (const auto& s) { return s.playing; });
    const auto counts = [anyPlaying] (const TimelineSegment& s) { return s.playing || ! anyPlaying; };

    // Tempo at fromBeats: the last counted segment starting at or before it, else the first counted one
    const TimelineSegment* current = nullptr;

    for (const auto& seg : segments)
    {
        if (! counts (seg))
            continue;

        if (current == nullptr || seg.beats <= fromBeats)
            current = &seg;

        if (seg.beats > fromBeats)
            break;
    }

    map.push_back ({ fromBeats, current->bpm });

    for (const auto& seg : segments)
    {
        if (! counts (seg) || seg.beats <= fromBeats || seg.beats >= toBeats)
            continue;

        if (seg.bpm == map.back().bpm)
            continue;

        const auto beats = std::max (seg.beats, map.back().beats);

        if (beats == map.back().beats)
            map.back().bpm = seg.bpm;
        else
            map.push_back ({ beats, seg.bpm });
    }

    return map;
}

std::vector<TimeSignaturePoint> TakeTimeline::timeSignatureMap (double fromBeats, double toBeats) const
{
    std::vector<TimeSignaturePoint> map;

    if (segments.empty())
    {
        map.push_back ({ fromBeats, 4, 4 });
        return map;
    }

    // Time signature at fromBeats: the last segment starting at or before it, else the first one
    const auto* first = &segments.front();

    for (const auto& seg : segments)
    {
        if (seg.beats > fromBeats)
            break;

        first = &seg;
    }

    map.push_back ({ fromBeats, first->timeSigNumerator, first->timeSigDenominator });

    for (const auto& seg : segments)
    {
        if (seg.beats <= fromBeats || seg.beats >= toBeats)
            continue;

        if (seg.timeSigNumerator == map.back().numerator && seg.timeSigDenominator == map.back().denominator)
            continue;

        // The change takes effect at the host bar start of this segment's grid (in unwrapped beats)
        const auto barLength = barLengthOf (seg);
        const auto k = std::floor ((seg.hostPpq - seg.barAnchorPpq + barSnapToleranceBeats) / barLength);
        const auto barStartBeats = seg.beats + (seg.barAnchorPpq + k * barLength - seg.hostPpq);
        const auto beats = std::clamp (barStartBeats, map.back().beats, seg.beats);

        if (beats == map.back().beats)
        {
            map.back().numerator = seg.timeSigNumerator;
            map.back().denominator = seg.timeSigDenominator;
        }
        else
        {
            map.push_back ({ beats, seg.timeSigNumerator, seg.timeSigDenominator });
        }
    }

    return map;
}

} // namespace vocalsynth
