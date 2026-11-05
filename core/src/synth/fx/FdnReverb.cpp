#include "vocalsynth/synth/fx/FdnReverb.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace vocalsynth::synth
{

// 8-line FDN reverb (docs/SYNTH-DESIGN.md 11.3, 11.4; structure in the header). Per tick: size -> line
// lengths, per-line gains (RT60) and diffuser delays; damping -> one-pole coefficient; the read modulation
// (one phasor per line, rotated per tick) -> line read positions. Line delays and gains ramp linearly over
// the tick (v += step, exact target at the tick end); every decision is taken in beginTick() (3.5).
//
// All buffers share one write counter (power-of-two lengths, masked). Clearing is logical: `written`
// counts the samples written since the last clear and a read further back than that gives 0.

namespace
{
constexpr std::array<double, FdnReverb::numLines> baseLineSeconds { 0.0297, 0.0371, 0.0411, 0.0437,
                                                                    0.0539, 0.0613, 0.0719, 0.0791 };
constexpr std::array<double, FdnReverb::numDiffusers> diffuserSeconds { 0.0013, 0.0021, 0.0034, 0.0055 };
constexpr double preDelaySeconds = 0.010;
constexpr double modulationDepthSeconds = 0.0003;
constexpr double minModulationHz = 0.1;
constexpr double maxModulationHz = 0.7;
constexpr float diffuserGain = 0.6f;
constexpr float inputGain = 0.70710678f; ///< per line, signs alternating
constexpr float outputGain = 0.35355339f; ///< 1 / sqrt (8)
constexpr double minRt60Seconds = 0.3;
constexpr double maxRt60Seconds = 10.0;
constexpr double dampingLowHz = 18000.0;
constexpr double dampingHighHz = 1500.0;
constexpr float tailThreshold = 1.0e-6f;
constexpr float denormalThreshold = 1.0e-20f;

inline float clamp01 (float v) noexcept
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

inline float finiteOr (float v, float fallback) noexcept
{
    return std::isfinite (v) ? v : fallback;
}

inline void mixGains (float mix, float& dry, float& wet) noexcept
{
    dry = std::min (1.0f, 2.0f * (1.0f - mix));
    wet = std::min (1.0f, 2.0f * mix);
}

std::size_t nextPowerOfTwo (std::size_t n) noexcept
{
    std::size_t p = 1;

    while (p < n)
        p <<= 1;

    return p;
}

inline double sizeScale (float size) noexcept
{
    return 0.5 + static_cast<double> (size);
}
} // namespace

void FdnReverb::prepare (double newSampleRate)
{
    sampleRate = newSampleRate > 0.0 && std::isfinite (newSampleRate) ? newSampleRate : 48000.0;
    const auto maxScale = sizeScale (1.0f);
    const auto samples = [this] (double seconds) noexcept { return static_cast<std::size_t> (std::ceil (seconds * sampleRate)); };

    for (std::size_t i = 0; i < lines.size(); ++i)
        lines[i].assign (nextPowerOfTwo (samples (baseLineSeconds[i] * maxScale + modulationDepthSeconds) + 4), 0.0f);

    for (std::size_t j = 0; j < diffusers.size(); ++j)
        diffusers[j].assign (nextPowerOfTwo (samples (diffuserSeconds[j] * maxScale) + 4), 0.0f);

    preDelay.assign (nextPowerOfTwo (samples (preDelaySeconds) + 4), 0.0f);
    preDelaySamples = std::max (1, static_cast<int> (std::lround (preDelaySeconds * sampleRate)));
    maxReach = static_cast<int> (samples (baseLineSeconds.back() * maxScale + modulationDepthSeconds)) + 4;
    maxReach = std::max ({ maxReach, preDelaySamples + 1, static_cast<int> (samples (diffuserSeconds.back() * maxScale)) + 1 });
    fadeStepPerSample = static_cast<float> (1.0 / (switchFadeSeconds * sampleRate));
    modulationDepth = static_cast<float> (modulationDepthSeconds * sampleRate);

    for (int i = 0; i < numLines; ++i)
    {
        const auto hz = minModulationHz + (maxModulationHz - minModulationHz) * static_cast<double> (i) / (numLines - 1);
        modulationAngle[static_cast<std::size_t> (i)] = 2.0 * std::numbers::pi * hz / sampleRate;
        const auto a = modulationAngle[static_cast<std::size_t> (i)] * controlTickSamples;
        tickRotation[static_cast<std::size_t> (i)] = { std::cos (a), std::sin (a) };
    }

    reset();
}

void FdnReverb::reset() noexcept
{
    writeCounter = 0;
    onGain = onTarget = onStep = 0.0f;
    dryGain = dryTarget = 1.0f;
    wetGain = wetTarget = 0.0f;
    dryStep = wetStep = 0.0f;
    lineDelay.fill (1.0f);
    lineDelayTarget.fill (1.0f);
    lineDelayStep.fill (0.0f);
    lineGain.fill (0.0f);
    lineGainTarget.fill (0.0f);
    lineGainStep.fill (0.0f);
    cachedSize = -1.0f;
    cachedDamping = -1.0f;

    for (int i = 0; i < numLines; ++i)
    {
        // Staggered start phases (deterministic)
        const auto p = 2.0 * std::numbers::pi * static_cast<double> (i) / numLines;
        modulation[static_cast<std::size_t> (i)] = { std::cos (p), std::sin (p) };
    }

    rampLeft = 0;
    clearState();
    bypassed = true;
    idle = false;
    firstTick = true;
}

void FdnReverb::clearState() noexcept
{
    written = 0;
    damping.fill (0.0f);
    silentRun = 0;
    nanCheck = 0.0f;
}

void FdnReverb::beginTick (const ReverbSettings& settings, float mixMod, int tickSamples) noexcept
{
    const int n = std::clamp (tickSamples, 1, controlTickSamples);
    finishRamp();

    if (! std::isfinite (nanCheck))
    {
        clearState();
        ++nanResets;
    }

    nanCheck = 0.0f;

    for (auto& d : damping)
        d = std::abs (d) < denormalThreshold ? 0.0f : d;

    // Read modulation: one phasor per line, rotated per tick (renormalised, so it never drifts in level)
    for (std::size_t i = 0; i < modulation.size(); ++i)
    {
        auto rotation = tickRotation[i];

        if (n != controlTickSamples)
            rotation = { std::cos (modulationAngle[i] * n), std::sin (modulationAngle[i] * n) };

        auto [c, s] = modulation[i];
        const auto nc = c * rotation.first - s * rotation.second;
        const auto ns = c * rotation.second + s * rotation.first;
        const auto norm = 1.5 - 0.5 * (nc * nc + ns * ns);
        modulation[i] = { nc * norm, ns * norm };
    }

    const auto fade = fadeStepPerSample * static_cast<float> (n);

    if (firstTick)
        onGain = settings.on ? 1.0f : 0.0f;

    onTarget = settings.on ? std::min (1.0f, onGain + fade) : std::max (0.0f, onGain - fade);
    onTarget = onTarget < 1.0e-6f ? 0.0f : (onTarget > 1.0f - 1.0e-6f ? 1.0f : onTarget); // rounding of the steps

    if (! settings.on && onGain == 0.0f)
    {
        if (! bypassed)
            clearState();

        bypassed = true;
        idle = false;
        onTarget = 0.0f;
        firstTick = false;
        return;
    }

    bypassed = false;

    // Size -> line lengths, gains (RT60) and diffuser delays (cached while the size is unchanged)
    const auto size = clamp01 (finiteOr (settings.sizePercent, 50.0f) * 0.01f);

    if (size != cachedSize)
    {
        cachedSize = size;
        const auto scale = sizeScale (size);
        const auto rt60 = minRt60Seconds * std::pow (maxRt60Seconds / minRt60Seconds, static_cast<double> (size));

        for (std::size_t i = 0; i < lineLength.size(); ++i)
        {
            const auto length = baseLineSeconds[i] * scale * sampleRate;
            lineLength[i] = static_cast<float> (length);
            lineGainTarget[i] = static_cast<float> (std::pow (10.0, -3.0 * length / (rt60 * sampleRate)));
        }

        for (std::size_t j = 0; j < diffuserDelay.size(); ++j)
            diffuserDelay[j] = std::max (1, static_cast<int> (std::lround (diffuserSeconds[j] * scale * sampleRate)));

        longestLine = static_cast<int> (std::ceil (lineLength.back() + modulationDepth)) + 2;
    }

    const auto damp = clamp01 (finiteOr (settings.dampingPercent, 50.0f) * 0.01f);

    if (damp != cachedDamping)
    {
        cachedDamping = damp;
        const auto fc = std::min (dampingLowHz * std::pow (dampingHighHz / dampingLowHz, static_cast<double> (damp)),
                                  0.45 * sampleRate);
        dampingCoefficient = static_cast<float> (1.0 - std::exp (-2.0 * std::numbers::pi * fc / sampleRate));
    }

    // Tail end: input 0 and every line write below -120 dBFS for 4 x the longest line
    if (! idle && silentRun >= 4 * longestLine)
    {
        idle = true;
        clearState();
    }

    for (std::size_t i = 0; i < lineDelayTarget.size(); ++i)
        lineDelayTarget[i] = lineLength[i] + modulationDepth * static_cast<float> (modulation[i].second);

    const auto mix = clamp01 (finiteOr (settings.mixPercent, 25.0f) * 0.01f + finiteOr (mixMod, 0.0f));
    mixGains (mix, dryTarget, wetTarget);

    if (firstTick || idle)
    {
        lineDelay = lineDelayTarget;
        lineGain = lineGainTarget;
        dryGain = dryTarget;
        wetGain = wetTarget;
        onGain = onTarget;
    }

    const auto inv = 1.0f / static_cast<float> (n);

    for (std::size_t i = 0; i < lineDelay.size(); ++i)
    {
        lineDelayStep[i] = (lineDelayTarget[i] - lineDelay[i]) * inv;
        lineGainStep[i] = (lineGainTarget[i] - lineGain[i]) * inv;
    }

    dryStep = (dryTarget - dryGain) * inv;
    wetStep = (wetTarget - wetGain) * inv;
    onStep = (onTarget - onGain) * inv;
    rampLeft = n;
    firstTick = false;
}

void FdnReverb::finishRamp() noexcept
{
    lineDelay = lineDelayTarget;
    lineGain = lineGainTarget;
    lineDelayStep.fill (0.0f);
    lineGainStep.fill (0.0f);
    dryGain = dryTarget;
    wetGain = wetTarget;
    onGain = onTarget;
    dryStep = wetStep = onStep = 0.0f;
    rampLeft = 0;
}

void FdnReverb::process (float* left, float* right, int numSamples) noexcept
{
    if (bypassed || numSamples <= 0)
        return;

    if (idle)
    {
        int first = 0;

        while (first < numSamples && left[first] == 0.0f && right[first] == 0.0f)
            ++first;

        if (first > 0)
        {
            const int r = std::min (first, rampLeft);

            if (r > 0)
            {
                rampLeft -= r;

                if (rampLeft == 0)
                    finishRamp();
            }
        }

        if (first == numSamples)
            return;

        idle = false;
        silentRun = 0;
        left += first;
        right += first;
        numSamples -= first;
    }

    while (numSamples > 0)
    {
        const int n = rampLeft > 0 ? std::min (numSamples, rampLeft) : numSamples;

        if (written >= maxReach)
            run<false> (left, right, n);
        else
            run<true> (left, right, n);

        left += n;
        right += n;
        numSamples -= n;

        if (rampLeft > 0)
        {
            rampLeft -= n;

            if (rampLeft == 0)
                finishRamp();
        }
    }
}

template <bool checked>
void FdnReverb::run (float* left, float* right, int n) noexcept
{
    uint32_t w = writeCounter;
    int valid = written;
    float dg = dryGain, wg = wetGain, on = onGain;
    const float sdg = dryStep, swg = wetStep, son = onStep;
    const float damp = dampingCoefficient;
    auto delays = lineDelay;
    auto gains = lineGain;
    const auto delaySteps = lineDelayStep;
    const auto gainSteps = lineGainStep;
    auto lp = damping;
    int run = silentRun;
    float acc = nanCheck;

    // Read distance k (1 = the previous sample) of a power-of-two buffer; 0 beyond the samples written
    const auto read = [&] (const std::vector<float>& buffer, uint32_t writePos, int k) noexcept
    {
        const auto mask = static_cast<uint32_t> (buffer.size() - 1);
        const float v = buffer[(writePos - static_cast<uint32_t> (k)) & mask];

        if constexpr (checked)
            return k <= valid ? v : 0.0f;
        else
            return v;
    };

    for (int s = 0; s < n; ++s)
    {
        const float l = left[s], r = right[s];
        const float mono = 0.5f * (l + r);

        // Pre-delay and the series all-pass diffusers
        float x = read (preDelay, w, preDelaySamples);
        preDelay[w & static_cast<uint32_t> (preDelay.size() - 1)] = mono;

        for (std::size_t j = 0; j < diffusers.size(); ++j)
        {
            auto& buffer = diffusers[j];
            const float delayed = read (buffer, w, diffuserDelay[j]);
            const float into = x + diffuserGain * delayed;
            buffer[w & static_cast<uint32_t> (buffer.size() - 1)] = into;
            x = delayed - diffuserGain * into;
        }

        // Line reads (modulated, linear interpolation) and damping
        std::array<float, numLines> y {};

        for (std::size_t i = 0; i < y.size(); ++i)
        {
            delays[i] += delaySteps[i];
            gains[i] += gainSteps[i];
            const int di = static_cast<int> (delays[i]);
            const float f = delays[i] - static_cast<float> (di);
            const float a = read (lines[i], w, di);
            const float b = read (lines[i], w, di + 1);
            lp[i] += damp * ((a + f * (b - a)) - lp[i]);
            y[i] = lp[i];
        }

        // Householder feedback y_i - (sum y) / 4 (on the gained reads) plus the diffused input
        std::array<float, numLines> v {};
        float sum = 0.0f;

        for (std::size_t i = 0; i < v.size(); ++i)
        {
            v[i] = gains[i] * y[i];
            sum += v[i];
        }

        const float quarter = 0.25f * sum;
        const float in = inputGain * x;
        float loudest = 0.0f;

        for (std::size_t i = 0; i < v.size(); ++i)
        {
            const float value = v[i] - quarter + ((i & 1u) == 0 ? in : -in);
            lines[i][w & static_cast<uint32_t> (lines[i].size() - 1)] = value;
            loudest = std::max (loudest, std::abs (value));
        }

        w += 1u;

        if constexpr (checked)
            valid = std::min (valid + 1, maxReach);

        const float wetL = outputGain * ((y[0] - y[2]) + (y[4] - y[6]));
        const float wetR = outputGain * ((y[1] - y[3]) + (y[5] - y[7]));
        dg += sdg;
        wg += swg;
        on += son;
        left[s] = l + on * ((l * dg + wetL * wg) - l);
        right[s] = r + on * ((r * dg + wetR * wg) - r);

        run = (mono == 0.0f && loudest < tailThreshold) ? run + 1 : 0;
        acc += sum + in;
    }

    writeCounter = w;
    written = checked ? valid : written;
    dryGain = dg;
    wetGain = wg;
    onGain = on;
    lineDelay = delays;
    lineGain = gains;
    damping = lp;
    silentRun = std::min (run, 1 << 30);
    nanCheck = acc;
}

bool FdnReverb::isBypassed() const noexcept
{
    return bypassed;
}

} // namespace vocalsynth::synth
