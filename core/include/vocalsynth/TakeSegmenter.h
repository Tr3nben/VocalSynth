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

    Rules (binding, docs/M3-SPEC.md 4):
    1. Synced (transport playing): a take starts at the transport start record and ends at the stop
       record. Loop wraps / position jumps while playing never split it (the timeline unwraps the beats).
    2. Free (stopped or no playhead): a take starts at the first note-on and ends takeGapSeconds after the
       last note-off without any sounding note (end sample = last note-off + freeTailSeconds, at most the
       gap end), or at a transport start (which starts a synced take at that sample).
    3. Marker newTake: closes the current take at the marker sample and starts the next one there (synced
       if playing, else a free take, kind manual). Notes held across the marker get a synthetic note-off at
       the marker in the old take and a synthetic note-on at the marker in the new one.
    4. A reset record (prepare) closes the current take (at the reset's sample on the old clock = the newest
       sample seen); with clearsHistory also all state and notes are forgotten.
    5. Takes without any note-on are discarded. A take that starts while notes sound opens them at the take
       start (synthetic note-ons with the original velocity).
    6. Takes that reach maxTakeEvents are closed and continued in a new take (manual-split rules).
    Marker clear: discards the open take (nothing is emitted); the next take starts by the rules at the
    marker (synced at once if playing, sounding notes opened). Marker captureOff closes the take; captureOn
    behaves like a transport start / free wait. Gap records mark the open take incomplete.
    Grab marker: makeGrab() from the history, appended to the closed takes like any other (kind grab),
    independent of the open take (which continues).

    The open take collects events, notes and settings as records arrive; closed takes get a timeline slice
    and the take number (counting from 1 per instance, never reused, also not after Clear). */
class TakeSegmenter
{
public:
    static constexpr double defaultTakeGapSeconds = 3.0;
    static constexpr double minTakeGapSeconds = 0.5;
    static constexpr double maxTakeGapSeconds = 30.0;
    static constexpr double freeTailSeconds = 0.5;         ///< audio after the last note-off of a free take
    static constexpr double timelineMarginSeconds = 1.0;   ///< timeline slice beyond the take (compensation)
    static constexpr std::size_t maxTakeEvents = CaptureHistory::eventCapacity;

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
        elapsed and updates the open take's endSample. */
    void advanceTo (int64_t sample, const CaptureHistory& history, std::vector<Take>& closedTakes);

    State getState() const noexcept { return state; }

    /** The take being recorded, nullptr if none (its endSample = the newest processed sample). */
    const Take* getOpenTake() const noexcept;

    /** Number the next take will get. */
    int getNextTakeNumber() const noexcept { return nextTakeNumber; }

    /** Builds a grab take ending at markerSample (pure; used by process()):
        - synced (the timeline is playing at markerSample): from the start of the bar grabBars - 1 bars before
          the bar containing markerSample, up to markerSample;
        - free: the last grabSeconds;
        clamped to the oldest stored record; notes sounding at the start are opened, notes sounding at the
        end are closed at markerSample (synthetic). nullopt if the span has no note-on. */
    static std::optional<Take> makeGrab (const CaptureHistory& history, int64_t markerSample,
                                         const MarkerRecord& marker, int number);

private:
    void closeTake (int64_t endSample, const CaptureHistory& history, std::vector<Take>& closedTakes);

    State state = State::off;
    double takeGapSeconds = defaultTakeGapSeconds;
    int nextTakeNumber = 1;
    bool hasOpenTake = false;
    Take openTake;

    // Stream state
    double sampleRate = 48000.0;
    int maxBlockSize = 0;
    bool playing = false;
    CaptureSettingsInfo settings {};
    std::array<std::bitset<128>, 16> soundingNotes {};
    std::array<std::array<uint8_t, 128>, 16> soundingVelocity {};
    int64_t lastNoteOffSample = 0;
    int64_t newestSample = 0;
};

} // namespace vocalsynth
