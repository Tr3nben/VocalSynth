// TakeRenderer (docs/M3-DESIGN.md 9, 16): compensation, origin, ticks, version (Raw / Clean), quantize,
// pairing, bends / CCs, end of track, audio span, onset refinement, golden .mid bytes (with SmfWriter).

#include "vocalsynth/SmfWriter.h"
#include "vocalsynth/TakeRenderer.h"

#include "SmfTestUtils.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numbers>
#include <vector>

using namespace vocalsynth;

namespace
{
constexpr double sr48 = 48000.0;

/** Synced take: transport playing from startSample at startPpq (a bar start), constant tempo. */
Take syncedTake (double sampleRate, double bpm, int numerator = 4, int denominator = 4, int64_t startSample = 0,
                 double startPpq = 0.0, int64_t endSample = 0)
{
    Take t;
    t.number = 1;
    t.mode = TakeMode::synced;
    t.closed = true;
    t.sampleRate = sampleRate;
    t.maxBlockSize = 2048;
    t.startSample = startSample;
    t.endSample = endSample > 0 ? endSample : startSample + static_cast<int64_t> (sampleRate * 600.0);
    t.captureEndSample = t.endSample + 26400;
    t.timeline.reset (sampleRate);

    TransportInfo info;
    info.isPlaying = true;
    info.hasPpq = true;
    info.ppqPosition = startPpq;
    info.hasBpm = true;
    info.bpm = bpm;
    info.hasTimeSignature = true;
    info.timeSigNumerator = numerator;
    info.timeSigDenominator = denominator;
    info.hasBarStart = true;
    const auto barLength = numerator * 4.0 / denominator;
    info.ppqPositionOfLastBarStart = std::floor (startPpq / barLength + 1e-9) * barLength;
    t.timeline.addTransport (startSample, info);
    return t;
}

Take freeTake (double sampleRate, double bpm)
{
    Take t;
    t.number = 2;
    t.mode = TakeMode::free;
    t.closed = true;
    t.sampleRate = sampleRate;
    t.maxBlockSize = 2048;
    t.freeBpm = bpm;
    t.startSample = 0;
    t.endSample = static_cast<int64_t> (sampleRate * 600.0);
    t.captureEndSample = t.endSample;
    return t;
}

/** Host sample of a beat for a take that started playing at sample start / ppq startPpq. */
int64_t sampleAt (double sampleRate, double bpm, double beat, int64_t start = 0, double startPpq = 0.0)
{
    return start + static_cast<int64_t> (std::llround ((beat - startPpq) * 60.0 / bpm * sampleRate));
}

void addEvent (Take& t, int64_t estimated, int32_t delay, const VsEvent& e, uint8_t flags = 0,
               EventTimingKind kind = EventTimingKind::measured, bool synthetic = false)
{
    CapturedEvent ce;
    ce.sample = estimated + delay;
    ce.estimatedSample = estimated;
    ce.event = e;
    ce.timing = { delay, kind, flags };
    ce.synthetic = synthetic;
    t.events.push_back (ce);
}

/** A note with true start / end (capture clock, i.e. host time + round trip) and a detection delay. */
void addNote (Take& t, uint8_t pitch, int64_t start, int64_t end, uint8_t velocity = 100, int32_t delay = 1100,
              uint8_t channel = 1)
{
    addEvent (t, start, delay, VsEvent::makeNoteOn (0, channel, pitch, velocity));
    addEvent (t, end, delay, VsEvent::makeNoteOff (0, channel, pitch));
}

void sortEvents (Take& t)
{
    std::stable_sort (t.events.begin(), t.events.end(), [] (const CapturedEvent& a, const CapturedEvent& b) { return a.sample < b.sample; });
}

RenderOptions raw()
{
    RenderOptions o;
    o.version = ExportVersion::raw;
    return o;
}

struct SimpleNote
{
    int64_t start = 0;
    int64_t end = 0;
    int pitch = 0;

    bool operator== (const SimpleNote&) const = default;
};

std::vector<SimpleNote> pairedNotes (const RenderedTake& r)
{
    std::vector<SimpleNote> out;
    std::array<int64_t, 128> open {};
    open.fill (-1);

    for (const auto& e : r.events)
    {
        if (e.type == TickEventType::noteOn)
        {
            REQUIRE (open[e.data1] < 0); // never overlapping on one pitch
            open[e.data1] = e.tick;
        }
        else if (e.type == TickEventType::noteOff)
        {
            REQUIRE (open[e.data1] >= 0); // every note-off has its note-on
            out.push_back ({ open[e.data1], e.tick, e.data1 });
            open[e.data1] = -1;
        }
    }

    for (const auto o : open)
        REQUIRE (o < 0); // every note-on has its note-off

    std::sort (out.begin(), out.end(), [] (const SimpleNote& a, const SimpleNote& b) { return a.start < b.start; });
    return out;
}

/** Invariants of every rendered take (TakeRenderer class comment, rule 4). */
void checkInvariants (const RenderedTake& r)
{
    for (std::size_t i = 1; i < r.events.size(); ++i)
    {
        const auto& a = r.events[i - 1];
        const auto& b = r.events[i];
        REQUIRE ((a.tick < b.tick || (a.tick == b.tick && static_cast<int> (a.type) <= static_cast<int> (b.type))));
    }

    for (const auto& e : r.events)
    {
        REQUIRE (e.tick >= 0);
        REQUIRE (e.tick <= r.endTick);
    }

    for (const auto& n : pairedNotes (r))
        REQUIRE (n.end - n.start >= 1);

    REQUIRE (! r.tempos.empty());
    REQUIRE (r.tempos.front().tick == 0);
    REQUIRE (! r.timeSignatures.empty());
    REQUIRE (r.timeSignatures.front().tick == 0);
}
} // namespace

//==============================================================================
TEST_CASE ("TakeRenderer: exact ticks at 60 / 97.5 / 120 / 174 BPM and PPQ 96 / 480 / 960", "[takerenderer]")
{
    for (const auto bpm : { 60.0, 97.5, 120.0, 174.0 })
        for (const auto ppq : { 96, 480, 960 })
            for (const auto sampleRate : { 44100.0, 48000.0, 96000.0 })
            {
                INFO ("bpm " << bpm << ", ppq " << ppq << ", sr " << sampleRate);
                auto take = syncedTake (sampleRate, bpm);
                // Notes at known beats (bar 2 starts at beat 4): 4, 4.5, 5.25, 6.75 -> 8
                const double beats[] { 4.0, 4.5, 5.25, 6.75, 8.0 };

                for (int i = 0; i < 4; ++i)
                    addNote (take, static_cast<uint8_t> (60 + i), sampleAt (sampleRate, bpm, beats[i]),
                             sampleAt (sampleRate, bpm, beats[i + 1]) - static_cast<int64_t> (sampleRate * 0.01));

                sortEvents (take);
                auto options = raw();
                options.ticksPerQuarterNote = ppq;
                const auto r = TakeRenderer::render (take, options);
                checkInvariants (r);

                CHECK (r.ticksPerQuarterNote == ppq);
                CHECK (r.originBar == 2);
                CHECK (r.originBeats == 4.0);
                REQUIRE (r.noteCount == 4);
                const auto notes = pairedNotes (r);
                REQUIRE (notes.size() == 4);

                for (int i = 0; i < 4; ++i)
                {
                    const auto expectedStart = std::llround ((beats[i] - 4.0) * ppq);
                    const auto expectedEnd = std::llround ((static_cast<double> (sampleAt (sampleRate, bpm, beats[i + 1]) - static_cast<int64_t> (sampleRate * 0.01)) / sampleRate * bpm / 60.0 - 4.0) * ppq);
                    CHECK (std::abs (notes[static_cast<std::size_t> (i)].start - expectedStart) <= 0); // sample rounding never moves a tick here
                    CHECK (notes[static_cast<std::size_t> (i)].end == expectedEnd);
                    CHECK (notes[static_cast<std::size_t> (i)].pitch == 60 + i);
                }

                REQUIRE (r.tempos.size() == 1);
                CHECK (r.tempos[0].bpm == bpm);
                CHECK (r.endTick == 4 * static_cast<int64_t> (ppq)); // last note-off inside bar 2
            }
}

TEST_CASE ("TakeRenderer: origin = start of the bar of the first (compensated) note; file bar number", "[takerenderer]")
{
    // Transport starts at bar 5 (ppq 16) at sample 1000; first note sung in bar 7 (beat 25.5)
    auto take = syncedTake (sr48, 120.0, 4, 4, 1000, 16.0);
    const auto rt = int64_t { 4096 };
    addNote (take, 62, sampleAt (sr48, 120.0, 25.5, 1000, 16.0) + rt, sampleAt (sr48, 120.0, 26.0, 1000, 16.0) + rt);
    addNote (take, 64, sampleAt (sr48, 120.0, 29.0, 1000, 16.0) + rt, sampleAt (sr48, 120.0, 30.0, 1000, 16.0) + rt);
    sortEvents (take);

    auto options = raw();
    options.roundTripSamples = rt;
    const auto r = TakeRenderer::render (take, options);
    checkInvariants (r);
    CHECK (r.originBar == 7);
    CHECK (r.originBeats == 24.0);
    const auto notes = pairedNotes (r);
    REQUIRE (notes.size() == 2);
    CHECK (notes[0] == SimpleNote { 1440, 1920, 62 }); // beats 25.5 .. 26 of a bar starting at beat 24
    CHECK (notes[1] == SimpleNote { 4800, 5760, 64 });
    CHECK (r.endTick == 7680); // end of bar 8

    // A first note a hair (< 1/32 beat) before the bar line starts that bar and clamps to tick 0
    auto early = syncedTake (sr48, 120.0);
    addNote (early, 60, sampleAt (sr48, 120.0, 8.0 - 1.0 / 64.0), sampleAt (sr48, 120.0, 9.0));
    sortEvents (early);
    const auto e = TakeRenderer::render (early, raw());
    CHECK (e.originBar == 3);
    CHECK (pairedNotes (e).at (0) == SimpleNote { 0, 960, 60 });
}

TEST_CASE ("TakeRenderer: 3/4 and 7/8 bar origins", "[takerenderer]")
{
    {
        auto take = syncedTake (sr48, 90.0, 3, 4);
        addNote (take, 60, sampleAt (sr48, 90.0, 7.0), sampleAt (sr48, 90.0, 8.0)); // bar 3 = beats 6..9
        sortEvents (take);
        const auto r = TakeRenderer::render (take, raw());
        checkInvariants (r);
        CHECK (r.originBar == 3);
        CHECK (r.originBeats == 6.0);
        CHECK (pairedNotes (r).at (0) == SimpleNote { 960, 1920, 60 });
        CHECK (r.timeSignatures == std::vector<TickTimeSignature> { { 0, 3, 4 } });
        CHECK (r.endTick == 2880); // one 3/4 bar
    }
    {
        auto take = syncedTake (sr48, 120.0, 7, 8);
        addNote (take, 60, sampleAt (sr48, 120.0, 8.0), sampleAt (sr48, 120.0, 11.0)); // bar 3 = beats 7..10.5
        sortEvents (take);
        const auto r = TakeRenderer::render (take, raw());
        checkInvariants (r);
        CHECK (r.originBar == 3);
        CHECK (r.originBeats == 7.0);
        CHECK (pairedNotes (r).at (0) == SimpleNote { 960, 3840, 60 });
        CHECK (r.timeSignatures == std::vector<TickTimeSignature> { { 0, 7, 8 } });
        CHECK (r.endTick == 6720); // note-off in the second 7/8 bar (3360 ticks each)
    }
}

TEST_CASE ("TakeRenderer: tempo change inside a take", "[takerenderer]")
{
    auto take = syncedTake (sr48, 120.0);
    TransportInfo slower;
    slower.isPlaying = slower.hasPpq = slower.hasBpm = slower.hasTimeSignature = slower.hasBarStart = true;
    slower.ppqPosition = 8.0;
    slower.ppqPositionOfLastBarStart = 8.0;
    slower.bpm = 60.0;
    take.timeline.addTransport (192000, slower); // beat 8 at 120 BPM

    addNote (take, 60, sampleAt (sr48, 120.0, 4.0), sampleAt (sr48, 120.0, 6.0));
    addNote (take, 62, 192000 + 96000, 192000 + 144000); // beats 10 .. 11 at 60 BPM
    sortEvents (take);

    const auto r = TakeRenderer::render (take, raw());
    checkInvariants (r);
    CHECK (r.originBar == 2);
    const auto notes = pairedNotes (r);
    REQUIRE (notes.size() == 2);
    CHECK (notes[0] == SimpleNote { 0, 1920, 60 });
    CHECK (notes[1] == SimpleNote { 5760, 6720, 62 });
    CHECK (r.tempos == std::vector<TickTempo> { { 0, 120.0 }, { 3840, 60.0 } });
    CHECK (r.endTick == 7680);
}

TEST_CASE ("TakeRenderer: origin after a loop wrap (one take, unwrapped beats)", "[takerenderer]")
{
    // 4-bar loop (16 beats) at 120 BPM: the host jumps back to ppq 0 at sample 384000
    auto take = syncedTake (sr48, 120.0);
    TransportInfo wrap;
    wrap.isPlaying = wrap.hasPpq = wrap.hasBpm = wrap.hasTimeSignature = wrap.hasBarStart = true;
    wrap.ppqPosition = 0.0;
    wrap.ppqPositionOfLastBarStart = 0.0;
    wrap.bpm = 120.0;
    wrap.isLooping = true;
    take.timeline.addTransport (384000, wrap);

    // First note after the wrap at host ppq 1 (bar 1 of the second pass), second at host ppq 6
    addNote (take, 60, 384000 + 24000, 384000 + 48000);
    addNote (take, 62, 384000 + 144000, 384000 + 168000);
    sortEvents (take);
    const auto r = TakeRenderer::render (take, raw());
    checkInvariants (r);
    CHECK (r.originBar == 1);
    CHECK (r.originBeats == 16.0);
    const auto notes = pairedNotes (r);
    REQUIRE (notes.size() == 2);
    CHECK (notes[0] == SimpleNote { 960, 1920, 60 });
    CHECK (notes[1] == SimpleNote { 5760, 6720, 62 });

    // A note before the wrap keeps counting: the second pass continues the ticks
    auto across = take;
    addNote (across, 59, sampleAt (sr48, 120.0, 14.0), sampleAt (sr48, 120.0, 15.0));
    sortEvents (across);
    const auto a = TakeRenderer::render (across, raw());
    checkInvariants (a);
    CHECK (a.originBar == 4); // bar 4 of the first pass (beats 12..16)
    CHECK (pairedNotes (a).at (0) == SimpleNote { 1920, 2880, 59 });
    CHECK (pairedNotes (a).at (1) == SimpleNote { 4800, 5760, 60 });
}

TEST_CASE ("TakeRenderer: free takes: seconds at the host bpm or 120, from the first note-on", "[takerenderer]")
{
    for (const auto bpm : { 100.0, TakeTimeline::fallbackBpm })
    {
        auto take = freeTake (sr48, bpm);
        addNote (take, 60, 50000, 50000 + 24000);  // 0.5 s
        addNote (take, 62, 50000 + 48000, 50000 + 96000);
        sortEvents (take);
        const auto r = TakeRenderer::render (take, raw());
        checkInvariants (r);
        const auto ticksPerSecond = bpm / 60.0 * 960.0;
        const auto notes = pairedNotes (r);
        REQUIRE (notes.size() == 2);
        CHECK (notes[0] == SimpleNote { 0, std::llround (0.5 * ticksPerSecond), 60 });
        CHECK (notes[1] == SimpleNote { std::llround (1.0 * ticksPerSecond), std::llround (2.0 * ticksPerSecond), 62 });
        CHECK (r.originBar == 0);
        CHECK (r.tempos == std::vector<TickTempo> { { 0, bpm } });
        CHECK (r.timeSignatures == std::vector<TickTimeSignature> { { 0, 4, 4 } });
        CHECK (r.endTick == notes[1].end); // free: the last event tick
        CHECK (r.audio == AudioSpan { 50000, take.endSample - 50000 });

        // The round trip never applies to free takes; pre-roll moves tick 0 earlier
        auto options = raw();
        options.roundTripSamples = 4096;
        options.freePreRollMs = 250.0;
        const auto p = TakeRenderer::render (take, options);
        CHECK (pairedNotes (p).at (0).start == std::llround (0.25 * ticksPerSecond));
        CHECK (p.audio.startSample == 50000 - 12000);
        CHECK (TakeRenderer::computeAudioSpan (take, options) == p.audio);
    }
}

TEST_CASE ("TakeRenderer: round trip (Auto 2048 @ 48 kHz = 85.3 ms) moves notes by the expected ticks", "[takerenderer]")
{
    CaptureOffsetSettings automatic;
    auto take = syncedTake (sr48, 120.0);
    CHECK (TakeRenderer::roundTripFor (take, automatic) == 4096);
    CaptureOffsetSettings manual { CaptureOffsetMode::manual, 20.0, 0.0 };
    CHECK (TakeRenderer::roundTripFor (take, manual) == 960);
    CHECK (TakeRenderer::roundTripFor (freeTake (sr48, 120.0), automatic) == 0);

    for (const auto bpm : { 97.5, 120.0, 174.0 })
    {
        auto t = syncedTake (sr48, bpm);
        addNote (t, 60, sampleAt (sr48, bpm, 4.5), sampleAt (sr48, bpm, 5.5));
        addNote (t, 62, sampleAt (sr48, bpm, 6.25), sampleAt (sr48, bpm, 7.0));
        sortEvents (t);
        auto o = raw();
        const auto noOffset = pairedNotes (TakeRenderer::render (t, o));
        o.roundTripSamples = TakeRenderer::roundTripFor (t, automatic);
        const auto r = TakeRenderer::render (t, o);
        const auto shifted = pairedNotes (r);
        const auto expectedDelta = std::llround (4096.0 / sr48 * bpm / 60.0 * 960.0);
        REQUIRE (shifted.size() == 2);
        CHECK (r.originBar == 2);

        for (std::size_t i = 0; i < 2; ++i)
        {
            CHECK (std::abs ((noOffset[i].start - shifted[i].start) - expectedDelta) <= 1);
            CHECK (std::abs ((noOffset[i].end - shifted[i].end) - expectedDelta) <= 1);
        }
    }
}

TEST_CASE ("TakeRenderer: compensated times are clamped into the host-time span", "[takerenderer]")
{
    // Transport from sample 48000 (beat 0) to 48000 + 8 beats; capture tail after it
    const int64_t start = 48000;
    auto take = syncedTake (sr48, 120.0, 4, 4, start, 0.0, start + 192000);
    addNote (take, 50, start - 20000, start - 5000);             // sung before the start: dropped
    addNote (take, 52, start - 10000, start + 12000);            // across the start: cut to tick 0
    addNote (take, 54, start + 96000, start + 120000);           // inside
    addNote (take, 56, start + 180000, start + 200000);          // across the end: cut at the end (beat 8)
    addNote (take, 58, start + 193000, start + 210000);          // after Stop (capture tail): dropped
    sortEvents (take);

    const auto r = TakeRenderer::render (take, raw());
    checkInvariants (r);
    CHECK (r.originBar == 1);
    const auto notes = pairedNotes (r);
    REQUIRE (notes.size() == 3);
    CHECK (notes[0] == SimpleNote { 0, 480, 52 });
    CHECK (notes[1] == SimpleNote { 3840, 4800, 54 });
    CHECK (notes[2] == SimpleNote { 7200, 7680, 56 });
    CHECK (r.endTick == 7680); // the cut note-off lies on the bar line: no extra bar
    CHECK (r.audio == AudioSpan { start, 192000 });
}

TEST_CASE ("TakeRenderer: unpaired note-offs dropped, open notes closed, synthetic notes kept", "[takerenderer]")
{
    auto take = syncedTake (sr48, 120.0, 4, 4, 0, 0.0, 192000);
    addEvent (take, 1000, 0, VsEvent::makeNoteOff (0, 1, 70));                 // capture switched on mid-note
    addEvent (take, 0, 0, VsEvent::makeNoteOn (0, 1, 60, 90), 0, EventTimingKind::none, true); // sounding at the start
    addEvent (take, 24000, 500, VsEvent::makeNoteOff (0, 1, 60));
    addEvent (take, 48000, 500, VsEvent::makeNoteOn (0, 1, 62, 100));         // never closed: ends at endSample
    sortEvents (take);

    const auto r = TakeRenderer::render (take, raw());
    checkInvariants (r);
    const auto notes = pairedNotes (r);
    REQUIRE (notes.size() == 2);
    CHECK (notes[0] == SimpleNote { 0, 960, 60 });      // 24000 samples = 1 beat at 120 BPM
    CHECK (notes[1] == SimpleNote { 1920, 7680, 62 });  // closed at endSample (beat 8)
    CHECK (r.noteCount == 2);
    CHECK (r.endTick == 7680);                          // that note-off lies on the bar line
}

TEST_CASE ("TakeRenderer: the origin uses the refined first note-on for Raw and Clean", "[takerenderer]")
{
    auto take = syncedTake (sr48, 120.0);
    addNote (take, 60, sampleAt (sr48, 120.0, 4.0) + 300, sampleAt (sr48, 120.0, 5.0));
    addNote (take, 62, sampleAt (sr48, 120.0, 6.0), sampleAt (sr48, 120.0, 7.0));
    sortEvents (take);
    take.events[0].refinedSample = sampleAt (sr48, 120.0, 4.0) - 12; // refined a little before the bar line

    auto clean = RenderOptions {};
    const auto c = TakeRenderer::render (take, clean);
    const auto r = TakeRenderer::render (take, raw());
    CHECK (c.originBar == 2);
    CHECK (r.originBar == 2);
    CHECK (c.audio == r.audio);
    CHECK (pairedNotes (c).at (0).start == 0);   // refined, clamped to tick 0
    CHECK (pairedNotes (r).at (0).start == 12);  // Raw: the estimate (300 samples = 12 ticks)
    CHECK (pairedNotes (c).at (1) == pairedNotes (r).at (1));
}

TEST_CASE ("TakeRenderer: end of track", "[takerenderer]")
{
    auto take = syncedTake (sr48, 120.0);
    CHECK (TakeRenderer::render (take, raw()).endTick == 3840); // empty: one bar
    CHECK (TakeRenderer::render (freeTake (sr48, 120.0), raw()).endTick == 1);

    addNote (take, 60, sampleAt (sr48, 120.0, 4.0), sampleAt (sr48, 120.0, 8.0)); // note-off on bar 3's line
    sortEvents (take);
    CHECK (TakeRenderer::render (take, raw()).endTick == 3840);

    auto longer = take;
    addNote (longer, 62, sampleAt (sr48, 120.0, 8.0), sampleAt (sr48, 120.0, 8.5));
    sortEvents (longer);
    CHECK (TakeRenderer::render (longer, raw()).endTick == 7680);
}

//==============================================================================
TEST_CASE ("TakeRenderer: quantize grids incl. triplets x strength 0 / 50 / 100 x ends", "[takerenderer]")
{
    struct Grid
    {
        QuantizeGrid grid;
        int64_t step;
    };

    const Grid grids[] { { QuantizeGrid::quarter, 960 }, { QuantizeGrid::eighth, 480 }, { QuantizeGrid::eighthTriplet, 320 },
                         { QuantizeGrid::sixteenth, 240 }, { QuantizeGrid::sixteenthTriplet, 160 }, { QuantizeGrid::thirtySecond, 120 } };

    for (const auto& g : grids)
        for (const auto strength : { 0.0, 50.0, 100.0 })
            for (const auto ends : { false, true })
            {
                INFO ("grid " << g.step << ", strength " << strength << ", ends " << ends);
                // Note starts at grid line k * step plus an offset of +30 % / -30 % of a step, ends at
                // +40 % of a step after a later line
                auto take = syncedTake (sr48, 120.0);
                const auto tickToSample = [] (double tick) { return static_cast<int64_t> (std::llround (192000.0 + tick * 25.0)); };
                std::vector<std::pair<int64_t, int64_t>> source;

                for (int k = 1; k <= 6; ++k)
                {
                    const auto offset = (k % 2 == 0 ? 0.3 : -0.3) * static_cast<double> (g.step);
                    const auto s = static_cast<double> (k * 4 * g.step) + offset;
                    const auto e = static_cast<double> ((k * 4 + 2) * g.step) + 0.4 * static_cast<double> (g.step);
                    addNote (take, static_cast<uint8_t> (60 + k), tickToSample (s), tickToSample (e));
                    source.emplace_back (std::llround (s), std::llround (e));
                }

                sortEvents (take);
                auto options = raw();
                options.quantize = g.grid;
                options.quantizeStrengthPercent = strength;
                options.quantizeEnds = ends;
                const auto r = TakeRenderer::render (take, options);
                checkInvariants (r);
                const auto notes = pairedNotes (r);
                REQUIRE (notes.size() == source.size());

                for (std::size_t i = 0; i < notes.size(); ++i)
                {
                    const auto [s, e] = source[i];
                    const auto k = static_cast<int64_t> (i + 1);
                    const auto line = k * 4 * g.step;
                    const auto expectedStart = s + std::llround (strength / 100.0 * static_cast<double> (line - s));
                    CHECK (notes[i].start == expectedStart);
                    CHECK (notes[i].end == (ends ? (k * 4 + 2) * g.step : e));
                    CHECK (notes[i].pitch == 61 + static_cast<int> (i));
                }
            }
}

TEST_CASE ("TakeRenderer: quantize keeps order, lengths >= 1 tick and the same-pitch rule", "[takerenderer]")
{
    auto take = syncedTake (sr48, 120.0);
    const auto at = [] (int64_t tick) { return 192000 + tick * 25; };
    addNote (take, 60, at (10), at (40));    // -> 0, ends on: >= half a grid step
    addNote (take, 62, at (100), at (130));  // same grid line as the first (grid 240)
    addNote (take, 60, at (230), at (500));  // same pitch as the first, moved to 240
    addNote (take, 64, at (700), at (705));  // tiny, starts move to 720 (past its end): keeps its length
    sortEvents (take);

    auto options = raw();
    options.quantize = QuantizeGrid::sixteenth;

    {
        const auto r = TakeRenderer::render (take, options);
        checkInvariants (r);
        const auto n = pairedNotes (r);
        REQUIRE (n.size() == 4);
        CHECK (n[0] == SimpleNote { 0, 40, 60 });
        CHECK (n[1] == SimpleNote { 0, 130, 62 });
        CHECK (n[2] == SimpleNote { 240, 500, 60 });
        CHECK (n[3] == SimpleNote { 720, 725, 64 });
    }

    options.quantizeEnds = true;
    {
        const auto r = TakeRenderer::render (take, options);
        checkInvariants (r);
        const auto n = pairedNotes (r);
        REQUIRE (n.size() == 4);
        CHECK (n[0] == SimpleNote { 0, 240, 60 });  // end 40 -> 0 is too short: next line >= half a step
        CHECK (n[1] == SimpleNote { 0, 240, 62 });
        CHECK (n[2] == SimpleNote { 240, 480, 60 });
        CHECK (n[3] == SimpleNote { 720, 960, 64 });
    }

    // Same pitch, both quantized onto one line: merged into one note (no zero-length note)
    auto twice = syncedTake (sr48, 120.0);
    addNote (twice, 60, at (10), at (50));
    addNote (twice, 60, at (60), at (200));
    sortEvents (twice);
    options.quantizeEnds = false;
    const auto merged = pairedNotes (TakeRenderer::render (twice, options));
    REQUIRE (merged.size() == 1);
    CHECK (merged[0] == SimpleNote { 0, 200, 60 });

    // Without quantize, overlapping same-pitch notes (sent one after the other; the second note-on has a
    // larger detection delay, so its estimate lies before the first note's end): the earlier one ends at
    // the later start
    auto overlap = syncedTake (sr48, 120.0);
    addNote (overlap, 60, at (0), at (300), 100, 100);
    addNote (overlap, 60, at (200), at (400), 100, 4000);
    sortEvents (overlap);
    const auto o = pairedNotes (TakeRenderer::render (overlap, raw()));
    REQUIRE (o.size() == 2);
    CHECK (o[0] == SimpleNote { 0, 200, 60 });
    CHECK (o[1] == SimpleNote { 200, 400, 60 });
}

TEST_CASE ("TakeRenderer: free takes are never quantized", "[takerenderer]")
{
    auto take = freeTake (sr48, 120.0);
    addNote (take, 60, 10000, 30000);
    addNote (take, 62, 40000, 50000);
    sortEvents (take);
    auto options = raw();
    const auto plain = TakeRenderer::render (take, options);
    options.quantize = QuantizeGrid::quarter;
    options.quantizeEnds = true;
    CHECK (TakeRenderer::render (take, options) == plain);
}

//==============================================================================
TEST_CASE ("TakeRenderer: Clean removes ghost notes, merges transitions, removes octave strays; Raw unchanged", "[takerenderer]")
{
    const auto ms = [] (double v) { return static_cast<int64_t> (std::llround (v * 48.0)); };
    auto take = freeTake (sr48, 120.0);
    const int64_t t0 = 100000;
    // A (60, 400 ms) - ghost 67 (50 ms, after a 100 ms gap) - B (60, 300 ms)
    addNote (take, 60, t0, t0 + ms (400));
    addNote (take, 67, t0 + ms (500), t0 + ms (550));
    addNote (take, 60, t0 + ms (650), t0 + ms (950));
    // transition: C (64, 300 ms), T (65, 100 ms, legato), D (67, 300 ms, 10 ms gap)
    addNote (take, 64, t0 + ms (1200), t0 + ms (1500));
    addNote (take, 65, t0 + ms (1500), t0 + ms (1600));
    addNote (take, 67, t0 + ms (1610), t0 + ms (1910));
    // octave stray: E (62, 300 ms), S (74, 150 ms), F (62, 300 ms)
    addNote (take, 62, t0 + ms (2200), t0 + ms (2500));
    addNote (take, 74, t0 + ms (2510), t0 + ms (2660));
    addNote (take, 62, t0 + ms (2670), t0 + ms (2970));
    // not a transition (pitch not between its neighbours) and long enough: kept
    addNote (take, 55, t0 + ms (3200), t0 + ms (3500));
    addNote (take, 50, t0 + ms (3500), t0 + ms (3590));
    addNote (take, 57, t0 + ms (3600), t0 + ms (3900));
    sortEvents (take);

    const auto rawNotes = pairedNotes (TakeRenderer::render (take, raw()));
    CHECK (rawNotes.size() == 12);

    const auto cleanRender = TakeRenderer::render (take, RenderOptions {});
    checkInvariants (cleanRender);
    const auto clean = pairedNotes (cleanRender);
    REQUIRE (clean.size() == 9);

    const auto tick = [&] (int64_t sample) { return std::llround (static_cast<double> (sample - t0) / 48000.0 * 2.0 * 960.0); };
    CHECK (clean[0] == SimpleNote { tick (t0), tick (t0 + ms (400)), 60 });
    CHECK (clean[1] == SimpleNote { tick (t0 + ms (650)), tick (t0 + ms (950)), 60 });
    CHECK (clean[2] == SimpleNote { tick (t0 + ms (1200)), tick (t0 + ms (1500)), 64 });
    CHECK (clean[3] == SimpleNote { tick (t0 + ms (1500)), tick (t0 + ms (1910)), 67 }); // took T's time
    CHECK (clean[4] == SimpleNote { tick (t0 + ms (2200)), tick (t0 + ms (2500)), 62 });
    CHECK (clean[5] == SimpleNote { tick (t0 + ms (2670)), tick (t0 + ms (2970)), 62 });
    CHECK (clean[6].pitch == 55);
    CHECK (clean[7].pitch == 50);
    CHECK (clean[8].pitch == 57);

    // The ghost threshold never goes below the take's minNoteLength
    auto strict = take;
    strict.settings.minNoteLengthMs = 120.0f;
    const auto s = pairedNotes (TakeRenderer::render (strict, RenderOptions {}));
    CHECK (std::none_of (s.begin(), s.end(), [] (const SimpleNote& n) { return n.pitch == 50; }));

    // Test-pattern takes are exact: Clean == Raw
    auto pattern = take;
    pattern.settings.testPattern = true;
    CHECK (TakeRenderer::render (pattern, RenderOptions {}) == TakeRenderer::render (pattern, raw()));
}

TEST_CASE ("TakeRenderer: Clean merges a provisional correction into one note from the provisional start", "[takerenderer]")
{
    auto take = freeTake (sr48, 120.0);
    const int32_t delay = 600;
    // Provisional 61 emitted at 10600 (onset 10000), corrected at 11800 to 60 with the provisional delay
    addEvent (take, 10000, delay, VsEvent::makeNoteOn (0, 1, 61, 100), 0, EventTimingKind::measured);
    addEvent (take, 11200, delay, VsEvent::makeNoteOff (0, 1, 61), EventTimingFlags::correction);
    addEvent (take, 11200, delay, VsEvent::makeNoteOn (0, 1, 60, 100), EventTimingFlags::correction);
    addEvent (take, 30000, 900, VsEvent::makeNoteOff (0, 1, 60));
    sortEvents (take);

    const auto r = pairedNotes (TakeRenderer::render (take, raw()));
    REQUIRE (r.size() == 2); // Raw: as sent
    CHECK (r[0].pitch == 61);
    CHECK (r[1].pitch == 60);

    const auto c = pairedNotes (TakeRenderer::render (take, RenderOptions {}));
    REQUIRE (c.size() == 1);
    CHECK (c[0] == SimpleNote { 0, std::llround (20000.0 / 48000.0 * 2.0 * 960.0), 60 });
}

//==============================================================================
TEST_CASE ("TakeRenderer: bends As plugin / Off / On, RPN range, centre after the last note; CCs", "[takerenderer]")
{
    auto take = freeTake (sr48, 120.0);
    take.settings.bendMode = BendMode::trueBend;
    take.settings.bendRangeSemitones = 12;
    // note start bend, note-on, update, note-off, centre
    addEvent (take, 1000, 200, VsEvent::makePitchBend (0, 1, 300));
    addEvent (take, 1000, 200, VsEvent::makeNoteOn (0, 1, 60, 100));
    addEvent (take, 4000, 300, VsEvent::makePitchBend (0, 1, 500));
    addEvent (take, 6000, 300, VsEvent::makeControlChange (0, 1, 1, 64));
    addEvent (take, 10000, 400, VsEvent::makeNoteOff (0, 1, 60));
    addEvent (take, 10000, 400, VsEvent::makePitchBend (0, 1, 0));
    // second note whose last bend is not centred (an unfinished take)
    addEvent (take, 20000, 200, VsEvent::makeNoteOn (0, 1, 62, 100));
    addEvent (take, 22000, 300, VsEvent::makePitchBend (0, 1, -700));
    addEvent (take, 30000, 400, VsEvent::makeNoteOff (0, 1, 62));
    sortEvents (take);

    const auto countType = [] (const RenderedTake& r, TickEventType type)
    { return std::count_if (r.events.begin(), r.events.end(), [type] (const TickEvent& e) { return e.type == type; }); };

    auto options = raw();
    const auto asPlugin = TakeRenderer::render (take, options);
    checkInvariants (asPlugin);
    CHECK (asPlugin.bendRangeSemitones == 12);
    CHECK (countType (asPlugin, TickEventType::pitchBend) == 5); // 4 + the centre after the last note
    CHECK (asPlugin.events.back().type == TickEventType::pitchBend);
    CHECK (asPlugin.events.back().bend == 0);
    CHECK (asPlugin.events.back().tick >= pairedNotes (asPlugin).back().end);
    CHECK (countType (asPlugin, TickEventType::controlChange) == 1);

    // start bend before its note-on on the same tick
    REQUIRE (asPlugin.events.size() >= 2);
    CHECK (asPlugin.events[0].type == TickEventType::pitchBend);
    CHECK (asPlugin.events[0].bend == 300);
    CHECK (asPlugin.events[1].type == TickEventType::noteOn);
    CHECK (asPlugin.events[0].tick == asPlugin.events[1].tick);

    options.bends = BendExport::off;
    const auto off = TakeRenderer::render (take, options);
    CHECK (off.bendRangeSemitones == 0);
    CHECK (countType (off, TickEventType::pitchBend) == 0);

    options.controlChanges = false;
    CHECK (countType (TakeRenderer::render (take, options), TickEventType::controlChange) == 0);

    auto offMode = take;
    offMode.settings.bendMode = BendMode::off;
    options = raw();
    CHECK (TakeRenderer::render (offMode, options).bendRangeSemitones == 0); // As plugin follows the take
    options.bends = BendExport::on;
    CHECK (TakeRenderer::render (offMode, options).bendRangeSemitones == 12);
}

TEST_CASE ("TakeRenderer: bends move with a quantized note start and stay inside the note", "[takerenderer]")
{
    auto take = syncedTake (sr48, 120.0);
    take.settings.bendMode = BendMode::trueBend;
    const auto at = [] (int64_t tick) { return 192000 + tick * 25; };
    addEvent (take, at (100), 0, VsEvent::makePitchBend (0, 1, 100));
    addEvent (take, at (100), 0, VsEvent::makeNoteOn (0, 1, 60, 100));
    addEvent (take, at (150), 0, VsEvent::makePitchBend (0, 1, 200));
    addEvent (take, at (400), 0, VsEvent::makeNoteOff (0, 1, 60));
    addEvent (take, at (400), 0, VsEvent::makePitchBend (0, 1, 0));
    sortEvents (take);

    auto options = raw();
    options.quantize = QuantizeGrid::sixteenth; // start 100 -> 0 (shift -100)
    const auto r = TakeRenderer::render (take, options);
    checkInvariants (r);
    std::vector<std::pair<int64_t, int>> bends;

    for (const auto& e : r.events)
        if (e.type == TickEventType::pitchBend)
            bends.emplace_back (e.tick, e.bend);

    CHECK (bends == std::vector<std::pair<int64_t, int>> { { 0, 100 }, { 50, 200 }, { 400, 0 } });
}

//==============================================================================
TEST_CASE ("TakeRenderer: audio span = WAV sample 0 at tick 0, shifted by the round trip", "[takerenderer]")
{
    auto take = syncedTake (sr48, 120.0, 4, 4, 30000, 0.0, 30000 + 480000);
    const int64_t rt = 4096;
    addNote (take, 60, 30000 + 100000 + rt, 30000 + 140000 + rt); // beat 4.17 -> bar 2 (beat 4 = 96000)
    sortEvents (take);
    auto options = raw();
    options.roundTripSamples = rt;
    const auto r = TakeRenderer::render (take, options);
    CHECK (r.originBar == 2);
    CHECK (r.audio == AudioSpan { 30000 + 96000 + rt, 480000 - 96000 });
    CHECK (TakeRenderer::computeAudioSpan (take, options) == r.audio);

    // The note's tick equals its position in the WAV (at the tempo): 4000 samples after WAV sample 0
    const auto note = pairedNotes (r).at (0);
    CHECK (note.start == std::llround (4000.0 / 48000.0 * 2.0 * 960.0));

    // An origin before the transport start (first note just after it, bar starting earlier) extrapolates
    auto mid = syncedTake (sr48, 120.0, 4, 4, 30000, 2.0); // started at beat 2 of bar 1
    addNote (mid, 60, 30000 + 4800, 30000 + 24000);
    sortEvents (mid);
    const auto m = TakeRenderer::render (mid, raw());
    CHECK (m.originBar == 1);
    CHECK (m.audio.startSample == 30000 - 48000);
    CHECK (pairedNotes (m).at (0).start == 1920 + 192);
}

TEST_CASE ("TakeRenderer: refineNoteOnsets finds the energy onset of the dry audio", "[takerenderer]")
{
    const int64_t audioStart = 10000;
    std::vector<float> audio (48000, 0.0f);
    const int64_t onset = 30000;

    for (int64_t s = onset; s < audioStart + static_cast<int64_t> (audio.size()); ++s)
        audio[static_cast<std::size_t> (s - audioStart)] = static_cast<float> (0.3 * std::sin (2.0 * std::numbers::pi * 220.0 * static_cast<double> (s - onset) / 48000.0));

    auto take = freeTake (sr48, 120.0);
    addEvent (take, onset + 400, 900, VsEvent::makeNoteOn (0, 1, 57, 100), 0, EventTimingKind::model);   // estimate 8 ms late
    addEvent (take, onset - 300, 1500, VsEvent::makeNoteOn (0, 1, 58, 100), 0, EventTimingKind::measured); // estimate early
    addEvent (take, onset + 20000, 900, VsEvent::makeNoteOn (0, 1, 59, 100), 0, EventTimingKind::measured);    // no quiet part
    addEvent (take, onset + 400, 900, VsEvent::makeNoteOn (0, 1, 60, 100), 0, EventTimingKind::model, true); // synthetic
    addEvent (take, onset + 400, 900, VsEvent::makeNoteOn (0, 1, 61, 100), EventTimingFlags::noteChange);
    addEvent (take, onset + 400, 900, VsEvent::makeNoteOn (0, 1, 62, 100), 0, EventTimingKind::none);
    TakeRenderer::refineNoteOnsets (take, audio, audioStart);

    REQUIRE (take.events[0].refinedSample != CapturedEvent::noSample);
    CHECK (std::abs (take.events[0].refinedSample - onset) <= 2);
    REQUIRE (take.events[1].refinedSample != CapturedEvent::noSample);
    CHECK (std::abs (take.events[1].refinedSample - onset) <= 2);
    CHECK (take.events[2].refinedSample == CapturedEvent::noSample);
    CHECK (take.events[3].refinedSample == CapturedEvent::noSample);
    CHECK (take.events[4].refinedSample == CapturedEvent::noSample);
    CHECK (take.events[5].refinedSample == CapturedEvent::noSample);

    // Outside the captured audio: nothing refined
    auto outside = freeTake (sr48, 120.0);
    addEvent (outside, 5000, 900, VsEvent::makeNoteOn (0, 1, 57, 100), 0, EventTimingKind::model);
    TakeRenderer::refineNoteOnsets (outside, audio, audioStart);
    CHECK (outside.events[0].refinedSample == CapturedEvent::noSample);
}

//==============================================================================
TEST_CASE ("TakeRenderer + SmfWriter: golden .mid bytes at several tempos and PPQ", "[takerenderer][smfwriter]")
{
    struct Golden
    {
        double bpm;
        int ppq;
        std::vector<uint8_t> bytes;
    };

    // Computed independently (docs/M3-DESIGN.md 10): name, tempo, 4/4, note 60 at 0..1 beat, note 64 at
    // 1.5 .. 3.25 beats, end-of-track at the end of the bar
    const std::vector<Golden> golden {
        { 120.0, 960, { 0x4D, 0x54, 0x68, 0x64, 0x00, 0x00, 0x00, 0x06, 0x00, 0x01, 0x00, 0x01, 0x03, 0xC0, 0x4D, 0x54, 0x72, 0x6B,
                        0x00, 0x00, 0x00, 0x42, 0x00, 0xFF, 0x03, 0x17, 0x56, 0x6F, 0x63, 0x61, 0x6C, 0x53, 0x79, 0x6E, 0x74, 0x68,
                        0x20, 0x54, 0x61, 0x6B, 0x65, 0x20, 0x31, 0x20, 0x43, 0x6C, 0x65, 0x61, 0x6E, 0x00, 0xFF, 0x51, 0x03, 0x07,
                        0xA1, 0x20, 0x00, 0xFF, 0x58, 0x04, 0x04, 0x02, 0x18, 0x08, 0x00, 0x90, 0x3C, 0x64, 0x87, 0x40, 0x80, 0x3C,
                        0x00, 0x83, 0x60, 0x90, 0x40, 0x50, 0x8D, 0x10, 0x80, 0x40, 0x00, 0x85, 0x50, 0xFF, 0x2F, 0x00 } },
        { 97.5, 480, { 0x4D, 0x54, 0x68, 0x64, 0x00, 0x00, 0x00, 0x06, 0x00, 0x01, 0x00, 0x01, 0x01, 0xE0, 0x4D, 0x54, 0x72, 0x6B,
                       0x00, 0x00, 0x00, 0x42, 0x00, 0xFF, 0x03, 0x17, 0x56, 0x6F, 0x63, 0x61, 0x6C, 0x53, 0x79, 0x6E, 0x74, 0x68,
                       0x20, 0x54, 0x61, 0x6B, 0x65, 0x20, 0x31, 0x20, 0x43, 0x6C, 0x65, 0x61, 0x6E, 0x00, 0xFF, 0x51, 0x03, 0x09,
                       0x63, 0xD9, 0x00, 0xFF, 0x58, 0x04, 0x04, 0x02, 0x18, 0x08, 0x00, 0x90, 0x3C, 0x64, 0x83, 0x60, 0x80, 0x3C,
                       0x00, 0x81, 0x70, 0x90, 0x40, 0x50, 0x86, 0x48, 0x80, 0x40, 0x00, 0x82, 0x68, 0xFF, 0x2F, 0x00 } },
        { 174.0, 96, { 0x4D, 0x54, 0x68, 0x64, 0x00, 0x00, 0x00, 0x06, 0x00, 0x01, 0x00, 0x01, 0x00, 0x60, 0x4D, 0x54, 0x72, 0x6B,
                       0x00, 0x00, 0x00, 0x3F, 0x00, 0xFF, 0x03, 0x17, 0x56, 0x6F, 0x63, 0x61, 0x6C, 0x53, 0x79, 0x6E, 0x74, 0x68,
                       0x20, 0x54, 0x61, 0x6B, 0x65, 0x20, 0x31, 0x20, 0x43, 0x6C, 0x65, 0x61, 0x6E, 0x00, 0xFF, 0x51, 0x03, 0x05,
                       0x42, 0xFC, 0x00, 0xFF, 0x58, 0x04, 0x04, 0x02, 0x18, 0x08, 0x00, 0x90, 0x3C, 0x64, 0x60, 0x80, 0x3C, 0x00,
                       0x30, 0x90, 0x40, 0x50, 0x81, 0x28, 0x80, 0x40, 0x00, 0x48, 0xFF, 0x2F, 0x00 } },
    };

    for (const auto& g : golden)
    {
        INFO ("bpm " << g.bpm << ", ppq " << g.ppq);
        // Sung in bar 2 (beats 4 ..), arriving a round trip (Auto 2048 @ 48 kHz) + detection delay later
        auto take = syncedTake (sr48, g.bpm, 4, 4, 0, 0.0, sampleAt (sr48, g.bpm, 12.0));
        const int64_t rt = 4096;
        addNote (take, 60, sampleAt (sr48, g.bpm, 4.0) + rt, sampleAt (sr48, g.bpm, 5.0) + rt, 100, 1100);
        addNote (take, 64, sampleAt (sr48, g.bpm, 5.5) + rt, sampleAt (sr48, g.bpm, 7.25) + rt, 80, 1500);
        sortEvents (take);

        RenderOptions options; // Clean, quantize off, bends as plugin (off)
        options.ticksPerQuarterNote = g.ppq;
        options.roundTripSamples = TakeRenderer::roundTripFor (take, CaptureOffsetSettings {});
        REQUIRE (options.roundTripSamples == rt);
        const auto rendered = TakeRenderer::render (take, options);
        CHECK (rendered.originBar == 2); // file name: ..._Bar002_...

        SmfOptions smf;
        smf.trackName = "VocalSynth Take 1 Clean";
        const auto bytes = SmfWriter::write (rendered, smf);
        CHECK (bytes == g.bytes);

        // Re-parse round trip
        const auto parsed = smftest::parseSmf (bytes);
        CHECK (parsed.division == g.ppq);
        std::vector<TickEvent> channel;

        for (const auto& e : parsed.events)
            if (! e.isMeta())
                channel.push_back ({ e.tick, e.kind() == 0x90 ? TickEventType::noteOn : TickEventType::noteOff,
                                     static_cast<uint8_t> ((e.status() & 0x0F) + 1), e.bytes[1], e.bytes[2], 0 });

        CHECK (channel == rendered.events);
        CHECK (parsed.endOfTrackTick == rendered.endTick);
    }
}

TEST_CASE ("TakeRenderer: deterministic, and a 10-minute True-bend take renders in < 50 ms", "[takerenderer]")
{
    auto take = syncedTake (sr48, 128.0);
    take.settings.bendMode = BendMode::trueBend;
    int64_t s = 1000;
    uint64_t seed = 1;
    const auto next = [&seed] { seed = seed * 6364136223846793005ull + 1442695040888963407ull; return static_cast<int> ((seed >> 33) % 1000); };

    while (s < static_cast<int64_t> (sr48 * 600.0) - 48000)
    {
        const auto pitch = static_cast<uint8_t> (48 + next() % 24);
        const auto length = 4800 + next() * 20;
        addEvent (take, s, 1000, VsEvent::makePitchBend (0, 1, static_cast<int16_t> (next() - 500)));
        addEvent (take, s, 1000, VsEvent::makeNoteOn (0, 1, pitch, 100));

        for (int64_t b = s + 240; b < s + length; b += 240) // 200 bends per second
            addEvent (take, b, 600, VsEvent::makePitchBend (0, 1, static_cast<int16_t> (next() * 4 - 2000)));

        addEvent (take, s + length, 900, VsEvent::makeNoteOff (0, 1, pitch));
        addEvent (take, s + length, 900, VsEvent::makePitchBend (0, 1, 0));
        s += length + 480 + next();
    }

    sortEvents (take);
    REQUIRE (take.events.size() > 100000);

    RenderOptions options;
    options.quantize = QuantizeGrid::sixteenth;
    options.roundTripSamples = 4096;
    const auto first = TakeRenderer::render (take, options);
    checkInvariants (first);

    auto best = std::chrono::steady_clock::duration::max();

    for (int run = 0; run < 3; ++run)
    {
        const auto t0 = std::chrono::steady_clock::now();
        const auto again = TakeRenderer::render (take, options);
        best = std::min (best, std::chrono::steady_clock::now() - t0);
        CHECK (again == first);
    }

    const auto bestMs = std::chrono::duration<double, std::milli> (best).count();
    INFO ("10-minute render: " << bestMs << " ms, " << take.events.size() << " events");
    CHECK (bestMs < 50.0);
}

TEST_CASE ("TakeRenderer: a note held across an overflow gap stays one note (resync)", "[takerenderer]")
{
    auto take = syncedTake (sr48, 120.0, 4, 4, 0, 0.0, 192000);
    addNote (take, 55, 10000, 20000);
    addEvent (take, 30000, 500, VsEvent::makeNoteOn (0, 1, 60, 100));
    // gap at 60000: the segmenter ends the note (synthetic), the tap re-states it (resync, kind none)
    addEvent (take, 60000, 0, VsEvent::makeNoteOff (0, 1, 60), 0, EventTimingKind::none, true);
    addEvent (take, 60000, 0, VsEvent::makeNoteOn (0, 1, 60, 100), EventTimingFlags::resync, EventTimingKind::none);
    // a resync note-on of a note whose note-on was lost in the gap starts it there
    addEvent (take, 60000, 0, VsEvent::makeNoteOn (0, 1, 55, 100), EventTimingFlags::resync, EventTimingKind::none);
    addEvent (take, 65000, 500, VsEvent::makeNoteOff (0, 1, 55));
    addEvent (take, 70000, 500, VsEvent::makeNoteOff (0, 1, 60));
    // a note that really ended in the gap: synthetic note-off, no resync -> ends at the gap
    addEvent (take, 80000, 500, VsEvent::makeNoteOn (0, 1, 62, 100));
    addEvent (take, 96000, 0, VsEvent::makeNoteOff (0, 1, 62), 0, EventTimingKind::none, true);
    addEvent (take, 120000, 500, VsEvent::makeNoteOn (0, 1, 62, 100));
    addEvent (take, 130000, 500, VsEvent::makeNoteOff (0, 1, 62));
    sortEvents (take);

    for (const auto& options : { raw(), RenderOptions {} })
    {
        const auto r = TakeRenderer::render (take, options);
        checkInvariants (r);
        const auto notes = pairedNotes (r);
        REQUIRE (notes.size() == 5);
        CHECK (notes[0] == SimpleNote { 400, 800, 55 });
        CHECK (notes[1] == SimpleNote { 1200, 2800, 60 }); // one note across the gap at 60000
        CHECK (notes[2] == SimpleNote { 2400, 2600, 55 });
        CHECK (notes[3] == SimpleNote { 3200, 3840, 62 });
        CHECK (notes[4] == SimpleNote { 4800, 5200, 62 });
    }
}
