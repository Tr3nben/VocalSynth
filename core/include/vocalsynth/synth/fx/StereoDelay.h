#pragma once

#include "vocalsynth/synth/SynthSettings.h"

#include <array>
#include <cstdint>
#include <vector>

namespace vocalsynth::synth
{

/** Stereo / ping-pong delay, global after the chorus (docs/SYNTH-DESIGN.md 11.2).
    - Time: timeMs, or tempo-synced (bpm from the host, 120 if unknown) clamped to limits::delayTimeMs;
      time changes glide (the read position moves at most 1 sample per sample: tape-like, no click).
    - Feedback 0..95 %, in the feedback path a one-pole high-pass at 120 Hz and a one-pole low-pass at
      6 kHz (fixed), soft-limited (u / sqrt (1 + u^2)) so the loop stays bounded.
    - Ping-pong: the mono sum enters the left line, the lines cross-feed (L -> R -> L); else two parallel
      lines (L -> L, R -> R).
    - Mix: dual-linear law (Chorus), on/off with a 10 ms crossfade, cleared when off.
    - Silence: when the input is 0 and the lines' peak stays below 1e-6 (-120 dBFS) for one delay time,
      the lines are cleared and output is exactly the dry input (tail ended).
    prepare() allocates maxDelaySeconds per channel at the given rate; the rest is real-time safe.
    NaN guard: a non-finite line sample clears the lines (getNanResetCount). Owner: agent C. */
class StereoDelay
{
public:
    static constexpr double maxDelaySeconds = 2.0;
    static constexpr double switchFadeSeconds = 0.010;

    void prepare (double sampleRate);
    void reset() noexcept;

    void beginTick (const DelaySettings& settings, double bpm, float mixMod, int tickSamples) noexcept;

    void process (float* left, float* right, int numSamples) noexcept;

    bool isBypassed() const noexcept;
    uint32_t getNanResetCount() const noexcept { return nanResets; }

private:
    double sampleRate = 48000.0;
    std::array<std::vector<float>, 2> lines;
    int writeIndex = 0;
    float onGain = 0.0f;
    uint32_t nanResets = 0;
};

} // namespace vocalsynth::synth
