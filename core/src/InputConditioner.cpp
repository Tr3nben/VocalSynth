#include "vocalsynth/InputConditioner.h"

#include <cmath>
#include <complex>
#include <numbers>

namespace vocalsynth
{

namespace
{
struct Coefficients
{
    double dcR, b0, b1, b2, a1, a2;
};

Coefficients coefficientsFor (double sampleRate) noexcept
{
    const auto sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    Coefficients c {};
    c.dcR = std::exp (-2.0 * std::numbers::pi * InputConditioner::dcBlockerHz / sr);

    // RBJ cookbook high-pass, normalised by a0
    const auto w0 = 2.0 * std::numbers::pi * InputConditioner::highPassHz / sr;
    const auto cosW0 = std::cos (w0);
    const auto alpha = std::sin (w0) / (2.0 * InputConditioner::highPassQ);
    const auto a0 = 1.0 + alpha;

    c.b0 = (1.0 + cosW0) / 2.0 / a0;
    c.b1 = -(1.0 + cosW0) / a0;
    c.b2 = c.b0;
    c.a1 = -2.0 * cosW0 / a0;
    c.a2 = (1.0 - alpha) / a0;
    return c;
}

/** Group delay (in samples) of the polynomial sum_k p[k] z^-k at e^{jw}: Re (sum k p_k e^{-jwk} / P). */
template <std::size_t N>
double polynomialDelay (const double (&p)[N], double w) noexcept
{
    std::complex<double> sum {}, weighted {};

    for (std::size_t k = 0; k < N; ++k)
    {
        const auto term = p[k] * std::polar (1.0, -w * static_cast<double> (k));
        sum += term;
        weighted += static_cast<double> (k) * term;
    }

    return std::abs (sum) > 0.0 ? (weighted / sum).real() : 0.0;
}
} // namespace

double InputConditioner::groupDelaySeconds (double sampleRate, double frequencyHz) noexcept
{
    if (! (sampleRate > 0.0) || ! (frequencyHz > 0.0) || frequencyHz >= 0.5 * sampleRate)
        return 0.0;

    const auto c = coefficientsFor (sampleRate);
    const auto w = 2.0 * std::numbers::pi * frequencyHz / sampleRate;

    const double dcNum[] { 1.0, -1.0 };
    const double dcDen[] { 1.0, -c.dcR };
    const double hpNum[] { c.b0, c.b1, c.b2 };
    const double hpDen[] { 1.0, c.a1, c.a2 };

    const auto samples = polynomialDelay (dcNum, w) - polynomialDelay (dcDen, w) + polynomialDelay (hpNum, w)
                         - polynomialDelay (hpDen, w);
    return samples / sampleRate;
}

void InputConditioner::prepare (double sampleRate) noexcept
{
    const auto c = coefficientsFor (sampleRate);
    dcR = c.dcR;
    b0 = c.b0;
    b1 = c.b1;
    b2 = c.b2;
    a1 = c.a1;
    a2 = c.a2;
    reset();
}

void InputConditioner::reset() noexcept
{
    dcX1 = dcY1 = 0.0;
    s1 = s2 = 0.0;
}

float InputConditioner::processSample (float input) noexcept
{
    const auto x = std::isfinite (input) ? static_cast<double> (input) : 0.0;

    // DC blocker
    const auto dc = x - dcX1 + dcR * dcY1;
    dcX1 = x;
    dcY1 = dc;

    // Biquad, transposed direct form II
    const auto y = b0 * dc + s1;
    s1 = b1 * dc - a1 * y + s2;
    s2 = b2 * dc - a2 * y;

    // Flush denormal-range memories (silence after a signal); deterministic per sample
    const auto flush = [] (double& v) noexcept
    {
        if (std::abs (v) < denormalFloor)
            v = 0.0;
    };

    flush (dcY1);
    flush (s1);
    flush (s2);

    return static_cast<float> (y);
}

void InputConditioner::process (const float* in, float* out, int numSamples) noexcept
{
    for (int i = 0; i < numSamples; ++i)
        out[i] = processSample (in[i]);
}

} // namespace vocalsynth
