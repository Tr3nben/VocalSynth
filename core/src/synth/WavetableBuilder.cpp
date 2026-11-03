#include "vocalsynth/synth/WavetableBuilder.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace vocalsynth::synth
{

// Scaffold (docs/SYNTH-DESIGN.md 5.3): build() returns an error, makeSineTable() a silent frame. Agent A
// implements. quantiseToPcm16() and fnv1a64() are final (state format, docs/SYNTH-DESIGN.md 13).

WavetableBuildResult WavetableBuilder::build (std::string, std::span<const float> samples, int,
                                              const std::atomic<bool>*)
{
    WavetableBuildResult result;
    result.error = samples.empty() ? WavetableBuildError::noSamples : WavetableBuildError::invalidFrameSize;
    return result;
}

int WavetableBuilder::chooseFrameSize (std::size_t, int, int) noexcept
{
    return 0;
}

WavetableBuildResult WavetableBuilder::buildFromWav (std::string name, const WavReadResult& wav, int overrideFrameSize,
                                                     const std::atomic<bool>* cancel)
{
    if (! wav.ok())
    {
        WavetableBuildResult result;
        result.error = WavetableBuildError::readError;
        return result;
    }

    return build (std::move (name), wav.samples, chooseFrameSize (wav.samples.size(), wav.clmFrameSize, overrideFrameSize),
                  cancel);
}

WavetableBuildResult WavetableBuilder::buildFromPcm16 (std::string name, std::span<const int16_t> pcm, int numFrames,
                                                       const std::atomic<bool>* cancel)
{
    std::vector<float> samples (pcm.size());
    std::transform (pcm.begin(), pcm.end(), samples.begin(), pcm16ToFloat);

    if (numFrames <= 0 || pcm.size() != static_cast<std::size_t> (numFrames) * Wavetable::frameSize)
    {
        WavetableBuildResult result;
        result.error = WavetableBuildError::noSamples;
        return result;
    }

    return build (std::move (name), samples, Wavetable::frameSize, cancel);
}

std::unique_ptr<Wavetable> WavetableBuilder::makeSineTable()
{
    return std::make_unique<Wavetable> ("Sine", 1);
}

int16_t WavetableBuilder::quantiseToPcm16 (float x) noexcept
{
    if (! std::isfinite (x))
        return 0;

    const auto scaled = static_cast<int> (std::lround (static_cast<double> (std::clamp (x, -1.0f, 1.0f)) * 32767.0));
    return static_cast<int16_t> (std::clamp (scaled, -32767, 32767));
}

uint64_t WavetableBuilder::fnv1a64 (std::span<const int16_t> pcm) noexcept
{
    uint64_t hash = 14695981039346656037ull;

    for (const auto v : pcm)
    {
        const auto u = static_cast<uint16_t> (v);

        for (const auto byte : { static_cast<uint8_t> (u & 0xffu), static_cast<uint8_t> (u >> 8) })
        {
            hash ^= byte;
            hash *= 1099511628211ull;
        }
    }

    return hash;
}

} // namespace vocalsynth::synth
