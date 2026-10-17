#pragma once

#include "vocalsynth/CaptureDrain.h"
#include "vocalsynth/CaptureHistory.h"
#include "vocalsynth/CaptureOptions.h"
#include "vocalsynth/CaptureRecord.h"
#include "vocalsynth/CaptureTap.h"
#include "vocalsynth/Take.h"
#include "vocalsynth/TakeSegmenter.h"
#include "vocalsynth/WavWriter.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace vocalsynth
{

/** TransportInfo of the current block from the host playhead (audio thread; no allocation: reads
    juce::AudioPlayHead::PositionInfo fields). nullptr or no position: the default (nothing valid). */
TransportInfo makeTransportInfo (juce::AudioPlayHead* playHead) noexcept;

/** What the Capture tab shows in its status line (docs/M3-DESIGN.md 14). Plain value. */
struct CaptureStatus
{
    TakeSegmenter::State state = TakeSegmenter::State::off;
    double bpm = 0.0;                 ///< 0 = unknown
    int timeSigNumerator = 4;
    int timeSigDenominator = 4;
    int recordingTakeNumber = 0;      ///< 0 = none
    int64_t recordingBar = 0;         ///< synced: current bar (FL convention), 0 = none
    CaptureTap::OverflowCounts overflow;
    std::size_t audioHistoryBytes = 0;
};

/** Cached dry-voice WAV of a take (docs/M3-DESIGN.md 12). */
struct TakeWavState
{
    enum class Kind
    {
        unavailable, ///< no audio for this take
        preparing,   ///< the worker is rendering it
        ready,       ///< file = the WAV in the instance cache
        persisted    ///< file = the WAV in the takes folder (already dragged / saved)
    };

    Kind kind = Kind::unavailable;
    juce::File file;
};

/** Options of the worker that come from CaptureState (non-parameter state). Plain value. */
struct CaptureWorkerOptions
{
    double takeGapSeconds = TakeSegmenter::defaultTakeGapSeconds;
    double audioHistoryMinutes = CaptureHistory::defaultAudioMinutes;
    double freePreRollMs = RenderOptions {}.freePreRollMs; ///< decides the WAV span of free takes
    WavSampleFormat wavFormat = WavSampleFormat::pcm24;

    bool operator== (const CaptureWorkerOptions&) const = default;
};

/** Where the worker reads what it needs from the processor without locks (all optional: tests leave them
    empty and get the defaults). */
struct CaptureWorkerSources
{
    /** Raw APVTS atomics of captureOffsetMode / captureOffsetMs / captureFineOffsetMs: the cached WAV of a
        take follows the round trip (TakeRenderer::roundTripFor), read at every step. */
    const std::atomic<float>* offsetMode = nullptr;
    const std::atomic<float>* offsetMs = nullptr;
    const std::atomic<float>* fineOffsetMs = nullptr;
    /** RecentBlockSize::get() of the processor (lock-free), stored in Take::observedBlockSize. */
    std::function<int()> observedBlockSize;

    CaptureOffsetSettings readOffset() const noexcept;
};

/** The capture worker thread (docs/M3-DESIGN.md 5): drains the CaptureTap rings every drainIntervalMs into
    a CaptureHistory (CaptureDrain), runs the TakeSegmenter, keeps the take list (newest first, at most
    maxTakes and maxTotalTakeEvents events in all: the oldest takes go first), refines note onsets on the
    dry audio, renders the WAV cache of closed takes into the instance folder and touches its heartbeat.

    Lifetime (binding): created with the processor (declared after the CaptureTap member, so it is
    destroyed first); the thread starts in the first prepareToPlay (start()) and runs until stop() in the
    processor's destructor (NOT in releaseResources: the history survives FL's engine restarts).
    prepareToPlay brackets CaptureTap::prepare() with suspend() / resume(): suspend() drains what is in the
    rings, parks the thread and returns when it is parked. stop() = signal, notify, wait for the thread
    without a timeout kill (juce::Thread::stopThread kills on timeout); it returns within one step because
    every step is bounded (drain + at most wavFramesPerStep WAV frames + bookkeeping, a few ms), an
    unfinished WAV job is aborted (temp file deleted). pluginval opens and closes many instances: no thread
    and no files before the first prepareToPlay; the instance folder is created only when the first file is
    written (WAV cache), the heartbeat is touched only while the folder exists.

    Each step: drain (CaptureDrain), segmenter.closeIdle() when the tap clock did not move for
    takeGapSeconds of wall time (the host stopped calling processBlock), closed takes -> wallClockStartMs,
    observedBlockSize, audio availability, refineNoteOnsets, overview -> publish; a clear marker empties the
    take list (cached WAVs of non-persisted takes deleted); WAV job slice; open-take snapshot; heartbeat.

    Threads: the worker thread owns the history and the segmenter. The take list, the open-take snapshot and
    the WAV states are guarded by a mutex that the audio thread never touches; the message thread reads them
    through getTakes() / getWavState() (copies of shared_ptr<const Take>, cheap). */
class CaptureWorker
{
public:
    static constexpr int drainIntervalMs = 10;
    static constexpr int heartbeatIntervalSeconds = 60;
    static constexpr int openTakeSnapshotIntervalMs = 500;
    static constexpr std::size_t maxTakes = 32;
    /** Memory bound of the take list: ~48 MiB of CapturedEvent (each take owns its events). */
    static constexpr std::size_t maxTotalTakeEvents = std::size_t { 1 } << 20;
    static constexpr int wavFramesPerStep = 1 << 16;

    /** instanceFolder: CaptureExporter::getInstanceFolder (id) (created lazily, see class comment). */
    CaptureWorker (CaptureTap& tap, juce::File instanceFolder, CaptureWorkerSources sources = {});
    ~CaptureWorker();

    CaptureWorker (const CaptureWorker&) = delete;
    CaptureWorker& operator= (const CaptureWorker&) = delete;

    //==============================================================================
    // Lifecycle (message thread)

    void start();      ///< starts the thread if it is not running (allocates the history on the thread)
    void suspend();    ///< drains and parks; no-op if not running
    void resume();     ///< un-parks
    void stop();       ///< joins (see class comment); idempotent
    bool isRunning() const noexcept;

    /** Tests: call before start(). start() then prepares the history on the calling thread and starts no
        thread; runStepForTests() runs the steps (suspend / resume drain / do nothing). */
    void setManualSteppingForTests (bool manual) noexcept;

    /** Any thread (setStateInformation may run on a background thread, pluginval): applied at the next
        step. */
    void setOptions (const CaptureWorkerOptions& options);

    //==============================================================================
    // Message thread

    /** Increments whenever the take list, the open take snapshot or a WAV state changed. */
    uint32_t getTakeListVersion() const noexcept { return takeListVersion.load (std::memory_order_acquire); }

    /** Closed takes, newest first. */
    std::vector<std::shared_ptr<const Take>> getTakes() const;

    /** Snapshot of the take being recorded (at most every openTakeSnapshotIntervalMs), nullptr if none. */
    std::shared_ptr<const Take> getOpenTake() const;

    TakeWavState getWavState (int takeNumber) const;

    /** The WAV of a take was moved into the takes folder (CaptureExporter::persistWav). */
    void markWavPersisted (int takeNumber, const juce::File& file);

    CaptureStatus getStatus() const noexcept;

    const juce::File& getInstanceFolder() const noexcept { return instanceFolder; }

    //==============================================================================
    // Tests: one worker step on the calling thread (manual stepping, or while the thread is not running).

    void runStepForTests();

private:
    class Thread;

    void step();

    CaptureTap& tap;
    const juce::File instanceFolder;
    const CaptureWorkerSources sources;
    std::unique_ptr<Thread> thread;
    bool manualStepping = false;

    CaptureHistory history;
    TakeSegmenter segmenter;
    CaptureDrain drainer;
    bool historyPrepared = false;

    mutable std::mutex optionsMutex;
    CaptureWorkerOptions pendingOptions;

    mutable std::mutex listMutex;
    std::vector<std::shared_ptr<const Take>> takes;
    std::shared_ptr<const Take> openTakeSnapshot;
    std::atomic<uint32_t> takeListVersion { 0 };
};

} // namespace vocalsynth
