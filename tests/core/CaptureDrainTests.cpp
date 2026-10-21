// CaptureDrain (docs/M3-DESIGN.md 5.3, 16). Owner: agent A.
// The consumer loop shared by the plugin's CaptureWorker and the end-to-end tests: CaptureTap ->
// CaptureDrain -> CaptureHistory + TakeSegmenter.

#include "CaptureTestHelpers.h"

#include "vocalsynth/CaptureDrain.h"
#include "vocalsynth/TakeRenderer.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <atomic>
#include <thread>
#include <vector>

using namespace vocalsynth;
using namespace vocalsynth::test;

namespace
{
/** One tap + the full consumer side. */
struct Pipeline
{
    explicit Pipeline (double audioMinutes = 1.0) { history.prepare (audioMinutes); }

    std::size_t drain() { return drainer.drain (tap, history, segmenter, closed); }

    CaptureTap tap;
    CaptureHistory history;
    TakeSegmenter segmenter;
    std::vector<Take> closed;
    CaptureDrain drainer;
};

/** The non-audio records the history keeps in its ring (events, markers, settings, gaps). */
bool storedInRing (const CaptureRecord& r)
{
    return r.type == CaptureRecordType::event || r.type == CaptureRecordType::marker
           || r.type == CaptureRecordType::settings || r.type == CaptureRecordType::gap;
}

void checkSameTakes (const std::vector<Take>& a, const std::vector<Take>& b)
{
    REQUIRE (a.size() == b.size());

    for (std::size_t i = 0; i < a.size(); ++i)
    {
        CAPTURE (i);
        const auto difference = takeDifference (a[i], b[i]);
        INFO (difference);
        CHECK (difference.empty());
    }
}
} // namespace

TEST_CASE ("CaptureDrain: an empty tap drains nothing and keeps its clock", "[capturedrain]")
{
    CaptureTap tap;
    tap.prepare (48000.0, 512);
    tap.processChunk (nullptr, 480, {}, {});

    CaptureHistory history;
    history.prepare (1.0);
    TakeSegmenter segmenter;
    std::vector<Take> closed;
    CaptureDrain drain;

    CHECK (drain.drain (tap, history, segmenter, closed) == 0);
    CHECK (drain.getLastClock() == tap.getClock());
    CHECK (closed.empty());
}

TEST_CASE ("CaptureDrain: records and audio frames reach the history in stream order", "[capturedrain]")
{
    // The same input into two taps: one drained raw (reference), one through CaptureDrain
    auto sc = makeInvarianceScenario (48000.0);
    sc.length = 600000; // stopped, free notes, bends, CCs
    sc.playhead = PlayheadSim (48000.0);
    sc.playhead.add ({ 0, false, 0.0, 120.0, 4, 4 });
    sc.newTakeAt = { 430080 };

    CaptureTap reference;
    TapOutput out;
    runTap (reference, sc, 512, [&] { drainTap (reference, out); });

    Pipeline p;
    std::size_t handled = 0;
    runTap (p.tap, sc, 512, [&] { handled += p.drain(); });

    CHECK (handled == out.records.size() + out.audioRecords.size());
    CHECK (p.drainer.getLastClock() == sc.length);
    CHECK (p.tap.getRecordRing().size() == 0);
    CHECK (p.tap.getAudioRing().size() == 0);

    // Ring records: identical and in the same order
    std::vector<CaptureRecord> expected;
    for (const auto& r : out.records)
        if (storedInRing (r))
            expected.push_back (r);

    CHECK (p.history.copyRecords (0, sc.length + 1) == expected);
    CHECK (p.history.getSampleRate() == 48000.0);
    CHECK (p.history.getMaxBlockSize() == sc.maxBlock);
    CHECK_FALSE (p.history.getTimeline().empty());

    // Audio: every frame at the sample of its audio record
    std::vector<float> stream;
    int64_t first = 0;
    REQUIRE (out.audioStream (stream, first));
    CHECK (first == 0);
    CHECK (p.history.getAudioRange().from == 0);
    CHECK (p.history.getAudioRange().to == static_cast<int64_t> (stream.size()));
    std::vector<float> stored (stream.size());
    CHECK (p.history.readAudio (0, stored) == stream.size());
    CHECK (stored == stream);
    CHECK (p.history.getMissingAudioSamples() == 0);

    // The segmenter saw the same stream: the free take was split by the New take marker
    REQUIRE (p.closed.size() == 1);
    CHECK (p.closed[0].endSample == 430080);
    REQUIRE (p.segmenter.getOpenTake() != nullptr);
    CHECK (p.segmenter.getOpenTake()->kind == TakeKind::manual);

    // Nothing new: nothing handled, the clock stays
    CHECK (p.drain() == 0);
    CHECK (p.drainer.getLastClock() == sc.length);
}

TEST_CASE ("CaptureDrain: the clock alone closes a free take after the gap", "[capturedrain]")
{
    Pipeline p;
    p.tap.prepare (48000.0, 512);
    CaptureBlockSettings bs;
    bs.captureAudio = false;
    const TransportInfo noPlayhead {};

    std::vector<VsEvent> on { VsEvent::makeNoteOn (10, 1, 60, 100) };
    std::vector<VsEvent> off { VsEvent::makeNoteOff (7, 1, 60) };
    p.tap.beginBlock (noPlayhead, bs);
    p.tap.processChunk (nullptr, 480, on, {});
    p.tap.beginBlock (noPlayhead, bs);
    p.tap.processChunk (nullptr, 480, off, {}); // note-off at 487
    p.drain();
    REQUIRE (p.segmenter.getOpenTake() != nullptr);

    // Silence: no records at all, only the clock moves; the take closes when the clock passes the gap
    const int64_t gapEnd = 487 + 144000;
    int64_t clock = 960;
    while (clock + 480 <= gapEnd)
    {
        p.tap.beginBlock (noPlayhead, bs);
        p.tap.processChunk (nullptr, 480, {}, {});
        clock += 480;
        CHECK (p.drain() == 0);
    }

    CHECK (p.drainer.getLastClock() == clock);
    CHECK (clock < gapEnd);
    CHECK (p.closed.empty());

    p.tap.beginBlock (noPlayhead, bs);
    p.tap.processChunk (nullptr, 480, {}, {});
    p.drain();
    REQUIRE (p.closed.size() == 1);
    CHECK (p.closed[0].endSample == 487 + 24000);
    CHECK (p.closed[0].noteCount == 1);
}

TEST_CASE ("CaptureDrain: a full record ring is emptied in one call", "[capturedrain]")
{
    Pipeline p;
    p.tap.prepare (48000.0, 4096);
    CaptureBlockSettings bs;
    bs.captureAudio = false;
    std::vector<VsEvent> bends;
    for (int i = 0; i < 1000; ++i)
        bends.push_back (VsEvent::makePitchBend (i, 1, static_cast<int16_t> (i)));

    for (int b = 0; b < 16; ++b)
    {
        p.tap.beginBlock ({}, bs);
        p.tap.processChunk (nullptr, 4096, bends, {});
    }

    const auto queued = p.tap.getRecordRing().size();
    CHECK (queued == 4 + 16 * 1000);
    CHECK (p.tap.getOverflowCounts().records == 0);
    CHECK (p.drain() == queued);
    CHECK (p.tap.getRecordRing().size() == 0);
    CHECK (p.drainer.getLastClock() == 16 * 4096);
}

//==============================================================================
TEST_CASE ("CaptureDrain: the takes are identical for every block size (end-to-end invariance)", "[capturedrain]")
{
    const double sr = GENERATE (44100.0, 48000.0, 96000.0);
    const auto sc = makeInvarianceScenario (sr);

    std::vector<Take> reference;
    Take referenceOpen;

    for (const int blockSize : { 4096, 1, 7, 64, 480, 512 })
    {
        // Block size 1 is slow: covered at one rate
        if (blockSize == 1 && sr != 48000.0)
            continue;

        CAPTURE (sr, blockSize);
        Pipeline p;
        runTap (p.tap, sc, blockSize, [&] { p.drain(); });

        CHECK (p.tap.getOverflowCounts().records == 0);
        CHECK (p.tap.getOverflowCounts().audioSamples == 0);
        REQUIRE (p.segmenter.getOpenTake() != nullptr);

        if (blockSize == 4096)
        {
            reference = p.closed;
            referenceOpen = *p.segmenter.getOpenTake();

            // The scenario's takes: free (stopped), synced until the New take, the grab, the manual synced
            // take until Stop, a free take until captureOff; after captureOn + Clear a free take is open
            REQUIRE (reference.size() == 5);
            CHECK (reference[0].mode == TakeMode::free);
            CHECK (reference[0].number == 1);
            CHECK (reference[1].mode == TakeMode::synced);
            CHECK (reference[1].kind == TakeKind::automatic);
            CHECK (reference[1].number == 2);
            CHECK (reference[2].kind == TakeKind::grab);
            CHECK (reference[2].number == 4);
            CHECK (reference[3].kind == TakeKind::manual);
            CHECK (reference[3].mode == TakeMode::synced);
            CHECK (reference[3].number == 3);
            CHECK (reference[4].mode == TakeMode::free);
            CHECK (reference[4].endSample == 7 * 430080);
            CHECK (referenceOpen.mode == TakeMode::free);
            CHECK (referenceOpen.startSample >= 8 * 430080);

            for (const auto& take : reference)
            {
                CAPTURE (take.number, take.startSample, take.endSample, take.captureEndSample, take.events.size());
                CHECK (notesPaired (take));
            }

            // The synced manual take spans the loop wrap with a tempo and a time-signature change
            CHECK (reference[3].timeline.getSegments().size() >= 3);
            CHECK (reference[3].timeline.getSegments().front().bpm == 97.5);
            const auto sig = reference[3].timeline.timeSignatureMap (0.0, 1.0e9);
            REQUIRE (sig.size() == 2);
            CHECK (sig.back().numerator == 3);
            continue;
        }

        checkSameTakes (p.closed, reference);
        const auto difference = takeDifference (*p.segmenter.getOpenTake(), referenceOpen);
        INFO (difference);
        CHECK (difference.empty());
    }
}

TEST_CASE ("CaptureDrain: producer and consumer on two threads give the same takes", "[capturedrain]")
{
    const auto sc = makeInvarianceScenario (48000.0);

    // Reference: single-threaded
    Pipeline single;
    runTap (single.tap, sc, 64, [&] { single.drain(); });
    REQUIRE (single.closed.size() == 5);

    // Producer thread = the audio thread (waits only for ring space, like a fast host would never
    // need to); consumer thread = the capture worker, draining whenever it gets scheduled
    Pipeline threaded;
    std::atomic<bool> prepared { false };
    std::atomic<bool> producerDone { false };
    std::atomic<uint64_t> drains { 0 };

    std::thread consumer ([&]
    {
        while (! prepared.load (std::memory_order_acquire))
            std::this_thread::yield();

        while (! producerDone.load (std::memory_order_acquire))
        {
            threaded.drain();
            drains.fetch_add (1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });

    std::thread producer ([&]
    {
        runTap (threaded.tap, sc, 64, [&]
        {
            prepared.store (true, std::memory_order_release);

            // Producer-side view of the free space (a lower bound)
            while (threaded.tap.getRecordRing().freeSpace() < 4096 || threaded.tap.getAudioRing().freeSpace() < 16384)
                std::this_thread::yield();
        }, 512);
        producerDone.store (true, std::memory_order_release);
    });

    producer.join();
    consumer.join();
    threaded.drain(); // whatever the consumer had not picked up yet

    CHECK (drains.load() > 0);
    CHECK (threaded.tap.getOverflowCounts().records == 0);
    CHECK (threaded.tap.getOverflowCounts().audioSamples == 0);
    CHECK (threaded.drainer.getLastClock() == sc.length);
    checkSameTakes (threaded.closed, single.closed);
    REQUIRE (threaded.segmenter.getOpenTake() != nullptr);
    CHECK (takeDifference (*threaded.segmenter.getOpenTake(), *single.segmenter.getOpenTake()).empty());

    std::vector<float> a (100000);
    std::vector<float> b (100000);
    threaded.history.readAudio (sc.length - 100000, a);
    single.history.readAudio (sc.length - 100000, b);
    CHECK (a == b);
}

TEST_CASE ("CaptureDrain M3.1: take limit, Record takes and Grab are identical for every block size",
           "[capturedrain][takelength][recordmode]")
{
    // Every control point on a multiple of P = 430080 = lcm (7, 64, 480, 512, 4096) (8.96 s, 4.48 bars at
    // 120 BPM): background synced takes cut after 4 bars, Record mode with a complete take, one shortened by
    // Stop, one armed while stopped (starts where playback starts), a Grab, back to Background, free takes
    // cut after 4 bars.
    constexpr int64_t P = 430080;
    const double sr = 48000.0;
    TapScenario sc;
    sc.sampleRate = sr;
    sc.maxBlock = 4096;
    sc.length = 11 * P + 1234;
    sc.playhead = PlayheadSim (sr);
    sc.playhead.add ({ 0, true, 0.0, 120.0, 4, 4 });
    sc.playhead.change (5 * P, false, 120.0);
    sc.playhead.change (7 * P, true, 120.0);
    sc.playhead.change (9 * P, false, 120.0);

    CaptureSettingsInfo background;
    background.takeMaxBars = 4;
    auto record = background;
    record.captureMode = CaptureMode::record;
    sc.settingsFrom = { { 0, background }, { 2 * P, record }, { 8 * P, background } };
    sc.recordAt = { { 3 * P, 2 }, { 4 * P, 8 }, { 6 * P, 1 } };
    sc.grabAt = { 8 * P };
    sc.grabBars = 2;

    for (int64_t s = 123, k = 0; s < sc.length; s += 5000, ++k)
    {
        const auto note = static_cast<uint8_t> (60 + k % 12);
        sc.events.push_back ({ s, VsEvent::makeNoteOn (0, 1, note, 90),
                               { static_cast<int32_t> ((k * 37) % 2000), EventTimingKind::measured, 0 } });
        sc.events.push_back ({ s + 3077, VsEvent::makeNoteOff (0, 1, note),
                               { static_cast<int32_t> ((k * 53) % 3000), EventTimingKind::model, 0 } });
    }

    std::stable_sort (sc.events.begin(), sc.events.end(),
                      [] (const ScheduledEvent& a, const ScheduledEvent& b) { return a.sample < b.sample; });

    std::vector<Take> reference;

    for (const int blockSize : { 4096, 1, 7, 64, 480, 512 })
    {
        CAPTURE (blockSize);
        Pipeline p;
        runTap (p.tap, sc, blockSize, [&] { p.drain(); });
        CHECK (p.tap.getOverflowCounts().records == 0);

        if (blockSize == 4096)
        {
            reference = p.closed;
            const auto count = [&reference] (auto predicate)
            { return std::count_if (reference.begin(), reference.end(), predicate); };

            // 2P of background play = 8.96 bars: anchored at bar 1, cut at bars 5 and 9
            CHECK (count ([] (const Take& t) { return t.mode == TakeMode::synced && t.kind == TakeKind::automatic && t.maxBars == 4; }) >= 3);
            CHECK (count ([] (const Take& t) { return t.kind == TakeKind::grab; }) == 1);
            CHECK (count ([] (const Take& t) { return t.mode == TakeMode::free && t.maxBars == 4; }) >= 2);
            REQUIRE (count ([] (const Take& t) { return t.kind == TakeKind::record; }) == 3);

            std::vector<Take> records;
            std::copy_if (reference.begin(), reference.end(), std::back_inserter (records),
                          [] (const Take& t) { return t.kind == TakeKind::record; });

            // Armed at 3P (bar 14.44) for 2 bars: bars 15 .. 17
            CHECK (records[0].startSample == 14 * 96000);
            CHECK (records[0].endSample == 16 * 96000);
            CHECK (records[0].anchorBar == 15);
            // Armed at 4P for 8 bars from bar 19, stopped at 5P
            CHECK (records[1].startSample == 18 * 96000);
            CHECK (records[1].endSample == 5 * P);
            // Armed at 6P while stopped: starts at the first bar line after Play at 7P (ppq 89.6 -> 92)
            CHECK (records[2].startSample == 7 * P + 57600);
            CHECK (records[2].endSample == records[2].startSample + 96000);
            CHECK (records[2].anchorBar == 24);

            for (const auto& t : records)
            {
                RenderOptions o;
                o.roundTripSamples = TakeRenderer::roundTripFor (t, CaptureOffsetSettings {});
                const auto r = TakeRenderer::render (t, o);
                CHECK (r.originBar == t.anchorBar);
                CHECK (r.endTick == (t.endSample == 5 * P ? 5 : t.maxBars) * 3840); // stopped after 4.4 bars
            }
            continue;
        }

        checkSameTakes (p.closed, reference);
    }
}
