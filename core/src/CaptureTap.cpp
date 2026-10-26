#include "vocalsynth/CaptureTap.h"

#include "vocalsynth/TakeTimeline.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace vocalsynth
{

static_assert (CaptureTap::resyncLimitBeats == TakeTimeline::jumpThresholdBeats,
               "the tap's re-sync class must match the timeline's re-sync / jump rule");
static_assert ((CaptureTap::recordRingCapacity & (CaptureTap::recordRingCapacity - 1)) == 0);

namespace
{
/** Single writer (the audio thread): load + store instead of a locked read-modify-write. */
void addRelaxed (std::atomic<uint64_t>& counter, uint64_t n) noexcept
{
    counter.store (counter.load (std::memory_order_relaxed) + n, std::memory_order_relaxed);
}

uint8_t clampData (int value) noexcept
{
    return static_cast<uint8_t> (std::clamp (value, 0, 127));
}

/** What the timeline would accept: NaN / non-positive values count as "not reported". Applied before the
    comparison, so a host that reports NaN cannot cause a record per block. */
TransportInfo sanitise (TransportInfo t) noexcept
{
    if (! std::isfinite (t.ppqPosition))
    {
        t.hasPpq = false;
        t.ppqPosition = 0.0;
    }

    if (! (std::isfinite (t.bpm) && t.bpm > 0.0))
    {
        t.hasBpm = false;
        t.bpm = TakeTimeline::fallbackBpm;
    }

    if (! std::isfinite (t.ppqPositionOfLastBarStart) || ! t.hasPpq)
    {
        t.hasBarStart = false;
        t.ppqPositionOfLastBarStart = t.ppqPosition;
    }

    if (t.timeSigNumerator < 1 || t.timeSigDenominator < 1)
        t.hasTimeSignature = false;

    return t;
}

double barLengthOf (const TransportInfo& t) noexcept
{
    if (! t.hasTimeSignature)
        return 4.0;

    return static_cast<double> (t.timeSigNumerator) * 4.0 / static_cast<double> (t.timeSigDenominator);
}
} // namespace

void CaptureTap::prepare (double newSampleRate, int newMaxBlockSize)
{
    const auto rate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    resetClearsHistory = ! (rate == sampleRate) || clock == 0;

    // Rings: the worker drained them in suspend(). The audio ring only grows (a lower rate keeps it).
    records.prepare (recordRingCapacity);
    const auto audioWanted = std::bit_ceil (static_cast<std::size_t> (std::ceil (audioRingSeconds * rate)));
    audio.prepare (std::max (audioWanted, audio.getCapacity()));

    if (resetClearsHistory)
    {
        clock = 0;
        openNotes = {};
        openVelocity = {};
        openNoteCount = 0;
        bendNotCentred.reset();
        frameFill = 0;
        enabled = false;
        audioOn = false;
        bypassed = false;
        hasLastTransport = false;
        hasLastSettings = false;
        gapPending = false;
        pendingDroppedRecords = 0;
        pendingDroppedAudio = 0;
        highWater = 0;
    }

    sampleRate = rate;
    maxBlockSize = std::max (0, newMaxBlockSize);
    resyncIntervalSamples = std::max<int64_t> (1, std::llround (minResyncIntervalSeconds * rate));
    stoppedIntervalSamples = std::max<int64_t> (1, std::llround (stoppedPositionIntervalSeconds * rate));
    droppedRecordsTotal.store (0, std::memory_order_relaxed);
    droppedAudioTotal.store (0, std::memory_order_relaxed);
    resetPending = true;
    forceStateRecords = true;
    publishedClock.store (clock, std::memory_order_release);
}

//==============================================================================
void CaptureTap::beginBlock (const TransportInfo& transport, const CaptureBlockSettings& settings) noexcept
{
    if (resetPending)
    {
        const ResetRecord reset { sampleRate, maxBlockSize, resetClearsHistory };
        pushRecord (CaptureRecord::makeReset (resetClearsHistory ? 0 : clock, reset), clock);
        resetPending = false;
    }

    if (enabled && ! settings.captureEnabled)
    {
        // Capture-only note-offs (the synth keeps its notes; the tap keeps tracking them)
        pushOpenNoteOffs();

        if (audioOn)
            stopAudio();

        pushMarker (CaptureMarkerKind::captureOff);
        enabled = false;
        audioOn = false;
        bypassed = false;
    }

    if (settings.captureEnabled)
    {
        const bool turningOn = ! enabled;
        const bool force = forceStateRecords || turningOn;
        forceStateRecords = false;

        pushTransportIfChanged (sanitise (transport), force);

        if (force || ! hasLastSettings || ! (settings.settings == lastSettings))
        {
            pushRecord (CaptureRecord::makeSettings (clock, settings.settings), clock);
            lastSettings = settings.settings;
            hasLastSettings = true;
        }

        if (turningOn)
        {
            pushMarker (CaptureMarkerKind::captureOn);
            enabled = true;
        }

        if (settings.captureAudio != audioOn)
        {
            if (settings.captureAudio)
            {
                startAudio();
                pushMarker (CaptureMarkerKind::audioOn);
            }
            else
            {
                stopAudio();
                pushMarker (CaptureMarkerKind::audioOff);
            }

            audioOn = settings.captureAudio;
        }

        if (settings.bypassed != bypassed)
        {
            pushMarker (settings.bypassed ? CaptureMarkerKind::bypassOn : CaptureMarkerKind::bypassOff);
            bypassed = settings.bypassed;
        }
    }

    // UI requests: at most one marker per kind per block (recorded in any state; the segmenter decides)
    if (const auto n = newTakeRequests.load (std::memory_order_acquire); n != handledNewTake)
    {
        handledNewTake = n;
        pushMarker (CaptureMarkerKind::newTake);
    }

    if (const auto n = grabRequests.load (std::memory_order_acquire); n != handledGrab)
    {
        handledGrab = n;
        pushMarker (CaptureMarkerKind::grab, grabBars.load (std::memory_order_relaxed),
                    grabSeconds.load (std::memory_order_relaxed));
    }

    if (const auto n = clearRequests.load (std::memory_order_acquire); n != handledClear)
    {
        handledClear = n;
        pushMarker (CaptureMarkerKind::clear);
    }

    // Record / Cancel: one word, so presses between two blocks keep their order (the last one wins: a
    // Cancel after Record is not lost; Cancel then Record discards / disarms and arms again)
    const auto record = recordRequestState.load (std::memory_order_acquire);

    if (const auto n = static_cast<uint32_t> ((record >> recordCancelShift) & recordCountMask); n != handledRecordCancel)
    {
        handledRecordCancel = n;
        pushMarker (CaptureMarkerKind::recordCancel);
    }

    if (const auto n = static_cast<uint32_t> ((record >> recordArmShift) & recordCountMask); n != handledRecord)
    {
        handledRecord = n;

        if ((record & recordLastIsArm) != 0)
            pushMarker (CaptureMarkerKind::recordArm, static_cast<int> (record & recordBarsMask));
    }
}

void CaptureTap::flushNotes() noexcept
{
    for (std::size_t ch = 0; ch < openNotes.size(); ++ch)
    {
        const auto channel = static_cast<uint8_t> (ch + 1);

        if (openNotes[ch].any())
        {
            for (std::size_t note = 0; note < 128; ++note)
            {
                if (openNotes[ch][note] && enabled)
                    pushRecord (CaptureRecord::makeEvent (clock,
                                                          VsEvent::makeNoteOff (0, channel, static_cast<uint8_t> (note)),
                                                          {}),
                                clock);
            }
        }

        if (bendNotCentred[ch] && enabled)
            pushRecord (CaptureRecord::makeEvent (clock, VsEvent::makePitchBend (0, channel, 0), {}), clock);

        openNotes[ch].reset();
    }

    openNoteCount = 0;
    bendNotCentred.reset();
}

void CaptureTap::processChunk (const float* mono, int numSamples, std::span<const VsEvent> events,
                               std::span<const VsEventTiming> timings) noexcept
{
    if (numSamples <= 0)
        return;

    for (std::size_t i = 0; i < events.size(); ++i)
    {
        const auto& e = events[i];
        const auto offset = std::clamp (e.sampleOffset, 0, numSamples - 1);
        pushEvent (e, i < timings.size() ? timings[i] : VsEventTiming {}, clock + offset);
    }

    if (audioOn)
        stageAudio (mono, numSamples);

    advanceClock (numSamples);
}

void CaptureTap::processBypassed (int numSamples) noexcept
{
    if (numSamples <= 0)
        return;

    if (audioOn)
        stageAudio (nullptr, numSamples);

    advanceClock (numSamples);
}

//==============================================================================
void CaptureTap::requestNewTake() noexcept
{
    newTakeRequests.fetch_add (1, std::memory_order_release);
}

void CaptureTap::requestGrab (int bars, double seconds) noexcept
{
    grabBars.store (bars, std::memory_order_relaxed);
    grabSeconds.store (seconds, std::memory_order_relaxed);
    grabRequests.fetch_add (1, std::memory_order_release);
}

void CaptureTap::requestClear() noexcept
{
    clearRequests.fetch_add (1, std::memory_order_release);
}

void CaptureTap::requestRecord (int bars) noexcept
{
    const auto clamped = static_cast<uint64_t> (std::clamp (bars, 1, static_cast<int> (recordBarsMask)));
    auto old = recordRequestState.load (std::memory_order_relaxed);
    uint64_t next = 0;

    do
    {
        const auto arms = (((old >> recordArmShift) & recordCountMask) + 1) & recordCountMask;
        next = (old & (recordCountMask << recordCancelShift)) | (arms << recordArmShift) | recordLastIsArm | clamped;
    } while (! recordRequestState.compare_exchange_weak (old, next, std::memory_order_release, std::memory_order_relaxed));
}

void CaptureTap::requestRecordCancel() noexcept
{
    auto old = recordRequestState.load (std::memory_order_relaxed);
    uint64_t next = 0;

    do
    {
        const auto cancels = (((old >> recordCancelShift) & recordCountMask) + 1) & recordCountMask;
        next = (old & ((recordCountMask << recordArmShift) | recordBarsMask)) | (cancels << recordCancelShift);
    } while (! recordRequestState.compare_exchange_weak (old, next, std::memory_order_release, std::memory_order_relaxed));
}

CaptureTap::OverflowCounts CaptureTap::getOverflowCounts() const noexcept
{
    return { droppedRecordsTotal.load (std::memory_order_relaxed), droppedAudioTotal.load (std::memory_order_relaxed) };
}

//==============================================================================
void CaptureTap::pushTransportIfChanged (const TransportInfo& transport, bool force) noexcept
{
    const auto q = quantiseTransport (transport);
    bool push = force || ! hasLastTransport;

    if (! push)
    {
        const auto& last = lastTransport;
        const auto elapsed = clock - lastTransportSample;

        bool structural = q.isPlaying != last.isPlaying || q.hasPpq != last.hasPpq || q.hasBpm != last.hasBpm
                          || q.hasTimeSignature != last.hasTimeSignature || q.hasBarStart != last.hasBarStart
                          || q.isLooping != last.isLooping
                          || (q.hasTimeSignature
                              && (q.timeSigNumerator != last.timeSigNumerator
                                  || q.timeSigDenominator != last.timeSigDenominator));
        bool resync = false;
        bool stoppedChange = false;
        const bool wasPlaying = last.isPlaying && last.hasPpq;

        if (! structural)
        {
            if (q.hasBpm && q.bpm != last.bpm)
                resync = true;

            if (q.hasPpq)
            {
                if (wasPlaying)
                {
                    const auto bpm = last.hasBpm ? last.bpm : TakeTimeline::fallbackBpm;
                    const auto predicted = last.ppqPosition + bpm * static_cast<double> (elapsed) / (60.0 * sampleRate);
                    const auto diff = std::abs (q.ppqPosition - predicted);

                    if (diff >= resyncLimitBeats)
                        structural = true; // loop wrap, relocation
                    else if (diff > ppqToleranceBeats)
                        resync = true;
                }
                else if (q.ppqPosition != last.ppqPosition)
                {
                    stoppedChange = true;
                }
            }

            if (! structural && q.hasBarStart)
            {
                // The bar start only has to stay on the previous record's bar grid: crossing a bar line
                // inside a block (or a host that reports the previous bar exactly at a bar line) is no change
                const auto barLength = barLengthOf (last);
                const auto bars = std::round ((q.ppqPositionOfLastBarStart - last.ppqPositionOfLastBarStart) / barLength);
                const auto predictedBarStart = last.ppqPositionOfLastBarStart + bars * barLength;

                if (std::abs (q.ppqPositionOfLastBarStart - predictedBarStart) > barStartToleranceBeats)
                    (wasPlaying ? resync : stoppedChange) = true;
            }
        }

        push = structural || (resync && elapsed >= resyncIntervalSamples)
               || (stoppedChange && elapsed >= stoppedIntervalSamples);
    }

    if (! push)
        return;

    pushRecord (CaptureRecord::makeTransport (clock, transport), clock);
    lastTransport = q;
    lastTransportSample = clock;
    hasLastTransport = true;
}

void CaptureTap::pushMarker (CaptureMarkerKind kind, int bars, double seconds) noexcept
{
    pushRecord (CaptureRecord::makeMarker (clock, MarkerRecord { kind, bars, seconds }), clock);
}

void CaptureTap::pushOpenNoteOffs() noexcept
{
    if (openNoteCount == 0)
        return;

    for (std::size_t ch = 0; ch < openNotes.size(); ++ch)
    {
        if (! openNotes[ch].any())
            continue;

        for (std::size_t note = 0; note < 128; ++note)
            if (openNotes[ch][note])
                pushRecord (CaptureRecord::makeEvent (clock,
                                                      VsEvent::makeNoteOff (0, static_cast<uint8_t> (ch + 1),
                                                                            static_cast<uint8_t> (note)),
                                                      {}),
                            clock);
    }
}

void CaptureTap::pushEvent (const VsEvent& e, const VsEventTiming& timing, int64_t sample) noexcept
{
    // Exactly what MidiOutputStage::writeEvents sends (binding, docs/M3-DESIGN.md 4)
    const auto channel = static_cast<uint8_t> (std::clamp (static_cast<int> (e.channel), 1, 16));
    const auto ch = static_cast<std::size_t> (channel - 1);
    auto t = timing;
    t.flags = static_cast<uint8_t> (t.flags & ~EventTimingFlags::resync); // set by the tap only

    const auto emit = [this, sample, &t] (const VsEvent& out) noexcept
    {
        if (enabled)
            pushRecord (CaptureRecord::makeEvent (sample, out, t), sample);
    };

    switch (e.type)
    {
        case VsEventType::noteOn:
        {
            const auto note = clampData (e.data1);
            const auto velocity = clampData (e.data2);
            auto&& open = openNotes[ch][note];

            if (velocity == 0)
            {
                if (open)
                {
                    open = false;
                    --openNoteCount;
                    emit (VsEvent::makeNoteOff (0, channel, note));
                }
                break;
            }

            if (open)
                emit (VsEvent::makeNoteOff (0, channel, note)); // no duplicate note-ons downstream
            else
                ++openNoteCount;

            open = true;
            openVelocity[ch][note] = velocity;
            emit (VsEvent::makeNoteOn (0, channel, note, velocity));
            break;
        }

        case VsEventType::noteOff:
        {
            const auto note = clampData (e.data1);
            auto&& open = openNotes[ch][note];

            if (! open)
                break; // never sent (or already off): dropped like the output stage does

            open = false;
            --openNoteCount;
            auto off = VsEvent::makeNoteOff (0, channel, note);
            off.data2 = clampData (e.data2);
            emit (off);
            break;
        }

        case VsEventType::pitchBend:
        {
            const auto bend = static_cast<int16_t> (std::clamp (static_cast<int> (e.bend), -8192, 8191));
            bendNotCentred[ch] = bend != 0;
            emit (VsEvent::makePitchBend (0, channel, bend));
            break;
        }

        case VsEventType::controlChange:
            emit (VsEvent::makeControlChange (0, channel, clampData (e.data1), clampData (e.data2)));
            break;
    }
}

//==============================================================================
void CaptureTap::startAudio() noexcept
{
    // Frame k covers [256 k, 256 k + 256): the part before the switch is silence
    frameFill = static_cast<int> (clock % audioFrameSamples);
    std::fill_n (frame.begin(), frameFill, 0.0f);
}

void CaptureTap::stopAudio() noexcept
{
    if (frameFill > 0)
    {
        std::fill (frame.begin() + frameFill, frame.end(), 0.0f);
        pushAudioFrame (clock - frameFill);
    }

    frameFill = 0;
}

void CaptureTap::stageAudio (const float* mono, int numSamples) noexcept
{
    int done = 0;

    while (done < numSamples)
    {
        const auto n = std::min (numSamples - done, audioFrameSamples - frameFill);
        const auto dest = frame.begin() + frameFill;

        if (mono != nullptr)
            std::copy_n (mono + done, n, dest);
        else
            std::fill_n (dest, n, 0.0f);

        frameFill += n;
        done += n;

        if (frameFill == audioFrameSamples)
        {
            pushAudioFrame (clock + done - audioFrameSamples);
            frameFill = 0;
        }
    }
}

void CaptureTap::pushAudioFrame (int64_t frameStart) noexcept
{
    const auto frameEnd = frameStart + audioFrameSamples;

    // Frame and its record go together or not at all (the worker places audio only through records)
    const bool fits = audio.freeSpace() >= static_cast<std::size_t> (audioFrameSamples)
                      && (! gapPending || pushPendingGap (frameEnd, 1)) && records.freeSpace() >= 1;

    if (! fits)
    {
        pendingDroppedAudio += static_cast<uint64_t> (audioFrameSamples);
        addRelaxed (droppedAudioTotal, static_cast<uint64_t> (audioFrameSamples));
        gapPending = true;
        forceStateRecords = true;
        return;
    }

    audio.tryPushAll (std::span<const float> { frame });
    records.push (CaptureRecord::makeAudio (frameStart, audioFrameSamples));
    highWater = std::max (highWater, frameEnd);
}

//==============================================================================
bool CaptureTap::pushRecord (const CaptureRecord& record, int64_t position) noexcept
{
    if (gapPending && ! pushPendingGap (position, 1))
    {
        dropRecords (1);
        return false;
    }

    if (! records.push (record))
    {
        dropRecords (1);
        return false;
    }

    highWater = std::max (highWater, position);
    return true;
}

bool CaptureTap::pushPendingGap (int64_t position, std::size_t reserve) noexcept
{
    const auto resyncNotes = enabled ? static_cast<std::size_t> (openNoteCount) : std::size_t { 0 };

    if (records.freeSpace() < 1 + resyncNotes + reserve)
        return false;

    const auto sample = std::max (position, highWater);
    records.push (CaptureRecord::makeGap (sample, GapRecord { pendingDroppedRecords, pendingDroppedAudio }));

    if (resyncNotes > 0)
    {
        // Re-state every note still open in the output: the worker may have lost a note-on or note-off
        const VsEventTiming resync { 0, EventTimingKind::none, EventTimingFlags::resync };

        for (std::size_t ch = 0; ch < openNotes.size(); ++ch)
        {
            if (! openNotes[ch].any())
                continue;

            for (std::size_t note = 0; note < 128; ++note)
                if (openNotes[ch][note])
                    records.push (CaptureRecord::makeEvent (
                        sample,
                        VsEvent::makeNoteOn (0, static_cast<uint8_t> (ch + 1), static_cast<uint8_t> (note),
                                             openVelocity[ch][note]),
                        resync));
        }
    }

    pendingDroppedRecords = 0;
    pendingDroppedAudio = 0;
    gapPending = false;
    forceStateRecords = true; // a dropped record may have been a transport or settings change
    highWater = sample;
    return true;
}

void CaptureTap::dropRecords (uint64_t count) noexcept
{
    pendingDroppedRecords += count;
    addRelaxed (droppedRecordsTotal, count);
    gapPending = true;
    forceStateRecords = true;
}

void CaptureTap::advanceClock (int numSamples) noexcept
{
    clock += numSamples;
    publishedClock.store (clock, std::memory_order_release);
}

} // namespace vocalsynth
