#include "vocalsynth/synth/RealFft.h"

#include <algorithm>

namespace vocalsynth::synth
{

// Scaffold (docs/SYNTH-DESIGN.md 5.2): allocates the buffers, transforms output zeros. Agent A implements.

void RealFft::prepare (int newOrder)
{
    order = std::clamp (newOrder, minOrder, maxOrder);
    const auto n = static_cast<std::size_t> (getSize());
    twiddles.assign (n / 2, {});
    bitReverse.assign (n, 0);
    scratch.assign (n, {});
    realScratch.assign (n, 0.0);
    binScratch.assign (n / 2 + 1, {});
}

void RealFft::forward (std::span<const double> input, std::span<std::complex<double>> output) noexcept
{
    if (order == 0 || input.size() != static_cast<std::size_t> (getSize())
        || output.size() != static_cast<std::size_t> (getNumBins()))
        return;

    std::fill (output.begin(), output.end(), std::complex<double> {});
}

void RealFft::inverse (std::span<const std::complex<double>> input, std::span<double> output) noexcept
{
    if (order == 0 || input.size() != static_cast<std::size_t> (getNumBins())
        || output.size() != static_cast<std::size_t> (getSize()))
        return;

    std::fill (output.begin(), output.end(), 0.0);
}

void RealFft::powerSpectrum (std::span<const float> input, std::span<float> power) noexcept
{
    if (order == 0 || input.size() != static_cast<std::size_t> (getSize())
        || power.size() != static_cast<std::size_t> (getNumBins()))
        return;

    std::fill (power.begin(), power.end(), 0.0f);
}

} // namespace vocalsynth::synth
