#pragma once

#include "vocalsynth/CaptureOptions.h"
#include "vocalsynth/Take.h"

#include <cstdint>
#include <span>
#include <vector>

namespace vocalsynth
{

/** Kind of a rendered MIDI event. The enum order is the sort order at equal ticks (note-offs first). */
enum class TickEventType : uint8_t
{
    noteOff = 0,
    controlChange,
    pitchBend,
    noteOn
};

/** One MIDI channel event at a tick (JUCE-free). */
struct TickEvent
{
    int64_t tick = 0;
    TickEventType type = TickEventType::noteOn;
    uint8_t channel = 1; ///< 1..16
    uint8_t data1 = 0;   ///< note / CC number
    uint8_t data2 = 0;   ///< velocity / CC value (note-off: 0)
    int16_t bend = 0;    ///< -8192..8191

    constexpr bool operator== (const TickEvent&) const noexcept = default;
};

struct TickTempo
{
    int64_t tick = 0;
    double bpm = 120.0;

    constexpr bool operator== (const TickTempo&) const noexcept = default;
};

struct TickTimeSignature
{
    int64_t tick = 0;
    int numerator = 4;
    int denominator = 4;

    constexpr bool operator== (const TickTimeSignature&) const noexcept = default;
};

/** The dry-voice WAV that lines up with a rendered take: capture-clock samples [startSample,
    startSample + numSamples); WAV sample 0 = tick 0. */
struct AudioSpan
{
    int64_t startSample = 0;
    int64_t numSamples = 0;

    constexpr bool operator== (const AudioSpan&) const noexcept = default;
};

/** Result of TakeRenderer::render(): everything the SMF writer needs, plus the WAV span. */
struct RenderedTake
{
    int ticksPerQuarterNote = 960;
    std::vector<TickEvent> events;                ///< sorted (tick, then TickEventType order, then emit order)
    std::vector<TickTempo> tempos;                ///< first at tick 0; synced: the host tempo map; free: one
    std::vector<TickTimeSignature> timeSignatures; ///< first at tick 0; free: 4/4
    int bendRangeSemitones = 0;                   ///< > 0: bends exported, write RPN 0 with this range at tick 0
    int64_t endTick = 0;                          ///< end-of-track (>= every event tick)
    int64_t originBar = 0;                        ///< synced: FL bar number of tick 0 (file name); free: 0
    double originBeats = 0.0;                     ///< synced: unwrapped beats of tick 0
    AudioSpan audio;
    int noteCount = 0;                            ///< note-ons in events

    bool operator== (const RenderedTake&) const = default;
};

/** Take -> tick-stamped MIDI events (docs/M3-DESIGN.md 9). Pure, deterministic (same take + options ->
    identical result on every platform: integer ticks from double maths with llround, no platform
    functions in decisions), allocates. Message thread (export, UI preview) and worker thread (WAV span).

    Order (binding): compensation -> version -> quantize -> pairing / clamping.
    1. Compensation: each event's musical time is taken at its estimatedSample (refinedSample for Clean
       note-ons when set) minus roundTripSamples (synced) or minus nothing (free). Synced ticks:
       llround ((timeline.beatsAt (s) - originBeats) * ppq) with originBeats = the bar containing the first
       compensated note-on (TakeTimeline::barAt). Free ticks: llround ((s - s0) / sr * freeBpm / 60 * ppq)
       with s0 = first compensated note-on - freePreRollMs.
    2. Version: Raw = the events as sent. Clean (first stage): correction merge (EventTimingFlags::correction),
       ghost notes < max (cleanMinNoteMs, settings.minNoteLengthMs) removed, transition notes
       (< cleanTransitionMaxMs between two notes with a gap <= cleanTransitionMaxGapMs, pitch between its
       neighbours) merged into the following note, octave strays (< cleanOctaveStrayMaxMs, >=
       cleanOctaveStraySemitones from both neighbours) removed. Clean never moves a note by more than the
       merged span.
    3. Quantize (synced takes only, never free): note starts move strength * (nearest grid line - start);
       ends to the nearest grid line when quantizeEnds (length >= half a grid step); bends and CCs that
       belong to a note move with its start (clamped into the note).
    4. Pairing / clamping: every note-on has a note-off inside the take (at endTick at the latest), no
       overlapping notes of one pitch on one channel (the earlier one ends at the later start), minimum
       length 1 tick, ticks >= 0, bends reset to centre after the last note (bends exported only if
       bendsEnabled (options.bends, take.settings.bendMode); then bendRangeSemitones =
       take.settings.bendRangeSemitones), CCs only if options.controlChanges.
    endTick: synced = end of the last bar that contains an event (at least one bar); free = the last event
    tick (at least 1). */
class TakeRenderer
{
public:
    // Clean thresholds (initial values, docs/M3-SPEC.md 7; tuned with tests and user takes)
    static constexpr double cleanTransitionMaxMs = 120.0;
    static constexpr double cleanTransitionMaxGapMs = 15.0;
    static constexpr double cleanOctaveStrayMaxMs = 200.0;
    static constexpr int cleanOctaveStraySemitones = 11;
    /** refineNoteOnsets() searches [estimate - onsetRefineWindowMs, emit sample]. */
    static constexpr double onsetRefineWindowMs = 15.0;

    static RenderedTake render (const Take& take, const RenderOptions& options);

    /** The WAV span of render (take, options).audio without rendering the events (worker: WAV cache).
        Synced: [sample of originBeats + roundTrip, take.endSample + roundTrip); free: [s0, take.endSample). */
    static AudioSpan computeAudioSpan (const Take& take, const RenderOptions& options);

    /** Worker, after a take closed with audio: sets CapturedEvent::refinedSample of note-ons (measured or
        model timing) to the energy onset of the dry audio inside [estimate - onsetRefineWindowMs, emit],
        if a clear onset is there. audio holds the capture-clock samples [audioStartSample,
        audioStartSample + audio.size()). Clean uses it, Raw does not. */
    static void refineNoteOnsets (Take& take, std::span<const float> audio, int64_t audioStartSample);
};

} // namespace vocalsynth
