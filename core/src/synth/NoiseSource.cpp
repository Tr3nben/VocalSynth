#include "vocalsynth/synth/NoiseSource.h"

#include "vocalsynth/synth/SynthSettings.h"

#include <algorithm>
#include <cmath>

namespace vocalsynth::synth
{

namespace
{
// Paul Kellet's economy pink filter: 3 one-poles + a direct path, pink = b0 + b1 + b2 + d * white with
// b_i = p_i b_i + g_i white. Kellet's coefficients are for 44.1 kHz; the other sets were refitted (same
// structure, minimax of the -3 dB / octave error over 20 Hz .. 20 kHz) because mapping the 44.1 kHz poles
// to 96 kHz misses the slope by 2.4 dB at the top. Error of every set: <= 0.5 dB. Rates between the sets
// use the nearest one with its poles moved to the rate (p^(rate_set / fs), g * rate_set / fs).
struct PinkSet
{
    double rate;
    double pole[3];
    double gain[3];
    double direct;
};

constexpr PinkSet pinkSets[] = {
    { 44100.0, { 0.99765, 0.96300, 0.57000 }, { 0.0990460, 0.2965164, 1.0526913 }, 0.1848 },
    { 48000.0, { 0.99598926, 0.95129043, 0.56321730 }, { 0.09599812, 0.24089921, 0.74832854 }, 0.20873406 },
    { 88200.0, { 0.99769084, 0.96897625, 0.66824003 }, { 0.05646422, 0.15256992, 0.51008601 }, 0.28109657 },
    { 96000.0, { 0.99779677, 0.96909150, 0.65949912 }, { 0.05176661, 0.14255669, 0.48636854 }, 0.23916550 },
    { 176400.0, { 0.99871534, 0.98073432, 0.75569060 }, { 0.02665253, 0.07414734, 0.28982990 }, 0.20811559 },
    { 192000.0, { 0.99884018, 0.98210996, 0.78152532 }, { 0.02660957, 0.07838296, 0.26287398 }, 0.30857015 },
};

constexpr double targetRms = 0.31622776601683794; // -10 dBFS at level 1
constexpr double uniformVariance = 1.0 / 3.0;     // uniform [-1, 1)
} // namespace

void NoiseSource::prepare (double sampleRate, uint32_t newSeed) noexcept
{
    seed = newSeed != 0 ? newSeed : 1u;
    const double fs = sampleRate > 0.0 && std::isfinite (sampleRate) ? sampleRate : 48000.0;
    const PinkSet* set = &pinkSets[0];

    for (const auto& candidate : pinkSets)
        if (std::abs (std::log (candidate.rate / fs)) < std::abs (std::log (set->rate / fs)))
            set = &candidate;

    const double ratio = set->rate / fs;
    double p[3];
    double g[3];

    for (int i = 0; i < 3; ++i)
    {
        p[i] = std::pow (set->pole[i], ratio);
        g[i] = set->gain[i] * ratio;
        pinkPole[static_cast<std::size_t> (i)] = static_cast<float> (p[i]);
        pinkGain[static_cast<std::size_t> (i)] = static_cast<float> (g[i]);
    }

    const double d = set->direct;
    pinkDirect = static_cast<float> (d);

    // Energy of the impulse response h[0] = d + sum g, h[n] = sum g_i p_i^n
    double energy = d * d;

    for (int i = 0; i < 3; ++i)
    {
        energy += 2.0 * d * g[i];

        for (int j = 0; j < 3; ++j)
            energy += g[i] * g[j] / (1.0 - p[i] * p[j]);
    }

    whiteScale = static_cast<float> (targetRms / std::sqrt (uniformVariance));
    pinkScale = static_cast<float> (targetRms / std::sqrt (uniformVariance * energy));
    reset();
}

void NoiseSource::reset() noexcept
{
    state = seed;
    pinkState.fill (0.0f);
    gain = gainStep = gainTarget = 0.0f;
    colour = colourStep = colourTarget = 0.0f;
    samplesLeftInTick = 0;
}

void NoiseSource::finishRamps() noexcept
{
    gain = gainTarget;
    gainStep = 0.0f;
    colour = colourTarget;
    colourStep = 0.0f;
}

void NoiseSource::beginTick (float level, float newColour, int tickSamples) noexcept
{
    const int n = std::clamp (tickSamples, 1, controlTickSamples);

    if (tickSamples >= controlTickSamples || samplesLeftInTick <= 0)
        finishRamps();

    gainTarget = std::isfinite (level) ? std::clamp (level, 0.0f, 1.0f) : 0.0f;
    colourTarget = std::isfinite (newColour) ? std::clamp (newColour, 0.0f, 1.0f) : 0.0f;
    const float invN = 1.0f / static_cast<float> (n);
    gainStep = (gainTarget - gain) * invN;
    colourStep = (colourTarget - colour) * invN;
    samplesLeftInTick = n;
}

void NoiseSource::render (float* left, float* right, int numSamples) noexcept
{
    while (numSamples > 0)
    {
        if (samplesLeftInTick <= 0)
        {
            finishRamps();
            samplesLeftInTick = numSamples;
        }

        const int n = std::min (numSamples, samplesLeftInTick);
        uint32_t x = state;
        float b0 = pinkState[0], b1 = pinkState[1], b2 = pinkState[2];
        const float p0 = pinkPole[0], p1 = pinkPole[1], p2 = pinkPole[2];
        const float g0 = pinkGain[0], g1 = pinkGain[1], g2 = pinkGain[2];
        const float d = pinkDirect, ws = whiteScale, ps = pinkScale;
        float g = gain;
        float c = colour;
        const float gStep = gainStep, cStep = colourStep;

        // Pure white for the whole tick (colour 0, not ramping): the pink filter is not run (its state waits;
        // the decision depends on per-tick values only, so it is the same for every block split).
        if (c == 0.0f && cStep == 0.0f)
        {
            for (int s = 0; s < n; ++s)
            {
                x ^= x << 13;
                x ^= x >> 17;
                x ^= x << 5;
                const float white = static_cast<float> (static_cast<int32_t> (x)) * (1.0f / 2147483648.0f);
                const float out = g * (ws * white);
                left[s] += out;
                right[s] += out;
                g += gStep;
            }
        }
        else
        {
            for (int s = 0; s < n; ++s)
            {
                x ^= x << 13;
                x ^= x >> 17;
                x ^= x << 5;
                const float white = static_cast<float> (static_cast<int32_t> (x)) * (1.0f / 2147483648.0f);
                b0 = p0 * b0 + g0 * white;
                b1 = p1 * b1 + g1 * white;
                b2 = p2 * b2 + g2 * white;
                const float pink = b0 + b1 + b2 + d * white;
                const float out = g * ((1.0f - c) * ws * white + c * ps * pink);
                left[s] += out;
                right[s] += out;
                g += gStep;
                c += cStep;
            }
        }

        state = x;
        pinkState = { b0, b1, b2 };
        gain = g;
        colour = c;
        samplesLeftInTick -= n;
        left += n;
        right += n;
        numSamples -= n;

        if (samplesLeftInTick == 0)
            finishRamps();
    }
}

} // namespace vocalsynth::synth
