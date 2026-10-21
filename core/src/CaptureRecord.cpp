#include "vocalsynth/CaptureRecord.h"

#include <algorithm>
#include <bit>

namespace vocalsynth
{

// Packing (binding, docs/M3-DESIGN.md 4.2):
//   reset:     word0 = sampleRate (double bits), word2 = maxBlockSize, byte0 = clearsHistory
//   transport: word0 = ppq (double), word1 = bpm (double), word2 = ppq - lastBarStart (float bits),
//              byte0 = flags, byte1 = numerator, byte2 = denominator
//   settings:  word0 = testPattern, word1 = captureMode | takeMaxBars << 8 (M3.1, bytes 0 and 1),
//              word2 = minNoteLengthMs (float bits), byte0 = bendMode, byte1 = bendRangeSemitones, byte2 = channel
//   event:     word0 = type | channel << 8 | data1 << 16 | data2 << 24 | uint16 (bend) << 32,
//              word2 = delaySamples (int32 bits), byte0 = timing kind, byte1 = timing flags
//   audio:     word2 = numSamples
//   marker:    word0 = grabBars (int64 bits), word1 = grabSeconds (double), byte0 = kind
//   gap:       word0 = droppedRecords, word1 = droppedAudioSamples

namespace
{
namespace TransportFlags
{
constexpr uint8_t playing = 1u << 0;
constexpr uint8_t hasPpq = 1u << 1;
constexpr uint8_t hasBpm = 1u << 2;
constexpr uint8_t hasTimeSignature = 1u << 3;
constexpr uint8_t hasBarStart = 1u << 4;
constexpr uint8_t looping = 1u << 5;
} // namespace TransportFlags

constexpr uint8_t toByte (int value, int lo, int hi) noexcept
{
    return static_cast<uint8_t> (std::clamp (value, lo, hi));
}

CaptureRecord makeRecord (CaptureRecordType type, int64_t sample) noexcept
{
    CaptureRecord r;
    r.type = type;
    r.sample = sample;
    return r;
}
} // namespace

CaptureRecord CaptureRecord::makeReset (int64_t sample, const ResetRecord& reset) noexcept
{
    auto r = makeRecord (CaptureRecordType::reset, sample);
    r.word0 = std::bit_cast<uint64_t> (reset.sampleRate);
    r.word2 = static_cast<uint32_t> (std::max (0, reset.maxBlockSize));
    r.byte0 = reset.clearsHistory ? 1 : 0;
    return r;
}

CaptureRecord CaptureRecord::makeTransport (int64_t sample, const TransportInfo& info) noexcept
{
    auto r = makeRecord (CaptureRecordType::transport, sample);
    r.word0 = std::bit_cast<uint64_t> (info.ppqPosition);
    r.word1 = std::bit_cast<uint64_t> (info.bpm);
    r.word2 = std::bit_cast<uint32_t> (static_cast<float> (info.ppqPosition - info.ppqPositionOfLastBarStart));

    uint8_t flags = 0;
    flags |= info.isPlaying ? TransportFlags::playing : uint8_t { 0 };
    flags |= info.hasPpq ? TransportFlags::hasPpq : uint8_t { 0 };
    flags |= info.hasBpm ? TransportFlags::hasBpm : uint8_t { 0 };
    flags |= info.hasTimeSignature ? TransportFlags::hasTimeSignature : uint8_t { 0 };
    flags |= info.hasBarStart ? TransportFlags::hasBarStart : uint8_t { 0 };
    flags |= info.isLooping ? TransportFlags::looping : uint8_t { 0 };
    r.byte0 = flags;
    r.byte1 = toByte (info.timeSigNumerator, 1, 255);
    r.byte2 = toByte (info.timeSigDenominator, 1, 255);
    return r;
}

CaptureRecord CaptureRecord::makeSettings (int64_t sample, const CaptureSettingsInfo& info) noexcept
{
    auto r = makeRecord (CaptureRecordType::settings, sample);
    r.word0 = info.testPattern ? 1u : 0u;
    r.word1 = static_cast<uint64_t> (toByte (static_cast<int> (info.captureMode), 0, numCaptureModes - 1))
              | (static_cast<uint64_t> (toByte (info.takeMaxBars, 0, 255)) << 8);
    r.word2 = std::bit_cast<uint32_t> (info.minNoteLengthMs);
    r.byte0 = toByte (static_cast<int> (info.bendMode), 0, numBendModes - 1);
    r.byte1 = toByte (info.bendRangeSemitones, 0, 255);
    r.byte2 = toByte (info.channel, 1, 16);
    return r;
}

CaptureRecord CaptureRecord::makeEvent (int64_t sample, const VsEvent& event, const VsEventTiming& timing) noexcept
{
    auto r = makeRecord (CaptureRecordType::event, sample);
    r.word0 = static_cast<uint64_t> (static_cast<uint8_t> (event.type))
              | (static_cast<uint64_t> (toByte (event.channel, 1, 16)) << 8)
              | (static_cast<uint64_t> (event.data1) << 16) | (static_cast<uint64_t> (event.data2) << 24)
              | (static_cast<uint64_t> (static_cast<uint16_t> (event.bend)) << 32);
    r.word2 = std::bit_cast<uint32_t> (timing.delaySamples);
    r.byte0 = static_cast<uint8_t> (timing.kind);
    r.byte1 = timing.flags;
    return r;
}

CaptureRecord CaptureRecord::makeAudio (int64_t sample, int numSamples) noexcept
{
    auto r = makeRecord (CaptureRecordType::audio, sample);
    r.word2 = static_cast<uint32_t> (std::max (0, numSamples));
    return r;
}

CaptureRecord CaptureRecord::makeMarker (int64_t sample, const MarkerRecord& marker) noexcept
{
    auto r = makeRecord (CaptureRecordType::marker, sample);
    r.word0 = std::bit_cast<uint64_t> (static_cast<int64_t> (marker.grabBars));
    r.word1 = std::bit_cast<uint64_t> (marker.grabSeconds);
    r.byte0 = static_cast<uint8_t> (marker.kind);
    return r;
}

CaptureRecord CaptureRecord::makeGap (int64_t sample, const GapRecord& gap) noexcept
{
    auto r = makeRecord (CaptureRecordType::gap, sample);
    r.word0 = gap.droppedRecords;
    r.word1 = gap.droppedAudioSamples;
    return r;
}

ResetRecord CaptureRecord::asReset() const noexcept
{
    if (type != CaptureRecordType::reset)
        return {};

    return { std::bit_cast<double> (word0), static_cast<int> (word2), byte0 != 0 };
}

TransportInfo CaptureRecord::asTransport() const noexcept
{
    if (type != CaptureRecordType::transport)
        return {};

    TransportInfo info;
    info.ppqPosition = std::bit_cast<double> (word0);
    info.bpm = std::bit_cast<double> (word1);
    info.ppqPositionOfLastBarStart = info.ppqPosition - static_cast<double> (std::bit_cast<float> (word2));
    info.isPlaying = (byte0 & TransportFlags::playing) != 0;
    info.hasPpq = (byte0 & TransportFlags::hasPpq) != 0;
    info.hasBpm = (byte0 & TransportFlags::hasBpm) != 0;
    info.hasTimeSignature = (byte0 & TransportFlags::hasTimeSignature) != 0;
    info.hasBarStart = (byte0 & TransportFlags::hasBarStart) != 0;
    info.isLooping = (byte0 & TransportFlags::looping) != 0;
    info.timeSigNumerator = byte1;
    info.timeSigDenominator = byte2;
    return info;
}

CaptureSettingsInfo CaptureRecord::asSettings() const noexcept
{
    if (type != CaptureRecordType::settings)
        return {};

    CaptureSettingsInfo info;
    info.testPattern = word0 != 0;
    info.captureMode = static_cast<CaptureMode> (std::min<uint64_t> (word1 & 0xffu, numCaptureModes - 1));
    info.takeMaxBars = static_cast<int> ((word1 >> 8) & 0xffu);
    info.minNoteLengthMs = std::bit_cast<float> (word2);
    info.bendMode = static_cast<BendMode> (byte0);
    info.bendRangeSemitones = byte1;
    info.channel = byte2;
    return info;
}

EventRecord CaptureRecord::asEvent() const noexcept
{
    if (type != CaptureRecordType::event)
        return {};

    EventRecord e;
    e.event.type = static_cast<VsEventType> (word0 & 0xffu);
    e.event.sampleOffset = 0;
    e.event.channel = static_cast<uint8_t> ((word0 >> 8) & 0xffu);
    e.event.data1 = static_cast<uint8_t> ((word0 >> 16) & 0xffu);
    e.event.data2 = static_cast<uint8_t> ((word0 >> 24) & 0xffu);
    e.event.bend = static_cast<int16_t> (static_cast<uint16_t> ((word0 >> 32) & 0xffffu));
    e.timing.delaySamples = std::bit_cast<int32_t> (word2);
    e.timing.kind = static_cast<EventTimingKind> (byte0);
    e.timing.flags = byte1;
    return e;
}

int CaptureRecord::asAudioNumSamples() const noexcept
{
    return type == CaptureRecordType::audio ? static_cast<int> (word2) : 0;
}

MarkerRecord CaptureRecord::asMarker() const noexcept
{
    if (type != CaptureRecordType::marker)
        return {};

    MarkerRecord m;
    m.kind = static_cast<CaptureMarkerKind> (byte0);
    m.grabBars = static_cast<int> (std::bit_cast<int64_t> (word0));
    m.grabSeconds = std::bit_cast<double> (word1);
    return m;
}

GapRecord CaptureRecord::asGap() const noexcept
{
    if (type != CaptureRecordType::gap)
        return {};

    return { word0, word1 };
}

int64_t CaptureRecord::estimatedSample() const noexcept
{
    if (type != CaptureRecordType::event)
        return sample;

    return sample - static_cast<int64_t> (std::bit_cast<int32_t> (word2));
}

TransportInfo quantiseTransport (const TransportInfo& info) noexcept
{
    return CaptureRecord::makeTransport (0, info).asTransport();
}

} // namespace vocalsynth
