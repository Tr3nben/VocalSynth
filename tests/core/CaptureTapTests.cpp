// CaptureTap (docs/M3-DESIGN.md 4, 16). Owner: agent A.

#include "CaptureTestHelpers.h"

#include "vocalsynth/CaptureTap.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <new>
#include <random>
#include <span>
#include <vector>

using namespace vocalsynth;
using namespace vocalsynth::test;

//==============================================================================
// Counting global operator new (this test binary only): armed per thread around real-time calls.
namespace
{
thread_local bool countingAllocations = false;
thread_local std::size_t allocationCount = 0;

struct CountAllocations
{
    CountAllocations() noexcept
    {
        allocationCount = 0;
        countingAllocations = true;
    }
    ~CountAllocations() { countingAllocations = false; }
    CountAllocations (const CountAllocations&) = delete;
    CountAllocations& operator= (const CountAllocations&) = delete;
};
} // namespace

void* operator new (std::size_t size)
{
    if (countingAllocations)
        ++allocationCount;

    if (void* p = std::malloc (size == 0 ? 1 : size))
        return p;

    throw std::bad_alloc();
}

void operator delete (void* p) noexcept { std::free (p); }
void operator delete (void* p, std::size_t) noexcept { std::free (p); }

//==============================================================================
namespace
{
TransportInfo playing (double ppq, double bpm = 120.0, int num = 4, int den = 4)
{
    TransportInfo t;
    t.isPlaying = true;
    t.hasPpq = true;
    t.ppqPosition = ppq;
    t.hasBpm = true;
    t.bpm = bpm;
    t.hasTimeSignature = true;
    t.timeSigNumerator = num;
    t.timeSigDenominator = den;
    t.hasBarStart = true;
    const auto bar = num * 4.0 / den;
    t.ppqPositionOfLastBarStart = std::floor ((ppq + 1.0e-9) / bar) * bar;
    return t;
}

TransportInfo stopped (double ppq, double bpm = 120.0)
{
    auto t = playing (ppq, bpm);
    t.isPlaying = false;
    return t;
}

/** A tap plus a consumer on the test's thread. */
struct Harness
{
    explicit Harness (double sr = 48000.0, int maxBlock = 512)
    {
        tap.prepare (sr, maxBlock);
        bs.captureAudio = false;
    }

    void block (int n, std::span<const VsEvent> events = {}, std::span<const VsEventTiming> timings = {},
                const float* mono = nullptr)
    {
        tap.beginBlock (transport, bs);
        tap.processChunk (mono, n, events, timings);
    }

    TapOutput drain()
    {
        TapOutput out;
        drainTap (tap, out);
        return out;
    }

    /** Every record in ring order, audio records included. */
    std::vector<CaptureRecord> drainAll()
    {
        std::vector<CaptureRecord> all;
        CaptureRecord r;
        std::vector<float> frame (CaptureTap::audioFrameSamples);

        while (tap.getRecordRing().pop (r))
        {
            if (r.type == CaptureRecordType::audio)
                tap.getAudioRing().popBulk (std::span<float> { frame.data(), static_cast<std::size_t> (r.asAudioNumSamples()) });

            all.push_back (r);
        }

        return all;
    }

    CaptureTap tap;
    CaptureBlockSettings bs;
    TransportInfo transport {};
};

std::vector<CaptureRecordType> typesOf (const std::vector<CaptureRecord>& records)
{
    std::vector<CaptureRecordType> types;
    for (const auto& r : records)
        types.push_back (r.type);
    return types;
}

std::vector<EventRecord> eventsOf (const TapOutput& out)
{
    std::vector<EventRecord> result;
    for (const auto& r : out.ofType (CaptureRecordType::event))
        result.push_back (r.asEvent());
    return result;
}

int countTransport (const TapOutput& out)
{
    return static_cast<int> (out.ofType (CaptureRecordType::transport).size());
}
} // namespace

//==============================================================================
TEST_CASE ("CaptureTap: first block pushes reset, transport, settings, then the on markers", "[capturetap]")
{
    Harness h;
    h.bs.captureAudio = true;
    h.bs.settings.bendRangeSemitones = 12;
    h.transport = playing (4.0);
    h.block (64);

    const auto out = h.drain();
    REQUIRE (out.records.size() == 5);
    CHECK (typesOf (out.records)
           == std::vector<CaptureRecordType> { CaptureRecordType::reset, CaptureRecordType::transport,
                                               CaptureRecordType::settings, CaptureRecordType::marker,
                                               CaptureRecordType::marker });

    for (const auto& r : out.records)
        CHECK (r.sample == 0);

    const auto reset = out.records[0].asReset();
    CHECK (reset.sampleRate == 48000.0);
    CHECK (reset.maxBlockSize == 512);
    CHECK (reset.clearsHistory);
    CHECK (out.records[1].asTransport() == quantiseTransport (playing (4.0)));
    CHECK (out.records[2].asSettings().bendRangeSemitones == 12);
    CHECK (out.records[3].asMarker().kind == CaptureMarkerKind::captureOn);
    CHECK (out.records[4].asMarker().kind == CaptureMarkerKind::audioOn);
    CHECK (h.tap.getClock() == 64);
    CHECK (out.frames.empty()); // 64 of 256 samples staged

    // A 0-sample chunk does nothing
    h.tap.processChunk (nullptr, 0, {}, {});
    CHECK (h.tap.getClock() == 64);
}

TEST_CASE ("CaptureTap: events are normalised exactly like MidiOutputStage sends them", "[capturetap]")
{
    Harness h;
    h.block (128); // start-up records
    h.drain();

    const VsEventTiming measured { 480, EventTimingKind::measured, EventTimingFlags::correction };
    const VsEventTiming withResync { 96, EventTimingKind::measured, EventTimingFlags::resync | EventTimingFlags::noteChange };

    std::vector<VsEvent> events {
        VsEvent::makeNoteOn (-5, 0, 60, 100),      // channel 0 -> 1, offset -5 -> 0
        VsEvent::makeNoteOn (3, 1, 60, 0),         // velocity 0 = note-off of an open note
        VsEvent::makeNoteOff (4, 1, 61),           // not open: dropped
        VsEvent::makeNoteOn (100, 20, 200, 200),   // channel 16, note 127, velocity 127, offset -> 63
        VsEvent::makeNoteOn (63, 16, 127, 90),     // duplicate: note-off first, then the note-on
        VsEvent::makePitchBend (10, 2, 9000),      // in range for int16 but above 8191
        VsEvent::makePitchBend (11, 2, -9000),
        VsEvent::makeControlChange (12, 3, 200, 255),
        VsEvent::makeNoteOn (13, 1, 64, 0),        // velocity 0 of a closed note: dropped
    };
    // Timings shorter than the events: the rest is kind none; the resync flag is the tap's own
    std::vector<VsEventTiming> timings { measured, withResync };

    h.block (64, events, timings);
    const auto out = h.drain();
    const auto got = eventsOf (out);
    const auto recs = out.ofType (CaptureRecordType::event);

    REQUIRE (got.size() == 8);
    // 1. note-on ch 1 note 60 at the chunk start, with its timing
    CHECK (recs[0].sample == 128);
    CHECK (got[0].event == VsEvent::makeNoteOn (0, 1, 60, 100));
    CHECK (got[0].timing == measured);
    // 2. velocity-0 note-on -> note-off, resync flag stripped
    CHECK (recs[1].sample == 131);
    CHECK (got[1].event == VsEvent::makeNoteOff (0, 1, 60));
    CHECK (got[1].timing.delaySamples == 96);
    CHECK (got[1].timing.flags == EventTimingFlags::noteChange);
    // 3. clamped note-on at the last sample of the chunk
    CHECK (recs[2].sample == 128 + 63);
    CHECK (got[2].event == VsEvent::makeNoteOn (0, 16, 127, 127));
    CHECK (got[2].timing == VsEventTiming {});
    // 4./5. duplicate note-on: note-off of the open note, then the new note-on (same sample)
    CHECK (recs[3].sample == 128 + 63);
    CHECK (got[3].event == VsEvent::makeNoteOff (0, 16, 127));
    CHECK (recs[4].sample == 128 + 63);
    CHECK (got[4].event == VsEvent::makeNoteOn (0, 16, 127, 90));
    // 6./7. bends clamped to the 14-bit range
    CHECK (got[5].event == VsEvent::makePitchBend (0, 2, 8191));
    CHECK (got[6].event == VsEvent::makePitchBend (0, 2, -8192));
    // 8. CC clamped; the velocity-0 note-on of a closed note after it was dropped
    CHECK (got[7].event == VsEvent::makeControlChange (0, 3, 127, 127));
    CHECK (recs[7].sample == 128 + 12);
}

TEST_CASE ("CaptureTap: control changes are clamped and recorded in emission order", "[capturetap]")
{
    Harness h;
    h.block (16);
    h.drain();

    std::vector<VsEvent> events { VsEvent::makeControlChange (0, 3, 200, 255), VsEvent::makeControlChange (1, 3, 1, 64),
                                  VsEvent::makeNoteOff (2, 3, 1) };
    h.block (16, events);
    const auto got = eventsOf (h.drain());
    REQUIRE (got.size() == 2);
    CHECK (got[0].event == VsEvent::makeControlChange (0, 3, 127, 127));
    CHECK (got[1].event == VsEvent::makeControlChange (0, 3, 1, 64));

    // A note-off keeps its release velocity (as the output stage sends it)
    std::vector<VsEvent> on { VsEvent::makeNoteOn (0, 1, 70, 80) };
    auto off = VsEvent::makeNoteOff (1, 1, 70);
    off.data2 = 33;
    on.push_back (off);
    h.block (16, on);
    const auto got2 = eventsOf (h.drain());
    REQUIRE (got2.size() == 2);
    CHECK (got2[1].event.data2 == 33);
}

TEST_CASE ("CaptureTap: same-rate prepare keeps clock, open notes and the staged frame; a new rate clears",
           "[capturetap]")
{
    Harness h;
    h.bs.captureAudio = true;
    std::vector<float> mono (300, 0.25f);
    std::vector<VsEvent> on { VsEvent::makeNoteOn (10, 1, 60, 100) };
    h.block (300, on, {}, mono.data());
    h.drain();

    h.tap.prepare (48000.0, 1024); // FL restarts its engine with a new buffer size
    CHECK (h.tap.getClock() == 300);
    h.block (212, {}, {}, mono.data()); // completes the frame [256, 512) staged before the prepare
    h.tap.flushNotes(); // processor flush after prepareToPlay (here after the block: clock 512)

    const auto all = h.drainAll();
    REQUIRE (all.size() >= 4);
    CHECK (all[0].type == CaptureRecordType::reset);
    CHECK (all[0].sample == 300);
    CHECK_FALSE (all[0].asReset().clearsHistory);
    CHECK (all[0].asReset().maxBlockSize == 1024);
    CHECK (all[1].type == CaptureRecordType::transport); // forced
    CHECK (all[2].type == CaptureRecordType::settings);  // forced
    // No captureOn / audioOn: the state survives a same-rate prepare. The staged frame continued:
    CHECK (all[3].type == CaptureRecordType::audio);
    CHECK (all[3].sample == 256);
    REQUIRE (all.size() == 5);
    CHECK (all[4].type == CaptureRecordType::event);
    CHECK (all[4].asEvent().event == VsEvent::makeNoteOff (0, 1, 60)); // the open note is still known
    CHECK (all[4].sample == 512);

    // New rate: clock 0, open notes forgotten, captureOn again
    std::vector<VsEvent> on2 { VsEvent::makeNoteOn (0, 1, 62, 100) };
    h.block (10, on2);
    h.drain();
    h.tap.prepare (96000.0, 512);
    CHECK (h.tap.getClock() == 0);
    h.block (10);
    h.tap.flushNotes();
    const auto out = h.drain();
    REQUIRE_FALSE (out.records.empty());
    CHECK (out.records[0].type == CaptureRecordType::reset);
    CHECK (out.records[0].sample == 0);
    CHECK (out.records[0].asReset().clearsHistory);
    CHECK (out.records[0].asReset().sampleRate == 96000.0);
    CHECK (out.markers (CaptureMarkerKind::captureOn).size() == 1);
    CHECK (out.ofType (CaptureRecordType::event).empty());
    CHECK (h.tap.getSampleRate() == 96000.0);
    CHECK (h.tap.getMaxBlockSize() == 512);
}

//==============================================================================
TEST_CASE ("CaptureTap: transport records only on change", "[capturetap]")
{
    const double sr = 48000.0;
    const double beatsPerSample = 120.0 / (60.0 * sr);

    SECTION ("constant tempo: one record, bar lines inside blocks are no change")
    {
        const int blockSize = GENERATE (1, 7, 512, 4096);
        Harness h (sr, 4096);
        int64_t s = 0;
        int records = 0;

        while (s < static_cast<int64_t> (10.0 * sr)) // 20 beats: 5 bar lines, most inside a block
        {
            h.transport = playing (1.5 + static_cast<double> (s) * beatsPerSample);
            h.block (blockSize);
            s += blockSize;

            if (s % 65536 < blockSize)
                records += countTransport (h.drain());
        }

        records += countTransport (h.drain());
        CHECK (records == 1);
    }

    SECTION ("structural changes go at once, a bpm change waits for the re-sync interval")
    {
        Harness h (sr, 512);
        h.transport = playing (0.0);
        h.block (240);
        h.drain();

        // Tempo change 5 ms after the last record: held until 10 ms, then sent with that block's values
        h.transport = playing (240 * beatsPerSample, 140.0);
        h.block (240);
        CHECK (countTransport (h.drain()) == 0);
        h.transport = playing (240 * beatsPerSample + 240 * 140.0 / (60.0 * sr), 140.0);
        h.block (240);
        auto out = h.drain();
        REQUIRE (countTransport (out) == 1);
        CHECK (out.ofType (CaptureRecordType::transport)[0].sample == 480);
        CHECK (out.ofType (CaptureRecordType::transport)[0].asTransport().bpm == 140.0);

        // Stop 1 sample later: at once
        auto t = h.transport;
        t.isPlaying = false;
        h.transport = t;
        h.block (1);
        out = h.drain();
        REQUIRE (countTransport (out) == 1);
        CHECK (out.ofType (CaptureRecordType::transport)[0].sample == 720);

        // Start, then a loop wrap and a time-signature change within a few samples: all at once
        h.transport = playing (16.0, 140.0);
        h.block (1);
        h.transport = playing (0.0, 140.0);
        h.block (1);
        h.transport = playing (140.0 / (60.0 * sr), 140.0, 3, 4);
        h.block (1);
        CHECK (countTransport (h.drain()) == 3);

        // A small deviation right after a structural record: re-sync class, held for 10 ms
        h.transport = playing (0.1, 140.0, 3, 4);
        h.block (1);
        CHECK (countTransport (h.drain()) == 0);
    }

    SECTION ("re-sync records are bounded with 1-sample blocks: jitter, tempo ramp, no bpm")
    {
        enum class Kind { jitter, ramp, noBpm };
        const auto kind = GENERATE (Kind::jitter, Kind::ramp, Kind::noBpm);
        Harness h (sr, 512);
        std::mt19937 rng (7);
        std::uniform_real_distribution<double> noise (-1.0e-3, 1.0e-3);
        double ppq = 0.0;
        int records = 0;

        for (int s = 0; s < static_cast<int> (sr); ++s)
        {
            double bpm = 120.0;

            if (kind == Kind::ramp)
                bpm = 120.0 + 20.0 * s / sr;

            TransportInfo t = playing (ppq + (kind == Kind::jitter ? noise (rng) : 0.0), bpm);

            if (kind == Kind::noBpm)
            {
                t.hasBpm = false; // the host plays at 140 but does not say so (prediction: 120)
                bpm = 140.0;
            }

            h.transport = t;
            h.block (1);
            ppq += bpm / (60.0 * sr);

            if (s % 4096 == 0)
                records += countTransport (h.drain());
        }

        records += countTransport (h.drain());
        CHECK (records <= 101);

        if (kind != Kind::jitter)
            CHECK (records >= 90); // the deviation grows every block: one re-sync per 10 ms
    }

    SECTION ("position changes while stopped: at most every 50 ms; NaN positions are no change")
    {
        Harness h (sr, 512);
        int records = 0;

        for (int s = 0; s < static_cast<int> (sr); ++s)
        {
            h.transport = stopped (static_cast<double> (s) * 0.001); // scrubbing
            h.block (1);
            if (s % 4096 == 0)
                records += countTransport (h.drain());
        }

        records += countTransport (h.drain());
        CHECK (records <= 21);
        CHECK (records >= 19);

        TransportInfo nan = playing (std::nan (""));
        nan.bpm = std::nan ("");
        for (int i = 0; i < 1000; ++i)
        {
            h.transport = nan;
            h.block (1);
        }
        const auto out = h.drain();
        REQUIRE (countTransport (out) == 1); // the change to "no position"
        CHECK_FALSE (out.ofType (CaptureRecordType::transport)[0].asTransport().hasPpq);
        CHECK (out.ofType (CaptureRecordType::transport)[0].asTransport().isPlaying);
    }
}

TEST_CASE ("CaptureTap: settings record only on change", "[capturetap]")
{
    Harness h;
    h.block (10);
    h.block (10);
    h.bs.settings.bendMode = BendMode::trueBend;
    h.block (10);
    h.block (10);
    const auto settings = h.drain().ofType (CaptureRecordType::settings);
    REQUIRE (settings.size() == 2);
    CHECK (settings[1].sample == 20);
    CHECK (settings[1].asSettings().bendMode == BendMode::trueBend);
}

//==============================================================================
TEST_CASE ("CaptureTap: UI requests coalesce to one marker per kind per block", "[capturetap]")
{
    Harness h;
    h.block (32);
    h.drain();

    h.tap.requestNewTake();
    h.tap.requestNewTake();
    h.tap.requestNewTake();
    h.tap.requestGrab (4, 10.0);
    h.tap.requestGrab (16, 60.0);
    h.tap.requestClear();
    h.block (32);

    auto out = h.drain();
    REQUIRE (out.records.size() == 3);
    CHECK (out.records[0].asMarker().kind == CaptureMarkerKind::newTake);
    CHECK (out.records[1].asMarker().kind == CaptureMarkerKind::grab);
    CHECK (out.records[1].asMarker().grabBars == 16);
    CHECK (out.records[1].asMarker().grabSeconds == 60.0);
    CHECK (out.records[2].asMarker().kind == CaptureMarkerKind::clear);
    for (const auto& r : out.records)
        CHECK (r.sample == 32);

    h.block (32);
    CHECK (h.drain().records.empty());

    // Requests are recorded while capture is off as well (the segmenter decides)
    h.bs.captureEnabled = false;
    h.block (32);
    h.drain();
    h.tap.requestNewTake();
    h.block (32);
    CHECK (h.drain().markers (CaptureMarkerKind::newTake).size() == 1);
}

TEST_CASE ("CaptureTap: flushNotes records the output's note-offs and bend centres", "[capturetap]")
{
    Harness h;
    h.block (16);
    h.drain();

    std::vector<VsEvent> events { VsEvent::makeNoteOn (0, 1, 60, 100), VsEvent::makePitchBend (1, 1, 300),
                                  VsEvent::makePitchBend (2, 2, 0), VsEvent::makeNoteOn (3, 3, 64, 90) };
    h.block (16, events);
    h.drain();

    h.tap.beginBlock (h.transport, h.bs);
    h.tap.flushNotes();
    h.tap.processChunk (nullptr, 16, {}, {});

    const auto out = h.drain();
    const auto got = eventsOf (out);
    REQUIRE (got.size() == 3);
    CHECK (got[0].event == VsEvent::makeNoteOff (0, 1, 60));
    CHECK (got[1].event == VsEvent::makePitchBend (0, 1, 0));
    CHECK (got[2].event == VsEvent::makeNoteOff (0, 3, 64));
    for (const auto& e : got)
        CHECK (e.timing == VsEventTiming {});
    for (const auto& r : out.ofType (CaptureRecordType::event))
        CHECK (r.sample == 32);

    // Nothing open any more
    h.tap.flushNotes();
    CHECK (h.drain().records.empty());

    // After the flush a note-off of a flushed note is dropped (the output stage drops it too)
    std::vector<VsEvent> off { VsEvent::makeNoteOff (0, 1, 60) };
    h.block (16, off);
    CHECK (h.drain().ofType (CaptureRecordType::event).empty());
}

//==============================================================================
TEST_CASE ("CaptureTap: audio frames are aligned to the clock; bypass captures silence", "[capturetap]")
{
    Harness h;
    h.bs.captureAudio = true;
    std::vector<float> mono (1000);
    int64_t s = 0;

    const auto run = [&] (int n)
    {
        for (int i = 0; i < n; ++i)
            mono[static_cast<std::size_t> (i)] = scenarioSignal (s + i);

        if (h.bs.bypassed)
        {
            h.tap.beginBlock (h.transport, h.bs);
            h.tap.flushNotes();
            h.tap.processBypassed (n);
        }
        else
        {
            h.block (n, {}, {}, mono.data());
        }

        s += n;
    };

    run (100);
    run (300);
    h.bs.bypassed = true;
    run (512);
    run (100);
    h.bs.bypassed = false;
    run (1000);

    const auto out = h.drain();
    CHECK (out.markers (CaptureMarkerKind::bypassOn).size() == 1);
    CHECK (out.markers (CaptureMarkerKind::bypassOn)[0].sample == 400);
    CHECK (out.markers (CaptureMarkerKind::bypassOff).size() == 1);
    CHECK (out.markers (CaptureMarkerKind::bypassOff)[0].sample == 1012);

    std::vector<float> stream;
    int64_t first = -1;
    REQUIRE (out.audioStream (stream, first));
    CHECK (first == 0);
    REQUIRE (stream.size() == 2012 / 256 * 256);

    bool ok = true;
    for (std::size_t i = 0; i < stream.size(); ++i)
    {
        const auto x = static_cast<int64_t> (i);
        const auto expected = (x >= 400 && x < 1012) ? 0.0f : scenarioSignal (x);
        ok = ok && stream[i] == expected;
    }
    CHECK (ok);

    for (const auto& r : out.audioRecords)
    {
        CHECK (r.sample % CaptureTap::audioFrameSamples == 0);
        CHECK (r.asAudioNumSamples() == CaptureTap::audioFrameSamples);
    }
}

TEST_CASE ("CaptureTap: audioOff / audioOn mid-frame", "[capturetap]")
{
    Harness h;
    h.bs.captureAudio = true;
    std::vector<float> ones (512, 1.0f);
    h.block (300, {}, {}, ones.data());
    h.drain();

    // Off at 300: the staged frame [256, 512) is completed with zeros and pushed before the marker
    h.bs.captureAudio = false;
    h.block (100, {}, {}, ones.data());
    auto all = h.drainAll();
    REQUIRE (all.size() == 2);
    CHECK (all[0].type == CaptureRecordType::audio);
    CHECK (all[0].sample == 256);
    CHECK (all[1].type == CaptureRecordType::marker);
    CHECK (all[1].asMarker().kind == CaptureMarkerKind::audioOff);
    CHECK (all[1].sample == 300);

    // Nothing while off
    h.block (200, {}, {}, ones.data()); // clock 600
    CHECK (h.drainAll().empty());

    // On at 600: frame [512, 768) has zeros before 600
    h.bs.captureAudio = true;
    h.block (200, {}, {}, ones.data());
    TapOutput out;
    drainTap (h.tap, out);
    REQUIRE (out.frames.size() == 1);
    const auto& frame = out.frames.at (512);
    CHECK (frame[87] == 0.0f);
    CHECK (frame[88] == 1.0f);
    CHECK (frame[255] == 1.0f);
    CHECK (out.markers (CaptureMarkerKind::audioOn).size() == 1);
}

TEST_CASE ("CaptureTap: captureOff / captureOn", "[capturetap]")
{
    Harness h;
    h.bs.captureAudio = true;
    std::vector<float> ones (512, 1.0f);
    std::vector<VsEvent> on { VsEvent::makeNoteOn (0, 1, 60, 100) };
    h.block (300, on, {}, ones.data());
    h.drain();

    // Off: capture-only note-offs, then the partial frame, then the marker
    h.bs.captureEnabled = false;
    std::vector<VsEvent> more { VsEvent::makeNoteOn (5, 1, 62, 100), VsEvent::makeNoteOff (6, 1, 60) };
    h.block (100, more, {}, ones.data());
    auto all = h.drainAll();
    REQUIRE (all.size() == 3);
    CHECK (all[0].type == CaptureRecordType::event);
    CHECK (all[0].asEvent().event == VsEvent::makeNoteOff (0, 1, 60));
    CHECK (all[0].sample == 300);
    CHECK (all[1].type == CaptureRecordType::audio);
    CHECK (all[1].sample == 256);
    CHECK (all[2].asMarker().kind == CaptureMarkerKind::captureOff);
    CHECK (h.tap.getClock() == 400);

    // While off only the clock advances
    h.block (100, {}, {}, ones.data());
    CHECK (h.drainAll().empty());

    // On: forced transport + settings, captureOn, audioOn. Note 62 (opened while off) is not re-opened;
    // its later note-off is recorded (unpaired, the renderer drops it)
    h.bs.captureEnabled = true;
    std::vector<VsEvent> off62 { VsEvent::makeNoteOff (1, 1, 62) };
    h.block (100, off62, {}, ones.data());
    const auto out = h.drain();
    REQUIRE (out.records.size() == 5);
    CHECK (typesOf (out.records)
           == std::vector<CaptureRecordType> { CaptureRecordType::transport, CaptureRecordType::settings,
                                               CaptureRecordType::marker, CaptureRecordType::marker,
                                               CaptureRecordType::event });
    CHECK (out.records[2].asMarker().kind == CaptureMarkerKind::captureOn);
    CHECK (out.records[3].asMarker().kind == CaptureMarkerKind::audioOn);
    CHECK (out.records[4].asEvent().event == VsEvent::makeNoteOff (0, 1, 62));
}

//==============================================================================
TEST_CASE ("CaptureTap: record overflow -> gap record, resync note-ons, forced transport and settings",
           "[capturetap]")
{
    Harness h;
    h.transport = playing (0.0);
    std::vector<VsEvent> on { VsEvent::makeNoteOn (0, 1, 60, 100), VsEvent::makeNoteOn (0, 2, 50, 77) };
    h.block (512, on);

    // No consumer: 100 blocks of 200 bends overflow the 16384-record ring
    std::vector<VsEvent> bends;
    for (int i = 0; i < 200; ++i)
        bends.push_back (VsEvent::makePitchBend (i, 1, static_cast<int16_t> (i)));

    for (int b = 0; b < 100; ++b)
    {
        h.transport = playing (static_cast<double> (512 * (b + 1)) * 120.0 / (60.0 * 48000.0));
        h.block (512, bends);
    }

    // 6 start-up records + 20000 bends; after the first drop every block also forces (and drops) a
    // transport and a settings record
    const auto dropped = h.tap.getOverflowCounts().records;
    CHECK (dropped >= 6 + 100 * 200 - CaptureTap::recordRingCapacity);
    CHECK (h.tap.getOverflowCounts().audioSamples == 0);
    CHECK (h.tap.getRecordRing().size() == CaptureTap::recordRingCapacity);

    // A partly drained ring that cannot hold gap + 2 resync note-ons + the record keeps the gap pending
    CaptureRecord r;
    REQUIRE (h.tap.getRecordRing().pop (r));
    REQUIRE (h.tap.getRecordRing().pop (r));
    h.block (512, std::span<const VsEvent> { bends.data(), 1 });
    const auto droppedAfter = h.tap.getOverflowCounts().records;
    CHECK (droppedAfter == dropped + 3); // transport, settings, bend
    CHECK (h.tap.getRecordRing().size() == CaptureTap::recordRingCapacity - 2);

    // Drain all; the next block pushes the gap (with every drop since the last gap), the resync note-ons,
    // then the forced transport + settings, then the block's event
    const auto before = h.drain();
    CHECK (before.ofType (CaptureRecordType::gap).empty());
    CHECK (before.records.size() + before.audioRecords.size() == CaptureTap::recordRingCapacity - 2);

    std::vector<VsEvent> one { VsEvent::makePitchBend (3, 1, 42) };
    h.transport = playing (100.0); // a jump: would be a structural record anyway
    h.block (512, one);
    const auto out = h.drain();
    REQUIRE (out.records.size() == 6);
    CHECK (out.records[0].type == CaptureRecordType::gap);
    CHECK (out.records[0].asGap().droppedRecords == droppedAfter);
    CHECK (out.records[0].asGap().droppedAudioSamples == 0);
    const auto gapSample = out.records[0].sample;
    CHECK (gapSample == 512 * 102);

    for (int i = 1; i <= 2; ++i)
    {
        CHECK (out.records[static_cast<std::size_t> (i)].sample == gapSample);
        const auto e = out.records[static_cast<std::size_t> (i)].asEvent();
        CHECK (e.event.type == VsEventType::noteOn);
        CHECK (e.timing.kind == EventTimingKind::none);
        CHECK ((e.timing.flags & EventTimingFlags::resync) != 0);
    }

    CHECK (out.records[1].asEvent().event == VsEvent::makeNoteOn (0, 1, 60, 100));
    CHECK (out.records[2].asEvent().event == VsEvent::makeNoteOn (0, 2, 50, 77));
    CHECK (out.records[3].type == CaptureRecordType::transport);
    CHECK (out.records[4].type == CaptureRecordType::settings);
    CHECK (out.records[5].asEvent().event == VsEvent::makePitchBend (0, 1, 42));

    // Counters are cumulative since prepare
    CHECK (h.tap.getOverflowCounts().records == droppedAfter);
    h.tap.prepare (48000.0, 512);
    CHECK (h.tap.getOverflowCounts().records == 0);
}

TEST_CASE ("CaptureTap: audio overflow drops whole frames and keeps later audio aligned", "[capturetap]")
{
    Harness h;
    h.bs.captureAudio = true;
    std::vector<float> mono (512);
    int64_t s = 0;
    const auto audioCapacity = static_cast<int64_t> (h.tap.getAudioRing().getCapacity());
    CHECK (audioCapacity == 131072);

    // 3 s without a consumer
    while (s < 144000)
    {
        for (int i = 0; i < 512; ++i)
            mono[static_cast<std::size_t> (i)] = scenarioSignal (s + i);
        h.block (512, {}, {}, mono.data());
        s += 512;
    }

    const auto droppedAudio = h.tap.getOverflowCounts().audioSamples;
    CHECK (droppedAudio == static_cast<uint64_t> (s / 256 * 256 - audioCapacity));
    CHECK (droppedAudio % 256 == 0);
    CHECK (h.tap.getOverflowCounts().records == 0);

    TapOutput out;
    drainTap (h.tap, out);
    CHECK (static_cast<int64_t> (out.frames.size()) == audioCapacity / 256);

    for (int i = 0; i < 512; ++i)
        mono[static_cast<std::size_t> (i)] = scenarioSignal (s + i);
    h.block (512, {}, {}, mono.data());
    drainTap (h.tap, out);

    // A gap goes out with the next record that fits (here the forced transport record of the next
    // block), so a ring that stays full gives one gap per block; together they count every drop
    const auto gaps = out.ofType (CaptureRecordType::gap);
    REQUIRE_FALSE (gaps.empty());
    uint64_t gapAudio = 0;
    int64_t previous = -1;

    for (const auto& g : gaps)
    {
        gapAudio += g.asGap().droppedAudioSamples;
        CHECK (g.asGap().droppedRecords == 0);
        CHECK (g.sample > previous);
        previous = g.sample;
    }

    CHECK (gapAudio == droppedAudio);

    // The frame after the gap is at its own clock position with its own samples
    const auto& frame = out.frames.at (s);
    CHECK (frame[0] == scenarioSignal (s));
    CHECK (frame[255] == scenarioSignal (s + 255));
}

//==============================================================================
TEST_CASE ("CaptureTap: no allocation on the audio thread", "[capturetap]")
{
    Harness h;
    h.bs.captureAudio = true;
    std::vector<float> mono (4096, 0.1f);
    std::vector<VsEvent> events;
    for (int i = 0; i < 64; ++i)
    {
        events.push_back (VsEvent::makeNoteOn (i * 8, 1, static_cast<uint8_t> (40 + i), 100));
        events.push_back (VsEvent::makePitchBend (i * 8 + 1, 1, static_cast<int16_t> (i * 10)));
    }
    std::vector<VsEventTiming> timings (events.size(), VsEventTiming { 100, EventTimingKind::model, 0 });

    std::size_t allocations = 0;
    {
        CountAllocations counting;
        for (int b = 0; b < 400; ++b) // overflows the record ring on the way (no consumer)
        {
            h.transport = b % 50 < 25 ? playing (b * 0.1, 100.0 + b) : stopped (3.0);
            h.bs.bypassed = b % 97 == 3;
            h.bs.captureEnabled = b % 131 != 7;
            h.bs.captureAudio = b % 61 != 5;
            h.bs.settings.bendRangeSemitones = 2 + b % 3;

            if (b % 10 == 0)
            {
                h.tap.requestNewTake();
                h.tap.requestGrab (4, 10.0);
                h.tap.requestClear();
            }

            h.tap.beginBlock (h.transport, h.bs);
            h.tap.flushNotes();

            if (h.bs.bypassed)
                h.tap.processBypassed (4096);
            else
                h.tap.processChunk (mono.data(), 4096, events, timings);
        }

        allocations = allocationCount;
    }

    CHECK (allocations == 0);
    CHECK (h.tap.getOverflowCounts().records > 0);
}

//==============================================================================
TEST_CASE ("CaptureTap: block-size invariance of the record and audio streams", "[capturetap]")
{
    const double sr = GENERATE (44100.0, 48000.0, 96000.0);
    const auto sc = makeInvarianceScenario (sr);

    TapOutput reference;

    for (const int blockSize : { 4096, 1, 7, 64, 480, 512 })
    {
        CAPTURE (sr, blockSize);
        CaptureTap tap;
        TapOutput out;
        runTap (tap, sc, blockSize, [&] { drainTap (tap, out); });

        CHECK (tap.getOverflowCounts().records == 0);
        CHECK (tap.getOverflowCounts().audioSamples == 0);
        CHECK (tap.getClock() == sc.length);

        // Audio off (5P..6P) and capture off (7P..8P) leave holes: the frames are compared by position
        if (blockSize == 4096)
        {
            reference = out;
            // Sanity of the scenario: every kind of record and marker is there
            // 0, start P, tempo 2P, loop wrap 3P, stop 6P, forced at captureOn 8P
            CHECK (out.ofType (CaptureRecordType::transport).size() == 6);
            CHECK (out.markers (CaptureMarkerKind::newTake).size() == 1);
            CHECK (out.markers (CaptureMarkerKind::grab).size() == 1);
            CHECK (out.markers (CaptureMarkerKind::clear).size() == 1);
            CHECK (out.markers (CaptureMarkerKind::bypassOn).size() == 1);
            CHECK (out.markers (CaptureMarkerKind::bypassOff).size() == 1);
            CHECK (out.markers (CaptureMarkerKind::audioOff).size() == 1);
            CHECK (out.markers (CaptureMarkerKind::audioOn).size() == 3);
            CHECK (out.markers (CaptureMarkerKind::captureOff).size() == 1);
            CHECK (out.markers (CaptureMarkerKind::captureOn).size() == 2);
            CHECK (out.ofType (CaptureRecordType::settings).size() == 3);
            CHECK (out.ofType (CaptureRecordType::event).size() > 1000);
            continue;
        }

        REQUIRE (out.records.size() == reference.records.size());
        bool same = true;
        std::size_t firstDifference = 0;

        for (std::size_t i = 0; i < out.records.size(); ++i)
        {
            if (! (out.records[i] == reference.records[i]))
            {
                same = false;
                firstDifference = i;
                break;
            }
        }

        CAPTURE (firstDifference);
        CHECK (same);
        CHECK (out.frames == reference.frames);
        CHECK (out.audioRecords.size() == reference.audioRecords.size());
    }
}

//==============================================================================
TEST_CASE ("CaptureTap: cost per 128-sample block", "[capturetap][cpu]")
{
    // Spec: < 1 % of a 128-sample block at 48 kHz, in CI (the measured cost is ~150 ns of 26.7 us)
    Harness h (48000.0, 128);
    h.bs.captureAudio = true;
    std::vector<float> mono (128, 0.1f);
    std::vector<VsEvent> events { VsEvent::makePitchBend (5, 1, 100), VsEvent::makeNoteOn (60, 1, 60, 100) };
    std::vector<VsEvent> offs { VsEvent::makeNoteOff (10, 1, 60) };
    TapOutput sink;
    constexpr int blocks = 200000;

    const auto start = std::chrono::steady_clock::now();
    for (int b = 0; b < blocks; ++b)
    {
        h.transport = playing (b * 128.0 * 2.0 / 48000.0);
        h.tap.beginBlock (h.transport, h.bs);
        h.tap.processChunk (mono.data(), 128, b % 2 == 0 ? std::span<const VsEvent> { events } : std::span<const VsEvent> { offs }, {});

        if (b % 64 == 0)
        {
            CaptureRecord r;
            while (h.tap.getRecordRing().pop (r)) {}
            std::array<float, 4096> scratch {};
            while (h.tap.getAudioRing().popBulk (scratch) > 0) {}
        }
    }
    const auto seconds = std::chrono::duration<double> (std::chrono::steady_clock::now() - start).count();
    const auto perBlock = seconds / blocks;
    const auto budget = 0.01 * 128.0 / 48000.0;
    WARN ("CaptureTap per 128-sample block: " << perBlock * 1.0e9 << " ns (budget " << budget * 1.0e9 << " ns)");
    CHECK (perBlock < budget);
}
