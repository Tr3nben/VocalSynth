// TakeTimeline (docs/M3-DESIGN.md 6, 16). Owner: agent A.

#include "vocalsynth/TakeTimeline.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cmath>

using namespace vocalsynth;
using Catch::Approx;

namespace
{
TransportInfo playingAt (double ppq, double bpm, int num = 4, int den = 4, bool playing = true)
{
    TransportInfo t;
    t.isPlaying = playing;
    t.hasPpq = true;
    t.ppqPosition = ppq;
    t.hasBpm = true;
    t.bpm = bpm;
    t.hasTimeSignature = true;
    t.timeSigNumerator = num;
    t.timeSigDenominator = den;
    const auto bar = num * 4.0 / den;
    t.hasBarStart = true;
    t.ppqPositionOfLastBarStart = std::floor ((ppq + 1e-9) / bar) * bar;
    return t;
}

TransportInfo stoppedAt (double ppq, double bpm = 120.0) { return playingAt (ppq, bpm, 4, 4, false); }
} // namespace

TEST_CASE ("TakeTimeline: exact beats at constant tempo", "[taketimeline]")
{
    const double sr = GENERATE (44100.0, 48000.0, 96000.0);
    const double bpm = GENERATE (60.0, 97.5, 120.0, 174.0);

    TakeTimeline tl;
    tl.reset (sr);
    tl.addTransport (1000, playingAt (8.0, bpm));

    CHECK (tl.beatsAt (1000.0) == 8.0);
    const auto oneMinute = 1000.0 + 60.0 * sr;
    CHECK (tl.beatsAt (oneMinute) == Approx (8.0 + bpm).epsilon (1e-12));
    CHECK (tl.sampleAtBeats (8.0 + bpm) == Approx (oneMinute).epsilon (1e-12));
    CHECK (tl.bpmAt (5000.0) == bpm);
    CHECK (tl.isPlayingAt (5000.0));

    // Backward extrapolation of a playing first segment (bar origin before the transport start)
    CHECK (tl.beatsAt (1000.0 - sr) == Approx (8.0 - bpm / 60.0));
    CHECK (tl.sampleAtBeats (8.0 - bpm / 60.0) == Approx (1000.0 - sr));
}

TEST_CASE ("TakeTimeline: empty timeline counts seconds at the fallback tempo", "[taketimeline]")
{
    TakeTimeline tl;
    tl.reset (48000.0);
    CHECK (tl.empty());
    CHECK (tl.segmentAt (0.0) == nullptr);
    CHECK (tl.beatsAt (48000.0) == Approx (2.0));
    CHECK (tl.sampleAtBeats (2.0) == Approx (48000.0));
    CHECK_FALSE (tl.isPlayingAt (0.0));
    CHECK (tl.bpmAt (0.0) == TakeTimeline::fallbackBpm);
    CHECK (tl.barAt (4.0 * 24000.0).number == 2);
    CHECK (tl.tempoMap (0.0, 10.0).size() == 1);
    CHECK (tl.timeSignatureMap (0.0, 10.0).front().numerator == 4);
}

TEST_CASE ("TakeTimeline: tempo change is honoured from its sample", "[taketimeline]")
{
    const double sr = 48000.0;
    TakeTimeline tl;
    tl.reset (sr);
    tl.addTransport (0, playingAt (0.0, 120.0));
    // 2 s at 120 = 4 beats, then 90 BPM
    tl.addTransport (96000, playingAt (4.0, 90.0));

    CHECK (tl.getSegments().size() == 2);
    CHECK (tl.beatsAt (96000.0) == Approx (4.0));
    CHECK (tl.beatsAt (96000.0 + sr) == Approx (5.5));
    CHECK (tl.sampleAtBeats (5.5) == Approx (96000.0 + sr));
    CHECK (tl.sampleAtBeats (2.0) == Approx (48000.0));
    CHECK (tl.bpmAt (95999.0) == 120.0);
    CHECK (tl.bpmAt (96000.0) == 90.0);

    const auto map = tl.tempoMap (1.0, 100.0);
    REQUIRE (map.size() == 2);
    CHECK (map[0].beats == 1.0);
    CHECK (map[0].bpm == 120.0);
    CHECK (map[1].beats == Approx (4.0));
    CHECK (map[1].bpm == 90.0);

    // A map that starts after the change has only the new tempo
    const auto later = tl.tempoMap (6.0, 100.0);
    REQUIRE (later.size() == 1);
    CHECK (later[0].bpm == 90.0);
}

TEST_CASE ("TakeTimeline: stop and start", "[taketimeline]")
{
    const double sr = 48000.0;
    TakeTimeline tl;
    tl.reset (sr);
    tl.addTransport (0, stoppedAt (0.0));
    CHECK_FALSE (tl.isPlayingAt (100.0));
    CHECK (tl.beatsAt (40000.0) == 0.0);

    tl.addTransport (48000, playingAt (0.0, 120.0)); // start after 1 s stopped
    CHECK (tl.beatsAt (48000.0 + 24000.0) == Approx (1.0));

    tl.addTransport (144000, stoppedAt (4.0));      // stop after 2 s = 4 beats
    CHECK (tl.beatsAt (200000.0) == Approx (4.0));  // U does not advance while stopped

    tl.addTransport (300000, playingAt (32.0, 120.0)); // start somewhere else: U continues, grid re-anchors
    CHECK (tl.beatsAt (300000.0) == Approx (4.0));
    CHECK (tl.beatsAt (324000.0) == Approx (5.0));
    CHECK (tl.barAt (300000.0).number == 9); // ppq 32 = bar 9
    CHECK (tl.barAt (300000.0).startBeats == Approx (4.0));

    // tempoMap ignores stopped segments
    const auto map = tl.tempoMap (0.0, 10.0);
    CHECK (map.size() == 1);
}

TEST_CASE ("TakeTimeline: loop wrap keeps unwrapped beats continuous and re-anchors the bar grid", "[taketimeline]")
{
    const double sr = 48000.0;
    TakeTimeline tl;
    tl.reset (sr);
    tl.addTransport (0, playingAt (16.0, 120.0)); // bar 5
    // 4 bars (16 beats = 8 s) later the host wraps back to ppq 16
    const int64_t wrap = 8 * 48000;
    tl.addTransport (wrap, playingAt (16.0, 120.0));

    CHECK (tl.beatsAt (static_cast<double> (wrap)) == Approx (32.0));
    CHECK (tl.beatsAt (static_cast<double> (wrap) + 24000.0) == Approx (33.0));
    CHECK (tl.barAt (static_cast<double> (wrap) - 2000.0).number == 8);
    CHECK (tl.barAt (static_cast<double> (wrap) - 1.0).number == 9); // within the snap tolerance of bar 9
    const auto bar = tl.barAt (static_cast<double> (wrap) + 100.0);
    CHECK (bar.number == 5);
    CHECK (bar.startBeats == Approx (32.0));
    CHECK (tl.sampleAtBeats (33.0) == Approx (static_cast<double> (wrap) + 24000.0));
}

TEST_CASE ("TakeTimeline: small deviations re-sync, never backwards", "[taketimeline]")
{
    const double sr = 48000.0;
    TakeTimeline tl;
    tl.reset (sr);
    tl.addTransport (0, playingAt (0.0, 120.0));

    // The host is 0.01 beats ahead of the prediction (tempo change inside a block): U follows the host
    tl.addTransport (48000, playingAt (2.01, 120.0));
    CHECK (tl.beatsAt (48000.0) == Approx (2.01));
    CHECK (tl.barAt (48000.0).startBeats == Approx (0.0));

    // Behind by 0.3 beats: U re-syncs down, but never below the previous segment's start
    tl.addTransport (48001, playingAt (1.72, 120.0));
    CHECK (tl.beatsAt (48001.0) == Approx (2.01));
    // Back in step with the host one second later: U = 2.01 + 2 beats (the host position is 0.29 behind U)
    tl.addTransport (96000, playingAt (3.72, 120.0));
    CHECK (tl.beatsAt (96000.0) == Approx (4.01).margin (1e-9));

    // Beats are monotonic over the whole timeline
    double previous = -1.0;
    bool monotonic = true;
    for (double s = 0.0; s < 150000.0; s += 97.0)
    {
        const auto b = tl.beatsAt (s);
        monotonic = monotonic && b >= previous;
        previous = b;
    }
    CHECK (monotonic);
}

TEST_CASE ("TakeTimeline: playing without a position counts as stopped, no bpm = 120", "[taketimeline]")
{
    TakeTimeline tl;
    tl.reset (48000.0);
    TransportInfo t;
    t.isPlaying = true; // a host that says "playing" without ppq has no grid: free mode
    tl.addTransport (0, t);
    CHECK_FALSE (tl.isPlayingAt (1000.0));
    CHECK (tl.bpmAt (1000.0) == TakeTimeline::fallbackBpm);
    CHECK (tl.beatsAt (100000.0) == tl.beatsAt (0.0));
    CHECK_FALSE (tl.getSegments().front().hasHostPosition);

    TransportInfo bad = playingAt (0.0, 120.0);
    bad.bpm = 0.0;
    tl.addTransport (1000, bad);
    CHECK (tl.bpmAt (2000.0) == TakeTimeline::fallbackBpm);
    CHECK (tl.isPlayingAt (2000.0));
}

TEST_CASE ("TakeTimeline: bar grid from the host bar start, from ppq 0, and in odd meters", "[taketimeline]")
{
    const double sr = 48000.0;

    SECTION ("4/4 from the host bar start")
    {
        TakeTimeline tl;
        tl.reset (sr);
        tl.addTransport (0, playingAt (10.0, 120.0)); // bar 3 (8..12)
        const auto bar = tl.barAt (0.0);
        CHECK (bar.number == 3);
        CHECK (bar.startBeats == Approx (8.0));
        CHECK (bar.lengthBeats == 4.0);
        CHECK (tl.barAt (48000.0).number == 4); // ppq 12
    }

    SECTION ("without a host bar start: bars of the current signature from ppq 0")
    {
        TakeTimeline tl;
        tl.reset (sr);
        auto t = playingAt (10.0, 120.0, 3, 4);
        t.hasBarStart = false;
        tl.addTransport (0, t);
        const auto bar = tl.barAt (0.0);
        CHECK (bar.number == 4); // 9..12
        CHECK (bar.startBeats == Approx (9.0));
        CHECK (bar.lengthBeats == 3.0);
        CHECK (bar.timeSigNumerator == 3);
    }

    SECTION ("7/8: 3.5 quarter notes per bar")
    {
        TakeTimeline tl;
        tl.reset (sr);
        tl.addTransport (0, playingAt (7.5, 120.0, 7, 8)); // bar 3 starts at 7.0
        const auto bar = tl.barAt (0.0);
        CHECK (bar.number == 3);
        CHECK (bar.startBeats == Approx (7.0));
        CHECK (bar.lengthBeats == 3.5);
        CHECK (tl.barAt (24000.0 * 3.0).number == 4); // ppq 10.5
    }

    SECTION ("snap tolerance: a hair before a bar line belongs to the next bar")
    {
        TakeTimeline tl;
        tl.reset (sr);
        tl.addTransport (0, playingAt (0.0, 120.0));
        const auto barLine = 4.0 * 24000.0; // beat 4 = bar 2
        const auto hair = TakeTimeline::barSnapToleranceBeats * 0.5 * 24000.0;
        CHECK (tl.barAt (barLine - hair).number == 2);
        CHECK (tl.barAt (barLine - 3.0 * hair).number == 1);
    }

    SECTION ("time signature map at the host bar start")
    {
        TakeTimeline tl;
        tl.reset (sr);
        tl.addTransport (0, playingAt (0.0, 120.0));
        // At ppq 8 (bar 3) the song switches to 3/4; the host reports it a little after the bar line
        auto t = playingAt (8.0 + 480.0 / 24000.0, 120.0, 3, 4);
        t.ppqPositionOfLastBarStart = 8.0;
        tl.addTransport (96000 + 480, t);
        const auto map = tl.timeSignatureMap (0.0, 100.0);
        REQUIRE (map.size() == 2);
        CHECK (map[0].numerator == 4);
        CHECK (map[1].numerator == 3);
        CHECK (map[1].beats == Approx (8.0));
        // Bars of the new signature (the bar number itself is approximate after a change, design 6)
        CHECK (tl.barAt (96000.0 + 480.0 + 3.0 * 24000.0).number == tl.barAt (96000.0 + 480.0).number + 1);
        CHECK (tl.barAt (96000.0 + 480.0 + 3.0 * 24000.0).startBeats == Approx (11.0));
    }
}

TEST_CASE ("TakeTimeline: slice starts at the requested sample with the same mapping", "[taketimeline]")
{
    const double sr = 48000.0;
    TakeTimeline tl;
    tl.reset (sr);
    tl.addTransport (0, stoppedAt (0.0));
    tl.addTransport (48000, playingAt (0.0, 120.0));
    tl.addTransport (96000, playingAt (2.0, 100.0));
    tl.addTransport (192000, stoppedAt (3.6667));

    const auto s = tl.slice (60000, 150000);
    REQUIRE (s.getSegments().size() == 2);
    CHECK (s.getSegments()[0].startSample == 60000);
    CHECK (s.getSampleRate() == sr);

    for (double x : { 60000.0, 80000.0, 96000.0, 120000.0, 150000.0 })
        CHECK (s.beatsAt (x) == Approx (tl.beatsAt (x)));

    CHECK (s.barAt (70000.0).number == tl.barAt (70000.0).number);
    CHECK (s.barAt (70000.0).startBeats == Approx (tl.barAt (70000.0).startBeats));

    // Earlier samples extrapolate with the playing segment (the stopped part is not in the slice)
    CHECK (s.beatsAt (36000.0) == Approx (-0.5));
    CHECK (s.sampleAtBeats (-0.5) == Approx (36000.0));

    // A slice of a sample on a segment start takes that segment unchanged
    CHECK (tl.slice (96000, 96000).getSegments().front() == tl.getSegments()[2]);
    CHECK (TakeTimeline {}.slice (0, 10).empty());
}

TEST_CASE ("TakeTimeline: equal sample replaces the state but keeps the beats; eviction", "[taketimeline]")
{
    TakeTimeline tl;
    tl.reset (48000.0);
    tl.addTransport (0, playingAt (0.0, 120.0));
    tl.addTransport (48000, playingAt (2.0, 120.0));
    tl.addTransport (48000, playingAt (2.0, 140.0));
    REQUIRE (tl.getSegments().size() == 2);
    CHECK (tl.getSegments()[1].bpm == 140.0);
    CHECK (tl.getSegments()[1].beats == Approx (2.0));

    tl.addTransport (96000, playingAt (2.0 + 140.0 / 60.0, 140.0));
    tl.evictBefore (50000);
    REQUIRE (tl.getSegments().size() == 2);
    CHECK (tl.getSegments()[0].startSample == 48000);
    tl.evictBefore (1000000);
    CHECK (tl.getSegments().size() == 1);
    CHECK (tl.beatsAt (96000.0 + 48000.0) == Approx (2.0 + 2.0 * 140.0 / 60.0));
}

TEST_CASE ("TakeTimeline: deterministic sampleAtBeats round trip over a tempo map", "[taketimeline]")
{
    TakeTimeline tl;
    tl.reset (44100.0);
    tl.addTransport (0, playingAt (0.0, 60.0));
    tl.addTransport (44100, playingAt (1.0, 97.5));
    tl.addTransport (88200, playingAt (1.0 + 97.5 / 60.0, 174.0));

    for (double s = -5000.0; s < 200000.0; s += 1234.5)
        CHECK (tl.sampleAtBeats (tl.beatsAt (s)) == Approx (s).margin (1e-6));
}
