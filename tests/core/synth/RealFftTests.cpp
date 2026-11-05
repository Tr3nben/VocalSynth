// RealFft: round trip, Parseval, impulse / DC / single bin, all sizes.
// Test plan: docs/SYNTH-DESIGN.md 21. Owner: agent A (Wavetable).

#include "vocalsynth/synth/RealFft.h"

#include "SynthTestUtils.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

using namespace vocalsynth::synth;
using namespace vocalsynth::synth::test;

namespace
{
std::vector<double> randomSignal (std::size_t n, uint32_t seed)
{
    std::vector<double> x (n);

    for (auto& v : x)
    {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        v = static_cast<double> (static_cast<int32_t> (seed)) / 2147483648.0;
    }

    return x;
}
} // namespace

TEST_CASE ("RealFft: sizes and order clamp", "[synth][fft]")
{
    for (int order = RealFft::minOrder; order <= RealFft::maxOrder; ++order)
    {
        const RealFft fft (order);
        CHECK (fft.getOrder() == order);
        CHECK (fft.getSize() == 1 << order);
        CHECK (fft.getNumBins() == (1 << order) / 2 + 1);
    }

    CHECK (RealFft (2).getOrder() == RealFft::minOrder);
    CHECK (RealFft (20).getOrder() == RealFft::maxOrder);
    CHECK (RealFft().getOrder() == 0);
}

TEST_CASE ("RealFft: round trip, Parseval and agreement with a direct DFT, all sizes 16..16384", "[synth][fft]")
{
    for (int order = RealFft::minOrder; order <= RealFft::maxOrder; ++order)
    {
        RealFft fft (order);
        const auto n = static_cast<std::size_t> (fft.getSize());
        const auto x = randomSignal (n, static_cast<uint32_t> (order * 7919 + 1));
        std::vector<std::complex<double>> spectrumOut (static_cast<std::size_t> (fft.getNumBins()));
        std::vector<double> back (n);
        fft.forward (x, spectrumOut);
        fft.inverse (spectrumOut, back);

        double energy = 0.0;
        double maxError = 0.0;
        double maxValue = 0.0;

        for (std::size_t i = 0; i < n; ++i)
        {
            energy += x[i] * x[i];
            maxError = std::max (maxError, std::abs (back[i] - x[i]));
            maxValue = std::max (maxValue, std::abs (x[i]));
        }

        CAPTURE (order);
        CHECK (maxError / maxValue < 1e-12);

        // Parseval: sum x^2 = (|X0|^2 + |XN/2|^2 + 2 sum_{0<k<N/2} |Xk|^2) / N
        double spectral = std::norm (spectrumOut.front()) + std::norm (spectrumOut.back());

        for (std::size_t k = 1; k + 1 < spectrumOut.size(); ++k)
            spectral += 2.0 * std::norm (spectrumOut[k]);

        CHECK (std::abs (spectral / static_cast<double> (n) - energy) / energy < 1e-12);

        // Against the test helper's independent FFT (and a direct DFT for a few bins)
        const auto reference = spectrum (std::span<const double> (x));
        double maxDiff = 0.0;

        for (std::size_t k = 0; k < reference.size(); ++k)
            maxDiff = std::max (maxDiff, std::abs (reference[k] - spectrumOut[k]));

        CHECK (maxDiff / std::sqrt (energy) < 1e-12);

        for (const std::size_t k : { std::size_t { 1 }, n / 3, n / 2 - 1 })
        {
            std::complex<double> direct;

            for (std::size_t i = 0; i < n; ++i)
            {
                const double a = -2.0 * std::numbers::pi * static_cast<double> ((k * i) % n) / static_cast<double> (n);
                direct += x[i] * std::complex<double> (std::cos (a), std::sin (a));
            }

            CHECK (std::abs (direct - spectrumOut[k]) / std::sqrt (energy) < 1e-11);
        }
    }
}

TEST_CASE ("RealFft: impulse, DC, single bin, Nyquist", "[synth][fft]")
{
    for (const int order : { 4, 9, 14 })
    {
        RealFft fft (order);
        const auto n = static_cast<std::size_t> (fft.getSize());
        std::vector<std::complex<double>> out (n / 2 + 1);

        // Impulse at 0: flat spectrum 1
        std::vector<double> x (n, 0.0);
        x[0] = 1.0;
        fft.forward (x, out);

        for (const auto& v : out)
            REQUIRE (std::abs (v - std::complex<double> (1.0, 0.0)) < 1e-12);

        // DC: N at bin 0, nothing elsewhere
        std::fill (x.begin(), x.end(), 1.0);
        fft.forward (x, out);
        CHECK (std::abs (out[0] - std::complex<double> (static_cast<double> (n), 0.0)) < 1e-9);

        for (std::size_t k = 1; k < out.size(); ++k)
            REQUIRE (std::abs (out[k]) < 1e-9);

        // cos at bin 3: N / 2 at bin 3 only
        for (std::size_t i = 0; i < n; ++i)
            x[i] = std::cos (2.0 * std::numbers::pi * 3.0 * static_cast<double> (i) / static_cast<double> (n));

        fft.forward (x, out);

        for (std::size_t k = 0; k < out.size(); ++k)
            REQUIRE (std::abs (out[k] - std::complex<double> (k == 3 ? static_cast<double> (n) / 2.0 : 0.0, 0.0)) < 1e-9);

        // Nyquist (+1, -1, ...): N at the last bin
        for (std::size_t i = 0; i < n; ++i)
            x[i] = i % 2 == 0 ? 1.0 : -1.0;

        fft.forward (x, out);
        CHECK (std::abs (out.back() - std::complex<double> (static_cast<double> (n), 0.0)) < 1e-9);

        // Inverse of a single bin: a sine (unnormalised forward, 1 / N inverse); imag of bins 0 and N/2 ignored
        std::vector<std::complex<double>> bins (n / 2 + 1);
        bins[5] = { 0.0, -static_cast<double> (n) / 2.0 };
        bins[0] = { 0.0, 123.0 };
        bins.back() = { 0.0, -7.0 };
        fft.inverse (bins, x);

        for (std::size_t i = 0; i < n; ++i)
            REQUIRE (std::abs (x[i] - std::sin (2.0 * std::numbers::pi * 5.0 * static_cast<double> (i) / static_cast<double> (n))) < 1e-12);
    }
}

TEST_CASE ("RealFft: powerSpectrum, wrong span sizes do nothing", "[synth][fft]")
{
    RealFft fft (10);
    std::vector<float> x (1024);

    for (std::size_t i = 0; i < x.size(); ++i)
        x[i] = static_cast<float> (0.5 * std::sin (2.0 * std::numbers::pi * 10.0 * static_cast<double> (i) / 1024.0));

    std::vector<float> power (513, -1.0f);
    fft.powerSpectrum (x, power);
    CHECK (std::abs (power[10] - 256.0f * 256.0f) < 1.0f);
    CHECK (power[11] < 1e-6f);

    std::vector<float> wrong (512, -1.0f);
    fft.powerSpectrum (x, wrong);
    CHECK (wrong[0] == -1.0f);

    std::vector<std::complex<double>> bins (100, { 7.0, 7.0 });
    std::vector<double> in (1024, 1.0);
    fft.forward (in, bins);
    CHECK (bins[0] == std::complex<double> (7.0, 7.0));

    RealFft unprepared;
    std::vector<double> out (16, -1.0);
    std::vector<std::complex<double>> nine (9);
    unprepared.inverse (nine, out);
    CHECK (out[0] == -1.0);
}
