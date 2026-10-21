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

    Layout (binding; bytes in hex): "MThd" 00 00 00 06, format (00 00 or 00 01), ntrks 00 01, division =
    ticksPerQuarterNote (16 bit, < 0x8000). One "MTrk" (32-bit big-endian length) with, at tick 0 in this
    order (the M1 order that FL accepted):
    - track name FF 03 len text (if set; len as a variable-length quantity);
    - tempo FF 51 03 tt tt tt, tt = llround (60e6 / bpm) us per quarter, clamped to 1..0xFFFFFF;
    - time signature FF 58 04 nn dd 18 08 (dd = log2 of the denominator; a denominator that is not a power
      of two is written as the nearest one, clamped to 1..2^7; nn clamped to 1..255);
    - RPN 0 bend range when bendRangeSemitones > 0, on every channel that has events, ascending:
      Bn 65 00, Bn 64 00, Bn 06 range, Bn 26 00, Bn 65 7F, Bn 64 7F;
    then the remaining tempo / time-signature changes and the channel events merged by tick (meta events
    before channel events on equal ticks; channel events keep their RenderedTake order); note-on 9n kk vv,
    note-off 8n kk 00, CC Bn cc vv, pitch bend En ll mm (value + 8192, 14 bit); every event with its own
    status byte (no running status); end-of-track FF 2F 00 at endTick (never before the last event).
    Delta times as variable-length quantities. FL ignores the tempo meta when a .mid is dropped into an
    existing project (notes are placed by ticks), so it is informational; the timing is in the ticks. */
class SmfWriter
{
public:
    static std::vector<uint8_t> write (const RenderedTake& take, const SmfOptions& options);

    /** Variable-length quantity of value (0 .. 0x0FFFFFFF; larger values are clamped). */
    static void appendVariableLength (std::vector<uint8_t>& out, uint32_t value);
};

} // namespace vocalsynth
