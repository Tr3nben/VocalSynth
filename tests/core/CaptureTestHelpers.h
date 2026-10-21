#pragma once

// Helpers for the M3 capture tests (docs/M3-DESIGN.md 16). Header-only, JUCE-free. Owner: agent A.

#include "vocalsynth/CaptureHistory.h"
#include "vocalsynth/CaptureRecord.h"
#include "vocalsynth/CaptureTap.h"
#include "vocalsynth/TakeSegmenter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace vocalsynth::test
{

/** Everything a tap produced, with the block structure removed: non-audio records in stream order and
    the audio frames by their first sample. */
struct TapOutput
{
    std::vector<CaptureRecord> records;
    std::vector<CaptureRecord> audioRecords;
    std::map<int64_t, std::vector<float>> frames;

    /** Audio as one stream from the first frame on (frames must be contiguous; returns false if not). */
    bool audioStream (std::vector<float>& out, int64_t& firstSample) const
    {
        out.clear();
        firstSample = frames.empty() ? 0 : frames.begin()->first;
        auto expected = firstSample;

        for (const auto& [start, data] : frames)
        {
            if (start != expected)
                return false;

            out.insert (out.end(), data.begin(), data.end());
            expected += static_cast<int64_t> (data.size());
        }

        return true;
    }

    std::vector<CaptureRecord> ofType (CaptureRecordType type) const
    {
        std::vector<CaptureRecord> result;
        std::copy_if (records.begin(), records.end(), std::back_inserter (result),
                      [type] (const CaptureRecord& r) { return r.type == type; });
        return result;
    }

    std::vector<CaptureRecord> markers (CaptureMarkerKind kind) const
    {
        std::vector<CaptureRecord> result;
        std::copy_if (records.begin(), records.end(), std::back_inserter (result),
                      [kind] (const CaptureRecord& r)
                      { return r.type == CaptureRecordType::marker && r.asMarker().kind == kind; });
        return result;
    }
};

/** Pops everything from the tap's rings into out (consumer side, the test's thread). */
inline void drainTap (CaptureTap& tap, TapOutput& out)
{
    CaptureRecord r;

    while (tap.getRecordRing().pop (r))
    {
        if (r.type == CaptureRecordType::audio)
        {
            std::vector<float> frame (static_cast<std::size_t> (r.asAudioNumSamples()));
            const auto got = tap.getAudioRing().popBulk (std::span<float> { frame });
            frame.resize (got);
            out.frames[r.sample] = std::move (frame);
            out.audioRecords.push_back (r);
        }
        else
        {
            out.records.push_back (r);
        }
    }
}

/** A host transport that changes only at given samples (piecewise: start, stop, tempo, relocation / loop
    wrap, time signature). transportAt (s) is what a host reports for a block starting at s. */
class PlayheadSim
{
public:
    struct Piece
    {
        int64_t startSample = 0;
        bool playing = false;
        double ppqAtStart = 0.0;
        double bpm = 120.0;
        int numerator = 4;
        int denominator = 4;
    };

    explicit PlayheadSim (double sampleRateToUse) : sampleRate (sampleRateToUse) {}

    PlayheadSim& add (const Piece& piece)
    {
        pieces.push_back (piece);
        return *this;
    }

    /** Continues from the previous piece at sample (ppq continuous) with new values. */
    PlayheadSim& change (int64_t sample, bool playing, double bpm, int numerator = 4, int denominator = 4)
    {
        return add ({ sample, playing, ppqAt (sample), bpm, numerator, denominator });
    }

    double ppqAt (int64_t sample) const
    {
        const auto& p = pieceAt (sample);
        if (! p.playing)
            return p.ppqAtStart;
        return p.ppqAtStart + p.bpm * static_cast<double> (sample - p.startSample) / (60.0 * sampleRate);
    }

    TransportInfo transportAt (int64_t sample) const
    {
        const auto& p = pieceAt (sample);
        TransportInfo t;
        t.isPlaying = p.playing;
        t.hasPpq = true;
        t.ppqPosition = ppqAt (sample);
        t.hasBpm = true;
        t.bpm = p.bpm;
        t.hasTimeSignature = true;
        t.timeSigNumerator = p.numerator;
        t.timeSigDenominator = p.denominator;
        t.hasBarStart = true;
        const auto barLength = p.numerator * 4.0 / p.denominator;
        t.ppqPositionOfLastBarStart = std::floor ((t.ppqPosition + 1.0e-9) / barLength) * barLength;
        return t;
    }

private:
    const Piece& pieceAt (int64_t sample) const
    {
        static const Piece none {};
        const Piece* found = pieces.empty() ? &none : &pieces.front();

        for (const auto& p : pieces)
            if (p.startSample <= sample)
                found = &p;

        return *found;
    }

    double sampleRate;
    std::vector<Piece> pieces;
};

/** Builds a record stream by hand and feeds it to a history + segmenter like CaptureDrain does. */
class RecordFeeder
{
public:
    explicit RecordFeeder (double sampleRateToUse = 48000.0, int maxBlock = 512, double audioMinutes = 1.0)
        : sampleRate (sampleRateToUse)
    {
        history.prepare (audioMinutes);
        feed (CaptureRecord::makeReset (0, { sampleRate, maxBlock, true }));
        settings (0, {});
        marker (0, CaptureMarkerKind::captureOn);
    }

    void feed (const CaptureRecord& r)
    {
        history.addRecord (r);
        segmenter.process (r, history, closed);
    }

    int64_t seconds (double s) const { return static_cast<int64_t> (std::llround (s * sampleRate)); }

    void transport (int64_t sample, bool playing, double ppq, double bpm = 120.0, int num = 4, int den = 4,
                    bool hasPpq = true)
    {
        TransportInfo t;
        t.isPlaying = playing;
        t.hasPpq = hasPpq;
        t.ppqPosition = ppq;
        t.hasBpm = true;
        t.bpm = bpm;
        t.hasTimeSignature = true;
        t.timeSigNumerator = num;
        t.timeSigDenominator = den;
        t.hasBarStart = hasPpq;
        const auto barLength = num * 4.0 / den;
        t.ppqPositionOfLastBarStart = std::floor ((ppq + 1.0e-9) / barLength) * barLength;
        feed (CaptureRecord::makeTransport (sample, t));
    }

    void noTransport (int64_t sample) { feed (CaptureRecord::makeTransport (sample, TransportInfo {})); }

    void settings (int64_t sample, const CaptureSettingsInfo& info) { feed (CaptureRecord::makeSettings (sample, info)); }

    void noteOn (int64_t sample, int note, int velocity = 100, int32_t delay = 0, uint8_t flags = 0)
    {
        feed (CaptureRecord::makeEvent (sample,
                                        VsEvent::makeNoteOn (0, 1, static_cast<uint8_t> (note),
                                                             static_cast<uint8_t> (velocity)),
                                        { delay, EventTimingKind::measured, flags }));
    }

    void noteOff (int64_t sample, int note, int32_t delay = 0)
    {
        feed (CaptureRecord::makeEvent (sample, VsEvent::makeNoteOff (0, 1, static_cast<uint8_t> (note)),
                                        { delay, EventTimingKind::measured, 0 }));
    }

    void bend (int64_t sample, int16_t value)
    {
        feed (CaptureRecord::makeEvent (sample, VsEvent::makePitchBend (0, 1, value), {}));
    }

    void marker (int64_t sample, CaptureMarkerKind kind, int bars = 8, double seconds = 30.0)
    {
        feed (CaptureRecord::makeMarker (sample, { kind, bars, seconds }));
    }

    void gap (int64_t sample) { feed (CaptureRecord::makeGap (sample, { 3, 0 })); }

    void advance (int64_t sample) { segmenter.advanceTo (sample, history, closed); }

    double sampleRate;
    CaptureHistory history;
    TakeSegmenter segmenter;
    std::vector<Take> closed;
};

inline int countEvents (const Take& take, VsEventType type, bool synthetic)
{
    return static_cast<int> (std::count_if (take.events.begin(), take.events.end(),
                                            [&] (const CapturedEvent& e)
                                            { return e.event.type == type && e.synthetic == synthetic; }));
}

/** True if every note-on of the take has a later (or same-sample) note-off of the same note. */
inline bool notesPaired (const Take& take)
{
    std::array<std::array<int, 128>, 16> open {};

    for (const auto& e : take.events)
    {
        const auto ch = static_cast<std::size_t> (e.event.channel - 1);
        const auto note = static_cast<std::size_t> (e.event.data1);

        if (e.event.type == VsEventType::noteOn)
            ++open[ch][note];
        else if (e.event.type == VsEventType::noteOff && open[ch][note] > 0)
            --open[ch][note];
    }

    for (const auto& ch : open)
        for (auto n : ch)
            if (n != 0)
                return false;

    return true;
}

//==============================================================================
/** An event at an absolute sample, delivered by runTap() to the chunk that contains it. */
struct ScheduledEvent
{
    int64_t sample = 0;
    VsEvent event;
    VsEventTiming timing;
};

/** Everything a host and the plugin would hand the tap, independent of the block structure. Control
    points (requests, flushes, switches) apply at the block that starts at their sample: put them on
    multiples of every compared block size (430080 = lcm (7, 64, 480, 512, 4096)). */
struct TapScenario
{
    double sampleRate = 48000.0;
    int maxBlock = 4096;
    int64_t length = 0;
    PlayheadSim playhead { 48000.0 };
    std::vector<ScheduledEvent> events;                  ///< sorted by sample
    std::vector<int64_t> newTakeAt, grabAt, clearAt, flushAt;
    std::vector<std::pair<int64_t, int>> recordAt;        ///< M3.1 Record button: sample, bars
    std::vector<int64_t> recordCancelAt;
    int grabBars = 8;
    double grabSeconds = 30.0;
    std::vector<std::pair<int64_t, int64_t>> bypassed;   ///< [from, to)
    std::vector<std::pair<int64_t, int64_t>> audioOff;   ///< [from, to)
    std::vector<std::pair<int64_t, int64_t>> captureOff; ///< [from, to)
    std::vector<std::pair<int64_t, CaptureSettingsInfo>> settingsFrom; ///< sorted; default before the first

    static bool contains (const std::vector<int64_t>& points, int64_t s)
    {
        return std::find (points.begin(), points.end(), s) != points.end();
    }

    static bool inside (const std::vector<std::pair<int64_t, int64_t>>& ranges, int64_t s)
    {
        return std::any_of (ranges.begin(), ranges.end(), [s] (const auto& r) { return s >= r.first && s < r.second; });
    }

    CaptureSettingsInfo settingsAt (int64_t s) const
    {
        CaptureSettingsInfo result {};

        for (const auto& [from, info] : settingsFrom)
            if (from <= s)
                result = info;

        return result;
    }
};

/** Deterministic test voice: a sum of two sines plus a slow ramp (never silent, distinct per sample). */
inline float scenarioSignal (int64_t s)
{
    const auto x = static_cast<double> (s);
    return static_cast<float> (0.5 * std::sin (x * 0.0123) + 0.25 * std::sin (x * 0.000777) + 1.0e-7 * static_cast<double> (s % 1000));
}

/** Drives tap like the processor does (beginBlock, flushNotes, processChunk / processBypassed) with
    blocks of blockSize samples; calls consume() after every consumeEvery samples and once at the end. */
template <typename Consumer>
void runTap (CaptureTap& tap, const TapScenario& sc, int blockSize, Consumer&& consume, int64_t consumeEvery = 8192)
{
    tap.prepare (sc.sampleRate, sc.maxBlock);

    std::vector<VsEvent> events;
    std::vector<VsEventTiming> timings;
    std::vector<float> mono (static_cast<std::size_t> (blockSize));
    events.reserve (1024);
    timings.reserve (1024);
    std::size_t next = 0;
    int64_t sinceConsume = 0;

    for (int64_t start = 0; start < sc.length; start += blockSize)
    {
        const auto n = static_cast<int> (std::min<int64_t> (blockSize, sc.length - start));

        if (TapScenario::contains (sc.newTakeAt, start))
            tap.requestNewTake();
        if (TapScenario::contains (sc.grabAt, start))
            tap.requestGrab (sc.grabBars, sc.grabSeconds);
        if (TapScenario::contains (sc.clearAt, start))
            tap.requestClear();
        if (TapScenario::contains (sc.recordCancelAt, start))
            tap.requestRecordCancel();

        for (const auto& [at, bars] : sc.recordAt)
            if (at == start)
                tap.requestRecord (bars);

        CaptureBlockSettings bs;
        bs.captureEnabled = ! TapScenario::inside (sc.captureOff, start);
        bs.captureAudio = ! TapScenario::inside (sc.audioOff, start);
        bs.bypassed = TapScenario::inside (sc.bypassed, start);
        bs.settings = sc.settingsAt (start);

        tap.beginBlock (sc.playhead.transportAt (start), bs);

        events.clear();
        timings.clear();

        while (next < sc.events.size() && sc.events[next].sample < start + n)
        {
            auto e = sc.events[next].event;
            e.sampleOffset = static_cast<int> (sc.events[next].sample - start);
            events.push_back (e);
            timings.push_back (sc.events[next].timing);
            ++next;
        }

        if (bs.bypassed)
        {
            tap.flushNotes();
            tap.processBypassed (n);
        }
        else
        {
            if (TapScenario::contains (sc.flushAt, start))
                tap.flushNotes();

            for (int i = 0; i < n; ++i)
                mono[static_cast<std::size_t> (i)] = scenarioSignal (start + i);

            tap.processChunk (mono.data(), n, events, timings);
        }

        sinceConsume += n;

        if (sinceConsume >= consumeEvery)
        {
            consume();
            sinceConsume = 0;
        }
    }

    consume();
}

/** Constant / changing tempo, loop wrap with a time-signature change, stop/start, bypass, audio off,
    captureOff/On, requests and a flush: every change on a multiple of 430080 samples. */
inline TapScenario makeInvarianceScenario (double sr)
{
    constexpr int64_t P = 430080;
    TapScenario sc;
    sc.sampleRate = sr;
    sc.maxBlock = 4096;
    sc.length = 9 * P + 1234;
    sc.playhead = PlayheadSim (sr);
    sc.playhead.add ({ 0, false, 0.0, 120.0, 4, 4 });
    sc.playhead.change (P, true, 120.0);                       // transport start
    sc.playhead.change (2 * P, true, 97.5);                    // tempo change
    sc.playhead.add ({ 3 * P, true, 8.0, 97.5, 3, 4 });        // loop wrap + 3/4
    sc.playhead.change (6 * P, false, 97.5, 3, 4);             // stop
    sc.newTakeAt = { 2 * P };
    sc.grabAt = { 3 * P };
    sc.grabBars = 2;
    sc.bypassed = { { 4 * P, 5 * P } };
    sc.audioOff = { { 5 * P, 6 * P } };
    sc.flushAt = { 6 * P };
    sc.captureOff = { { 7 * P, 8 * P } };
    sc.clearAt = { 8 * P };
    CaptureSettingsInfo trueBend;
    trueBend.bendMode = BendMode::trueBend;
    sc.settingsFrom = { { 6 * P, trueBend } };

    for (int64_t s = 123, k = 0; s < sc.length; s += 5000, ++k)
    {
        const auto note = static_cast<uint8_t> (60 + k % 12);
        sc.events.push_back ({ s, VsEvent::makeNoteOn (0, 1, note, static_cast<uint8_t> (60 + k % 50)),
                               { static_cast<int32_t> ((k * 37) % 2000), EventTimingKind::measured, 0 } });
        sc.events.push_back ({ s + 3077, VsEvent::makeNoteOff (0, 1, note),
                               { static_cast<int32_t> ((k * 53) % 3000), EventTimingKind::model, 0 } });
    }

    for (int64_t s = 50; s < sc.length; s += 777)
        sc.events.push_back ({ s, VsEvent::makePitchBend (0, 1, static_cast<int16_t> ((s % 16000) - 8000)), {} });

    for (int64_t s = 999; s < sc.length; s += 9001)
        sc.events.push_back ({ s, VsEvent::makeControlChange (0, 1, 1, static_cast<uint8_t> (s % 128)), {} });

    std::stable_sort (sc.events.begin(), sc.events.end(),
                      [] (const ScheduledEvent& a, const ScheduledEvent& b) { return a.sample < b.sample; });
    return sc;
}

/** Empty string if a and b are the same take, else a description of the first difference. */
inline std::string takeDifference (const Take& a, const Take& b)
{
    std::ostringstream out;
    const auto field = [&out] (const char* name, auto x, auto y)
    {
        if (! (x == y) && out.tellp() == 0)
            out << name << ": " << +x << " vs " << +y;
    };

    field ("number", a.number, b.number);
    field ("mode", static_cast<int> (a.mode), static_cast<int> (b.mode));
    field ("kind", static_cast<int> (a.kind), static_cast<int> (b.kind));
    field ("closed", a.closed, b.closed);
    field ("startSample", a.startSample, b.startSample);
    field ("endSample", a.endSample, b.endSample);
    field ("captureEndSample", a.captureEndSample, b.captureEndSample);
    field ("sampleRate", a.sampleRate, b.sampleRate);
    field ("maxBlockSize", a.maxBlockSize, b.maxBlockSize);
    field ("settings", a.settings == b.settings, true);
    field ("freeBpm", a.freeBpm, b.freeBpm);
    field ("maxBars", a.maxBars, b.maxBars);
    field ("anchorBar", a.anchorBar, b.anchorBar);
    field ("noteCount", a.noteCount, b.noteCount);
    field ("incomplete", a.incomplete, b.incomplete);
    field ("audio", static_cast<int> (a.audio), static_cast<int> (b.audio));
    field ("events.size", a.events.size(), b.events.size());
    field ("timeline", a.timeline.getSegments() == b.timeline.getSegments(), true);

    if (out.tellp() == 0)
        for (std::size_t i = 0; i < a.events.size(); ++i)
            if (! (a.events[i] == b.events[i]))
            {
                out << "event " << i << " at " << a.events[i].sample << " vs " << b.events[i].sample;
                break;
            }

    return out.str();
}

} // namespace vocalsynth::test
