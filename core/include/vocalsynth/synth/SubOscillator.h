#pragma once

#include "vocalsynth/synth/SynthSettings.h"
#include "vocalsynth/synth/Wavetable.h"

#include <array>
#include <memory>

namespace vocalsynth::synth
{

/** Sub oscillator: sine, triangle or square, band-limited from its own small mip tables (built in
    prepare(): one Wavetable of 1 frame per shape, same mip rules as WavetableOscillator), mono, added to
    both channels (docs/SYNTH-DESIGN.md 6.2). Pitch: the voice's note pitch (glide, bend; NOT Osc A's
    octave / semi / fine / pitch modulation) minus 1 or 2 octaves.
    process side real-time safe; audio thread only. Owner: agent A. */
class SubOscillator
{
public:
    /** Builds the three shape tables (allocates; NOT real-time). Calls reset(). */
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
    double sampleRate = 48000.0;
    std::array<std::unique_ptr<Wavetable>, numSubShapes> tables;
    SubShape shape = SubShape::sine;
    double phase = 0.0;
    double increment = 0.0;
    float gain = 0.0f;
    float gainStep = 0.0f;
};

} // namespace vocalsynth::synth
