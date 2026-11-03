#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vocalsynth::synth
{

/** Mip layout of a Wavetable (free functions so the class can use them in constant expressions).
    Critique 0.4 (docs/SYNTH-DESIGN.md 5.1, 23): every level is 8x oversampled (at least 1024 samples) and read
    with 4-point Hermite interpolation; 4x + linear interpolation aliased at -42 dB and missed the alias gate. */
constexpr int wavetableMaxHarmonic (int level) noexcept
{
    return 1024 >> level;
}

constexpr int wavetableMipLength (int level) noexcept
{
    const int wanted = 8 * wavetableMaxHarmonic (level);
    return wanted < 1024 ? 1024 : (wanted > 8192 ? 8192 : wanted);
}

/** Guard samples around every level (Hermite reads x[i - 1] .. x[i + 2] without a wrap test): one copy of the
    last sample before sample 0, copies of samples 0 and 1 after the last one. */
inline constexpr int wavetableGuardBefore = 1;
inline constexpr int wavetableGuardAfter = 2;

/** Offset of sample 0 of a level within one frame's storage. */
constexpr int wavetableLevelOffset (int level) noexcept
{
    int offset = wavetableGuardBefore;

    for (int l = 0; l < level; ++l)
        offset += wavetableMipLength (l) + wavetableGuardBefore + wavetableGuardAfter;

    return offset;
}

/** An immutable, band-limited wavetable: up to maxFrames frames of frameSize samples, each stored as
    numMipLevels mip levels (docs/SYNTH-DESIGN.md 5.1).

    Mip level L contains only the harmonics 1..maxHarmonic (L) = 1024 >> L (exactly band-limited by the
    WavetableBuilder via FFT; bin 1024 of level 0 = the source Nyquist is always dropped), at
    mipLength (L) = clamp (8 * maxHarmonic (L), 1024, 8192) samples (>= 8x oversampled), with guardBefore = 1
    sample in front (copy of the last sample) and guardAfter = 2 behind (copies of samples 0, 1) so the
    oscillator's 4-point Hermite interpolation never wraps:

        level      0     1     2     3     4..10
        harmonics  1024  512   256   128   64 .. 1
        length     8192  4096  2048  1024  1024

    = 22528 samples (+ 33 guard samples) per frame = 90.2 KB, 5.8 MB for 64 frames, <= 23.1 MB for 256 frames.

    Built once (WavetableBuilder, background thread), then never modified: the audio thread reads it
    through a raw pointer (WavetableStore protocol, docs/SYNTH-DESIGN.md 12). Allocation and destruction
    happen off the audio thread only. Owner: agent A. */
class Wavetable
{
public:
    static constexpr int frameSize = 2048;
    static constexpr int maxFrames = 256;
    static constexpr int numMipLevels = 11;
    static constexpr int guardBefore = wavetableGuardBefore;
    static constexpr int guardAfter = wavetableGuardAfter;

    static constexpr int maxHarmonic (int level) noexcept { return wavetableMaxHarmonic (level); }
    static constexpr int mipLength (int level) noexcept { return wavetableMipLength (level); }

    /** Offset of sample 0 of a level within one frame's storage (levels are stored back to back, each as
        guardBefore + mipLength + guardAfter samples). */
    static constexpr int levelOffset (int level) noexcept { return wavetableLevelOffset (level); }

    /** Floats per frame (all levels + guards). */
    static constexpr int frameStride = wavetableLevelOffset (numMipLevels) - wavetableGuardBefore;

    /** Allocates numFrames (clamped to 1..maxFrames) zeroed frames. NOT real-time (may throw). */
    Wavetable (std::string name, int numFrames);

    const std::string& getName() const noexcept { return name; }
    int getNumFrames() const noexcept { return numFrames; }

    /** Sample 0 of one mip level of one frame: p[0 .. mipLength (level) - 1] is one cycle, p[-1], p[mipLength]
        and p[mipLength + 1] are the guard copies (valid to read). frame in [0, getNumFrames()), level in
        [0, numMipLevels); not range-checked (audio thread). */
    const float* getLevel (int frame, int level) const noexcept
    {
        return data.data() + static_cast<std::size_t> (frame) * static_cast<std::size_t> (frameStride)
               + static_cast<std::size_t> (levelOffset (level));
    }

    /** Builder only, before the table is published (the builder also writes the guard samples). */
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

static_assert (wavetableMipLength (0) == 8192 && wavetableMipLength (3) == 1024 && wavetableMipLength (10) == 1024);
static_assert (Wavetable::frameStride
               == 22528 + Wavetable::numMipLevels * (Wavetable::guardBefore + Wavetable::guardAfter));

} // namespace vocalsynth::synth
