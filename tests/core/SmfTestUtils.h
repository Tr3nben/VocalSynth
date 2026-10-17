#pragma once

// Minimal Standard MIDI File reader for the M3 export tests (independent of SmfWriter): parses the header
// and one track into absolute-tick events; fails (REQUIRE) on anything the writer must never produce
// (running status, missing end-of-track, data after it).

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace vocalsynth::smftest
{

struct ParsedEvent
{
    int64_t tick = 0;
    std::vector<uint8_t> bytes; ///< status + data (meta: FF type data...)

    bool isMeta() const { return ! bytes.empty() && bytes[0] == 0xFF; }
    uint8_t status() const { return bytes.empty() ? 0 : bytes[0]; }
    uint8_t kind() const { return static_cast<uint8_t> (status() & 0xF0u); }
};

struct ParsedSmf
{
    int format = -1;
    int numTracks = 0;
    int division = 0;
    std::vector<ParsedEvent> events; ///< in file order, end-of-track included
    int64_t endOfTrackTick = -1;
};

inline uint32_t readBigEndian (std::span<const uint8_t> b, std::size_t at, int numBytes)
{
    uint32_t v = 0;

    for (int i = 0; i < numBytes; ++i)
        v = (v << 8) | b[at + static_cast<std::size_t> (i)];

    return v;
}

inline uint32_t readVariableLength (std::span<const uint8_t> b, std::size_t& pos)
{
    uint32_t v = 0;

    for (int i = 0; i < 4; ++i)
    {
        REQUIRE (pos < b.size());
        const auto byte = b[pos++];
        v = (v << 7) | (byte & 0x7Fu);

        if ((byte & 0x80u) == 0)
            return v;
    }

    FAIL ("variable-length quantity longer than 4 bytes");
    return v;
}

inline ParsedSmf parseSmf (std::span<const uint8_t> b)
{
    ParsedSmf smf;
    REQUIRE (b.size() >= 22);
    REQUIRE (std::string (reinterpret_cast<const char*> (b.data()), 4) == "MThd");
    REQUIRE (readBigEndian (b, 4, 4) == 6);
    smf.format = static_cast<int> (readBigEndian (b, 8, 2));
    smf.numTracks = static_cast<int> (readBigEndian (b, 10, 2));
    smf.division = static_cast<int> (readBigEndian (b, 12, 2));
    REQUIRE (std::string (reinterpret_cast<const char*> (b.data() + 14), 4) == "MTrk");
    const auto length = readBigEndian (b, 18, 4);
    REQUIRE (b.size() == 22 + static_cast<std::size_t> (length));

    std::size_t pos = 22;
    int64_t tick = 0;

    while (pos < b.size())
    {
        REQUIRE (smf.endOfTrackTick < 0); // nothing after end-of-track
        tick += readVariableLength (b, pos);
        REQUIRE (pos < b.size());
        ParsedEvent e;
        e.tick = tick;
        const auto status = b[pos];
        REQUIRE ((status & 0x80u) != 0); // no running status

        if (status == 0xFF)
        {
            REQUIRE (pos + 2 <= b.size());
            const auto type = b[pos + 1];
            auto p = pos + 2;
            const auto len = readVariableLength (b, p);
            REQUIRE (p + len <= b.size());
            e.bytes.assign (b.begin() + static_cast<std::ptrdiff_t> (pos), b.begin() + static_cast<std::ptrdiff_t> (p + len));
            pos = p + len;

            if (type == 0x2F)
                smf.endOfTrackTick = tick;
        }
        else
        {
            const auto kind = status & 0xF0u;
            const std::size_t size = (kind == 0xC0u || kind == 0xD0u) ? 2 : 3;
            REQUIRE (pos + size <= b.size());

            for (std::size_t i = 1; i < size; ++i)
                REQUIRE ((b[pos + i] & 0x80u) == 0);

            e.bytes.assign (b.begin() + static_cast<std::ptrdiff_t> (pos), b.begin() + static_cast<std::ptrdiff_t> (pos + size));
            pos += size;
        }

        smf.events.push_back (e);
    }

    REQUIRE (smf.endOfTrackTick >= 0);
    return smf;
}

} // namespace vocalsynth::smftest
