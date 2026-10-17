#include "CaptureWorker.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>

namespace vocalsynth
{

namespace
{
double nowMs() noexcept
{
    return juce::Time::getMillisecondCounterHiRes();
}

std::size_t takeBytesOf (const Take& take) noexcept
{
    return sizeof (Take) + take.events.size() * sizeof (CapturedEvent) + take.overview.size() * sizeof (float)
           + take.timeline.getSegments().size() * sizeof (TimelineSegment);
}
} // namespace

TransportInfo makeTransportInfo (juce::AudioPlayHead* playHead) noexcept
{
    TransportInfo t;

    if (playHead == nullptr)
        return t;

    const auto position = playHead->getPosition();

    if (! position.hasValue())
        return t;

    t.isPlaying = position->getIsPlaying();
    t.isLooping = position->getIsLooping();

    if (const auto ppq = position->getPpqPosition(); ppq.hasValue() && std::isfinite (*ppq))
    {
        t.hasPpq = true;
        t.ppqPosition = *ppq;
    }

    if (const auto bpm = position->getBpm(); bpm.hasValue() && std::isfinite (*bpm) && *bpm > 0.0)
    {
        t.hasBpm = true;
        t.bpm = *bpm;
    }

    if (const auto sig = position->getTimeSignature(); sig.hasValue() && sig->numerator > 0 && sig->denominator > 0)
    {
        t.hasTimeSignature = true;
        t.timeSigNumerator = sig->numerator;
        t.timeSigDenominator = sig->denominator;
    }

    if (const auto bar = position->getPpqPositionOfLastBarStart(); bar.hasValue() && std::isfinite (*bar))
    {
        t.hasBarStart = true;
        t.ppqPositionOfLastBarStart = *bar;
    }

    return t;
}

CaptureOffsetSettings CaptureWorkerSources::readOffset() const noexcept
{
    CaptureOffsetSettings o;

    if (offsetMode != nullptr)
        o.mode = static_cast<CaptureOffsetMode> (
            juce::jlimit (0, numCaptureOffsetModes - 1, juce::roundToInt (offsetMode->load (std::memory_order_relaxed))));

    if (offsetMs != nullptr)
        o.offsetMs = static_cast<double> (offsetMs->load (std::memory_order_relaxed));

    if (fineOffsetMs != nullptr)
        o.fineOffsetMs = static_cast<double> (fineOffsetMs->load (std::memory_order_relaxed));

    return o;
}

//==============================================================================
CaptureWorker::CaptureWorker (CaptureTap& t, juce::File folder, CaptureWorkerSources s)
    : tap (t),
      instanceFolder (std::move (folder)),
      sources (std::move (s))
{
}

CaptureWorker::~CaptureWorker()
{
    stop();
}

void CaptureWorker::start()
{
    if (started.load (std::memory_order_acquire))
        return;

    started.store (true, std::memory_order_release);

    if (manualStepping)
    {
        prepareHistory();
        return;
    }

    {
        const std::scoped_lock lock (controlMutex);
        stopRequested = false;
        suspendRequested = false;
        parked = false;
    }

    threadRunning.store (true, std::memory_order_release);
    thread = std::thread ([this] { threadLoop(); });
}

void CaptureWorker::suspend()
{
    if (! isStarted())
        return;

    if (! thread.joinable())
    {
        safeStep(); // manual stepping (or stopped): drain on the calling thread
        return;
    }

    std::unique_lock lock (controlMutex);
    suspendRequested = true;
    controlCv.notify_all();
    controlCv.wait (lock, [this] { return parked || ! threadRunning.load (std::memory_order_acquire); });
}

void CaptureWorker::resume()
{
    if (! thread.joinable())
        return;

    const std::scoped_lock lock (controlMutex);
    suspendRequested = false;
    controlCv.notify_all();
}

void CaptureWorker::stop()
{
    if (thread.joinable())
    {
        {
            const std::scoped_lock lock (controlMutex);
            stopRequested = true;
            controlCv.notify_all();
        }

        thread.join();
    }

    threadRunning.store (false, std::memory_order_release);

    // The thread is gone (or never existed): its state is ours now
    abortWavJob();
}

bool CaptureWorker::isRunning() const noexcept
{
    return threadRunning.load (std::memory_order_acquire);
}

void CaptureWorker::setManualSteppingForTests (bool manual) noexcept
{
    jassert (! isStarted());
    manualStepping = manual;
}

void CaptureWorker::setOptions (const CaptureWorkerOptions& options)
{
    const std::scoped_lock lock (optionsMutex);
    pendingOptions = options;
}

CaptureWorkerOptions CaptureWorker::getOptions() const
{
    const std::scoped_lock lock (optionsMutex);
    return pendingOptions;
}

void CaptureWorker::runStepForTests()
{
    jassert (! thread.joinable());
    safeStep();
}

//==============================================================================
void CaptureWorker::threadLoop()
{
    prepareHistory();

    std::unique_lock lock (controlMutex);

    while (! stopRequested)
    {
        if (suspendRequested)
        {
            // Drain what the audio thread left in the rings (it is stopped now), then park until resumed:
            // the processor reallocates the rings in CaptureTap::prepare() meanwhile
            lock.unlock();
            safeStep();
            lock.lock();

            parked = true;
            controlCv.notify_all();
            controlCv.wait (lock, [this] { return stopRequested || ! suspendRequested; });
            parked = false;
            continue;
        }

        lock.unlock();
        safeStep();
        lock.lock();

        controlCv.wait_for (lock, std::chrono::milliseconds (drainIntervalMs),
                            [this] { return stopRequested || suspendRequested; });
    }

    threadRunning.store (false, std::memory_order_release);
    controlCv.notify_all();
}

void CaptureWorker::prepareHistory()
{
    try
    {
        const auto options = getOptions();
        history.prepare (options.audioHistoryMinutes);
        segmenter.setTakeGapSeconds (options.takeGapSeconds);
        appliedOptions = options;
        appliedOffset = sources.readOffset();
        historyPrepared = true;
    }
    catch (const std::exception&)
    {
        historyPrepared = false; // out of memory: retried at the next step, the rings are discarded meanwhile
    }

    // Folders of instances that did not shut down (crash, killed host) and stopped beating a day ago
    if (instanceFolder != juce::File())
        CaptureExporter::deleteStaleInstanceFolders (instanceFolder.getParentDirectory(), instanceFolder.getFileName());

    lastHeartbeatMs = nowMs();
}

void CaptureWorker::safeStep() noexcept
{
    try
    {
        step();
    }
    catch (const std::exception&)
    {
        // Out of memory or a file system error: skip this step, the next one tries again
    }
}

//==============================================================================
void CaptureWorker::step()
{
    if (! historyPrepared)
    {
        prepareHistory();

        if (! historyPrepared)
        {
            // Keep the rings flowing (nothing can be stored)
            CaptureRecord r;
            float f = 0.0f;

            while (tap.getRecordRing().pop (r)) {}
            while (tap.getAudioRing().pop (f)) {}
            return;
        }
    }

    // Options (non-parameter state) and the offset parameters
    const auto options = getOptions();

    const std::not_equal_to<double> differs;

    if (differs (options.takeGapSeconds, appliedOptions.takeGapSeconds))
        segmenter.setTakeGapSeconds (options.takeGapSeconds);

    if (differs (options.audioHistoryMinutes, appliedOptions.audioHistoryMinutes))
        history.setAudioHistoryMinutes (options.audioHistoryMinutes);

    if (differs (options.freePreRollMs, appliedOptions.freePreRollMs) || options.wavFormat != appliedOptions.wavFormat)
        rendersDirty = true;

    appliedOptions = options;

    if (const auto offset = sources.readOffset(); ! sameOffset (offset, appliedOffset))
    {
        appliedOffset = offset;
        rendersDirty = true;
    }

    // Drain the rings into the history and the segmenter
    closedScratch.clear();
    drainer.drain (tap, history, segmenter, closedScratch);
    const auto clock = drainer.getLastClock();
    const auto now = nowMs();

    // The host stopped calling processBlock: a free take would never see its gap elapse on the audio clock
    if (clock != lastClock)
    {
        lastClock = clock;
        lastClockChangeMs = now;
        idleClosed = false;
    }
    else if (! idleClosed && now - lastClockChangeMs >= appliedOptions.takeGapSeconds * 1000.0)
    {
        segmenter.closeIdle (history, closedScratch);
        idleClosed = true;
    }

    for (auto& take : closedScratch)
        processClosedTake (std::move (take), clock);

    closedScratch.clear();

    if (segmenter.getClearCount() != seenClearCount)
    {
        seenClearCount = segmenter.getClearCount();
        clearTakeList();
    }

    if (rendersDirty)
    {
        rendersDirty = false;
        checkWavRenders();
    }

    runWavJob();
    updateOpenTakeSnapshot (manualStepping || now - lastSnapshotMs >= openTakeSnapshotIntervalMs);
    updateStatus (clock);

    if (now - lastHeartbeatMs >= heartbeatIntervalSeconds * 1000.0)
    {
        lastHeartbeatMs = now;
        CaptureExporter::touchHeartbeat (instanceFolder);
    }
}

RenderOptions CaptureWorker::wavRenderOptions (const Take& take) const
{
    RenderOptions o;
    o.freePreRollMs = appliedOptions.freePreRollMs;
    o.roundTripSamples = TakeRenderer::roundTripFor (take, appliedOffset);
    return o;
}

void CaptureWorker::processClosedTake (Take&& take, int64_t clock)
{
    const auto rate = take.sampleRate > 0.0 ? take.sampleRate : 48000.0;
    const auto agoMs = static_cast<double> (clock - take.startSample) * 1000.0 / rate;
    take.wallClockStartMs = juce::Time::currentTimeMillis() - static_cast<int64_t> (std::llround (agoMs));
    take.observedBlockSize = sources.observedBlockSize != nullptr ? sources.observedBlockSize() : 0;

    // Refinement first: the origin (and so the WAV span) uses the refined first note-on
    refineOnsets (take);

    const auto span = TakeRenderer::computeAudioSpan (take, wavRenderOptions (take));
    take.audio = audioAvailability (span);

    if (take.audio != TakeAudio::none)
        computeOverview (take, span);

    publish (std::make_shared<const Take> (std::move (take)), span);
}

void CaptureWorker::refineOnsets (Take& take) const
{
    const auto range = history.getAudioRange();

    if (range.to <= range.from)
        return;

    // One small window per note-on (TakeRenderer::refineNoteOnsets on a one-event take), so a 10-minute take
    // never needs its whole audio in one buffer
    const auto rate = take.sampleRate > 0.0 ? take.sampleRate : 48000.0;
    const auto lookBack = static_cast<int64_t> (std::ceil ((TakeRenderer::onsetRefineWindowMs + TakeRenderer::refineQuietMs + 5.0)
                                                           * rate / 1000.0));
    Take single;
    single.sampleRate = take.sampleRate;
    single.maxBlockSize = take.maxBlockSize;
    single.settings = take.settings;
    single.mode = take.mode;
    single.events.resize (1);
    std::vector<float> audio;

    for (auto& event : take.events)
    {
        if (event.event.type != VsEventType::noteOn || event.event.data2 == 0 || event.synthetic
            || event.timing.kind == EventTimingKind::none)
            continue;

        const auto from = std::min (event.estimatedSample, event.sample) - lookBack;
        const auto to = event.sample + 1;

        if (from < range.from || to > range.to || to - from > (int64_t { 1 } << 20))
            continue;

        audio.resize (static_cast<std::size_t> (to - from));
        history.readAudio (from, std::span<float> { audio });

        single.events[0] = event;
        TakeRenderer::refineNoteOnsets (single, std::span<const float> { audio }, from);
        event.refinedSample = single.events[0].refinedSample;
    }
}

TakeAudio CaptureWorker::audioAvailability (const AudioSpan& span) const noexcept
{
    const auto range = history.getAudioRange();
    const auto end = span.startSample + span.numSamples;

    if (span.numSamples <= 0 || range.to <= range.from || range.to <= span.startSample || range.from >= end)
        return TakeAudio::none;

    return range.from <= span.startSample && range.to >= end ? TakeAudio::full : TakeAudio::truncated;
}

void CaptureWorker::computeOverview (Take& take, const AudioSpan& span) const
{
    constexpr auto perPoint = Take::overviewSamplesPerPoint;
    const auto points = static_cast<std::size_t> ((span.numSamples + perPoint - 1) / perPoint);
    take.overview.assign (points * 2, 0.0f);

    std::vector<float> block (static_cast<std::size_t> (perPoint) * 128);

    for (std::size_t p = 0; p < points;)
    {
        const auto first = span.startSample + static_cast<int64_t> (p) * perPoint;
        const auto n = static_cast<std::size_t> (std::min<int64_t> (static_cast<int64_t> (block.size()),
                                                                    span.startSample + span.numSamples - first));
        history.readAudio (first, std::span<float> { block.data(), n });

        for (std::size_t i = 0; i < n && p < points; i += perPoint, ++p)
        {
            const auto count = std::min<std::size_t> (perPoint, n - i);
            const auto [lo, hi] = std::minmax_element (block.begin() + static_cast<std::ptrdiff_t> (i),
                                                       block.begin() + static_cast<std::ptrdiff_t> (i + count));
            take.overview[2 * p] = *lo;
            take.overview[2 * p + 1] = *hi;
        }
    }
}

void CaptureWorker::publish (std::shared_ptr<const Take> take, const AudioSpan& span)
{
    std::vector<TakeWavState> dropped;

    {
        const std::scoped_lock lock (listMutex);
        Entry entry;
        entry.take = take;
        entry.wavSpan = span;
        entry.wavFormat = appliedOptions.wavFormat;
        entry.wav.kind = take->audio == TakeAudio::none ? TakeWavState::Kind::unavailable : TakeWavState::Kind::preparing;
        entry.wavQueued = take->audio != TakeAudio::none;
        entries.insert (entries.begin(), std::move (entry));

        std::size_t totalEvents = 0;

        for (const auto& e : entries)
            totalEvents += e.take->events.size();

        while (entries.size() > 1 && (entries.size() > maxTakes || totalEvents > maxTotalTakeEvents))
        {
            totalEvents -= entries.back().take->events.size();
            dropped.push_back (entries.back().wav);

            if (wavJob != nullptr && wavJob->takeNumber == entries.back().take->number)
                wavJob->takeNumber = 0; // aborted below

            entries.pop_back();
        }
    }

    if (take->audio != TakeAudio::none)
        wavQueue.push_back (take->number);

    if (wavJob != nullptr && wavJob->takeNumber == 0)
        abortWavJob();

    for (const auto& state : dropped)
        deleteCachedWav (state);

    bumpVersion();
}

void CaptureWorker::clearTakeList()
{
    std::vector<Entry> old;

    {
        const std::scoped_lock lock (listMutex);
        old.swap (entries);
        openTakeSnapshot.reset();
    }

    abortWavJob();
    wavQueue.clear();

    for (const auto& e : old)
        deleteCachedWav (e.wav); // persisted WAVs stay (deleteCachedWav only deletes ready cache files)

    snapshotNumber = 0;
    bumpVersion();
}

void CaptureWorker::deleteCachedWav (const TakeWavState& state)
{
    if (state.kind != TakeWavState::Kind::ready)
        return;

    const std::scoped_lock lock (fileMutex);
    state.file.deleteFile();
}

void CaptureWorker::checkWavRenders()
{
    // The offset parameters, the free pre-roll or the WAV format changed: re-render cached WAVs whose span
    // or format differs, while the history still holds their audio
    const auto range = history.getAudioRange();
    std::vector<int> toQueue;
    std::vector<TakeWavState> outdated;
    bool restartJob = false;

    {
        const std::scoped_lock lock (listMutex);

        for (auto& e : entries)
        {
            if (e.take->audio == TakeAudio::none)
                continue;

            const auto span = TakeRenderer::computeAudioSpan (*e.take, wavRenderOptions (*e.take));

            if (span == e.wavSpan && e.wavFormat == appliedOptions.wavFormat)
                continue;

            if (range.to <= range.from || range.from > span.startSample)
                continue; // evicted: keep the WAV as it is

            e.wavSpan = span;
            e.wavFormat = appliedOptions.wavFormat;

            // The old cached file no longer lines up with the MIDI; a persisted one belongs to the user
            if (e.wav.kind == TakeWavState::Kind::ready)
                outdated.push_back (e.wav);

            e.wav.kind = TakeWavState::Kind::preparing;
            e.wav.file = juce::File();

            if (wavJob != nullptr && wavJob->takeNumber == e.take->number)
                restartJob = true;

            if (! e.wavQueued || restartJob)
            {
                e.wavQueued = true;
                toQueue.push_back (e.take->number);
            }
        }
    }

    if (restartJob)
    {
        wavJob->takeNumber = 0; // its state is already "preparing" again
        abortWavJob();
    }

    for (const auto& state : outdated)
        deleteCachedWav (state);

    for (const auto n : toQueue)
        if (std::find (wavQueue.begin(), wavQueue.end(), n) == wavQueue.end())
            wavQueue.push_back (n);

    if (! toQueue.empty())
        bumpVersion();
}

void CaptureWorker::runWavJob()
{
    if (wavJob == nullptr)
    {
        while (! wavQueue.empty() && wavJob == nullptr)
        {
            const auto number = wavQueue.front();
            wavQueue.pop_front();

            std::shared_ptr<const Take> take;
            AudioSpan span;
            WavSampleFormat format = WavSampleFormat::pcm24;

            {
                const std::scoped_lock lock (listMutex);

                for (auto& e : entries)
                    if (e.take->number == number)
                    {
                        e.wavQueued = false;
                        take = e.take;
                        span = e.wavSpan;
                        format = e.wavFormat;
                    }
            }

            if (take == nullptr || span.numSamples <= 0)
                continue;

            auto job = std::make_unique<WavJob>();
            job->takeNumber = number;
            job->take = take;
            job->span = span;
            job->format = format;

            const auto rendered = TakeRenderer::render (*take, wavRenderOptions (*take));
            const auto target = instanceFolder.getChildFile (CaptureExporter::makeWavFileName (*take, rendered));

            // The instance folder is created with its first file (pluginval instances leave nothing behind)
            if (! instanceFolder.createDirectory()
                || ! job->writer.open (target, take->sampleRate, format, static_cast<uint64_t> (span.numSamples)))
            {
                const std::scoped_lock lock (listMutex);

                for (auto& e : entries)
                    if (e.take->number == number && e.wav.kind == TakeWavState::Kind::preparing)
                        e.wav.kind = TakeWavState::Kind::unavailable;

                bumpVersion();
                continue;
            }

            if (lastHeartbeatMs <= 0.0 || ! instanceFolder.getChildFile (CaptureExporter::heartbeatFileName).exists())
                CaptureExporter::touchHeartbeat (instanceFolder);

            wavJob = std::move (job);
        }

        if (wavJob == nullptr)
            return;
    }

    // One slice per step: draining keeps its rhythm and stop() returns promptly
    auto& job = *wavJob;
    const auto written = static_cast<int64_t> (job.writer.getFramesWritten());
    const auto remaining = job.span.numSamples - written;
    const auto n = static_cast<std::size_t> (std::min<int64_t> (wavFramesPerStep, remaining));

    if (n > 0)
    {
        const auto from = job.span.startSample + written;
        const auto range = history.getAudioRange();

        if (range.from > from || range.to < from + static_cast<int64_t> (n))
            job.truncated = true; // evicted meanwhile (or never captured): silence there

        wavBuffer.resize (n);
        history.readAudio (from, std::span<float> { wavBuffer });

        if (! job.writer.write (std::span<const float> { wavBuffer }))
        {
            abortWavJob();
            return;
        }
    }

    if (job.writer.getFramesWritten() >= job.writer.getNumFrames())
        finishWavJob();
}

void CaptureWorker::finishWavJob()
{
    auto job = std::move (wavJob);
    TakeWavState previous;
    juce::File file;

    {
        const std::scoped_lock fileLock (fileMutex);
        file = job->writer.finish();

        const std::scoped_lock lock (listMutex);

        for (auto& e : entries)
            if (e.take->number == job->takeNumber)
            {
                previous = e.wav;

                if (file == juce::File())
                {
                    e.wav = {};
                }
                else
                {
                    e.wav.kind = TakeWavState::Kind::ready;
                    e.wav.file = file;
                }

                if (job->truncated && e.take->audio == TakeAudio::full)
                {
                    auto copy = std::make_shared<Take> (*e.take);
                    copy->audio = TakeAudio::truncated;
                    e.take = std::move (copy);
                }
            }

        // A re-render replaced an older cached file under another name (numbered sibling)
        if (previous.kind == TakeWavState::Kind::ready && previous.file != file)
            previous.file.deleteFile();
    }

    bumpVersion();
}

void CaptureWorker::abortWavJob()
{
    if (wavJob == nullptr)
        return;

    const auto number = wavJob->takeNumber;
    wavJob->writer.abort();
    wavJob.reset();

    if (number != 0)
    {
        const std::scoped_lock lock (listMutex);

        for (auto& e : entries)
            if (e.take->number == number && e.wav.kind == TakeWavState::Kind::preparing)
                e.wav.kind = TakeWavState::Kind::unavailable;
    }
}

void CaptureWorker::updateOpenTakeSnapshot (bool due)
{
    const auto* open = segmenter.getOpenTake();

    if (open == nullptr || open->closed)
    {
        if (snapshotNumber != 0)
        {
            snapshotNumber = 0;
            {
                const std::scoped_lock lock (listMutex);
                openTakeSnapshot.reset();
            }
            bumpVersion();
        }

        return;
    }

    if (! due && snapshotNumber == open->number)
        return;

    if (snapshotNumber == open->number && snapshotEvents == open->events.size() && snapshotEnd == open->endSample)
        return;

    snapshotNumber = open->number;
    snapshotEvents = open->events.size();
    snapshotEnd = open->endSample;
    lastSnapshotMs = nowMs();

    // The UI needs the number, the span and the note count: no copy of a long take's events every 500 ms
    auto copy = std::make_shared<Take>();
    copy->number = open->number;
    copy->mode = open->mode;
    copy->kind = open->kind;
    copy->startSample = open->startSample;
    copy->endSample = open->endSample;
    copy->captureEndSample = open->captureEndSample;
    copy->sampleRate = open->sampleRate;
    copy->maxBlockSize = open->maxBlockSize;
    copy->settings = open->settings;
    copy->freeBpm = open->freeBpm;
    copy->noteCount = open->noteCount;
    copy->incomplete = open->incomplete;

    {
        const std::scoped_lock lock (listMutex);
        openTakeSnapshot = std::move (copy);
    }

    bumpVersion();
}

void CaptureWorker::updateStatus (int64_t clock)
{
    CaptureStatus s;
    s.started = true;
    s.state = segmenter.getState();

    const auto& timeline = history.getTimeline();

    if (const auto* seg = timeline.segmentAt (static_cast<double> (clock)); seg != nullptr)
    {
        s.bpm = seg->bpm;
        s.timeSigNumerator = seg->timeSigNumerator;
        s.timeSigDenominator = seg->timeSigDenominator;

        if (s.state == TakeSegmenter::State::synced)
            s.recordingBar = timeline.barAt (static_cast<double> (clock)).number;
    }

    if (const auto* open = segmenter.getOpenTake(); open != nullptr)
        s.recordingTakeNumber = open->number;

    s.audioHistoryBytes = history.getAllocatedAudioBytes();

    const std::scoped_lock lock (listMutex);
    s.numTakes = static_cast<int> (entries.size());

    for (const auto& e : entries)
        s.takeBytes += takeBytesOf (*e.take);

    status = s;
}

//==============================================================================
std::vector<std::shared_ptr<const Take>> CaptureWorker::getTakes() const
{
    const std::scoped_lock lock (listMutex);
    std::vector<std::shared_ptr<const Take>> result;
    result.reserve (entries.size());

    for (const auto& e : entries)
        result.push_back (e.take);

    return result;
}

std::shared_ptr<const Take> CaptureWorker::getTake (int takeNumber) const
{
    const std::scoped_lock lock (listMutex);

    for (const auto& e : entries)
        if (e.take->number == takeNumber)
            return e.take;

    return nullptr;
}

std::shared_ptr<const Take> CaptureWorker::getOpenTake() const
{
    const std::scoped_lock lock (listMutex);
    return openTakeSnapshot;
}

TakeWavState CaptureWorker::getWavState (int takeNumber) const
{
    const std::scoped_lock lock (listMutex);

    for (const auto& e : entries)
        if (e.take->number == takeNumber)
            return e.wav;

    return {};
}

juce::File CaptureWorker::persistWav (int takeNumber, const juce::File& takesFolder)
{
    const std::scoped_lock fileLock (fileMutex);
    const auto state = getWavState (takeNumber);

    if (state.kind == TakeWavState::Kind::persisted && state.file.existsAsFile())
        return state.file;

    if (state.kind != TakeWavState::Kind::ready)
        return {};

    const auto persisted = CaptureExporter::persistWav (state.file, takesFolder);

    if (persisted == juce::File())
        return {};

    {
        const std::scoped_lock lock (listMutex);

        for (auto& e : entries)
            if (e.take->number == takeNumber && e.wav.file == state.file)
            {
                e.wav.kind = TakeWavState::Kind::persisted;
                e.wav.file = persisted;
            }
    }

    bumpVersion();
    return persisted;
}

CaptureStatus CaptureWorker::getStatus() const
{
    CaptureStatus s;

    {
        const std::scoped_lock lock (listMutex);
        s = status;
    }

    s.started = isStarted();
    s.overflow = tap.getOverflowCounts();
    return s;
}

} // namespace vocalsynth
