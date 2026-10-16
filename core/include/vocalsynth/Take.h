#pragma once

#include "vocalsynth/CaptureRecord.h"
#include "vocalsynth/EventTiming.h"
#include "vocalsynth/TakeTimeline.h"
#include "vocalsynth/VsEvent.h"

#include <cstdint>
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
    grab       ///< "Grab last ..." (retroactive)
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
    int64_t sample = 0;          ///< emit sample on the capture clock (what the MIDI output saw)
    int64_t estimatedSample = 0; ///< true-time estimate: sample - timing.delaySamples
    int64_t refinedSample = -1;  ///< note-on refined on the dry audio (TakeRenderer::refineNoteOnsets), -1 = none
    VsEvent event;               ///< sampleOffset unused (0)
    VsEventTiming timing;
    bool synthetic = false;      ///< added by the TakeSegmenter: note-on of a note sounding at the take start,
                                 ///< or note-off closing a note at the take end / at a manual split

    constexpr bool operator== (const CapturedEvent&) const noexcept = default;
};

/** A take as the worker stores it and the renderer reads it (docs/M3-DESIGN.md 5). Plain value; owns its
    events and a slice of the timeline, so it stays valid after the history evicted the data. Built on the
    worker thread; published to the message thread as std::shared_ptr<const Take> (immutable from then on). */
struct Take
{
    int number = 0;                 ///< 1, 2, ... per plugin instance (never reused)
    TakeMode mode = TakeMode::free;
    TakeKind kind = TakeKind::automatic;
    bool closed = false;

    int64_t startSample = 0;        ///< capture clock, inclusive
    int64_t endSample = 0;          ///< exclusive; open take: the newest sample processed
    double sampleRate = 48000.0;    ///< from the reset record
    int maxBlockSize = 0;           ///< from the reset record (Auto capture offset)
    CaptureSettingsInfo settings;   ///< at the take start (bend mode / range, channel, minNoteLength, source)

    std::vector<CapturedEvent> events; ///< sorted by sample (stable: the order they were sent)
    TakeTimeline timeline;          ///< slice covering [startSample - margin, endSample + margin]
    double freeBpm = TakeTimeline::fallbackBpm; ///< free mode: host bpm at the take start, else 120

    int noteCount = 0;              ///< note-ons (incl. synthetic ones at the take start)
    bool incomplete = false;        ///< a gap record (ring overflow) fell into the take
    TakeAudio audio = TakeAudio::unknown;

    int64_t wallClockStartMs = 0;   ///< plugin: milliseconds since 1970 at startSample (file names), 0 = unknown
};

} // namespace vocalsynth
