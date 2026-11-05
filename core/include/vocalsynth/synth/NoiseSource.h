#pragma once

#include <array>
#include <cstdint>

namespace vocalsynth::synth
{

/** White -> pink noise (docs/SYNTH-DESIGN.md 6.3): xorshift32 seeded in prepare() (reproducible tests);
    colour 0 = white, 1 = pink (-3 dB / octave, Paul Kellet's economy filter), blended linearly. Mono,
    added to both channels, RMS of white and pink matched (about -10 dBFS at level 1).
    Real-time safe, audio thread only. Owner: agent A. */
class NoiseSource
{
public:
    void prepare (double sampleRate, uint32_t seed) noexcept;

    /** Reseeds with the prepare() seed and clears the filter: the same sequence again. */
    void reset() noexcept;

    void beginTick (float level, float colour, int tickSamples) noexcept;

    /** ADDS the same signal to left and right. */
    void render (float* left, float* right, int numSamples) noexcept;

private:
    void finishRamps() noexcept;

    uint32_t seed = 1;
    uint32_t state = 1;
    std::array<float, 3> pinkState {};
    std::array<float, 3> pinkPole {};   ///< Kellet's poles mapped to the sample rate
    std::array<float, 3> pinkGain {};
    float pinkDirect = 0.1848f;
    float whiteScale = 1.0f;            ///< white RMS -> target RMS
    float pinkScale = 1.0f;             ///< pink RMS -> target RMS
    float gain = 0.0f;
    float gainStep = 0.0f;
    float gainTarget = 0.0f;
    float colour = 0.0f;
    float colourStep = 0.0f;
    float colourTarget = 0.0f;
    int samplesLeftInTick = 0;
};

} // namespace vocalsynth::synth
