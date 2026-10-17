// WavWriter (docs/M3-DESIGN.md 11, 16): headers, pad byte, sample encoding, streaming.

#include "vocalsynth/WavWriter.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <algorithm>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <vector>

using namespace vocalsynth;

namespace
{
uint32_t readU32 (const std::vector<uint8_t>& b, std::size_t at)
{
    return static_cast<uint32_t> (b[at]) | (static_cast<uint32_t> (b[at + 1]) << 8) | (static_cast<uint32_t> (b[at + 2]) << 16)
           | (static_cast<uint32_t> (b[at + 3]) << 24);
}

uint16_t readU16 (const std::vector<uint8_t>& b, std::size_t at)
{
    return static_cast<uint16_t> (b[at] | (b[at + 1] << 8));
}

std::string readTag (const std::vector<uint8_t>& b, std::size_t at)
{
    return std::string (reinterpret_cast<const char*> (b.data() + at), 4);
}

int32_t decode24 (const uint8_t* p)
{
    auto v = static_cast<int32_t> (static_cast<uint32_t> (p[0]) | (static_cast<uint32_t> (p[1]) << 8) | (static_cast<uint32_t> (p[2]) << 16));

    if ((v & 0x800000) != 0)
        v -= 0x1000000;

    return v;
}

std::vector<uint8_t> encodeAll (const std::vector<float>& samples, WavSampleFormat format)
{
    std::vector<uint8_t> out (samples.size() * static_cast<std::size_t> (WavWriter::bytesPerSample (format)));
    const auto n = WavWriter::encode (samples, format, out);
    REQUIRE (n == out.size());
    return out;
}
} // namespace

TEST_CASE ("WavWriter: 24-bit PCM header (44 bytes, mono, little endian)", "[wavwriter]")
{
    const auto h = WavWriter::makeHeader (WavSampleFormat::pcm24, 48000.0, 1000);
    REQUIRE (h.size() == 44);
    CHECK (readTag (h, 0) == "RIFF");
    CHECK (readU32 (h, 4) == 36 + 3000);
    CHECK (readTag (h, 8) == "WAVE");
    CHECK (readTag (h, 12) == "fmt ");
    CHECK (readU32 (h, 16) == 16);
    CHECK (readU16 (h, 20) == 1);      // WAVE_FORMAT_PCM
    CHECK (readU16 (h, 22) == 1);      // mono
    CHECK (readU32 (h, 24) == 48000);
    CHECK (readU32 (h, 28) == 144000); // byte rate
    CHECK (readU16 (h, 32) == 3);      // block align
    CHECK (readU16 (h, 34) == 24);
    CHECK (readTag (h, 36) == "data");
    CHECK (readU32 (h, 40) == 3000);
    CHECK (WavWriter::dataPaddingBytes (WavSampleFormat::pcm24, 1000) == 0);
}

TEST_CASE ("WavWriter: odd 24-bit data gets a pad byte counted in the RIFF size only", "[wavwriter]")
{
    const auto h = WavWriter::makeHeader (WavSampleFormat::pcm24, 44100.0, 3);
    CHECK (readU32 (h, 40) == 9);           // data size without the pad byte
    CHECK (readU32 (h, 4) == 36 + 9 + 1);   // RIFF size with it
    CHECK (WavWriter::dataPaddingBytes (WavSampleFormat::pcm24, 3) == 1);
    CHECK (WavWriter::dataPaddingBytes (WavSampleFormat::pcm24, 4) == 0);
    CHECK (WavWriter::dataPaddingBytes (WavSampleFormat::float32, 3) == 0);
    CHECK (readU32 (h, 24) == 44100);
}

TEST_CASE ("WavWriter: 32-bit float header (58 bytes, fmt 18 + fact)", "[wavwriter]")
{
    const auto h = WavWriter::makeHeader (WavSampleFormat::float32, 96000.0, 7);
    REQUIRE (h.size() == 58);
    CHECK (readTag (h, 0) == "RIFF");
    CHECK (readU32 (h, 4) == 50 + 28);
    CHECK (readTag (h, 12) == "fmt ");
    CHECK (readU32 (h, 16) == 18);
    CHECK (readU16 (h, 20) == 3);       // WAVE_FORMAT_IEEE_FLOAT
    CHECK (readU16 (h, 22) == 1);
    CHECK (readU32 (h, 24) == 96000);
    CHECK (readU32 (h, 28) == 384000);
    CHECK (readU16 (h, 32) == 4);
    CHECK (readU16 (h, 34) == 32);
    CHECK (readU16 (h, 36) == 0);       // cbSize
    CHECK (readTag (h, 38) == "fact");
    CHECK (readU32 (h, 42) == 4);
    CHECK (readU32 (h, 46) == 7);
    CHECK (readTag (h, 50) == "data");
    CHECK (readU32 (h, 54) == 28);
}

TEST_CASE ("WavWriter: sample rate rounded, frame count clamped to the RIFF limit", "[wavwriter]")
{
    CHECK (readU32 (WavWriter::makeHeader (WavSampleFormat::pcm24, 44099.6, 0), 24) == 44100);

    for (const auto format : { WavSampleFormat::pcm24, WavSampleFormat::float32 })
    {
        const auto maxFrames = WavWriter::maxFrames (format);
        const auto bps = static_cast<uint64_t> (WavWriter::bytesPerSample (format));
        const auto header = static_cast<uint64_t> (WavWriter::makeHeader (format, 48000.0, 0).size());
        const auto riffSize = [&] (uint64_t n) { return header - 8 + n * bps + ((n * bps) & 1u); };
        CHECK (riffSize (maxFrames) <= 0xFFFFFFFFull);
        CHECK (riffSize (maxFrames + 1) > 0xFFFFFFFFull);
        // 24-bit: > 8 h of 48 kHz mono, float: > 6 h; never reached (history <= 30 min)
        CHECK (maxFrames / 48000 / 3600 >= (format == WavSampleFormat::pcm24 ? 8u : 6u));

        const auto h = WavWriter::makeHeader (format, 48000.0, maxFrames + 12345);
        CHECK (readU32 (h, h.size() - 4) == static_cast<uint32_t> (maxFrames * bps));
    }
}

TEST_CASE ("WavWriter: 24-bit encoding rounds, clamps and maps NaN to 0", "[wavwriter]")
{
    const std::vector<float> samples { 0.0f, 0.5f, -0.5f, 1.0f, -1.0f, 2.0f, -3.0f, std::numeric_limits<float>::quiet_NaN(),
                                       std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(),
                                       1.0f / 8388608.0f, 0.6f / 8388608.0f, -0.6f / 8388608.0f, 0.4f / 8388608.0f };
    const auto bytes = encodeAll (samples, WavSampleFormat::pcm24);
    const std::vector<int32_t> expected { 0, 4194304, -4194304, 8388607, -8388608, 8388607, -8388608, 0,
                                          8388607, -8388608, 1, 1, -1, 0 };

    for (std::size_t i = 0; i < samples.size(); ++i)
    {
        INFO ("sample " << i);
        CHECK (decode24 (bytes.data() + 3 * i) == expected[i]);
    }

    // little endian byte order of 0x400000
    CHECK (bytes[3] == 0x00);
    CHECK (bytes[4] == 0x00);
    CHECK (bytes[5] == 0x40);
}

TEST_CASE ("WavWriter: float encoding is the IEEE value, NaN -> 0", "[wavwriter]")
{
    const std::vector<float> samples { 0.25f, -1.5f, std::numeric_limits<float>::quiet_NaN(), 3.0f };
    const auto bytes = encodeAll (samples, WavSampleFormat::float32);

    for (std::size_t i = 0; i < samples.size(); ++i)
    {
        float v = 0.0f;
        std::memcpy (&v, bytes.data() + 4 * i, 4);
        CHECK (v == (std::isnan (samples[i]) ? 0.0f : samples[i]));
    }

    CHECK (bytes[0] == 0x00); // 0.25f = 0x3E800000, little endian
    CHECK (bytes[3] == 0x3E);
}

TEST_CASE ("WavWriter: encode refuses a too small buffer", "[wavwriter]")
{
    const std::vector<float> samples (10, 0.1f);
    std::vector<uint8_t> out (29);
    CHECK (WavWriter::encode (samples, WavSampleFormat::pcm24, out) == 0);
    out.resize (30);
    CHECK (WavWriter::encode (samples, WavSampleFormat::pcm24, out) == 30);
    CHECK (WavWriter::encode ({}, WavSampleFormat::float32, {}) == 0);
}

TEST_CASE ("WavWriter: streaming in blocks gives the same bytes as one block", "[wavwriter]")
{
    std::vector<float> samples (10007);

    for (std::size_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<float> (0.9 * std::sin (0.013 * static_cast<double> (i)) + 0.2 * std::sin (0.31 * static_cast<double> (i)));

    for (const auto format : { WavSampleFormat::pcm24, WavSampleFormat::float32 })
    {
        const auto whole = encodeAll (samples, format);
        std::vector<uint8_t> streamed;
        std::vector<uint8_t> scratch (1000 * 4);

        for (std::size_t pos = 0; pos < samples.size();)
        {
            const auto n = std::min<std::size_t> ({ 1 + pos % 997, samples.size() - pos, 1000 });
            const auto bytes = WavWriter::encode (std::span<const float> { samples.data() + pos, n }, format, scratch);
            REQUIRE (bytes == n * static_cast<std::size_t> (WavWriter::bytesPerSample (format)));
            streamed.insert (streamed.end(), scratch.begin(), scratch.begin() + static_cast<std::ptrdiff_t> (bytes));
            pos += n;
        }

        CHECK (streamed == whole);

        // A complete file: header + data + pad has the size the header announces
        const auto header = WavWriter::makeHeader (format, 48000.0, samples.size());
        const auto fileSize = header.size() + whole.size() + static_cast<std::size_t> (WavWriter::dataPaddingBytes (format, samples.size()));
        CHECK (readU32 (header, 4) + 8 == fileSize);
    }
}
