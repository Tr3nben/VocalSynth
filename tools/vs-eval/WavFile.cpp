#include "WavFile.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>

namespace vseval
{

namespace
{

constexpr uint16_t formatPcm = 1;
constexpr uint16_t formatFloat = 3;
constexpr uint16_t formatExtensible = 0xfffe;

uint16_t readU16 (const uint8_t* p) { return static_cast<uint16_t> (p[0] | (p[1] << 8)); }

uint32_t readU32 (const uint8_t* p)
{
    return static_cast<uint32_t> (p[0]) | (static_cast<uint32_t> (p[1]) << 8) | (static_cast<uint32_t> (p[2]) << 16)
           | (static_cast<uint32_t> (p[3]) << 24);
}

/** One sample of the given format at p, as -1..1. */
double decodeSample (const uint8_t* p, uint16_t format, int bits)
{
    if (format == formatFloat)
    {
        if (bits == 32)
        {
            float f;
            const auto u = readU32 (p);
            std::memcpy (&f, &u, sizeof (f));
            return static_cast<double> (f);
        }

        uint64_t u = 0;

        for (int i = 7; i >= 0; --i)
            u = (u << 8) | p[i];

        double d;
        std::memcpy (&d, &u, sizeof (d));
        return d;
    }

    switch (bits)
    {
        case 8:  return (static_cast<double> (p[0]) - 128.0) / 128.0; // 8-bit WAV is unsigned
        case 16: return static_cast<double> (static_cast<int16_t> (readU16 (p))) / 32768.0;
        case 24:
        {
            auto v = static_cast<int32_t> (p[0] | (p[1] << 8) | (p[2] << 16));

            if ((v & 0x800000) != 0)
                v -= 0x1000000;

            return static_cast<double> (v) / 8388608.0;
        }
        case 32: return static_cast<double> (static_cast<int32_t> (readU32 (p))) / 2147483648.0;
        default: return 0.0;
    }
}

void writeU16 (std::ofstream& out, uint16_t v)
{
    const char b[2] = { static_cast<char> (v & 0xff), static_cast<char> (v >> 8) };
    out.write (b, 2);
}

void writeU32 (std::ofstream& out, uint32_t v)
{
    const char b[4] = { static_cast<char> (v & 0xff), static_cast<char> ((v >> 8) & 0xff),
                        static_cast<char> ((v >> 16) & 0xff), static_cast<char> ((v >> 24) & 0xff) };
    out.write (b, 4);
}

} // namespace

bool readWav (const std::string& path, WavData& out, std::string& error)
{
    out = {};
    std::ifstream file (path, std::ios::binary);

    if (! file)
    {
        error = "cannot open " + path;
        return false;
    }

    const std::vector<uint8_t> bytes ((std::istreambuf_iterator<char> (file)), std::istreambuf_iterator<char>());

    if (bytes.size() < 12 || std::memcmp (bytes.data(), "RIFF", 4) != 0 || std::memcmp (bytes.data() + 8, "WAVE", 4) != 0)
    {
        error = path + ": not a RIFF/WAVE file";
        return false;
    }

    uint16_t format = 0, channels = 0, bits = 0, blockAlign = 0;
    uint32_t sampleRate = 0;
    const uint8_t* data = nullptr;
    std::size_t dataSize = 0;
    std::size_t pos = 12;

    while (pos + 8 <= bytes.size())
    {
        const auto* chunk = bytes.data() + pos;
        const auto size = static_cast<std::size_t> (readU32 (chunk + 4));
        const auto available = std::min (size, bytes.size() - (pos + 8)); // truncated files: use what is there

        if (std::memcmp (chunk, "fmt ", 4) == 0 && available >= 16)
        {
            format = readU16 (chunk + 8);
            channels = readU16 (chunk + 10);
            sampleRate = readU32 (chunk + 12);
            blockAlign = readU16 (chunk + 20);
            bits = readU16 (chunk + 22);

            if (format == formatExtensible && available >= 40)
                format = readU16 (chunk + 8 + 24); // first two bytes of the sub-format GUID
        }
        else if (std::memcmp (chunk, "data", 4) == 0)
        {
            data = chunk + 8;
            dataSize = available;
        }

        pos += 8 + size + (size & 1); // chunks are padded to an even size
    }

    if (format == 0 || data == nullptr)
    {
        error = path + ": missing fmt or data chunk";
        return false;
    }

    const auto supported = (format == formatPcm && (bits == 8 || bits == 16 || bits == 24 || bits == 32))
                           || (format == formatFloat && (bits == 32 || bits == 64));

    if (! supported || channels == 0 || sampleRate == 0 || blockAlign < channels * ((bits + 7) / 8))
    {
        error = path + ": unsupported format (tag " + std::to_string (format) + ", " + std::to_string (bits)
                + " bits, " + std::to_string (channels) + " channels)";
        return false;
    }

    const auto bytesPerSample = static_cast<std::size_t> ((bits + 7) / 8);
    const auto numFrames = dataSize / blockAlign;
    out.sampleRate = static_cast<double> (sampleRate);
    out.numChannels = channels;
    out.mono.resize (numFrames);

    for (std::size_t i = 0; i < numFrames; ++i)
    {
        double sum = 0.0;

        for (std::size_t c = 0; c < channels; ++c)
            sum += decodeSample (data + i * blockAlign + c * bytesPerSample, format, bits);

        out.mono[i] = static_cast<float> (sum / static_cast<double> (channels));
    }

    return true;
}

bool writeWavMono (const std::string& path, const std::vector<float>& samples, double sampleRate)
{
    std::ofstream out (path, std::ios::binary);

    if (! out || ! (sampleRate > 0.0))
        return false;

    const auto rate = static_cast<uint32_t> (sampleRate + 0.5);
    const auto dataBytes = static_cast<uint32_t> (samples.size() * 4);

    out.write ("RIFF", 4);
    writeU32 (out, 4 + (8 + 16) + (8 + 4) + (8 + dataBytes));
    out.write ("WAVE", 4);

    out.write ("fmt ", 4);
    writeU32 (out, 16);
    writeU16 (out, formatFloat);
    writeU16 (out, 1);        // channels
    writeU32 (out, rate);
    writeU32 (out, rate * 4); // byte rate
    writeU16 (out, 4);        // block align
    writeU16 (out, 32);       // bits

    out.write ("fact", 4);    // required for non-PCM formats
    writeU32 (out, 4);
    writeU32 (out, static_cast<uint32_t> (samples.size()));

    out.write ("data", 4);
    writeU32 (out, dataBytes);

    for (const auto s : samples)
    {
        uint32_t u;
        std::memcpy (&u, &s, sizeof (u));
        writeU32 (out, u);
    }

    return static_cast<bool> (out);
}

} // namespace vseval
