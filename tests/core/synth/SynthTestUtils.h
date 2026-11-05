#pragma once

// Shared helpers for the synth tests (spectra, alias energy, block-split renders). Owner: agent A; the other
// synth test files may include it read-only and ask agent A for additions (docs/SYNTH-DESIGN.md 19).

#include "vocalsynth/synth/SynthSettings.h"
#include "vocalsynth/synth/Wavetable.h"
#include "vocalsynth/synth/WavetableBuilder.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <map>
#include <memory>
#include <numbers>
#include <span>
#include <vector>

namespace vocalsynth::synth::test
{

//==============================================================================
// Spectra (double precision, any power-of-two size; RealFft stops at 16384)

/** In-place iterative radix-2 complex FFT (forward, unnormalised). x.size() must be a power of two. */
inline void fftInPlace (std::vector<std::complex<double>>& x)
{
    const std::size_t n = x.size();

    for (std::size_t i = 1, j = 0; i < n; ++i)
    {
        std::size_t bit = n >> 1;

        for (; j & bit; bit >>= 1)
            j ^= bit;

        j ^= bit;

        if (i < j)
            std::swap (x[i], x[j]);
    }

    static std::map<std::size_t, std::vector<std::complex<double>>> twiddleCache;
    auto& tw = twiddleCache[n];

    if (tw.size() != n / 2)
    {
        tw.resize (n / 2);

        for (std::size_t k = 0; k < n / 2; ++k)
        {
            const double a = -2.0 * std::numbers::pi * static_cast<double> (k) / static_cast<double> (n);
            tw[k] = { std::cos (a), std::sin (a) };
        }
    }

    for (std::size_t len = 2; len <= n; len <<= 1)
    {
        const std::size_t half = len / 2;
        const std::size_t stride = n / len;

        for (std::size_t start = 0; start < n; start += len)
            for (std::size_t j = 0; j < half; ++j)
            {
                const auto a = x[start + j];
                const auto c = x[start + j + half];
                const auto w = tw[j * stride];
                const double br = c.real() * w.real() - c.imag() * w.imag(); // no __muldc3
                const double bi = c.real() * w.imag() + c.imag() * w.real();
                x[start + j] = { a.real() + br, a.imag() + bi };
                x[start + j + half] = { a.real() - br, a.imag() - bi };
            }
    }
}

/** Complex spectrum (bins 0..N/2) of the real signal x (size a power of two >= 4) times an optional window:
    a complex FFT of size N/2 on z[m] = x[2m] + i x[2m + 1] and the real split. */
template <typename T>
std::vector<std::complex<double>> spectrum (std::span<const T> x, std::span<const double> window = {})
{
    const std::size_t n = x.size();
    const std::size_t m = n / 2;
    const auto at = [&] (std::size_t i) { return static_cast<double> (x[i]) * (window.empty() ? 1.0 : window[i]); };
    std::vector<std::complex<double>> z (m);

    for (std::size_t i = 0; i < m; ++i)
        z[i] = { at (2 * i), at (2 * i + 1) };

    fftInPlace (z);
    static std::map<std::size_t, std::vector<std::complex<double>>> splitCache;
    auto& split = splitCache[n];

    if (split.size() != m)
    {
        split.resize (m);

        for (std::size_t k = 0; k < m; ++k)
        {
            const double a = -2.0 * std::numbers::pi * static_cast<double> (k) / static_cast<double> (n);
            split[k] = { std::cos (a), std::sin (a) };
        }
    }

    std::vector<std::complex<double>> out (m + 1);
    out[0] = { z[0].real() + z[0].imag(), 0.0 };
    out[m] = { z[0].real() - z[0].imag(), 0.0 };

    for (std::size_t k = 1; k < m; ++k)
    {
        const auto zk = z[k];
        const auto zc = std::conj (z[m - k]);
        const auto even = 0.5 * (zk + zc);
        const auto d = zk - zc;
        const std::complex<double> odd (0.5 * d.imag(), -0.5 * d.real()); // (zk - zc) / 2i
        const double c = split[k].real(), s = split[k].imag();
        out[k] = { even.real() + odd.real() * c - odd.imag() * s, even.imag() + odd.real() * s + odd.imag() * c };
    }

    return out;
}

/** |X[k]|^2, bins 0..N/2, rectangular window unless one is given. */
template <typename T>
std::vector<double> powerSpectrum (std::span<const T> x, std::span<const double> window = {})
{
    const auto s = spectrum (x, window);
    std::vector<double> p (s.size());

    for (std::size_t k = 0; k < s.size(); ++k)
        p[k] = std::norm (s[k]);

    return p;
}

inline std::vector<double> hannWindow (std::size_t n)
{
    std::vector<double> w (n);

    for (std::size_t i = 0; i < n; ++i)
        w[i] = 0.5 - 0.5 * std::cos (2.0 * std::numbers::pi * static_cast<double> (i) / static_cast<double> (n));

    return w;
}

inline double toDb (double powerRatio)
{
    return 10.0 * std::log10 (std::max (powerRatio, 1e-300));
}

/** Alias gate measure (docs/SYNTH-DESIGN.md 21, critique F1/F15): f0 on bin m (m odd, rectangular window),
    every bin that is not a multiple of m and not DC is non-harmonic; result = 10 log10 (non-harmonic energy /
    reference power). reference: the fundamental's power (bin m) unless given. */
inline double nonHarmonicDb (const std::vector<double>& power, std::size_t m, double referencePower = -1.0)
{
    double nonHarmonic = 0.0;

    for (std::size_t k = 1; k < power.size(); ++k)
        if (k % m != 0)
            nonHarmonic += power[k];

    return toDb (nonHarmonic / (referencePower > 0.0 ? referencePower : power[m]));
}

/** Frequency of the strongest component (Hz): Hann FFT peak, refined by the phase advance between two
    windows (hop 1024) - accurate to far better than 0.01 cent for a steady tone. */
inline double measureFrequency (std::span<const float> x, double sampleRate, std::size_t fftSize = 65536)
{
    const std::size_t hop = 1024;
    const auto w = hannWindow (fftSize);
    const auto a = spectrum (x.first (fftSize), std::span<const double> (w));
    const auto b = spectrum (x.subspan (hop, fftSize), std::span<const double> (w));
    std::size_t peak = 1;

    for (std::size_t k = 1; k + 1 < a.size(); ++k)
        if (std::norm (a[k]) > std::norm (a[peak]))
            peak = k;

    const double expected = 2.0 * std::numbers::pi * static_cast<double> (peak) * static_cast<double> (hop)
                            / static_cast<double> (fftSize);
    double d = std::arg (b[peak]) - std::arg (a[peak]) - expected;
    d = std::remainder (d, 2.0 * std::numbers::pi);
    const double bin = static_cast<double> (peak) + d * static_cast<double> (fftSize)
                                                        / (2.0 * std::numbers::pi * static_cast<double> (hop));
    return bin * sampleRate / static_cast<double> (fftSize);
}

inline double cents (double f, double reference)
{
    return 1200.0 * std::log2 (f / reference);
}

//==============================================================================
// Levels

template <typename T>
double rms (std::span<const T> x)
{
    double sum = 0.0;

    for (const auto v : x)
        sum += static_cast<double> (v) * static_cast<double> (v);

    return x.empty() ? 0.0 : std::sqrt (sum / static_cast<double> (x.size()));
}

template <typename T>
double peakAbs (std::span<const T> x)
{
    double p = 0.0;

    for (const auto v : x)
        p = std::max (p, std::abs (static_cast<double> (v)));

    return p;
}

//==============================================================================
// Renders on the control tick grid (docs/SYNTH-DESIGN.md 3)

struct Stereo
{
    std::vector<float> left, right;

    explicit Stereo (std::size_t n = 0) : left (n, 0.0f), right (n, 0.0f) {}
    std::size_t size() const noexcept { return left.size(); }
};

/** Runs numSamples samples split into host blocks (blockSizes repeated) and control ticks: onTick (tickIndex)
    at every absolute multiple of controlTickSamples, render (offset, n) for every piece between a tick and a
    block boundary. */
template <typename OnTick, typename Render>
void runTicks (int numSamples, std::span<const int> blockSizes, OnTick&& onTick, Render&& render)
{
    int position = 0;
    std::size_t blockIndex = 0;

    while (position < numSamples)
    {
        const int block = std::min (blockSizes.empty() ? numSamples : std::max (1, blockSizes[blockIndex++ % blockSizes.size()]),
                                    numSamples - position);
        const int end = position + block;

        while (position < end)
        {
            if (position % controlTickSamples == 0)
                onTick (position / controlTickSamples);

            const int toTick = controlTickSamples - position % controlTickSamples;
            const int n = std::min (toTick, end - position);
            render (position, n);
            position += n;
        }
    }
}

//==============================================================================
// Tables

/** One 2048-sample cycle from sine / cosine series amplitudes (x = sum a[k] cos + b[k] sin, k >= 1). */
inline std::vector<float> cycleFromSeries (const std::vector<double>& sinAmp, const std::vector<double>& cosAmp = {},
                                           int length = Wavetable::frameSize)
{
    std::vector<float> x (static_cast<std::size_t> (length));

    for (int i = 0; i < length; ++i)
    {
        const double t = static_cast<double> (i) / static_cast<double> (length);
        double v = 0.0;

        for (std::size_t k = 1; k < sinAmp.size(); ++k)
            if (sinAmp[k] != 0.0)
                v += sinAmp[k] * std::sin (2.0 * std::numbers::pi * static_cast<double> (k) * t);

        for (std::size_t k = 1; k < cosAmp.size(); ++k)
            if (cosAmp[k] != 0.0)
                v += cosAmp[k] * std::cos (2.0 * std::numbers::pi * static_cast<double> (k) * t);

        x[static_cast<std::size_t> (i)] = static_cast<float> (v);
    }

    return x;
}

/** Saw sine-series amplitudes 1/k up to maxHarmonic (index = harmonic). */
inline std::vector<double> sawSeries (int maxHarmonic = 1023)
{
    std::vector<double> b (static_cast<std::size_t> (maxHarmonic) + 1, 0.0);

    for (int k = 1; k <= maxHarmonic; ++k)
        b[static_cast<std::size_t> (k)] = (k % 2 == 1 ? 1.0 : -1.0) / static_cast<double> (k);

    return b;
}

/** Square sine-series amplitudes (odd harmonics 1/k). */
inline std::vector<double> squareSeries (int maxHarmonic = 1023)
{
    std::vector<double> b (static_cast<std::size_t> (maxHarmonic) + 1, 0.0);

    for (int k = 1; k <= maxHarmonic; k += 2)
        b[static_cast<std::size_t> (k)] = 1.0 / static_cast<double> (k);

    return b;
}

/** Builds a table from concatenated 2048-sample frames (asserting success is up to the caller). */
inline std::shared_ptr<const Wavetable> buildTable (const std::vector<std::vector<float>>& frames, const char* name = "test")
{
    std::vector<float> all;

    for (const auto& f : frames)
        all.insert (all.end(), f.begin(), f.end());

    auto result = WavetableBuilder::build (name, all, Wavetable::frameSize);
    return std::shared_ptr<const Wavetable> (std::move (result.table));
}

/** Process-wide test tables (built once per test binary). */
inline const Wavetable& sawTable()
{
    static const auto table = buildTable ({ cycleFromSeries (sawSeries()) }, "Saw");
    return *table;
}

inline const Wavetable& squareTable()
{
    static const auto table = buildTable ({ cycleFromSeries (squareSeries()) }, "Square");
    return *table;
}

/** Two frames: saw (position 0) -> square (position 1). */
inline const Wavetable& sawToSquareTable()
{
    static const auto table = buildTable ({ cycleFromSeries (sawSeries()), cycleFromSeries (squareSeries()) }, "SawSquare");
    return *table;
}

inline const Wavetable& sineTable()
{
    static const std::shared_ptr<const Wavetable> table (WavetableBuilder::makeSineTable());
    return *table;
}

/** Complex harmonic coefficients c[k] of one stored frame (x (t) = Re sum c[k] e^(2 pi i k t)), from the
    level-0 data of the table (k <= 1023). */
inline std::vector<std::complex<double>> frameHarmonics (const Wavetable& table, int frame)
{
    const int n = Wavetable::mipLength (0);
    const float* p = table.getLevel (frame, 0);
    const auto s = spectrum (std::span<const float> (p, static_cast<std::size_t> (n)));
    std::vector<std::complex<double>> c (1024);

    for (std::size_t k = 1; k < c.size(); ++k)
        c[k] = 2.0 * s[k] / static_cast<double> (n);

    return c;
}

} // namespace vocalsynth::synth::test
