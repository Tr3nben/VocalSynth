// TakeSegmenter (docs/M3-DESIGN.md 5.2, 16). Owner: agent A.

#include "CaptureTestHelpers.h"

#include "vocalsynth/TakeRenderer.h"
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
    CHECK (open.noteCount == 1); // kept up to date while the take is open (the status line shows it)

    // Every note-on that arrives while the take is open counts at once
    f.noteOff (60000, 60);
    f.noteOn (70000, 62);
    REQUIRE (f.segmenter.getOpenTake() != nullptr);
    CHECK (f.segmenter.getOpenTake()->noteCount == 2);
    f.noteOff (80000, 62);
    f.noteOn (90000, 64);
    CHECK (f.segmenter.getOpenTake()->noteCount == 3);
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

//==============================================================================
// M3.1 take length and Record mode (docs/M3.1-DESIGN.md 3, 4)

namespace
{
constexpr int64_t rt = 1024;   // Auto round trip of the RecordFeeder's 512-sample blocks (rtSeg)
constexpr int64_t barTicks = 3840;

CaptureSettingsInfo withLimit (int bars, CaptureMode mode = CaptureMode::background)
{
    CaptureSettingsInfo info;
    info.takeMaxBars = bars;
    info.captureMode = mode;
    return info;
}

CaptureSettingsInfo recordMode() { return withLimit (16, CaptureMode::record); }

/** A note sung at host time [from, to), emitted a round trip later (no detection delay). */
void sing (RecordFeeder& f, int64_t from, int64_t to, int note)
{
    f.noteOn (from + rt, note);
    f.noteOff (to + rt, note);
}

RenderOptions rawWithRoundTrip()
{
    RenderOptions o;
    o.version = ExportVersion::raw;
    o.roundTripSamples = rt;
    return o;
}

struct TickNote
{
    int64_t start = 0;
    int64_t end = 0;
    int pitch = 0;
    bool operator== (const TickNote&) const = default;
};

std::vector<TickNote> tickNotes (const RenderedTake& r)
{
    std::vector<TickNote> notes;
    std::array<int64_t, 128> on {};
    on.fill (-1);

    for (const auto& e : r.events)
    {
        if (e.type == TickEventType::noteOn)
            on[e.data1] = e.tick;
        else if (e.type == TickEventType::noteOff && on[e.data1] >= 0)
        {
            notes.push_back ({ on[e.data1], e.tick, e.data1 });
            on[e.data1] = -1;
        }
    }

    return notes;
}
} // namespace

TEST_CASE ("TakeSegmenter M3.1: a background take is anchored at its first note and cut after N bars",
           "[takesegmenter][takelength]")
{
    RecordFeeder f;
    f.settings (0, withLimit (4));
    f.transport (0, true, 0.0);
    REQUIRE (f.segmenter.getOpenTake() != nullptr);
    CHECK (f.segmenter.getOpenTake()->anchorBar == 0);
    CHECK (f.segmenter.getOpenTakeBar() == 0); // waiting for a note
    CHECK (f.segmenter.getTakeMaxBars() == 4);

    // Sung before Play (host time < 0), emitted after it: ended before the start, never anchors
    f.noteOn (500, 59);
    f.noteOff (900, 59);
    CHECK (f.segmenter.getOpenTake()->anchorBar == 0);

    // The first note sung in bar 3 anchors the take there, once it lasts the ghost threshold (80 ms)
    f.noteOn (2 * bar + 10000 + rt, 60);
    const auto* open = f.segmenter.getOpenTake();
    REQUIRE (open != nullptr);
    CHECK (open->anchorBar == 0);
    f.noteOff (2 * bar + 40000 + rt, 60);
    open = f.segmenter.getOpenTake();
    REQUIRE (open != nullptr);
    CHECK (open->anchorBar == 3);
    CHECK (open->maxBars == 4);
    CHECK (open->startSample == 2 * bar);
    CHECK (f.segmenter.getOpenTakeBar() == 1);
    sing (f, 4 * bar + 1000, 5 * bar + 90000, 62); // ends in the 4th bar of the take (bar 6)

    f.advance (5 * bar + 1);
    CHECK (f.segmenter.getOpenTakeBar() == 4);

    // Cut at the 4th bar line after the start of bar 3 = bar 7, once the stream is past it
    f.advance (6 * bar);
    CHECK (f.segmenter.getOpenTake()->number == 1);
    f.advance (6 * bar + 1);
    REQUIRE (f.segmenter.getOpenTake() != nullptr);
    CHECK (f.segmenter.getOpenTake()->number == 2);
    CHECK (f.segmenter.getOpenTake()->kind == TakeKind::automatic);
    CHECK (f.segmenter.getOpenTake()->startSample == 6 * bar);
    CHECK (f.segmenter.getOpenTake()->anchorBar == 0);
    CHECK (f.segmenter.getOpenTakeBar() == 0);
    CHECK (f.segmenter.getState() == TakeSegmenter::State::synced);

    f.advance (6 * bar + tail);
    REQUIRE (f.closed.size() == 1);
    const auto first = f.closed[0];
    CHECK (first.number == 1);
    CHECK (first.kind == TakeKind::automatic);
    CHECK (first.startSample == 2 * bar);
    CHECK (first.endSample == 6 * bar);
    CHECK (first.captureEndSample == 6 * bar + tail);
    CHECK (first.anchorBar == 3);
    CHECK (first.maxBars == 4);

    const auto r = TakeRenderer::render (first, rawWithRoundTrip());
    CHECK (r.originBar == 3);
    CHECK (r.endTick == 4 * barTicks);
    CHECK (r.noteCount == 2); // the pre-roll note is dropped

    // After a pause the next note opens a take at its own bar (9), not at the cut (bar 7)
    sing (f, 8 * bar + 5000, 8 * bar + 30000, 64);
    REQUIRE (f.segmenter.getOpenTake() != nullptr);
    CHECK (f.segmenter.getOpenTake()->number == 2);
    CHECK (f.segmenter.getOpenTake()->anchorBar == 9);
    CHECK (f.segmenter.getOpenTake()->startSample == 8 * bar);
    f.transport (9 * bar, false, 36.0); // Stop before the cut ends it there
    f.advance (9 * bar + tail);
    REQUIRE (f.closed.size() == 2);
    CHECK (f.closed[1].number == 2);
    CHECK (f.closed[1].startSample == 8 * bar);
    CHECK (f.closed[1].endSample == 9 * bar);
    const auto second = TakeRenderer::render (f.closed[1], rawWithRoundTrip());
    CHECK (second.originBar == 9);
    CHECK (second.endTick == barTicks);
}

TEST_CASE ("TakeSegmenter M3.1: a note held across the cut continues in the next take at bar X + N",
           "[takesegmenter][takelength]")
{
    RecordFeeder f;
    f.settings (0, withLimit (4));
    f.transport (0, true, 0.0);
    f.noteOn (1000 + rt, 60);
    f.advance (4 * bar + 1); // cut at bar 5 while 60 is held
    REQUIRE (f.segmenter.getOpenTake() != nullptr);
    CHECK (f.segmenter.getOpenTake()->number == 2);
    CHECK (f.segmenter.getOpenTake()->anchorBar == 0); // 60 sounds at the start: decided by its note-off

    f.noteOff (4 * bar + 20000 + rt, 60); // sung until after the bar line: anchors take 2 at the cut
    CHECK (f.segmenter.getOpenTake()->anchorBar == 5);
    CHECK (f.segmenter.getOpenTake()->startSample == 4 * bar);
    sing (f, 5 * bar + 100, 5 * bar + 20000, 62);
    f.transport (6 * bar, false, 24.0);
    f.advance (6 * bar + tail);
    REQUIRE (f.closed.size() == 2);

    const auto& a = f.closed[0];
    const auto& b = f.closed[1];
    CHECK (a.startSample == 0);
    CHECK (a.endSample == 4 * bar);
    CHECK (a.anchorBar == 1);
    CHECK (b.startSample == 4 * bar);
    CHECK (b.endSample == 6 * bar);
    CHECK (b.anchorBar == 5);
    CHECK (b.events.front().synthetic);
    CHECK (b.events.front().sample == 4 * bar);

    // Consecutive in the export: take 1 = bars 1..4 with the note clipped at the end (with its note-off),
    // take 2 = from bar 5, the note continues at tick 0
    const auto ra = TakeRenderer::render (a, rawWithRoundTrip());
    const auto rb = TakeRenderer::render (b, rawWithRoundTrip());
    CHECK (ra.originBar == 1);
    CHECK (ra.endTick == 4 * barTicks);
    CHECK (tickNotes (ra) == std::vector<TickNote> { { 40, 4 * barTicks, 60 } });
    CHECK (rb.originBar == 5);
    REQUIRE (tickNotes (rb).size() == 2);
    CHECK (tickNotes (rb)[0] == TickNote { 0, 800, 60 });
}

TEST_CASE ("TakeSegmenter M3.1: anchor candidates at the start of a take", "[takesegmenter][takelength]")
{
    SECTION ("a note that ended before the cut (emitted after it) does not anchor the next take")
    {
        RecordFeeder f;
        f.settings (0, withLimit (4));
        f.transport (0, true, 0.0);
        sing (f, 1000, 20000, 60);
        f.noteOn (4 * bar - 5000 + rt, 62); // sung just before bar 5 ...
        f.advance (4 * bar + 1);            // ... cut at bar 5 ...
        f.noteOff (4 * bar - 524 + rt, 62); // ... ended before it
        REQUIRE (f.segmenter.getOpenTake() != nullptr);
        CHECK (f.segmenter.getOpenTake()->number == 2);
        CHECK (f.segmenter.getOpenTake()->anchorBar == 0);
        sing (f, 6 * bar + 100, 6 * bar + 20000, 64);
        CHECK (f.segmenter.getOpenTake()->anchorBar == 7);
        CHECK (f.segmenter.getOpenTake()->startSample == 6 * bar);
    }

    SECTION ("a note sounding at the start and still on after the capture tail anchors at the start")
    {
        RecordFeeder f;
        f.settings (0, withLimit (4));
        f.transport (0, false, 0.0);
        f.noteOn (90000, 60); // free take
        f.transport (100000, true, 0.0);
        REQUIRE (f.closed.size() == 1);
        CHECK (f.closed[0].mode == TakeMode::free);
        REQUIRE (f.segmenter.getOpenTake() != nullptr);
        const auto minLength = TakeSegmenter::anchorMinNoteSamples (*f.segmenter.getOpenTake());
        CHECK (minLength == 3840); // 80 ms
        f.advance (100000 + tail + minLength - 1);
        CHECK (f.segmenter.getOpenTake()->anchorBar == 0);
        f.advance (100000 + tail + minLength);
        CHECK (f.segmenter.getOpenTake()->anchorBar == 1);
        CHECK (f.segmenter.getOpenTake()->startSample == 100000);
        f.noteOff (100000 + 2 * bar, 60);
        f.advance (100000 + 4 * bar + 1);
        CHECK (f.segmenter.getOpenTake()->startSample == 100000 + 4 * bar); // cut after bars 1..4
    }

    SECTION ("the limit in force when the take is anchored applies; a change applies from the next take")
    {
        RecordFeeder f;
        f.settings (0, withLimit (4));
        f.transport (0, true, 0.0);
        sing (f, 1000, 20000, 60);
        f.settings (bar, withLimit (8));
        CHECK (f.segmenter.getOpenTake()->maxBars == 4);
        f.advance (4 * bar + 1);
        CHECK (f.segmenter.getOpenTake()->number == 2);
        sing (f, 4 * bar + 1000, 4 * bar + 20000, 62);
        CHECK (f.segmenter.getOpenTake()->maxBars == 8);
        f.advance (12 * bar);
        CHECK (f.segmenter.getOpenTake()->number == 2);
        f.advance (12 * bar + 1);
        CHECK (f.segmenter.getOpenTake()->number == 3);
    }
}

TEST_CASE ("TakeSegmenter M3.1: a downbeat sung a little early anchors at its own bar; the export matches the cut",
           "[takesegmenter][takelength]")
{
    SECTION ("25 ms before bar 3 (within 1/8 beat): bar 3, the 8-bar phrase stays in one take")
    {
        RecordFeeder f;
        f.settings (0, withLimit (8));
        f.transport (0, true, 0.0);
        sing (f, 2 * bar - 1200, 2 * bar + 40000, 60);
        REQUIRE (f.segmenter.getOpenTake() != nullptr);
        CHECK (f.segmenter.getOpenTake()->anchorBar == 3);
        CHECK (f.segmenter.getOpenTake()->startSample == 2 * bar);

        for (int64_t k = 1; k < 8; ++k)
            sing (f, 2 * bar + k * bar, 2 * bar + k * bar + 60000, 62); // one note in each of bars 4 .. 10

        f.advance (10 * bar);
        CHECK (f.segmenter.getOpenTake()->number == 1); // the phrase's 8th bar (bar 10) is still in take 1
        f.advance (10 * bar + tail + 1);
        REQUIRE (f.closed.size() == 1);
        CHECK (f.closed[0].startSample == 2 * bar);
        CHECK (f.closed[0].endSample == 10 * bar); // cut at bar 11
        const auto r = TakeRenderer::render (f.closed[0], rawWithRoundTrip());
        CHECK (r.originBar == 3);
        CHECK (r.endTick == 8 * barTicks);
        REQUIRE (tickNotes (r).size() == 8);
        CHECK (tickNotes (r).front().start == 0); // the early note clamps to the downbeat
    }

    SECTION ("73 ms before bar 3 (a pickup, more than 1/8 beat): bar 2")
    {
        RecordFeeder f;
        f.settings (0, withLimit (8));
        f.transport (0, true, 0.0);
        sing (f, 2 * bar - 3500, 2 * bar + 40000, 60);
        CHECK (f.segmenter.getOpenTake()->anchorBar == 2);
        CHECK (f.segmenter.getOpenTake()->startSample == bar);
    }

    SECTION ("onset refinement moving the first note into the next bar keeps the anchor bar as the origin")
    {
        RecordFeeder f;
        f.settings (0, withLimit (4));
        f.transport (0, true, 0.0);
        sing (f, 2 * bar - 3500, 2 * bar + 40000, 60);
        f.transport (5 * bar, false, 20.0);
        f.advance (5 * bar + tail);
        REQUIRE (f.closed.size() == 1);
        auto take = f.closed[0];
        CHECK (take.anchorBar == 2);

        for (auto& e : take.events)
            if (e.event.type == VsEventType::noteOn && ! e.synthetic)
                e.refinedSample = e.estimatedSample + 4000; // host 2 * bar + 500: bar 3

        RenderOptions clean;
        clean.roundTripSamples = rt;
        const auto r = TakeRenderer::render (take, clean);
        CHECK (r.originBar == 2); // where the cut was counted from (file name Bar002)
        CHECK (r.endTick <= 4 * barTicks);
        CHECK (TakeRenderer::computeAudioSpan (take, clean).startSample == bar + rt);
    }
}

TEST_CASE ("TakeSegmenter M3.1: click and noise notes never anchor a take", "[takesegmenter][takelength]")
{
    SECTION ("a click in bar 1 before a phrase from bar 3: anchored at bar 3, cut at bar 11")
    {
        RecordFeeder f;
        f.settings (0, withLimit (8));
        f.transport (0, true, 0.0);
        sing (f, 10000, 11000, 70); // 21 ms: a mouse click
        CHECK (f.segmenter.getOpenTake()->anchorBar == 0);
        f.advance (10000 + bar);
        CHECK (f.segmenter.getOpenTake()->anchorBar == 0);

        for (int64_t k = 0; k < 8; ++k)
            sing (f, 2 * bar + k * bar + 2000, 2 * bar + k * bar + 60000, 60 + static_cast<int> (k % 3));

        CHECK (f.segmenter.getOpenTake()->anchorBar == 3);
        CHECK (f.segmenter.getOpenTake()->startSample == 2 * bar);
        f.advance (10 * bar + 1);
        REQUIRE (f.segmenter.getOpenTake() != nullptr);
        CHECK (f.segmenter.getOpenTake()->number == 2);

        // Clicks after the cut: take 2 holds only them and is never listed; its number is given again
        sing (f, 10 * bar + 30000, 10 * bar + 30800, 70);
        sing (f, 11 * bar + 5000, 11 * bar + 6000, 71);
        CHECK (f.segmenter.getOpenTake()->anchorBar == 0);
        f.transport (12 * bar, false, 48.0);
        f.advance (12 * bar + tail);
        REQUIRE (f.closed.size() == 1);
        CHECK (f.closed[0].number == 1);
        CHECK (f.closed[0].startSample == 2 * bar);
        CHECK (f.closed[0].endSample == 10 * bar);
        CHECK (f.segmenter.getNextTakeNumber() == 2);
        const auto r = TakeRenderer::render (f.closed[0], rawWithRoundTrip());
        CHECK (r.originBar == 3);
        CHECK (r.noteCount == 8); // the click before bar 3 is dropped by the clamp
    }

    SECTION ("a note-on that is still on after the threshold anchors at its note-on")
    {
        RecordFeeder f;
        f.settings (0, withLimit (4));
        f.transport (0, true, 0.0);
        const auto minLength = TakeSegmenter::anchorMinNoteSamples (*f.segmenter.getOpenTake());
        const auto detectionTail = static_cast<int64_t> (std::llround (TakeSegmenter::detectionTailSeconds * 48000.0));
        f.noteOn (bar + 100 + rt, 60);
        f.advance (bar + 100 + rt + minLength + detectionTail - 1);
        CHECK (f.segmenter.getOpenTake()->anchorBar == 0);
        f.advance (bar + 100 + rt + minLength + detectionTail);
        CHECK (f.segmenter.getOpenTake()->anchorBar == 2);
        CHECK (f.segmenter.getOpenTake()->startSample == bar);
    }

    SECTION ("with limit Off a click-only take is still listed (0.3.0)")
    {
        RecordFeeder f;
        f.settings (0, withLimit (0));
        f.transport (0, true, 0.0);
        sing (f, 10000, 11000, 70);
        f.transport (bar, false, 4.0);
        f.advance (bar + tail);
        REQUIRE (f.closed.size() == 1);
        CHECK (f.closed[0].anchorBar == 0);
    }

    SECTION ("free: the cut is counted from the first real note; a click-only free take is not listed")
    {
        RecordFeeder f;
        f.settings (0, withLimit (4));
        f.noTransport (0);
        const auto limit = freeModeBarsToSamples (4, 48000.0, 120.0);
        f.noteOn (48000, 70); // click: opens the free take, does not start the count
        f.noteOff (49000, 70);

        for (int64_t k = 0; k < 8; ++k)
        {
            f.noteOn (96000 + k * 50000, 60);
            f.noteOff (106000 + k * 50000, 60);
        }

        f.advance (48000 + limit + 1);
        CHECK (f.closed.empty());
        f.advance (96000 + limit + 1);
        REQUIRE (f.closed.size() == 1);
        CHECK (f.closed[0].startSample == 48000);
        CHECK (f.closed[0].endSample == 96000 + limit);
        CHECK (f.closed[0].noteCount == 9);

        f.noteOn (1000000, 70);
        f.noteOff (1001000, 70);
        f.advance (1001000 + gap3s + 1);
        CHECK (f.closed.size() == 1);
        CHECK (f.segmenter.getNextTakeNumber() == 2);
    }
}

TEST_CASE ("TakeSegmenter M3.1: a note just overhanging the cut neither anchors nor lists the next take",
           "[takesegmenter][takelength]")
{
    SECTION ("held 20 ms past bar 5, next phrase at bar 7: take 2 starts at bar 7")
    {
        RecordFeeder f;
        f.settings (0, withLimit (4));
        f.transport (0, true, 0.0);
        f.noteOn (1000 + rt, 60);
        f.advance (4 * bar + 1); // cut at bar 5 while 60 sounds
        REQUIRE (f.segmenter.getOpenTake() != nullptr);
        CHECK (f.segmenter.getOpenTake()->number == 2);
        f.noteOff (4 * bar + 960 + rt, 60);
        CHECK (f.segmenter.getOpenTake()->anchorBar == 0);
        sing (f, 6 * bar + 1000, 6 * bar + 30000, 62);
        CHECK (f.segmenter.getOpenTake()->anchorBar == 7);
        CHECK (f.segmenter.getOpenTake()->startSample == 6 * bar);
        f.advance (10 * bar);
        CHECK (f.segmenter.getOpenTake()->number == 2); // cut at bar 11, not bar 9
        f.transport (10 * bar + 1, false, 40.0);
        f.advance (10 * bar + 1 + tail);
        REQUIRE (f.closed.size() == 2);
        CHECK (f.closed[1].number == 2);
        CHECK (f.closed[1].anchorBar == 7);
        const auto r = TakeRenderer::render (f.closed[1], rawWithRoundTrip());
        CHECK (r.originBar == 7);
        CHECK (r.noteCount == 1); // the 20 ms rest of note 60 is dropped
    }

    SECTION ("held 20 ms past the cut, then silence until Stop: no junk take, no lost number")
    {
        RecordFeeder f;
        f.settings (0, withLimit (4));
        f.transport (0, true, 0.0);
        f.noteOn (1000 + rt, 60);
        f.advance (4 * bar + 1);
        f.noteOff (4 * bar + 960 + rt, 60);
        f.transport (5 * bar, false, 20.0);
        f.advance (5 * bar + tail);
        REQUIRE (f.closed.size() == 1);
        CHECK (f.closed[0].number == 1);
        CHECK (f.segmenter.getNextTakeNumber() == 2);
    }

    SECTION ("ended just before the cut, emitted after it, then silence: the next take gets number 2")
    {
        RecordFeeder f;
        f.settings (0, withLimit (4));
        f.transport (0, true, 0.0);
        sing (f, 1000, 20000, 60);
        f.noteOn (3 * bar + rt, 62);
        f.advance (4 * bar + 1);            // cut at bar 5 ...
        f.noteOff (4 * bar - 524 + rt, 62); // ... the note ended before it
        f.transport (5 * bar, false, 20.0);
        f.advance (5 * bar + tail);
        REQUIRE (f.closed.size() == 1);
        CHECK (f.segmenter.getNextTakeNumber() == 2);
        f.transport (6 * bar, true, 24.0);
        sing (f, 6 * bar + 1000, 6 * bar + 30000, 64);
        REQUIRE (f.segmenter.getOpenTake() != nullptr);
        CHECK (f.segmenter.getOpenTake()->number == 2);
    }
}

TEST_CASE ("TakeSegmenter M3.1: limit Off keeps the 0.3.0 Play .. Stop take; New take restarts the count",
           "[takesegmenter][takelength]")
{
    SECTION ("Off")
    {
        RecordFeeder f;
        f.settings (0, withLimit (0));
        f.transport (0, true, 0.0);
        sing (f, 2 * bar + 1000, 2 * bar + 20000, 60);
        sing (f, 24 * bar + 1000, 24 * bar + 20000, 62);
        f.advance (30 * bar);
        CHECK (f.segmenter.getOpenTakeBar() == 0);
        f.transport (30 * bar, false, 120.0);
        f.advance (30 * bar + tail);
        REQUIRE (f.closed.size() == 1);
        CHECK (f.closed[0].startSample == 0);
        CHECK (f.closed[0].endSample == 30 * bar);
        CHECK (f.closed[0].anchorBar == 3); // status line only
        CHECK (f.closed[0].maxBars == 0);
        const auto r = TakeRenderer::render (f.closed[0], rawWithRoundTrip());
        CHECK (r.originBar == 3);
        CHECK (r.endTick == 23 * barTicks); // bars 3 .. 25: ends at the bar line after the last note
    }

    SECTION ("Off, then Max length switched on during the take (review F4): anchored, no limit for it")
    {
        RecordFeeder f;
        f.settings (0, withLimit (0));
        f.transport (0, true, 0.0);
        CHECK (f.segmenter.getOpenTake()->anchorBar == 0); // waiting for a note
        sing (f, 2 * bar + 1000, 2 * bar + 20000, 60);
        REQUIRE (f.segmenter.getOpenTake() != nullptr);
        CHECK (f.segmenter.getOpenTake()->anchorBar == 3); // the status line shows it recording
        f.settings (3 * bar, withLimit (16));
        CHECK (f.segmenter.getTakeMaxBars() == 16);
        sing (f, 4 * bar + 1000, 4 * bar + 20000, 62);
        CHECK (f.segmenter.getOpenTake()->anchorBar == 3);
        CHECK (f.segmenter.getOpenTake()->maxBars == 0);
        CHECK (f.segmenter.getOpenTakeBar() == 0);
        f.advance (30 * bar);
        CHECK (f.segmenter.getOpenTake()->number == 1); // never cut: the limit applies from the next take
    }

    SECTION ("New take")
    {
        RecordFeeder f;
        f.settings (0, withLimit (4));
        f.transport (0, true, 0.0);
        sing (f, 1000, 20000, 60);
        f.marker (2 * bar + 100, CaptureMarkerKind::newTake);
        REQUIRE (f.segmenter.getOpenTake() != nullptr);
        CHECK (f.segmenter.getOpenTake()->kind == TakeKind::manual);
        CHECK (f.segmenter.getOpenTake()->startSample == 2 * bar + 100);
        sing (f, 2 * bar + 5000, 2 * bar + 20000, 62);
        CHECK (f.segmenter.getOpenTake()->anchorBar == 3);
        CHECK (f.segmenter.getOpenTake()->startSample == 2 * bar + 100); // never before the split
        f.advance (6 * bar + 1);                                          // 4 bars from bar 3
        CHECK (f.segmenter.getOpenTake()->number == 3);
        f.advance (6 * bar + tail);
        REQUIRE (f.closed.size() == 2);
        CHECK (f.closed[0].endSample == 2 * bar + 100);
        CHECK (f.closed[1].endSample == 6 * bar);
    }
}

TEST_CASE ("TakeSegmenter M3.1: the cut follows tempo changes and the time signature", "[takesegmenter][takelength]")
{
    SECTION ("120 -> 60 BPM at bar 3")
    {
        RecordFeeder f;
        f.settings (0, withLimit (4));
        f.transport (0, true, 0.0);
        sing (f, 1000, 20000, 60);
        f.transport (2 * bar, true, 8.0, 60.0); // bars 3 and 4 take 192000 samples each
        f.advance (2 * bar + 2 * 192000);
        CHECK (f.segmenter.getOpenTake()->number == 1);
        f.advance (2 * bar + 2 * 192000 + 1);
        CHECK (f.segmenter.getOpenTake()->number == 2);
        CHECK (f.segmenter.getOpenTake()->startSample == 2 * bar + 2 * 192000);
    }

    SECTION ("3/4")
    {
        RecordFeeder f;
        f.settings (0, withLimit (4));
        f.transport (0, true, 0.0, 120.0, 3, 4);
        sing (f, 73000, 90000, 60); // bar 2 (72000 samples per bar)
        CHECK (f.segmenter.getOpenTake()->anchorBar == 2);
        f.advance (5 * 72000 + 1);
        CHECK (f.segmenter.getOpenTake()->number == 2);
        f.advance (5 * 72000 + tail);
        REQUIRE (f.closed.size() == 1);
        CHECK (f.closed[0].startSample == 72000);
        CHECK (f.closed[0].endSample == 5 * 72000);
        const auto r = TakeRenderer::render (f.closed[0], rawWithRoundTrip());
        CHECK (r.originBar == 2);
        CHECK (r.endTick == 3 * 960);
    }
}

TEST_CASE ("TakeSegmenter M3.1: free takes are cut after N bars at the host tempo (else 120 BPM)",
           "[takesegmenter][takelength]")
{
    SECTION ("held note at 100 BPM: continues in the next free take")
    {
        RecordFeeder f;
        f.settings (0, withLimit (4));
        f.transport (0, false, 0.0, 100.0);
        const auto limit = freeModeBarsToSamples (4, 48000.0, 100.0);
        CHECK (limit == 460800);
        f.noteOn (48000, 60);
        REQUIRE (f.segmenter.getOpenTake() != nullptr);
        CHECK (f.segmenter.getOpenTake()->maxBars == 4);
        f.advance (48000 + limit);
        CHECK (f.segmenter.getOpenTake()->number == 1);
        f.advance (48000 + limit + 1);
        REQUIRE (f.closed.size() == 1);
        CHECK (f.closed[0].mode == TakeMode::free);
        CHECK (f.closed[0].endSample == 48000 + limit);
        CHECK (f.closed[0].events.back().synthetic); // the held note is closed at the cut
        CHECK (notesPaired (f.closed[0]));
        REQUIRE (f.segmenter.getOpenTake() != nullptr);
        CHECK (f.segmenter.getOpenTake()->startSample == 48000 + limit);
        CHECK (f.segmenter.getOpenTake()->events.front().synthetic);
        f.noteOff (48000 + limit + 40000, 60);
        f.advance (48000 + limit + 40000 + gap3s);
        REQUIRE (f.closed.size() == 2);
        CHECK (f.closed[1].endSample == 48000 + limit + 40000 + freeTail);
    }

    SECTION ("no playhead: 120 BPM; nothing sounding at the cut -> the next note-on starts a take")
    {
        RecordFeeder f;
        f.settings (0, withLimit (4));
        f.noTransport (0);
        const auto limit = freeModeBarsToSamples (4, 48000.0, 120.0);
        CHECK (limit == 384000);

        for (int64_t k = 0; k < 8; ++k)
        {
            f.noteOn (50000 + k * 50000, 60);
            f.noteOff (60000 + k * 50000, 60);
        }

        f.advance (50000 + limit + 1);
        REQUIRE (f.closed.size() == 1);
        CHECK (f.closed[0].endSample == 50000 + limit);
        CHECK (f.closed[0].noteCount == 8);
        CHECK (f.segmenter.getOpenTake() == nullptr);
        CHECK (f.segmenter.getState() == TakeSegmenter::State::freeWaiting);
        f.noteOn (450000, 62);
        REQUIRE (f.segmenter.getOpenTake() != nullptr);
        CHECK (f.segmenter.getOpenTake()->startSample == 450000);
    }
}

//==============================================================================
TEST_CASE ("TakeSegmenter M3.1: Record armed while playing starts at the next bar line and spans exactly N bars",
           "[takesegmenter][recordmode]")
{
    RecordFeeder f;
    f.settings (0, recordMode());
    f.transport (0, true, 0.0);
    CHECK (f.segmenter.getState() == TakeSegmenter::State::syncedIdle);
    CHECK (f.segmenter.getOpenTake() == nullptr);
    CHECK (f.segmenter.getRecordStatus().phase == RecordStatus::Phase::idle);

    f.marker (bar + 30000, CaptureMarkerKind::recordArm, 2);
    f.advance (bar + 40000);
    auto status = f.segmenter.getRecordStatus();
    CHECK (status.phase == RecordStatus::Phase::countIn);
    CHECK (status.bars == 2);
    CHECK (status.startBar == 3);
    CHECK (status.beatsToStart == Approx (56000.0 / 24000.0));

    // Sung over the start: begins in bar 2, held into bar 3
    f.noteOn (bar + 50000 + rt, 60);
    CHECK (f.segmenter.getOpenTake() == nullptr); // no background take in Record mode
    f.advance (2 * bar);
    CHECK (f.segmenter.getOpenTake() == nullptr);
    f.advance (2 * bar + 1);
    REQUIRE (f.segmenter.getOpenTake() != nullptr);
    CHECK (f.segmenter.getOpenTake()->kind == TakeKind::record);
    CHECK (f.segmenter.getOpenTake()->startSample == 2 * bar);
    CHECK (f.segmenter.getState() == TakeSegmenter::State::synced);
    status = f.segmenter.getRecordStatus();
    CHECK (status.phase == RecordStatus::Phase::recording);
    CHECK (status.barInTake == 1);
    CHECK (status.startBar == 3);
    CHECK (status.takeNumber == 1);

    f.noteOff (2 * bar + 10000 + rt, 60);
    sing (f, 2 * bar + 30000, 2 * bar + 60000, 62);
    f.noteOn (3 * bar + 50000 + rt, 64); // held over the end
    f.advance (3 * bar + 1);
    CHECK (f.segmenter.getRecordStatus().barInTake == 2);
    CHECK (f.segmenter.getOpenTakeBar() == 2);
    f.advance (4 * bar + 1);
    CHECK (f.segmenter.getOpenTake() == nullptr); // ended by itself, one shot
    CHECK (f.segmenter.getState() == TakeSegmenter::State::syncedIdle);
    CHECK (f.segmenter.getRecordStatus().phase == RecordStatus::Phase::idle);
    f.noteOff (4 * bar + 20000 + rt, 64);
    f.advance (4 * bar + tail);
    REQUIRE (f.closed.size() == 1);

    const auto& take = f.closed[0];
    CHECK (take.kind == TakeKind::record);
    CHECK (take.mode == TakeMode::synced);
    CHECK (take.startSample == 2 * bar);
    CHECK (take.endSample == 4 * bar);
    CHECK (take.anchorBar == 3);
    CHECK (take.maxBars == 2);

    for (const auto version : { ExportVersion::raw, ExportVersion::clean })
    {
        auto o = rawWithRoundTrip();
        o.version = version;
        const auto r = TakeRenderer::render (take, o);
        CHECK (r.originBar == 3);
        CHECK (r.endTick == 2 * barTicks);
        // 60 sung before bar 3 starts at X, 64 is clipped at the end with a note-off
        CHECK (tickNotes (r) == std::vector<TickNote> { { 0, 400, 60 }, { 1200, 2400, 62 }, { 5840, 2 * barTicks, 64 } });
        CHECK (r.audio.startSample == 2 * bar + rt);
        CHECK (r.audio.numSamples == 2 * bar);
    }

    // Nothing else is recorded afterwards
    sing (f, 5 * bar, 5 * bar + 20000, 65);
    f.transport (6 * bar, false, 24.0);
    sing (f, 7 * bar, 7 * bar + 20000, 65);
    f.advance (12 * bar);
    CHECK (f.closed.size() == 1);
}

TEST_CASE ("TakeSegmenter M3.1: Record start rules", "[takesegmenter][recordmode]")
{
    SECTION ("a marker exactly on a bar line starts there")
    {
        RecordFeeder f;
        f.settings (0, recordMode());
        f.transport (0, true, 0.0);
        f.marker (2 * bar, CaptureMarkerKind::recordArm, 1);
        f.advance (2 * bar + 1);
        REQUIRE (f.segmenter.getOpenTake() != nullptr);
        CHECK (f.segmenter.getOpenTake()->startSample == 2 * bar);
        CHECK (f.segmenter.getOpenTake()->anchorBar == 3);
    }

    SECTION ("armed while stopped: starts at the bar line where playback starts")
    {
        const double startPpq = GENERATE (0.0, 1.0);
        RecordFeeder f;
        f.settings (0, recordMode());
        f.transport (0, false, startPpq);
        f.marker (10000, CaptureMarkerKind::recordArm, 2);
        f.advance (50000);
        CHECK (f.segmenter.getRecordStatus().phase == RecordStatus::Phase::waitingForPlay);
        CHECK (f.segmenter.getRecordStatus().bars == 2);
        f.noteOn (60000, 60); // no free take in Record mode
        f.noteOff (70000, 60);
        CHECK (f.segmenter.getOpenTake() == nullptr);

        f.transport (100000, true, startPpq);
        const auto expectedStart = startPpq == 0.0 ? 100000 : 100000 + 3 * 24000;
        f.advance (100000 + 1);
        CHECK (f.segmenter.getRecordStatus().phase
               == (startPpq == 0.0 ? RecordStatus::Phase::recording : RecordStatus::Phase::countIn));
        f.advance (expectedStart + 1);
        REQUIRE (f.segmenter.getOpenTake() != nullptr);
        CHECK (f.segmenter.getOpenTake()->startSample == expectedStart);
        CHECK (f.segmenter.getOpenTake()->anchorBar == (startPpq == 0.0 ? 1 : 2));
        f.advance (expectedStart + 2 * bar + 1);
        CHECK (f.segmenter.getOpenTake() == nullptr);
    }

    SECTION ("a jump during the count-in recomputes the start; Stop keeps it armed")
    {
        RecordFeeder f;
        f.settings (0, recordMode());
        f.transport (0, true, 0.0);
        f.marker (48000, CaptureMarkerKind::recordArm, 4);
        f.advance (50000);
        CHECK (f.segmenter.getRecordStatus().startBar == 2);
        f.transport (60000, true, 10.0); // jump to beat 3 of bar 3: the next line is bar 4
        f.advance (60001);
        CHECK (f.segmenter.getRecordStatus().startBar == 4);
        CHECK (f.segmenter.getRecordStatus().beatsToStart == Approx (2.0 - 1.0 / 24000.0));
        f.advance (96001);
        CHECK (f.segmenter.getOpenTake() == nullptr); // the old grid's bar line does not count
        f.transport (100000, false, 10.0 + 40000.0 / 24000.0);
        f.advance (200000);
        CHECK (f.segmenter.getRecordStatus().phase == RecordStatus::Phase::waitingForPlay);
        CHECK (f.segmenter.getOpenTake() == nullptr);
        f.transport (300000, true, 16.0); // bar 5
        f.advance (300001);
        REQUIRE (f.segmenter.getOpenTake() != nullptr);
        CHECK (f.segmenter.getOpenTake()->startSample == 300000);
        CHECK (f.segmenter.getOpenTake()->anchorBar == 5);
    }
}

TEST_CASE ("TakeSegmenter M3.1: Record take Stop, Cancel and mode switches", "[takesegmenter][recordmode]")
{
    SECTION ("Stop before the end shortens the take (kept, whole bars in the export)")
    {
        RecordFeeder f;
        f.settings (0, recordMode());
        f.transport (0, true, 0.0);
        f.marker (0, CaptureMarkerKind::recordArm, 8);
        sing (f, 10000, 30000, 60);
        f.transport (bar + 50000, false, 4.0 + 50000.0 / 24000.0);
        CHECK (f.segmenter.getRecordStatus().phase == RecordStatus::Phase::idle);
        f.advance (bar + 50000 + tail);
        REQUIRE (f.closed.size() == 1);
        CHECK (f.closed[0].kind == TakeKind::record);
        CHECK (f.closed[0].endSample == bar + 50000);
        const auto r = TakeRenderer::render (f.closed[0], rawWithRoundTrip());
        CHECK (r.endTick == 2 * barTicks);
        CHECK (r.audio.numSamples == bar + 50000);
        // One shot: playing again records nothing
        f.transport (5 * bar, true, 8.0);
        sing (f, 5 * bar + 1000, 5 * bar + 9000, 62);
        f.advance (9 * bar);
        CHECK (f.segmenter.getOpenTake() == nullptr);
    }

    SECTION ("Cancel while armed disarms; Cancel while recording discards the take")
    {
        RecordFeeder f;
        f.settings (0, recordMode());
        f.transport (0, true, 0.0);
        f.marker (1000, CaptureMarkerKind::recordArm, 2);
        f.marker (2000, CaptureMarkerKind::recordCancel);
        f.advance (3 * bar);
        CHECK (f.segmenter.getOpenTake() == nullptr);
        CHECK (f.segmenter.getRecordStatus().phase == RecordStatus::Phase::idle);

        f.marker (3 * bar, CaptureMarkerKind::recordArm, 2);
        sing (f, 3 * bar + 1000, 3 * bar + 20000, 60);
        f.advance (3 * bar + 30000);
        REQUIRE (f.segmenter.getOpenTake() != nullptr);
        CHECK (f.segmenter.getOpenTake()->number == 1);
        f.marker (3 * bar + 40000, CaptureMarkerKind::recordCancel);
        CHECK (f.segmenter.getOpenTake() == nullptr);
        CHECK (f.segmenter.getState() == TakeSegmenter::State::syncedIdle);
        f.advance (8 * bar);
        CHECK (f.closed.empty());
        CHECK (f.segmenter.getNextTakeNumber() == 1); // never listed: the number is free again

        // The history still has it for a grab
        f.marker (8 * bar, CaptureMarkerKind::grab, 8);
        f.advance (8 * bar + tail);
        REQUIRE (f.closed.size() == 1);
        CHECK (f.closed[0].kind == TakeKind::grab);
        CHECK (f.closed[0].noteCount == 1);
    }

    SECTION ("Background -> Record closes the background take; Record -> Background ends a Record take there")
    {
        RecordFeeder f;
        f.settings (0, withLimit (16));
        f.transport (0, true, 0.0);
        sing (f, 1000, 20000, 60);
        f.settings (bar + 10000, recordMode());
        CHECK (f.segmenter.getOpenTake() == nullptr);
        CHECK (f.segmenter.getState() == TakeSegmenter::State::syncedIdle);
        CHECK (f.segmenter.getCaptureMode() == CaptureMode::record);
        f.marker (bar + 20000, CaptureMarkerKind::newTake); // disabled in Record mode
        CHECK (f.segmenter.getOpenTake() == nullptr);

        f.marker (bar + 30000, CaptureMarkerKind::recordArm, 4);
        sing (f, 2 * bar + 1000, 2 * bar + 20000, 62);
        f.advance (2 * bar + 30000);
        REQUIRE (f.segmenter.getOpenTake() != nullptr);
        CHECK (f.segmenter.getOpenTake()->kind == TakeKind::record);

        f.settings (3 * bar + 5000, withLimit (16)); // back to Background while recording
        REQUIRE (f.segmenter.getOpenTake() != nullptr);
        CHECK (f.segmenter.getOpenTake()->kind == TakeKind::automatic);
        CHECK (f.segmenter.getOpenTake()->startSample == 3 * bar + 5000);
        CHECK (f.segmenter.getRecordStatus().phase == RecordStatus::Phase::idle);
        sing (f, 3 * bar + 10000, 3 * bar + 20000, 64);
        CHECK (f.segmenter.getOpenTake()->anchorBar == 4);
        f.transport (5 * bar, false, 20.0);
        f.advance (5 * bar + tail);
        REQUIRE (f.closed.size() == 3);
        CHECK (f.closed[0].kind == TakeKind::automatic);
        CHECK (f.closed[0].endSample == bar + 10000);
        CHECK (f.closed[1].kind == TakeKind::record);
        CHECK (f.closed[1].startSample == 2 * bar);
        CHECK (f.closed[1].endSample == 3 * bar + 5000);
        CHECK (TakeRenderer::render (f.closed[1], rawWithRoundTrip()).endTick == 2 * barTicks);
        CHECK (f.closed[2].kind == TakeKind::automatic);
    }

    SECTION ("Record mode while stopped: no free takes; Grab still works")
    {
        RecordFeeder f;
        f.settings (0, recordMode());
        f.transport (0, false, 0.0);

        for (int64_t s = 48000; s < 10 * 48000; s += 48000)
        {
            f.noteOn (s, 60);
            f.noteOff (s + 20000, 60);
        }

        f.advance (20 * 48000);
        CHECK (f.closed.empty());
        CHECK (f.segmenter.getOpenTake() == nullptr);
        f.marker (20 * 48000, CaptureMarkerKind::grab, 8, 30.0);
        f.advance (21 * 48000);
        REQUIRE (f.closed.size() == 1);
        CHECK (f.closed[0].kind == TakeKind::grab);
        CHECK (f.closed[0].noteCount == 9);
    }
}

TEST_CASE ("TakeSegmenter M3.1: take length and Record results do not depend on the advanceTo pattern",
           "[takesegmenter][takelength][recordmode]")
{
    std::vector<CaptureRecord> records;
    const auto tr = [&] (int64_t s, bool playing, double ppq, double bpm = 120.0)
    {
        TransportInfo t;
        t.isPlaying = playing;
        t.hasPpq = true;
        t.ppqPosition = ppq;
        t.hasBpm = true;
        t.bpm = bpm;
        t.hasTimeSignature = true;
        t.hasBarStart = true;
        t.ppqPositionOfLastBarStart = std::floor ((ppq + 1.0e-9) / 4.0) * 4.0;
        records.push_back (CaptureRecord::makeTransport (s, t));
    };
    const auto mk = [&] (int64_t s, CaptureMarkerKind k, int bars = 2)
    { records.push_back (CaptureRecord::makeMarker (s, { k, bars, 10.0 })); };

    records.push_back (CaptureRecord::makeReset (0, { 48000.0, 512, true }));
    records.push_back (CaptureRecord::makeSettings (0, withLimit (4)));
    mk (0, CaptureMarkerKind::captureOn);
    tr (0, true, 0.0);

    std::mt19937 rng (5);

    for (int64_t s = 1000; s < 60 * bar; s += 9000 + static_cast<int64_t> (rng() % 40000))
    {
        const auto note = static_cast<uint8_t> (50 + (rng() % 20));
        const auto length = 3000 + static_cast<int64_t> (rng() % 60000);
        records.push_back (CaptureRecord::makeEvent (s, VsEvent::makeNoteOn (0, 1, note, 100),
                                                     { static_cast<int32_t> (rng() % 3000), EventTimingKind::measured, 0 }));
        records.push_back (CaptureRecord::makeEvent (s + length, VsEvent::makeNoteOff (0, 1, note),
                                                     { static_cast<int32_t> (rng() % 3000), EventTimingKind::measured, 0 }));
    }

    tr (9 * bar + 7, true, 36.0 + 7.0 / 24000.0, 97.0);                // tempo change mid-take
    records.push_back (CaptureRecord::makeSettings (20 * bar + 3, recordMode()));
    mk (21 * bar + 5000, CaptureMarkerKind::recordArm, 3);
    mk (27 * bar, CaptureMarkerKind::recordArm, 8);
    tr (30 * bar + 11, false, 100.0);                                  // Stop shortens the second Record take
    tr (32 * bar, true, 101.0);                                        // a new pass, nothing armed
    mk (33 * bar, CaptureMarkerKind::recordArm, 2);
    mk (35 * bar, CaptureMarkerKind::grab, 2);
    records.push_back (CaptureRecord::makeSettings (40 * bar + 1, withLimit (4)));
    tr (50 * bar, false, 140.0);                                       // free takes with the 4-bar cut

    std::stable_sort (records.begin(), records.end(),
                      [] (const CaptureRecord& a, const CaptureRecord& b) { return a.sample < b.sample; });
    const auto end = records.back().sample + 20 * 48000;

    const auto run = [&] (int pattern)
    {
        CaptureHistory history;
        history.prepare (1.0);
        TakeSegmenter seg;
        std::vector<Take> closed;
        std::mt19937 coin (13);

        for (const auto& r : records)
        {
            if (pattern == 2 || (pattern == 3 && coin() % 3 == 0))
                seg.advanceTo (r.sample, history, closed);

            history.addRecord (r);
            seg.process (r, history, closed);

            if (pattern == 1)
                seg.advanceTo (r.sample, history, closed);
        }

        seg.advanceTo (end, history, closed);
        return closed;
    };

    const auto reference = run (0);
    const auto count = [&reference] (auto predicate)
    { return std::count_if (reference.begin(), reference.end(), predicate); };
    CHECK (count ([] (const Take& t) { return t.kind == TakeKind::record; }) == 3);
    CHECK (count ([] (const Take& t) { return t.kind == TakeKind::grab; }) == 1);
    CHECK (count ([] (const Take& t) { return t.mode == TakeMode::synced && t.kind == TakeKind::automatic && t.maxBars == 4; }) >= 4);
    CHECK (count ([] (const Take& t) { return t.mode == TakeMode::free && t.maxBars == 4; }) >= 2);

    for (const auto& t : reference)
    {
        CAPTURE (t.number, static_cast<int> (t.kind), t.startSample, t.endSample);
        CHECK (notesPaired (t));

        if (t.kind == TakeKind::record && t.endSample != 30 * bar + 11) // not the one Stop shortened
        {
            const auto r = TakeRenderer::render (t, rawWithRoundTrip());
            CHECK (r.endTick == t.maxBars * barTicks); // exactly N bars
        }
    }

    for (int pattern = 1; pattern <= 3; ++pattern)
    {
        CAPTURE (pattern);
        const auto other = run (pattern);
        REQUIRE (other.size() == reference.size());
        for (std::size_t i = 0; i < other.size(); ++i)
            CHECK (takeDifference (other[i], reference[i]).empty());
    }
}
