// CaptureRecord wire format and EventTiming buffers (docs/M3-DESIGN.md 4.2). Owner from the implementation
// phase on: agent A (the packing is binding; change it only together with the design).

#include "vocalsynth/CaptureRecord.h"
#include "vocalsynth/EventTiming.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cmath>
#include <limits>

using namespace vocalsynth;

TEST_CASE ("CaptureRecord: transport round-trips (bar start within float precision)", "[capturerecord]")
{
    TransportInfo info;
    info.isPlaying = GENERATE (false, true);
    info.hasPpq = true;
    info.ppqPosition = GENERATE (0.0, 3.999999, 1234.5678901234, -2.5);
    info.hasBpm = true;
    info.bpm = GENERATE (60.0, 97.5, 174.0001);
    info.hasTimeSignature = true;
    info.timeSigNumerator = 7;
    info.timeSigDenominator = 8;
    info.hasBarStart = true;
    info.ppqPositionOfLastBarStart = std::floor (info.ppqPosition / 3.5) * 3.5;
    info.isLooping = true;

    const auto r = CaptureRecord::makeTransport (123456789012LL, info);
    CHECK (r.type == CaptureRecordType::transport);
    CHECK (r.sample == 123456789012LL);

    const auto d = r.asTransport();
    CHECK (d.isPlaying == info.isPlaying);
    CHECK (d.hasPpq);
    CHECK (d.ppqPosition == info.ppqPosition);
    CHECK (d.bpm == info.bpm);
    CHECK (d.hasTimeSignature);
    CHECK (d.timeSigNumerator == 7);
    CHECK (d.timeSigDenominator == 8);
    CHECK (d.hasBarStart);
    CHECK (std::abs (d.ppqPositionOfLastBarStart - info.ppqPositionOfLastBarStart) < 1.0e-5);
    CHECK (d.isLooping);

    // The quantised value is a fixed point: comparing quantised values never sees a spurious change
    CHECK (quantiseTransport (d) == d);
    CHECK (quantiseTransport (info) == d);
}

TEST_CASE ("CaptureRecord: default transport (no playhead) round-trips exactly", "[capturerecord]")
{
    const TransportInfo none;
    CHECK (CaptureRecord::makeTransport (0, none).asTransport() == none);
}

TEST_CASE ("CaptureRecord: event with timing round-trips exactly", "[capturerecord]")
{
    const auto event = GENERATE (VsEvent::makeNoteOn (17, 3, 60, 100), VsEvent::makeNoteOff (0, 16, 127),
                                 VsEvent::makePitchBend (5, 1, -8192), VsEvent::makePitchBend (5, 1, 8191),
                                 VsEvent::makeControlChange (2, 9, 74, 127));
    const VsEventTiming timing { GENERATE (0, 1, 1103, -5, std::numeric_limits<int32_t>::max()),
                                 EventTimingKind::measured,
                                 static_cast<uint8_t> (EventTimingFlags::correction | EventTimingFlags::noteChange) };

    const auto r = CaptureRecord::makeEvent (48000, event, timing);
    CHECK (r.type == CaptureRecordType::event);

    const auto d = r.asEvent();
    auto expected = event;
    expected.sampleOffset = 0;
    CHECK (d.event == expected);
    CHECK (d.timing == timing);
    CHECK (r.estimatedSample() == 48000 - static_cast<int64_t> (timing.delaySamples));
}

TEST_CASE ("CaptureRecord: other record types round-trip", "[capturerecord]")
{
    const auto reset = CaptureRecord::makeReset (0, { 96000.0, 4096, true }).asReset();
    CHECK (reset.sampleRate == 96000.0);
    CHECK (reset.maxBlockSize == 4096);
    CHECK (reset.clearsHistory);

    CaptureSettingsInfo settings;
    settings.bendMode = BendMode::trueBend;
    settings.bendRangeSemitones = 48;
    settings.channel = 16;
    settings.minNoteLengthMs = 123.5f;
    settings.testPattern = true;
    CHECK (CaptureRecord::makeSettings (7, settings).asSettings() == settings);

    CHECK (CaptureRecord::makeAudio (256, 256).asAudioNumSamples() == 256);

    const MarkerRecord marker { CaptureMarkerKind::grab, 16, 120.0 };
    const auto m = CaptureRecord::makeMarker (99, marker).asMarker();
    CHECK (m.kind == CaptureMarkerKind::grab);
    CHECK (m.grabBars == 16);
    CHECK (m.grabSeconds == 120.0);

    const auto gap = CaptureRecord::makeGap (5, { 3, 1024 }).asGap();
    CHECK (gap.droppedRecords == 3);
    CHECK (gap.droppedAudioSamples == 1024);

    // Accessors of the wrong type give defaults
    CHECK (CaptureRecord::makeAudio (0, 256).asEvent().timing == VsEventTiming {});
    CHECK (CaptureRecord::makeGap (0, {}).asAudioNumSamples() == 0);
}

TEST_CASE ("VsEventTimingBuffer / VsEventTimingSink", "[capturerecord]")
{
    VsEventTimingBuffer<2> buffer;
    VsEventTimingSink sink { buffer };
    CHECK (sink.isConnected());
    CHECK (sink.push ({ 1, EventTimingKind::model, 0 }));
    CHECK (sink.push ({ 2, EventTimingKind::measured, 0 }));
    CHECK_FALSE (sink.push ({ 3, EventTimingKind::none, 0 })); // full
    REQUIRE (buffer.size() == 2);
    CHECK (buffer[1].delaySamples == 2);

    const VsEventTimingSink none;
    CHECK_FALSE (none.isConnected());
    CHECK_FALSE (none.push ({}));
}
