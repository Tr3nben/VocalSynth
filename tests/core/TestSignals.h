#pragma once

// Synthetic test signals for the M2 pitch -> MIDI tests and tools/vs-eval (docs/M2-DESIGN.md 6).
// Header-only, JUCE-free, deterministic: noise uses its own PRNG (xorshift64*) instead of <random>
// distributions, whose output differs between standard libraries. Same seed -> same samples everywhere.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <numbers>
#include <span>
#include <vector>

namespace vocalsynth::test
{

using Signal = std::vector<float>;

inline std::size_t numSamplesFor (double seconds, double sampleRate)
{
    return static_cast<std::size_t> (std::llround (seconds * sampleRate));
}

/** Deterministic PRNG (xorshift64*). uniform() is in [-1, 1). */
class TestRandom
{
public:
    explicit TestRandom (uint64_t seed) noexcept : state (seed != 0 ? seed : 0x9e3779b97f4a7c15ull) {}

    uint64_t next() noexcept
    {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        return state * 0x2545f4914f6cdd1dull;
    }

    double uniform() noexcept { return static_cast<double> (next() >> 11) * (2.0 / 9007199254740992.0) - 1.0; }

private:
    uint64_t state;
};

/** Generic oscillator driven by an instantaneous-frequency function f(t) in Hz (phase-continuous).
    shape(phase 0..1) -> -1..1. */
template <typename FrequencyFn, typename ShapeFn>
Signal oscillator (double sampleRate, double seconds, float amplitude, FrequencyFn&& frequencyAt, ShapeFn&& shape)
{
    Signal out (numSamplesFor (seconds, sampleRate));
    double phase = 0.0;

    for (std::size_t i = 0; i < out.size(); ++i)
    {
        out[i] = amplitude * static_cast<float> (shape (phase));
        phase += frequencyAt (static_cast<double> (i) / sampleRate) / sampleRate;
        phase -= std::floor (phase);
    }

    return out;
}

inline double sineShape (double phase) { return std::sin (2.0 * std::numbers::pi * phase); }

/** Naive (aliasing) sawtooth -1..1; fine for f0 <= ~1 kHz at the analysis rates used in the tests. */
inline double sawShape (double phase) { return 2.0 * phase - 1.0; }

inline Signal sine (double sampleRate, double frequencyHz, double seconds, float amplitude = 0.5f)
{
    return oscillator (sampleRate, seconds, amplitude, [=] (double) { return frequencyHz; }, sineShape);
}

inline Signal saw (double sampleRate, double frequencyHz, double seconds, float amplitude = 0.5f)
{
    return oscillator (sampleRate, seconds, amplitude, [=] (double) { return frequencyHz; }, sawShape);
}

/** Sum of harmonics k = firstHarmonic..lastHarmonic of f0 with amplitudes 1/k, normalised to peak
    `amplitude`. firstHarmonic = 2 gives a "missing fundamental" signal (period is still 1/f0).
    secondHarmonicGain scales harmonic 2 (e.g. 3.0 for a strong 2nd harmonic). */
inline Signal harmonicStack (double sampleRate, double f0Hz, double seconds, int firstHarmonic, int lastHarmonic,
                             float amplitude = 0.5f, double secondHarmonicGain = 1.0)
{
    Signal out (numSamplesFor (seconds, sampleRate));
    float peak = 0.0f;

    for (std::size_t i = 0; i < out.size(); ++i)
    {
        const auto t = static_cast<double> (i) / sampleRate;
        double v = 0.0;

        for (int k = firstHarmonic; k <= lastHarmonic; ++k)
        {
            if (static_cast<double> (k) * f0Hz >= 0.5 * sampleRate)
                break;

            const auto gain = (k == 2 ? secondHarmonicGain : 1.0) / static_cast<double> (k);
            v += gain * std::sin (2.0 * std::numbers::pi * static_cast<double> (k) * f0Hz * t);
        }

        out[i] = static_cast<float> (v);
        peak = std::max (peak, std::abs (out[i]));
    }

    if (peak > 0.0f)
        for (auto& s : out)
            s *= amplitude / peak;

    return out;
}

/** Sine with sinusoidal vibrato: f(t) = f0 * 2^(depthCents/1200 * sin (2 pi rate t)). */
inline Signal vibrato (double sampleRate, double f0Hz, double depthCents, double rateHz, double seconds,
                       float amplitude = 0.5f)
{
    return oscillator (
        sampleRate, seconds, amplitude,
        [=] (double t) { return f0Hz * std::exp2 (depthCents / 1200.0 * std::sin (2.0 * std::numbers::pi * rateHz * t)); },
        sineShape);
}

/** Exponential glide (constant semitones per second) from startHz to endHz over `seconds`. */
inline Signal glissando (double sampleRate, double startHz, double endHz, double seconds, float amplitude = 0.5f)
{
    const auto octaves = std::log2 (endHz / startHz);
    return oscillator (
        sampleRate, seconds, amplitude, [=] (double t) { return startHz * std::exp2 (octaves * t / seconds); },
        sineShape);
}

inline Signal whiteNoise (double sampleRate, double seconds, float amplitude, uint64_t seed)
{
    Signal out (numSamplesFor (seconds, sampleRate));
    TestRandom rng (seed);

    for (auto& s : out)
        s = amplitude * static_cast<float> (rng.uniform());

    return out;
}

/** Pink (1/f) noise with Paul Kellet's refined filter on white noise, normalised to peak `amplitude`. */
inline Signal pinkNoise (double sampleRate, double seconds, float amplitude, uint64_t seed)
{
    Signal out (numSamplesFor (seconds, sampleRate));
    TestRandom rng (seed);
    double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
    float peak = 0.0f;

    for (auto& s : out)
    {
        const auto white = rng.uniform();
        b0 = 0.99886 * b0 + white * 0.0555179;
        b1 = 0.99332 * b1 + white * 0.0750759;
        b2 = 0.96900 * b2 + white * 0.1538520;
        b3 = 0.86650 * b3 + white * 0.3104856;
        b4 = 0.55000 * b4 + white * 0.5329522;
        b5 = -0.7616 * b5 - white * 0.0168980;
        s = static_cast<float> (b0 + b1 + b2 + b3 + b4 + b5 + b6 + white * 0.5362);
        b6 = white * 0.115926;
        peak = std::max (peak, std::abs (s));
    }

    if (peak > 0.0f)
        for (auto& s : out)
            s *= amplitude / peak;

    return out;
}

/** Breath-like noise bursts: white noise through a one-pole low-pass (~2 kHz) with a raised-cosine
    envelope, burstSeconds long, one burst every periodSeconds, silence in between. */
inline Signal noiseBursts (double sampleRate, double seconds, double burstSeconds, double periodSeconds,
                           float amplitude, uint64_t seed)
{
    Signal out (numSamplesFor (seconds, sampleRate));
    TestRandom rng (seed);
    const auto lowPass = std::exp (-2.0 * std::numbers::pi * 2000.0 / sampleRate);
    double state = 0.0;

    for (std::size_t i = 0; i < out.size(); ++i)
    {
        const auto t = static_cast<double> (i) / sampleRate;
        const auto inPeriod = std::fmod (t, periodSeconds);
        state = lowPass * state + (1.0 - lowPass) * rng.uniform();

        if (inPeriod < burstSeconds)
        {
            const auto env = 0.5 - 0.5 * std::cos (2.0 * std::numbers::pi * inPeriod / burstSeconds);
            out[i] = amplitude * static_cast<float> (4.0 * state * env); // * 4: the low-pass loses level
        }
    }

    return out;
}

inline Signal silence (double sampleRate, double seconds) { return Signal (numSamplesFor (seconds, sampleRate), 0.0f); }

/** Concatenates signals (e.g. silence + note + silence). */
inline Signal concat (std::initializer_list<std::span<const float>> parts)
{
    Signal out;

    for (auto p : parts)
        out.insert (out.end(), p.begin(), p.end());

    return out;
}

/** Adds b into a (a is extended if b is longer). */
inline void mixInto (Signal& a, std::span<const float> b, float gain = 1.0f)
{
    if (a.size() < b.size())
        a.resize (b.size(), 0.0f);

    for (std::size_t i = 0; i < b.size(); ++i)
        a[i] += gain * b[i];
}

/** Amplitude for a target RMS level in dBFS of a sine (rms = amp / sqrt 2). */
inline float sineAmplitudeForRmsDb (double rmsDb)
{
    return static_cast<float> (std::sqrt (2.0) * std::pow (10.0, rmsDb / 20.0));
}

} // namespace vocalsynth::test
