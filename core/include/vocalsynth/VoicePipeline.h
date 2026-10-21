#pragma once

#include "vocalsynth/BendGenerator.h"
#include "vocalsynth/Decimator.h"
#include "vocalsynth/EventTiming.h"
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
    int quantisedNote = -1;             ///< activeNote without the octave shift (same publish), -1 if none
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
        quantisedNote.store (s.quantisedNote, std::memory_order_relaxed);
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
        s.quantisedNote = quantisedNote.load (std::memory_order_relaxed);
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
    std::atomic<int> quantisedNote { -1 };
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
    maxEventsPerFlush + maxEventsPerHop * ceil (numSamples / getHopInputSamples())
    + NoteSegmenter::maxEventsPerOnsetFrame events (< 1024 for numSamples <= 4096 at any rate >= 8 kHz).
    Why the fast path adds only one onset: per hop interval (previous hop sample, hop sample] at most
    3 events, because a provisional onset (2) inside an interval makes the segmenter ignore the full
    frame at its end (provisionalIgnoreSamples = 1 hop), the fast path does not run while a note is on,
    and it cannot fire right after a note ends (not armed); a call can end with one more interval that
    holds only a fast onset. Events that do not fit are dropped.

    Real-time safe after prepare(): process(), flush(), reset() and getSnapshot() never allocate, lock or
    throw. Not thread-safe except getSnapshot() and computeAlgorithmicLatencyMs(). */
class VoicePipeline
{
public:
    static constexpr int maxEventsPerHop = NoteSegmenter::maxEventsPerFrame;
    static constexpr int maxEventsPerFlush = 2;

    /** Hops of extra delay counted by the latency budget of the CONFIRMED (full-window) onset path,
        derived from the tuning constants so the formula can never go stale: frame-clock quantisation (1)
        + median delay (medianLength / 2 = 1) + extra onset-confirmation frames (onsetConfirmFrames - 1 = 0). */
    static constexpr int latencyBudgetHops =
        1 + PitchPostProcessor::defaultMedianLength / 2 + (NoteSegmenter::onsetConfirmFrames - 1);

    /** Provisional onsets (docs/M2-DESIGN.md 11): while no note is on, the FastOnsetDetector runs every
        getFastHopSamples() = max (1, hop / fastHopDivisor) analysis samples (1 ms) on the Framer window. */
    static constexpr int fastHopDivisor = 2;

    /** A provisional note that the full window has not confirmed (strong frame) after
        windowSamples + provisionalTimeoutHops hops ends (false start) unless that frame is weak voiced
        or a strong frame came before. Settle time (NoteSegmenter::setOnsetTiming): windowSamples. */
    static constexpr int provisionalTimeoutHops = 3;

    /** Fast-path frames (of getFastHopSamples()) in the provisional latency model: frame-clock
        quantisation (1) + extra confirmation frames (fastOnsetConfirmFrames - 1) + 1 for the onset bias to
        settle within NoteSegmenter::fastMaxSpreadCents (the first confident frames read sharp while the
        lagged span is still partly silent). */
    static constexpr int fastLatencyBudgetHops = NoteSegmenter::fastOnsetConfirmFrames + 1;

    /** Worst-case onset -> note-on delay in ms for a host rate and range (pure function, any thread):
        the faster of the provisional (fast) and the confirmed (full-window) onset path,
        min (computeFastOnsetLatencyMs(), computeConfirmedLatencyMs()). For a clear voiced onset the
        fast path fires. This is the worst case (a tone at f0 min); 48 kHz: Tenor 23.2 ms (v0.2.0: 24.2),
        Bass/Wide 38.0 ms (39.1); higher tones are much faster, see computeFastOnsetLatencyMs (sr, range, f0)
        (table in docs/M2-DESIGN.md 11). Returns 0 for sampleRate <= 0. */
    static double computeAlgorithmicLatencyMs (double sampleRate, VoiceRange range) noexcept;

    /** Model of the provisional path for a tone at f0Hz (<= 0 or below the range: f0 min, the worst case):
        1000 * ((numTaps - 1) / 2 + (D - 1)) / sampleRate + 1000 * InputConditioner::groupDelaySeconds (sampleRate, f)
        + 1000 * (FastOnsetDetector::requiredSamplesFor (fa, f0Min, f0Max, ceil (fa / f) + 1) + fastLatencyBudgetHops * fastHop) / fa
        with f = max (f0Hz, f0Min). The provisional path needs about two periods of the tone, so the
        latency falls with the pitch: 48 kHz Tenor 23.2 ms at A2 (f0 min), 12.4 ms at B3. */
    static double computeFastOnsetLatencyMs (double sampleRate, VoiceRange range, double f0Hz = 0.0) noexcept;

    /** Model of the confirmed (full-window) path, the v0.2.0 formula (docs/M2-DESIGN.md 3.11):
        1000 * ((numTaps - 1) / 2 + (D - 1)) / sampleRate
        + 1000 * InputConditioner::groupDelaySeconds (sampleRate, f0Min)
        + 1000 * (windowSamples + latencyBudgetHops * hopSamples) / analysisRate
        with D = Decimator::factorFor, numTaps = Decimator::numTapsFor (D), analysisRate = sampleRate / D,
        hop/window from Framer, f0Min of the range. 48 kHz: Tenor ~24.2 ms, Bass/Wide ~39.1 ms. This is
        the onset latency when the fast path does not fire (weak or noisy onsets). */
    static double computeConfirmedLatencyMs (double sampleRate, VoiceRange range) noexcept;

    /** Fast-path frame period in analysis samples for an analysis rate: max (1, hop / fastHopDivisor). */
    static int fastHopSamplesFor (double analysisSampleRate) noexcept;

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

    /** M3 capture side output (docs/M3-DESIGN.md 7.1): the same events as process (mono, n, settings, out)
        (identical with and without a timing buffer); for every event that out accepted, one VsEventTiming
        is pushed to timing, in the same order. A timing buffer with fewer entries than events (e.g. the
        test pattern, which has none) means kind none for the missing ones (CaptureTap). */
    template <std::size_t N>
    void process (const float* mono, int numSamples, const VoiceSettings& settings, VsEventBuffer<N>& out,
                  VsEventTimingBuffer<N>& timing) noexcept
    {
        processToSink (mono, numSamples, settings, VsEventSink { out }, VsEventTimingSink { timing });
    }

    /** flush() with timings (kind none, delay 0), see process() above. */
    template <std::size_t N>
    void flush (int sampleOffset, VsEventBuffer<N>& out, VsEventTimingBuffer<N>& timing) noexcept
    {
        flushToSink (sampleOffset, VsEventSink { out }, VsEventTimingSink { timing });
    }

    /** Non-template versions with timings (docs/M3-DESIGN.md 7). The overloads without a timing argument
        forward here with a disconnected sink. With a connected sink the segmenter's events of each call
        go through a member scratch buffer and are forwarded to out in the same order; each accepted
        event gets the timing NoteSegmenter::getLastEventTiming() gives for its type (a bend right
        before a note-on: the note-on's; right after a note-off: the note-off's; other bends: update).
        Settings flushes and flushToSink(): kind none, delay 0. Never changes the events. */
    void processToSink (const float* mono, int numSamples, const VoiceSettings& settings, VsEventSink out,
                        VsEventTimingSink timing) noexcept;
    void flushToSink (int sampleOffset, VsEventSink out, VsEventTimingSink timing) noexcept;

    /** Level envelope for the onset / release estimates of the timing side output: one-pole power
        smoother (time constant in ms) on the conditioned input at the host rate. Its upward crossings
        of gateThresholdDb - NoteSegmenter::gateHysteresisDb and downward crossings of that level minus
        levelReleaseHysteresisDb (a Schmitt trigger) are passed to the NoteSegmenter
        (setLevelOnsetSample / setLevelReleaseSample; the release corrected for the envelope's decay:
        max (newest rise of the envelope, crossing - tau * ln (envelope at that rise / level))). A release
        counts only once the envelope stayed below the on level for one period of the range's f0 min
        (reported then); a rise before that is the same sound (the power of a low tone dips to ~0 twice per
        period), so it gives no new onset.
        Per sample and only on absolute samples, so block-size invariant. */
    static constexpr double onsetEnvelopeTimeConstantMs = 1.0;
    static constexpr double levelReleaseHysteresisDb = 6.0;
    /** A level onset counts for a note start up to the confirmed-path model delay + this many hops
        before the note-on (NoteSegmenter::DetectionDelayModel::onsetSearchSamples). */
    static constexpr int onsetSearchExtraHops = 4;

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
    int getFastHopSamples() const noexcept { return fastHopSamples; }             ///< analysis rate
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
    /** Runs one segmenter call: straight into out without a timing sink, else through the scratch buffer
        (forwardWithTimings). */
    template <typename Call>
    void runSegmenter (VsEventSink out, VsEventTimingSink timing, Call&& call) noexcept
    {
        if (! timing.isConnected())
        {
            call (out);
            return;
        }

        segmenterScratch.clear();
        call (VsEventSink { segmenterScratch });
        forwardWithTimings (out, timing);
    }
    void forwardWithTimings (VsEventSink out, VsEventTimingSink timing) noexcept;
    void updateLevelEnvelope (float conditionedSample, int64_t absoluteSample) noexcept;
    static int64_t fastOnsetDelayFor (const void* pipeline, double f0Hz) noexcept;
    PitchDetector& currentDetector() noexcept;
    void publishSnapshot() noexcept;

    double sampleRate = 48000.0;

    InputConditioner conditioner;
    Decimator decimator;
    Framer framer;
    YinPitchDetector yin;
    MpmPitchDetector mpm;
    FastOnsetDetector fastDetector;
    int fastHopSamples = 16;
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

    // M3 timing side output
    VsEventBuffer<NoteSegmenter::maxEventsPerFrame> segmenterScratch;
    VoiceRange timingRange = VoiceRange::tenor; ///< range of the delay model (applySettings)
    double levelCoefficient = 1.0;
    double levelEnvelope = 0.0;
    double levelOnPower = 0.0;
    double levelOffPower = 0.0;
    /** silent -> above (onset) -> falling (below the off level) -> silent after levelHoldSamples below the
        on level (release); falling -> above again without an onset (the power of a low tone dips to ~0
        twice per period: one tone, one onset). */
    enum class LevelState : uint8_t
    {
        silent,
        above,
        falling
    };
    LevelState levelState = LevelState::silent;
    int64_t levelHoldSamples = 1;       ///< one period of the range's f0 min
    int64_t levelFallingSamples = 0;
    int64_t levelPendingRelease = 0;
    double levelLastRise = 0.0;
    int64_t levelLastRiseSample = 0;

    VoiceSnapshotPublisher snapshot;
};

} // namespace vocalsynth
