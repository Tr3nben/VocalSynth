#pragma once

#include "vocalsynth/BendGenerator.h"
#include "vocalsynth/Decimator.h"
#include "vocalsynth/Framer.h"
#include "vocalsynth/InputConditioner.h"
#include "vocalsynth/NoteSegmenter.h"
#include "vocalsynth/PitchDetector.h"
#include "vocalsynth/PitchPostProcessor.h"
#include "vocalsynth/VoiceSettings.h"
#include "vocalsynth/VsEvent.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace vocalsynth
{

/** What the UI shows (tuner, note, latency). Plain values, copied out of the pipeline's seqlock. */
struct VoiceAnalysisSnapshot
{
    float frequencyHz = 0.0f;           ///< cleaned f0 of the newest frame, 0 = unvoiced
    float centsOffset = 0.0f;           ///< cents from nearestNote, -50..+50 (0 when unvoiced)
    int nearestNote = -1;               ///< nearest chromatic MIDI note to frequencyHz (tuner), -1 if unvoiced
    int activeNote = -1;                ///< MIDI note now sounding (incl. octave shift), -1 if none
    float confidence = 0.0f;            ///< 0..1, cleaned confidence of the newest frame
    float rmsDb = PitchFrame::silenceDb; ///< input level (conditioned, analysis rate) for the meter / gate line
    bool voiced = false;                ///< voicing decision of the NoteSegmenter for the newest frame
    float latencyMs = 0.0f;             ///< getAlgorithmicLatencyMs() for the current range
    uint32_t frameCounter = 0;          ///< frames analysed since prepare() (wraps), lets the UI see "alive"
};

/** Single-writer seqlock for VoiceAnalysisSnapshot. publish(): audio thread only, wait-free.
    read(): any thread, lock-free (retries up to maxReadAttempts times if it races a publish; if every
    attempt raced, it returns one more unchecked copy, whose fields are each valid but may mix two
    publishes - never a default-constructed snapshot, so the UI does not flicker).
    All fields are relaxed atomics, so there is no data race in the C++ sense; the sequence counter +
    fences make the copy consistent. */
class VoiceSnapshotPublisher
{
public:
    void publish (const VoiceAnalysisSnapshot& s) noexcept
    {
        const auto seq = sequence.load (std::memory_order_relaxed);
        sequence.store (seq + 1, std::memory_order_relaxed); // odd = write in progress
        std::atomic_thread_fence (std::memory_order_release);

        frequencyHz.store (s.frequencyHz, std::memory_order_relaxed);
        centsOffset.store (s.centsOffset, std::memory_order_relaxed);
        nearestNote.store (s.nearestNote, std::memory_order_relaxed);
        activeNote.store (s.activeNote, std::memory_order_relaxed);
        confidence.store (s.confidence, std::memory_order_relaxed);
        rmsDb.store (s.rmsDb, std::memory_order_relaxed);
        voiced.store (s.voiced, std::memory_order_relaxed);
        latencyMs.store (s.latencyMs, std::memory_order_relaxed);
        frameCounter.store (s.frameCounter, std::memory_order_relaxed);

        sequence.store (seq + 2, std::memory_order_release);
    }

    VoiceAnalysisSnapshot read() const noexcept
    {
        VoiceAnalysisSnapshot s;

        for (int attempt = 0; attempt < maxReadAttempts; ++attempt)
        {
            const auto before = sequence.load (std::memory_order_acquire);

            if ((before & 1u) != 0)
                continue;

            loadFields (s);
            std::atomic_thread_fence (std::memory_order_acquire);

            if (sequence.load (std::memory_order_relaxed) == before)
                return s;
        }

        loadFields (s); // raced a publish every time: each value is valid, they may mix two frames
        return s;
    }

private:
    static constexpr int maxReadAttempts = 8;

    void loadFields (VoiceAnalysisSnapshot& s) const noexcept
    {
        s.frequencyHz = frequencyHz.load (std::memory_order_relaxed);
        s.centsOffset = centsOffset.load (std::memory_order_relaxed);
        s.nearestNote = nearestNote.load (std::memory_order_relaxed);
        s.activeNote = activeNote.load (std::memory_order_relaxed);
        s.confidence = confidence.load (std::memory_order_relaxed);
        s.rmsDb = rmsDb.load (std::memory_order_relaxed);
        s.voiced = voiced.load (std::memory_order_relaxed);
        s.latencyMs = latencyMs.load (std::memory_order_relaxed);
        s.frameCounter = frameCounter.load (std::memory_order_relaxed);
    }

    std::atomic<uint32_t> sequence { 0 };
    std::atomic<float> frequencyHz { 0.0f };
    std::atomic<float> centsOffset { 0.0f };
    std::atomic<int> nearestNote { -1 };
    std::atomic<int> activeNote { -1 };
    std::atomic<float> confidence { 0.0f };
    std::atomic<float> rmsDb { PitchFrame::silenceDb };
    std::atomic<bool> voiced { false };
    std::atomic<float> latencyMs { 0.0f };
    std::atomic<uint32_t> frameCounter { 0 };

    static_assert (std::atomic<float>::is_always_lock_free);
    static_assert (std::atomic<int>::is_always_lock_free);
    static_assert (std::atomic<uint32_t>::is_always_lock_free);
    static_assert (std::atomic<bool>::is_always_lock_free);
};

/** Per-frame debug/evaluation data, passed to an optional FrameObserver (tests, vs-eval; not the plugin). */
struct AnalysisFrameInfo
{
    int64_t hopSample = 0;  ///< absolute host-rate index (since reset) of the input sample that completed the hop
    PitchFrame raw;         ///< detector output
    PitchFrame processed;   ///< after median + octave guard
    int activeNote = -1;    ///< output note after this frame, -1 if none
    bool voiced = false;    ///< NoteSegmenter voicing decision for this frame
};

/** Voice -> MIDI: composes InputConditioner -> Decimator -> Framer -> PitchDetector (YIN/MPM) ->
    PitchPostProcessor -> NoteSegmenter (+ ScaleQuantizer, BendGenerator). docs/M2-DESIGN.md 2 and 3.10.

    This is THE interface the plugin uses (frozen for M2):

        prepareToPlay:  pipeline.prepare (sampleRate, maxBlock);          // allocates, calls reset()
        each chunk:     pipeline.process (mono, n, settings, events);    // VsEventBuffer<1024>, n <= 4096
        flush:          midiOutput.flushAllNotes (0, midi); pipeline.reset();
        UI timer:       pipeline.getSnapshot();                           // any thread

    Timing (binding): events carry the offset (within this call's numSamples) of the host input sample
    whose arrival completed the analysis hop that produced them, so their ABSOLUTE positions depend only on
    the input signal and the settings, never on the block split. A flush caused by a settings change is at
    offset 0. Offsets are non-decreasing and in [0, numSamples).

    Settings (lock-free): the caller passes the full VoiceSettings by value with every call (built from the
    APVTS raw atomics). The pipeline sanitises them and compares them with the previous call's settings:
    requiresFlush() changes (range, algorithm, key, scale, channel) end the active note at offset 0
    (note-off + bend centre), reset the PitchPostProcessor and reconfigure the window/detector; a new onset
    is then needed. All other changes take effect at the next frame. The very first call after reset()
    never flushes. A call with numSamples <= 0 does nothing (settings are not consumed).

    Note context: whenever a note ends (release, flush, settings flush), the PitchPostProcessor forgets its
    octave reference, so the next onset is never "corrected" into the octave of the previous note.

    Event capacity: one call emits at most maxEventsForSamples (numSamples) =
    maxEventsPerFlush + maxEventsPerHop * ceil (numSamples / getHopInputSamples()) events
    (< 1024 for numSamples <= 4096 at any rate >= 8 kHz). Events that do not fit are dropped.

    Real-time safe after prepare(): process(), flush(), reset() and getSnapshot() never allocate, lock or
    throw. Not thread-safe except getSnapshot() and computeAlgorithmicLatencyMs(). */
class VoicePipeline
{
public:
    static constexpr int maxEventsPerHop = NoteSegmenter::maxEventsPerFrame;
    static constexpr int maxEventsPerFlush = 2;

    /** Hops of extra delay counted by the latency budget, derived from the tuning constants so the
        formula can never go stale: frame-clock quantisation (1) + median delay (medianLength / 2 = 1)
        + extra onset-confirmation frames (onsetConfirmFrames - 1 = 0). */
    static constexpr int latencyBudgetHops =
        1 + PitchPostProcessor::defaultMedianLength / 2 + (NoteSegmenter::onsetConfirmFrames - 1);

    /** Worst-case onset -> note-on delay in ms for a host rate and range (pure function, any thread):
        1000 * ((numTaps - 1) / 2 + (D - 1)) / sampleRate
        + 1000 * InputConditioner::groupDelaySeconds (sampleRate, f0Min)
        + 1000 * (windowSamples + latencyBudgetHops * hopSamples) / analysisRate
        with D = Decimator::factorFor, numTaps = Decimator::numTapsFor (D), analysisRate = sampleRate / D,
        hop/window from Framer, f0Min of the range. 48 kHz: Tenor ~24.2 ms, Bass/Wide ~39.1 ms (table in
        docs/M2-DESIGN.md 3.11; spec budgets 30 / 40 ms). Returns 0 for sampleRate <= 0. */
    static double computeAlgorithmicLatencyMs (double sampleRate, VoiceRange range) noexcept;

    /** Spec budgets (docs/M2-SPEC.md): Tenor <= 30 ms, Bass <= 40 ms; Wide shares Bass's window. */
    static constexpr double tenorLatencyBudgetMs = 30.0;
    static constexpr double bassLatencyBudgetMs = 40.0;

    VoicePipeline();

    /** Allocates every buffer for the worst case at this rate (window for lowestF0MinHz, i.e. Bass/Wide;
        both detectors), so later range/algorithm changes never allocate. NOT real-time; may throw
        std::bad_alloc. maxBlockSize is only a hint (process() accepts any numSamples). Calls reset(). */
    void prepare (double sampleRate, int maxBlockSize);

    /** Clears all DSP state (filters, decimator, ring, median, segmenter, bend) WITHOUT emitting, restarts
        the absolute sample clock at 0 and forgets the previous settings (no flush on the next call).
        Publishes a default snapshot. Real-time safe. */
    void reset() noexcept;

    /** Analyses numSamples of mono input (nullptr = silence) and appends the resulting events. */
    template <std::size_t N>
    void process (const float* mono, int numSamples, const VoiceSettings& settings, VsEventBuffer<N>& out) noexcept
    {
        processToSink (mono, numSamples, settings, VsEventSink { out });
    }

    /** Ends the active note (note-off + bend centre if needed) at sampleOffset and returns to idle; the
        analysis keeps running. Emits nothing if no note is active. */
    template <std::size_t N>
    void flush (int sampleOffset, VsEventBuffer<N>& out) noexcept
    {
        flushToSink (sampleOffset, VsEventSink { out });
    }

    /** Non-template versions of process() and flush(), implemented in the .cpp. */
    void processToSink (const float* mono, int numSamples, const VoiceSettings& settings, VsEventSink out) noexcept;
    void flushToSink (int sampleOffset, VsEventSink out) noexcept;

    /** Lock-free copy of the newest analysis values. Published at the end of every process() call that
        analysed at least one frame (or flushed), and by reset(); a 1-sample host block therefore does
        not publish 48000 times a second. Any thread. */
    VoiceAnalysisSnapshot getSnapshot() const noexcept { return snapshot.read(); }

    /** computeAlgorithmicLatencyMs() for the prepared rate and the range of the last process() call
        (Tenor before the first call). Audio thread / after prepare(); the UI uses getSnapshot().latencyMs. */
    double getAlgorithmicLatencyMs() const noexcept;

    /** Upper bound of events one process() call of numSamples can emit (see class comment). */
    int maxEventsForSamples (int numSamples) const noexcept;

    /** Output note now sounding (incl. octave shift), -1 if none. Audio thread. */
    int getActiveNote() const noexcept { return segmenter.getActiveNote(); }

    double getSampleRate() const noexcept { return sampleRate; }
    double getAnalysisSampleRate() const noexcept { return decimator.getOutputSampleRate(); }
    int getDecimationFactor() const noexcept { return decimator.getFactor(); }
    int getHopSamples() const noexcept { return framer.getHopSamples(); }        ///< analysis rate
    int getHopInputSamples() const noexcept { return framer.getHopSamples() * decimator.getFactor(); }
    int getWindowSamples() const noexcept { return framer.getWindowSamples(); }  ///< analysis rate, current range

    /** Delay in input samples from the centre of the analysed window to the input sample that completed
        the hop: decimator FIR group delay + D * (windowSamples - 1) / 2. vs-eval subtracts it from
        AnalysisFrameInfo::hopSample to time-stamp frames for the pitch metrics. Current range. */
    double getFrameCentreDelayInputSamples() const noexcept;

    /** Absolute host samples processed since reset(). */
    int64_t getSamplePosition() const noexcept { return samplePosition; }

    /** Optional per-frame callback for tests and vs-eval (called on the processing thread, inside
        process(), once per hop, after the NoteSegmenter). Not used by the plugin. Set it before
        processing; nullptr disables it. */
    using FrameObserver = void (*) (void* context, const AnalysisFrameInfo& info) noexcept;
    void setFrameObserver (FrameObserver observer, void* context) noexcept;

private:
    void applySettings (const VoiceSettings& sanitised) noexcept;
    PitchDetector& currentDetector() noexcept;
    void publishSnapshot() noexcept;

    double sampleRate = 48000.0;

    InputConditioner conditioner;
    Decimator decimator;
    Framer framer;
    YinPitchDetector yin;
    MpmPitchDetector mpm;
    PitchPostProcessor postProcessor;
    NoteSegmenter segmenter;
    BendGenerator bend;

    VoiceSettings lastSettings {};
    bool hasLastSettings = false;
    int64_t samplePosition = 0;   ///< absolute host samples since reset()
    PitchFrame lastFrame {};      ///< newest cleaned frame (snapshot)
    uint32_t frameCounter = 0;

    FrameObserver frameObserver = nullptr;
    void* frameObserverContext = nullptr;

    VoiceSnapshotPublisher snapshot;
};

} // namespace vocalsynth
