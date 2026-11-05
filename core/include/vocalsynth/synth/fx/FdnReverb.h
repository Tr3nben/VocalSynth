#pragma once

#include "vocalsynth/synth/SynthSettings.h"

#include <array>
#include <cstdint>
#include <utility>
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
    static constexpr int numDiffusers = 4;
    static constexpr double switchFadeSeconds = 0.010;

    void prepare (double sampleRate);
    void reset() noexcept;

    void beginTick (const ReverbSettings& settings, float mixMod, int tickSamples) noexcept;

    void process (float* left, float* right, int numSamples) noexcept;

    bool isBypassed() const noexcept;
    uint32_t getNanResetCount() const noexcept { return nanResets; }

private:
    void finishRamp() noexcept;
    void clearState() noexcept;
    template <bool checked>
    void run (float* left, float* right, int numSamples) noexcept;

    double sampleRate = 48000.0;
    std::array<std::vector<float>, numLines> lines;         ///< power-of-two lengths
    std::array<std::vector<float>, numDiffusers> diffusers; ///< all-pass delay lines
    std::vector<float> preDelay;
    uint32_t writeCounter = 0;   ///< shared by every buffer (masked per buffer, wraps)
    int written = 0;             ///< samples written since the last (logical) clear, saturating at maxReach
    int maxReach = 1;            ///< longest read distance of any buffer + margin
    int preDelaySamples = 1;
    std::array<int, numDiffusers> diffuserDelay {};
    std::array<float, numLines> lineLength {};   ///< nominal length (samples) for the current size
    std::array<float, numLines> lineDelay {}, lineDelayTarget {}, lineDelayStep {}; ///< incl. modulation
    std::array<float, numLines> lineGain {}, lineGainTarget {}, lineGainStep {};    ///< RT60 gain per pass
    std::array<float, numLines> damping {};      ///< one-pole low-pass states
    std::array<std::pair<double, double>, numLines> modulation {};   ///< (cos, sin) phasor per line
    std::array<std::pair<double, double>, numLines> tickRotation {}; ///< phasor step per 16-sample tick
    std::array<double, numLines> modulationAngle {};                 ///< radians per sample
    float modulationDepth = 0.0f; ///< samples
    float dampingCoefficient = 1.0f;
    float cachedSize = -1.0f, cachedDamping = -1.0f;
    int longestLine = 1;
    float fadeStepPerSample = 0.0f;
    float onGain = 0.0f, onTarget = 0.0f, onStep = 0.0f;
    float dryGain = 1.0f, dryTarget = 1.0f, dryStep = 0.0f;
    float wetGain = 0.0f, wetTarget = 0.0f, wetStep = 0.0f;
    int rampLeft = 0;
    int silentRun = 0;     ///< consecutive samples with input 0 and every line write below 1e-6
    float nanCheck = 0.0f; ///< sum of the line feedback of this tick (non-finite -> clear)
    uint32_t nanResets = 0;
    bool bypassed = true;
    bool idle = false;     ///< tail ended
    bool firstTick = true;
};

} // namespace vocalsynth::synth
