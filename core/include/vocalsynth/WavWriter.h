#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace vocalsynth
{

enum class WavSampleFormat : uint8_t
{
    pcm24 = 0, ///< 24-bit integer PCM (default, as M1)
    float32    ///< 32-bit IEEE float (keeps overs above 0 dBFS)
};

inline constexpr int numWavSampleFormats = 2;

/** Mono WAV encoding without I/O (docs/M3-DESIGN.md 11): the caller writes makeHeader() and then the
    encode()d sample blocks to a file, in order, so a 10-minute take streams through a small buffer.
    JUCE-free, deterministic, real-time safe encode() (no allocation).

    Header (binding): RIFF / WAVE, "fmt " chunk: pcm24 = WAVE_FORMAT_PCM (1), 16-byte fmt, 44-byte header;
    float32 = WAVE_FORMAT_IEEE_FLOAT (3), 18-byte fmt (cbSize 0) + "fact" chunk (frame count), 58-byte
    header. Mono, sample rate = llround (sampleRate), little endian, data chunk of numFrames frames; an odd
    data size (24-bit with an odd frame count) is followed by one zero pad byte, counted in the RIFF size
    but not in the data size (dataPaddingBytes()).
    Samples: pcm24 = clamp (llround (x * 8388608), -8388608, 8388607) (no dither; NaN -> 0); float32 = x
    (NaN -> 0). */
class WavWriter
{
public:
    static constexpr int bytesPerSample (WavSampleFormat format) noexcept
    {
        return format == WavSampleFormat::float32 ? 4 : 3;
    }

    /** Largest frame count whose RIFF size fits in 32 bits. */
    static uint64_t maxFrames (WavSampleFormat format) noexcept;

    /** Header bytes for numFrames mono frames (numFrames clamped to maxFrames()). */
    static std::vector<uint8_t> makeHeader (WavSampleFormat format, double sampleRate, uint64_t numFrames);

    /** 1 if the data chunk needs a pad byte (odd byte count), else 0. Append that many zero bytes last. */
    static int dataPaddingBytes (WavSampleFormat format, uint64_t numFrames) noexcept;

    /** Encodes samples into out (needs samples.size() * bytesPerSample() bytes); returns the bytes written
        (0 if out is too small). */
    static std::size_t encode (std::span<const float> samples, WavSampleFormat format,
                               std::span<uint8_t> out) noexcept;
};

} // namespace vocalsynth
