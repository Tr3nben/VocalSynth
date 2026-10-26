#pragma once

#include "vocalsynth/CaptureRecord.h"
#include "vocalsynth/EventTiming.h"
#include "vocalsynth/TakeTimeline.h"
#include "vocalsynth/VsEvent.h"

#include <cstdint>
#include <limits>
#include <vector>

namespace vocalsynth
{

/** How a take's time is mapped (docs/M3-DESIGN.md 5). */
enum class TakeMode : uint8_t
{
    synced, ///< transport playing: beats from the host tempo map, tick 0 at a bar start
    free    ///< transport stopped / no playhead: seconds at the host bpm (else 120), tick 0 at the first note
};

/** Why a take exists. */
enum class TakeKind : uint8_t
{
    automatic, ///< transport start/stop or free-mode note/gap rules
    manual,    ///< started by "New take"
    grab,      ///< "Grab last ..." (retroactive)
    record     ///< M3.1 Record mode: exactly Take::maxBars bars from bar Take::anchorBar (docs/M3.1-DESIGN.md 4)
};

/** Whether the dry voice of a take is available (set by the worker when the take closes). */
enum class TakeAudio : uint8_t
{
    unknown,   ///< not decided yet (open take)
    none,      ///< captureAudio was off, or the history no longer holds any of it
    full,
    truncated  ///< part of the take's audio span was evicted or not captured (silence there in the WAV)
};

/** One captured event of a take (decoded CaptureRecord). */
struct CapturedEvent
{
    static constexpr int64_t noSample = std::numeric_limits<int64_t>::min();

    int64_t sample = 0;          ///< emit sample on the capture clock (what the MIDI output saw)
    int64_t estimatedSample = 0; ///< true-time estimate: sample - timing.delaySamples (may be < 0 near clock 0)
    int64_t refinedSample = noSample; ///< note-on refined on the dry audio (TakeRenderer::refineNoteOnsets)
    VsEvent event;               ///< sampleOffset unused (0)
    VsEventTiming timing;
    bool synthetic = false;      ///< added by the TakeSegmenter: note-on of a note sounding at the take start,
                                 ///< or note-off closing a note at captureEndSample / at a gap (resync)
                                 ///< (resync note-ons of the tap keep EventTimingFlags::resync instead)

    constexpr bool operator== (const CapturedEvent&) const noexcept = default;
};

/** A take as the worker stores it and the renderer reads it (docs/M3-DESIGN.md 5). Plain value; owns its
    events and a slice of the timeline, so it stays valid after the history evicted the data. Built on the
    worker thread; published to the message thread as std::shared_ptr<const Take> (immutable from then on).

    Two spans (docs/M3-DESIGN.md 5.2):
    - [startSample, endSample): where the take is, in host time (transport start / stop, markers; free takes:
      first note-on emit sample to last note-off + freeTailSeconds; M3.1: a synced take anchored at bar X
      starts at bar X at the earliest and is cut after maxBars bars, a Record take spans its bars exactly).
      Synced exports clamp every compensated event time (estimatedSample - roundTrip) into it. An anchored
      take may hold events emitted before its startSample (the clamp drops them).
    - [startSample, captureEndSample): the emit samples whose events the take holds. Synced takes and grabs
      keep collecting for TakeSegmenter::compensationTailSamples() after endSample, because what was sung
      just before Stop / the split / the grab reaches the plugin a round trip + detection delay later
      (consecutive takes overlap there: both hold those events, the renderer's clamp sorts them out).
      Free takes: captureEndSample == endSample. */
struct Take
{
    int number = 0;                 ///< 1, 2, ... per plugin instance (never reused)
    TakeMode mode = TakeMode::free;
    TakeKind kind = TakeKind::automatic;
    bool closed = false;

    int64_t startSample = 0;        ///< capture clock, inclusive
    int64_t endSample = 0;          ///< exclusive; open take: the newest sample processed
    int64_t captureEndSample = 0;   ///< exclusive, >= endSample (see above); open take: the newest sample
    double sampleRate = 48000.0;    ///< from the reset record
    int maxBlockSize = 0;           ///< from the reset record (Auto capture offset)
    CaptureSettingsInfo settings;   ///< at the take start (bend mode / range, channel, minNoteLength, source)

    std::vector<CapturedEvent> events; ///< sorted by sample (stable: the order they were sent)
    TakeTimeline timeline;          ///< synced: segments from startSample (the playing segment) to
                                    ///< captureEndSample, kept by the segmenter while the take is open (the
                                    ///< history may evict the start of a long take); free: may be empty
    double freeBpm = TakeTimeline::fallbackBpm; ///< free mode: host bpm at the take start, else 120

    /** M3.1 take length (docs/M3.1-DESIGN.md 3, 4): the limit in bars in force for this take (background:
        takeMaxBars when the take was anchored / opened; Record: N), 0 = none. */
    int maxBars = 0;
    /** M3.1: FL bar number X the take is anchored at (synced background: the bar of its first note that lasts
        the ghost threshold, set once it is known, also with limit Off where only the status line uses it;
        Record: the first recorded bar), 0 = not anchored (yet), free takes, grabs. */
    int64_t anchorBar = 0;

    int noteCount = 0;              ///< note-ons (incl. synthetic ones at the take start); kept up to date
                                    ///< while the take is open (the status line shows it)
    uint32_t clockEpoch = 0;        ///< CaptureHistory::getClockEpoch() when the take opened: the capture
                                    ///< clock restarts at a clearing reset (new sample rate), so sample
                                    ///< positions of takes of an earlier epoch never address newer audio
    bool incomplete = false;        ///< a gap record (ring overflow) fell into the take
    TakeAudio audio = TakeAudio::unknown;

    /** Waveform strip for the UI (worker, when the take closes with audio): min / max pairs
        (overview[2 i], overview[2 i + 1]) of overviewSamplesPerPoint samples each, from the take's audio span. */
    static constexpr int overviewSamplesPerPoint = 512;
    std::vector<float> overview;

    int64_t wallClockStartMs = 0;   ///< plugin: milliseconds since 1970 at startSample (file names), 0 = unknown
    int observedBlockSize = 0;      ///< plugin: largest host block seen when the take closed (RecentBlockSize),
                                    ///< informational (UI, FL check of Auto); 0 = unknown
};

} // namespace vocalsynth
