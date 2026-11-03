#pragma once

#include "vocalsynth/synth/SynthSettings.h"

#include <cstdint>

namespace vocalsynth::synth
{

/** ADSR with exponential segments, evaluated at control positions (docs/SYNTH-DESIGN.md 8.1).

    The envelope is a pure function of time between events; advance (n) moves it n samples forward and
    returns the value there. Segments (times in samples, t from the segment start, v0 = start value):
    - attack  A: v = 1.5 - (1.5 - v0) * exp (-t / tauA), tauA = A / ln ((1.5 - v0) / 0.5)  -> reaches 1
                 exactly at t = A (v0 >= 1 or A == 0: jumps to 1); then decay;
    - decay   D: v = S + (1 - S) * exp (-t * ln (10000) / D)  -> at t = D the value snaps to S (sustain);
    - sustain:   v = S (follows sustain changes: the caller smooths them);
    - release R: v = v0 * exp (-t * ln (10000) / R)            -> at t = R snaps to 0 -> idle.
    A segment boundary inside an advance() is handled exactly (the remaining samples continue in the next
    segment), so every segment time is exact to the sample at any tick split. Time 0 = jump.

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
