#pragma once

#include "vocalsynth/CaptureRecord.h"

#include <cstdint>
#include <vector>

namespace vocalsynth
{

/** One piece of the piecewise-constant tempo map (docs/M3-DESIGN.md 6). */
struct TimelineSegment
{
    int64_t startSample = 0;      ///< capture clock
    double beats = 0.0;           ///< unwrapped beats (quarter notes) at startSample
    double bpm = 120.0;           ///< > 0 (hosts without bpm: 120)
    bool playing = false;         ///< unwrapped beats advance only while playing
    bool hasHostPosition = false; ///< the host reported ppq (else the bar grid counts from beats 0)
    double hostPpq = 0.0;         ///< host ppq at startSample (or = beats without a host position)
    int timeSigNumerator = 4;
    int timeSigDenominator = 4;
    double barAnchorPpq = 0.0;    ///< host ppq of a bar start of this segment's bar grid
    int64_t barAnchorNumber = 1;  ///< FL bar number (first bar = 1) of the bar that starts at barAnchorPpq

    constexpr bool operator== (const TimelineSegment&) const noexcept = default;
};

/** A bar of the host's grid, in unwrapped beats. */
struct BarPosition
{
    int64_t number = 1;           ///< FL convention: the first bar of the song is 1
    double startBeats = 0.0;      ///< unwrapped beats of the bar start
    double lengthBeats = 4.0;     ///< quarter notes per bar = numerator * 4 / denominator
    int timeSigNumerator = 4;
    int timeSigDenominator = 4;
};

struct TempoPoint
{
    double beats = 0.0;           ///< unwrapped beats where this tempo starts
    double bpm = 120.0;
};

struct TimeSignaturePoint
{
    double beats = 0.0;
    int numerator = 4;
    int denominator = 4;
};

/** Sample -> musical time for the capture (docs/M3-DESIGN.md 6). Built from transport records in clock
    order; pure, allocation only in addTransport()/slice() (worker / message thread).

    Playing = isPlaying AND hasPpq (a host that reports "playing" without a position has no grid to sync
    to: it counts as stopped, i.e. free mode). Without bpm: fallbackBpm.

    Unwrapped beats U: beats advance by bpm * elapsed / (60 * sampleRate) from the previous segment while
    playing, and stay constant while stopped. A transport record while playing (previous segment playing
    too) whose host ppq differs from the prediction by less than jumpThresholdBeats re-syncs U to the host
    (U += difference: tempo changes the host made inside a block, rate-limited re-sync records of the tap),
    but U never goes below the U of the previous segment start (monotonic). A larger difference (loop wrap,
    position jump) or a transport start keeps U continuous and only re-anchors the bar grid. A take's ticks
    are (U (s) - U (origin)) * PPQ, so a melody sung across two passes of a loop stays one continuous take.

    Before the first segment, beatsAt() / sampleAtBeats() extrapolate backwards with the first segment's bpm
    if it is playing (a synced take's slice starts with its playing segment, so its bar origin may lie a
    little before the transport start and still has a well-defined sample for the WAV).

    Bar grid: from the host bar start (ppqPositionOfLastBarStart; its bar number = round (barStart / barLength)
    + 1, i.e. the current time signature counted from ppq 0, an approximation when the song changes the time
    signature) or, without it, bars of the current time signature from ppq 0. Bar length = numerator * 4 /
    denominator quarter notes (the real denominator; the SMF writer rounds non-powers of two only in the
    meta event). barAt (s) gives the bar containing s; positions just before a bar line within
    barSnapToleranceBeats count as the next bar (a note sung a hair early on beat 1 starts the bar; its tick
    then clamps to 0, i.e. it moves later by at most the tolerance: 15.6 ms at 120 BPM). */
class TakeTimeline
{
public:
    static constexpr double jumpThresholdBeats = 0.5;
    static constexpr double barSnapToleranceBeats = 1.0 / 32.0;
    static constexpr double fallbackBpm = 120.0;

    TakeTimeline() = default;

    /** Clears all segments. sampleRate > 0 (else 48000). */
    void reset (double sampleRate);

    /** Appends the transport reported at sample (non-decreasing; an equal sample replaces the last
        segment's state but keeps its unwrapped beats). Allocates (amortised). */
    void addTransport (int64_t sample, const TransportInfo& info);

    /** Drops segments that end before sample (keeps the one containing it). */
    void evictBefore (int64_t sample);

    /** Copy of the segments for [fromSample, toSample]: the one containing fromSample (it then starts at
        fromSample: earlier samples extrapolate, see class comment) and every later one up to toSample. */
    TakeTimeline slice (int64_t fromSample, int64_t toSample) const;

    bool empty() const noexcept { return segments.empty(); }
    double getSampleRate() const noexcept { return sampleRate; }
    const std::vector<TimelineSegment>& getSegments() const noexcept { return segments; }

    /** Segment containing sample (the first one for samples before it); nullptr if empty. */
    const TimelineSegment* segmentAt (double sample) const noexcept;

    /** Unwrapped beats at a (fractional) sample. Before the first segment: extrapolated backwards with
        its bpm if it is playing, else its beats. Empty timeline: seconds at fallbackBpm. */
    double beatsAt (double sample) const noexcept;

    /** Inverse of beatsAt() for playing segments (first sample whose beats >= value; used for the WAV
        origin). Empty timeline: fallbackBpm. */
    double sampleAtBeats (double beats) const noexcept;

    double bpmAt (double sample) const noexcept;
    bool isPlayingAt (double sample) const noexcept;

    /** The bar containing sample (with barSnapToleranceBeats), see class comment. */
    BarPosition barAt (double sample) const noexcept;

    /** Tempo map from fromBeats to toBeats: the tempo at fromBeats first (at fromBeats), then every change. */
    std::vector<TempoPoint> tempoMap (double fromBeats, double toBeats) const;

    /** Time signatures from fromBeats to toBeats, like tempoMap(). */
    std::vector<TimeSignaturePoint> timeSignatureMap (double fromBeats, double toBeats) const;

private:
    double sampleRate = 48000.0;
    std::vector<TimelineSegment> segments;
};

} // namespace vocalsynth
