#include "vocalsynth/ExpressionTracker.h"

#include "vocalsynth/synth/RealFft.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace vocalsynth
{

// Voice expression once per analysis hop (docs/SYNTH-DESIGN.md 16.2, formulas in the header). Everything is a
// function of the hop sequence only (one-poles at the hop rate, the spectrum every 2nd hop counted from
// reset()), so the frames never depend on the host block split.

namespace
{
constexpr float spectrumLowHz = 80.0f;
constexpr float spectrumHighHz = 8000.0f;
constexpr float powerFloor = 1.0e-20f;
constexpr float levelSnap = 1.0e-4f; ///< a released level below this is 0 (brightness / noisiness follow)

inline float clamp01 (float v) noexcept
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

inline float finiteOr (float v, float fallback) noexcept
{
    return std::isfinite (v) ? v : fallback;
}

inline float onePoleCoefficient (double seconds, double hopSeconds) noexcept
{
    return static_cast<float> (1.0 - std::exp (-hopSeconds / seconds));
}
} // namespace

ExpressionTracker::ExpressionTracker() = default;
ExpressionTracker::~ExpressionTracker() = default;
ExpressionTracker::ExpressionTracker (ExpressionTracker&&) noexcept = default;
ExpressionTracker& ExpressionTracker::operator= (ExpressionTracker&&) noexcept = default;

void ExpressionTracker::prepare (double analysisSampleRate, int hop)
{
    analysisRate = analysisSampleRate > 0.0 && std::isfinite (analysisSampleRate) ? analysisSampleRate : 16000.0;
    hopSamples = hop > 0 ? hop : 1;
    if (fft == nullptr)
        fft = std::make_unique<synth::RealFft> (fftOrder);
    windowed.assign (static_cast<std::size_t> (fftSize), 0.0f);
    power.assign (static_cast<std::size_t> (fftSize / 2 + 1), 0.0f);
    hann.assign (static_cast<std::size_t> (fftSize), 0.0f);
    hannLength = 0;

    const auto hopSeconds = static_cast<double> (hopSamples) / analysisRate;
    attackCoefficient = onePoleCoefficient (levelAttackSeconds, hopSeconds);
    releaseCoefficient = onePoleCoefficient (levelReleaseSeconds, hopSeconds);
    smoothingCoefficient = onePoleCoefficient (smoothingSeconds, hopSeconds);

    const auto binHz = analysisRate / static_cast<double> (fftSize);
    const auto high = std::min (static_cast<double> (spectrumHighHz), 0.45 * analysisRate);
    firstBin = std::max (1, static_cast<int> (std::ceil (static_cast<double> (spectrumLowHz) / binHz)));
    lastBin = std::clamp (static_cast<int> (std::floor (high / binHz)), firstBin, fftSize / 2);
    binHertz = static_cast<float> (binHz);
    reset();
}

void ExpressionTracker::reset() noexcept
{
    hopCounter = 0;
    current = {};
    brightnessTarget = 0.0f;
    flatnessTarget = 0.0f;
}

void ExpressionTracker::updateHann (int length) noexcept
{
    if (length == hannLength)
        return;

    hannLength = length;

    for (int i = 0; i < length; ++i)
        hann[static_cast<std::size_t> (i)] = static_cast<float> (
            0.5 - 0.5 * std::cos (2.0 * std::numbers::pi * static_cast<double> (i) / static_cast<double> (length)));
}

void ExpressionTracker::analyseSpectrum (std::span<const float> window) noexcept
{
    // Newest fftSize samples, Hann over the samples present, zero-padded at the end
    const auto available = std::min (window.size(), static_cast<std::size_t> (fftSize));
    const auto used = static_cast<int> (available);
    const auto first = window.size() - available;
    updateHann (used);
    std::fill (windowed.begin(), windowed.end(), 0.0f);

    for (int i = 0; i < used; ++i)
    {
        const auto x = window[first + static_cast<std::size_t> (i)];
        windowed[static_cast<std::size_t> (i)] = std::isfinite (x) ? x * hann[static_cast<std::size_t> (i)] : 0.0f;
    }

    if (fft == nullptr)
        return; // not prepared

    fft->powerSpectrum (windowed, power);
    double sum = 0.0, weighted = 0.0, logSum = 0.0;

    for (int k = firstBin; k <= lastBin; ++k)
    {
        const auto p = static_cast<double> (power[static_cast<std::size_t> (k)]) + static_cast<double> (powerFloor);
        sum += p;
        weighted += p * static_cast<double> (k);
        logSum += std::log (p);
    }

    const auto bins = static_cast<double> (lastBin - firstBin + 1);
    const auto mean = sum / bins;
    const auto centroidHz = weighted / sum * static_cast<double> (binHertz);
    const auto flatness = std::exp (logSum / bins) / mean;

    brightnessTarget = clamp01 (static_cast<float> (std::log2 (centroidHz / static_cast<double> (brightnessLowHz))
                                                    / std::log2 (static_cast<double> (brightnessHighHz / brightnessLowHz))));
    flatnessTarget = clamp01 (static_cast<float> (flatness) / 0.5f);

    if (! std::isfinite (brightnessTarget))
        brightnessTarget = 0.0f;

    if (! std::isfinite (flatnessTarget))
        flatnessTarget = 0.0f;
}

ExpressionFrame ExpressionTracker::processHop (const ExpressionTrackerInput& input) noexcept
{
    // Level: rmsDb between the gate (0) and -6 dBFS (1), attack 5 ms / release 50 ms
    const auto gate = std::min (finiteOr (input.gateThresholdDb, -35.0f), levelTopDb - 1.0f);
    const auto x = clamp01 ((finiteOr (input.rmsDb, -120.0f) - gate) / (levelTopDb - gate));
    current.level += (x > current.level ? attackCoefficient : releaseCoefficient) * (x - current.level);

    if (x == 0.0f && current.level < levelSnap)
        current.level = 0.0f;

    if (hopCounter % spectrumEveryHops == 0)
        analyseSpectrum (input.window);

    hopCounter = (hopCounter + 1) % spectrumEveryHops;

    // Brightness / noisiness (0 without a voice), pitch from the note centre; smoothed 10 ms
    const bool sounding = current.level > 0.0f;
    const auto confidence = clamp01 (finiteOr (input.confidence, 0.0f));
    const auto brightness = sounding ? brightnessTarget : 0.0f;
    const auto noisiness = sounding ? clamp01 (0.5f * flatnessTarget + 0.5f * (1.0f - confidence)) : 0.0f;
    float cents = 0.0f;

    if (input.sungNote >= 0 && input.frequencyHz > 0.0f && std::isfinite (input.frequencyHz))
    {
        const auto centre = 440.0 * std::exp2 ((static_cast<double> (input.sungNote) - 69.0) / 12.0);
        cents = std::clamp (static_cast<float> (1200.0 * std::log2 (static_cast<double> (input.frequencyHz) / centre)),
                            -100.0f, 100.0f);
    }

    current.brightness += smoothingCoefficient * (brightness - current.brightness);
    current.noisiness += smoothingCoefficient * (noisiness - current.noisiness);
    current.pitchCents += smoothingCoefficient * (cents - current.pitchCents);

    // Settled values are exact (a held 0 stays 0, no denormal tail)
    if (std::abs (brightness - current.brightness) < 1.0e-6f)
        current.brightness = brightness;

    if (std::abs (noisiness - current.noisiness) < 1.0e-6f)
        current.noisiness = noisiness;

    if (std::abs (cents - current.pitchCents) < 1.0e-4f)
        current.pitchCents = cents;

    auto frame = current;
    frame.sampleOffset = 0;
    return frame;
}

} // namespace vocalsynth
