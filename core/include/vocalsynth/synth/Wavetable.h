#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vocalsynth::synth
{

/** Mip layout of a Wavetable (free functions so the class can use them in constant expressions). */
constexpr int wavetableMaxHarmonic (int level) noexcept
{
    return 1024 >> level;
}

constexpr int wavetableMipLength (int level) noexcept
{
    const int wanted = 4 * wavetableMaxHarmonic (level);
    return wanted < 256 ? 256 : (wanted > 2048 ? 2048 : wanted);
}

constexpr int wavetableLevelOffset (int level) noexcept
{
    int offset = 0;

    for (int l = 0; l < level; ++l)
        offset += wavetableMipLength (l) + 1; // + Wavetable::guardSamples

    return offset;
}

/** An immutable, band-limited wavetable: up to maxFrames frames of frameSize samples, each stored as
    numMipLevels mip levels (docs/SYNTH-DESIGN.md 5.1).

    Mip level L contains only the harmonics 1..maxHarmonic (L) = 1024 >> L (exactly band-limited by the
    WavetableBuilder via FFT), at mipLength (L) = clamp (4 * maxHarmonic (L), 256, 2048) samples, followed by
    guardSamples copies of its first samples so the oscillator's linear interpolation never wraps:

        level      0     1     2     3    4    5..10
        harmonics  1024  512   256   128  64   32 .. 1
        length     2048  2048  1024  512  256  256

    = 7424 samples (+ 11 guard samples) per frame, <= 7.6 MB for 256 frames.

    Built once (WavetableBuilder, background thread), then never modified: the audio thread reads it
    through a raw pointer (WavetableStore protocol, docs/SYNTH-DESIGN.md 12). Allocation and destruction
    happen off the audio thread only. Owner: agent A. */
class Wavetable
{
public:
    static constexpr int frameSize = 2048;
    static constexpr int maxFrames = 256;
    static constexpr int numMipLevels = 11;
    static constexpr int guardSamples = 1;

    static constexpr int maxHarmonic (int level) noexcept { return wavetableMaxHarmonic (level); }
    static constexpr int mipLength (int level) noexcept { return wavetableMipLength (level); }

    /** Offset of a level within one frame's storage (levels are stored back to back, each followed by
        its guard samples). */
    static constexpr int levelOffset (int level) noexcept { return wavetableLevelOffset (level); }

    /** Floats per frame (all levels + guards). */
    static constexpr int frameStride = wavetableLevelOffset (numMipLevels);

    /** Allocates numFrames (clamped to 1..maxFrames) zeroed frames. NOT real-time (may throw). */
    Wavetable (std::string name, int numFrames);

    const std::string& getName() const noexcept { return name; }
    int getNumFrames() const noexcept { return numFrames; }

    /** Samples of one mip level of one frame: mipLength (level) + guardSamples values.
        frame in [0, getNumFrames()), level in [0, numMipLevels); not range-checked (audio thread). */
    const float* getLevel (int frame, int level) const noexcept
    {
        return data.data() + static_cast<std::size_t> (frame) * static_cast<std::size_t> (frameStride)
               + static_cast<std::size_t> (levelOffset (level));
    }

    /** Builder only, before the table is published. */
    float* getLevelForWriting (int frame, int level) noexcept
    {
        return data.data() + static_cast<std::size_t> (frame) * static_cast<std::size_t> (frameStride)
               + static_cast<std::size_t> (levelOffset (level));
    }

    /** Heap bytes of the sample data. */
    std::size_t getMemoryBytes() const noexcept { return data.size() * sizeof (float); }

    /** Content hash set by the builder (FNV-1a 64 of the source frames, docs/SYNTH-DESIGN.md 5.4); 0 = unset. */
    uint64_t getContentHash() const noexcept { return contentHash; }
    void setContentHash (uint64_t hash) noexcept { contentHash = hash; }

private:
    std::string name;
    int numFrames = 1;
    uint64_t contentHash = 0;
    std::vector<float> data;
};

static_assert (Wavetable::guardSamples == 1 && Wavetable::frameSize == wavetableMipLength (0));
static_assert (Wavetable::frameStride == 7424 + Wavetable::numMipLevels * Wavetable::guardSamples);

} // namespace vocalsynth::synth
