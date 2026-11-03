#pragma once

#include "vocalsynth/synth/SynthSettings.h"

#include <cstdint>

namespace vocalsynth::synth
{

/** ADSR with exponential segments, evaluated at control positions (docs/SYNTH-DESIGN.md 8.1).

    Incremental form (binding; critique 0.4: a closed form in the time since the segment start jumps when an
    automated / smoothed segment time changes mid-segment). advance (settings, n) moves n samples forward
    with the CURRENT settings (times in samples, t = samples since the segment start, v = current value,
    v0 = value at the segment start):
    - attack  A: v = 1.5 - (1.5 - v) * exp (-n / tauA), tauA = A / ln ((1.5 - v0) / 0.5)  -> with constant
                 A it reaches 1 exactly at t = A; the segment ends when t >= A (value snaps to 1; v0 >= 1 or
                 A == 0: jumps to 1); then decay;
    - decay   D: v = S + (v - S) * exp (-n * ln (10000) / D); ends when t >= D: snaps to S (sustain);
    - sustain:   v = S (follows sustain changes: the caller smooths them);
    - release R: v = v * exp (-n * ln (10000) / R); ends when t >= R: snaps to 0 -> idle.
    A segment boundary inside an advance() is handled exactly (the remaining samples continue in the next
    segment), so every segment time is exact to the sample at any tick split. Time 0 = jump. The control
    positions are absolute (ticks, commands), so the result is block-size invariant although
    exp (-a) * exp (-b) is not bit-equal to exp (-(a + b)). With constant settings the values match the closed
    forms within 1e-5 (test).

    noteOn (v) / noteOff (v) restart the attack / release from v (the caller's current output value, so a
    retrigger or a release mid-tick never jumps). Legato keeps the envelope untouched.
    Real-time safe, no allocation. Audio thread only. Owner: agent B. */
class Envelope
{
public:
    enum class Stage : uint8_t
    {
        idle,
        attack,
        decay,
        sustain,
        release
    };

    void prepare (double sampleRate) noexcept;

    /** Idle, value 0. */
    void reset() noexcept;

    void noteOn (float startValue) noexcept;
    void noteOff (float startValue) noexcept;

    /** Moves numSamples (>= 0) forward with these settings and returns the value at the new position. */
    float advance (const EnvelopeSettings& settings, int numSamples) noexcept;

    float getValue() const noexcept { return value; }
    Stage getStage() const noexcept { return stage; }
    bool isActive() const noexcept { return stage != Stage::idle; }

private:
    double sampleRate = 48000.0;
    Stage stage = Stage::idle;
    float value = 0.0f;
    float segmentStart = 0.0f;
    double segmentTime = 0.0; ///< samples since the segment start
};

} // namespace vocalsynth::synth
