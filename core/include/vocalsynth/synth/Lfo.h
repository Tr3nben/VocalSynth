#pragma once

#include "vocalsynth/synth/SynthSettings.h"

#include <cstdint>

namespace vocalsynth::synth
{

/** Global LFO (one per engine, not per voice), evaluated once per control tick (docs/SYNTH-DESIGN.md 8.2).

    - Rate: settings.rateHz (sync off) or bpm / 60 / tempoDivisionQuarterNotes (sync) cycles per second,
      times 2^(rateOctaves) from the mod matrix (LFO n Rate, +-4 octaves at 100 %), clamped 0.001..100 Hz.
      The phase runs freely (not locked to the host's song position): block-size invariant.
    - Shapes (phase p in [0, 1)): sine sin (2 pi p); triangle 1 - 4 |p - 0.5| shifted so that p = 0 is 0
      and rising; saw up 2 p - 1; saw down 1 - 2 p; square p < 0.5 ? 1 : -1; S&H a new xorshift32 value in
      [-1, 1] at each cycle start; smooth random: cosine interpolation between successive S&H values.
      Square and S&H jump at the tick (the voice ramps every modulation over the tick, so no click).
    - retrigger() (engine: on every envelope-starting note-on): phase 0 if settings.retrigger; the fade-in
      restarts in any case: output *= min (1, t / fadeMs) with t since the last retrigger() call.
    Output -1..1. Real-time safe, audio thread only. Owner: agent B. */
class Lfo
{
public:
    void prepare (double sampleRate, uint32_t seed) noexcept;

    /** Phase 0, random generator reseeded, fade finished (output not faded). */
    void reset() noexcept;

    void retrigger (const LfoSettings& settings) noexcept;

    /** Advances tickSamples and returns the value at the new position. bpm <= 0: 120. */
    float advanceTick (const LfoSettings& settings, double bpm, float rateOctaves, int tickSamples) noexcept;

    float getValue() const noexcept { return value; }

private:
    double sampleRate = 48000.0;
    uint32_t seed = 1;
    uint32_t random = 1;
    double phase = 0.0;
    double fadeSamples = 0.0; ///< samples since the last retrigger
    float holdValue = 0.0f;
    float nextHoldValue = 0.0f;
    float value = 0.0f;
};

} // namespace vocalsynth::synth
