#pragma once

#include "vocalsynth/synth/SynthSettings.h"
#include "vocalsynth/synth/Wavetable.h"

#include <array>
#include <memory>

namespace vocalsynth::synth
{

/** Sub oscillator: sine, triangle or square, band-limited from mip tables (one Wavetable of 1 frame per
    shape, same mip and Hermite rules as WavetableOscillator), mono, added to both channels
    (docs/SYNTH-DESIGN.md 6.2). The three tables are immutable and SHARED by every sub oscillator of the
    process: built once by the first prepare() (function-local static, thread-safe initialisation, never
    on the audio thread) - not 3 tables x 16 voices x instances. Pitch: the voice's note pitch (glide, bend,
    the "Pitch" mod destination; NOT Osc A's octave / semi / fine / Osc A Pitch modulation) minus 1 or 2
    octaves.
    process side real-time safe; audio thread only. Owner: agent A. */
class SubOscillator
{
public:
    /** Builds the shared shape tables on the first call in the process (allocates; NOT real-time). Calls
        reset(). */
    void prepare (double sampleRate);

    void reset() noexcept;

    /** Note start (not legato): phase 0, gain ramp from 0. */
    void restart() noexcept;

    /** increment: cycles per sample of the SUB tone (the caller applies the octave), level: linear gain.
        Shape switches at the tick (no crossfade: the sub is quiet and phase-continuous). */
    void beginTick (double increment, float level, SubShape shape, int tickSamples) noexcept;

    /** ADDS the same signal to left and right. */
    void render (float* left, float* right, int numSamples) noexcept;

private:
    template <bool levelFade>
    void renderSegment (float* left, float* right, int numSamples) noexcept;
    void finishRamps() noexcept;

    double sampleRate = 48000.0;
    const std::array<std::unique_ptr<Wavetable>, numSubShapes>* tables = nullptr; ///< shared, see class comment
    SubShape shape = SubShape::sine;
    double phase = 0.0;
    double increment = 0.0;
    double incrementStep = 0.0;
    double incrementTarget = 0.0;
    float gain = 0.0f;
    float gainStep = 0.0f;
    float gainTarget = 0.0f;
    float levelMix = 1.0f, levelMixStep = 0.0f;   ///< weight of mipLevel vs fromMipLevel
    int mipLevel = 0;
    int fromMipLevel = 0;
    int samplesLeftInTick = 0;
    bool jumpToTargets = true;
};

} // namespace vocalsynth::synth
