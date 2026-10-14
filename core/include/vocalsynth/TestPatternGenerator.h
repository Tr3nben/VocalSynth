#pragma once

#include "vocalsynth/VsEvent.h"

#include <cstddef>
#include <cstdint>

namespace vocalsynth
{

/** Test pattern selection. The numeric values match the "testPattern" choice parameter index. */
enum class TestPattern
{
    off = 0,
    notes,
    pitchBend,
    controlChange,
    all
};

struct TestPatternSettings
{
    TestPattern pattern = TestPattern::off;
    int channel = 1;     ///< MIDI channel 1..16 (clamped by the generator)
    double bpm = 120.0;  ///< 60..200; changes take effect smoothly, without a flush
    int ccNumber = 1;    ///< 1..119 (clamped by the generator)

    /** Compares ALL fields including bpm. Do not use it to decide on a flush: only pattern, channel
        and ccNumber changes flush + restart; a bpm change never does. */
    bool operator== (const TestPatternSettings&) const noexcept = default;
};

/** Deterministic MIDI test pattern for the M1 FL Studio routing spike (no voice analysis).

    Free-running from an internal sample counter (NOT the host transport). Event positions are a
    function of the absolute sample position only, so any block-size split produces identical
    events, and all durations scale with the sample rate. See docs/M1-DESIGN.md, section 3.2.

    Patterns:
    - notes:          C major scale 60 62 64 65 67 69 71 72 repeating, one note per eighth note,
                      gate 75 %, velocity 100.
    - pitchBend:      note 60 held, bend = triangle over 2 beats covering -8192..8191, emitted at most
                      every sampleRate/200 samples and only when the value changed.
    - controlChange:  note 60 held, CC <ccNumber> = triangle 0..127 over 4 beats, only on change.
    - all:            repeating cycle 4 bars notes, 2 bars pitchBend, 2 bars controlChange (4/4).

    The held note of pitchBend/controlChange is re-struck once per bar (note-off + note-on 60 at the
    same sample, every heldNoteRetriggerBeats from the pattern/segment start), so a synth that is
    routed or loaded while the pattern runs starts sounding within one bar (docs/M1-DESIGN.md 3.2).

    When pattern, channel or ccNumber change between calls: flush at offset 0 and restart the pattern
    from its beginning. off emits nothing after that flush.

    Real-time safe: no allocation, no locks. Not thread-safe; audio thread only. */
class TestPatternGenerator
{
public:
    static constexpr int scaleNotes[8] = { 60, 62, 64, 65, 67, 69, 71, 72 };
    static constexpr int heldNote = 60;
    static constexpr uint8_t velocity = 100;
    static constexpr double gateFraction = 0.75;
    static constexpr double bendUpdatesPerSecond = 200.0;
    static constexpr double beatsPerBar = 4.0;              // 4/4
    static constexpr double heldNoteRetriggerBeats = 4.0;   // one bar
    static constexpr double minBpm = 20.0;                  // input clamp (the parameter is 60..200)
    static constexpr double maxBpm = 400.0;

    /** Must be called before process(); may be called repeatedly with different rates. Calls reset(). */
    void prepare (double sampleRate) noexcept;

    /** Restarts from sample 0 and forgets the active note WITHOUT emitting anything.
        Call flush() first if a note might be sounding downstream. */
    void reset() noexcept;

    /** Appends the events of the next numSamples samples to out (offsets in [0, numSamples)). */
    template <std::size_t N>
    void process (int numSamples, const TestPatternSettings& settings, VsEventBuffer<N>& out) noexcept
    {
        processToSink (numSamples, settings, VsEventSink { out });
    }

    /** Emits a note-off for the active note (if any) and a pitch-bend centre (only if the last
        emitted bend was not 0) at sampleOffset. Emits nothing if nothing is active. Does not restart. */
    template <std::size_t N>
    void flush (int sampleOffset, VsEventBuffer<N>& out) noexcept
    {
        flushToSink (sampleOffset, VsEventSink { out });
    }

    /** Non-template versions of process() and flush(), implemented in the .cpp. */
    void processToSink (int numSamples, const TestPatternSettings& settings, VsEventSink out) noexcept;
    void flushToSink (int sampleOffset, VsEventSink out) noexcept;

    /** The note currently held by the pattern, or -1 if none (for the UI, via an atomic in the processor). */
    int currentNote() const noexcept { return activeNote; }

private:
    /** Kind of the segment currently running ('all' switches between them, the others use one). */
    enum class SegmentType
    {
        notes,
        pitchBend,
        controlChange
    };

    static constexpr double allCycleBeats = 32.0; ///< 16 beats notes, 8 beats pitch bend, 8 beats CC

    void restart (const TestPatternSettings& sanitised) noexcept;
    void startSegment (int64_t segmentIndex) noexcept;
    void processSample (int64_t t, int offset, VsEventSink out) noexcept;
    double beatAt (int64_t t) const noexcept;

    double sampleRate = 48000.0;
    double samplesPerMinute = 60.0 * 48000.0;
    int64_t samplePosition = 0; ///< absolute samples since reset()

    // Last (sanitised) settings seen, to detect changes that require a flush + restart
    TestPatternSettings lastSettings {};
    bool hasLastSettings = false;

    // Beat clock: beat(t) = (t - anchorSample) * bpm / samplesPerMinute + anchorBeat, where beat 0 is
    // the pattern start. Re-anchored at the block start on every bpm change.
    int64_t anchorSample = 0;
    double anchorBeat = 0.0;
    double bpm = 120.0;

    // Pattern output state
    uint8_t channel = 1;        ///< channel of the running pattern (and of the active note / bend)
    uint8_t ccNumber = 1;
    int activeNote = -1;        ///< note number of the sounding note, -1 if none
    uint8_t activeChannel = 1;  ///< channel the active note was sent on
    int lastBend = 0;           ///< last emitted bend value (0 = centre)
    int lastCcValue = -1;       ///< last emitted CC value, -1 = none yet

    // Segment state (pattern beats; segmentEndBeat is infinite unless the pattern is 'all')
    bool isAllPattern = false;
    SegmentType segmentType = SegmentType::notes;
    int64_t segmentIndex = 0;
    double segmentStartBeat = 0.0;
    double segmentEndBeat = 0.0;
    bool segmentBegun = false;      ///< start events of the segment have been emitted
    int64_t segmentStartSample = 0; ///< first sample of the segment (bend grid origin)

    int64_t nextNoteIndex = 0;      ///< notes: index k of the next eighth note (segment relative)
    double noteOffBeat = 0.0;       ///< notes: pattern beat of the active note's note-off
    double nextRestrikeBeat = 0.0;  ///< held patterns: pattern beat of the next re-strike
    int64_t bendGridSamples = 240;  ///< max (1, lround (sampleRate / 200))
    int64_t nextGridSample = 0;     ///< next bend evaluation sample
};

} // namespace vocalsynth
