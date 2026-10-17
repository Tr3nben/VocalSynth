#pragma once

#include "vocalsynth/CaptureHistory.h"
#include "vocalsynth/CaptureRecord.h"
#include "vocalsynth/Take.h"

#include <array>
#include <bitset>
#include <cstdint>
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
    assigned when the take opens. */
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
        synced         ///< playing, a synced take is open
    };

    /** Forgets the open take and all note / transport state (the take numbering continues). */
    void reset();

    /** Clamped to [minTakeGapSeconds, maxTakeGapSeconds]; applies from the next note-off on. */
    void setTakeGapSeconds (double seconds) noexcept;
    double getTakeGapSeconds() const noexcept { return takeGapSeconds; }

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
