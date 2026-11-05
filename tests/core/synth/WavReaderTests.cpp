// WavReader: PCM 8/16/24/32, float 32/64, extensible, stereo -> mid, clm chunk, padding, chunk order, truncated data, fuzz.
// Test plan: docs/SYNTH-DESIGN.md 21. Owner: agent A (Wavetable).

#include "vocalsynth/synth/WavReader.h"

#include "vocalsynth/WavWriter.h"
#include "vocalsynth/synth/WavetableBuilder.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>
#include <string>

using namespace vocalsynth::synth;

namespace
{
/** A small WAV writer for the tests: any format tag / bit depth / channel count, extra chunks, any order. */
struct WavBuilder
{
    std::vector<uint8_t> bytes;

    void u16 (uint16_t v)
    {
        bytes.push_back (static_cast<uint8_t> (v & 0xff));
        bytes.push_back (static_cast<uint8_t> (v >> 8));
    }

    void u32 (uint32_t v)
    {
        for (int s = 0; s < 32; s += 8)
            bytes.push_back (static_cast<uint8_t> ((v >> s) & 0xff));
    }

    void tag (const char* t) { bytes.insert (bytes.end(), t, t + 4); }

    void chunk (const char* id, const std::vector<uint8_t>& body, bool pad = true)
    {
        tag (id);
        u32 (static_cast<uint32_t> (body.size()));
        bytes.insert (bytes.end(), body.begin(), body.end());

        if (pad && body.size() % 2 == 1)
            bytes.push_back (0);
    }

    static std::vector<uint8_t> fmtBody (uint16_t format, int channels, int bits, bool extensible = false, uint16_t subFormat = 0)
    {
        WavBuilder b;
        b.bytes.clear(); // a chunk body, not a file
        b.u16 (extensible ? uint16_t { 0xFFFE } : format);
        b.u16 (static_cast<uint16_t> (channels));
        b.u32 (48000);
        b.u32 (static_cast<uint32_t> (48000 * channels * bits / 8));
        b.u16 (static_cast<uint16_t> (channels * bits / 8));
        b.u16 (static_cast<uint16_t> (bits));

        if (extensible)
        {
            b.u16 (22);
            b.u16 (static_cast<uint16_t> (bits));
            b.u32 (channels == 1 ? 4u : 3u);
            b.u16 (subFormat);
            const uint8_t tail[14] = { 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 };
            b.bytes.insert (b.bytes.end(), tail, tail + 14);
        }

        return b.bytes;
    }

    /** RIFF header (size patched by finish()). */
    WavBuilder()
    {
        tag ("RIFF");
        u32 (0);
        tag ("WAVE");
    }

    std::vector<std::byte> finish (bool patchSize = true)
    {
        if (patchSize)
        {
            const auto riff = static_cast<uint32_t> (bytes.size() - 8);
            for (int i = 0; i < 4; ++i)
                bytes[4 + static_cast<std::size_t> (i)] = static_cast<uint8_t> ((riff >> (8 * i)) & 0xff);
        }

        std::vector<std::byte> out (bytes.size());
        std::memcpy (out.data(), bytes.data(), bytes.size());
        return out;
    }
};

/** Encodes interleaved samples (-1..1) in the given format. */
std::vector<uint8_t> encode (const std::vector<double>& x, uint16_t format, int bits)
{
    WavBuilder b;
    b.bytes.clear();

    for (const double v : x)
    {
        if (format == 3 && bits == 32)
        {
            const float f = static_cast<float> (v);
            uint32_t u;
            std::memcpy (&u, &f, 4);
            b.u32 (u);
        }
        else if (format == 3)
        {
            uint64_t u;
            std::memcpy (&u, &v, 8);
            b.u32 (static_cast<uint32_t> (u));
            b.u32 (static_cast<uint32_t> (u >> 32));
        }
        else if (bits == 8)
            b.bytes.push_back (static_cast<uint8_t> (std::clamp (std::lround (v * 128.0) + 128, 0L, 255L)));
        else if (bits == 16)
            b.u16 (static_cast<uint16_t> (static_cast<int16_t> (std::lround (v * 32767.0))));
        else if (bits == 24)
        {
            const auto i = static_cast<int32_t> (std::lround (v * 8388607.0));
            b.bytes.push_back (static_cast<uint8_t> (i & 0xff));
            b.bytes.push_back (static_cast<uint8_t> ((i >> 8) & 0xff));
            b.bytes.push_back (static_cast<uint8_t> ((i >> 16) & 0xff));
        }
        else
            b.u32 (static_cast<uint32_t> (static_cast<int32_t> (std::llround (v * 2147483647.0))));
    }

    return b.bytes;
}

std::vector<double> ramp (std::size_t n, int channels)
{
    std::vector<double> x (n * static_cast<std::size_t> (channels));

    for (std::size_t i = 0; i < n; ++i)
        for (int c = 0; c < channels; ++c)
            x[i * static_cast<std::size_t> (channels) + static_cast<std::size_t> (c)]
                = 0.9 * std::sin (2.0 * std::numbers::pi * static_cast<double> (i) / 64.0 + c);

    return x;
}

std::vector<std::byte> simpleWav (const std::vector<double>& x, uint16_t format, int bits, int channels, bool extensible = false)
{
    WavBuilder w;
    w.chunk ("fmt ", WavBuilder::fmtBody (format, channels, bits, extensible, format));
    w.chunk ("data", encode (x, format, bits));
    return w.finish();
}
} // namespace

TEST_CASE ("WavReader: every PCM / float format, mono and multichannel -> mean", "[synth][wav]")
{
    struct Case
    {
        uint16_t format;
        int bits;
        double tolerance;
    };

    for (const auto c : { Case { 1, 8, 1.0 / 100.0 }, Case { 1, 16, 1e-4 }, Case { 1, 24, 1e-6 }, Case { 1, 32, 1e-7 },
                          Case { 3, 32, 1e-7 }, Case { 3, 64, 1e-7 } })
        for (const int channels : { 1, 2, 3, 8 })
            for (const bool extensible : { false, true })
            {
                const auto x = ramp (300, channels);
                const auto result = WavReader::read (simpleWav (x, c.format, c.bits, channels, extensible));
                CAPTURE (c.format, c.bits, channels, extensible);
                REQUIRE (result.ok());
                CHECK (result.numChannels == channels);
                CHECK (result.bitsPerSample == c.bits);
                CHECK (result.floatingPoint == (c.format == 3));
                CHECK (result.sampleRate == 48000.0);
                CHECK (result.clmFrameSize == 0);
                CHECK (! result.dataTruncated);
                REQUIRE (result.samples.size() == 300);

                for (std::size_t i = 0; i < 300; ++i)
                {
                    double mean = 0.0;

                    for (int ch = 0; ch < channels; ++ch)
                        mean += x[i * static_cast<std::size_t> (channels) + static_cast<std::size_t> (ch)];

                    mean /= channels;
                    REQUIRE (std::abs (result.samples[i] - mean) < c.tolerance * 1.01 + 5e-7);
                }
            }

    // Full-scale integer values map to [-1, 1)
    const std::vector<uint8_t> extremes16 = { 0x00, 0x80, 0xff, 0x7f };
    WavBuilder w;
    w.chunk ("fmt ", WavBuilder::fmtBody (1, 1, 16));
    w.chunk ("data", extremes16);
    const auto r16 = WavReader::read (w.finish());
    REQUIRE (r16.ok());
    CHECK (r16.samples[0] == -1.0f);
    CHECK (r16.samples[1] == 32767.0f / 32768.0f);
}

TEST_CASE ("WavReader: agrees with the project's own WavWriter", "[synth][wav]")
{
    for (const auto format : { vocalsynth::WavSampleFormat::pcm24, vocalsynth::WavSampleFormat::float32 })
    {
        std::vector<float> x (1001);

        for (std::size_t i = 0; i < x.size(); ++i)
            x[i] = static_cast<float> (0.7 * std::sin (0.01 * static_cast<double> (i)));

        auto bytes = vocalsynth::WavWriter::makeHeader (format, 44100.0, x.size());
        const auto headerSize = bytes.size();
        bytes.resize (headerSize + x.size() * static_cast<std::size_t> (vocalsynth::WavWriter::bytesPerSample (format)));
        vocalsynth::WavWriter::encode (x, format, std::span<uint8_t> (bytes).subspan (headerSize));
        bytes.resize (bytes.size() + static_cast<std::size_t> (vocalsynth::WavWriter::dataPaddingBytes (format, x.size())), 0);

        std::vector<std::byte> data (bytes.size());
        std::memcpy (data.data(), bytes.data(), bytes.size());
        const auto result = WavReader::read (data);
        REQUIRE (result.ok());
        CHECK (result.sampleRate == 44100.0);
        REQUIRE (result.samples.size() == x.size());

        for (std::size_t i = 0; i < x.size(); ++i)
            REQUIRE (std::abs (result.samples[i] - x[i]) < 2e-7);
    }
}

TEST_CASE ("WavReader: clm chunk (Serum style), chunk order, padding, unknown chunks", "[synth][wav]")
{
    CHECK (WavReader::parseClmFrameSize ("<!>2048 01000000 wavetable (www.xferrecords.com)") == 2048);
    CHECK (WavReader::parseClmFrameSize ("<!>256") == 256);
    CHECK (WavReader::parseClmFrameSize ("<!>32 x") == 32);
    CHECK (WavReader::parseClmFrameSize ("<!>8192") == 8192);
    CHECK (WavReader::parseClmFrameSize ("<!>16384") == 0);
    CHECK (WavReader::parseClmFrameSize ("<!>16") == 0);
    CHECK (WavReader::parseClmFrameSize ("<!>1000") == 0);
    CHECK (WavReader::parseClmFrameSize ("<!>") == 0);
    CHECK (WavReader::parseClmFrameSize ("2048") == 0);
    CHECK (WavReader::parseClmFrameSize (" <!>2048") == 0);
    CHECK (WavReader::parseClmFrameSize ("<!>99999999999999999992048") == 0);
    CHECK (WavReader::parseClmFrameSize ("") == 0);

    const auto x = ramp (4096, 1);
    const std::string clmText = "<!>2048 10000000 wavetable (vocalsynth test)"; // 44 bytes
    std::vector<uint8_t> clm (clmText.begin(), clmText.end());
    std::vector<uint8_t> odd { 1, 2, 3 }; // odd chunk: one pad byte follows

    // data first, then an odd unknown chunk, clm, LIST, fmt last
    WavBuilder w;
    w.chunk ("data", encode (x, 1, 16));
    w.chunk ("junk", odd);
    w.chunk ("clm ", clm);
    w.chunk ("LIST", { 'I', 'N', 'F', 'O' });
    w.chunk ("fmt ", WavBuilder::fmtBody (1, 1, 16));
    const auto result = WavReader::read (w.finish());
    REQUIRE (result.ok());
    CHECK (result.clmFrameSize == 2048);
    CHECK (result.samples.size() == 4096);

    // An odd-sized clm chunk before fmt / data
    WavBuilder w2;
    const std::string oddText = "<!>512 x"; // 8 + 1 = 9 bytes with the terminating 'y'
    std::vector<uint8_t> clm2 (oddText.begin(), oddText.end());
    clm2.push_back ('y');
    w2.chunk ("clm ", clm2);
    w2.chunk ("fmt ", WavBuilder::fmtBody (1, 2, 24));
    w2.chunk ("data", encode (ramp (100, 2), 1, 24));
    const auto r2 = WavReader::read (w2.finish());
    REQUIRE (r2.ok());
    CHECK (r2.clmFrameSize == 512);
    CHECK (r2.samples.size() == 100);

    // A 24-bit mono file with an odd data size and its pad byte, followed by another chunk
    WavBuilder w3;
    w3.chunk ("fmt ", WavBuilder::fmtBody (1, 1, 24));
    w3.chunk ("data", encode (ramp (5, 1), 1, 24)); // 15 bytes + pad
    w3.chunk ("cue ", { 0, 0, 0, 0 });
    const auto r3 = WavReader::read (w3.finish());
    REQUIRE (r3.ok());
    CHECK (r3.samples.size() == 5);

    // The RIFF size is not trusted (0 / garbage from streaming writers)
    WavBuilder w4;
    w4.chunk ("fmt ", WavBuilder::fmtBody (1, 1, 16));
    w4.chunk ("data", encode (ramp (10, 1), 1, 16));
    auto bytes4 = w4.finish (false);
    CHECK (WavReader::read (bytes4).ok());

    // Imported end to end: 2 frames of 2048 with clm, as a table
    WavBuilder w5;
    w5.chunk ("fmt ", WavBuilder::fmtBody (3, 1, 32));
    w5.chunk ("clm ", clm);
    w5.chunk ("data", encode (ramp (4096, 1), 3, 32));
    const auto wav = WavReader::read (w5.finish());
    const auto table = WavetableBuilder::buildFromWav ("serum.wav", wav, 0);
    REQUIRE (table.ok());
    CHECK (table.table->getNumFrames() == 2);
}

TEST_CASE ("WavReader: truncated data, huge sizes, zero frames, clean errors", "[synth][wav]")
{
    const auto x = ramp (1000, 2);

    // Data chunk longer than the file: the complete frames present are read
    {
        WavBuilder w;
        w.chunk ("fmt ", WavBuilder::fmtBody (1, 2, 16));
        auto data = encode (x, 1, 16);
        w.tag ("data");
        w.u32 (static_cast<uint32_t> (data.size() + 1000));
        w.bytes.insert (w.bytes.end(), data.begin(), data.end() - 3); // the last frame is incomplete
        const auto result = WavReader::read (w.finish());
        REQUIRE (result.ok());
        CHECK (result.dataTruncated);
        CHECK (result.samples.size() == 999);
    }

    // data size 0xFFFFFFFF (streaming writer)
    {
        WavBuilder w;
        w.chunk ("fmt ", WavBuilder::fmtBody (1, 1, 16));
        w.tag ("data");
        w.u32 (0xFFFFFFFFu);
        const auto data = encode (ramp (64, 1), 1, 16);
        w.bytes.insert (w.bytes.end(), data.begin(), data.end());
        const auto result = WavReader::read (w.finish());
        REQUIRE (result.ok());
        CHECK (result.dataTruncated);
        CHECK (result.samples.size() == 64);
    }

    // A huge unknown chunk before fmt: the file is cut off before the format
    {
        WavBuilder w;
        w.tag ("JUNK");
        w.u32 (0x7FFFFFF0u);
        w.u32 (0);
        CHECK (WavReader::read (w.finish()).error == WavReadError::missingFormat);
    }

    // Zero frames / no data / data smaller than one frame
    {
        WavBuilder w;
        w.chunk ("fmt ", WavBuilder::fmtBody (1, 2, 24));
        w.chunk ("data", {});
        CHECK (WavReader::read (w.finish()).error == WavReadError::missingData);

        WavBuilder w2;
        w2.chunk ("fmt ", WavBuilder::fmtBody (1, 2, 24));
        w2.chunk ("data", { 1, 2, 3, 4, 5 });
        CHECK (WavReader::read (w2.finish()).error == WavReadError::missingData);

        WavBuilder w3;
        w3.chunk ("fmt ", WavBuilder::fmtBody (1, 1, 16));
        CHECK (WavReader::read (w3.finish()).error == WavReadError::missingData);
    }

    // No fmt, broken fmt, unsupported formats
    {
        WavBuilder w;
        w.chunk ("data", encode (ramp (10, 1), 1, 16));
        CHECK (WavReader::read (w.finish()).error == WavReadError::missingFormat);

        WavBuilder cut; // fmt chunk cut off
        cut.tag ("fmt ");
        cut.u32 (16);
        cut.u16 (1);
        CHECK (WavReader::read (cut.finish()).error == WavReadError::truncated);

        WavBuilder header; // chunk header cut off
        header.tag ("fm");
        CHECK (WavReader::read (header.finish()).error == WavReadError::truncated);

        const auto unsupported = [] (const std::vector<uint8_t>& fmt)
        {
            WavBuilder u;
            u.chunk ("fmt ", fmt);
            u.chunk ("data", std::vector<uint8_t> (64, 0));
            return WavReader::read (u.finish()).error == WavReadError::unsupportedFormat;
        };

        CHECK (unsupported (WavBuilder::fmtBody (2, 1, 16)));     // ADPCM
        CHECK (unsupported (WavBuilder::fmtBody (1, 1, 12)));     // 12-bit
        CHECK (unsupported (WavBuilder::fmtBody (3, 1, 16)));     // 16-bit float
        CHECK (unsupported (WavBuilder::fmtBody (1, 9, 16)));     // 9 channels
        CHECK (unsupported (WavBuilder::fmtBody (1, 0, 16)));     // 0 channels
        CHECK (unsupported (WavBuilder::fmtBody (0, 1, 16, true, 2))); // extensible ADPCM
        auto shortFmt = WavBuilder::fmtBody (1, 1, 16);
        shortFmt.resize (14);
        CHECK (unsupported (shortFmt));
        auto shortExt = WavBuilder::fmtBody (0, 1, 16, true, 1);
        shortExt.resize (30);
        CHECK (unsupported (shortExt));
        auto badGuid = WavBuilder::fmtBody (0, 1, 16, true, 1);
        badGuid.back() ^= 0x55;
        CHECK (unsupported (badGuid));
    }

    // Not RIFF / WAVE, RIFX, RF64, empty
    CHECK (WavReader::read ({}).error == WavReadError::empty);
    {
        const char text[] = "RIFX\0\0\0\0WAVEfmt ";
        std::vector<std::byte> b (sizeof (text));
        std::memcpy (b.data(), text, sizeof (text));
        CHECK (WavReader::read (b).error == WavReadError::notRiffWave);
        b[3] = std::byte { 'F' };
        b[8] = std::byte { 'A' };
        CHECK (WavReader::read (b).error == WavReadError::notRiffWave);
        CHECK (WavReader::read (std::span (b).first (11)).error == WavReadError::notRiffWave);
        std::memcpy (b.data(), "RF64", 4);
        CHECK (WavReader::read (b).error == WavReadError::notRiffWave);
    }

    // Header only
    {
        WavBuilder w;
        CHECK (WavReader::read (w.finish()).error == WavReadError::missingFormat);
    }

    // Too long: more than 256 x 4096 frames (8-bit mono keeps the test small)
    {
        WavBuilder w;
        w.chunk ("fmt ", WavBuilder::fmtBody (1, 1, 8));
        w.chunk ("data", std::vector<uint8_t> (WavReader::maxSampleFrames + 1, 128));
        CHECK (WavReader::read (w.finish()).error == WavReadError::tooLong);

        WavBuilder ok;
        ok.chunk ("fmt ", WavBuilder::fmtBody (1, 1, 8));
        ok.chunk ("data", std::vector<uint8_t> (WavReader::maxSampleFrames, 128));
        CHECK (WavReader::read (ok.finish()).ok());
    }

    // Non-finite floats read as 0
    {
        WavBuilder w;
        w.chunk ("fmt ", WavBuilder::fmtBody (3, 1, 32));
        w.chunk ("data", encode ({ std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(), 0.5 }, 3, 32));
        const auto r = WavReader::read (w.finish());
        REQUIRE (r.ok());
        CHECK (r.samples == std::vector<float> { 0.0f, 0.0f, 0.5f });
    }

    // Every error has a text
    for (int e = 0; e <= static_cast<int> (WavReadError::tooLong); ++e)
        CHECK (! describeWavReadError (static_cast<WavReadError> (e)).empty());

    CHECK (describeWavReadError (WavReadError::unsupportedFormat) == "Unsupported WAV format");
}

TEST_CASE ("WavReader: fuzz (random bytes, mutated and cut valid files): no crash, clean result", "[synth][wav]")
{
    WavBuilder base;
    const std::string clmText = "<!>256 wavetable";
    base.chunk ("clm ", std::vector<uint8_t> (clmText.begin(), clmText.end()));
    base.chunk ("fmt ", WavBuilder::fmtBody (0, 2, 24, true, 1));
    base.chunk ("data", encode (ramp (200, 2), 1, 24));
    const auto valid = base.finish();
    REQUIRE (WavReader::read (valid).ok());

    uint32_t state = 12345u;
    const auto next = [&state]
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    };

    int okCount = 0;

    for (int i = 0; i < 10000; ++i)
    {
        std::vector<std::byte> bytes;

        switch (i % 4)
        {
            case 0: // random bytes, sometimes with a valid RIFF / WAVE header
            {
                bytes.resize (next() % 300);

                for (auto& b : bytes)
                    b = static_cast<std::byte> (next() & 0xff);

                if (bytes.size() >= 12 && next() % 2 == 0)
                {
                    std::memcpy (bytes.data(), "RIFF", 4);
                    std::memcpy (bytes.data() + 8, "WAVE", 4);
                }
                break;
            }
            case 1: // valid file with random byte flips
            case 2:
            {
                bytes = valid;
                const int flips = 1 + static_cast<int> (next() % 8);

                for (int f = 0; f < flips; ++f)
                    bytes[next() % bytes.size()] = static_cast<std::byte> (next() & 0xff);
                break;
            }
            default: // valid file cut anywhere
                bytes.assign (valid.begin(), valid.begin() + static_cast<std::ptrdiff_t> (next() % valid.size()));
                break;
        }

        const auto result = WavReader::read (bytes);

        if (result.ok())
        {
            ++okCount;
            REQUIRE (! result.samples.empty());
            REQUIRE (result.samples.size() <= WavReader::maxSampleFrames);
            REQUIRE (result.numChannels >= 1);
            REQUIRE (result.numChannels <= 8);

            for (const auto v : result.samples)
                REQUIRE (std::isfinite (v));
        }
        else
        {
            REQUIRE (result.samples.empty());
            REQUIRE (! describeWavReadError (result.error).empty());
        }
    }

    CHECK (okCount > 100); // the mutations keep many files readable
}
