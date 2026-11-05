#include "vocalsynth/synth/WavReader.h"

#include <array>
#include <cmath>
#include <cstring>

namespace vocalsynth::synth
{

std::string_view describeWavReadError (WavReadError error) noexcept
{
    switch (error)
    {
        case WavReadError::none:              return "OK";
        case WavReadError::empty:             return "The file is empty";
        case WavReadError::notRiffWave:       return "Not a WAV file";
        case WavReadError::missingFormat:     return "The WAV file has no format chunk";
        case WavReadError::unsupportedFormat: return "Unsupported WAV format";
        case WavReadError::missingData:       return "The WAV file has no audio data";
        case WavReadError::truncated:         return "The WAV file is damaged (cut off)";
        case WavReadError::tooLong:           return "The WAV file is too long for a wavetable";
    }

    return "Unknown error";
}

namespace
{
constexpr uint16_t formatPcm = 1;
constexpr uint16_t formatFloat = 3;
constexpr uint16_t formatExtensible = 0xFFFE;

/** Bytes 2..15 of KSDATAFORMAT_SUBTYPE_PCM / _IEEE_FLOAT (the first two bytes are the format tag). */
constexpr std::array<uint8_t, 14> subFormatTail { 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00,
                                                  0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 };

/** Bounds-checked little-endian reads from the span (callers check the range first). */
struct Bytes
{
    std::span<const std::byte> data;

    uint8_t u8 (std::size_t at) const noexcept { return static_cast<uint8_t> (data[at]); }
    uint16_t u16 (std::size_t at) const noexcept { return static_cast<uint16_t> (u8 (at) | (u8 (at + 1) << 8)); }

    uint32_t u32 (std::size_t at) const noexcept
    {
        return static_cast<uint32_t> (u8 (at)) | (static_cast<uint32_t> (u8 (at + 1)) << 8)
               | (static_cast<uint32_t> (u8 (at + 2)) << 16) | (static_cast<uint32_t> (u8 (at + 3)) << 24);
    }

    bool tagIs (std::size_t at, const char (&tag)[5]) const noexcept
    {
        for (std::size_t i = 0; i < 4; ++i)
            if (u8 (at + i) != static_cast<uint8_t> (tag[i]))
                return false;

        return true;
    }
};

struct Format
{
    uint16_t tag = 0;
    int channels = 0;
    double sampleRate = 0.0;
    int bits = 0;
};

/** Parses a complete fmt chunk body; unsupportedFormat for anything this reader cannot decode. */
WavReadError parseFormat (const Bytes& b, std::size_t body, std::size_t size, Format& format) noexcept
{
    if (size < 16)
        return WavReadError::unsupportedFormat;

    format.tag = b.u16 (body);
    format.channels = b.u16 (body + 2);
    format.sampleRate = static_cast<double> (b.u32 (body + 4));
    format.bits = b.u16 (body + 14);

    if (format.tag == formatExtensible)
    {
        // cbSize (2), validBits (2), channel mask (4), sub-format GUID (16)
        if (size < 40)
            return WavReadError::unsupportedFormat;

        for (std::size_t i = 0; i < subFormatTail.size(); ++i)
            if (b.u8 (body + 26 + i) != subFormatTail[i])
                return WavReadError::unsupportedFormat;

        format.tag = b.u16 (body + 24);
    }

    if (format.channels < 1 || format.channels > 8)
        return WavReadError::unsupportedFormat;

    const bool pcm = format.tag == formatPcm
                     && (format.bits == 8 || format.bits == 16 || format.bits == 24 || format.bits == 32);
    const bool ieee = format.tag == formatFloat && (format.bits == 32 || format.bits == 64);
    return pcm || ieee ? WavReadError::none : WavReadError::unsupportedFormat;
}

float decodeSample (const Bytes& b, std::size_t at, const Format& f) noexcept
{
    if (f.tag == formatFloat)
    {
        if (f.bits == 32)
        {
            const uint32_t u = b.u32 (at);
            float v = 0.0f;
            std::memcpy (&v, &u, sizeof (v));
            return std::isfinite (v) ? v : 0.0f;
        }

        const uint64_t u = static_cast<uint64_t> (b.u32 (at)) | (static_cast<uint64_t> (b.u32 (at + 4)) << 32);
        double v = 0.0;
        std::memcpy (&v, &u, sizeof (v));
        return std::isfinite (v) ? static_cast<float> (v) : 0.0f;
    }

    switch (f.bits)
    {
        case 8: return static_cast<float> (static_cast<int> (b.u8 (at)) - 128) * (1.0f / 128.0f);
        case 16: return static_cast<float> (static_cast<int16_t> (b.u16 (at))) * (1.0f / 32768.0f);
        case 24:
        {
            const uint32_t u = static_cast<uint32_t> (b.u8 (at)) | (static_cast<uint32_t> (b.u8 (at + 1)) << 8)
                               | (static_cast<uint32_t> (b.u8 (at + 2)) << 16);
            const auto v = static_cast<int32_t> (u << 8) >> 8; // sign-extend
            return static_cast<float> (v) * (1.0f / 8388608.0f);
        }
        default: return static_cast<float> (static_cast<double> (static_cast<int32_t> (b.u32 (at))) * (1.0 / 2147483648.0));
    }
}
} // namespace

WavReadResult WavReader::read (std::span<const std::byte> bytes)
{
    WavReadResult result;
    const Bytes b { bytes };
    const std::size_t size = bytes.size();

    if (size == 0)
    {
        result.error = WavReadError::empty;
        return result;
    }

    if (size < 12 || ! b.tagIs (0, "RIFF") || ! b.tagIs (8, "WAVE"))
    {
        result.error = WavReadError::notRiffWave;
        return result;
    }

    // The RIFF size is not trusted (writers that stream leave it 0 or 0xFFFFFFFF): the chunks are walked up to
    // the end of the span.
    Format format;
    bool haveFormat = false;
    bool haveData = false;
    std::size_t dataOffset = 0;
    std::size_t dataBytes = 0;
    uint64_t pos = 12;

    while (pos < size)
    {
        if (size - pos < 8)
        {
            // A cut-off chunk header: harmless after the data (e.g. a lost pad byte), fatal before
            if (! haveFormat || ! haveData)
            {
                result.error = WavReadError::truncated;
                return result;
            }

            break;
        }

        const auto at = static_cast<std::size_t> (pos);
        const uint64_t chunkSize = b.u32 (at + 4);
        const std::size_t body = at + 8;
        const std::size_t available = size - body;
        const bool complete = chunkSize <= available;

        if (b.tagIs (at, "fmt "))
        {
            if (! complete)
            {
                result.error = WavReadError::truncated;
                return result;
            }

            if (const auto error = parseFormat (b, body, static_cast<std::size_t> (chunkSize), format);
                error != WavReadError::none)
            {
                result.error = error;
                return result;
            }

            haveFormat = true;
        }
        else if (b.tagIs (at, "data"))
        {
            if (! haveData) // the first data chunk counts
            {
                haveData = true;
                dataOffset = body;
                dataBytes = complete ? static_cast<std::size_t> (chunkSize) : available;
                result.dataTruncated = ! complete;
            }
        }
        else if (b.tagIs (at, "clm "))
        {
            const auto n = static_cast<std::size_t> (complete ? chunkSize : available);
            const std::string_view text (reinterpret_cast<const char*> (bytes.data() + body), n);
            result.clmFrameSize = parseClmFrameSize (text);
        }

        if (! complete)
            break; // nothing can follow a chunk that runs past the end

        pos = static_cast<uint64_t> (body) + chunkSize + (chunkSize & 1u);
    }

    if (! haveFormat)
    {
        result.error = WavReadError::missingFormat;
        result.clmFrameSize = 0;
        result.dataTruncated = false;
        return result;
    }

    result.sampleRate = format.sampleRate;
    result.numChannels = format.channels;
    result.bitsPerSample = format.bits;
    result.floatingPoint = format.tag == formatFloat;

    const std::size_t bytesPerSample = static_cast<std::size_t> (format.bits / 8);
    const std::size_t frameBytes = bytesPerSample * static_cast<std::size_t> (format.channels);
    const std::size_t frames = haveData ? dataBytes / frameBytes : 0;

    if (frames == 0)
    {
        result.error = WavReadError::missingData;
        return result;
    }

    if (frames > maxSampleFrames)
    {
        result.error = WavReadError::tooLong;
        return result;
    }

    result.samples.resize (frames);
    const float scale = 1.0f / static_cast<float> (format.channels);

    for (std::size_t i = 0; i < frames; ++i)
    {
        const std::size_t frameAt = dataOffset + i * frameBytes;
        float sum = 0.0f;

        for (int c = 0; c < format.channels; ++c)
            sum += decodeSample (b, frameAt + static_cast<std::size_t> (c) * bytesPerSample, format);

        result.samples[i] = format.channels == 1 ? sum : sum * scale;
    }

    return result;
}

int WavReader::parseClmFrameSize (std::string_view chunkText) noexcept
{
    if (chunkText.substr (0, 3) != "<!>")
        return 0;

    int value = 0;
    std::size_t digits = 0;

    for (std::size_t i = 3; i < chunkText.size() && digits < 6; ++i, ++digits)
    {
        const char c = chunkText[i];

        if (c < '0' || c > '9')
            break;

        value = value * 10 + (c - '0');
    }

    if (digits == 0 || (digits == 6 && chunkText.size() > 9 && chunkText[9] >= '0' && chunkText[9] <= '9'))
        return 0;

    const bool powerOfTwo = value > 0 && (value & (value - 1)) == 0;
    return powerOfTwo && value >= 32 && value <= 8192 ? value : 0;
}

} // namespace vocalsynth::synth
