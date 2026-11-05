#include "vocalsynth/synth/fx/Chorus.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace vocalsynth::synth
{

// Stereo chorus (docs/SYNTH-DESIGN.md 11.1, 11.4). One line holds the mono sum; the left tap reads it at
// 7 ms + depth sin (2 pi p), the right one at 11 ms + depth cos (2 pi p). The LFO phase advances per tick and
// the tap delays, the mix gains and the on/off fade ramp linearly over the tick (v += step per sample, set
// exactly to the target at the tick end), so any split of a tick gives the same operations (3.5).
//
// "Cleared" is logical: `written` counts the samples written since the last clear and a tap further back
// than that reads 0. A reset (the processor resets the engine in every bypassed block) therefore costs
// nothing and never touches the line memory on the audio thread.

namespace
{
constexpr double leftBaseSeconds = 0.007;
constexpr double rightBaseSeconds = 0.011;
constexpr double maxDepthSeconds = 0.004;

inline float clamp01 (float v) noexcept
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

inline float finiteOr (float v, float fallback) noexcept
{
    return std::isfinite (v) ? v : fallback;
}

/** Dual-linear mix law: dry stays at full level up to 50 %, wet reaches full level at 50 %. */
inline void mixGains (float mix, float& dry, float& wet) noexcept
{
    dry = std::min (1.0f, 2.0f * (1.0f - mix));
    wet = std::min (1.0f, 2.0f * mix);
}

int nextPowerOfTwo (int n) noexcept
{
    int p = 1;

    while (p < n)
        p <<= 1;

    return p;
}
} // namespace

void Chorus::prepare (double newSampleRate)
{
    sampleRate = newSampleRate > 0.0 && std::isfinite (newSampleRate) ? newSampleRate : 48000.0;
    const auto length = static_cast<int> (std::ceil (maxDelaySeconds * sampleRate)) + 4;
    line.assign (static_cast<std::size_t> (nextPowerOfTwo (length)), 0.0f);
    mask = static_cast<int> (line.size()) - 1;
    reach = static_cast<int> (std::ceil ((rightBaseSeconds + maxDepthSeconds) * sampleRate)) + 2;
    fadeStepPerSample = static_cast<float> (1.0 / (switchFadeSeconds * sampleRate));
    reset();
}

void Chorus::reset() noexcept
{
    writeIndex = 0;
    written = 0;
    lfoPhase = 0.0;
    onGain = onTarget = onStep = 0.0f;
    delayLeft = delayLeftTarget = delayLeftStep = 0.0f;
    delayRight = delayRightTarget = delayRightStep = 0.0f;
    dryGain = dryTarget = 1.0f;
    wetGain = wetTarget = 0.0f;
    dryStep = wetStep = 0.0f;
    rampLeft = 0;
    silentRun = 0;
    nanCheck = 0.0f;
    bypassed = true;
    idle = false;
    firstTick = true;
}

void Chorus::clearLines() noexcept
{
    written = 0;
    silentRun = 0;
    nanCheck = 0.0f;
}

void Chorus::beginTick (const ChorusSettings& settings, float mixMod, int tickSamples) noexcept
{
    const int n = std::clamp (tickSamples, 1, controlTickSamples);
    finishRamp();

    if (! std::isfinite (nanCheck))
    {
        clearLines();
        ++nanResets;
    }

    nanCheck = 0.0f;

    // On / off: 10 ms linear fade; the first tick after reset() jumps
    const auto fade = fadeStepPerSample * static_cast<float> (n);

    if (firstTick)
        onGain = settings.on ? 1.0f : 0.0f;

    onTarget = settings.on ? std::min (1.0f, onGain + fade) : std::max (0.0f, onGain - fade);
    onTarget = onTarget < 1.0e-6f ? 0.0f : (onTarget > 1.0f - 1.0e-6f ? 1.0f : onTarget); // rounding of the steps

    if (! settings.on && onGain == 0.0f)
    {
        if (! bypassed)
            clearLines();

        bypassed = true;
        idle = false;
        onTarget = 0.0f;
        firstTick = false;
        return;
    }

    bypassed = false;

    // Tail end (no feedback): the input was 0 for longer than the longest tap -> the taps read only zeros
    if (! idle && silentRun >= reach)
    {
        idle = true;
        clearLines();
    }

    // LFO (advanced per tick) and tap delays at the tick end
    const auto rate = std::clamp (finiteOr (settings.rateHz, 0.8f), limits::chorusRateHz.min, limits::chorusRateHz.max);
    lfoPhase += static_cast<double> (rate) * static_cast<double> (n) / sampleRate;
    lfoPhase -= std::floor (lfoPhase);
    const auto depth = clamp01 (finiteOr (settings.depthPercent, 50.0f) * 0.01f) * maxDepthSeconds * sampleRate;
    const auto angle = 2.0 * std::numbers::pi * lfoPhase;
    delayLeftTarget = static_cast<float> (leftBaseSeconds * sampleRate + depth * std::sin (angle));
    delayRightTarget = static_cast<float> (rightBaseSeconds * sampleRate + depth * std::cos (angle));

    const auto mix = clamp01 (finiteOr (settings.mixPercent, 50.0f) * 0.01f + finiteOr (mixMod, 0.0f));
    mixGains (mix, dryTarget, wetTarget);

    if (firstTick || idle)
    {
        // Nothing audible depends on the ramp: jump
        delayLeft = delayLeftTarget;
        delayRight = delayRightTarget;
        dryGain = dryTarget;
        wetGain = wetTarget;
        onGain = onTarget;
    }

    const auto inv = 1.0f / static_cast<float> (n);
    delayLeftStep = (delayLeftTarget - delayLeft) * inv;
    delayRightStep = (delayRightTarget - delayRight) * inv;
    dryStep = (dryTarget - dryGain) * inv;
    wetStep = (wetTarget - wetGain) * inv;
    onStep = (onTarget - onGain) * inv;
    rampLeft = n;
    firstTick = false;
}

void Chorus::finishRamp() noexcept
{
    delayLeft = delayLeftTarget;
    delayRight = delayRightTarget;
    dryGain = dryTarget;
    wetGain = wetTarget;
    onGain = onTarget;
    delayLeftStep = delayRightStep = dryStep = wetStep = onStep = 0.0f;
    rampLeft = 0;
}

void Chorus::process (float* left, float* right, int numSamples) noexcept
{
    if (bypassed || numSamples <= 0)
        return;

    if (idle)
    {
        // Tail ended: the output is the (zero) input until a sample is not 0; from there it runs again
        int first = 0;

        while (first < numSamples && left[first] == 0.0f && right[first] == 0.0f)
            ++first;

        if (first > 0)
            advanceIdle (first);

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

        if (written >= reach)
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

void Chorus::advanceIdle (int n) noexcept
{
    // Ramps advance as if processed (their values only matter once the input returns)
    const int r = std::min (n, rampLeft);

    if (r > 0)
    {
        rampLeft -= r;

        if (rampLeft == 0)
            finishRamp();
    }
}

template <bool checked>
void Chorus::run (float* left, float* right, int n) noexcept
{
    float* buffer = line.data();
    const int m = mask;
    int w = writeIndex;
    int valid = written;
    float dl = delayLeft, dr = delayRight, dg = dryGain, wg = wetGain, on = onGain;
    const float sdl = delayLeftStep, sdr = delayRightStep, sdg = dryStep, swg = wetStep, son = onStep;
    int run = silentRun;
    float acc = nanCheck;

    const auto tap = [&] (float d) noexcept
    {
        const int di = static_cast<int> (d);
        const float f = d - static_cast<float> (di);
        float a = buffer[(w - di) & m];
        float b = buffer[(w - di - 1) & m];

        if constexpr (checked)
        {
            a = di <= valid ? a : 0.0f;
            b = di + 1 <= valid ? b : 0.0f;
        }

        return a + f * (b - a);
    };

    for (int i = 0; i < n; ++i)
    {
        const float l = left[i], r = right[i];
        const float mono = 0.5f * (l + r);
        dl += sdl;
        dr += sdr;
        dg += sdg;
        wg += swg;
        on += son;
        const float wetL = tap (dl);
        const float wetR = tap (dr);
        buffer[w] = mono;
        w = (w + 1) & m;

        if constexpr (checked)
            valid = std::min (valid + 1, reach);

        left[i] = l + on * ((l * dg + wetL * wg) - l);
        right[i] = r + on * ((r * dg + wetR * wg) - r);
        run = mono == 0.0f ? run + 1 : 0;
        acc += mono;
    }

    writeIndex = w;
    written = checked ? valid : written;
    delayLeft = dl;
    delayRight = dr;
    dryGain = dg;
    wetGain = wg;
    onGain = on;
    silentRun = std::min (run, reach);
    nanCheck = acc;
}

bool Chorus::isBypassed() const noexcept
{
    return bypassed;
}

} // namespace vocalsynth::synth
