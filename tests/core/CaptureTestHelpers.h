#pragma once

// Helpers for the M3 capture tests (docs/M3-DESIGN.md 16). Header-only, JUCE-free. Owner: agent A.

#include "vocalsynth/CaptureHistory.h"
#include "vocalsynth/CaptureRecord.h"
#include "vocalsynth/CaptureTap.h"
#include "vocalsynth/TakeSegmenter.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
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

} // namespace vocalsynth::test
