#pragma once

#include "vocalsynth/TakeRenderer.h"

#include <cstdint>
#include <string>
#include <vector>

namespace vocalsynth
{

/** Standard MIDI File format of the export. */
enum class SmfFormat : uint8_t
{
    type0 = 0, ///< one track with everything
    type1      ///< one track as well (as M1: FL imports it into one pattern); header says format 1
};

struct SmfOptions
{
    SmfFormat format = SmfFormat::type1;
    std::string trackName;  ///< ASCII (non-ASCII bytes are replaced by '?'); empty: no track-name event
};

/** RenderedTake -> Standard MIDI File bytes (docs/M3-DESIGN.md 10). Pure, JUCE-free, deterministic
    (byte-identical output for identical input), no I/O: the plugin writes the bytes to a file.

    Layout (binding): "MThd" (length 6, format, 1 track, division = ticksPerQuarterNote), one "MTrk" with,
    at tick 0 in this order: track name (FF 03) if set, time signature (FF 58 nn dd 24 08), tempo (FF 51,
    llround (60e6 / bpm) us per quarter, clamped to 1..0xFFFFFF), RPN 0 bend range when
    bendRangeSemitones > 0 on every channel that has events (B0 65 00, B0 64 00, B0 06 range, B0 26 00,
    B0 65 7F, B0 64 7F); then the remaining tempo / time-signature changes and the channel events merged by
    tick (meta before channel events on equal ticks; channel events keep their RenderedTake order), note-off
    as 8n kk 00, no running status; end-of-track (FF 2F 00) at endTick. Delta times as variable-length
    quantities. */
class SmfWriter
{
public:
    static std::vector<uint8_t> write (const RenderedTake& take, const SmfOptions& options);

    /** Variable-length quantity of value (0 .. 0x0FFFFFFF; larger values are clamped). */
    static void appendVariableLength (std::vector<uint8_t>& out, uint32_t value);
};

} // namespace vocalsynth
