#include "vocalsynth/synth/RealFft.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace vocalsynth::synth
{

// Real FFT of size N via a complex radix-2 FFT of size M = N / 2 on z[m] = x[2m] + i x[2m + 1] and the
// standard split (docs/SYNTH-DESIGN.md 5.2):
//     X[k] = E[k] + W^k O[k],  E[k] = (Z[k] + conj Z[M - k]) / 2,  O[k] = (Z[k] - conj Z[M - k]) / 2i,
// W = e^(-2 pi i / N). twiddles[k] = W^k for k < N / 2; the size-M transform uses twiddles[2 j].

void RealFft::prepare (int newOrder)
{
    order = std::clamp (newOrder, minOrder, maxOrder);
    const auto n = static_cast<std::size_t> (getSize());
    const auto m = n / 2;

    twiddles.assign (m, {});

    for (std::size_t k = 0; k < m; ++k)
    {
        const double angle = -2.0 * std::numbers::pi * static_cast<double> (k) / static_cast<double> (n);
        twiddles[k] = { std::cos (angle), std::sin (angle) };
    }

    // Bit reversal permutation of the size-M complex transform
    bitReverse.assign (m, 0);
    const int bits = order - 1;

    for (std::size_t i = 0; i < m; ++i)
    {
        int r = 0;

        for (int b = 0; b < bits; ++b)
            if ((i >> b) & 1u)
                r |= 1 << (bits - 1 - b);

        bitReverse[i] = r;
    }

    scratch.assign (m, {});
    realScratch.assign (n, 0.0);
    binScratch.assign (m + 1, {});
}

namespace
{
/** a * b without the NaN-aware library call that std::complex operator* compiles to (no -ffast-math). */
inline std::complex<double> mul (std::complex<double> a, std::complex<double> b) noexcept
{
    return { a.real() * b.real() - a.imag() * b.imag(), a.real() * b.imag() + a.imag() * b.real() };
}

/** In-place iterative radix-2 FFT of size m (already bit-reversed). twiddles[2 j * (m / len)] = e^(-2 pi i j / len)
    for the full-size table (stride 2 because the table belongs to the real size 2 m). inverse: conjugated
    twiddles, no scaling. */
template <bool inverse>
void complexFft (std::complex<double>* z, std::size_t m, const std::complex<double>* twiddles) noexcept
{
    for (std::size_t len = 2; len <= m; len <<= 1)
    {
        const std::size_t half = len / 2;
        const std::size_t stride = 2 * (m / len);

        for (std::size_t start = 0; start < m; start += len)
        {
            for (std::size_t j = 0; j < half; ++j)
            {
                const auto w = twiddles[j * stride];
                const double wr = w.real();
                const double wi = inverse ? -w.imag() : w.imag();
                const auto a = z[start + j];
                const auto c = z[start + j + half];
                // b = c * w written out (std::complex operator* calls the NaN-aware __muldc3 without -ffast-math)
                const double br = c.real() * wr - c.imag() * wi;
                const double bi = c.real() * wi + c.imag() * wr;
                z[start + j] = { a.real() + br, a.imag() + bi };
                z[start + j + half] = { a.real() - br, a.imag() - bi };
            }
        }
    }
}
} // namespace

void RealFft::forward (std::span<const double> input, std::span<std::complex<double>> output) noexcept
{
    if (order == 0 || input.size() != static_cast<std::size_t> (getSize())
        || output.size() != static_cast<std::size_t> (getNumBins()))
        return;

    const auto m = static_cast<std::size_t> (getSize() / 2);

    for (std::size_t i = 0; i < m; ++i)
    {
        const auto r = static_cast<std::size_t> (bitReverse[i]);
        scratch[r] = { input[2 * i], input[2 * i + 1] };
    }

    complexFft<false> (scratch.data(), m, twiddles.data());

    const auto z0 = scratch[0];
    output[0] = { z0.real() + z0.imag(), 0.0 };
    output[m] = { z0.real() - z0.imag(), 0.0 };

    for (std::size_t k = 1; k < m; ++k)
    {
        const auto zk = scratch[k];
        const auto zc = std::conj (scratch[m - k]);
        const auto even = 0.5 * (zk + zc);
        const auto diff = zk - zc;
        const std::complex<double> odd { 0.5 * diff.imag(), -0.5 * diff.real() }; // (zk - zc) / 2i
        output[k] = even + mul (twiddles[k], odd);
    }
}

void RealFft::inverse (std::span<const std::complex<double>> input, std::span<double> output) noexcept
{
    if (order == 0 || input.size() != static_cast<std::size_t> (getNumBins())
        || output.size() != static_cast<std::size_t> (getSize()))
        return;

    const auto m = static_cast<std::size_t> (getSize() / 2);

    // Z[k] = E[k] + i O[k] with E[k] = (X[k] + conj X[M - k]) / 2, O[k] = (X[k] - conj X[M - k]) / (2 W^k)
    for (std::size_t k = 0; k < m; ++k)
    {
        auto xk = input[k];
        auto xc = std::conj (input[m - k]);

        if (k == 0)
        {
            xk = { input[0].real(), 0.0 };
            xc = { input[m].real(), 0.0 };
        }

        const auto even = 0.5 * (xk + xc);
        const auto odd = mul (0.5 * (xk - xc), std::conj (twiddles[k]));
        const auto r = static_cast<std::size_t> (bitReverse[k]);
        scratch[r] = { even.real() - odd.imag(), even.imag() + odd.real() }; // even + i odd
    }

    complexFft<true> (scratch.data(), m, twiddles.data());

    const double scale = 1.0 / static_cast<double> (m);

    for (std::size_t i = 0; i < m; ++i)
    {
        output[2 * i] = scratch[i].real() * scale;
        output[2 * i + 1] = scratch[i].imag() * scale;
    }
}

void RealFft::powerSpectrum (std::span<const float> input, std::span<float> power) noexcept
{
    if (order == 0 || input.size() != static_cast<std::size_t> (getSize())
        || power.size() != static_cast<std::size_t> (getNumBins()))
        return;

    std::transform (input.begin(), input.end(), realScratch.begin(), [] (float v) { return static_cast<double> (v); });
    forward (realScratch, binScratch);

    for (std::size_t k = 0; k < power.size(); ++k)
        power[k] = static_cast<float> (std::norm (binScratch[k]));
}

} // namespace vocalsynth::synth
