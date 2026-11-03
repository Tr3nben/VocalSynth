#pragma once

#include "vocalsynth/synth/SynthSettings.h"

#include <array>
#include <cstdint>
#include <vector>

namespace vocalsynth::synth
{

/** 8-channel feedback delay network reverb, global after the delay (docs/SYNTH-DESIGN.md 11.3), own code.
    - Input: mono sum -> pre-delay 10 ms -> 4 series all-pass diffusers (1.3 / 2.1 / 3.4 / 5.5 ms * size
      scale, g = 0.6) -> distributed into the 8 lines with alternating signs.
    - 8 delay lines, base lengths (ms) 29.7, 37.1, 41.1, 43.7, 53.9, 61.3, 71.9, 79.1, scaled by
      0.5 + 1.0 * size; each read position slowly modulated (+-0.3 ms, 0.1..0.7 Hz, different per line).
    - Feedback matrix: Householder H = I - (2 / 8) 1 1^T (lossless, cheap: y_i = x_i - (sum x) / 4).
    - Decay: RT60 = 0.3 s .. 10 s exponential in size; per line gain g_i = 10^(-3 L_i / (RT60 fs)).
      Damping: one-pole low-pass in each line, cutoff 18 kHz (0 %) .. 1.5 kHz (100 %), exponential.
    - Output: L = lines 0, 2, 4, 6, R = lines 1, 3, 5, 7 (signs alternating), scaled by 1 / sqrt (8).
    - Mix: dual-linear law (Chorus), on/off with a 10 ms crossfade, cleared when off; silence detection
      as the delay (input 0 and every line below 1e-6 for the longest line length * 4).
    - NaN guard: a non-finite state clears all lines at the next beginTick() (getNanResetCount).
    prepare() allocates for the largest size at the given rate; the rest is real-time safe. Owner: agent C. */
class FdnReverb
{
public:
    static constexpr int numLines = 8;
    static constexpr double switchFadeSeconds = 0.010;

    void prepare (double sampleRate);
    void reset() noexcept;

    void beginTick (const ReverbSettings& settings, float mixMod, int tickSamples) noexcept;

    void process (float* left, float* right, int numSamples) noexcept;

    bool isBypassed() const noexcept;
    uint32_t getNanResetCount() const noexcept { return nanResets; }

private:
    double sampleRate = 48000.0;
    std::array<std::vector<float>, numLines> lines;
    std::array<float, numLines> damping {};
    int writeIndex = 0;
    float onGain = 0.0f;
    uint32_t nanResets = 0;
};

} // namespace vocalsynth::synth
