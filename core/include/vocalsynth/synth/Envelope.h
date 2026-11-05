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
                 A it reaches 1 exactly at t = A; the segment ends where v reaches 1 (snaps to 1; v0 >= 1 or
                 A == 0: jumps to 1); then decay;
    - decay   D: v = S + (v - S) * exp (-n * ln (10000) / D); ends where |v - S| reaches 1e-4 |v0 - S|
                 (= t = D with constant D): snaps to S (sustain);
    - sustain:   v = S (follows sustain changes: the caller smooths them);
    - release R: v = v * exp (-n * ln (10000) / R); ends where v reaches 1e-4 v0 (= t = R): snaps to 0 -> idle.
    (End points from the value instead of "t >= time": identical with constant times, and a time changed
    mid-segment can neither snap early nor overshoot.)
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
    /** exp (-n * rate) for this segment; n == controlTickSamples comes from a cache that is valid while the
        segment and its time setting are unchanged (docs/SYNTH-DESIGN.md 18: no exp per tick). */
    double decayFactor (double rate, float timeMs, int n) noexcept;

    double sampleRate = 48000.0;
    Stage stage = Stage::idle;
    float value = 0.0f;
    float segmentStart = 0.0f;
    double segmentTime = 0.0; ///< samples since the segment start
    double level = 0.0;       ///< the value in double precision (float accumulation would drift)
    double cachedFactor = 1.0;
    float cachedTimeMs = -1.0f; ///< time setting of cachedFactor, < 0: invalid
};

} // namespace vocalsynth::synth
