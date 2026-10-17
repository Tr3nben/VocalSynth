#include "vocalsynth/WavWriter.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace vocalsynth
{

namespace
{
constexpr uint16_t formatPcm = 1;
constexpr uint16_t formatIeeeFloat = 3;
constexpr std::size_t pcmHeaderBytes = 44;   // RIFF 12 + fmt 8+16 + data 8
constexpr std::size_t floatHeaderBytes = 58; // RIFF 12 + fmt 8+18 + fact 8+4 + data 8

std::size_t headerBytesFor (WavSampleFormat format) noexcept
{
    return format == WavSampleFormat::float32 ? floatHeaderBytes : pcmHeaderBytes;
}

void appendTag (std::vector<uint8_t>& out, const char (&tag)[5])
{
    out.insert (out.end(), tag, tag + 4);
}

void appendU16 (std::vector<uint8_t>& out, uint16_t v)
{
    out.push_back (static_cast<uint8_t> (v & 0xffu));
    out.push_back (static_cast<uint8_t> ((v >> 8) & 0xffu));
}

void appendU32 (std::vector<uint8_t>& out, uint32_t v)
{
    for (int shift = 0; shift < 32; shift += 8)
        out.push_back (static_cast<uint8_t> ((v >> shift) & 0xffu));
}

/** The RIFF chunk size (file size - 8) for numFrames frames, incl. the pad byte. */
uint64_t riffSizeFor (WavSampleFormat format, uint64_t numFrames) noexcept
{
    const auto dataBytes = numFrames * static_cast<uint64_t> (WavWriter::bytesPerSample (format));
    return static_cast<uint64_t> (headerBytesFor (format)) - 8 + dataBytes + (dataBytes & 1u);
}
} // namespace

uint64_t WavWriter::maxFrames (WavSampleFormat format) noexcept
{
    // Largest n with riffSizeFor (n) <= 0xFFFFFFFF
    const auto limit = uint64_t { std::numeric_limits<uint32_t>::max() };
    const auto overhead = static_cast<uint64_t> (headerBytesFor (format)) - 8;
    const auto bps = static_cast<uint64_t> (bytesPerSample (format));
    auto n = (limit - overhead) / bps;

    while (n > 0 && riffSizeFor (format, n) > limit)
        --n;

    return n;
}

int WavWriter::dataPaddingBytes (WavSampleFormat format, uint64_t numFrames) noexcept
{
    const auto dataBytes = std::min (numFrames, maxFrames (format)) * static_cast<uint64_t> (bytesPerSample (format));
    return (dataBytes & 1u) != 0 ? 1 : 0;
}

std::vector<uint8_t> WavWriter::makeHeader (WavSampleFormat format, double sampleRate, uint64_t numFrames)
{
    const auto frames = std::min (numFrames, maxFrames (format));
    const auto bps = static_cast<uint32_t> (bytesPerSample (format));
    const auto dataBytes = static_cast<uint32_t> (frames * bps);
    const auto rate = std::isfinite (sampleRate) && sampleRate >= 1.0
                          ? static_cast<uint32_t> (std::min (std::llround (sampleRate), static_cast<long long> (std::numeric_limits<uint32_t>::max())))
                          : uint32_t { 48000 };
    const auto isFloat = format == WavSampleFormat::float32;

    std::vector<uint8_t> out;
    out.reserve (headerBytesFor (format));

    appendTag (out, "RIFF");
    appendU32 (out, static_cast<uint32_t> (riffSizeFor (format, frames)));
    appendTag (out, "WAVE");

    appendTag (out, "fmt ");
    appendU32 (out, isFloat ? 18u : 16u);
    appendU16 (out, isFloat ? formatIeeeFloat : formatPcm);
    appendU16 (out, 1);                     // mono
    appendU32 (out, rate);
    appendU32 (out, rate * bps);            // byte rate
    appendU16 (out, static_cast<uint16_t> (bps)); // block align
    appendU16 (out, static_cast<uint16_t> (bps * 8)); // bits per sample

    if (isFloat)
    {
        appendU16 (out, 0); // cbSize
        appendTag (out, "fact");
        appendU32 (out, 4);
        appendU32 (out, static_cast<uint32_t> (frames));
    }

    appendTag (out, "data");
    appendU32 (out, dataBytes);
    return out;
}

std::size_t WavWriter::encode (std::span<const float> samples, WavSampleFormat format, std::span<uint8_t> out) noexcept
{
    const auto bps = static_cast<std::size_t> (bytesPerSample (format));
    const auto needed = samples.size() * bps;

    if (out.size() < needed)
        return 0;

    auto* dst = out.data();

    if (format == WavSampleFormat::float32)
    {
        for (const auto x : samples)
        {
            const auto v = std::isnan (x) ? 0.0f : x;
            uint32_t bits = 0;
            std::memcpy (&bits, &v, sizeof (bits));
            dst[0] = static_cast<uint8_t> (bits & 0xffu);
            dst[1] = static_cast<uint8_t> ((bits >> 8) & 0xffu);
            dst[2] = static_cast<uint8_t> ((bits >> 16) & 0xffu);
            dst[3] = static_cast<uint8_t> ((bits >> 24) & 0xffu);
            dst += 4;
        }

        return needed;
    }

    for (const auto x : samples)
    {
        // clamp in double first: llround of +-inf or huge values is undefined
        const auto scaled = std::isnan (x) ? 0.0 : std::clamp (static_cast<double> (x) * 8388608.0, -8388608.0, 8388607.0);
        const auto v = static_cast<int32_t> (std::llround (scaled));
        const auto bits = static_cast<uint32_t> (v);
        dst[0] = static_cast<uint8_t> (bits & 0xffu);
        dst[1] = static_cast<uint8_t> ((bits >> 8) & 0xffu);
        dst[2] = static_cast<uint8_t> ((bits >> 16) & 0xffu);
        dst += 3;
    }

    return needed;
}

} // namespace vocalsynth
