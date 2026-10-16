#pragma once

#include "vocalsynth/BendGenerator.h"
#include "vocalsynth/PitchDetector.h"
#include "vocalsynth/PitchPostProcessor.h"
#include "vocalsynth/ScaleQuantizer.h"
#include "vocalsynth/VoiceSettings.h"
#include "vocalsynth/VsEvent.h"

#include <cstdint>

namespace vocalsynth
{

/** Stickiness parameters derived from the 0..100 % control (docs/M2-DESIGN.md 3.8.2). */
struct StickinessParams
{
    double hysteresisSemitones = 0.2; ///< h: a new note must win by 2h, i.e. be >= 0.5 + h st away (chromatic)
    double minChangeMs = 35.0;        ///< T_min: the change condition must hold this long
};

/** Turns cleaned pitch frames into note-on/off events (+ bend via BendGenerator)
    (docs/M2-DESIGN.md 3.8). Called once per hop by the VoicePipeline.

    Frame classification (p = frequencyToMidiNote (frequencyHz)):
    - strong voiced: frequencyHz > 0, confidence >= confidenceOn, rmsDb >= gateThresholdDb
    - weak voiced:   frequencyHz > 0, confidence >= confidenceOff, rmsDb >= gateThresholdDb - gateHysteresisDb

    States and transitions (all times in HOST samples, compared on absoluteSample):
    - idle: onsetConfirmFrames consecutive strong-voiced frames -> note-on at the last of them. Default 1:
      the median of 3 before this stage already needs 2 of the 3 newest detector frames voiced with
      confidence >= confidenceOn, which is the confirmation; every extra frame costs one hop (2 ms) of
      onset latency and the Bass budget (40 ms) has no room for it (docs/M2-DESIGN.md 3.11). Tune noise
      and breath rejection with confidenceOn / the gate first.
      q = quantizer.quantize (p, -1); output note = q + 12 * octaveShift (not played if outside 0..127).
      Velocity = velocityFor (max rmsDb of the confirming frames, gate, sensitivity).
    - note on, any frame: if octaveShift changed so that q + 12 * shift is outside 0..127, an immediate
      note-off (+ bend centre), no minNoteLength.
    - note on, weak-voiced frame:
      * octaveShift changed so that q + 12 * shift != output note: immediate note change (same q, no T_min,
        no minNoteLength). (On a not-weak frame the change waits for the next weak one: a note that is
        being released is not retriggered.)
      * candidate n = quantizer.quantize (p, q). Change condition: n != q, confidence >= confidenceOn
        (a weak frame below confidenceOn only holds the note: detector octave errors live there, and
        the octave guard does not check such frames) and
        |p - q| - |p - n| >= 2 h (chromatic neighbours: |p - q| >= 0.5 + h). The condition must hold on
        consecutive frames for >= T_min (timer starts at the first frame where it holds, restarts when it
        fails; n may differ between those frames), AND the current note must have lasted
        >= minNoteLength. Then change to quantizer.quantize (p, q) of this frame. A frame whose candidate
        wins but whose shifted output note would be outside 0..127 counts as not weak-voiced.
      * otherwise bend.update (100 * (p - q)) if the frame's pitch is reliable: confidence >= confidenceOn
        and rmsDb >= noteLevelDb - bendHoldDropDb, where noteLevelDb follows the note's level (set at
        the note start, rises at once, falls by levelFallDbPerSecond on absolute samples). Weak or
        decaying frames hold the note but freeze the bend: when the sound ends, the analysis window
        reaches past it and the pitch drifts (YIN up to +75 cents in the last frames before the
        note-off), which in True mode was a bend spike right before every release.
    - note on, not weak-voiced: after releaseFrames consecutive such frames AND a note age
      >= minNoteLength -> note-off; back to idle. Until then the note is held (no bend updates).
    minNoteLength never delays a note-on: it only bounds note duration (deferred note-off and change).
    Short blips never start a note because of the onset confirmation + gate + confidence.

    Provisional onsets (v0.2.1, docs/M2-DESIGN.md 11): processOnsetFrame() takes frames of the
    FastOnsetDetector (short integration, newest samples), which see a tone several ms before the
    full-window frames do. While idle and armed, fastOnsetConfirmFrames consecutive fast frames with
    confidence >= fastConfidenceOn, rmsDb >= gate and pitches within fastMaxSpreadCents of each other
    start a PROVISIONAL note at the last of them (q = quantize (p, -1), velocity from the max rmsDb of
    those frames, same event order as a note start). Armed = the fast path saw a frame that was not
    fast-strong since the last note start (or reset): after a note (also a failed provisional one) the
    fast path needs a gap in the voice first, so it can never re-trigger in a loop where the full path
    disagrees. While provisional, the full-window frames (processFrame):
    - within provisionalIgnoreSamples of the start are ignored (keeps the event bound, see
      VoicePipeline::maxEventsForSamples);
    - the first strong frame confirms the note: if its candidate quantize (p, q) wins by 2 h (the usual
      change condition without T_min and minNoteLength), the note is corrected at once (change with the
      legato/retrigger order); otherwise the note continues as a normal one, with this frame;
    - other frames hold the note (no release counting, no bend updates) until provisionalTimeoutSamples
      after the start; then the note becomes normal if the frame is weak voiced, else it ends at once
      (false start; minNoteLength does not apply). After a false start the fast path stays off until
      a frame below gateThresholdDb - gateHysteresisDb (a real gap): where the full window never
      agrees (e.g. a tone whose 2 f0 lies above the range and whose 2nd harmonic dominates, which YIN
      rejects as out of range), alternating fast frames would otherwise re-arm it every timeout.

    Event order at one sample (binding):
    - note start:            bend.noteStarted, note-on
    - retrigger change:      note-off (old), bend.noteStarted (new q), note-on (new)     [legato == false]
    - legato change:         bend.noteStarted (new q), note-on (new), note-off (old)     [legato == true]
    - note end / flush:      note-off, bend.noteEnded (centre if needed)
    At most 3 events per frame. Note-on velocity of a change = velocityFor (rmsDb of that frame, ...).

    Private members may change freely (owner B); the public interface is frozen for M2.
    Real-time safe, no allocation. Audio thread only. */
class NoteSegmenter
{
public:
    static constexpr float confidenceOn = 0.80f;
    static constexpr float confidenceOff = 0.60f;
    static constexpr float gateHysteresisDb = 3.0f;
    static constexpr int onsetConfirmFrames = 1;
    static constexpr int releaseFrames = 3;
    static constexpr double velocityRangeDb = 36.0; ///< onset RMS from gate to gate + 36 dB -> full range
    static constexpr int minFullVelocity = 20;      ///< velocity at the gate with 100 % sensitivity
    static constexpr int fixedVelocity = 100;       ///< velocity with 0 % sensitivity
    static constexpr int maxEventsPerFrame = 3;
    static constexpr float bendHoldDropDb = 3.0f;          ///< see "reliable" above
    static constexpr double levelFallDbPerSecond = 20.0;   ///< level follower for bendHoldDropDb

    // Provisional onsets from FastOnsetDetector frames (see class comment)
    static constexpr float fastConfidenceOn = 0.95f;
    static constexpr int fastOnsetConfirmFrames = 2;
    static constexpr double fastMaxSpreadCents = 20.0;  ///< also absorbs the onset bias (FastOnsetDetector)
    static constexpr int maxEventsPerOnsetFrame = 2;       ///< bend + note-on

    // Thresholds of other modules that are defined relative to these (kept in sync at compile time)
    static_assert (YinPitchDetector::fallbackMaxConfidence >= confidenceOff
                   && YinPitchDetector::fallbackMaxConfidence < confidenceOn);
    static_assert (PitchPostProcessor::inconsistentMaxConfidence >= confidenceOff
                   && PitchPostProcessor::inconsistentMaxConfidence < confidenceOn);
    static_assert (PitchPostProcessor::guardMinConfidence >= confidenceOn  // == without -Wfloat-equal
                   && PitchPostProcessor::guardMinConfidence <= confidenceOn);

    /** h = 0.4 * u semitones, T_min = 10 + 50 * u ms with u = clamp (percent, 0, 100) / 100.
        0 % -> (0, 10 ms), 50 % (default) -> (0.2, 35 ms), 100 % -> (0.4, 60 ms). */
    static StickinessParams stickinessFromPercent (double percent) noexcept;

    /** s = sensitivity / 100, x = clamp ((onsetRmsDb - gateDb) / velocityRangeDb, 0, 1),
        full = minFullVelocity + (127 - minFullVelocity) * x,
        velocity = clamp (lround ((1 - s) * fixedVelocity + s * full), 1, 127). */
    static uint8_t velocityFor (double onsetRmsDb, double gateThresholdDb, double sensitivityPercent) noexcept;

    /** Host sample rate (ms -> samples). Calls reset(). */
    void prepare (double sampleRate) noexcept;

    /** Back to idle WITHOUT emitting (the caller flushes downstream if needed). */
    void reset() noexcept;

    /** Per block, before the frames of that block. Takes the sanitised settings; key/scale go to the
        internal ScaleQuantizer. Never emits and never flushes (the pipeline decides on flushes). */
    void setSettings (const VoiceSettings& settings) noexcept;

    /** One cleaned frame. absoluteSample = host-rate index (since reset) of the input sample that
        completed the hop; sampleOffset = its offset in the current host block (events use it). */
    void processFrame (const PitchFrame& frame, int64_t absoluteSample, int sampleOffset, BendGenerator& bend,
                       VsEventSink out) noexcept;

    /** Provisional-note timing in host samples (VoicePipeline: ignore = 1 hop, timeout = window + a few
        hops of the current range). Never emits. Values < 0 are treated as 0. */
    void setOnsetTiming (int64_t provisionalIgnoreSamples, int64_t provisionalTimeoutSamples) noexcept;

    /** One FastOnsetDetector frame (only used while idle, see class comment). Same arguments as
        processFrame(). Emits at most maxEventsPerOnsetFrame events. */
    void processOnsetFrame (const PitchFrame& frame, int64_t absoluteSample, int sampleOffset, BendGenerator& bend,
                            VsEventSink out) noexcept;

    /** True while the active note was started by processOnsetFrame() and not yet confirmed. */
    bool isProvisional() const noexcept { return provisional; }

    /** Ends the active note (note-off + bend.noteEnded) at sampleOffset and goes idle. Emits nothing if
        no note is active. */
    void flush (int64_t absoluteSample, int sampleOffset, BendGenerator& bend, VsEventSink out) noexcept;

    /** Output note now sounding (incl. octave shift), -1 if none. */
    int getActiveNote() const noexcept { return activeOutputNote; }

    /** Quantised note before the octave shift, -1 if none. */
    int getQuantisedNote() const noexcept { return activeQuantisedNote; }

    /** Voicing decision of the last frame (strong-voiced while idle, weak-voiced while a note is on). */
    bool isFrameVoiced() const noexcept { return lastFrameVoiced; }

    const ScaleQuantizer& getQuantizer() const noexcept { return quantizer; }

private:
    /** Absorbs rounding in the change condition, so a pitch exactly on the boundary 0.5 + h changes. */
    static constexpr double stickinessEpsilonSemitones = 1e-9;

    static constexpr bool isPlayable (int note) noexcept { return note >= 0 && note <= 127; }

    void processIdleFrame (const PitchFrame& frame, bool strong, double p, int64_t absoluteSample, int sampleOffset,
                           BendGenerator& bend, VsEventSink out) noexcept;
    void processNoteFrame (const PitchFrame& frame, bool weak, double p, int64_t absoluteSample, int sampleOffset,
                           BendGenerator& bend, VsEventSink out) noexcept;
    /** Provisional-note handling of a full-window frame; true if the frame was consumed. */
    bool processProvisionalFrame (const PitchFrame& frame, bool strong, bool weak, double p, int64_t absoluteSample,
                                  int sampleOffset, BendGenerator& bend, VsEventSink out) noexcept;
    void startNote (int q, int outputNote, uint8_t velocity, float levelDb, double p, int64_t absoluteSample,
                    int sampleOffset, BendGenerator& bend, VsEventSink out) noexcept;
    /** Updates the level follower with this frame; true if the frame's pitch may drive the bend. */
    bool isPitchReliable (const PitchFrame& frame, int64_t absoluteSample) noexcept;
    void changeNote (int q, int outputNote, const PitchFrame& frame, double p, int64_t absoluteSample,
                     int sampleOffset, BendGenerator& bend, VsEventSink out) noexcept;
    void endNote (int64_t absoluteSample, int sampleOffset, BendGenerator& bend, VsEventSink out) noexcept;

    double sampleRate = 48000.0;
    ScaleQuantizer quantizer;

    // From setSettings()
    float gateThresholdDb = -45.0f;
    double hysteresisSemitones = 0.2;
    int64_t minChangeSamples = 0;
    int64_t minNoteSamples = 0;
    int octaveShift = 0;
    double velocitySensitivityPercent = 70.0;
    bool legato = false;
    uint8_t channel = 1;

    // State
    int onsetFrames = 0;           ///< consecutive strong-voiced frames while idle
    float onsetMaxRmsDb = PitchFrame::silenceDb;
    int unvoicedFrames = 0;        ///< consecutive not-weak-voiced frames while a note is on
    int activeQuantisedNote = -1;
    int activeOutputNote = -1;
    uint8_t activeChannel = 1;
    int64_t noteStartSample = 0;
    bool changeTimerRunning = false;
    int64_t changeTimerStart = 0;
    bool lastFrameVoiced = false;
    float noteLevelDb = PitchFrame::silenceDb;  ///< level follower of the active note (bend hold)
    int64_t noteLevelSample = 0;                ///< absolute sample of the last follower update

    // Provisional onsets
    int64_t provisionalIgnoreSamples = 0;
    int64_t provisionalTimeoutSamples = 0;
    bool provisional = false;
    bool fastArmed = true;         ///< see class comment
    bool fastBlocked = false;      ///< after a false start: no fast onsets until a gap below the gate
    int fastFrames = 0;            ///< consecutive agreeing fast-strong frames while idle
    double fastLastPitch = 0.0;
    float fastMaxRmsDb = PitchFrame::silenceDb;
};

} // namespace vocalsynth
