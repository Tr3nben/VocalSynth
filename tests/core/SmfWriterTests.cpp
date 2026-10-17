// SmfWriter (docs/M3-DESIGN.md 10, 16): exact bytes, meta order and encodings, RPN, VLQ, end-of-track,
// running-status-free output, determinism.

#include "vocalsynth/SmfWriter.h"

#include "SmfTestUtils.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace vocalsynth;
using namespace vocalsynth::smftest;

namespace
{
RenderedTake emptyTake (int ppq = 960)
{
    RenderedTake t;
    t.ticksPerQuarterNote = ppq;
    t.tempos = { { 0, 120.0 } };
    t.timeSignatures = { { 0, 4, 4 } };
    t.endTick = 4 * ppq;
    return t;
}

std::vector<uint8_t> vlq (uint32_t v)
{
    std::vector<uint8_t> out;
    SmfWriter::appendVariableLength (out, v);
    return out;
}

std::vector<uint8_t> metaBytes (const ParsedSmf& smf, uint8_t type, std::size_t index = 0)
{
    std::size_t seen = 0;

    for (const auto& e : smf.events)
        if (e.isMeta() && e.bytes[1] == type && seen++ == index)
            return e.bytes;

    return {};
}
} // namespace

TEST_CASE ("SmfWriter: variable-length quantities at the edges", "[smfwriter]")
{
    CHECK (vlq (0) == std::vector<uint8_t> { 0x00 });
    CHECK (vlq (0x40) == std::vector<uint8_t> { 0x40 });
    CHECK (vlq (0x7F) == std::vector<uint8_t> { 0x7F });
    CHECK (vlq (0x80) == std::vector<uint8_t> { 0x81, 0x00 });
    CHECK (vlq (0x2000) == std::vector<uint8_t> { 0xC0, 0x00 });
    CHECK (vlq (0x3FFF) == std::vector<uint8_t> { 0xFF, 0x7F });
    CHECK (vlq (0x4000) == std::vector<uint8_t> { 0x81, 0x80, 0x00 });
    CHECK (vlq (0x1FFFFF) == std::vector<uint8_t> { 0xFF, 0xFF, 0x7F });
    CHECK (vlq (0x200000) == std::vector<uint8_t> { 0x81, 0x80, 0x80, 0x00 });
    CHECK (vlq (0x0FFFFFFF) == std::vector<uint8_t> { 0xFF, 0xFF, 0xFF, 0x7F });
    CHECK (vlq (0xFFFFFFFF) == std::vector<uint8_t> { 0xFF, 0xFF, 0xFF, 0x7F }); // clamped
}

TEST_CASE ("SmfWriter: minimal file, exact bytes (M1 layout: tempo, 4/4, end-of-track)", "[smfwriter]")
{
    const std::vector<uint8_t> expected {
        'M', 'T', 'h', 'd', 0x00, 0x00, 0x00, 0x06, 0x00, 0x01, 0x00, 0x01, 0x03, 0xC0,
        'M', 'T', 'r', 'k', 0x00, 0x00, 0x00, 0x14,
        0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20,       // 500000 us = 120 BPM
        0x00, 0xFF, 0x58, 0x04, 0x04, 0x02, 0x18, 0x08, // 4/4
        0x9E, 0x00, 0xFF, 0x2F, 0x00                    // end-of-track at 3840
    };
    CHECK (SmfWriter::write (emptyTake(), {}) == expected);

    SmfOptions type0;
    type0.format = SmfFormat::type0;
    const auto bytes = SmfWriter::write (emptyTake(), type0);
    CHECK (bytes[8] == 0x00);
    CHECK (bytes[9] == 0x00);
}

TEST_CASE ("SmfWriter: golden bytes with name, RPN, notes, bend, CC, tempo and time-signature changes", "[smfwriter]")
{
    RenderedTake t;
    t.ticksPerQuarterNote = 480;
    t.tempos = { { 0, 97.5 }, { 1920, 140.0 } };
    t.timeSignatures = { { 0, 3, 4 }, { 1440, 7, 8 } };
    t.bendRangeSemitones = 2;
    t.events = {
        { 0, TickEventType::noteOn, 1, 60, 100, 0 },
        { 240, TickEventType::pitchBend, 1, 0, 0, 4096 },
        { 480, TickEventType::noteOff, 1, 60, 0, 0 },
        { 480, TickEventType::noteOn, 1, 62, 90, 0 },
        { 1440, TickEventType::noteOff, 1, 62, 0, 0 },
        { 1440, TickEventType::controlChange, 1, 1, 64, 0 },
        { 1920, TickEventType::pitchBend, 1, 0, 0, 0 },
    };
    t.endTick = 2160;

    SmfOptions options;
    options.trackName = "VocalSynth Take 3 Clean";

    // Computed independently (meta first on equal ticks, every event with its own status byte)
    const std::vector<uint8_t> expected {
        0x4D, 0x54, 0x68, 0x64, 0x00, 0x00, 0x00, 0x06, 0x00, 0x01, 0x00, 0x01, 0x01, 0xE0, 0x4D, 0x54, 0x72, 0x6B,
        0x00, 0x00, 0x00, 0x76, 0x00, 0xFF, 0x03, 0x17, 0x56, 0x6F, 0x63, 0x61, 0x6C, 0x53, 0x79, 0x6E, 0x74, 0x68,
        0x20, 0x54, 0x61, 0x6B, 0x65, 0x20, 0x33, 0x20, 0x43, 0x6C, 0x65, 0x61, 0x6E, 0x00, 0xFF, 0x51, 0x03, 0x09,
        0x63, 0xD9, 0x00, 0xFF, 0x58, 0x04, 0x03, 0x02, 0x18, 0x08, 0x00, 0xB0, 0x65, 0x00, 0x00, 0xB0, 0x64, 0x00,
        0x00, 0xB0, 0x06, 0x02, 0x00, 0xB0, 0x26, 0x00, 0x00, 0xB0, 0x65, 0x7F, 0x00, 0xB0, 0x64, 0x7F, 0x00, 0x90,
        0x3C, 0x64, 0x81, 0x70, 0xE0, 0x00, 0x60, 0x81, 0x70, 0x80, 0x3C, 0x00, 0x00, 0x90, 0x3E, 0x5A, 0x87, 0x40,
        0xFF, 0x58, 0x04, 0x07, 0x03, 0x18, 0x08, 0x00, 0x80, 0x3E, 0x00, 0x00, 0xB0, 0x01, 0x40, 0x83, 0x60, 0xFF,
        0x51, 0x03, 0x06, 0x8A, 0x1B, 0x00, 0xE0, 0x00, 0x40, 0x81, 0x70, 0xFF, 0x2F, 0x00
    };

    const auto bytes = SmfWriter::write (t, options);
    CHECK (bytes == expected);
    CHECK (SmfWriter::write (t, options) == bytes); // deterministic

    const auto smf = parseSmf (bytes);
    CHECK (smf.format == 1);
    CHECK (smf.numTracks == 1);
    CHECK (smf.division == 480);
    CHECK (smf.endOfTrackTick == 2160);
}

TEST_CASE ("SmfWriter: time-signature denominators as log2, nearest power of two", "[smfwriter]")
{
    const auto denominatorByte = [] (int numerator, int denominator)
    {
        auto t = emptyTake();
        t.timeSignatures = { { 0, numerator, denominator } };
        const auto meta = metaBytes (parseSmf (SmfWriter::write (t, {})), 0x58);
        REQUIRE (meta.size() == 7);
        CHECK (meta[2] == 0x04);
        CHECK (meta[5] == 0x18);
        CHECK (meta[6] == 0x08);
        return std::pair<int, int> { meta[3], meta[4] };
    };

    CHECK (denominatorByte (4, 4) == std::pair { 4, 2 });
    CHECK (denominatorByte (3, 4) == std::pair { 3, 2 });
    CHECK (denominatorByte (7, 8) == std::pair { 7, 3 });
    CHECK (denominatorByte (6, 8) == std::pair { 6, 3 });
    CHECK (denominatorByte (5, 2) == std::pair { 5, 1 });
    CHECK (denominatorByte (2, 1) == std::pair { 2, 0 });
    CHECK (denominatorByte (5, 16) == std::pair { 5, 4 });
    CHECK (denominatorByte (4, 3) == std::pair { 4, 1 });   // not a power of two: nearest (tie -> smaller)
    CHECK (denominatorByte (4, 7) == std::pair { 4, 3 });   // 7 -> 8
    CHECK (denominatorByte (4, 12) == std::pair { 4, 3 });  // 12 -> 8 (tie)
    CHECK (denominatorByte (4, 200) == std::pair { 4, 7 }); // clamped to 128
    CHECK (denominatorByte (0, 4) == std::pair { 1, 2 });   // numerator clamped to 1..255
    CHECK (denominatorByte (300, 4) == std::pair { 255, 2 });
}

TEST_CASE ("SmfWriter: tempo microseconds rounded and clamped", "[smfwriter]")
{
    const auto tempoValue = [] (double bpm)
    {
        auto t = emptyTake();
        t.tempos = { { 0, bpm } };
        const auto meta = metaBytes (parseSmf (SmfWriter::write (t, {})), 0x51);
        REQUIRE (meta.size() == 6);
        return (static_cast<uint32_t> (meta[3]) << 16) | (static_cast<uint32_t> (meta[4]) << 8) | meta[5];
    };

    CHECK (tempoValue (120.0) == 500000);
    CHECK (tempoValue (174.0) == 344828); // 344827.59
    CHECK (tempoValue (60.0) == 1000000);
    CHECK (tempoValue (97.5) == 615385);
    CHECK (tempoValue (0.0) == 500000);   // invalid -> 120
    CHECK (tempoValue (1.0) == 0xFFFFFF); // 60e6 does not fit 24 bits
    CHECK (tempoValue (1.0e9) == 1);

    auto t = emptyTake();
    t.tempos.clear(); // no tempo map: 120
    CHECK (metaBytes (parseSmf (SmfWriter::write (t, {})), 0x51) == std::vector<uint8_t> { 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20 });
}

TEST_CASE ("SmfWriter: track name ASCII only, RPN per used channel in ascending order", "[smfwriter]")
{
    auto t = emptyTake();
    t.bendRangeSemitones = 12;
    t.events = { { 0, TickEventType::noteOn, 3, 60, 100, 0 }, { 10, TickEventType::noteOff, 3, 60, 0, 0 },
                 { 20, TickEventType::noteOn, 1, 61, 100, 0 }, { 30, TickEventType::noteOff, 1, 61, 0, 0 } };

    SmfOptions options;
    options.trackName = std::string ("Take \xC3\xA4\x01!");
    const auto smf = parseSmf (SmfWriter::write (t, options));

    REQUIRE (smf.events.size() >= 3);
    CHECK (smf.events[0].bytes == std::vector<uint8_t> { 0xFF, 0x03, 0x09, 'T', 'a', 'k', 'e', ' ', '?', '?', '?', '!' });
    CHECK (smf.events[1].bytes[1] == 0x51);
    CHECK (smf.events[2].bytes[1] == 0x58);

    // RPN 0 = 12 semitones on channel 1 then channel 3, all at tick 0
    const std::vector<std::vector<uint8_t>> rpn {
        { 0xB0, 0x65, 0x00 }, { 0xB0, 0x64, 0x00 }, { 0xB0, 0x06, 0x0C }, { 0xB0, 0x26, 0x00 }, { 0xB0, 0x65, 0x7F }, { 0xB0, 0x64, 0x7F },
        { 0xB2, 0x65, 0x00 }, { 0xB2, 0x64, 0x00 }, { 0xB2, 0x06, 0x0C }, { 0xB2, 0x26, 0x00 }, { 0xB2, 0x65, 0x7F }, { 0xB2, 0x64, 0x7F },
    };
    REQUIRE (smf.events.size() >= 3 + rpn.size());

    for (std::size_t i = 0; i < rpn.size(); ++i)
    {
        CHECK (smf.events[3 + i].tick == 0);
        CHECK (smf.events[3 + i].bytes == rpn[i]);
    }

    // No bends exported: no RPN
    t.bendRangeSemitones = 0;
    const auto plain = parseSmf (SmfWriter::write (t, {}));

    for (const auto& e : plain.events)
        CHECK (e.kind() != 0xB0);
}

TEST_CASE ("SmfWriter: channel events by tick, note-off as 8n kk 00, end-of-track never before the last event", "[smfwriter]")
{
    auto t = emptyTake (96);
    t.events = {
        { 0, TickEventType::noteOn, 16, 127, 0, 0 },   // velocity 0 would be a note-off: written as 1
        { 5, TickEventType::pitchBend, 16, 0, 0, -8192 },
        { 6, TickEventType::pitchBend, 16, 0, 0, 8191 },
        { 200000, TickEventType::noteOff, 16, 127, 0, 0 },
    };
    t.endTick = 100; // before the last event: moved to it

    const auto smf = parseSmf (SmfWriter::write (t, {}));
    std::vector<ParsedEvent> channel;

    for (const auto& e : smf.events)
        if (! e.isMeta())
            channel.push_back (e);

    REQUIRE (channel.size() == 4);
    CHECK (channel[0].bytes == std::vector<uint8_t> { 0x9F, 0x7F, 0x01 });
    CHECK (channel[1].bytes == std::vector<uint8_t> { 0xEF, 0x00, 0x00 });
    CHECK (channel[2].bytes == std::vector<uint8_t> { 0xEF, 0x7F, 0x7F });
    CHECK (channel[3].bytes == std::vector<uint8_t> { 0x8F, 0x7F, 0x00 });
    CHECK (channel[3].tick == 200000);
    CHECK (smf.endOfTrackTick == 200000);
    CHECK (smf.division == 96);
}

TEST_CASE ("SmfWriter: meta changes before channel events on the same tick", "[smfwriter]")
{
    auto t = emptyTake();
    t.tempos = { { 0, 120.0 }, { 3840, 90.0 } };
    t.timeSignatures = { { 0, 4, 4 }, { 3840, 3, 4 } };
    t.events = { { 3840, TickEventType::noteOn, 1, 60, 100, 0 }, { 4800, TickEventType::noteOff, 1, 60, 0, 0 } };
    t.endTick = 6720;

    const auto smf = parseSmf (SmfWriter::write (t, {}));
    REQUIRE (smf.events.size() == 7);
    CHECK (smf.events[2].tick == 3840);
    CHECK (smf.events[2].bytes[1] == 0x51);
    CHECK (smf.events[3].tick == 3840);
    CHECK (smf.events[3].bytes[1] == 0x58);
    CHECK (smf.events[4].bytes == std::vector<uint8_t> { 0x90, 0x3C, 0x64 });
    CHECK (smf.events[6].tick == 6720);
}
