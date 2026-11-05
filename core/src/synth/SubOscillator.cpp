#include "vocalsynth/synth/SubOscillator.h"

#include "vocalsynth/synth/RealFft.h"
#include "vocalsynth/synth/WavetableBuilder.h"
#include "vocalsynth/synth/WavetableOscillator.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>
#include <vector>

namespace vocalsynth::synth
{

namespace
{
/** One band-limited 2048-sample cycle from sine-series amplitudes b[k] (x = sum b[k] sin (2 pi k t)). */
std::vector<float> cycleFromSineSeries (const std::vector<double>& b)
{
    constexpr int n = Wavetable::frameSize;
    RealFft fft (11);
    std::vector<std::complex<double>> bins (n / 2 + 1);

    for (std::size_t k = 1; k < b.size() && k < static_cast<std::size_t> (n / 2); ++k)
        bins[k] = { 0.0, -0.5 * n * b[k] }; // sin -> X[k] = -i N / 2 b

    std::vector<double> time (n);
    fft.inverse (bins, time);
    return { time.begin(), time.end() };
}

std::unique_ptr<Wavetable> buildShape (SubShape shape)
{
    if (shape == SubShape::sine)
        return WavetableBuilder::makeSineTable();

    std::vector<double> b (Wavetable::frameSize / 2, 0.0);

    for (std::size_t k = 1; k < b.size(); k += 2)
    {
        const double kd = static_cast<double> (k);

        if (shape == SubShape::triangle)
            b[k] = 8.0 / (std::numbers::pi * std::numbers::pi * kd * kd) * ((k / 2) % 2 == 0 ? 1.0 : -1.0);
        else
            b[k] = 4.0 / (std::numbers::pi * kd);
    }

    const auto cycle = cycleFromSineSeries (b);
    auto result = WavetableBuilder::build (shape == SubShape::triangle ? "Sub Triangle" : "Sub Square", cycle,
                                           Wavetable::frameSize);
    return result.ok() ? std::move (result.table) : WavetableBuilder::makeSineTable();
}

/** The process-wide shape tables (built on the first call, thread-safe static initialisation; never on the
    audio thread). ~3 x 90 KB for the whole process (docs/SYNTH-DESIGN.md 6.2). */
const std::array<std::unique_ptr<Wavetable>, numSubShapes>& sharedTables()
{
    static const std::array<std::unique_ptr<Wavetable>, numSubShapes> shared {
        buildShape (SubShape::sine), buildShape (SubShape::triangle), buildShape (SubShape::square)
    };
    return shared;
}

/** Same rule as WavetableOscillator (6.1): up at 0.45, down again only at 0.40. */
int chooseMipLevel (int current, double maxIncrement) noexcept
{
    constexpr int top = Wavetable::numMipLevels - 1;
    int up = 0;

    while (up < top && Wavetable::maxHarmonic (up) * maxIncrement > WavetableOscillator::maxIncrementForLevel)
        ++up;

    if (up >= current)
        return up;

    int down = up;

    while (down < top && Wavetable::maxHarmonic (down) * maxIncrement > WavetableOscillator::hysteresisIncrementForLevel)
        ++down;

    return std::min (down, current);
}

inline float hermite (const float* p, double x) noexcept
{
    const int i = static_cast<int> (x);
    const float f = static_cast<float> (x - static_cast<double> (i));
    const float* a = p + i;
    const float xm1 = a[-1];
    const float x0 = a[0];
    const float x1 = a[1];
    const float x2 = a[2];
    const float c1 = 0.5f * (x1 - xm1);
    const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * f + c2) * f + c1) * f + x0;
}
} // namespace

void SubOscillator::prepare (double newSampleRate)
{
    sampleRate = newSampleRate > 0.0 && std::isfinite (newSampleRate) ? newSampleRate : 48000.0;
    tables = &sharedTables();
    reset();
}

void SubOscillator::reset() noexcept
{
    shape = SubShape::sine;
    phase = 0.0;
    increment = incrementStep = incrementTarget = 0.0;
    gain = gainStep = gainTarget = 0.0f;
    levelMix = 1.0f;
    levelMixStep = 0.0f;
    mipLevel = fromMipLevel = 0;
    samplesLeftInTick = 0;
    jumpToTargets = true;
}

void SubOscillator::restart() noexcept
{
    phase = 0.0;
    gain = gainStep = gainTarget = 0.0f;
    fromMipLevel = mipLevel;
    levelMix = 1.0f;
    levelMixStep = 0.0f;
    jumpToTargets = true;
}

void SubOscillator::finishRamps() noexcept
{
    increment = incrementTarget;
    incrementStep = 0.0;
    gain = gainTarget;
    gainStep = 0.0f;
    levelMix = 1.0f;
    levelMixStep = 0.0f;
}

void SubOscillator::beginTick (double newIncrement, float level, SubShape newShape, int tickSamples) noexcept
{
    const int n = std::clamp (tickSamples, 1, controlTickSamples);
    const bool newTick = tickSamples >= controlTickSamples || samplesLeftInTick <= 0;

    if (newTick)
    {
        finishRamps();
        fromMipLevel = mipLevel;
        const auto s = static_cast<int> (newShape);
        shape = s >= 0 && s < numSubShapes ? newShape : SubShape::sine;
    }

    incrementTarget = std::isfinite (newIncrement) ? std::clamp (newIncrement, 0.0, 0.5) : 0.0;

    if (jumpToTargets)
        increment = incrementTarget;

    const double maxInc = std::max (increment, incrementTarget);

    if (jumpToTargets)
    {
        mipLevel = fromMipLevel = chooseMipLevel (0, maxInc);
        levelMix = 1.0f;
    }
    else
    {
        const int wanted = chooseMipLevel (mipLevel, maxInc);

        if (wanted != mipLevel)
        {
            if (fromMipLevel == mipLevel)
            {
                levelMix = 0.0f;
            }
            else if (wanted == fromMipLevel)
            {
                fromMipLevel = mipLevel;
                levelMix = 1.0f - levelMix;
            }

            mipLevel = wanted;
        }
    }

    const float safeLevel = std::isfinite (level) ? std::clamp (level, 0.0f, 1.0f) : 0.0f;
    gainTarget = maxInc <= WavetableOscillator::maxIncrementForLevel ? safeLevel : 0.0f;

    const float invN = 1.0f / static_cast<float> (n);
    incrementStep = (incrementTarget - increment) / static_cast<double> (n);
    gainStep = (gainTarget - gain) * invN;
    levelMixStep = (1.0f - levelMix) * invN;
    samplesLeftInTick = n;
    jumpToTargets = false;
}

template <bool levelFade>
void SubOscillator::renderSegment (float* left, float* right, int numSamples) noexcept
{
    const Wavetable& t = *(*tables)[static_cast<std::size_t> (shape)];
    const float* to = t.getLevel (0, mipLevel);
    [[maybe_unused]] const float* from = t.getLevel (0, fromMipLevel);
    const double lenTo = static_cast<double> (Wavetable::mipLength (mipLevel));
    [[maybe_unused]] const double lenFrom = static_cast<double> (Wavetable::mipLength (fromMipLevel));
    double ph = phase;
    double inc = increment;
    float g = gain;
    float mix = levelMix;

    for (int s = 0; s < numSamples; ++s)
    {
        float v = hermite (to, ph * lenTo);

        if constexpr (levelFade)
        {
            const float vf = hermite (from, ph * lenFrom);
            v = vf + (v - vf) * mix;
            mix += levelMixStep;
        }

        const float out = v * g;
        left[s] += out;
        right[s] += out;
        ph += inc;

        if (ph >= 1.0)
            ph -= 1.0;

        inc += incrementStep;
        g += gainStep;
    }

    phase = ph;
    increment = inc;
    gain = g;
    levelMix = mix;
}

void SubOscillator::render (float* left, float* right, int numSamples) noexcept
{
    if (tables == nullptr)
        return;

    while (numSamples > 0)
    {
        if (samplesLeftInTick <= 0)
        {
            finishRamps();
            samplesLeftInTick = numSamples;
        }

        const int n = std::min (numSamples, samplesLeftInTick);

        if (fromMipLevel != mipLevel)
            renderSegment<true> (left, right, n);
        else
            renderSegment<false> (left, right, n);

        samplesLeftInTick -= n;
        left += n;
        right += n;
        numSamples -= n;

        if (samplesLeftInTick == 0)
            finishRamps();
    }
}

} // namespace vocalsynth::synth
