#include "vocalsynth/TestPatternGenerator.h"
#include "vocalsynth/VsEvent.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <vector>

using namespace vocalsynth;

namespace
{
    /** An event with its absolute sample position (sampleOffset is kept as emitted). */
    struct AbsEvent
    {
        int64_t time = 0;
        VsEvent event {};
    };

    TestPatternSettings makeSettings (TestPattern pattern, int channel = 1, double bpm = 120.0, int ccNumber = 1)
    {
        TestPatternSettings s;
        s.pattern = pattern;
        s.channel = channel;
        s.bpm = bpm;
        s.ccNumber = ccNumber;
        return s;
    }

    /** Drives a generator with blocks and collects every event with its absolute time. Checks that every
        offset is inside its block and that offsets are non-decreasing within each block. */
    class Runner
    {
    public:
        explicit Runner (double sampleRate)
        {
            generator.prepare (sampleRate);
        }

        void run (int64_t totalSamples, int blockSize, const TestPatternSettings& settings)
        {
            int64_t done = 0;

            while (done < totalSamples)
            {
                const auto n = static_cast<int> (std::min<int64_t> (blockSize, totalSamples - done));
                block (n, settings);
                done += n;
            }
        }

        /** Varying block sizes taken cyclically from the list. */
        void runVarying (int64_t totalSamples, const std::vector<int>& sizes, const TestPatternSettings& settings)
        {
            int64_t done = 0;
            std::size_t i = 0;

            while (done < totalSamples)
            {
                const auto n = static_cast<int> (std::min<int64_t> (sizes[i++ % sizes.size()], totalSamples - done));
                block (n, settings);
                done += n;
            }
        }

        void block (int n, const TestPatternSettings& settings)
        {
            auto& buffer = *bufferStorage;
            buffer.clear();
            generator.process (n, settings, buffer);
            // Plain ifs instead of REQUIRE per event: block size 1 runs ~10^6 blocks per test
            if (buffer.full())
                FAIL ("event buffer full at sample " << position);

            int lastOffset = 0;

            for (const auto& e : buffer)
            {
                if (e.sampleOffset < 0 || e.sampleOffset >= n || e.sampleOffset < lastOffset)
                    FAIL ("bad offset " << e.sampleOffset << " in a block of " << n << " at sample " << position);

                lastOffset = e.sampleOffset;
                events.push_back ({ position + e.sampleOffset, e });
            }

            if (n > 0)
                position += n;
        }

        void flush()
        {
            auto& buffer = *bufferStorage;
            buffer.clear();
            generator.flush (0, buffer);

            for (const auto& e : buffer)
                events.push_back ({ position, e });
        }

        TestPatternGenerator generator;
        // Big enough for a whole test run in one block (the reference renders); heap, not stack
        std::unique_ptr<VsEventBuffer<65536>> bufferStorage = std::make_unique<VsEventBuffer<65536>>();
        std::vector<AbsEvent> events;
        int64_t position = 0;
    };

    std::vector<AbsEvent> render (TestPattern pattern, double sampleRate, int64_t totalSamples, int blockSize,
                                  double bpm = 120.0, int channel = 1, int ccNumber = 1)
    {
        Runner r (sampleRate);
        r.run (totalSamples, blockSize, makeSettings (pattern, channel, bpm, ccNumber));
        return r.events;
    }

    std::vector<AbsEvent> filter (const std::vector<AbsEvent>& events, VsEventType type)
    {
        std::vector<AbsEvent> result;

        for (const auto& e : events)
            if (e.event.type == type)
                result.push_back (e);

        return result;
    }

    std::vector<AbsEvent> withoutBend (const std::vector<AbsEvent>& events)
    {
        std::vector<AbsEvent> result;

        for (const auto& e : events)
            if (e.event.type != VsEventType::pitchBend)
                result.push_back (e);

        return result;
    }

    bool sameEvent (const AbsEvent& a, const AbsEvent& b)
    {
        return a.time == b.time && a.event.type == b.event.type && a.event.channel == b.event.channel
            && a.event.data1 == b.event.data1 && a.event.data2 == b.event.data2 && a.event.bend == b.event.bend;
    }

    bool sameEvents (const std::vector<AbsEvent>& a, const std::vector<AbsEvent>& b)
    {
        if (a.size() != b.size())
            return false;

        for (std::size_t i = 0; i < a.size(); ++i)
            if (! sameEvent (a[i], b[i]))
                return false;

        return true;
    }

    /** Checks mono note pairing per channel: a note-on only when no note sounds on that channel, every
        note-off matches the sounding note. Returns the number of notes still sounding at the end. */
    int checkPairing (const std::vector<AbsEvent>& events)
    {
        std::array<int, 17> active {};
        active.fill (-1);

        for (const auto& e : events)
        {
            REQUIRE (e.event.channel >= 1);
            REQUIRE (e.event.channel <= 16);
            auto& slot = active[e.event.channel];

            if (e.event.type == VsEventType::noteOn)
            {
                INFO ("note-on at " << e.time);
                REQUIRE (slot == -1);
                REQUIRE (e.event.data2 > 0);
                slot = e.event.data1;
            }
            else if (e.event.type == VsEventType::noteOff)
            {
                INFO ("note-off at " << e.time);
                REQUIRE (slot == static_cast<int> (e.event.data1));
                slot = -1;
            }
        }

        return static_cast<int> (std::count_if (active.begin(), active.end(), [] (int n) { return n >= 0; }));
    }

    constexpr double sr48 = 48000.0;
    constexpr int64_t beat48 = 24000; // samples per beat at 120 bpm, 48 kHz
} // namespace

// ---------------------------------------------------------------------------------------------------------
// VsEventBuffer / VsEventSink

TEST_CASE ("VsEventBuffer is a fixed-capacity, non-allocating list", "[vsevent]")
{
    VsEventBuffer<4> buffer;
    CHECK (buffer.empty());
    CHECK (buffer.size() == 0);
    CHECK (VsEventBuffer<4>::capacity == 4);

    for (int i = 0; i < 4; ++i)
        CHECK (buffer.push (VsEvent::makeNoteOn (i, 1, static_cast<uint8_t> (60 + i), 100)));

    CHECK (buffer.full());
    CHECK_FALSE (buffer.push (VsEvent::makeNoteOff (9, 1, 99)));
    REQUIRE (buffer.size() == 4);
    CHECK (buffer[3].data1 == 63);
    CHECK (buffer.view().size() == 4);
    CHECK (buffer.view()[0] == VsEvent::makeNoteOn (0, 1, 60, 100));

    int count = 0;
    for (const auto& e : buffer)
        CHECK (e.sampleOffset == count++);
    CHECK (count == 4);

    buffer.clear();
    CHECK (buffer.empty());
    CHECK (buffer.begin() == buffer.end());
}

TEST_CASE ("VsEvent factories fill the right fields", "[vsevent]")
{
    const auto on = VsEvent::makeNoteOn (5, 3, 64, 90);
    CHECK ((on.type == VsEventType::noteOn && on.sampleOffset == 5 && on.channel == 3 && on.data1 == 64 && on.data2 == 90));

    const auto off = VsEvent::makeNoteOff (6, 4, 65);
    CHECK ((off.type == VsEventType::noteOff && off.channel == 4 && off.data1 == 65 && off.data2 == 0));

    const auto pb = VsEvent::makePitchBend (7, 5, -8192);
    CHECK ((pb.type == VsEventType::pitchBend && pb.bend == -8192 && pb.channel == 5));

    const auto cc = VsEvent::makeControlChange (8, 16, 74, 127);
    CHECK ((cc.type == VsEventType::controlChange && cc.data1 == 74 && cc.data2 == 127 && cc.channel == 16));
}

TEST_CASE ("VsEventSink forwards to the buffer and reports a full buffer", "[vsevent]")
{
    VsEventBuffer<2> buffer;
    const VsEventSink sink { buffer };

    CHECK (sink.push (VsEvent::makeNoteOn (0, 1, 60, 100)));
    CHECK (sink.push (VsEvent::makeNoteOff (1, 1, 60)));
    CHECK_FALSE (sink.push (VsEvent::makeNoteOn (2, 1, 62, 100)));
    CHECK (buffer.size() == 2);
}

// ---------------------------------------------------------------------------------------------------------
// Block-size and sample-rate invariance

TEST_CASE ("Absolute event positions do not depend on the block size", "[generator]")
{
    const auto pattern = GENERATE (TestPattern::notes, TestPattern::pitchBend, TestPattern::controlChange, TestPattern::all);
    const auto sampleRate = GENERATE (44100.0, 48000.0);
    const auto bpm = GENERATE (120.0, 137.3);

    // A bit more than one full 'all' cycle (32 beats)
    const auto total = static_cast<int64_t> (std::ceil (34.0 * 60.0 * sampleRate / bpm));
    const auto reference = render (pattern, sampleRate, total, static_cast<int> (total), bpm, 3, 7);
    REQUIRE_FALSE (reference.empty());

    const auto blockSize = GENERATE (1, 7, 64, 480, 512, 4096);
    INFO ("pattern " << static_cast<int> (pattern) << ", sr " << sampleRate << ", bpm " << bpm << ", block " << blockSize);
    CHECK (sameEvents (render (pattern, sampleRate, total, blockSize, bpm, 3, 7), reference));
}

TEST_CASE ("Varying block sizes give the same events as one big block", "[generator]")
{
    const auto pattern = GENERATE (TestPattern::notes, TestPattern::pitchBend, TestPattern::controlChange, TestPattern::all);
    const auto total = 34 * beat48;
    const auto reference = render (pattern, sr48, total, static_cast<int> (total));

    Runner r (sr48);
    r.runVarying (total, { 1, 4096, 7, 3, 480, 512, 1000, 64, 2, 4095 }, makeSettings (pattern));
    CHECK (sameEvents (r.events, reference));
}

TEST_CASE ("Event times in seconds scale with the sample rate", "[generator]")
{
    const auto pattern = GENERATE (TestPattern::notes, TestPattern::pitchBend, TestPattern::controlChange, TestPattern::all);
    const double rates[] = { 44100.0, 48000.0, 96000.0 };

    std::vector<AbsEvent> results[3];

    for (int i = 0; i < 3; ++i)
        results[i] = withoutBend (render (pattern, rates[i], static_cast<int64_t> (34.0 * 0.5 * rates[i]), 512));

    // The bend grid is round(sr / 200) samples, so bend values differ between rates; everything else must match
    for (int i = 1; i < 3; ++i)
    {
        INFO ("pattern " << static_cast<int> (pattern) << ", rate " << rates[i]);
        REQUIRE (results[i].size() == results[0].size());

        for (std::size_t k = 0; k < results[0].size(); ++k)
        {
            const auto& a = results[0][k];
            const auto& b = results[i][k];
            CHECK (a.event.type == b.event.type);
            CHECK (a.event.data1 == b.event.data1);
            CHECK (a.event.data2 == b.event.data2);

            const auto seconds0 = static_cast<double> (a.time) / rates[0];
            const auto seconds1 = static_cast<double> (b.time) / rates[i];
            CHECK (std::abs (seconds0 - seconds1) <= 1.0 / rates[0] + 1e-12);
        }
    }
}

TEST_CASE ("Bend events scale with the sample rate", "[generator]")
{
    // Period 2 beats = 1 s at 120 bpm: extremes at 0.25 s (+8191) and 0.75 s (-8192), for every rate
    for (const auto rate : { 44100.0, 48000.0, 96000.0 })
    {
        const auto bends = filter (render (TestPattern::pitchBend, rate, static_cast<int64_t> (rate), 256), VsEventType::pitchBend);
        REQUIRE_FALSE (bends.empty());

        const auto maxIt = std::max_element (bends.begin(), bends.end(), [] (auto& a, auto& b) { return a.event.bend < b.event.bend; });
        const auto minIt = std::min_element (bends.begin(), bends.end(), [] (auto& a, auto& b) { return a.event.bend < b.event.bend; });
        const auto grid = std::lround (rate / 200.0);

        INFO ("rate " << rate);
        CHECK (std::abs (static_cast<double> (maxIt->time) / rate - 0.25) <= static_cast<double> (grid) / rate);
        CHECK (std::abs (static_cast<double> (minIt->time) / rate - 0.75) <= static_cast<double> (grid) / rate);
        CHECK (maxIt->event.bend > 8100);
        CHECK (minIt->event.bend < -8100);
    }
}

// ---------------------------------------------------------------------------------------------------------
// Notes pattern

TEST_CASE ("Notes: ascending C major scale, eighth notes, gate 75 %, velocity 100", "[generator]")
{
    const auto events = render (TestPattern::notes, sr48, 8 * beat48, 480, 120.0, 5);
    const auto ons = filter (events, VsEventType::noteOn);
    const auto offs = filter (events, VsEventType::noteOff);

    REQUIRE (ons.size() == 16);
    REQUIRE (offs.size() == 16);
    CHECK (checkPairing (events) == 0);

    constexpr int expected[] = { 60, 62, 64, 65, 67, 69, 71, 72 };

    for (std::size_t k = 0; k < ons.size(); ++k)
    {
        INFO ("note " << k);
        CHECK (ons[k].time == static_cast<int64_t> (k) * beat48 / 2);
        CHECK (ons[k].event.data1 == expected[k % 8]);
        CHECK (ons[k].event.data2 == 100);
        CHECK (ons[k].event.channel == 5);
        CHECK (offs[k].time == ons[k].time + 9000); // 0.75 * eighth (12000 samples)
        CHECK (offs[k].event.data1 == ons[k].event.data1);
        CHECK (offs[k].event.channel == 5);
    }

    CHECK (filter (events, VsEventType::pitchBend).empty());
    CHECK (filter (events, VsEventType::controlChange).empty());
}

TEST_CASE ("Notes: currentNote follows the sounding note", "[generator]")
{
    TestPatternGenerator gen;
    gen.prepare (sr48);
    VsEventBuffer<64> buffer;
    const auto s = makeSettings (TestPattern::notes);

    CHECK (gen.currentNote() == -1);
    gen.process (1, s, buffer);
    CHECK (gen.currentNote() == 60);
    gen.process (8999, s, buffer); // up to sample 9000 (exclusive)
    CHECK (gen.currentNote() == 60);
    gen.process (1, s, buffer); // sample 9000: note-off
    CHECK (gen.currentNote() == -1);
    gen.process (3000, s, buffer); // sample 12000: next note-on
    CHECK (gen.currentNote() == 62);
}

// ---------------------------------------------------------------------------------------------------------
// Held patterns

TEST_CASE ("Held note 60 is re-struck every bar, note-off before note-on on the same sample", "[generator]")
{
    const auto pattern = GENERATE (TestPattern::pitchBend, TestPattern::controlChange);
    const auto events = render (pattern, sr48, 16 * beat48 + 1, 333);
    const auto ons = filter (events, VsEventType::noteOn);
    const auto offs = filter (events, VsEventType::noteOff);

    REQUIRE (ons.size() == 5); // beats 0, 4, 8, 12, 16
    REQUIRE (offs.size() == 4);
    CHECK (checkPairing (events) == 1);

    for (std::size_t m = 0; m < ons.size(); ++m)
    {
        CHECK (ons[m].time == static_cast<int64_t> (m) * 4 * beat48);
        CHECK (ons[m].event.data1 == 60);
        CHECK (ons[m].event.data2 == 100);
    }

    for (std::size_t m = 0; m < offs.size(); ++m)
        CHECK (offs[m].time == static_cast<int64_t> (m + 1) * 4 * beat48);

    // Order on the re-strike sample: the note-off directly precedes the note-on
    for (std::size_t i = 0; i < events.size(); ++i)
    {
        if (events[i].event.type == VsEventType::noteOff)
        {
            REQUIRE (i + 1 < events.size());
            CHECK (events[i + 1].event.type == VsEventType::noteOn);
            CHECK (events[i + 1].time == events[i].time);
        }
    }
}

TEST_CASE ("Pitch bend: triangle over 2 beats, full range, rate limited, only on change", "[generator]")
{
    const auto events = render (TestPattern::pitchBend, sr48, 8 * beat48, 512);
    const auto bends = filter (events, VsEventType::pitchBend);
    const int64_t grid = 240; // 48000 / 200

    REQUIRE (bends.size() > 100);
    CHECK (bends.front().time > 0); // the value at the start is 0 == the initial value: nothing is sent

    bool sawMax = false;
    bool sawMin = false;

    for (std::size_t i = 0; i < bends.size(); ++i)
    {
        const auto& b = bends[i];
        CHECK (b.event.bend >= -8192);
        CHECK (b.event.bend <= 8191);
        CHECK (b.time % grid == 0);

        if (i > 0)
        {
            CHECK (b.time - bends[i - 1].time >= grid);
            CHECK (b.event.bend != bends[i - 1].event.bend);
        }

        if (b.time % (2 * beat48) == beat48 / 2)
        {
            CHECK (b.event.bend == 8191);
            sawMax = true;
        }

        if (b.time % (2 * beat48) == 3 * beat48 / 2)
        {
            CHECK (b.event.bend == -8192);
            sawMin = true;
        }
    }

    CHECK (sawMax);
    CHECK (sawMin);

    // Rising at the start: first value positive and small
    CHECK (bends.front().event.bend > 0);
    CHECK (bends.front().event.bend < 1000);
    CHECK (bends.front().time == grid);
}

TEST_CASE ("CC sweep: triangle 0..127 over 4 beats on the configured CC number", "[generator]")
{
    const auto ccNumber = GENERATE (1, 74, 119);
    const auto events = render (TestPattern::controlChange, sr48, 8 * beat48, 64, 120.0, 2, ccNumber);
    const auto ccs = filter (events, VsEventType::controlChange);

    REQUIRE (ccs.size() > 200);

    // First events: note-on 60 then CC value 0, both at sample 0
    REQUIRE (events.size() >= 2);
    CHECK (events[0].event.type == VsEventType::noteOn);
    CHECK (events[0].time == 0);
    CHECK (events[1].event.type == VsEventType::controlChange);
    CHECK (events[1].time == 0);
    CHECK (events[1].event.data2 == 0);

    int maxValue = -1;
    int64_t maxTime = -1;
    int zeroCount = 0;

    for (std::size_t i = 0; i < ccs.size(); ++i)
    {
        const auto& c = ccs[i];
        CHECK (c.event.data1 == ccNumber);
        CHECK (c.event.channel == 2);
        CHECK (c.event.data2 <= 127);

        if (i > 0)
        {
            CHECK (c.event.data2 != ccs[i - 1].event.data2);
            CHECK (std::abs (static_cast<int> (c.event.data2) - static_cast<int> (ccs[i - 1].event.data2)) == 1);
            CHECK (c.time > ccs[i - 1].time);
        }

        if (c.event.data2 > maxValue && c.time < 4 * beat48)
        {
            maxValue = c.event.data2;
            maxTime = c.time;
        }

        if (c.event.data2 == 0)
            ++zeroCount;
    }

    CHECK (maxValue == 127);
    CHECK (std::abs (maxTime - 2 * beat48) < beat48 / 50); // peak near beat 2
    CHECK (zeroCount >= 2);                                 // start and end of each cycle
    CHECK (filter (events, VsEventType::pitchBend).empty());
}

// ---------------------------------------------------------------------------------------------------------
// 'all' cycle

TEST_CASE ("All: 16 beats notes, 8 beats pitch bend, 8 beats CC, repeating", "[generator]")
{
    const auto cycle = 32 * beat48;
    const auto events = render (TestPattern::all, sr48, 2 * cycle + beat48, 480, 120.0, 1, 11);
    CHECK (checkPairing (events) == 0); // the last beat (notes 60, 62) has no sounding note at its end

    for (int c = 0; c < 2; ++c)
    {
        const auto base = c * cycle;
        const auto bendStart = base + 16 * beat48;
        const auto ccStart = base + 24 * beat48;
        const auto end = base + cycle;

        INFO ("cycle " << c);

        std::vector<AbsEvent> noteSeg, bendSeg, ccSeg;

        for (const auto& e : events)
        {
            if (e.time >= base && e.time < bendStart)
                noteSeg.push_back (e);
            else if (e.time >= bendStart && e.time < ccStart)
                bendSeg.push_back (e);
            else if (e.time >= ccStart && e.time < end)
                ccSeg.push_back (e);
        }

        // Notes segment: 32 eighths, scale restarts at the segment start
        const auto ons = filter (noteSeg, VsEventType::noteOn);
        REQUIRE (ons.size() == 32);
        CHECK (ons.front().event.data1 == 60);
        CHECK (ons[7].event.data1 == 72);
        CHECK (ons.back().time == base + 31 * beat48 / 2);
        CHECK (filter (noteSeg, VsEventType::pitchBend).empty());
        CHECK (filter (noteSeg, VsEventType::controlChange).empty());

        // Pitch bend segment: note 60 at its start, re-struck at segment beat 4, bends only here
        const auto bendOns = filter (bendSeg, VsEventType::noteOn);
        REQUIRE (bendOns.size() == 2);
        CHECK (bendOns[0].time == bendStart);
        CHECK (bendOns[1].time == bendStart + 4 * beat48);
        CHECK (bendOns[0].event.data1 == 60);
        CHECK (filter (bendSeg, VsEventType::pitchBend).size() > 50);
        CHECK (filter (bendSeg, VsEventType::controlChange).empty());

        // CC segment
        const auto ccOns = filter (ccSeg, VsEventType::noteOn);
        REQUIRE (ccOns.size() == 2);
        CHECK (ccOns[0].time == ccStart);
        CHECK (ccOns[1].time == ccStart + 4 * beat48);
        const auto ccs = filter (ccSeg, VsEventType::controlChange);
        CHECK (ccs.size() > 200);
        CHECK (filter (ccSeg, VsEventType::pitchBend).size() <= 1); // at most the bend centre at the boundary

        for (const auto& e : ccs)
            CHECK (e.event.data1 == 11);

        // Boundary pitch bend -> CC: note-off 60, (bend centre), note-on 60, all at ccStart, in this order
        std::vector<AbsEvent> atCcStart;
        for (const auto& e : events)
            if (e.time == ccStart)
                atCcStart.push_back (e);

        REQUIRE (atCcStart.size() >= 2);
        CHECK (atCcStart[0].event.type == VsEventType::noteOff);
        std::size_t idx = 1;
        if (atCcStart[idx].event.type == VsEventType::pitchBend)
        {
            CHECK (atCcStart[idx].event.bend == 0);
            ++idx;
        }
        REQUIRE (idx < atCcStart.size());
        CHECK (atCcStart[idx].event.type == VsEventType::noteOn);

        // The last bend value before ccStart was not left off-centre
        const auto allBends = filter (events, VsEventType::pitchBend);
        int lastBendBeforeCc = 0;
        for (const auto& b : allBends)
            if (b.time <= ccStart)
                lastBendBeforeCc = b.event.bend;
        CHECK (lastBendBeforeCc == 0);

        // Boundary CC -> notes: note-off 60 then note-on 60 at the cycle end
        std::vector<AbsEvent> atEnd;
        for (const auto& e : events)
            if (e.time == end)
                atEnd.push_back (e);

        REQUIRE (atEnd.size() == 2);
        CHECK (atEnd[0].event.type == VsEventType::noteOff);
        CHECK (atEnd[1].event.type == VsEventType::noteOn);
        CHECK (atEnd[1].event.data1 == 60);
    }
}

TEST_CASE ("All: notes segment ends without a sounding note before the pitch bend segment", "[generator]")
{
    const auto events = render (TestPattern::all, sr48, 17 * beat48, 4096);
    std::vector<AbsEvent> atBoundary;

    for (const auto& e : events)
        if (e.time == 16 * beat48)
            atBoundary.push_back (e);

    REQUIRE (atBoundary.size() == 1);
    CHECK (atBoundary[0].event.type == VsEventType::noteOn);
    CHECK (atBoundary[0].event.data1 == 60);
}

// ---------------------------------------------------------------------------------------------------------
// Settings changes, flush, off

TEST_CASE ("Pattern change flushes at offset 0 on the old channel and restarts", "[generator]")
{
    Runner r (sr48);
    r.run (6000, 480, makeSettings (TestPattern::notes, 4)); // note 60 sounding (off at 9000)
    REQUIRE (r.generator.currentNote() == 60);
    r.events.clear();

    r.block (480, makeSettings (TestPattern::pitchBend, 4));
    REQUIRE (r.events.size() >= 2);
    CHECK (r.events[0].event == VsEvent::makeNoteOff (0, 4, 60));
    CHECK (r.events[1].event == VsEvent::makeNoteOn (0, 4, 60, 100));

    // The restarted pattern counts from this block: the first re-strike is 4 beats later
    r.run (4 * beat48, 480, makeSettings (TestPattern::pitchBend, 4));
    const auto ons = filter (r.events, VsEventType::noteOn);
    REQUIRE (ons.size() == 2);
    CHECK (ons[1].time == 6000 + 4 * beat48);
}

TEST_CASE ("Channel change flushes on the old channel and restarts on the new one", "[generator]")
{
    Runner r (sr48);
    r.run (30000, 512, makeSettings (TestPattern::notes, 2)); // note 64 sounding (on at 24000, off at 33000)
    REQUIRE (r.generator.currentNote() == 64);
    r.events.clear();

    r.block (512, makeSettings (TestPattern::notes, 9));
    REQUIRE (r.events.size() >= 2);
    CHECK (r.events[0].event == VsEvent::makeNoteOff (0, 2, 64));
    CHECK (r.events[1].event == VsEvent::makeNoteOn (0, 9, 60, 100)); // scale restarts
}

TEST_CASE ("Pattern change during a bend sends the bend centre", "[generator]")
{
    Runner r (sr48);
    r.run (beat48 / 2 + 1, 100, makeSettings (TestPattern::pitchBend, 3)); // bend at maximum
    r.events.clear();

    r.block (100, makeSettings (TestPattern::off, 3));
    REQUIRE (r.events.size() == 2);
    CHECK (r.events[0].event == VsEvent::makeNoteOff (0, 3, 60));
    CHECK (r.events[1].event == VsEvent::makePitchBend (0, 3, 0));
    CHECK (r.generator.currentNote() == -1);

    // Off stays silent
    r.events.clear();
    r.run (10 * beat48, 4096, makeSettings (TestPattern::off, 3));
    CHECK (r.events.empty());
}

TEST_CASE ("CC number change flushes and restarts the sweep", "[generator]")
{
    Runner r (sr48);
    r.run (beat48, 256, makeSettings (TestPattern::controlChange, 1, 120.0, 1));
    r.events.clear();

    r.block (256, makeSettings (TestPattern::controlChange, 1, 120.0, 20));
    REQUIRE (r.events.size() >= 3);
    CHECK (r.events[0].event == VsEvent::makeNoteOff (0, 1, 60));
    CHECK (r.events[1].event == VsEvent::makeNoteOn (0, 1, 60, 100));
    CHECK (r.events[2].event == VsEvent::makeControlChange (0, 1, 20, 0));
}

TEST_CASE ("A bpm change takes effect smoothly without a flush", "[generator]")
{
    Runner r (sr48);
    r.run (6000, 6000, makeSettings (TestPattern::notes, 1, 120.0)); // beat 0.25, note 60 sounding
    REQUIRE (r.generator.currentNote() == 60);

    // Remaining 0.125 beat to the note-off and 0.25 beat to the next note at 60 bpm (48000 samples/beat)
    r.run (30000, 64, makeSettings (TestPattern::notes, 1, 60.0));

    const auto offs = filter (r.events, VsEventType::noteOff);
    const auto ons = filter (r.events, VsEventType::noteOn);
    REQUIRE (offs.size() >= 1);
    REQUIRE (ons.size() >= 2);
    CHECK (offs[0].time == 6000 + 6000);
    CHECK (ons[1].time == 6000 + 12000);
    CHECK (ons[1].event.data1 == 62);
    CHECK (checkPairing (r.events) <= 1);
}

TEST_CASE ("Tempo automation every block keeps notes paired and in order", "[generator]")
{
    Runner r (sr48);

    for (int i = 0; i < 2000; ++i)
    {
        const auto bpm = 60.0 + 140.0 * (0.5 + 0.5 * std::sin (i * 0.01));
        r.block (480, makeSettings (TestPattern::all, 1, bpm));
    }

    r.flush();
    CHECK (checkPairing (r.events) == 0);

    for (std::size_t i = 1; i < r.events.size(); ++i)
        CHECK (r.events[i].time >= r.events[i - 1].time);
}

TEST_CASE ("Off emits nothing, and flush with nothing active emits nothing", "[generator]")
{
    Runner r (sr48);
    r.run (5 * beat48, 512, makeSettings (TestPattern::off));
    CHECK (r.events.empty());
    r.flush();
    CHECK (r.events.empty());

    TestPatternGenerator fresh;
    fresh.prepare (sr48);
    VsEventBuffer<8> buffer;
    fresh.flush (0, buffer);
    CHECK (buffer.empty());
    CHECK (fresh.currentNote() == -1);
}

TEST_CASE ("Flush ends every pattern cleanly", "[generator]")
{
    const auto pattern = GENERATE (TestPattern::notes, TestPattern::pitchBend, TestPattern::controlChange, TestPattern::all);
    const auto length = GENERATE (int64_t { 1 }, int64_t { 5000 }, int64_t { 30001 }, int64_t { 17 * 24000 + 12345 });

    Runner r (sr48);
    r.run (length, 480, makeSettings (pattern, 6));
    r.flush();

    CHECK (checkPairing (r.events) == 0);
    CHECK (r.generator.currentNote() == -1);

    int bend = 0;
    for (const auto& e : r.events)
        if (e.event.type == VsEventType::pitchBend)
            bend = e.event.bend;
    CHECK (bend == 0);

    // A second flush has nothing left to do
    r.events.clear();
    r.flush();
    CHECK (r.events.empty());
}

TEST_CASE ("flush() places its events at the given offset and does not restart", "[generator]")
{
    TestPatternGenerator gen;
    gen.prepare (sr48);
    VsEventBuffer<64> buffer;
    const auto s = makeSettings (TestPattern::notes);

    gen.process (100, s, buffer);
    buffer.clear();
    gen.flush (42, buffer);
    REQUIRE (buffer.size() == 1);
    CHECK (buffer[0] == VsEvent::makeNoteOff (42, 1, 60));

    // The pattern continues: the next note (62) comes at sample 12000, no note-off for the flushed note
    buffer.clear();
    gen.process (11900, s, buffer);
    CHECK (buffer.empty());
    gen.process (1, s, buffer);
    REQUIRE (buffer.size() == 1);
    CHECK (buffer[0] == VsEvent::makeNoteOn (0, 1, 62, 100));
}

TEST_CASE ("reset() and prepare() restart the pattern without emitting anything", "[generator]")
{
    TestPatternGenerator gen;
    gen.prepare (sr48);
    VsEventBuffer<64> buffer;
    const auto s = makeSettings (TestPattern::notes);

    gen.process (30000, s, buffer); // up to note 64
    gen.reset();
    CHECK (gen.currentNote() == -1);

    buffer.clear();
    gen.process (10, s, buffer);
    REQUIRE (buffer.size() == 1); // no flush after reset: only the restart's first note
    CHECK (buffer[0] == VsEvent::makeNoteOn (0, 1, 60, 100));

    gen.prepare (96000.0);
    buffer.clear();
    gen.process (24000, s, buffer); // 0.25 s
    REQUIRE (buffer.size() == 2);
    CHECK (buffer[0] == VsEvent::makeNoteOn (0, 1, 60, 100));
    CHECK (buffer[1] == VsEvent::makeNoteOff (18000, 1, 60)); // 0.1875 s at 96 kHz
}

TEST_CASE ("Zero or negative block sizes emit nothing and do not advance the clock", "[generator]")
{
    TestPatternGenerator gen;
    gen.prepare (sr48);
    VsEventBuffer<64> buffer;
    const auto s = makeSettings (TestPattern::notes);

    gen.process (0, s, buffer);
    gen.process (-5, s, buffer);
    CHECK (buffer.empty());

    gen.process (12001, s, buffer);
    REQUIRE (buffer.size() == 3);
    CHECK (buffer[0] == VsEvent::makeNoteOn (0, 1, 60, 100));
    CHECK (buffer[1] == VsEvent::makeNoteOff (9000, 1, 60));
    CHECK (buffer[2] == VsEvent::makeNoteOn (12000, 1, 62, 100));

    // A 0-sample block with changed settings does not flush either
    buffer.clear();
    gen.process (0, makeSettings (TestPattern::off), buffer);
    CHECK (buffer.empty());
    CHECK (gen.currentNote() == 62);
}

TEST_CASE ("Out-of-range settings are clamped", "[generator]")
{
    SECTION ("channel")
    {
        auto events = render (TestPattern::notes, sr48, 100, 100, 120.0, 0);
        REQUIRE_FALSE (events.empty());
        CHECK (events[0].event.channel == 1);

        events = render (TestPattern::notes, sr48, 100, 100, 120.0, 40);
        REQUIRE_FALSE (events.empty());
        CHECK (events[0].event.channel == 16);
    }

    SECTION ("CC number")
    {
        auto events = filter (render (TestPattern::controlChange, sr48, 100, 100, 120.0, 1, 0), VsEventType::controlChange);
        REQUIRE_FALSE (events.empty());
        CHECK (events[0].event.data1 == 1);

        events = filter (render (TestPattern::controlChange, sr48, 100, 100, 120.0, 1, 127), VsEventType::controlChange);
        REQUIRE_FALSE (events.empty());
        CHECK (events[0].event.data1 == 119);
    }

    SECTION ("bpm")
    {
        // 1000 bpm is clamped to 400: eighth note = 3600 samples at 48 kHz
        auto ons = filter (render (TestPattern::notes, sr48, 4000, 512, 1000.0), VsEventType::noteOn);
        REQUIRE (ons.size() == 2);
        CHECK (ons[1].time == 3600);

        // 1 bpm is clamped to 20: eighth note = 72000 samples
        ons = filter (render (TestPattern::notes, sr48, 72001, 4096, 1.0), VsEventType::noteOn);
        REQUIRE (ons.size() == 2);
        CHECK (ons[1].time == 72000);

        // NaN falls back to 120
        ons = filter (render (TestPattern::notes, sr48, 12001, 4096, std::nan ("")), VsEventType::noteOn);
        REQUIRE (ons.size() == 2);
        CHECK (ons[1].time == 12000);
    }

    SECTION ("invalid pattern value is treated as off")
    {
        CHECK (render (static_cast<TestPattern> (42), sr48, 48000, 512).empty());
    }
}

TEST_CASE ("Worst-case event count per 4096 samples stays far below the buffer capacity", "[generator]")
{
    const auto pattern = GENERATE (TestPattern::notes, TestPattern::pitchBend, TestPattern::controlChange, TestPattern::all);

    TestPatternGenerator gen;
    gen.prepare (8000.0);
    VsEventBuffer<1024> buffer;
    std::size_t maxEvents = 0;

    for (int i = 0; i < 100; ++i)
    {
        buffer.clear();
        gen.process (4096, makeSettings (pattern, 1, 400.0), buffer);
        maxEvents = std::max (maxEvents, buffer.size());
    }

    INFO ("pattern " << static_cast<int> (pattern) << ", max events " << maxEvents);
    CHECK (maxEvents < 300);
}
