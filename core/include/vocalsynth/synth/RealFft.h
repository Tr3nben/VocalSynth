#pragma once

#include <complex>
#include <span>
#include <vector>

namespace vocalsynth::synth
{

/** Own radix-2 FFT for real signals (docs/SYNTH-DESIGN.md 5.2), double precision. Used by the
    WavetableBuilder (background thread, exact band limiting < -120 dB) and the ExpressionTracker
    (audio thread, after prepare()). Size 2^order, order 4..14 (16 .. 16384 points).

    Conventions (binding): forward() is the unnormalised DFT X[k] = sum x[n] e^(-2 pi i k n / N) for
    k = 0..N/2 (N/2 + 1 bins); inverse() expects N/2 + 1 bins (imaginary parts of bins 0 and N/2 ignored)
    and returns x[n] = (1/N) sum X[k] e^(+2 pi i k n / N), so inverse (forward (x)) == x up to rounding.

    prepare() allocates (twiddles, bit reversal, scratch); forward(), inverse() and powerSpectrum() never
    allocate, lock or throw. Not thread-safe: one instance per thread. Owner: agent A. */
class RealFft
{
public:
    static constexpr int minOrder = 4;
    static constexpr int maxOrder = 14;

    RealFft() = default;
    explicit RealFft (int fftOrder) { prepare (fftOrder); }

    /** order clamped to minOrder..maxOrder. NOT real-time (may throw std::bad_alloc). */
    void prepare (int newOrder);

    int getOrder() const noexcept { return order; }
    int getSize() const noexcept { return 1 << order; }
    int getNumBins() const noexcept { return getSize() / 2 + 1; }

    /** input: getSize() samples, output: getNumBins() bins. Spans of other sizes: nothing happens. */
    void forward (std::span<const double> input, std::span<std::complex<double>> output) noexcept;

    /** input: getNumBins() bins, output: getSize() samples. */
    void inverse (std::span<const std::complex<double>> input, std::span<double> output) noexcept;

    /** |X[k]|^2 of a float frame (getSize() samples, no window applied here) into power (getNumBins()).
        Audio-thread helper for the ExpressionTracker; uses the internal scratch. */
    void powerSpectrum (std::span<const float> input, std::span<float> power) noexcept;

private:
    int order = 0;
    std::vector<std::complex<double>> twiddles;
    std::vector<int> bitReverse;
    std::vector<std::complex<double>> scratch;
    std::vector<double> realScratch;
    std::vector<std::complex<double>> binScratch;
};

} // namespace vocalsynth::synth
