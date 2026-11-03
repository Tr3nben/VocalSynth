#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace vocalsynth::synth
{

/** Why a WAV could not be read. */
enum class WavReadError
{
    none = 0,
    empty,             ///< no bytes
    notRiffWave,       ///< no "RIFF....WAVE" header (also RF64 / RIFX: not supported)
    missingFormat,     ///< no "fmt " chunk before "data"
    unsupportedFormat, ///< not PCM 8/16/24/32 or IEEE float 32/64 (incl. WAVE_FORMAT_EXTENSIBLE), or 0 / > 8 channels
    missingData,       ///< no "data" chunk or zero sample frames
    truncated,         ///< a chunk header or the fmt chunk is cut off
    tooLong            ///< more than maxSampleFrames sample frames
};

/** Short English text for the import dialog ("Unsupported WAV format", ...). */
std::string_view describeWavReadError (WavReadError error) noexcept;

/** Result of readWav(). On error, samples is empty. */
struct WavReadResult
{
    WavReadError error = WavReadError::none;
    std::vector<float> samples;   ///< mono (stereo / multichannel: mean of the channels), -1..1
    double sampleRate = 0.0;      ///< from the fmt chunk (information only: tables ignore it)
    int numChannels = 0;
    int bitsPerSample = 0;
    bool floatingPoint = false;
    int clmFrameSize = 0;         ///< frame size from a "clm " chunk ("<!>2048 ..."), 0 if absent / unreadable
    bool dataTruncated = false;   ///< the data chunk claimed more bytes than the file holds (the whole
                                  ///< sample frames that are present were read)

    bool ok() const noexcept { return error == WavReadError::none; }
};

/** Own RIFF/WAVE reader for wavetable import (docs/SYNTH-DESIGN.md 5.4): PCM 8 (unsigned), 16, 24, 32 bit,
    IEEE float 32 / 64, WAVE_FORMAT_EXTENSIBLE with those sub-formats; chunks in any order, odd chunk sizes
    padded, unknown chunks skipped, "clm " parsed (parseClmFrameSize). Any byte sequence gives a result or
    a clean error: no crash, no exception other than std::bad_alloc, no read outside the span (fuzz test).
    Reads from memory only: the plugin loads the file (juce::File, Unicode paths) and passes the bytes.
    Background thread (allocates). Owner: agent A. */
class WavReader
{
public:
    /** 256 frames of 4096 samples, or a long single cycle: anything longer is rejected (tooLong). */
    static constexpr std::size_t maxSampleFrames = 256u * 4096u;

    static WavReadResult read (std::span<const std::byte> bytes);

    /** Frame size from the text of a "clm " chunk: "<!>" followed by decimal digits (e.g. "<!>2048 10000000
        wavetable ..."). Returns the number if it is a power of two in 32..8192, else 0. */
    static int parseClmFrameSize (std::string_view chunkText) noexcept;
};

} // namespace vocalsynth::synth
