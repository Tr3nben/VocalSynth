#pragma once

namespace vocalsynth
{

/** Turns the raw analysis snapshots (one per editor tick, ~30 per second) into calm tuner values, the
    way a hardware tuner shows them (v0.2.1, docs/M2-DESIGN.md 11). Pure logic, no JUCE, message thread
    only. All smoothing uses the elapsed time per update, so it does not depend on the timer rate.

    - Note name: the note VocalSynth sends (segmented, sticky; in the sung octave, i.e. without the
      Octave shift) while one is active. Without an active note, the nearest chromatic note is named only
      after it has stayed the same for noteStableMs (a note that is already shown continues at once).
      After the pitch is gone the name is held for noteHoldMs, then it fades out over noteFadeMs to "-".
    - Needle: cents of the sung pitch from the shown note, clamped to +-maxNeedleCents, smoothed by a
      one-pole low pass (needleTimeConstantMs). It glides to the new value when the note changes
      (no jump), holds while the pitch is briefly gone, and starts at its target when a note appears
      after the name had faded out.
    - Hz and cents texts: stepped every textIntervalMs (at most 5 changes per second) from the smoothed
      pitch / needle; refreshed at once when a note appears, and when the shown note changes: then the
      smoothed pitch jumps to the new pitch and the cents text shows its offset from the new note
      (the needle still glides), so the texts never show the lagged pitch of the previous note.
    - Voiced LED: on at once, off only after the voicing has been gone for ledHoldMs.
    - Confidence bar: fast attack (confidenceAttackMs), slower release (confidenceReleaseMs). */
class TunerDisplayModel
{
public:
    static constexpr double noteStableMs = 100.0;
    static constexpr double noteHoldMs = 250.0;
    static constexpr double noteFadeMs = 200.0;
    static constexpr double needleTimeConstantMs = 120.0; ///< 5.5 Hz vibrato of +-50 cents shows as about +-12
    static constexpr double textIntervalMs = 200.0;
    static constexpr double ledHoldMs = 150.0;
    static constexpr double confidenceAttackMs = 15.0;
    static constexpr double confidenceReleaseMs = 250.0;
    static constexpr double maxStepMs = 500.0; ///< longer gaps (stalled timer) count as this much
    static constexpr float maxNeedleCents = 50.0f;
    static constexpr float inTuneCents = 10.0f;

    struct Input
    {
        float frequencyHz = 0.0f; ///< newest f0, 0 = unvoiced
        int nearestNote = -1;     ///< nearest chromatic note of frequencyHz, -1 if none
        int sungNote = -1;        ///< note VocalSynth sends, octave shift removed (-1 = none)
        float confidence = 0.0f;  ///< 0..1
        bool voiced = false;      ///< voicing decision of the newest frame
        bool active = true;       ///< false while source = Test Pattern: everything is cleared
    };

    struct State
    {
        int note = -1;                 ///< note to name, -1 = "-"
        bool noteIsSegmented = false;  ///< true while the shown note is the one being sent
        float noteAlpha = 0.0f;        ///< 1 = shown, falls to 0 during the fade (needle alike)
        float needleCents = 0.0f;      ///< smoothed, -maxNeedleCents .. +maxNeedleCents
        float displayHz = 0.0f;        ///< stepped Hz text value, 0 = "-" (and no needle: no pitch yet)
        int displayCents = 0;          ///< stepped cents text value (valid while displayHz > 0)
        bool ledOn = false;
        float confidence = 0.0f;       ///< smoothed bar value 0..1
    };

    /** Advances the display by elapsedMs (negative values count as 0, long gaps as maxStepMs). */
    const State& update (const Input& input, double elapsedMs) noexcept;

    const State& getState() const noexcept { return state; }

    void reset() noexcept;

    /** 1 - exp(-elapsedMs / timeConstantMs): the one-pole coefficient for one update. */
    static float smoothingCoefficient (double elapsedMs, double timeConstantMs) noexcept;

private:
    void updateNote (const Input& input, bool hasPitch, double elapsedMs, bool& appeared, bool& changed) noexcept;

    State state;
    int candidateNote = -1;
    double candidateMs = 0.0;
    double sinceConfirmedMs = 0.0;
    double sinceVoicedMs = 0.0;
    double sinceTextMs = 0.0;
    double smoothedPitch = 0.0; ///< MIDI note number (fractional) of the smoothed f0, 0 = none yet
    bool needleNeedsSnap = true; ///< the next pitch sets needle and pitch directly (a note appeared)
    bool textNeedsSnap = false;  ///< the next pitch sets the smoothed pitch and the texts (note changed)
};

} // namespace vocalsynth
