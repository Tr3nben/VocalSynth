#include "vocalsynth/Framer.h"

#include <algorithm>
#include <cmath>

namespace vocalsynth
{

int Framer::hopSamplesFor (double analysisSampleRate) noexcept
{
    if (! (analysisSampleRate > 0.0))
        return 1;

    return std::max (1, static_cast<int> (std::lround (analysisSampleRate * hopSeconds)));
}

int Framer::maxLagFor (double analysisSampleRate, double f0MinHz) noexcept
{
    if (! (analysisSampleRate > 0.0) || ! (f0MinHz > 0.0))
        return 2;

    return std::max (2, static_cast<int> (std::ceil (analysisSampleRate / f0MinHz)) + lagGuardSamples);
}

int Framer::windowSamplesFor (double analysisSampleRate, double f0MinHz) noexcept
{
    return 2 * maxLagFor (analysisSampleRate, f0MinHz);
}

void Framer::prepare (double analysisSampleRate, int maxWindowSamples)
{
    hopSamples = hopSamplesFor (analysisSampleRate);
    maxWindow = std::max (1, maxWindowSamples);
    windowSamples = maxWindow;
    ring.assign (2 * static_cast<std::size_t> (maxWindow), 0.0f);
    reset();
}

void Framer::reset() noexcept
{
    std::fill (ring.begin(), ring.end(), 0.0f);
    writePos = 0;
    pushed = 0;
}

void Framer::setWindowSamples (int newWindowSamples) noexcept
{
    windowSamples = std::clamp (newWindowSamples, 1, std::max (1, maxWindow));
}

bool Framer::pushSample (float sample) noexcept
{
    if (maxWindow > 0)
    {
        // Doubled ring: after this write the newest maxWindow samples are
        // ring[writePos + 1 .. writePos + maxWindow], oldest first (see getWindow()).
        ring[static_cast<std::size_t> (writePos)] = sample;
        ring[static_cast<std::size_t> (writePos + maxWindow)] = sample;
        writePos = writePos + 1 < maxWindow ? writePos + 1 : 0;
    }

    ++pushed;
    return pushed % hopSamples == 0;
}

std::span<const float> Framer::getWindow() const noexcept
{
    if (ring.empty())
        return {};

    // The newest sample sits at index (writePos - 1) mod maxWindow + maxWindow, so the newest
    // windowSamples (<= maxWindow) end there and start at an index >= 0. Before the first push this is
    // the zero-initialised end of the ring.
    const auto newest = (writePos == 0 ? maxWindow - 1 : writePos - 1) + maxWindow;
    const auto first = newest - windowSamples + 1;
    return { ring.data() + first, static_cast<std::size_t> (windowSamples) };
}

} // namespace vocalsynth
