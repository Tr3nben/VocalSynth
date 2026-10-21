#include "vocalsynth/TakeSegmenter.h"

#include "vocalsynth/CaptureOptions.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace vocalsynth
{

static_assert (TakeSegmenter::splitReserveEvents < TakeSegmenter::maxTakeEvents);

namespace
{
constexpr auto noLimit = std::numeric_limits<int64_t>::max();

bool isPlayingTransport (const TransportInfo& info) noexcept
{
    return info.isPlaying && info.hasPpq && std::isfinite (info.ppqPosition);
}

bool isNoteOn (const VsEvent& e) noexcept
{
    return e.type == VsEventType::noteOn && e.data2 > 0;
}

bool isNoteOff (const VsEvent& e) noexcept
{
    return e.type == VsEventType::noteOff || (e.type == VsEventType::noteOn && e.data2 == 0);
}

std::size_t channelIndex (const VsEvent& e) noexcept
{
    return static_cast<std::size_t> (std::clamp (static_cast<int> (e.channel), 1, 16) - 1);
}

std::size_t noteIndex (const VsEvent& e) noexcept
{
    return static_cast<std::size_t> (std::min (static_cast<int> (e.data1), 127));
}

CapturedEvent makeCaptured (int64_t sample, const EventRecord& record) noexcept
{
    CapturedEvent e;
    e.sample = sample;
    e.estimatedSample = sample - static_cast<int64_t> (record.timing.delaySamples);
    e.event = record.event;
    e.event.sampleOffset = 0;
    e.timing = record.timing;
    return e;
}

CapturedEvent makeSynthetic (int64_t sample, const VsEvent& event) noexcept
{
    CapturedEvent e;
    e.sample = sample;
    e.estimatedSample = sample;
    e.event = event;
    e.synthetic = true;
    return e;
}

/** Appends one event to a take and keeps its open-note set and note count. */
void addToTake (Take& take, std::array<std::bitset<128>, 16>& notes, const CapturedEvent& e)
{
    if (isNoteOn (e.event))
    {
        notes[channelIndex (e.event)][noteIndex (e.event)] = true;
        ++take.noteCount;
    }
    else if (isNoteOff (e.event))
        notes[channelIndex (e.event)][noteIndex (e.event)] = false;

    take.events.push_back (e);
}

/** Synthetic note-off for every note open in notes, at sample. */
void closeNotes (Take& take, std::array<std::bitset<128>, 16>& notes, int64_t sample)
{
    for (std::size_t ch = 0; ch < notes.size(); ++ch)
    {
        if (! notes[ch].any())
            continue;

        for (std::size_t note = 0; note < 128; ++note)
            if (notes[ch][note])
                take.events.push_back (makeSynthetic (
                    sample, VsEvent::makeNoteOff (0, static_cast<uint8_t> (ch + 1), static_cast<uint8_t> (note))));

        notes[ch].reset();
    }
}

/** First sample at or after a fractional sample (where the unwrapped beats reach a bar line). */
int64_t ceilSample (double sample) noexcept
{
    return static_cast<int64_t> (std::ceil (sample - 1.0e-6));
}

int countNoteOns (const Take& take) noexcept
{
    return static_cast<int> (std::count_if (take.events.begin(), take.events.end(),
                                            [] (const CapturedEvent& e) { return isNoteOn (e.event); }));
}
} // namespace

int64_t TakeSegmenter::compensationTailSamples (double sampleRate, int maxBlockSize) noexcept
{
    if (! (sampleRate > 0.0))
        return 0;

    return maxRoundTripSamples (maxBlockSize, sampleRate)
           + static_cast<int64_t> (std::llround (detectionTailSeconds * sampleRate));
}

void TakeSegmenter::reset()
{
    state = State::off;
    hasOpenTake = false;
    openTake = {};
    closingTakes.clear();
    pendingGrabs.clear();
    freeStartAllowedFrom = 0;
    playing = false;
    hostBpm = TakeTimeline::fallbackBpm;
    settings = {};
    soundingNotes = {};
    soundingVelocity = {};
    numSounding = 0;
    lastNoteOffSample = 0;
    gapEndSample = 0;
    newestSample = 0;
    recordPhase = RecordPhase::idle;
    recordBars = 0;
    recordSearchFrom = 0;
    recordStatus = {};
}

void TakeSegmenter::setTakeGapSeconds (double seconds) noexcept
{
    takeGapSeconds = std::clamp (seconds, minTakeGapSeconds, maxTakeGapSeconds);
}

int64_t TakeSegmenter::tailSamples() const noexcept
{
    return compensationTailSamples (sampleRate, maxBlockSize);
}

int64_t TakeSegmenter::gapSamples() const noexcept
{
    return static_cast<int64_t> (std::llround (takeGapSeconds * sampleRate));
}

const Take* TakeSegmenter::getOpenTake() const noexcept
{
    return hasOpenTake ? &openTake.take : nullptr;
}

int TakeSegmenter::getOpenTakeBar() const noexcept
{
    if (! hasOpenTake || ! openTake.anchored || openTake.take.maxBars <= 0 || openTake.take.mode != TakeMode::synced)
        return 0;

    return std::min (openTake.barsDone + 1, openTake.take.maxBars);
}

int64_t TakeSegmenter::segmentRoundTrip (const Take& take) const noexcept
{
    // The test pattern is generated at the block's playhead: no input latency (docs/M3-DESIGN.md 19.1 F5)
    if (take.settings.testPattern)
        return 0;

    return computeRoundTripSamples (offsetSettings, maxBlockSize, sampleRate);
}

//==============================================================================
void TakeSegmenter::process (const CaptureRecord& record, const CaptureHistory& history, std::vector<Take>& closedTakes)
{
    if (record.type == CaptureRecordType::audio)
        return;

    if (record.type == CaptureRecordType::reset)
    {
        handleReset (record, history, closedTakes);
        return;
    }

    const auto sample = record.sample;

    // Every record before sample is in: whatever was due before it happens first (call-pattern independent)
    advanceInternal (sample, history, closedTakes);
    newestSample = std::max (newestSample, sample);

    switch (record.type)
    {
        case CaptureRecordType::transport: handleTransport (sample, record.asTransport(), history, closedTakes); break;
        case CaptureRecordType::settings: handleSettings (sample, record.asSettings(), history, closedTakes); break;
        case CaptureRecordType::event: handleEvent (sample, record.asEvent(), history, closedTakes); break;
        case CaptureRecordType::marker: handleMarker (sample, record.asMarker(), history, closedTakes); break;
        case CaptureRecordType::gap: handleGap (sample); break;
        case CaptureRecordType::reset:
        case CaptureRecordType::audio: break;
    }

    updateOpenSpan();

    if (record.type != CaptureRecordType::event)
        updateRecordStatus (history);
}

void TakeSegmenter::advanceTo (int64_t sample, const CaptureHistory& history, std::vector<Take>& closedTakes)
{
    newestSample = std::max (newestSample, sample);
    advanceInternal (newestSample, history, closedTakes);
    updateOpenSpan();
    updateRecordStatus (history);
}

void TakeSegmenter::closeIdle (const CaptureHistory&, std::vector<Take>& closedTakes)
{
    if (state != State::freeRecording || ! hasOpenTake || numSounding > 0)
        return;

    const auto tail = static_cast<int64_t> (std::llround (freeTailSeconds * sampleRate));
    const auto end = std::max (openTake.take.startSample, std::min (lastNoteOffSample + tail, newestSample));
    endOpenTake (end, false, closedTakes);
    state = State::freeWaiting;
}

void TakeSegmenter::updateOpenSpan() noexcept
{
    if (! hasOpenTake)
        return;

    auto& take = openTake.take;
    take.endSample = std::max (take.startSample, newestSample);
    take.captureEndSample = take.endSample;
}

//==============================================================================
void TakeSegmenter::advanceInternal (int64_t sample, const CaptureHistory& history, std::vector<Take>& closedTakes)
{
    for (;;)
    {
        // The earliest item due at or before sample: a closing tail, a pending grab or the free gap; M3.1
        // (taken only once the stream is past them, see class comment): the anchor timeout, a bar line of the
        // open take (its cut), the free-take cut, the Record start
        enum class Due { none, closing, grab, freeGap, anchor, barLine, freeCut, recordStart };
        Due due = Due::none;
        int64_t dueAt = noLimit;
        std::size_t index = 0;
        double lineSample = 0.0;

        for (std::size_t i = 0; i < closingTakes.size(); ++i)
        {
            if (closingTakes[i].take.captureEndSample <= sample && closingTakes[i].take.captureEndSample < dueAt)
            {
                due = Due::closing;
                dueAt = closingTakes[i].take.captureEndSample;
                index = i;
            }
        }

        for (std::size_t i = 0; i < pendingGrabs.size(); ++i)
        {
            if (pendingGrabs[i].captureEndSample <= sample && pendingGrabs[i].captureEndSample < dueAt)
            {
                due = Due::grab;
                dueAt = pendingGrabs[i].captureEndSample;
                index = i;
            }
        }

        if (state == State::freeRecording && hasOpenTake && numSounding == 0 && gapEndSample <= sample
            && gapEndSample < dueAt)
        {
            due = Due::freeGap;
            dueAt = gapEndSample;
        }

        if (hasOpenTake && openTake.take.mode == TakeMode::synced)
        {
            const auto& b = openTake;

            if (! b.anchored)
            {
                if (b.anchorDeadline <= sample && b.anchorDeadline < dueAt
                    && std::any_of (b.anchorCandidates.begin(), b.anchorCandidates.end(), [] (const auto& c) { return c.any(); }))
                {
                    due = Due::anchor;
                    dueAt = b.anchorDeadline;
                }
            }
            else if (b.take.maxBars > 0)
            {
                if (const auto line = b.take.timeline.barLineAfter (b.lineSample))
                {
                    const auto at = ceilSample (line->sample);

                    if (at < sample && at < dueAt)
                    {
                        due = Due::barLine;
                        dueAt = at;
                        lineSample = line->sample;
                    }
                }
            }
        }
        else if (hasOpenTake && state == State::freeRecording && openTake.freeCutSample < sample
                 && openTake.freeCutSample < dueAt)
        {
            due = Due::freeCut;
            dueAt = openTake.freeCutSample;
        }

        if (recordPhase == RecordPhase::armed && playing && state != State::off)
        {
            if (const auto line = history.getTimeline().firstBarLineAtOrAfter (static_cast<double> (recordSearchFrom)))
            {
                const auto at = ceilSample (line->sample);

                if (at < sample && at < dueAt)
                {
                    due = Due::recordStart;
                    dueAt = at;
                    lineSample = line->sample;
                }
            }
        }

        switch (due)
        {
            case Due::none: return;

            case Due::closing:
            {
                auto builder = std::move (closingTakes[index]);
                closingTakes.erase (closingTakes.begin() + static_cast<std::ptrdiff_t> (index));
                finish (std::move (builder), closedTakes);
                break;
            }

            case Due::grab:
            {
                const auto grab = pendingGrabs[index];
                pendingGrabs.erase (pendingGrabs.begin() + static_cast<std::ptrdiff_t> (index));

                if (auto take = buildGrab (history, grab.markerSample, grab.marker, grab.number, grab.captureEndSample))
                    closedTakes.push_back (std::move (*take));
                else
                    releaseNumber (grab.number);
                break;
            }

            case Due::freeGap:
            {
                const auto tail = static_cast<int64_t> (std::llround (freeTailSeconds * sampleRate));
                const auto end = std::max (openTake.take.startSample, std::min (lastNoteOffSample + tail, gapEndSample));
                endOpenTake (end, false, closedTakes);
                state = State::freeWaiting;
                break;
            }

            case Due::anchor: anchorOpenTake (openTake.take.startSample); break;

            case Due::barLine:
            {
                openTake.lineSample = lineSample;

                if (++openTake.barsDone < openTake.take.maxBars)
                    break;

                if (openTake.take.kind == TakeKind::record)
                {
                    // The Record take is complete: exactly bars X .. X + N; one shot
                    endOpenTake (dueAt, true, closedTakes);
                    recordPhase = RecordPhase::idle;
                    state = State::syncedIdle;
                }
                else
                {
                    splitAt (dueAt, TakeKind::automatic, history, closedTakes); // the next take anchors anew
                }
                break;
            }

            case Due::freeCut:
            {
                const bool sounding = numSounding > 0;
                endOpenTake (dueAt, false, closedTakes); // synthetic note-offs at the cut

                if (sounding)
                    openTakeAt (dueAt, TakeMode::free, TakeKind::automatic, history);
                else
                    state = State::freeWaiting;
                break;
            }

            case Due::recordStart:
            {
                openTakeAt (dueAt, TakeMode::synced, TakeKind::record, history);
                auto& b = openTake;
                b.anchored = true;
                b.anchorCandidates = {};
                b.lineSample = lineSample;
                b.barsDone = 0;
                b.take.maxBars = std::max (1, recordBars);
                b.take.anchorBar = b.take.timeline.barAt (static_cast<double> (dueAt)).number;
                recordPhase = RecordPhase::recording;
                break;
            }
        }
    }
}

//==============================================================================
void TakeSegmenter::handleReset (const CaptureRecord& record, const CaptureHistory& history,
                                 std::vector<Take>& closedTakes)
{
    const auto r = record.asReset();
    recordPhase = RecordPhase::idle; // nothing is armed across a prepare

    // On the old clock nothing arrives after the newest sample seen: close everything there
    const auto at = r.clearsHistory ? newestSample : std::max (newestSample, record.sample);
    advanceInternal (at, history, closedTakes);
    newestSample = at;

    if (hasOpenTake)
        endOpenTake (at, false, closedTakes);

    if (r.clearsHistory)
    {
        // The history is already cleared (it gets every record first): pending grabs cannot be built
        for (const auto& grab : pendingGrabs)
            releaseNumber (grab.number);

        pendingGrabs.clear();
        endAllTailsAt (at, history, closedTakes);
        reset();
        sampleRate = r.sampleRate > 0.0 ? r.sampleRate : 48000.0;
        maxBlockSize = r.maxBlockSize;
        return;
    }

    endAllTailsAt (at, history, closedTakes);
    maxBlockSize = r.maxBlockSize;

    // The processor flushes after prepareToPlay; the forced transport record starts the next synced take
    playing = false;
    soundingNotes = {};
    numSounding = 0;

    if (state != State::off)
        state = State::freeWaiting;
}

void TakeSegmenter::endAllTailsAt (int64_t sample, const CaptureHistory& history, std::vector<Take>& closedTakes)
{
    for (auto& builder : closingTakes)
        builder.take.captureEndSample = std::max (builder.take.endSample, std::min (builder.take.captureEndSample, sample));

    for (auto& grab : pendingGrabs)
        grab.captureEndSample = std::max (grab.markerSample, std::min (grab.captureEndSample, sample));

    // Everything is due now; advanceInternal keeps the time order
    auto maxDue = sample;

    for (const auto& builder : closingTakes)
        maxDue = std::max (maxDue, builder.take.captureEndSample);

    for (const auto& grab : pendingGrabs)
        maxDue = std::max (maxDue, grab.captureEndSample);

    const auto savedState = state;
    if (state == State::freeRecording)
        state = State::freeWaiting; // the free gap is not due here (the open take was handled by the caller)

    advanceInternal (maxDue, history, closedTakes);
    state = savedState;
}

//==============================================================================
void TakeSegmenter::handleTransport (int64_t sample, const TransportInfo& info, const CaptureHistory& history,
                                     std::vector<Take>& closedTakes)
{
    const bool nowPlaying = isPlayingTransport (info);
    hostBpm = info.hasBpm && std::isfinite (info.bpm) && info.bpm > 0.0 ? info.bpm : TakeTimeline::fallbackBpm;

    // Takes keep their own timeline (the history may evict the start of a long take)
    if (hasOpenTake && openTake.take.mode == TakeMode::synced)
        openTake.take.timeline.addTransport (sample, info);

    for (auto& builder : closingTakes)
        if (sample < builder.take.captureEndSample)
            builder.take.timeline.addTransport (sample, info);

    if (state != State::off)
    {
        if (! playing && nowPlaying)
        {
            // Transport start: a free take ends here, a synced one starts here (Record mode: armed takes
            // start at the bar line where playback starts)
            if (hasOpenTake)
                endOpenTake (sample, false, closedTakes);

            if (recordPhase == RecordPhase::armed)
                recordSearchFrom = sample;

            startSyncedState (sample, history);
        }
        else if (playing && ! nowPlaying)
        {
            // Transport stop: the synced take ends here and keeps collecting its capture tail. A Record take
            // stopped early is kept (shorter); during the count-in the Record button stays armed.
            if (hasOpenTake)
                endOpenTake (sample, true, closedTakes);

            if (recordPhase == RecordPhase::recording)
                recordPhase = RecordPhase::idle;
            else if (recordPhase == RecordPhase::armed)
                recordSearchFrom = sample;

            freeStartAllowedFrom = std::max (freeStartAllowedFrom, sample + tailSamples());
            state = State::freeWaiting;
        }
    }

    playing = nowPlaying;
}

void TakeSegmenter::handleEvent (int64_t sample, const EventRecord& record, const CaptureHistory& history,
                                 std::vector<Take>& closedTakes)
{
    const auto captured = makeCaptured (sample, record);
    const auto& e = captured.event;
    const auto ch = channelIndex (e);
    const auto note = noteIndex (e);

    if (isNoteOn (e))
    {
        if (state == State::freeWaiting && sample >= freeStartAllowedFrom && backgroundMode())
        {
            openTakeAt (sample, TakeMode::free, TakeKind::automatic, history);

            if ((record.timing.flags & EventTimingFlags::resync) != 0)
                openTake.take.incomplete = true; // starts with a note re-stated after a gap
        }

        appendEvent (captured, history, closedTakes);
        trackAnchor (captured);

        if (! soundingNotes[ch][note])
        {
            soundingNotes[ch][note] = true;
            ++numSounding;
        }

        soundingVelocity[ch][note] = e.data2;
        return;
    }

    appendEvent (captured, history, closedTakes); // unpaired note-offs too (the renderer drops them)
    trackAnchor (captured);

    if (isNoteOff (e) && soundingNotes[ch][note])
    {
        soundingNotes[ch][note] = false;

        if (--numSounding == 0)
        {
            lastNoteOffSample = sample;
            gapEndSample = sample + gapSamples();
        }
    }
}

void TakeSegmenter::handleMarker (int64_t sample, const MarkerRecord& marker, const CaptureHistory& history,
                                  std::vector<Take>& closedTakes)
{
    switch (marker.kind)
    {
        case CaptureMarkerKind::newTake:
            if (state != State::off && backgroundMode()) // Record mode: disabled (rule 9)
                splitAt (sample, TakeKind::manual, history, closedTakes);
            break;

        case CaptureMarkerKind::grab:
            pendingGrabs.push_back ({ sample, grabCaptureEnd (history, sample), marker, nextTakeNumber++ });
            break;

        case CaptureMarkerKind::clear:
            // Discard everything in progress (numbers are not reused); the next take starts by the rules
            ++clearCount;
            hasOpenTake = false;
            openTake = {};
            closingTakes.clear();
            pendingGrabs.clear();
            freeStartAllowedFrom = sample;
            recordPhase = RecordPhase::idle;

            if (state != State::off)
            {
                state = State::freeWaiting;

                if (playing)
                    startSyncedState (sample, history);
            }
            break;

        case CaptureMarkerKind::captureOn:
            if (state == State::off)
            {
                state = State::freeWaiting;

                if (playing)
                    startSyncedState (sample, history);
            }
            break;

        case CaptureMarkerKind::captureOff:
            // Nothing is recorded after the marker: no capture tail (the tap ended the notes before it)
            if (hasOpenTake)
                endOpenTake (sample, false, closedTakes);

            recordPhase = RecordPhase::idle;
            endAllTailsAt (sample, history, closedTakes);
            soundingNotes = {};
            numSounding = 0;
            state = State::off;
            break;

        case CaptureMarkerKind::recordArm:
        case CaptureMarkerKind::recordCancel: handleRecordMarker (sample, marker); break;

        case CaptureMarkerKind::audioOn:
        case CaptureMarkerKind::audioOff:
        case CaptureMarkerKind::bypassOn:
        case CaptureMarkerKind::bypassOff: break;
    }
}

void TakeSegmenter::handleGap (int64_t sample)
{
    if (hasOpenTake)
        openTake.take.incomplete = true;

    // A note that sounded at the take start may have lost its note-off: it anchors the take at its start
    if (hasOpenTake && ! openTake.anchored
        && std::any_of (openTake.anchorCandidates.begin(), openTake.anchorCandidates.end(), [] (const auto& c) { return c.any(); }))
        anchorOpenTake (openTake.take.startSample);

    for (auto& builder : closingTakes)
        if (sample < builder.take.captureEndSample)
            builder.take.incomplete = true;

    if (numSounding == 0)
        return;

    // A lost note-off must not keep a note sounding forever: end every note here; the tap's resync
    // note-ons that follow re-open the notes that are really still on
    if (hasOpenTake)
        closeNotes (openTake.take, openTake.notes, sample);

    for (auto& builder : closingTakes)
        if (sample < builder.take.captureEndSample)
            closeNotes (builder.take, builder.notes, sample);

    soundingNotes = {};
    numSounding = 0;
    lastNoteOffSample = sample;
    gapEndSample = sample + gapSamples();
}

//==============================================================================
void TakeSegmenter::openTakeAt (int64_t sample, TakeMode mode, TakeKind kind, const CaptureHistory& history)
{
    Builder builder;
    auto& take = builder.take;
    take.number = nextTakeNumber++;
    take.mode = mode;
    take.kind = kind;
    take.startSample = sample;
    take.endSample = sample;
    take.captureEndSample = sample;
    take.sampleRate = sampleRate;
    take.maxBlockSize = maxBlockSize;
    take.settings = settings;
    take.freeBpm = hostBpm;
    take.clockEpoch = history.getClockEpoch();
    builder.collectFrom = sample;

    if (mode == TakeMode::synced && kind != TakeKind::record)
    {
        // Rule 7: anchored at its first note; notes sounding now are candidates for the start
        builder.anchored = false;
        builder.anchorCandidates = soundingNotes;
        builder.anchorDeadline = sample + tailSamples();
    }
    else if (mode == TakeMode::free && backgroundMode() && settings.takeMaxBars > 0)
    {
        // Rule 8: free "bars" are 4/4 at the last host tempo (else 120 BPM)
        take.maxBars = settings.takeMaxBars;
        builder.freeCutSample = sample + std::max<int64_t> (1, freeModeBarsToSamples (take.maxBars, sampleRate, take.freeBpm));
    }

    if (mode == TakeMode::synced)
        take.timeline = history.getTimeline().slice (sample, sample);
    else
        take.timeline.reset (sampleRate);

    // Notes sounding at the start are opened there (synthetic, original velocity)
    for (std::size_t ch = 0; ch < soundingNotes.size(); ++ch)
    {
        if (! soundingNotes[ch].any())
            continue;

        for (std::size_t note = 0; note < 128; ++note)
            if (soundingNotes[ch][note])
                addToTake (take, builder.notes,
                           makeSynthetic (sample, VsEvent::makeNoteOn (0, static_cast<uint8_t> (ch + 1),
                                                                       static_cast<uint8_t> (note),
                                                                       soundingVelocity[ch][note])));
    }

    openTake = std::move (builder);
    hasOpenTake = true;
    state = mode == TakeMode::synced ? State::synced : State::freeRecording;

    if (mode == TakeMode::free && numSounding == 0)
    {
        // A manual free take without a sounding note waits takeGapSeconds for one
        lastNoteOffSample = sample;
        gapEndSample = sample + gapSamples();
    }
}

void TakeSegmenter::splitAt (int64_t sample, TakeKind kind, const CaptureHistory& history,
                             std::vector<Take>& closedTakes)
{
    const bool synced = state == State::synced;
    const bool record = hasOpenTake && openTake.take.kind == TakeKind::record;
    const auto recordTake = record ? openTake.take.maxBars : 0;
    const auto recordBar = record ? openTake.take.anchorBar : 0;
    const auto recordLine = openTake.lineSample;
    const auto recordDone = openTake.barsDone;

    if (hasOpenTake)
        endOpenTake (sample, synced, closedTakes); // free: synthetic note-offs at the split

    openTakeAt (sample, synced ? TakeMode::synced : TakeMode::free, record ? TakeKind::record : kind, history);

    if (record)
    {
        // A Record take split for the event limit continues to the same end bar line
        openTake.take.maxBars = recordTake;
        openTake.take.anchorBar = recordBar;
        openTake.lineSample = recordLine;
        openTake.barsDone = recordDone;
    }
}

void TakeSegmenter::endOpenTake (int64_t endSample, bool withTail, std::vector<Take>& closedTakes)
{
    auto builder = std::move (openTake);
    openTake = {};
    hasOpenTake = false;

    auto& take = builder.take;
    take.endSample = std::max (take.startSample, endSample);

    if (take.mode == TakeMode::synced && withTail)
    {
        take.captureEndSample = take.endSample + tailSamples();
        closingTakes.push_back (std::move (builder));
        return;
    }

    take.captureEndSample = take.endSample;

    // Nothing after the end belongs to the take (free: bends / CCs in the silence before the gap closed it).
    // Note-offs AT the end stay: they close notes at the boundary (the tap's capture-only note-offs come
    // right before a captureOff marker at the same sample). The open-note set is rebuilt from what is
    // left, so finish() closes exactly the notes the remaining events leave open.
    const auto end = take.endSample;
    const auto removed = std::erase_if (take.events, [end] (const CapturedEvent& e)
                                        { return e.sample > end || (e.sample == end && ! isNoteOff (e.event)); });

    if (removed > 0)
    {
        builder.notes = {};

        for (const auto& e : take.events)
        {
            if (isNoteOn (e.event))
                builder.notes[channelIndex (e.event)][noteIndex (e.event)] = true;
            else if (isNoteOff (e.event))
                builder.notes[channelIndex (e.event)][noteIndex (e.event)] = false;
        }
    }

    finish (std::move (builder), closedTakes);
}

void TakeSegmenter::finish (Builder&& builder, std::vector<Take>& closedTakes)
{
    auto& take = builder.take;
    closeNotes (take, builder.notes, take.captureEndSample);
    take.noteCount = countNoteOns (take);

    if (take.noteCount == 0)
    {
        releaseNumber (take.number); // never listed
        return;
    }

    take.closed = true;
    take.audio = TakeAudio::unknown;
    closedTakes.push_back (std::move (take));
}

void TakeSegmenter::releaseNumber (int number) noexcept
{
    // A number that was never listed may be given again, but only if no later take holds a newer one
    if (number == nextTakeNumber - 1)
        --nextTakeNumber;
}

void TakeSegmenter::appendEvent (const CapturedEvent& event, const CaptureHistory& history,
                                 std::vector<Take>& closedTakes)
{
    if (hasOpenTake && event.sample >= openTake.collectFrom)
    {
        if (openTake.take.events.size() >= maxTakeEvents - splitReserveEvents)
            splitAt (event.sample, TakeKind::automatic, history, closedTakes);

        addToTake (openTake.take, openTake.notes, event);
    }

    for (auto& builder : closingTakes)
    {
        if (event.sample >= builder.take.captureEndSample)
            continue;

        if (builder.take.events.size() >= maxTakeEvents)
        {
            builder.take.incomplete = true;
            continue;
        }

        addToTake (builder.take, builder.notes, event);
    }
}

//==============================================================================
void TakeSegmenter::startSyncedState (int64_t sample, const CaptureHistory& history)
{
    if (backgroundMode())
        openTakeAt (sample, TakeMode::synced, TakeKind::automatic, history);
    else
        state = State::syncedIdle;
}

void TakeSegmenter::handleSettings (int64_t sample, const CaptureSettingsInfo& info, const CaptureHistory& history,
                                    std::vector<Take>& closedTakes)
{
    const auto previousMode = settings.captureMode;
    settings = info;

    if (state == State::off || previousMode == info.captureMode)
        return;

    if (info.captureMode == CaptureMode::record)
    {
        // Background -> Record: the background take ends here (like newTake), no new one opens
        if (hasOpenTake)
            endOpenTake (sample, openTake.take.mode == TakeMode::synced, closedTakes);

        recordPhase = RecordPhase::idle;
        state = playing ? State::syncedIdle : State::freeWaiting;
        return;
    }

    // Record -> Background: the arm is dropped, a Record take ends here (kept, shorter), the background
    // rules resume at this sample
    recordPhase = RecordPhase::idle;

    if (hasOpenTake)
        endOpenTake (sample, true, closedTakes);

    state = State::freeWaiting;

    if (playing)
        openTakeAt (sample, TakeMode::synced, TakeKind::automatic, history);
}

void TakeSegmenter::handleRecordMarker (int64_t sample, const MarkerRecord& marker)
{
    if (marker.kind == CaptureMarkerKind::recordArm)
    {
        if (state == State::off || backgroundMode() || recordPhase != RecordPhase::idle)
            return;

        recordPhase = RecordPhase::armed;
        recordBars = std::max (1, marker.grabBars);
        recordSearchFrom = sample;
        return;
    }

    // recordCancel
    if (recordPhase == RecordPhase::recording && hasOpenTake && openTake.take.kind == TakeKind::record)
    {
        // Discarded: nothing is listed (the history still has it for a grab)
        releaseNumber (openTake.take.number);
        hasOpenTake = false;
        openTake = {};
        state = playing ? State::syncedIdle : State::freeWaiting;
    }

    recordPhase = RecordPhase::idle;
}

void TakeSegmenter::trackAnchor (const CapturedEvent& event)
{
    if (! hasOpenTake || openTake.anchored || openTake.take.mode != TakeMode::synced)
        return;

    auto& b = openTake;
    const auto& e = event.event;
    const auto h = event.estimatedSample - segmentRoundTrip (b.take);
    const auto ch = channelIndex (e);
    const auto note = noteIndex (e);
    const auto start = b.take.startSample;
    const auto pending = std::any_of (b.anchorCandidates.begin(), b.anchorCandidates.end(), [] (const auto& c) { return c.any(); });

    if (isNoteOn (e))
    {
        if (h >= start)
            anchorOpenTake (pending ? start : h); // a note still sounding from the start decides first
        else
            b.anchorCandidates[ch][note] = true;  // sung before the start: anchors only if held past it

        return;
    }

    if (isNoteOff (e) && b.anchorCandidates[ch][note])
    {
        b.anchorCandidates[ch][note] = false;

        if (h > start)
            anchorOpenTake (start); // held across the start (a note held over the cut: bar X + N)
    }
}

void TakeSegmenter::anchorOpenTake (int64_t at)
{
    auto& b = openTake;
    auto& take = b.take;
    b.anchored = true;
    b.anchorCandidates = {};

    // The limit in force now applies to this take; Off: nothing changes (0.3.0)
    if (! backgroundMode() || settings.takeMaxBars <= 0)
        return;

    const auto bar = take.timeline.barAt (static_cast<double> (std::max (at, take.startSample)));
    const auto barStart = take.timeline.sampleAtBeats (bar.startBeats);
    take.maxBars = settings.takeMaxBars;
    take.anchorBar = bar.number;
    b.lineSample = barStart;
    b.barsDone = 0;

    // Pre-roll before bar X can never pull the origin earlier: the take is at most maxBars long
    take.startSample = std::max (take.startSample, ceilSample (barStart));
}

void TakeSegmenter::updateRecordStatus (const CaptureHistory& history) noexcept
{
    RecordStatus r;

    switch (recordPhase)
    {
        case RecordPhase::idle: break;

        case RecordPhase::armed:
        {
            r.phase = RecordStatus::Phase::waitingForPlay;
            r.bars = recordBars;

            if (! playing)
                break;

            const auto& timeline = history.getTimeline();

            if (const auto line = timeline.firstBarLineAtOrAfter (static_cast<double> (recordSearchFrom)))
            {
                r.phase = RecordStatus::Phase::countIn;
                r.startBar = timeline.barAt (line->sample).number;
                r.beatsToStart = std::max (0.0, line->beats - timeline.beatsAt (static_cast<double> (newestSample)));
            }
            break;
        }

        case RecordPhase::recording:
            r.phase = RecordStatus::Phase::recording;

            if (hasOpenTake)
            {
                r.bars = openTake.take.maxBars;
                r.startBar = openTake.take.anchorBar;
                r.barInTake = std::min (openTake.barsDone + 1, openTake.take.maxBars);
                r.takeNumber = openTake.take.number;
            }
            break;
    }

    recordStatus = r;
}

//==============================================================================
int64_t TakeSegmenter::grabCaptureEnd (const CaptureHistory& history, int64_t markerSample) noexcept
{
    const auto sr = history.getSampleRate();

    if (history.getTimeline().isPlayingAt (static_cast<double> (markerSample)))
        return markerSample + compensationTailSamples (sr, history.getMaxBlockSize());

    return markerSample + static_cast<int64_t> (std::llround (detectionTailSeconds * sr));
}

std::optional<Take> TakeSegmenter::makeGrab (const CaptureHistory& history, int64_t markerSample,
                                             const MarkerRecord& marker, int number)
{
    return buildGrab (history, markerSample, marker, number, noLimit);
}

std::optional<Take> TakeSegmenter::buildGrab (const CaptureHistory& history, int64_t markerSample,
                                              const MarkerRecord& marker, int number, int64_t captureEndLimit)
{
    const auto& timeline = history.getTimeline();
    const auto sr = history.getSampleRate();
    const auto marker_ = static_cast<double> (markerSample);
    const bool synced = timeline.isPlayingAt (marker_);
    const auto captureEnd = std::max (markerSample, std::min (grabCaptureEnd (history, markerSample), captureEndLimit));

    int64_t start = 0;

    if (synced)
    {
        // From the start of the bar grabBars - 1 bars before the bar containing the marker
        auto bar = timeline.barAt (marker_);

        for (int i = 1; i < std::max (1, marker.grabBars); ++i)
        {
            const auto before = timeline.sampleAtBeats (bar.startBeats - 2.0 * TakeTimeline::barSnapToleranceBeats);
            const auto previous = timeline.barAt (before);

            if (! (previous.startBeats < bar.startBeats))
                break;

            bar = previous;
        }

        start = static_cast<int64_t> (std::llround (timeline.sampleAtBeats (bar.startBeats)));

        // Not before the transport start of this playing run (stopped time has no beats)
        const auto& segments = timeline.getSegments();
        const auto* seg = timeline.segmentAt (marker_);
        auto index = static_cast<std::size_t> (seg - segments.data());

        while (index > 0 && segments[index - 1].playing)
            --index;

        start = std::max (start, segments[index].startSample);
    }
    else
    {
        start = markerSample - static_cast<int64_t> (std::llround (std::max (0.0, marker.grabSeconds) * sr));
    }

    start = std::clamp (start, history.getOldestEventSample(), markerSample);

    Take take;
    take.number = number;
    take.mode = synced ? TakeMode::synced : TakeMode::free;
    take.kind = TakeKind::grab;
    take.startSample = start;
    take.endSample = markerSample;
    take.captureEndSample = captureEnd;
    take.sampleRate = sr;
    take.maxBlockSize = history.getMaxBlockSize();
    take.settings = history.getSettingsAt (start);
    take.freeBpm = timeline.empty() ? TakeTimeline::fallbackBpm : timeline.bpmAt (marker_);
    take.clockEpoch = history.getClockEpoch();

    // Segments of [start, captureEnd) only: a transport record AT captureEnd may or may not be in the
    // history yet when the grab is built (it arrives with the record that triggers the build, or with a
    // later drain), so including it would make the grab depend on the drain pattern.
    if (synced)
        take.timeline = timeline.slice (start, std::max (start, captureEnd - 1));
    else
        take.timeline.reset (sr);

    // Note state at the start from everything the history still holds before it
    std::array<std::bitset<128>, 16> sounding {};
    std::array<std::array<uint8_t, 128>, 16> velocity {};
    std::array<std::bitset<128>, 16> notes {};
    const auto records = history.copyRecords (history.getOldestEventSample(), captureEnd);
    bool opened = false;

    for (const auto& record : records)
    {
        if (! opened && record.sample >= start)
        {
            for (std::size_t ch = 0; ch < sounding.size(); ++ch)
                for (std::size_t note = 0; note < 128; ++note)
                    if (sounding[ch][note])
                        addToTake (take, notes,
                                   makeSynthetic (start, VsEvent::makeNoteOn (0, static_cast<uint8_t> (ch + 1),
                                                                              static_cast<uint8_t> (note),
                                                                              velocity[ch][note])));
            opened = true;
        }

        if (record.type == CaptureRecordType::gap)
        {
            sounding = {};

            if (opened)
            {
                take.incomplete = true;
                closeNotes (take, notes, record.sample);
            }
            continue;
        }

        if (record.type != CaptureRecordType::event)
            continue;

        const auto ev = record.asEvent();
        const auto ch = channelIndex (ev.event);
        const auto note = noteIndex (ev.event);

        if (isNoteOn (ev.event))
        {
            sounding[ch][note] = true;
            velocity[ch][note] = ev.event.data2;
        }
        else if (isNoteOff (ev.event))
        {
            sounding[ch][note] = false;
        }

        if (opened)
            addToTake (take, notes, makeCaptured (record.sample, ev));
    }

    if (! opened)
    {
        for (std::size_t ch = 0; ch < sounding.size(); ++ch)
            for (std::size_t note = 0; note < 128; ++note)
                if (sounding[ch][note])
                    addToTake (take, notes,
                               makeSynthetic (start, VsEvent::makeNoteOn (0, static_cast<uint8_t> (ch + 1),
                                                                          static_cast<uint8_t> (note),
                                                                          velocity[ch][note])));
    }

    closeNotes (take, notes, captureEnd);
    take.noteCount = countNoteOns (take);

    if (take.noteCount == 0)
        return std::nullopt;

    take.closed = true;
    take.audio = TakeAudio::unknown;
    return take;
}

} // namespace vocalsynth
