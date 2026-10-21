#pragma once

#include "vocalsynth/CaptureRecord.h"
#include "vocalsynth/EventTiming.h"
#include "vocalsynth/SpscRing.h"
#include "vocalsynth/VsEvent.h"

#include <array>
#include <atomic>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <span>

namespace vocalsynth
{

/** Per-block switches and settings handed to CaptureTap::beginBlock() (built from the raw parameter atomics). */
struct CaptureBlockSettings
{
    bool captureEnabled = true;   ///< parameter captureEnabled
    bool captureAudio = true;     ///< parameter captureAudio
    CaptureSettingsInfo settings; ///< recorded on change
    bool bypassed = false;        ///< the block is processed by processBlockBypassed (markers bypassOn / bypassOff)
};

/** Audio-thread side of the M3 capture (docs/M3-DESIGN.md 4). Turns what the plugin sends (VsEvents incl.
    flush note-offs), the dry mono input and the host transport into a stream of CaptureRecords plus a
    stream of mono audio frames, written into two SPSC rings that the capture worker drains.

    Per host block with numSamples >= 1 (binding call order, see docs/M3-DESIGN.md 4.1; 0-sample blocks
    do not call the tap):
        tap.beginBlock (transport, settings);          // once, before anything of the block
        tap.flushNotes();                              // only if the processor flushes (midiOutput.flushAllNotes)
        for each chunk: tap.processChunk (mono, n, events, timings);   // after the chunk's events are final
    Bypassed block: tap.beginBlock (...); tap.flushNotes(); tap.processBypassed (n);

    Clock: absolute host samples since the last clearing reset (first prepare() or a new sample rate);
    prepare() with the same rate keeps the clock running, so the history survives FL's engine restarts.
    Records of one block are stamped: reset, transport, settings, markers, flush note-offs at the block
    start sample; events at chunk start + clamp (sampleOffset, 0, n - 1); audio frames at their first sample.
    Block-size invariant (binding): for the same input, events and per-block transport (with transport
    changes on block boundaries of every compared block size), the record stream without the block
    structure is identical for any block split; audio frames are fixed (audioFrameSamples, aligned to the
    clock).

    Events are recorded exactly as MidiOutputStage sends them (binding, so Raw = what the synth got and the
    tap's open-note set always equals the output's): channel / data / bend clamped to MIDI ranges, a note-on
    with velocity 0 becomes a note-off, a note-off for a note that is not open is dropped, a note-on for an
    open note is preceded by a note-off of that note (same sample, same timing). timings[i] belongs to
    events[i]; missing entries (timings shorter than events, e.g. empty) mean kind none, delay 0.

    Transport records (docs/M3-DESIGN.md 4.3) are pushed only on change, compared on quantised values
    (quantiseTransport):
    - at once ("structural"): first block after a reset record, captureOn or a gap record; play state;
      valid flags; time signature; a position jump while playing (|reported ppq - predicted| >=
      resyncLimitBeats: loop wrap, relocation);
    - rate-limited to one record per minResyncIntervalSeconds ("re-sync"): bpm-only changes and position
      deviations below resyncLimitBeats while playing (|reported - predicted| > ppqToleranceBeats, or the bar
      start differs from the predicted one by more than barStartToleranceBeats). A held change is sent with
      the first block at or after the interval, with that block's values. Bounded: <= 100 records/s for tempo
      ramps, hosts with jittery positions or without bpm, even with 1-sample blocks;
    - while stopped (before and now), position-only changes at most every stoppedPositionIntervalSeconds.
    Prediction from the last record: ppq + bpm * (sample - recordSample) / (60 sr) while playing (unchanged
    while stopped); bar start = last bar start + k * barLength with k = floor ((predicted ppq - last bar start
    + barStartToleranceBeats) / barLength), so crossing a bar line inside a block is no discontinuity.

    Overflow (docs/M3-DESIGN.md 4.4): a record or audio frame that does not fit is dropped and counted (ring
    overflow counters); the next records that fit start with a gap record, followed by a note-on with flag
    EventTimingFlags::resync for every note still open (the worker's note state may have lost a note-on or
    note-off) and a forced transport and settings record. Gap + resync note-ons are pushed together or
    stay pending. Audio frame + its audio record are pushed together or not at all (the worker places audio
    only through audio records).

    Real-time safe after prepare(): beginBlock, flushNotes, processChunk, processBypassed never allocate,
    lock, log or throw; bounded by the number of samples and events (worst case per call: 16 x 128 note-offs
    at captureOff, one marker per kind). request...() are lock-free (any thread). Not thread-safe otherwise:
    one producer thread (the audio thread). */
class CaptureTap
{
public:
    static constexpr std::size_t recordRingCapacity = 16384; ///< records (power of two, 512 KB)
    static constexpr double audioRingSeconds = 2.0;           ///< at least this much audio, rounded up to 2^n
    static constexpr int audioFrameSamples = 256;             ///< audio granularity (aligned to the clock)
    static constexpr double ppqToleranceBeats = 1.0e-6;
    /** The bar start travels as a float offset (CaptureRecord): up to ~4e-6 beats of rounding for long bars. */
    static constexpr double barStartToleranceBeats = 1.0e-4;
    /** Same value as TakeTimeline::jumpThresholdBeats (static_assert in CaptureTap.cpp): below it the
        timeline re-syncs, at or above it the bar grid re-anchors (loop wrap / jump). */
    static constexpr double resyncLimitBeats = 0.5;
    static constexpr double minResyncIntervalSeconds = 0.01;
    static constexpr double stoppedPositionIntervalSeconds = 0.05;

    CaptureTap() = default;

    /** Message thread, while neither process...() nor the consumer runs (the plugin suspends the worker).
        Allocates the rings for this rate if they must grow (NOT real-time; may throw std::bad_alloc) and
        arms a reset record for the next beginBlock(). Same rate as before: keeps the clock, the open notes
        (MidiOutputStage keeps them too and the processor's next flush turns them off: flushNotes() then
        records the same note-offs) and the staged audio frame. New rate or first call (clearing reset):
        clock 0, open notes and staged audio forgotten. */
    void prepare (double sampleRate, int maxBlockSize);

    //==============================================================================
    // Audio thread

    /** Start of a host block: pushes the pending reset record, a transport record if it changed, a settings
        record if they changed, on/off markers for captureEnabled, captureAudio and bypass transitions
        (captureOff: note-offs for open notes first; captureOff / audioOff: the staged partial audio frame is
        completed with zeros and pushed before the marker), then the markers of pending requests: at most one
        per kind per block, in the order newTake, grab, clear, recordCancel, recordArm (requests made between
        two blocks coalesce; a grab / Record carries the newest lengths). */
    void beginBlock (const TransportInfo& transport, const CaptureBlockSettings& settings) noexcept;

    /** Note-off for every note the tap saw on (and a bend centre on channels whose last bend was not 0) at
        the current clock: the processor called midiOutput.flushAllNotes(). Timing kind none. */
    void flushNotes() noexcept;

    /** One chunk of the block: events as written to the MIDI output (sampleOffset within this chunk) with
        their timing (timings.size() == events.size(), or empty = kind none for all), and the chunk's dry
        mono input (nullptr = silence; captured only while captureAudio is on). Advances the clock by
        numSamples (<= 0: nothing). */
    void processChunk (const float* mono, int numSamples, std::span<const VsEvent> events,
                       std::span<const VsEventTiming> timings) noexcept;

    /** A bypassed block: no events; the audio is captured as silence; advances the clock. */
    void processBypassed (int numSamples) noexcept;

    //==============================================================================
    // Any thread (UI requests: the next beginBlock() turns them into markers at the block start)

    void requestNewTake() noexcept;
    void requestGrab (int bars, double seconds) noexcept;
    void requestClear() noexcept;
    /** M3.1 Record button (docs/M3.1-DESIGN.md 4.2): marker recordArm carrying bars (clamped to >= 1). */
    void requestRecord (int bars) noexcept;
    /** M3.1 Cancel: marker recordCancel. Marker order at one block start: newTake, grab, clear, recordCancel,
        recordArm. */
    void requestRecordCancel() noexcept;

    /** Samples processed since the last clearing reset. Stored with release after the block's records were
        pushed: a consumer that loads it (acquire) and then drains the record ring has every record with
        sample < clock. */
    int64_t getClock() const noexcept { return publishedClock.load (std::memory_order_acquire); }

    struct OverflowCounts
    {
        uint64_t records = 0;      ///< records dropped since prepare()
        uint64_t audioSamples = 0; ///< audio samples dropped since prepare()
    };

    OverflowCounts getOverflowCounts() const noexcept;

    double getSampleRate() const noexcept { return sampleRate; }  ///< after prepare()
    int getMaxBlockSize() const noexcept { return maxBlockSize; } ///< after prepare()

    //==============================================================================
    // Consumer (capture worker thread)

    SpscRing<CaptureRecord>& getRecordRing() noexcept { return records; }
    SpscRing<float>& getAudioRing() noexcept { return audio; }

private:
    /** Pushes record (after a pending gap, see pushPendingGap); position = the sample the tap is at (for
        the gap record). Counts a drop and arms a gap if it does not fit. */
    bool pushRecord (const CaptureRecord& record, int64_t position) noexcept;
    /** Gap record + one resync note-on per open note at position, if they and reserve more records fit. */
    bool pushPendingGap (int64_t position, std::size_t reserve) noexcept;
    void dropRecords (uint64_t count) noexcept;
    void pushTransportIfChanged (const TransportInfo& transport, bool force) noexcept;
    void pushMarker (CaptureMarkerKind kind, int bars = 8, double seconds = 30.0) noexcept;
    void pushOpenNoteOffs() noexcept; ///< capture-only note-offs (captureOff), state kept
    void pushEvent (const VsEvent& event, const VsEventTiming& timing, int64_t sample) noexcept;
    void startAudio() noexcept;
    void stopAudio() noexcept;
    void stageAudio (const float* mono, int numSamples) noexcept;
    void pushAudioFrame (int64_t frameStart) noexcept;
    void advanceClock (int numSamples) noexcept;

    SpscRing<CaptureRecord> records;
    SpscRing<float> audio;

    double sampleRate = 0.0;
    int maxBlockSize = 0;
    int64_t clock = 0;
    std::atomic<int64_t> publishedClock { 0 };
    int64_t resyncIntervalSamples = 480;
    int64_t stoppedIntervalSamples = 2400;

    bool resetPending = true;
    bool forceStateRecords = true; ///< transport + settings record at the next enabled beginBlock (reset, captureOn, drop)
    bool resetClearsHistory = true;

    // Change detection (audio thread)
    bool hasLastTransport = false;
    TransportInfo lastTransport {};     ///< quantised (as decoded from its record)
    int64_t lastTransportSample = 0;
    bool hasLastSettings = false;
    CaptureSettingsInfo lastSettings {};

    // State as recorded in the stream (markers); cleared by a clearing reset
    bool enabled = false;
    bool audioOn = false;  ///< enabled and captureAudio: audio frames are staged
    bool bypassed = false;

    // Open notes, mirrored from what MidiOutputStage sends (tracked also while capture is off)
    std::array<std::bitset<128>, 16> openNotes {};
    std::array<std::array<uint8_t, 128>, 16> openVelocity {};
    int openNoteCount = 0;
    std::bitset<16> bendNotCentred {};

    // Audio staging: one frame
    std::array<float, audioFrameSamples> frame {};
    int frameFill = 0;

    // Overflow -> gap record
    uint64_t pendingDroppedRecords = 0;
    uint64_t pendingDroppedAudio = 0;
    bool gapPending = false;
    int64_t highWater = 0; ///< newest position anything was pushed at (gap records never go backwards)
    std::atomic<uint64_t> droppedRecordsTotal { 0 };
    std::atomic<uint64_t> droppedAudioTotal { 0 };

    // UI requests (counters: the audio thread remembers the last value it handled)
    std::atomic<uint32_t> newTakeRequests { 0 };
    std::atomic<uint32_t> grabRequests { 0 };
    std::atomic<uint32_t> clearRequests { 0 };
    std::atomic<uint32_t> recordRequests { 0 };
    std::atomic<uint32_t> recordCancelRequests { 0 };
    std::atomic<int> recordBars { 8 };
    std::atomic<int> grabBars { 8 };
    std::atomic<double> grabSeconds { 30.0 };
    uint32_t handledNewTake = 0;
    uint32_t handledGrab = 0;
    uint32_t handledClear = 0;
    uint32_t handledRecord = 0;
    uint32_t handledRecordCancel = 0;

    static_assert (std::atomic<int64_t>::is_always_lock_free);
    static_assert (std::atomic<double>::is_always_lock_free);
};

} // namespace vocalsynth
