#pragma once

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

/** The capture worker thread (docs/M3-DESIGN.md 5): drains the CaptureTap rings every drainIntervalMs into
    a CaptureHistory, runs the TakeSegmenter, keeps the take list (max maxTakes, newest first), renders the
    WAV cache of closed takes into the instance folder and touches its heartbeat file.

    Lifetime (binding): created with the processor; the thread starts in the first prepareToPlay (start())
    and runs until stop() in the processor's destructor (NOT in releaseResources: the history survives FL's
    engine restarts). prepareToPlay brackets CaptureTap::prepare() with suspend() / resume(): suspend()
    drains what is in the rings, parks the thread and returns when it is parked. stop() joins (pluginval
    opens and closes many instances: no thread before the first prepareToPlay, a quick, clean join).

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

    /** instanceFolder: CaptureExporter::getInstanceFolder (id) (created by start()). */
    CaptureWorker (CaptureTap& tap, juce::File instanceFolder);
    ~CaptureWorker();

    CaptureWorker (const CaptureWorker&) = delete;
    CaptureWorker& operator= (const CaptureWorker&) = delete;

    //==============================================================================
    // Lifecycle (message thread)

    void start();      ///< starts the thread if it is not running (allocates the history on the thread)
    void suspend();    ///< drains and parks; no-op if not running
    void resume();     ///< un-parks
    void stop();       ///< joins; idempotent
    bool isRunning() const noexcept;

    /** Take gap and audio look-back (applied on the worker thread at its next step). */
    void setOptions (double takeGapSeconds, double audioHistoryMinutes);

    /** Options that decide the cached WAV (round trip + format): takes whose cache was rendered with other
        values are re-rendered in the background while their audio is still in the history. */
    void setWavOptions (const RenderOptions& render, WavSampleFormat format);

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
    // Tests: one worker step on the calling thread (only while the thread is not running).

    void runStepForTests();

private:
    class Thread;

    void step();

    CaptureTap& tap;
    const juce::File instanceFolder;
    std::unique_ptr<Thread> thread;

    CaptureHistory history;
    TakeSegmenter segmenter;
    bool historyPrepared = false;

    mutable std::mutex listMutex;
    std::vector<std::shared_ptr<const Take>> takes;
    std::shared_ptr<const Take> openTakeSnapshot;
    std::atomic<uint32_t> takeListVersion { 0 };
};

} // namespace vocalsynth
