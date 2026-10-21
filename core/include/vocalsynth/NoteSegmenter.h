#pragma once

#include "vocalsynth/BendGenerator.h"
#include "vocalsynth/EventTiming.h"
#include "vocalsynth/PitchDetector.h"
#include "vocalsynth/PitchPostProcessor.h"
#include "vocalsynth/ScaleQuantizer.h"
#include "vocalsynth/VoiceSettings.h"
#include "vocalsynth/VsEvent.h"

#include <array>
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
        v0.3.1 (docs/M2-DESIGN.md 12), on the pitch history = the consecutive frames of the note with
        confidence >= confidenceOn (any other frame and every note start clear it):
        * arrival: the change also happens once the condition has held for arrivalFraction * T_min if every
          history frame of that span lies within arrivalMaxDistanceSemitones of this frame's candidate and
          their spread is <= arrivalMaxSpreadSemitones (the voice has landed on the new note; a vibrato
          peak only gets there beyond about +-85 cents, where T_min alone flips as well);
        * glide hold: no change (by T_min or arrival) on a frame whose pitch slope (least squares over the
          history of the last glideWindowMs) exceeds glideHoldSemitonesPerSecond, or, within
          onsetGlideHoldMs of a note start from rest (a provisional correction keeps that start),
          onsetGlideHoldSemitonesPerSecond over onsetGlideWindowMs (a scoop into the note). The timer keeps
          running, so the change goes straight to the note where the glide ends instead of stepping
          through the notes it passes (each held for minNoteLength). A slow glissando still steps.
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
    those frames, same event order as a note start), unless that last pitch lies within
    fastNoteMarginSemitones of a boundary between two allowed notes (then the run continues). Armed = the fast path saw a frame that was not
    fast-strong since the last note start (or reset): after a note (also a failed provisional one) the
    fast path needs a gap in the voice first, so it can never re-trigger in a loop where the full path
    disagrees. While provisional, the full-window frames (processFrame):
    - within provisionalIgnoreSamples of the start are ignored (keeps the event bound, see
      VoicePipeline::maxEventsForSamples);
    - strong frames within provisionalSettleSamples of the start (the full window still holds audio
      from before the start: the onset, the start of a scoop into the note, so it lags behind the fast
      frames): if the candidate quantize (p, q) wins by 2 h (the usual change condition without T_min
      and minNoteLength: the fast frames read another note or octave), the note is corrected at once
      (change with the legato/retrigger order) and is a normal note from then on; otherwise the frame
      is processed as by a normal note (bend, change timer) and the note stays provisional;
    - the first strong frame after that confirms the note: if its candidate wins by
      min (provisionalSettleMarginSemitones, 2 h) (2 cents: the nearest note; a near-tie within the
      detector noise keeps q), the note is
      corrected at once; either way it is a normal note from then on. A scoop into the note that
      takes longer than about one window is still read mid-scoop here and then changes by the normal
      path (T_min, minNoteLength), as in v0.2.0;
    - other frames hold the note (no release counting, no bend updates) until provisionalTimeoutSamples
      after the start; then the note becomes normal if the frame is weak voiced or a strong frame was
      seen since the start, else it ends at once (false start; minNoteLength does not apply). After a
      false start the fast path stays off until a frame below gateThresholdDb or a note start by the
      full window: where the full window never agrees (e.g. a tone whose 2 f0 lies above the range
      and whose 2nd harmonic dominates, which YIN rejects as out of range), alternating fast frames
      would otherwise re-arm it every timeout. (v0.2.1 review: with gateThresholdDb - gateHysteresisDb
      as the gap, a noise floor just below the gate kept the fast path off for the whole session after
      one short blip.)

    Event order at one sample (binding):
    - note start:            bend.noteStarted, note-on
    - retrigger change:      note-off (old), bend.noteStarted (new q), note-on (new)     [legato == false]
    - legato change:         bend.noteStarted (new q), note-on (new), note-off (old)     [legato == true]
    - note end / flush:      note-off, bend.noteEnded (centre if needed)
    At most 3 events per frame. Note-on velocity of a change = velocityFor (rmsDb of that frame, ...).

    Detection-delay side output (M3 capture, docs/M3-DESIGN.md 7): every processFrame / processOnsetFrame
    / flush call also leaves the timing of the events it emitted in getLastEventTiming() (estimated true
    time = emit sample - delaySamples). It never changes the events. Estimates:
    - note start from silence: the newest upward level crossing (setLevelOnsetSample) if it lies within
      DetectionDelayModel::onsetSearchSamples before the emit sample and after the previous note's
      estimated end (kind measured); else the latency model (provisional start: fastOnsetDelay at the
      note's pitch, full-window start: confirmedOnsetDelaySamples; kind model);
    - note change (T_min path, legato or retrigger): the first frame of the change timer minus
      frameDelaySamples, after the old note's estimated start (kind measured, flag noteChange); the old
      note's note-off gets the same estimate;
    - provisional correction (the full window corrects a provisional note): the note-off of the
      provisional note and the corrected note-on get the provisional note-on's delay (flag correction);
      a false start's note-off gets that delay as well;
    - release: the newest downward level crossing (setLevelReleaseSample) after the note's estimated
      start, else the first not-voiced frame of the release run minus frameDelaySamples (kind measured);
    - bend updates: emit - frameDelaySamples (kind measured);
    - flush, octave-shift changes and notes ended by the octave shift: kind none (they happen when sent).

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
    /** A provisional note starts only if p +- this quantises to the same note as p (not near a boundary). */
    static constexpr double fastNoteMarginSemitones = 0.15;
    /** Correction margin of the first strong frame after the settle time (see class comment). */
    static constexpr double provisionalSettleMarginSemitones = 0.02;
    static constexpr int maxEventsPerOnsetFrame = 2;       ///< bend + note-on

    // Note changes on real singing (v0.3.1, see class comment and docs/M2-DESIGN.md 12)
    static constexpr double arrivalFraction = 0.5;                ///< of T_min
    static constexpr double arrivalMaxDistanceSemitones = 0.2;    ///< every frame of the span from the candidate
    static constexpr double arrivalMaxSpreadSemitones = 0.3;      ///< max - min of the span
    static constexpr double glideHoldSemitonesPerSecond = 35.0;
    static constexpr double glideWindowMs = 8.0;
    static constexpr double onsetGlideHoldMs = 120.0;             ///< after a note start from rest
    static constexpr double onsetGlideHoldSemitonesPerSecond = 8.0;
    static constexpr double onsetGlideWindowMs = 16.0;
    /** Pitch history length in frames: covers the longest span (arrival at T_min 60 ms, onset glide window)
        at the pipeline's 2 ms hop twice over. */
    static constexpr int pitchHistoryFrames = 32;

    /** Timing of the events emitted by the most recent processFrame / processOnsetFrame / flush call
        (M3, see class comment): noteOn = a note-on of this call and a bend sent right before it (note
        start bend); noteOff = a note-off of this call and a bend centre sent right after it; update = any
        other bend. Delays relative to that call's absoluteSample. Kind none for what the call did not emit. */
    struct FrameEventTiming
    {
        VsEventTiming noteOn;
        VsEventTiming noteOff;
        VsEventTiming update;
    };

    /** Latency model for the detection-delay estimates, in host samples (VoicePipeline::applySettings). */
    struct DetectionDelayModel
    {
        /** Frame time -> audio time: the input sample that completed the hop minus this is where the
            analysed pitch was (window centre + decimator FIR + median delay). */
        double frameDelaySamples = 0.0;
        int64_t confirmedOnsetDelaySamples = 0; ///< model of a full-window start (VoicePipeline::computeConfirmedLatencyMs)
        /** Model of a provisional start for a tone at f0Hz (VoicePipeline::computeFastOnsetLatencyMs);
            nullptr: confirmedOnsetDelaySamples. Called on the audio thread: must be real-time safe. */
        int64_t (*fastOnsetDelay) (const void* context, double f0Hz) noexcept = nullptr;
        const void* fastOnsetDelayContext = nullptr;
        int64_t onsetSearchSamples = 0;         ///< a level onset up to this far before the emit sample counts
    };

    /** M3: sets the model (never emits; takes effect at the next estimate). */
    void setDetectionDelayModel (const DetectionDelayModel& model) noexcept;

    /** M3: newest upward / downward crossing of the pipeline's level envelope (absolute host samples on
        the processFrame clock; the release value already corrected for the envelope's decay). Never emits. */
    void setLevelOnsetSample (int64_t absoluteSample) noexcept;
    void setLevelReleaseSample (int64_t absoluteSample) noexcept;

    const FrameEventTiming& getLastEventTiming() const noexcept { return lastTiming; }

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
        hops, settle = window of the current range). Never emits. Values < 0 are treated as 0. */
    void setOnsetTiming (int64_t provisionalIgnoreSamples, int64_t provisionalTimeoutSamples,
                         int64_t provisionalSettleSamples = 0) noexcept;

    /** One FastOnsetDetector frame (only used while idle, see class comment). Same arguments as
        processFrame(). Emits at most maxEventsPerOnsetFrame events. */
    void processOnsetFrame (const PitchFrame& frame, int64_t absoluteSample, int sampleOffset, BendGenerator& bend,
                            VsEventSink out) noexcept;

    /** True while the active note was started by processOnsetFrame() and not yet confirmed. */
    bool isProvisional() const noexcept { return provisional; }

    /** True while processOnsetFrame() reads only frame.rmsDb (fast path off after a false start, see
        class comment): the caller may skip the pitch search and pass an RMS-only frame. */
    bool isFastPathBlocked() const noexcept { return fastBlocked; }

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
                    int sampleOffset, BendGenerator& bend, VsEventSink out, const VsEventTiming& timing) noexcept;
    /** Updates the level follower with this frame; true if the frame's pitch may drive the bend. */
    bool isPitchReliable (const PitchFrame& frame, int64_t absoluteSample) noexcept;
    void changeNote (int q, int outputNote, const PitchFrame& frame, double p, int64_t absoluteSample,
                     int sampleOffset, BendGenerator& bend, VsEventSink out, const VsEventTiming& timing) noexcept;
    void endNote (int64_t absoluteSample, int sampleOffset, BendGenerator& bend, VsEventSink out,
                  const VsEventTiming& timing) noexcept;

    // Pitch history of the active note (v0.3.1 change rules)
    void pushPitchHistory (double p, int64_t absoluteSample) noexcept;
    /** Least-squares slope in semitones per second of the history frames within windowSamples of
        absoluteSample (inclusive), 0 with fewer than 3 such frames. */
    double pitchSlope (int64_t absoluteSample, int64_t windowSamples) const noexcept;
    /** True if the history covers windowSamples before absoluteSample and every frame of that span is
        within arrivalMaxDistanceSemitones of candidate with a spread <= arrivalMaxSpreadSemitones. */
    bool hasArrived (int64_t absoluteSample, int64_t windowSamples, int candidate) const noexcept;
    bool isGliding (int64_t absoluteSample) const noexcept;

    // Detection-delay estimates (see class comment)
    static VsEventTiming timingFor (int64_t emitSample, int64_t estimate, EventTimingKind kind, uint8_t flags) noexcept;
    VsEventTiming onsetTiming (int64_t absoluteSample, bool provisionalPath, double p) const noexcept;
    VsEventTiming changeTiming (int64_t absoluteSample) const noexcept;
    VsEventTiming releaseTiming (int64_t absoluteSample) const noexcept;
    int64_t frameDelay() const noexcept;

    double sampleRate = 48000.0;
    ScaleQuantizer quantizer;

    // From setSettings()
    float gateThresholdDb = -35.0f;  ///< VoiceSettings default
    double hysteresisSemitones = 0.2;
    int64_t minChangeSamples = 0;
    int64_t arrivalSamples = 0;      ///< arrivalFraction * T_min
    int64_t glideWindowSamples = 0;
    int64_t onsetGlideHoldSamples = 0;
    int64_t onsetGlideWindowSamples = 0;
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
    std::array<double, pitchHistoryFrames> historyPitch {};
    std::array<int64_t, pitchHistoryFrames> historySample {};
    int historyCount = 0;
    int historyNext = 0;                        ///< ring index of the next push
    bool noteFromRest = false;                  ///< the active note started from rest (not by a change)
    int64_t restStartSample = 0;                ///< its start (a provisional correction keeps it)

    // Provisional onsets
    int64_t provisionalIgnoreSamples = 0;
    int64_t provisionalTimeoutSamples = 0;
    int64_t provisionalSettleSamples = 0;
    int64_t provisionalStartSample = 0;  ///< fast onset of the provisional note (corrections keep it)
    bool provisional = false;
    bool provisionalSeenStrong = false;  ///< a strong full frame came since the provisional start
    bool fastArmed = true;         ///< see class comment
    bool fastBlocked = false;      ///< after a false start: no fast onsets until below the gate / a full start
    int fastFrames = 0;            ///< consecutive agreeing fast-strong frames while idle
    double fastLastPitch = 0.0;
    float fastMaxRmsDb = PitchFrame::silenceDb;

    // Detection-delay side output (never affects the events)
    DetectionDelayModel delayModel {};
    FrameEventTiming lastTiming {};
    bool hasLevelOnset = false;
    int64_t levelOnsetSample = 0;
    bool hasLevelRelease = false;
    int64_t levelReleaseSample = 0;
    VsEventTiming noteOnTiming {};       ///< timing of the active note's note-on
    int64_t noteEstimatedStart = 0;      ///< its estimated true start
    bool hasPreviousNoteEnd = false;
    int64_t previousNoteEnd = 0;         ///< estimated true end of the last note that ended
    int64_t releaseRunStart = 0;         ///< first not-weak frame of the current release run
};

} // namespace vocalsynth
