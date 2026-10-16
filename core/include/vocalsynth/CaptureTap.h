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
};

/** Audio-thread side of the M3 capture (docs/M3-DESIGN.md 4). Turns what the plugin sends (VsEvents incl.
    flush note-offs), the dry mono input and the host transport into a stream of CaptureRecords plus a
    stream of mono audio frames, written into two SPSC rings that the capture worker drains.

    Per host block (binding call order, see docs/M3-DESIGN.md 4.1):
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

    Transport records are pushed only on change: first block, play state, bpm, time signature, valid flags,
    or a ppq / bar-start discontinuity (|reported - predicted| > ppqToleranceBeats, predicted from the
    previous record by bpm and elapsed samples while playing, unchanged while stopped). A bpm-only change
    within minTempoRecordIntervalSeconds of the previous transport record is held back and sent with the
    first block after that interval (tempo ramps with 1-sample blocks).

    Overflow: a record or audio frame that does not fit is dropped and counted (ring overflow counters);
    the next record that fits is preceded by a gap record. Audio frame + its audio record are pushed
    together or not at all (the worker places audio only through audio records).

    Real-time safe after prepare(): beginBlock, flushNotes, processChunk, processBypassed never allocate,
    lock, log or throw; bounded by the number of samples and events. request...() are lock-free (any
    thread). Not thread-safe otherwise: one producer thread (the audio thread). */
class CaptureTap
{
public:
    static constexpr std::size_t recordRingCapacity = 16384; ///< records (power of two, 512 KB)
    static constexpr double audioRingSeconds = 2.0;           ///< at least this much audio, rounded up to 2^n
    static constexpr int audioFrameSamples = 256;             ///< audio granularity (aligned to the clock)
    static constexpr double ppqToleranceBeats = 1.0e-6;
    static constexpr double minTempoRecordIntervalSeconds = 0.001;

    CaptureTap() = default;

    /** Message thread, while neither process...() nor the consumer runs (the plugin suspends the worker).
        Allocates the rings for this rate if they must grow (NOT real-time; may throw std::bad_alloc), keeps
        the clock if the rate is unchanged (else restarts it at 0) and arms a reset record for the next
        beginBlock(). Also forgets the open notes (the processor flushes after prepareToPlay anyway). */
    void prepare (double sampleRate, int maxBlockSize);

    //==============================================================================
    // Audio thread

    /** Start of a host block: pushes the pending reset record, a transport record if it changed, a settings
        record if they changed, the markers of pending requests, and on/off markers for captureEnabled,
        captureAudio and bypass transitions (captureOff: note-offs for open notes first). */
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
    bool pushRecord (const CaptureRecord& record) noexcept;
    void pushPendingGap() noexcept;
    void pushAudioFrame() noexcept;
    void trackEvent (const VsEvent& event) noexcept;

    SpscRing<CaptureRecord> records;
    SpscRing<float> audio;

    double sampleRate = 0.0;
    int maxBlockSize = 0;
    int64_t clock = 0;
    std::atomic<int64_t> publishedClock { 0 };

    bool resetPending = true;
    bool resetClearsHistory = true;

    // Change detection (audio thread)
    bool hasLastTransport = false;
    TransportInfo lastTransport {};     ///< quantised (as decoded from its record)
    int64_t lastTransportSample = 0;
    bool tempoChangeHeld = false;
    bool hasLastSettings = false;
    CaptureSettingsInfo lastSettings {};
    bool enabled = false;
    bool audioOn = false;
    bool bypassed = false;

    // Open notes for flushNotes() / captureOff
    std::array<std::bitset<128>, 16> openNotes {};
    std::bitset<16> bendNotCentred {};

    // Audio staging: one frame
    std::array<float, audioFrameSamples> frame {};
    int frameFill = 0;

    // Overflow -> gap record
    uint64_t pendingDroppedRecords = 0;
    uint64_t pendingDroppedAudio = 0;

    // UI requests (counters: the audio thread remembers the last value it handled)
    std::atomic<uint32_t> newTakeRequests { 0 };
    std::atomic<uint32_t> grabRequests { 0 };
    std::atomic<uint32_t> clearRequests { 0 };
    std::atomic<int> grabBars { 8 };
    std::atomic<double> grabSeconds { 30.0 };
    uint32_t handledNewTake = 0;
    uint32_t handledGrab = 0;
    uint32_t handledClear = 0;

    static_assert (std::atomic<int64_t>::is_always_lock_free);
    static_assert (std::atomic<double>::is_always_lock_free);
};

} // namespace vocalsynth
