#include "vocalsynth/Decimator.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace vocalsynth
{

namespace
{
/** Zeroth-order modified Bessel function of the first kind (power series; converges fast for the
    arguments of a Kaiser window, beta < 10). */
double besselI0 (double x) noexcept
{
    const auto halfX = 0.5 * x;
    double sum = 1.0, term = 1.0;

    for (int k = 1; k < 64; ++k)
    {
        const auto f = halfX / static_cast<double> (k);
        term *= f * f;
        sum += term;

        if (term < 1.0e-17 * sum)
            break;
    }

    return sum;
}

/** Design margin. With the binding tap counts (Kaiser length estimate for 60 dB), beta for exactly
    60 dB and the cutoff at 0.5 fa only reach 58.9-60.5 dB at 0.65 fa (the estimate is about 1 dB
    optimistic at these short lengths). beta for 62 dB with the cutoff at 0.49 fa gives >= 60.7 dB in the
    stopband for every factor 2..13 and a passband (<= 0.35 fa) within -0.05 / +0.01 dB. */
constexpr double designAttenuationDb = 62.0;
constexpr double cutoffFraction = 0.49; ///< of the output rate

/** Kaiser-windowed sinc low-pass with the cutoff at cutoffFraction of the output rate, normalised to
    DC gain 1 (docs/M2-DESIGN.md 3.3). Computed in double and written symmetrically, so the float taps
    are exactly symmetric. */
void designTaps (int factor, std::vector<float>& taps)
{
    const auto numTaps = static_cast<int> (taps.size());

    if (numTaps <= 1)
    {
        std::fill (taps.begin(), taps.end(), 1.0f);
        return;
    }

    const auto beta = 0.1102 * (designAttenuationDb - 8.7);
    const auto cutoff = cutoffFraction / static_cast<double> (factor); // cycles per input sample
    const auto centre = 0.5 * static_cast<double> (numTaps - 1);
    const auto i0Beta = besselI0 (beta);

    std::vector<double> h (static_cast<std::size_t> (numTaps), 0.0); // prepare() only: may allocate
    double sum = 0.0;

    for (int i = 0; i < numTaps; ++i)
    {
        const auto mirrored = numTaps - 1 - i;

        if (mirrored < i)
        {
            h[static_cast<std::size_t> (i)] = h[static_cast<std::size_t> (mirrored)];
        }
        else
        {
            const auto t = static_cast<double> (i) - centre;
            const auto x = 2.0 * std::numbers::pi * cutoff * t;
            const auto sinc = t == 0.0 ? 2.0 * cutoff : 2.0 * cutoff * std::sin (x) / x;
            const auto r = t / centre;
            const auto window = besselI0 (beta * std::sqrt (std::max (0.0, 1.0 - r * r))) / i0Beta;
            h[static_cast<std::size_t> (i)] = sinc * window;
        }

        sum += h[static_cast<std::size_t> (i)];
    }

    for (int i = 0; i < numTaps; ++i)
        taps[static_cast<std::size_t> (i)] = static_cast<float> (h[static_cast<std::size_t> (i)] / sum);
}
} // namespace

int Decimator::factorFor (double sampleRate) noexcept
{
    if (! (sampleRate > 0.0))
        return 1;

    return std::max (1, static_cast<int> (std::floor (sampleRate / targetAnalysisRate)));
}

int Decimator::numTapsFor (int factor) noexcept
{
    if (factor <= 1)
        return 1;

    const auto transition = 2.0 * std::numbers::pi * (stopbandFraction - passbandFraction) / static_cast<double> (factor);
    auto taps = static_cast<int> (std::ceil ((stopbandAttenuationDb - 7.95) / (2.285 * transition))) + 1;

    if (taps % 2 == 0)
        ++taps;

    return taps;
}

void Decimator::prepare (double inputSampleRate)
{
    inputRate = inputSampleRate > 0.0 ? inputSampleRate : 48000.0;
    factor = factorFor (inputRate);

    const auto numTaps = numTapsFor (factor);
    taps.assign (static_cast<std::size_t> (numTaps), 0.0f);
    designTaps (factor, taps);
    history.assign (2 * static_cast<std::size_t> (numTaps), 0.0f);
    reset();
}

void Decimator::reset() noexcept
{
    std::fill (history.begin(), history.end(), 0.0f);
    writePos = 0;
    phase = 0;
}

bool Decimator::pushSample (float input, float& out) noexcept
{
    const auto numTaps = static_cast<int> (taps.size());

    if (numTaps == 0) // not prepared: behave as a pass-through
    {
        out = input;
        return true;
    }

    // Doubled ring: the newest numTaps samples are always history[writePos + 1 .. writePos + numTaps],
    // oldest first, so the dot product below is one contiguous loop.
    history[static_cast<std::size_t> (writePos)] = input;
    history[static_cast<std::size_t> (writePos + numTaps)] = input;
    const auto* newestWindow = history.data() + writePos + 1;
    writePos = writePos + 1 < numTaps ? writePos + 1 : 0;

    const bool produces = phase == 0;
    phase = phase + 1 < factor ? phase + 1 : 0;

    if (! produces)
        return false;

    // y[k] = sum_j h[j] x[kD - j]; the taps are symmetric (linear phase), so h[N-1-i] == h[i] and the
    // oldest-first window can be multiplied with the taps in storage order.
    const auto* h = taps.data();
    float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, s3 = 0.0f;
    int i = 0;

    for (; i + 4 <= numTaps; i += 4)
    {
        s0 += h[i] * newestWindow[i];
        s1 += h[i + 1] * newestWindow[i + 1];
        s2 += h[i + 2] * newestWindow[i + 2];
        s3 += h[i + 3] * newestWindow[i + 3];
    }

    for (; i < numTaps; ++i)
        s0 += h[i] * newestWindow[i];

    out = (s0 + s1) + (s2 + s3);
    return true;
}

} // namespace vocalsynth
