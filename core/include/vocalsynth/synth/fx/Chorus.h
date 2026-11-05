#pragma once

#include "vocalsynth/synth/SynthSettings.h"

#include <cstdint>
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
    once and the lines are cleared (no stale tail when switched on again). Every such decision (fade end,
    bypass, clearing, tail end, NaN reset, denormal flush) is taken in beginTick(), never at the end of a
    process() call (block-size invariance, docs/SYNTH-DESIGN.md 3.5); the same holds for the delay and the
    reverb.
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
    uint32_t getNanResetCount() const noexcept { return nanResets; }

private:
    void finishRamp() noexcept;
    void clearLines() noexcept;
    void advanceIdle (int numSamples) noexcept;
    template <bool checked>
    void run (float* left, float* right, int numSamples) noexcept;

    double sampleRate = 48000.0;
    std::vector<float> line; ///< mono sum (both taps read it), power-of-two length
    int mask = 0;
    int reach = 1;           ///< longest tap + 2 samples
    int writeIndex = 0;
    int written = 0;         ///< samples written since the last (logical) clear, saturating at reach
    double lfoPhase = 0.0;
    float fadeStepPerSample = 0.0f;
    float onGain = 0.0f, onTarget = 0.0f, onStep = 0.0f;
    float delayLeft = 0.0f, delayLeftTarget = 0.0f, delayLeftStep = 0.0f;
    float delayRight = 0.0f, delayRightTarget = 0.0f, delayRightStep = 0.0f;
    float dryGain = 1.0f, dryTarget = 1.0f, dryStep = 0.0f;
    float wetGain = 0.0f, wetTarget = 0.0f, wetStep = 0.0f;
    int rampLeft = 0;
    int silentRun = 0;       ///< consecutive zero input samples
    float nanCheck = 0.0f;   ///< sum of the written samples of this tick (non-finite -> clear)
    uint32_t nanResets = 0;
    bool bypassed = true;
    bool idle = false;       ///< tail ended: nothing to do while the input is 0
    bool firstTick = true;   ///< the first beginTick() after reset() jumps to the targets
};

} // namespace vocalsynth::synth
