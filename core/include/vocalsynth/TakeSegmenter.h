#pragma once

#include "vocalsynth/CaptureHistory.h"
#include "vocalsynth/CaptureMode.h"
#include "vocalsynth/CaptureOptions.h"
#include "vocalsynth/CaptureRecord.h"
#include "vocalsynth/Take.h"

#include <array>
#include <bitset>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace vocalsynth
{

/** Decides take boundaries from the record stream and builds the takes (docs/M3-DESIGN.md 5.2). Pure C++,
    capture worker thread only; allocates (take event vectors).

    Rules (binding, docs/M3-SPEC.md 4). "Playing" = TakeTimeline's definition (isPlaying and hasPpq).
    1. Synced (transport playing): a take starts at the transport start record and ends at the stop
       record. Loop wraps / position jumps while playing never split it (the timeline unwraps the beats).
    2. Free (stopped or no playhead): a take starts at the first note-on and ends takeGapSeconds after the
       last note-off without any sounding note (end sample = last note-off + freeTailSeconds, at most the
       gap end), or at a transport start (which starts a synced take at that sample). After a synced take
       ended, a free take can only start with a note-on emitted after that take's capture tail (earlier
       note-ons were sung before the stop and belong to the synced take).
    3. Marker newTake: closes the current take at the marker sample and starts the next one there (synced
       if playing, else a free take, kind manual). Notes held across the marker are closed / re-opened at
       the marker in compensated time (see the capture tail below; a free take: synthetic note-off at the
       marker in the old take, synthetic note-on at the marker in the new one).
    4. A reset record (prepare) closes the current take (at the reset's sample on the old clock = the newest
       sample seen; no capture tail: nothing more arrives on the old clock); with clearsHistory also all
       state and notes are forgotten.
    5. Takes without any note-on are discarded. A take that starts while notes sound opens them at the take
       start (synthetic note-ons with the original velocity).
    6. Takes that reach maxTakeEvents are closed and continued in a new take (manual-split rules).
    Marker clear: discards the open take, closing takes and pending grabs (nothing is emitted); the next
    take starts by the rules at the marker (synced at once if playing, sounding notes opened). Marker
    captureOff closes the take; captureOn behaves like a transport start / free wait. Gap records mark the
    open (and closing) takes incomplete; at a gap every sounding note ends (synthetic note-off at the gap
    sample) and the tap's resync note-ons that follow re-open the notes that are really still on.
    Note-offs of notes that are not sounding are ignored for the note state (the take keeps them; the
    renderer drops unpaired note-offs).

    Capture tail (synced takes and grabs): a take's span [startSample, endSample) is in host time, but what
    was sung just before endSample reaches the plugin up to a round trip + detection delay later. A synced
    take therefore keeps collecting events until captureEndSample = endSample +
    compensationTailSamples (sampleRate, maxBlockSize) ("closing"), in parallel with the next take, and
    is appended to closedTakes only then (notes still sounding get a synthetic note-off at
    captureEndSample). A grab is built once the records up to its marker + tail were processed. A reset
    record or Clear ends the tail at once. Free takes have no tail (no round trip; their end already lies
    freeTailSeconds after the last note-off).

    The open take collects events, notes, settings and (synced) its timeline segments as records arrive;
    closed takes get the take number (counting from 1 per instance, never reused, also not after Clear),
    assigned when the take opens.

    M3.1 take length and Record mode (docs/M3.1-DESIGN.md 3, 4; settings captureMode / takeMaxBars travel in
    the settings record, Record / Cancel as markers, so everything is timed by the audio clock):
    7. Synced background takes are anchored at their first note that lasts anchorMinNoteSamples() (the Clean
       ghost threshold: a click or noise note never anchors): anchor bar X = the bar (TakeTimeline::barAt)
       containing max (startSample, h), h = estimatedSample - rtSeg of that note's note-on (h >= startSample;
       a bar line at most anchorEarlyToleranceBeats after h counts: a downbeat sung a little early starts
       its bar), decided at its note-off (h of the note-off - h >= the threshold, else it is no candidate)
       or once it is still on at its emit sample + threshold + detectionTailSeconds; or startSample if a
       note sounding at the start (synthetic note-on, or a note-on with h < startSample) has a note-off with
       h >= startSample + threshold or is still on at startSample + the capture tail + threshold (it decides
       before a later note). rtSeg = computeRoundTripSamples (setOffsetSettings(), maxBlockSize, rate), 0 for
       test-pattern takes. Anchor bar X is also set with limit Off (status line only; maxBars stays 0).
       A take opened with a limit (synced or free) that is never anchored and has no note of at least the
       threshold inside its span (only clicks / noise, or a note that ended at the cut) is discarded like an
       empty take (its number is released).
       With takeMaxBars N > 0 at that moment (Take::maxBars = N, Take::anchorBar = X): startSample = max
       (startSample, start of bar X), and the take is cut at the N-th bar line after the start of bar X
       (TakeTimeline::barLineAfter, counted as the timeline crosses them) like a newTake marker there: the
       next synced take opens at the cut and anchors by the same rule (a note held across the cut: at the
       cut, i.e. bar X + N; otherwise at the bar of its own first note). Stop ends it earlier (rule 1).
    8. Free background takes are cut at A + freeModeBarsToSamples (N, rate, freeBpm) like a free manual split
       (synthetic note-offs), A = the emit sample of the first note-on that lasts the threshold (rule 7, in
       emit time; startSample for a note held across the start of a continued take); sounding notes continue
       in a new free take, else the segmenter waits for the next note-on. The gap rule stays.
    9. Record mode: no background takes (no synced / free takes, newTake ignored; state syncedIdle while
       playing). recordArm (N = grabBars) arms; the Record take (kind record) starts at the first bar line at
       or after the marker (TakeTimeline::firstBarLineAtOrAfter on the history timeline; stopped: where
       playback starts; a jump during the count-in recomputes it) and ends by itself at the N-th bar line
       after its start (capture tail as rule 1), then disarms. Stop ends it there (kept); Stop during the
       count-in keeps it armed. recordCancel disarms, or discards the Record take being recorded.
       Background -> Record closes an open background take at the switch; Record -> Background drops the
       arm, ends a Record take there (kept) and resumes the background rules (playing: a synced take opens).
       Grabs work in both modes.
    Bar lines, cuts and the Record start are taken at sample C once a record or advanceTo() comes after C
    (every record up to C is in by then), so the result never depends on the drain pattern. An anchored take
    may hold events emitted before its (moved) startSample; the renderer's host-time clamp drops them. */
class TakeSegmenter
{
public:
    static constexpr double defaultTakeGapSeconds = 3.0;
    static constexpr double minTakeGapSeconds = 0.5;
    static constexpr double maxTakeGapSeconds = 30.0;
    static constexpr double freeTailSeconds = 0.5;         ///< audio after the last note-off of a free take
    /** Detection delay allowance of the capture tail: worst note-on delay (Bass ~40 ms) and release
        detection, with margin. */
    static constexpr double detectionTailSeconds = 0.25;
    static constexpr std::size_t maxTakeEvents = CaptureHistory::eventCapacity;
    /** Rule 7: a bar line at most this far after the anchoring note-on counts as its bar (1/32 note: sung
        anticipation and onset-estimate error of a downbeat; TakeTimeline::barSnapToleranceBeats is 1/32 beat). */
    static constexpr double anchorEarlyToleranceBeats = 1.0 / 8.0;
    /** Rule 7: shorter notes never anchor; = the default Clean ghost threshold (RenderOptions::cleanMinNoteMs). */
    static constexpr double anchorMinNoteMs = 80.0;

    /** max (anchorMinNoteMs, take.settings.minNoteLengthMs) in samples of take.sampleRate. */
    static int64_t anchorMinNoteSamples (const Take& take) noexcept;

    /** maxRoundTripSamples (maxBlockSize, sampleRate) + detectionTailSeconds, in samples. */
    static int64_t compensationTailSamples (double sampleRate, int maxBlockSize) noexcept;

    static constexpr std::array<int, 6> grabBarChoices { 1, 2, 4, 8, 16, 32 };
    static constexpr int defaultGrabBars = 8;
    static constexpr std::array<int, 5> grabSecondChoices { 10, 20, 30, 60, 120 };
    static constexpr int defaultGrabSeconds = 30;

    enum class State : uint8_t
    {
        off,           ///< captureEnabled off (or before the first reset record)
        freeWaiting,   ///< stopped, no take open
        freeRecording, ///< stopped, a free take is open
        synced,        ///< playing, a synced take is open (background, or a Record take being recorded)
        syncedIdle     ///< playing, no take open (Record mode: not recording)
    };

    /** Forgets the open take and all note / transport state (the take numbering continues). */
    void reset();

    /** Clamped to [minTakeGapSeconds, maxTakeGapSeconds]; applies from the next note-off on. */
    void setTakeGapSeconds (double seconds) noexcept;
    double getTakeGapSeconds() const noexcept { return takeGapSeconds; }

    /** Capture offset used for the anchor of synced takes (rule 7). The worker sets it before each drain;
        default: CaptureOffsetSettings {} (Auto). */
    void setOffsetSettings (const CaptureOffsetSettings& offset) noexcept { offsetSettings = offset; }

    /** Record-mode state at the newest sample (updated by process() and advanceTo()); O(1). */
    RecordStatus getRecordStatus() const noexcept { return recordStatus; }

    /** Mode and take limit of the newest settings record. */
    CaptureMode getCaptureMode() const noexcept { return settings.captureMode; }
    int getTakeMaxBars() const noexcept { return settings.takeMaxBars; }

    /** Bar k (1 .. maxBars) of the open take at the newest sample, 0 if it is not anchored or has no limit. */
    int getOpenTakeBar() const noexcept;

    /** Feeds one record in stream order, AFTER history.addRecord (record) (grabs read the history).
        Takes that close are appended to closedTakes (closed = true, oldest first). */
    void process (const CaptureRecord& record, const CaptureHistory& history, std::vector<Take>& closedTakes);

    /** The worker has every record before sample (CaptureTap::getClock()): closes a free take whose gap
        elapsed, finishes closing takes and pending grabs whose tail is complete, updates the open take's
        endSample / captureEndSample. Results never depend on how often it is called. */
    void advanceTo (int64_t sample, const CaptureHistory& history, std::vector<Take>& closedTakes);

    /** The host stopped calling processBlock (the worker saw no clock progress for takeGapSeconds of wall
        time): closes an open free take if no note sounds (end = min (last note-off + freeTailSeconds,
        newest sample)). Not deterministic in the audio-clock sense; nothing else uses it. */
    void closeIdle (const CaptureHistory& history, std::vector<Take>& closedTakes);

    State getState() const noexcept { return state; }

    /** The take being recorded, nullptr if none (its endSample = the newest processed sample). */
    const Take* getOpenTake() const noexcept;

    /** Number the next take will get. */
    int getNextTakeNumber() const noexcept { return nextTakeNumber; }

    /** Builds a grab take ending at markerSample (pure; called by advanceTo() once the history holds every
        record before captureEnd = markerSample + compensationTailSamples (synced) or + detectionTailSeconds
        (free)):
        - synced (the timeline is playing at markerSample): span from the start of the bar grabBars - 1 bars
          before the bar containing markerSample, up to markerSample;
        - free: the last grabSeconds;
        the start is clamped to the oldest stored record; events of [start, captureEnd); notes sounding at
        the start are opened (synthetic), notes sounding at captureEnd are closed there (synthetic). nullopt
        if the span has no note-on. */
    static std::optional<Take> makeGrab (const CaptureHistory& history, int64_t markerSample,
                                         const MarkerRecord& marker, int number);

    /** Number of clear markers processed since construction (not reset by reset()): the worker empties its
        take list when this changes. */
    uint32_t getClearCount() const noexcept { return clearCount; }

    /** Events a take may still receive after it was split for reaching the limit (its capture tail and the
        synthetic note-offs): the open take is split at maxTakeEvents - splitReserveEvents. */
    static constexpr std::size_t splitReserveEvents = 8192;

private:
    /** A take being built, with the notes it has open (for synthetic note-offs at its end). */
    struct Builder
    {
        Take take;
        std::array<std::bitset<128>, 16> notes {};
        int64_t collectFrom = 0;  ///< events emitted from here on belong to it (startSample may move later)
        // M3.1 take length (rules 7 - 9)
        bool anchored = true;     ///< synced background takes and free takes with a limit start unanchored
        bool discardUnanchored = false; ///< opened with a limit: discarded if never anchored (rule 7)
        std::array<std::bitset<128>, 16> anchorCandidates {}; ///< notes sounding at startSample (rule 7)
        int64_t anchorDeadline = 0; ///< candidates still on here anchor at startSample
        /** Note-ons that anchor once they last anchorMinNoteSamples (rule 7), in arrival order. */
        struct AnchorNote
        {
            int64_t at = 0;       ///< anchor position: synced h, free emit sample
            int64_t on = 0;       ///< start for the length test (synced: max (h, startSample); free: estimate)
            int64_t deadline = 0; ///< still on at this emit sample: long enough
            uint8_t channel = 0;  ///< index 0..15
            uint8_t note = 0;
        };
        static constexpr std::size_t maxAnchorNotes = 8;
        std::array<AnchorNote, maxAnchorNotes> anchorNotes {};
        std::size_t numAnchorNotes = 0;
        double lineSample = 0.0;  ///< newest bar line counted (start of bar X at first), fractional sample
        int barsDone = 0;         ///< bar lines crossed after the start of bar X
        int64_t freeCutSample = std::numeric_limits<int64_t>::max(); ///< free take: cut here (rule 8)
    };

    enum class RecordPhase : uint8_t
    {
        idle,
        armed,
        recording
    };

    struct PendingGrab
    {
        int64_t markerSample = 0;
        int64_t captureEndSample = 0;
        MarkerRecord marker;
        int number = 0;
    };

    void handleReset (const CaptureRecord& record, const CaptureHistory& history, std::vector<Take>& closedTakes);
    void handleTransport (int64_t sample, const TransportInfo& info, const CaptureHistory& history,
                          std::vector<Take>& closedTakes);
    void handleEvent (int64_t sample, const EventRecord& event, const CaptureHistory& history,
                      std::vector<Take>& closedTakes);
    void handleMarker (int64_t sample, const MarkerRecord& marker, const CaptureHistory& history,
                       std::vector<Take>& closedTakes);
    void handleGap (int64_t sample);
    void handleSettings (int64_t sample, const CaptureSettingsInfo& info, const CaptureHistory& history,
                         std::vector<Take>& closedTakes);
    void handleRecordMarker (int64_t sample, const MarkerRecord& marker);
    bool backgroundMode() const noexcept { return settings.captureMode == CaptureMode::background; }
    /** Opens the synced take at sample (background) or goes to syncedIdle (Record mode). */
    void startSyncedState (int64_t sample, const CaptureHistory& history);
    void trackAnchor (const CapturedEvent& event);
    /** fromNote: at is a note-on's h (the early tolerance applies), else the take start. */
    void anchorOpenTake (int64_t at, bool fromNote);
    /** Rule 7, after a take closed unanchored: a note of at least the threshold inside its kept span. */
    bool hasAnchorLengthNote (const Take& take) const;
    int64_t segmentRoundTrip (const Take& take) const noexcept;
    void updateRecordStatus (const CaptureHistory& history) noexcept;

    /** Handles everything due at or before sample (closing tails, pending grabs, the free gap) in time order. */
    void advanceInternal (int64_t sample, const CaptureHistory& history, std::vector<Take>& closedTakes);
    void openTakeAt (int64_t sample, TakeMode mode, TakeKind kind, const CaptureHistory& history);
    void splitAt (int64_t sample, TakeKind kind, const CaptureHistory& history, std::vector<Take>& closedTakes);
    /** Ends the open take at endSample: synced with tail -> closing; otherwise finished at once. */
    void endOpenTake (int64_t endSample, bool withTail, std::vector<Take>& closedTakes);
    void endAllTailsAt (int64_t sample, const CaptureHistory& history, std::vector<Take>& closedTakes);
    void finish (Builder&& builder, std::vector<Take>& closedTakes);
    void appendEvent (const CapturedEvent& event, const CaptureHistory& history, std::vector<Take>& closedTakes);
    void updateOpenSpan() noexcept;
    void releaseNumber (int number) noexcept;
    int64_t tailSamples() const noexcept;
    int64_t gapSamples() const noexcept;

    static std::optional<Take> buildGrab (const CaptureHistory& history, int64_t markerSample,
                                          const MarkerRecord& marker, int number, int64_t captureEndLimit);
    static int64_t grabCaptureEnd (const CaptureHistory& history, int64_t markerSample) noexcept;

    State state = State::off;
    double takeGapSeconds = defaultTakeGapSeconds;
    int nextTakeNumber = 1;
    uint32_t clearCount = 0;
    bool hasOpenTake = false;
    Builder openTake;
    std::vector<Builder> closingTakes; ///< synced takes in their capture tail (usually 0 or 1)
    std::vector<PendingGrab> pendingGrabs;
    int64_t freeStartAllowedFrom = 0;  ///< no free take starts with a note-on before this (synced tail)
    CaptureOffsetSettings offsetSettings {};

    // Record mode (rule 9)
    RecordPhase recordPhase = RecordPhase::idle;
    int recordBars = 0;
    int64_t recordSearchFrom = 0;      ///< the Record take starts at the first bar line at or after this
    RecordStatus recordStatus;

    // Stream state
    double sampleRate = 48000.0;
    int maxBlockSize = 0;
    bool playing = false;
    double hostBpm = TakeTimeline::fallbackBpm;
    CaptureSettingsInfo settings {};
    std::array<std::bitset<128>, 16> soundingNotes {};
    std::array<std::array<uint8_t, 128>, 16> soundingVelocity {};
    int numSounding = 0;
    int64_t lastNoteOffSample = 0;
    int64_t gapEndSample = 0;          ///< free take: closes here if no note sounds (set at the last note-off)
    int64_t newestSample = 0;
};

} // namespace vocalsynth
