#pragma once

#include "vocalsynth/synth/SynthSettings.h"

#include <array>
#include <vector>

namespace vocalsynth::synth
{

/** Stereo chorus, global after the voice sum (docs/SYNTH-DESIGN.md 11.1): two modulated delay lines
    (base 7 ms and 11 ms), each read with linear interpolation, modulated by one sine LFO with 90 degree
    offset between left and right (rate = rateHz), depth 0..100 % = 0..4 ms of modulation. Line 1 feeds
    left, line 2 right; both are fed by the mono sum of the input. No feedback.
    Mix (dual-linear law, also used by the delay and the reverb): out = dry * min (1, 2 (1 - m)) +
    wet * min (1, 2 m), m = clamp (mixPercent / 100 + mixMod, 0, 1).
    On/off: 10 ms linear crossfade between dry and the effect; once off and faded, process() returns at
    once and the lines are cleared (no stale tail when switched on again).
    prepare() allocates for maxDelaySeconds at the given rate (any rate up to 192 kHz and beyond);
    everything else is real-time safe. Audio thread only. Owner: agent C. */
class Chorus
{
public:
    static constexpr double maxDelaySeconds = 0.030;
    static constexpr double switchFadeSeconds = 0.010;

    void prepare (double sampleRate);
    void reset() noexcept;

    /** Targets for the end of the current tick (rate, depth and mix ramp over tickSamples). */
    void beginTick (const ChorusSettings& settings, float mixMod, int tickSamples) noexcept;

    /** In place. */
    void process (float* left, float* right, int numSamples) noexcept;

    /** Off and faded: process() does nothing. */
    bool isBypassed() const noexcept;

private:
    double sampleRate = 48000.0;
    std::array<std::vector<float>, 2> lines;
    int writeIndex = 0;
    double lfoPhase = 0.0;
    float onGain = 0.0f;
};

} // namespace vocalsynth::synth
