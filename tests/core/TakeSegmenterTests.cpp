// TakeSegmenter (docs/M3-DESIGN.md 5.2, 16). Owner: agent A.

#include "CaptureTestHelpers.h"

#include "vocalsynth/TakeSegmenter.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <random>
#include <vector>

using namespace vocalsynth;
using namespace vocalsynth::test;
using Catch::Approx;

namespace
{
// RecordFeeder defaults: 48 kHz, max block 512 -> capture tail max (1024 + 4800, 14400) + 12000
constexpr int64_t tail = 26400;
constexpr int64_t gap3s = 144000;
constexpr int64_t freeTail = 24000;
constexpr int64_t bar = 96000; // 4/4 at 120 BPM, 48 kHz

int countSynthetic (const Take& t)
{
    return static_cast<int> (std::count_if (t.events.begin(), t.events.end(), [] (const CapturedEvent& e) { return e.synthetic; }));
}
} // namespace

TEST_CASE ("TakeSegmenter: capture tail covers the largest round trip plus the detection allowance",
           "[takesegmenter]")
{
    // ASIO 2048 at 48 kHz: max (4096 + 4800, 14400) + 0.25 s = 14400 + 12000
    CHECK (TakeSegmenter::compensationTailSamples (48000.0, 2048) == 26400);
    CHECK (TakeSegmenter::compensationTailSamples (48000.0, 512) == tail);
    // 8192-sample blocks: 2 x 8192 + 100 ms beats the 300 ms manual maximum
    CHECK (TakeSegmenter::compensationTailSamples (48000.0, 8192) == 16384 + 4800 + 12000);
    CHECK (TakeSegmenter::compensationTailSamples (0.0, 2048) == 0);

    TakeSegmenter segmenter;
    CHECK (segmenter.getOpenTake() == nullptr);
    CHECK (segmenter.getNextTakeNumber() == 1);
    CHECK (segmenter.getState() == TakeSegmenter::State::off);
    segmenter.setTakeGapSeconds (0.1);
    CHECK (segmenter.getTakeGapSeconds() == TakeSegmenter::minTakeGapSeconds);
    segmenter.setTakeGapSeconds (100.0);
    CHECK (segmenter.getTakeGapSeconds() == TakeSegmenter::maxTakeGapSeconds);
}

//==============================================================================
TEST_CASE ("TakeSegmenter: transport start/stop gives one synced take that closes after its capture tail",
           "[takesegmenter]")
{
    RecordFeeder f;
    CHECK (f.segmenter.getState() == TakeSegmenter::State::freeWaiting);

    f.transport (48000, true, 0.0);
    REQUIRE (f.segmenter.getOpenTake() != nullptr);
    CHECK (f.segmenter.getState() == TakeSegmenter::State::synced);
    CHECK (f.segmenter.getOpenTake()->mode == TakeMode::synced);
    CHECK (f.segmenter.getOpenTake()->number == 1);

    f.noteOn (60000, 60, 100, 2000);
    f.noteOff (70000, 60, 1500);
    f.advance (100000);
    CHECK (f.segmenter.getOpenTake()->endSample == 100000);

    f.transport (144000, false, 2.0);
    CHECK (f.segmenter.getOpenTake() == nullptr);
    CHECK (f.segmenter.getState() == TakeSegmenter::State::freeWaiting);

    // Sung just before Stop, emitted after it (round trip + detection delay): stays in the synced take and
    // does not start a free take
    f.noteOn (150000, 62, 90, 8000);
    f.noteOff (160000, 62);
    CHECK (f.segmenter.getOpenTake() == nullptr);

    f.advance (144000 + tail - 1);
    CHECK (f.closed.empty());
    f.advance (144000 + tail);
    REQUIRE (f.closed.size() == 1);

    const auto& take = f.closed[0];
    CHECK (take.closed);
    CHECK (take.number == 1);
    CHECK (take.mode == TakeMode::synced);
    CHECK (take.kind == TakeKind::automatic);
    CHECK (take.startSample == 48000);
    CHECK (take.endSample == 144000);
    CHECK (take.captureEndSample == 144000 + tail);
    CHECK (take.sampleRate == 48000.0);
    CHECK (take.maxBlockSize == 512);
    CHECK (take.noteCount == 2);
    REQUIRE (take.events.size() == 4);
    CHECK (take.events[0].sample == 60000);
    CHECK (take.events[0].estimatedSample == 58000);
    CHECK (take.events[0].refinedSample == CapturedEvent::noSample);
    CHECK (take.events[2].sample == 150000);
    CHECK (take.events[2].estimatedSample == 142000);
    CHECK (countSynthetic (take) == 0);
    CHECK (notesPaired (take));
    CHECK_FALSE (take.incomplete);
    CHECK (take.audio == TakeAudio::unknown);

    // The take's own timeline: from its playing segment at the start to the stop
    REQUIRE (take.timeline.getSegments().size() == 2);
    CHECK (take.timeline.getSegments()[0].startSample == 48000);
    CHECK (take.timeline.getSegments()[0].playing);
    CHECK_FALSE (take.timeline.getSegments()[1].playing);
    CHECK (take.timeline.beatsAt (72000.0) == Approx (1.0));

    // After the tail a note-on starts a free take
    f.noteOn (144000 + tail, 64);
    REQUIRE (f.segmenter.getOpenTake() != nullptr);
    CHECK (f.segmenter.getOpenTake()->mode == TakeMode::free);
    CHECK (f.segmenter.getOpenTake()->number == 2);
    CHECK (f.segmenter.getOpenTake()->startSample == 144000 + tail);
}

TEST_CASE ("TakeSegmenter: a note-on in the tail held past it is closed there; it starts no free take",
           "[takesegmenter]")
{
    RecordFeeder f;
    f.transport (0, true, 0.0);
    f.noteOn (1000, 60);
    f.noteOff (2000, 60);
    f.transport (48000, false, 2.0);
    f.noteOn (50000, 64); // in the tail, held
    f.advance (48000 + tail);
    REQUIRE (f.closed.size() == 1);
    CHECK (f.closed[0].events.back().synthetic);
    CHECK (f.closed[0].events.back().sample == 48000 + tail);
    CHECK (notesPaired (f.closed[0]));

    f.noteOff (100000, 64);
    CHECK (f.segmenter.getOpenTake() == nullptr);
    f.noteOn (110000, 65);
    REQUIRE (f.segmenter.getOpenTake() != nullptr);
    CHECK (f.segmenter.getOpenTake()->startSample == 110000);
    CHECK (f.segmenter.getOpenTake()->events.size() == 1);
}

TEST_CASE ("TakeSegmenter: a transport start ends a free take; sounding notes continue in the synced take",
           "[takesegmenter]")
{
    RecordFeeder f;
    f.transport (0, false, 0.0);
    f.noteOn (1000, 60);
    f.transport (48000, true, 0.0);
    REQUIRE (f.closed.size() == 1);
    CHECK (f.closed[0].mode == TakeMode::free);
    CHECK (f.closed[0].endSample == 48000);
    CHECK (f.closed[0].events.back().synthetic);
    CHECK (f.closed[0].events.back().sample == 48000);

    REQUIRE (f.segmenter.getOpenTake() != nullptr);
    const auto& open = *f.segmenter.getOpenTake();
    CHECK (open.mode == TakeMode::synced);
    REQUIRE (open.events.size() == 1);
    CHECK (open.events[0].synthetic);
    CHECK (open.events[0].sample == 48000);
    CHECK (open.events[0].event == VsEvent::makeNoteOn (0, 1, 60, 100));
    CHECK (open.noteCount == 0); // counted when the take closes
}

TEST_CASE ("TakeSegmenter: a loop wrap keeps one take", "[takesegmenter]")
{
    RecordFeeder f;
    f.transport (0, true, 16.0);
    f.noteOn (10000, 60);
    f.noteOff (300000, 60);
    // 16 beats (8 s) later the host jumps back to ppq 16, twice
    f.transport (8 * 48000, true, 16.0);
    f.noteOn (400000, 62);
    f.noteOff (500000, 62);
    f.transport (16 * 48000, true, 16.0);
    f.noteOn (800000, 64);
    f.noteOff (810000, 64);
    f.transport (900000, false, 16.0 + (900000.0 - 768000.0) / 24000.0);
    f.advance (900000 + tail);

    REQUIRE (f.closed.size() == 1);
    const auto& take = f.closed[0];
    CHECK (take.startSample == 0);
    CHECK (take.endSample == 900000);
    CHECK (take.noteCount == 3);
    // Unwrapped beats run on across both wraps
    CHECK (take.timeline.beatsAt (8.0 * 48000.0) == Approx (32.0));
    CHECK (take.timeline.beatsAt (16.0 * 48000.0) == Approx (48.0));
    CHECK (take.timeline.beatsAt (800000.0) == Approx (16.0 + 800000.0 / 24000.0));
    CHECK (take.timeline.barAt (8.0 * 48000.0 + 10.0).number == 5); // ppq 16 = bar 5 again
}

//==============================================================================
TEST_CASE ("TakeSegmenter: free mode, gap just below / above takeGapSeconds", "[takesegmenter]")
{
    const double gapSeconds = GENERATE (3.0, 0.5, 5.0);
    const auto gap = static_cast<int64_t> (gapSeconds * 48000.0);
    CAPTURE (gapSeconds);

    SECTION ("a note-on 1 sample before the gap continues the take")
    {
        RecordFeeder f;
        f.segmenter.setTakeGapSeconds (gapSeconds);
        f.noteOn (1000, 60);
        f.noteOff (10000, 60);
        f.noteOn (10000 + gap - 1, 62);
        f.noteOff (10000 + gap + 1000, 62);
        f.advance (10000 + gap + 1000 + gap - 1);
        CHECK (f.closed.empty());
        f.advance (10000 + gap + 1000 + gap);
        REQUIRE (f.closed.size() == 1);
        CHECK (f.closed[0].noteCount == 2);
        CHECK (f.closed[0].endSample == 10000 + gap + 1000 + std::min (freeTail, gap));
        CHECK (f.closed[0].captureEndSample == f.closed[0].endSample);
    }

    SECTION ("a note-on at the gap starts a new take")
    {
        RecordFeeder f;
        f.segmenter.setTakeGapSeconds (gapSeconds);
        f.noteOn (1000, 60);
        f.noteOff (10000, 60);
        f.noteOn (10000 + gap, 62);
        REQUIRE (f.closed.size() == 1);
        const auto& first = f.closed[0];
        CHECK (first.mode == TakeMode::free);
        CHECK (first.startSample == 1000);
        CHECK (first.endSample == 10000 + std::min (freeTail, gap));
        CHECK (first.noteCount == 1);
        REQUIRE (f.segmenter.getOpenTake() != nullptr);
        CHECK (f.segmenter.getOpenTake()->startSample == 10000 + gap);
        CHECK (f.segmenter.getOpenTake()->number == 2);
    }
}

TEST_CASE ("TakeSegmenter: free take events after its end are dropped; a note sounding keeps it open",
           "[takesegmenter]")
{
    RecordFeeder f;
    f.noteOn (0, 60);
    f.advance (10 * 48000); // held 10 s: no gap while a note sounds
    CHECK (f.closed.empty());
    f.noteOff (10 * 48000, 60);
    f.bend (10 * 48000 + 1000, 100);    // inside the 0.5 s tail: kept
    f.bend (10 * 48000 + 30000, 200);   // after the tail, before the gap closes: dropped
    f.advance (10 * 48000 + gap3s);
    REQUIRE (f.closed.size() == 1);
    CHECK (f.closed[0].endSample == 10 * 48000 + freeTail);
    CHECK (f.closed[0].events.size() == 3);
}

TEST_CASE ("TakeSegmenter: results do not depend on the advanceTo call pattern", "[takesegmenter]")
{
    // A stream with free takes, a synced take with tail, a split, a grab, a gap and a clear
    std::vector<CaptureRecord> records;
    const auto ev = [&] (int64_t s, VsEvent e, int32_t delay = 0, uint8_t flags = 0)
    { records.push_back (CaptureRecord::makeEvent (s, e, { delay, EventTimingKind::measured, flags })); };
    const auto tr = [&] (int64_t s, bool playing, double ppq)
    {
        TransportInfo t;
        t.isPlaying = playing;
        t.hasPpq = true;
        t.ppqPosition = ppq;
        t.hasBpm = true;
        t.bpm = 120.0;
        records.push_back (CaptureRecord::makeTransport (s, t));
    };
    const auto mk = [&] (int64_t s, CaptureMarkerKind k, int bars = 2, double seconds = 10.0)
    { records.push_back (CaptureRecord::makeMarker (s, { k, bars, seconds })); };

    records.push_back (CaptureRecord::makeReset (0, { 48000.0, 512, true }));
    records.push_back (CaptureRecord::makeSettings (0, {}));
    mk (0, CaptureMarkerKind::captureOn);
    tr (0, false, 0.0);

    std::mt19937 rng (3);
    int64_t s = 1000;

    for (int phase = 0; phase < 6; ++phase)
    {
        for (int k = 0; k < 40; ++k)
        {
            const auto note = static_cast<uint8_t> (50 + (rng() % 20));
            ev (s, VsEvent::makeNoteOn (0, 1, note, 100), static_cast<int32_t> (rng() % 3000));
            ev (s + 2000 + static_cast<int64_t> (rng() % 5000), VsEvent::makePitchBend (0, 1, 100));
            ev (s + 9000, VsEvent::makeNoteOff (0, 1, note), static_cast<int32_t> (rng() % 3000));
            s += 12000 + static_cast<int64_t> (rng() % 3000);
        }

        // Phase transitions: long pause (gap), start, split, grab, stop, gap record, clear
        switch (phase)
        {
            case 0: s += 4 * 48000; break;
            case 1: tr (s, true, 0.0); mk (s + 50000, CaptureMarkerKind::newTake); break;
            case 2:
                mk (s, CaptureMarkerKind::grab);
                records.push_back (CaptureRecord::makeGap (s + 1000, { 5, 0 }));
                break;
            case 3: tr (s, false, 50.0); s += 5000; break;
            case 4: break;
            case 5: mk (s, CaptureMarkerKind::clear); break;
        }

        s += 3000;
    }

    std::stable_sort (records.begin(), records.end(),
                      [] (const CaptureRecord& a, const CaptureRecord& b) { return a.sample < b.sample; });
    const auto end = records.back().sample + 10 * 48000;

    const auto run = [&] (int pattern)
    {
        CaptureHistory history;
        history.prepare (1.0);
        TakeSegmenter seg;
        std::vector<Take> closed;
        std::mt19937 coin (11);

        for (const auto& r : records)
        {
            if (pattern == 2 || (pattern == 3 && coin() % 3 == 0))
                seg.advanceTo (r.sample, history, closed); // every record before r is in

            history.addRecord (r);
            seg.process (r, history, closed);

            if (pattern == 1)
                seg.advanceTo (r.sample, history, closed);
        }

        seg.advanceTo (end, history, closed);
        return closed;
    };

    const auto reference = run (0);
    CHECK (reference.size() == 5); // free, free, synced, grab, manual synced (the last free one is cleared)
    CHECK (std::any_of (reference.begin(), reference.end(), [] (const Take& t) { return t.kind == TakeKind::grab; }));
    CHECK (std::any_of (reference.begin(), reference.end(), [] (const Take& t) { return t.kind == TakeKind::manual; }));
    CHECK (std::any_of (reference.begin(), reference.end(), [] (const Take& t) { return t.incomplete; }));

    for (int pattern = 1; pattern <= 3; ++pattern)
    {
        CAPTURE (pattern);
        const auto other = run (pattern);
        REQUIRE (other.size() == reference.size());
        for (std::size_t i = 0; i < other.size(); ++i)
            CHECK (takeDifference (other[i], reference[i]).empty());
    }
}

//==============================================================================
TEST_CASE ("TakeSegmenter: New take inside a held note", "[takesegmenter]")
{
    SECTION ("free: synthetic note-off / note-on at the marker")
    {
        RecordFeeder f;
        f.noteOn (1000, 60, 77);
        f.marker (5000, CaptureMarkerKind::newTake);
        REQUIRE (f.closed.size() == 1);
        const auto& first = f.closed[0];
        CHECK (first.endSample == 5000);
        CHECK (first.kind == TakeKind::automatic);
        REQUIRE (first.events.size() == 2);
        CHECK (first.events[1].synthetic);
        CHECK (first.events[1].sample == 5000);
        CHECK (first.events[1].event.type == VsEventType::noteOff);

        REQUIRE (f.segmenter.getOpenTake() != nullptr);
        const auto& second = *f.segmenter.getOpenTake();
        CHECK (second.kind == TakeKind::manual);
        CHECK (second.mode == TakeMode::free);
        CHECK (second.startSample == 5000);
        REQUIRE (second.events.size() == 1);
        CHECK (second.events[0].synthetic);
        CHECK (second.events[0].event == VsEvent::makeNoteOn (0, 1, 60, 77));

        f.noteOff (9000, 60);
        f.advance (9000 + gap3s);
        REQUIRE (f.closed.size() == 2);
        CHECK (f.closed[1].number == 2);
        CHECK (notesPaired (f.closed[1]));
    }

    SECTION ("free, no note sounding: the manual take waits takeGapSeconds for a note")
    {
        RecordFeeder f;
        f.marker (5000, CaptureMarkerKind::newTake);
        REQUIRE (f.segmenter.getOpenTake() != nullptr);
        f.noteOn (5000 + gap3s - 1, 60);
        f.noteOff (5000 + gap3s + 100, 60);
        f.advance (5000 + 3 * gap3s);
        REQUIRE (f.closed.size() == 1);
        CHECK (f.closed[0].startSample == 5000);
        CHECK (f.closed[0].kind == TakeKind::manual);

        // Without a note the manual take is discarded and its number freed
        RecordFeeder g;
        g.marker (5000, CaptureMarkerKind::newTake);
        g.advance (5000 + gap3s);
        CHECK (g.closed.empty());
        CHECK (g.segmenter.getOpenTake() == nullptr);
        CHECK (g.segmenter.getNextTakeNumber() == 1);
    }

    SECTION ("synced: the old take collects its tail in parallel with the new one")
    {
        RecordFeeder f;
        f.transport (0, true, 0.0);
        f.noteOn (10000, 60);
        f.marker (48000, CaptureMarkerKind::newTake);
        f.noteOff (60000, 60);
        f.noteOn (62000, 62);
        f.noteOff (70000, 62);
        f.advance (48000 + tail - 1);
        CHECK (f.closed.empty());
        f.advance (48000 + tail);
        REQUIRE (f.closed.size() == 1);
        f.noteOn (80000, 64); // after the tail: only in the new take

        const auto& first = f.closed[0];
        CHECK (first.endSample == 48000);
        CHECK (first.captureEndSample == 48000 + tail);
        CHECK (first.noteCount == 2);     // 60 and 62 (in the tail; the renderer's clamp sorts it out)
        CHECK (notesPaired (first));
        CHECK (countSynthetic (first) == 0);

        REQUIRE (f.segmenter.getOpenTake() != nullptr);
        const auto& second = *f.segmenter.getOpenTake();
        CHECK (second.kind == TakeKind::manual);
        CHECK (second.mode == TakeMode::synced);
        CHECK (second.startSample == 48000);
        CHECK (second.number == 2);
        REQUIRE (second.events.size() == 5);
        CHECK (second.events[0].synthetic);
        CHECK (second.events[0].event.data1 == 60);
        REQUIRE_FALSE (second.timeline.empty());
        CHECK (second.timeline.getSegments()[0].startSample == 48000);
        CHECK (second.timeline.beatsAt (48000.0) == Approx (2.0));
    }
}

TEST_CASE ("TakeSegmenter: empty takes are discarded, numbers are never reused once listed", "[takesegmenter]")
{
    RecordFeeder f;
    f.transport (0, true, 0.0);
    f.transport (48000, false, 2.0);
    f.advance (48000 + tail);
    CHECK (f.closed.empty());
    CHECK (f.segmenter.getNextTakeNumber() == 1);

    f.noteOn (100000, 60);
    f.noteOff (101000, 60);
    f.marker (110000, CaptureMarkerKind::clear); // discards take 1 (never listed, but its number is gone)
    CHECK (f.segmenter.getClearCount() == 1);
    f.noteOn (120000, 60);
    f.noteOff (121000, 60);
    f.advance (121000 + gap3s);
    REQUIRE (f.closed.size() == 1);
    CHECK (f.closed[0].number == 2);
}

//==============================================================================
TEST_CASE ("TakeSegmenter: reset records", "[takesegmenter]")
{
    SECTION ("same-rate reset closes the open take at once (no tail) and ends pending tails")
    {
        RecordFeeder f;
        f.transport (0, true, 0.0);
        f.noteOn (1000, 60);
        f.noteOff (2000, 60);
        f.transport (10000, false, 10000.0 / 24000.0);  // closing until 10000 + tail
        f.noteOn (12000, 62);                           // free start not allowed (tail)
        f.feed (CaptureRecord::makeReset (20000, { 48000.0, 1024, false }));
        REQUIRE (f.closed.size() == 1);
        CHECK (f.closed[0].captureEndSample == 20000);
        CHECK (f.closed[0].endSample == 10000);
        CHECK (f.closed[0].events.back().synthetic); // note 62 closed at the reset
        CHECK (f.segmenter.getState() == TakeSegmenter::State::freeWaiting);

        // The forced transport record after the reset starts the next synced take
        f.transport (20000, true, 1.0);
        REQUIRE (f.segmenter.getOpenTake() != nullptr);
        CHECK (f.segmenter.getOpenTake()->maxBlockSize == 1024);
        f.noteOn (21000, 60);
        f.feed (CaptureRecord::makeReset (30000, { 48000.0, 1024, false }));
        REQUIRE (f.closed.size() == 2);
        CHECK (f.closed[1].endSample == 30000);
        CHECK (f.closed[1].captureEndSample == 30000);
    }

    SECTION ("clearing reset: the open take closes at the newest sample, everything else is forgotten")
    {
        RecordFeeder f;
        f.noteOn (1000, 60);
        f.advance (5000);
        f.feed (CaptureRecord::makeReset (0, { 96000.0, 256, true }));
        REQUIRE (f.closed.size() == 1);
        CHECK (f.closed[0].endSample == 5000);
        CHECK (f.closed[0].sampleRate == 48000.0);
        CHECK (f.segmenter.getState() == TakeSegmenter::State::off);
        CHECK (f.segmenter.getNextTakeNumber() == 2);

        // Capture starts again with captureOn; the note sounding before is forgotten
        f.marker (0, CaptureMarkerKind::captureOn);
        f.noteOn (100, 61);
        REQUIRE (f.segmenter.getOpenTake() != nullptr);
        CHECK (f.segmenter.getOpenTake()->sampleRate == 96000.0);
        CHECK (f.segmenter.getOpenTake()->events.size() == 1);
    }
}

TEST_CASE ("TakeSegmenter: Clear discards the open take, closing takes and pending grabs", "[takesegmenter]")
{
    RecordFeeder f;
    f.transport (0, true, 0.0);
    f.noteOn (1000, 60);
    f.marker (40000, CaptureMarkerKind::grab); // pending until 40000 + tail
    f.transport (48000, false, 2.0); // take 1 closing
    f.transport (50000, true, 2.0);  // take 3 open (the grab holds number 2)
    f.noteOn (51000, 62);
    f.marker (52000, CaptureMarkerKind::clear);
    f.advance (52000 + 10 * tail);

    CHECK (f.closed.empty());
    CHECK (f.segmenter.getClearCount() == 1);
    // Playing: a new synced take starts at the marker with the sounding notes
    REQUIRE (f.segmenter.getOpenTake() != nullptr);
    const auto& open = *f.segmenter.getOpenTake();
    CHECK (open.startSample == 52000);
    CHECK (open.number == 4);
    CHECK (open.events.size() == 2); // 60 and 62 still sound
    CHECK (open.events[0].synthetic);
}

TEST_CASE ("TakeSegmenter: a gap ends every sounding note; resync note-ons re-open the real ones", "[takesegmenter]")
{
    SECTION ("note still on in the output")
    {
        RecordFeeder f;
        f.noteOn (1000, 60);
        f.gap (5000);
        f.noteOn (5000, 60, 100, 0, EventTimingFlags::resync);
        f.noteOff (9000, 60);
        f.advance (9000 + gap3s);
        REQUIRE (f.closed.size() == 1);
        const auto& take = f.closed[0];
        CHECK (take.incomplete);
        REQUIRE (take.events.size() == 4);
        CHECK (take.events[1].synthetic);
        CHECK (take.events[1].sample == 5000);
        CHECK (take.events[1].event.type == VsEventType::noteOff);
        CHECK ((take.events[2].timing.flags & EventTimingFlags::resync) != 0);
        CHECK_FALSE (take.events[2].synthetic);
        CHECK (notesPaired (take));
    }

    SECTION ("lost note-off: the take still ends")
    {
        RecordFeeder f;
        f.noteOn (1000, 60);
        f.gap (5000); // the note-off was among the dropped records: no resync note-on
        f.advance (5000 + gap3s);
        REQUIRE (f.closed.size() == 1);
        CHECK (f.closed[0].incomplete);
        CHECK (f.closed[0].endSample == 5000 + freeTail);
    }

    SECTION ("a free take starting with a resync note-on is incomplete; gaps in a closing tail mark it")
    {
        RecordFeeder f;
        f.gap (100);
        f.noteOn (100, 60, 100, 0, EventTimingFlags::resync);
        REQUIRE (f.segmenter.getOpenTake() != nullptr);
        CHECK (f.segmenter.getOpenTake()->incomplete);

        RecordFeeder g;
        g.transport (0, true, 0.0);
        g.noteOn (1000, 60);
        g.transport (48000, false, 2.0);
        g.gap (50000);
        g.advance (48000 + tail);
        REQUIRE (g.closed.size() == 1);
        CHECK (g.closed[0].incomplete);
        CHECK (notesPaired (g.closed[0]));
    }
}

TEST_CASE ("TakeSegmenter: closeIdle closes a free take only when no note sounds", "[takesegmenter]")
{
    RecordFeeder f;
    f.noteOn (1000, 60);
    f.segmenter.closeIdle (f.history, f.closed);
    CHECK (f.closed.empty());
    f.noteOff (2000, 60);
    f.advance (10000);
    f.segmenter.closeIdle (f.history, f.closed);
    REQUIRE (f.closed.size() == 1);
    CHECK (f.closed[0].endSample == 10000); // min (note-off + 0.5 s, newest sample)
    CHECK (f.segmenter.getState() == TakeSegmenter::State::freeWaiting);

    // Synced takes are not affected
    f.transport (20000, true, 0.0);
    f.noteOn (21000, 60);
    f.noteOff (22000, 60);
    f.segmenter.closeIdle (f.history, f.closed);
    CHECK (f.closed.size() == 1);
    CHECK (f.segmenter.getOpenTake() != nullptr);
}

TEST_CASE ("TakeSegmenter: captureOff closes without a tail, captureOn while playing opens a synced take",
           "[takesegmenter]")
{
    RecordFeeder f;
    f.transport (0, true, 0.0);
    f.noteOn (1000, 60);
    f.noteOff (30000, 60); // the tap's capture-only note-off before the marker
    f.marker (30000, CaptureMarkerKind::captureOff);
    REQUIRE (f.closed.size() == 1);
    CHECK (f.closed[0].endSample == 30000);
    CHECK (f.closed[0].captureEndSample == 30000);
    // The note-off at the marker sample stays in the take (it is not "after the end")
    CHECK (notesPaired (f.closed[0]));
    CHECK_FALSE (f.closed[0].events.back().synthetic);
    CHECK (f.closed[0].events.back().sample == 30000);
    CHECK (f.segmenter.getState() == TakeSegmenter::State::off);

    // Same in free mode, plus a bend at the marker sample, which is dropped
    RecordFeeder g;
    g.noteOn (1000, 60);
    g.noteOff (30000, 60);
    g.bend (30000, 500);
    g.marker (30000, CaptureMarkerKind::captureOff);
    REQUIRE (g.closed.size() == 1);
    CHECK (g.closed[0].events.size() == 2);
    CHECK (notesPaired (g.closed[0]));
    CHECK_FALSE (g.closed[0].events.back().synthetic);

    f.noteOn (40000, 62); // not recorded by a real tap while off; ignored by the state anyway
    CHECK (f.segmenter.getOpenTake() == nullptr);

    f.transport (50000, true, 50000.0 / 24000.0);
    f.marker (50000, CaptureMarkerKind::captureOn);
    REQUIRE (f.segmenter.getOpenTake() != nullptr);
    CHECK (f.segmenter.getOpenTake()->mode == TakeMode::synced);
    CHECK (f.segmenter.getOpenTake()->startSample == 50000);
}

//==============================================================================
TEST_CASE ("TakeSegmenter: a take reaching maxTakeEvents is split and continued", "[takesegmenter]")
{
    RecordFeeder f;
    f.noteOn (0, 60);
    const auto limit = static_cast<int64_t> (TakeSegmenter::maxTakeEvents - TakeSegmenter::splitReserveEvents);

    for (int64_t i = 1; i < limit + 1000; ++i)
        f.bend (i, static_cast<int16_t> (i % 8000));

    REQUIRE (f.closed.size() == 1);
    const auto& first = f.closed[0];
    CHECK (first.events.size() == static_cast<std::size_t> (limit) + 1); // + synthetic note-off
    CHECK (first.endSample == limit);
    CHECK (notesPaired (first));

    REQUIRE (f.segmenter.getOpenTake() != nullptr);
    const auto& second = *f.segmenter.getOpenTake();
    CHECK (second.kind == TakeKind::automatic);
    CHECK (second.startSample == limit);
    CHECK (second.events.front().synthetic); // note 60 continues
    CHECK (second.events.size() == 1000 + 1);
}

TEST_CASE ("TakeSegmenter: a long take keeps its own timeline when the history evicts its start", "[takesegmenter]")
{
    RecordFeeder f;
    f.transport (0, true, 0.0);
    f.noteOn (100, 60);
    f.noteOff (2000, 60);

    // 1500 tempo changes and 300 k non-event records (they fill the history ring, not the take)
    double ppq = 0.0;
    double bpm = 120.0;
    int64_t last = 0;

    for (int64_t s = 10; s < 3'000'000; s += 10)
    {
        if (s % 2000 == 0)
        {
            ppq += bpm * static_cast<double> (s - last) / (60.0 * 48000.0);
            last = s;
            bpm = 100.0 + static_cast<double> ((s / 2000) % 40);
            f.transport (s, true, ppq, bpm);
        }

        f.marker (s, (s / 10) % 2 == 0 ? CaptureMarkerKind::bypassOn : CaptureMarkerKind::bypassOff);
    }

    f.noteOn (3'000'000, 62);
    f.noteOff (3'010'000, 62);
    f.transport (3'100'000, false, ppq);
    f.advance (3'100'000 + tail);

    CHECK (f.history.getTimeline().getSegments().front().startSample > 0); // evicted in the history
    REQUIRE (f.closed.size() == 1);
    const auto& take = f.closed[0];
    CHECK (take.timeline.getSegments().front().startSample == 0);
    CHECK (take.timeline.getSegments().size() == 1501);
    CHECK (take.timeline.beatsAt (100.0) == Approx (100.0 / 24000.0));
    CHECK (take.noteCount == 2);
}

//==============================================================================
TEST_CASE ("TakeSegmenter: tempo changes and time signatures in a synced take's timeline", "[takesegmenter]")
{
    RecordFeeder f;
    f.transport (0, true, 0.0, 120.0);
    f.noteOn (1000, 60);
    f.transport (96000, true, 4.0, 90.0);       // 2 s at 120 = 4 beats
    // 4 beats at 90 = 128000 samples later the song switches to 3/4 at bar 3 (host bar start = ppq 8)
    TransportInfo threeFour;
    threeFour.isPlaying = true;
    threeFour.hasPpq = true;
    threeFour.ppqPosition = 8.0;
    threeFour.hasBpm = true;
    threeFour.bpm = 90.0;
    threeFour.hasTimeSignature = true;
    threeFour.timeSigNumerator = 3;
    threeFour.timeSigDenominator = 4;
    threeFour.hasBarStart = true;
    threeFour.ppqPositionOfLastBarStart = 8.0;
    f.feed (CaptureRecord::makeTransport (224000, threeFour));
    f.noteOff (300000, 60);
    f.transport (400000, false, 8.0 + 176000.0 * 90.0 / (60.0 * 48000.0), 90.0, 3, 4);
    f.advance (400000 + tail);

    REQUIRE (f.closed.size() == 1);
    const auto& tl = f.closed[0].timeline;
    const auto tempo = tl.tempoMap (0.0, 100.0);
    REQUIRE (tempo.size() == 2);
    CHECK (tempo[0].bpm == 120.0);
    CHECK (tempo[1].beats == Approx (4.0));
    CHECK (tempo[1].bpm == 90.0);
    const auto sig = tl.timeSignatureMap (0.0, 100.0);
    REQUIRE (sig.size() == 2);
    CHECK (sig[1].beats == Approx (8.0));
    CHECK (sig[1].numerator == 3);
    // The 3/4 grid starts at the host bar start (its bar number is approximate after a change, design 6)
    CHECK (tl.barAt (224000.0 + 1000.0).startBeats == Approx (8.0));
    CHECK (tl.barAt (224000.0 + 3.0 * 32000.0 + 10.0).startBeats == Approx (11.0));
    CHECK (tl.barAt (224000.0 + 3.0 * 32000.0 + 10.0).lengthBeats == 3.0);
    CHECK (tl.barAt (224000.0 + 3.0 * 32000.0 + 10.0).number == tl.barAt (224000.0 + 1000.0).number + 1);
    CHECK (tl.sampleAtBeats (6.0) == Approx (96000.0 + 64000.0));
}

TEST_CASE ("TakeSegmenter: no playhead -> free mode", "[takesegmenter]")
{
    SECTION ("no transport at all")
    {
        RecordFeeder f;
        f.noTransport (0);
        f.noteOn (1000, 60);
        REQUIRE (f.segmenter.getOpenTake() != nullptr);
        CHECK (f.segmenter.getOpenTake()->mode == TakeMode::free);
        CHECK (f.segmenter.getOpenTake()->freeBpm == TakeTimeline::fallbackBpm);
        CHECK (f.segmenter.getOpenTake()->timeline.empty());
    }

    SECTION ("\"playing\" without a position")
    {
        RecordFeeder f;
        f.transport (0, true, 0.0, 135.0, 4, 4, false);
        CHECK (f.segmenter.getOpenTake() == nullptr);
        f.noteOn (1000, 60);
        REQUIRE (f.segmenter.getOpenTake() != nullptr);
        CHECK (f.segmenter.getOpenTake()->mode == TakeMode::free);
        CHECK (f.segmenter.getOpenTake()->freeBpm == 135.0);
    }

    SECTION ("stopped host: free take at the host bpm")
    {
        RecordFeeder f;
        f.transport (0, false, 3.0, 95.0);
        f.noteOn (1000, 60);
        REQUIRE (f.segmenter.getOpenTake() != nullptr);
        CHECK (f.segmenter.getOpenTake()->freeBpm == 95.0);
        CHECK (f.segmenter.getOpenTake()->settings == CaptureSettingsInfo {});
    }
}

//==============================================================================
TEST_CASE ("TakeSegmenter: grab N bars (synced)", "[takesegmenter]")
{
    RecordFeeder f;
    f.transport (0, true, 0.0);
    f.noteOn (100000, 60, 88); // held across the grab start
    f.noteOn (200000, 62);
    f.noteOff (210000, 62);
    f.noteOff (300000, 60);
    // Marker at 400000 = ppq 16.67 in bar 5 (ppq 16..20); 2 bars: from bar 4 (ppq 12 = sample 288000)
    f.marker (400000, CaptureMarkerKind::grab, 2, 10.0);
    f.noteOn (410000, 64, 100, 5000); // sung before the marker, arrives after it: in the tail
    f.advance (400000 + tail - 1);
    CHECK (f.closed.empty());
    f.advance (400000 + tail);
    REQUIRE (f.closed.size() == 1);

    const auto& grab = f.closed[0];
    CHECK (grab.kind == TakeKind::grab);
    CHECK (grab.mode == TakeMode::synced);
    CHECK (grab.number == 2); // the open synced take holds 1
    CHECK (grab.startSample == 3 * bar);
    CHECK (grab.endSample == 400000);
    CHECK (grab.captureEndSample == 400000 + tail);
    CHECK (grab.noteCount == 2); // 60 (opened at the start) and 64
    CHECK (notesPaired (grab));
    REQUIRE_FALSE (grab.timeline.empty());
    CHECK (grab.timeline.getSegments().front().startSample == 3 * bar);
    CHECK (grab.timeline.barAt (static_cast<double> (3 * bar)).number == 4);
    // 60 sounds at the grab start: synthetic note-on there with its velocity
    CHECK (grab.events.front().synthetic);
    CHECK (grab.events.front().event == VsEvent::makeNoteOn (0, 1, 60, 88));
    CHECK (grab.events.front().sample == 3 * bar);
    // 64 is still sounding at the capture end: closed there
    CHECK (grab.events.back().synthetic);
    CHECK (grab.events.back().sample == 400000 + tail);
    // The open take continues
    REQUIRE (f.segmenter.getOpenTake() != nullptr);
    CHECK (f.segmenter.getOpenTake()->number == 1);

    SECTION ("1 bar: the current bar as far as it was played")
    {
        f.marker (500000, CaptureMarkerKind::grab, 1, 10.0); // ppq 20.83: bar 6 from 480000
        f.advance (500000 + tail);
        REQUIRE (f.closed.size() == 2);
        const auto& g = f.closed[1];
        CHECK (g.startSample == 5 * bar);
        CHECK (g.number == 3);
        CHECK (g.noteCount == 1); // 64, sounding since 410000
        CHECK (notesPaired (g));
    }

    SECTION ("more bars than played: not before the transport start")
    {
        f.marker (500000, CaptureMarkerKind::grab, 32, 10.0);
        f.advance (500000 + tail);
        REQUIRE (f.closed.size() == 2);
        CHECK (f.closed[1].startSample == 0);
        CHECK (f.closed[1].noteCount == 3);
    }
}

TEST_CASE ("TakeSegmenter: grab N seconds (free); clamped to the history; no notes -> nothing", "[takesegmenter]")
{
    const int64_t detectionTail = 12000; // 0.25 s
    RecordFeeder f;
    f.transport (0, false, 0.0);
    for (int64_t s = 48000; s < 20 * 48000; s += 48000)
    {
        f.noteOn (s, 60);
        f.noteOff (s + 20000, 60);
    }

    // 1 s pauses keep one free take open; the grab is listed independently
    f.marker (20 * 48000, CaptureMarkerKind::grab, 8, 10.0);
    f.advance (20 * 48000 + detectionTail);
    REQUIRE (f.closed.size() == 1);
    const auto grab = f.closed[0]; // a copy: closed grows below
    CHECK (grab.kind == TakeKind::grab);
    CHECK (grab.mode == TakeMode::free);
    CHECK (grab.startSample == 10 * 48000);
    CHECK (grab.endSample == 20 * 48000);
    CHECK (grab.captureEndSample == 20 * 48000 + detectionTail);
    CHECK (grab.noteCount == 10); // the note-ons at 10 .. 19 s (the start is inclusive)
    CHECK (grab.freeBpm == 120.0);
    CHECK (grab.number == 2);

    // 120 s reach back before the oldest record: clamped to it
    f.marker (21 * 48000, CaptureMarkerKind::grab, 8, 120.0);
    f.advance (21 * 48000 + detectionTail);
    REQUIRE (f.closed.size() == 2);
    CHECK (f.closed[1].startSample == f.history.getOldestEventSample());

    // Static makeGrab gives the same take without the capture-end limit
    const auto direct = TakeSegmenter::makeGrab (f.history, 20 * 48000, { CaptureMarkerKind::grab, 8, 10.0 }, 2);
    REQUIRE (direct.has_value());
    INFO (takeDifference (*direct, grab));
    CHECK (takeDifference (*direct, grab).empty());

    // A span without any note-on lists nothing and frees the number
    RecordFeeder g;
    g.transport (0, false, 0.0);
    g.marker (48000, CaptureMarkerKind::grab, 8, 10.0);
    g.advance (48000 + detectionTail);
    CHECK (g.closed.empty());
    CHECK (g.segmenter.getNextTakeNumber() == 1);
    CHECK_FALSE (TakeSegmenter::makeGrab (g.history, 48000, { CaptureMarkerKind::grab, 8, 10.0 }, 1).has_value());
}
