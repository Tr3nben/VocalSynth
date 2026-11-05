#include "vocalsynth/synth/fx/StereoDelay.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace vocalsynth::synth
{

// Stereo / ping-pong delay (docs/SYNTH-DESIGN.md 11.2, 11.4). The delay time moves towards its target by at
// most one sample per sample (tape-like); feedback, mix and the on/off fade ramp linearly over the tick.
// Every decision (bypass, tail end, NaN reset, denormal flush) is taken in beginTick() from per-sample
// counters, so any split of the host block gives the same output (3.5).
//
// Clearing is logical (as in the Chorus): `written` counts the samples written since the last clear and a
// read further back than that gives 0, so no clear ever touches the 2 x 2 s of line memory on the audio thread.

namespace
{
constexpr float highPassHz = 120.0f;
constexpr float lowPassHz = 6000.0f;
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

inline float softLimit (float u) noexcept
{
    return u / std::sqrt (1.0f + u * u);
}

inline float flushDenormal (float v) noexcept
{
    return std::abs (v) < denormalThreshold ? 0.0f : v;
}
} // namespace

void StereoDelay::prepare (double newSampleRate)
{
    sampleRate = newSampleRate > 0.0 && std::isfinite (newSampleRate) ? newSampleRate : 48000.0;
    length = static_cast<int> (std::ceil (maxDelaySeconds * sampleRate)) + 4;

    for (auto& l : lines)
        l.assign (static_cast<std::size_t> (length), 0.0f);

    maxDelaySamples = static_cast<float> (length - 3);
    fadeStepPerSample = static_cast<float> (1.0 / (switchFadeSeconds * sampleRate));
    highPassCoefficient = static_cast<float> (std::exp (-2.0 * std::numbers::pi * highPassHz / sampleRate));
    lowPassCoefficient = static_cast<float> (1.0 - std::exp (-2.0 * std::numbers::pi
                                                              * std::min (static_cast<double> (lowPassHz), 0.45 * sampleRate)
                                                              / sampleRate));
    reset();
}

void StereoDelay::reset() noexcept
{
    writeIndex = 0;
    written = 0;
    delay = delayTarget = 1.0f;
    feedback = feedbackTarget = feedbackStep = 0.0f;
    onGain = onTarget = onStep = 0.0f;
    dryGain = dryTarget = 1.0f;
    wetGain = wetTarget = 0.0f;
    dryStep = wetStep = 0.0f;
    pingPong = false;
    rampLeft = 0;
    clearState();
    bypassed = true;
    idle = false;
    firstTick = true;
}

void StereoDelay::clearState() noexcept
{
    written = 0;
    silentRun = 0;
    nanCheck = 0.0f;
    filter = {};
}

float StereoDelay::targetDelaySamples (const DelaySettings& settings, const SynthTransport& transport) const noexcept
{
    auto ms = finiteOr (settings.timeMs, 375.0f);

    if (settings.sync != TempoDivision::off)
    {
        const auto bpm = transport.bpm > 0.0 && std::isfinite (transport.bpm) ? transport.bpm : 120.0;
        const auto qpb = transport.quarterNotesPerBar > 0.0 && std::isfinite (transport.quarterNotesPerBar)
                             ? transport.quarterNotesPerBar
                             : 4.0;
        ms = static_cast<float> (tempoDivisionQuarterNotes (settings.sync, qpb) * 60000.0 / bpm);
    }

    ms = std::clamp (ms, limits::delayTimeMs.min, limits::delayTimeMs.max);
    return std::clamp (static_cast<float> (static_cast<double> (ms) * 0.001 * sampleRate), 1.0f, maxDelaySamples);
}

void StereoDelay::beginTick (const DelaySettings& settings, const SynthTransport& transport, float mixMod,
                             int tickSamples) noexcept
{
    const int n = std::clamp (tickSamples, 1, controlTickSamples);
    finishRamp();

    if (! std::isfinite (nanCheck))
    {
        clearState();
        ++nanResets;
    }

    nanCheck = 0.0f;

    for (auto& f : filter)
    {
        f.highPassIn = flushDenormal (f.highPassIn);
        f.highPassOut = flushDenormal (f.highPassOut);
        f.lowPass = flushDenormal (f.lowPass);
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
    delayTarget = targetDelaySamples (settings, transport);

    // Tail end: input 0 and every written sample below -120 dBFS for one delay time
    const auto span = static_cast<int> (std::ceil (std::max (delay, delayTarget))) + 2;

    if (! idle && silentRun >= span)
    {
        idle = true;
        clearState();
    }

    pingPong = settings.pingPong;
    feedbackTarget = std::clamp (finiteOr (settings.feedbackPercent, 35.0f), limits::delayFeedbackPercent.min,
                                 limits::delayFeedbackPercent.max)
                     * 0.01f;
    const auto mix = clamp01 (finiteOr (settings.mixPercent, 25.0f) * 0.01f + finiteOr (mixMod, 0.0f));
    mixGains (mix, dryTarget, wetTarget);

    if (firstTick || idle)
    {
        delay = delayTarget;
        feedback = feedbackTarget;
        dryGain = dryTarget;
        wetGain = wetTarget;
        onGain = onTarget;
    }

    const auto inv = 1.0f / static_cast<float> (n);
    feedbackStep = (feedbackTarget - feedback) * inv;
    dryStep = (dryTarget - dryGain) * inv;
    wetStep = (wetTarget - wetGain) * inv;
    onStep = (onTarget - onGain) * inv;
    rampLeft = n;
    firstTick = false;
}

void StereoDelay::finishRamp() noexcept
{
    feedback = feedbackTarget;
    dryGain = dryTarget;
    wetGain = wetTarget;
    onGain = onTarget;
    feedbackStep = dryStep = wetStep = onStep = 0.0f;
    rampLeft = 0;
}

void StereoDelay::process (float* left, float* right, int numSamples) noexcept
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

        const bool gliding = delay != delayTarget;

        if (pingPong)
            gliding ? run<true, true> (left, right, n) : run<true, false> (left, right, n);
        else
            gliding ? run<false, true> (left, right, n) : run<false, false> (left, right, n);

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

template <bool pingPongMode, bool gliding>
void StereoDelay::run (float* left, float* right, int n) noexcept
{
    float* lineL = lines[0].data();
    float* lineR = lines[1].data();
    const int len = length;
    int w = writeIndex;
    int valid = written;
    float d = delay;
    const float dT = delayTarget;
    float fb = feedback, dg = dryGain, wg = wetGain, on = onGain;
    const float sfb = feedbackStep, sdg = dryStep, swg = wetStep, son = onStep;
    const float hpA = highPassCoefficient, lpB = lowPassCoefficient;
    auto fl = filter[0], fr = filter[1];
    int run = silentRun;
    float acc = nanCheck;

    const float lpA = 1.0f - lpB;
    int di = static_cast<int> (d);
    float f = d - static_cast<float> (di);

    for (int i = 0; i < n; ++i)
    {
        // Read position: at most one sample per sample towards the target (tape-like). A steady time
        // (d == target) skips the slew: d += 0 would give the same values, without the per-sample chain.
        if constexpr (gliding)
        {
            d += std::clamp (dT - d, -1.0f, 1.0f);
            di = static_cast<int> (d);
            f = d - static_cast<float> (di);
        }

        int i0 = w - di;
        i0 += i0 < 0 ? len : 0;
        int i1 = i0 - 1;
        i1 += i1 < 0 ? len : 0;
        const bool v0 = di <= valid, v1 = di + 1 <= valid;
        const float a0 = v0 ? lineL[i0] : 0.0f, a1 = v1 ? lineL[i1] : 0.0f;
        const float b0 = v0 ? lineR[i0] : 0.0f, b1 = v1 ? lineR[i1] : 0.0f;
        const float wetL = a0 + f * (a1 - a0);
        const float wetR = b0 + f * (b1 - b0);

        // Feedback: high-pass 120 Hz, low-pass 6 kHz (one-poles), soft limiter
        fb += sfb;
        const float hl = hpA * (fl.highPassOut + wetL - fl.highPassIn);
        fl.highPassIn = wetL;
        fl.highPassOut = hl;
        fl.lowPass = lpA * fl.lowPass + lpB * hl;
        const float hr = hpA * (fr.highPassOut + wetR - fr.highPassIn);
        fr.highPassIn = wetR;
        fr.highPassOut = hr;
        fr.lowPass = lpA * fr.lowPass + lpB * hr;
        const float backL = softLimit (fb * fl.lowPass);
        const float backR = softLimit (fb * fr.lowPass);

        const float l = left[i], r = right[i];
        float inL, inR;

        if constexpr (pingPongMode)
        {
            inL = 0.5f * (l + r) + backR; // mono sum into the left line, L -> R -> L
            inR = backL;
        }
        else
        {
            inL = l + backL;
            inR = r + backR;
        }

        lineL[w] = inL;
        lineR[w] = inR;
        w = w + 1 < len ? w + 1 : 0;
        valid = std::min (valid + 1, len);

        dg += sdg;
        wg += swg;
        on += son;
        left[i] = l + on * ((l * dg + wetL * wg) - l);
        right[i] = r + on * ((r * dg + wetR * wg) - r);

        const bool quiet = l == 0.0f && r == 0.0f && std::abs (inL) < tailThreshold && std::abs (inR) < tailThreshold;
        run = quiet ? run + 1 : 0;
        acc += inL + inR;
    }

    writeIndex = w;
    written = valid;
    delay = d;
    feedback = fb;
    dryGain = dg;
    wetGain = wg;
    onGain = on;
    filter[0] = fl;
    filter[1] = fr;
    silentRun = std::min (run, len);
    nanCheck = acc;
}

bool StereoDelay::isBypassed() const noexcept
{
    return bypassed;
}

} // namespace vocalsynth::synth
