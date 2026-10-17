#pragma once

#include "CaptureExporter.h"
#include "vocalsynth/CaptureDrain.h"
#include "vocalsynth/CaptureHistory.h"
#include "vocalsynth/CaptureOptions.h"
#include "vocalsynth/CaptureRecord.h"
#include "vocalsynth/CaptureTap.h"
#include "vocalsynth/Take.h"
#include "vocalsynth/TakeRenderer.h"
#include "vocalsynth/TakeSegmenter.h"
#include "vocalsynth/WavWriter.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace vocalsynth
{

/** Exact comparisons of option structs without -Wfloat-equal (the core structs' defaulted operator==
    compares doubles; std::equal_to does the same, the plugin code just must not spell it). */
inline bool sameRenderOptions (const RenderOptions& a, const RenderOptions& b) noexcept
{
    const std::equal_to<double> eq;
    return a.version == b.version && a.quantize == b.quantize && eq (a.quantizeStrengthPercent, b.quantizeStrengthPercent)
           && a.quantizeEnds == b.quantizeEnds && a.bends == b.bends && a.controlChanges == b.controlChanges
           && a.ticksPerQuarterNote == b.ticksPerQuarterNote && eq (a.cleanMinNoteMs, b.cleanMinNoteMs)
           && eq (a.freePreRollMs, b.freePreRollMs) && a.roundTripSamples == b.roundTripSamples;
}

inline bool sameOffset (const CaptureOffsetSettings& a, const CaptureOffsetSettings& b) noexcept
{
    const std::equal_to<double> eq;
    return a.mode == b.mode && eq (a.offsetMs, b.offsetMs) && eq (a.fineOffsetMs, b.fineOffsetMs);
}

/** TransportInfo of the current block from the host playhead (audio thread; no allocation: reads
    juce::AudioPlayHead::PositionInfo fields). nullptr or no position: the default (nothing valid). */
TransportInfo makeTransportInfo (juce::AudioPlayHead* playHead) noexcept;

/** What the Capture tab shows in its status line (docs/M3-DESIGN.md 14). Plain value. */
struct CaptureStatus
{
    bool started = false;             ///< the worker runs (first prepareToPlay seen)
    TakeSegmenter::State state = TakeSegmenter::State::off;
    double bpm = 0.0;                 ///< 0 = unknown
    int timeSigNumerator = 4;
    int timeSigDenominator = 4;
    int recordingTakeNumber = 0;      ///< 0 = none
    int64_t recordingBar = 0;         ///< synced: current bar (FL convention), 0 = none
    CaptureTap::OverflowCounts overflow;
    std::size_t audioHistoryBytes = 0; ///< RAM of the audio look-back (grows over the first minutes)
    std::size_t takeBytes = 0;         ///< RAM of the listed takes (events, overviews)
    int numTakes = 0;
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
    rings, parks the thread and returns when it is parked. stop() = signal, notify, join (std::thread: no
    timeout kill); it returns within one step because every step is bounded (drain + at most
    wavFramesPerStep WAV frames + bookkeeping, a few ms), an unfinished WAV job is aborted (temp file
    deleted). pluginval opens and closes many instances: no thread and no files before the first
    prepareToPlay; the instance folder is created only when the first file is written (WAV cache), the
    heartbeat is touched only while the folder exists.

    Each step: options, drain (CaptureDrain), segmenter.closeIdle() when the tap clock did not move for
    takeGapSeconds of wall time (the host stopped calling processBlock), closed takes -> wallClockStartMs,
    observedBlockSize, refineNoteOnsets, audio availability, overview -> publish; a clear marker empties the
    take list (cached WAVs of non-persisted takes deleted); WAV re-render check (offset parameters, pre-roll,
    format); WAV job slice; open-take snapshot; status; heartbeat.

    Threads: the worker thread owns the history and the segmenter. The take list, the open-take snapshot,
    the WAV states and the status are guarded by listMutex, which the audio thread never touches; the
    message thread reads them through getTakes() / getWavState() / getStatus() (copies of
    shared_ptr<const Take>, cheap). Cached WAV files are moved (persistWav) or deleted / replaced (worker)
    only under fileMutex (never taken while listMutex is held). */
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
    void suspend();    ///< drains and parks; no-op if not started
    void resume();     ///< un-parks
    void stop();       ///< joins (see class comment); idempotent
    bool isRunning() const noexcept;                    ///< the thread exists
    bool isStarted() const noexcept { return started.load (std::memory_order_acquire); } ///< start() was called

    /** Tests: call before start(). start() then prepares the history on the calling thread and starts no
        thread; runStepForTests() runs the steps, suspend() runs one step (the drain) on the calling thread,
        resume() does nothing, and the open-take snapshot is taken at every step (no 500 ms throttle). */
    void setManualSteppingForTests (bool manual) noexcept;

    /** Any thread (setStateInformation may run on a background thread, pluginval): applied at the next
        step. */
    void setOptions (const CaptureWorkerOptions& options);
    CaptureWorkerOptions getOptions() const;

    //==============================================================================
    // Message thread

    /** Increments whenever the take list, the open take snapshot or a WAV state changed. */
    uint32_t getTakeListVersion() const noexcept { return takeListVersion.load (std::memory_order_acquire); }

    /** Closed takes, newest first. */
    std::vector<std::shared_ptr<const Take>> getTakes() const;

    /** The listed take with this number, nullptr if it is not (or no longer) listed. */
    std::shared_ptr<const Take> getTake (int takeNumber) const;

    /** Snapshot of the take being recorded (at most every openTakeSnapshotIntervalMs), nullptr if none.
        Without its events, timeline and overview (the UI shows number, span, note count). */
    std::shared_ptr<const Take> getOpenTake() const;

    TakeWavState getWavState (int takeNumber) const;

    /** Moves the cached WAV of a take into takesFolder (CaptureExporter::persistWav) and remembers it, so a
        second drag reuses the persisted file. Returns the persisted file, or juce::File() if the WAV is
        not ready (preparing / unavailable) or the move failed. Message thread (file I/O). */
    juce::File persistWav (int takeNumber, const juce::File& takesFolder);

    CaptureStatus getStatus() const;

    const juce::File& getInstanceFolder() const noexcept { return instanceFolder; }

    //==============================================================================
    // Tests: one worker step on the calling thread (manual stepping, or while the thread is not running).

    void runStepForTests();

private:
    struct Entry
    {
        std::shared_ptr<const Take> take;
        TakeWavState wav;
        AudioSpan wavSpan;            ///< span of the cached / rendering WAV
        WavSampleFormat wavFormat = WavSampleFormat::pcm24;
        bool wavQueued = false;
    };

    struct WavJob
    {
        int takeNumber = 0;
        std::shared_ptr<const Take> take;
        AudioSpan span;
        WavSampleFormat format = WavSampleFormat::pcm24;
        CaptureExporter::WavFileWriter writer;
        bool truncated = false;
    };

    void threadLoop();
    void prepareHistory();
    void step();
    void safeStep() noexcept;

    RenderOptions wavRenderOptions (const Take& take) const;
    void processClosedTake (Take&& take, int64_t clock);
    void refineOnsets (Take& take) const;
    TakeAudio audioAvailability (const AudioSpan& span) const noexcept;
    void computeOverview (Take& take, const AudioSpan& span) const;
    void publish (std::shared_ptr<const Take> take, const AudioSpan& span);
    void clearTakeList();
    void checkWavRenders();
    void runWavJob();
    void finishWavJob();
    void abortWavJob();
    void deleteCachedWav (const TakeWavState& state);
    void updateOpenTakeSnapshot (bool force);
    void updateStatus (int64_t clock);
    void bumpVersion() noexcept { takeListVersion.fetch_add (1, std::memory_order_acq_rel); }

    CaptureTap& tap;
    const juce::File instanceFolder;
    const CaptureWorkerSources sources;

    // Thread control
    std::thread thread;
    std::mutex controlMutex;
    std::condition_variable controlCv;
    bool stopRequested = false;
    bool suspendRequested = false;
    bool parked = false;
    std::atomic<bool> threadRunning { false };
    bool manualStepping = false;
    std::atomic<bool> started { false };

    // Worker thread only
    CaptureHistory history;
    TakeSegmenter segmenter;
    CaptureDrain drainer;
    bool historyPrepared = false;
    std::vector<Take> closedScratch;
    CaptureWorkerOptions appliedOptions;
    CaptureOffsetSettings appliedOffset;
    bool rendersDirty = false;
    uint32_t seenClearCount = 0;
    int64_t lastClock = -1;
    double lastClockChangeMs = 0.0;
    bool idleClosed = false;
    double lastSnapshotMs = 0.0;
    double lastHeartbeatMs = 0.0;
    int snapshotNumber = 0;
    std::size_t snapshotEvents = 0;
    int64_t snapshotEnd = 0;
    std::deque<int> wavQueue;
    std::unique_ptr<WavJob> wavJob;
    std::vector<float> wavBuffer;

    mutable std::mutex optionsMutex;
    CaptureWorkerOptions pendingOptions;

    mutable std::mutex listMutex;
    std::vector<Entry> entries; ///< newest first
    std::shared_ptr<const Take> openTakeSnapshot;
    CaptureStatus status;
    std::atomic<uint32_t> takeListVersion { 0 };

    std::mutex fileMutex;
};

} // namespace vocalsynth
