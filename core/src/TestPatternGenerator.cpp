#include "vocalsynth/TestPatternGenerator.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace vocalsynth
{

namespace
{
    constexpr double eighthBeats = 0.5;
    constexpr double bendPeriodBeats = 2.0;
    constexpr double ccPeriodBeats = 4.0;
    constexpr double notesSegmentBeats = 16.0;     // 'all': 4 bars notes
    constexpr double pitchBendSegmentBeats = 8.0;  // 'all': 2 bars pitch bend (CC gets the rest)

    double fractionalPart (double x) noexcept
    {
        return x - std::floor (x);
    }

    /** Triangle 0 -> +8191 -> 0 -> -8192 -> 0 over two beats (docs/M1-DESIGN.md 3.2). */
    int bendValueAt (double beatsSinceSegmentStart) noexcept
    {
        const auto p = fractionalPart (beatsSinceSegmentStart / bendPeriodBeats);
        const auto tri = p < 0.25 ? 4.0 * p : (p < 0.75 ? 2.0 - 4.0 * p : 4.0 * p - 4.0);
        const auto value = tri >= 0.0 ? std::lround (tri * 8191.0) : std::lround (tri * 8192.0);
        return static_cast<int> (std::clamp (value, -8192L, 8191L));
    }

    /** Triangle 0 -> 127 -> 0 over four beats. */
    int ccValueAt (double beatsSinceSegmentStart) noexcept
    {
        const auto q = fractionalPart (beatsSinceSegmentStart / ccPeriodBeats);
        const auto value = std::lround (127.0 * (q < 0.5 ? 2.0 * q : 2.0 - 2.0 * q));
        return static_cast<int> (std::clamp (value, 0L, 127L));
    }

    TestPatternSettings sanitise (const TestPatternSettings& s) noexcept
    {
        TestPatternSettings result;

        const auto patternIndex = static_cast<int> (s.pattern);
        result.pattern = (patternIndex >= static_cast<int> (TestPattern::off) && patternIndex <= static_cast<int> (TestPattern::all))
                             ? s.pattern
                             : TestPattern::off;
        result.channel = std::clamp (s.channel, 1, 16);
        result.ccNumber = std::clamp (s.ccNumber, 1, 119);
        result.bpm = std::isnan (s.bpm) ? 120.0
                                        : std::clamp (s.bpm, TestPatternGenerator::minBpm, TestPatternGenerator::maxBpm);
        return result;
    }
} // namespace

void TestPatternGenerator::prepare (double newSampleRate) noexcept
{
    sampleRate = (std::isfinite (newSampleRate) && newSampleRate > 0.0) ? newSampleRate : 48000.0;
    samplesPerMinute = 60.0 * sampleRate;
    bendGridSamples = std::max (int64_t { 1 }, static_cast<int64_t> (std::llround (sampleRate / bendUpdatesPerSecond)));
    reset();
}

void TestPatternGenerator::reset() noexcept
{
    samplePosition = 0;
    hasLastSettings = false;
    lastSettings = {};

    anchorSample = 0;
    anchorBeat = 0.0;
    bpm = 120.0;

    channel = 1;
    ccNumber = 1;
    activeNote = -1;
    activeChannel = 1;
    lastBend = 0;
    lastCcValue = -1;

    isAllPattern = false;
    segmentType = SegmentType::notes;
    segmentIndex = 0;
    segmentStartBeat = 0.0;
    segmentEndBeat = std::numeric_limits<double>::infinity();
    segmentBegun = false;
    segmentStartSample = 0;
    nextNoteIndex = 0;
    noteOffBeat = 0.0;
    nextRestrikeBeat = 0.0;
    nextGridSample = 0;
}

double TestPatternGenerator::beatAt (int64_t t) const noexcept
{
    // Multiply before dividing: with integer sample rates and bpm values the result is exact whenever the
    // true beat position is representable (e.g. 0.5, 16.0), so events land on the mathematically exact sample.
    return static_cast<double> (t - anchorSample) * bpm / samplesPerMinute + anchorBeat;
}

void TestPatternGenerator::restart (const TestPatternSettings& s) noexcept
{
    anchorSample = samplePosition;
    anchorBeat = 0.0;
    bpm = s.bpm;

    channel = static_cast<uint8_t> (s.channel);
    ccNumber = static_cast<uint8_t> (s.ccNumber);
    activeNote = -1;
    activeChannel = channel;
    lastBend = 0;
    lastCcValue = -1;

    isAllPattern = s.pattern == TestPattern::all;

    switch (s.pattern)
    {
        case TestPattern::pitchBend:     segmentType = SegmentType::pitchBend; break;
        case TestPattern::controlChange: segmentType = SegmentType::controlChange; break;
        case TestPattern::off:
        case TestPattern::notes:
        case TestPattern::all:
        default:                         segmentType = SegmentType::notes; break;
    }

    startSegment (0);
}

void TestPatternGenerator::startSegment (int64_t index) noexcept
{
    segmentIndex = index;
    segmentBegun = false;
    nextNoteIndex = 0;

    if (! isAllPattern)
    {
        segmentStartBeat = 0.0;
        segmentEndBeat = std::numeric_limits<double>::infinity();
        return;
    }

    const auto cycleStart = static_cast<double> (index / 3) * allCycleBeats;

    switch (index % 3)
    {
        case 0:
            segmentType = SegmentType::notes;
            segmentStartBeat = cycleStart;
            segmentEndBeat = cycleStart + notesSegmentBeats;
            break;

        case 1:
            segmentType = SegmentType::pitchBend;
            segmentStartBeat = cycleStart + notesSegmentBeats;
            segmentEndBeat = segmentStartBeat + pitchBendSegmentBeats;
            break;

        default:
            segmentType = SegmentType::controlChange;
            segmentStartBeat = cycleStart + notesSegmentBeats + pitchBendSegmentBeats;
            segmentEndBeat = cycleStart + allCycleBeats;
            break;
    }
}

void TestPatternGenerator::processSample (int64_t t, int offset, VsEventSink out) noexcept
{
    const auto beat = beatAt (t);

    // Order of events on one sample: note-off, bend centre, note-on, new bend/CC value.

    // 1. 'all' segment boundary: close the old segment (note-off, bend centre), then switch.
    if (beat >= segmentEndBeat)
    {
        if (activeNote >= 0)
        {
            out.push (VsEvent::makeNoteOff (offset, activeChannel, static_cast<uint8_t> (activeNote)));
            activeNote = -1;
        }

        if (lastBend != 0)
        {
            out.push (VsEvent::makePitchBend (offset, channel, 0));
            lastBend = 0;
        }

        do
            startSegment (segmentIndex + 1);
        while (beat >= segmentEndBeat);
    }

    // 2. Note-offs inside the running segment
    bool strikeHeldNote = false;

    if (segmentBegun)
    {
        if (segmentType == SegmentType::notes)
        {
            const auto nextOnBeat = segmentStartBeat + static_cast<double> (nextNoteIndex) * eighthBeats;

            if (activeNote >= 0 && (beat >= noteOffBeat || beat >= nextOnBeat))
            {
                out.push (VsEvent::makeNoteOff (offset, activeChannel, static_cast<uint8_t> (activeNote)));
                activeNote = -1;
            }
        }
        else if (beat >= nextRestrikeBeat)
        {
            if (activeNote >= 0)
            {
                out.push (VsEvent::makeNoteOff (offset, activeChannel, static_cast<uint8_t> (activeNote)));
                activeNote = -1;
            }

            strikeHeldNote = true;
            nextRestrikeBeat += heldNoteRetriggerBeats;
        }
    }
    else
    {
        // The segment starts on this sample (pattern restart or 'all' boundary)
        segmentBegun = true;
        segmentStartSample = t;
        nextGridSample = t;

        if (segmentType != SegmentType::notes)
        {
            strikeHeldNote = true;
            nextRestrikeBeat = segmentStartBeat + heldNoteRetriggerBeats;
        }
    }

    // 3. Note-ons
    if (segmentType == SegmentType::notes)
    {
        const auto onBeat = segmentStartBeat + static_cast<double> (nextNoteIndex) * eighthBeats;

        if (beat >= onBeat)
        {
            const auto note = scaleNotes[static_cast<std::size_t> (nextNoteIndex % 8)];
            out.push (VsEvent::makeNoteOn (offset, channel, static_cast<uint8_t> (note), velocity));
            activeNote = note;
            activeChannel = channel;
            noteOffBeat = onBeat + gateFraction * eighthBeats;
            ++nextNoteIndex;
        }
    }
    else if (strikeHeldNote)
    {
        out.push (VsEvent::makeNoteOn (offset, channel, static_cast<uint8_t> (heldNote), velocity));
        activeNote = heldNote;
        activeChannel = channel;
    }

    // 4. Continuous values
    if (segmentType == SegmentType::pitchBend)
    {
        if (t >= nextGridSample)
        {
            nextGridSample += bendGridSamples;
            const auto value = bendValueAt (beat - segmentStartBeat);

            if (value != lastBend)
            {
                out.push (VsEvent::makePitchBend (offset, channel, static_cast<int16_t> (value)));
                lastBend = value;
            }
        }
    }
    else if (segmentType == SegmentType::controlChange)
    {
        const auto value = ccValueAt (beat - segmentStartBeat);

        if (value != lastCcValue)
        {
            out.push (VsEvent::makeControlChange (offset, channel, ccNumber, static_cast<uint8_t> (value)));
            lastCcValue = value;
        }
    }
}

void TestPatternGenerator::processToSink (int numSamples, const TestPatternSettings& settings, VsEventSink out) noexcept
{
    if (numSamples <= 0)
        return;

    const auto s = sanitise (settings);

    if (! hasLastSettings)
    {
        restart (s);
        hasLastSettings = true;
    }
    else if (s.pattern != lastSettings.pattern || s.channel != lastSettings.channel || s.ccNumber != lastSettings.ccNumber)
    {
        flushToSink (0, out);
        restart (s);
    }
    else if (s.bpm != bpm)
    {
        // Smooth tempo change: keep the beat position, change only its rate from this block on
        anchorBeat = beatAt (samplePosition);
        anchorSample = samplePosition;
        bpm = s.bpm;
    }

    lastSettings = s;

    if (s.pattern != TestPattern::off)
        for (int i = 0; i < numSamples; ++i)
            processSample (samplePosition + i, i, out);

    samplePosition += numSamples;
}

void TestPatternGenerator::flushToSink (int sampleOffset, VsEventSink out) noexcept
{
    const auto offset = std::max (0, sampleOffset);

    if (activeNote >= 0)
    {
        out.push (VsEvent::makeNoteOff (offset, activeChannel, static_cast<uint8_t> (activeNote)));
        activeNote = -1;
    }

    if (lastBend != 0)
    {
        out.push (VsEvent::makePitchBend (offset, channel, 0));
        lastBend = 0;
    }
}

} // namespace vocalsynth
