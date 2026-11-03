#pragma once

#include "vocalsynth/synth/SynthSettings.h"

#include <array>
#include <cstdint>

namespace vocalsynth::synth
{

/** Targets of the voice filter for the END of the current control tick (docs/SYNTH-DESIGN.md 7). */
struct FilterTick
{
    FilterMode mode = FilterMode::lp24; ///< switched at the tick, the integrator states are kept
    float cutoffHz = 20000.0f;          ///< incl. keytrack, env and modulation; clamped to 20 Hz .. 0.49 fs
    float resonance = 0.0f;             ///< 0..1
    float driveDb = 0.0f;               ///< 0..24; 0 = exactly linear (no saturation stage)
};

/** Stereo TPT ("zero-delay feedback") state-variable filter of one voice (docs/SYNTH-DESIGN.md 7), own
    implementation of the published topology (Zavalishin, "The Art of VA Filter Design"):

        g = tan (pi fc / fs), k = 1 / Q,  a1 = 1 / (1 + g (g + k)), a2 = g a1, a3 = g a2
        v3 = x - ic2;  v1 = a1 ic1 + a2 v3;  v2 = ic2 + a2 ic1 + a3 v3;  ic1 = 2 v1 - ic1;  ic2 = 2 v2 - ic2
        LP = v2, BP = k v1 (unity peak), HP = x - k v1 - v2, Notch = x - k v1

    - Q (resonance r): LP12 / BP / HP / Notch: Q = 0.7071 * (qMax / 0.7071)^r, qMax = 25.
      LP24 = two LP stages with the same g: stage 1 Q = 0.5412, stage 2 Q = 1.3066 * (qMax / 1.3066)^r
      (4-pole Butterworth at r = 0, so -3 dB at fc for LP12 and LP24).
    - Drive d dB > 0: before the filter x' = s (G x) / sqrt (G), G = 10^(d / 20), s (u) = u / sqrt (1 + u^2)
      (bounded, odd, soft); d == 0 skips the stage (exactly linear for the response tests).
    - Coefficient ramp: g and k are computed for the tick end; per sample linear ramp of g and k over the
      tick (the TPT structure is stable for any g > 0, k > 0, also while ramping). Cutoff smoothing is the
      caller's (SynthVoice smooths in log-frequency per tick).
    - NaN / Inf guard: after each render() call, a non-finite state resets the integrators to 0 and
      increments getNanResetCount(); a state below 1e-20 in magnitude is set to exactly 0 (denormals).
    Real-time safe, audio thread only. Owner: agent B. */
class SvfFilter
{
public:
    static constexpr float qMax = 25.0f;

    void prepare (double sampleRate) noexcept;

    /** Integrators 0, coefficient ramp finished at the current targets. */
    void reset() noexcept;

    void beginTick (const FilterTick& target, int tickSamples) noexcept;

    /** In place, numSamples <= the samples left in the current tick. */
    void process (float* left, float* right, int numSamples) noexcept;

    uint32_t getNanResetCount() const noexcept { return nanResets; }

private:
    double sampleRate = 48000.0;
    FilterMode mode = FilterMode::lp24;
    float g = 0.0f, gStep = 0.0f;
    float k = 1.4142f, kStep = 0.0f;
    float k1 = 1.8478f; ///< LP24 stage 1
    float driveGain = 1.0f;
    std::array<std::array<float, 4>, 2> state {}; ///< [channel][ic1, ic2 of stage 1, ic1, ic2 of stage 2]
    uint32_t nanResets = 0;
};

} // namespace vocalsynth::synth
