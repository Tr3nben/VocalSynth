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
        prepareHistory(); // file tasks run on the calling thread
        return;
    }

    {
        const std::scoped_lock lock (controlMutex);
        stopRequested = false;
        suspendRequested = false;
        drainedGeneration = suspendGeneration;
    }

    startFileThread();
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

    // Wait for a drain that started after this request: a suspend() right after resume() must not return
    // before the worker drained what the audio thread pushed in between (prepare() resets the rings)
    std::unique_lock lock (controlMutex);
    suspendRequested = true;
    const auto generation = ++suspendGeneration;
    controlCv.notify_all();
    controlCv.wait (lock, [this, generation]
                    { return drainedGeneration >= generation || ! threadRunning.load (std::memory_order_acquire); });
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

    // The worker thread is gone (or never existed): its state is ours now. The abort is queued behind the
    // job's running task; the file thread finishes its queue before it is joined.
    abortWavJob();
    stopFileThread();
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
void CaptureWorker::startFileThread()
{
    {
        const std::scoped_lock lock (fileQueueMutex);
        fileThreadStop = false;
    }

    fileThread = std::thread ([this] { fileThreadLoop(); });
}

void CaptureWorker::stopFileThread()
{
    if (! fileThread.joinable())
        return;

    {
        const std::scoped_lock lock (fileQueueMutex);
        fileThreadStop = true;
        fileQueueCv.notify_all();
    }

    fileThread.join();
}

void CaptureWorker::fileThreadLoop()
{
    std::unique_lock lock (fileQueueMutex);

    for (;;)
    {
        fileQueueCv.wait (lock, [this] { return fileThreadStop || ! fileTasks.empty(); });

        if (fileTasks.empty())
            return; // stop requested and every queued task is done

        auto task = std::move (fileTasks.front());
        fileTasks.pop_front();
        lock.unlock();

        try
        {
            task();
        }
        catch (...)
        {
            // Out of memory or a file system error: the task is lost (a WAV job task reports it itself)
        }

        task = nullptr; // releases what the task held, outside the lock
        lock.lock();
    }
}

void CaptureWorker::postFileTask (std::function<void()> task)
{
    if (task == nullptr)
        return;

    if (fileThread.joinable())
    {
        const std::scoped_lock lock (fileQueueMutex);
        fileTasks.push_back (std::move (task));
        fileQueueCv.notify_all();
        return;
    }

    try
    {
        task();
    }
    catch (...)
    {
    }
}

void CaptureWorker::postJobTask (const std::shared_ptr<WavJob>& job, std::function<void (WavJob&)> jobTask)
{
    job->busy.store (true, std::memory_order_release);

    postFileTask ([job, task = std::move (jobTask)]
    {
        try
        {
            task (*job);
        }
        catch (...)
        {
            job->failed = true;
        }

        job->busy.store (false, std::memory_order_release);
    });
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
            if (drainedGeneration != suspendGeneration)
            {
                // Drain what the audio thread left in the rings (it is stopped now), then park until
                // resumed: the processor reallocates the rings in CaptureTap::prepare() meanwhile
                const auto generation = suspendGeneration;
                lock.unlock();
                safeStep();
                lock.lock();

                drainedGeneration = generation;
                controlCv.notify_all();
                continue; // another suspend() may have come meanwhile: drain again for it
            }

            controlCv.wait (lock, [this]
                            { return stopRequested || ! suspendRequested || drainedGeneration != suspendGeneration; });
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

void CaptureWorker::prepareHistory() noexcept
{
    lastPrepareAttemptMs = nowMs();

    try
    {
        const auto options = getOptions();
        history.prepare (options.audioHistoryMinutes);
        segmenter.setTakeGapSeconds (options.takeGapSeconds);
        appliedOptions = options;
        appliedOffset = sources.readOffset();
        historyPrepared = true;
    }
    catch (...)
    {
        historyPrepared = false; // out of memory: retried later, the rings are discarded meanwhile
    }

    lastHeartbeatMs = lastPrepareAttemptMs;

    if (staleCleanupPosted || instanceFolder == juce::File())
        return;

    // Folders of instances that did not shut down (crash, killed host) and stopped beating a day ago:
    // once, on the file thread
    staleCleanupPosted = true;

    try
    {
        postFileTask ([root = instanceFolder.getParentDirectory(), keep = instanceFolder.getFileName()]
                      { CaptureExporter::deleteStaleInstanceFolders (root, keep); });
    }
    catch (...)
    {
    }
}

void CaptureWorker::safeStep() noexcept
{
    try
    {
        step();
    }
    catch (...)
    {
        // Out of memory or a file system error: skip this step, the next one tries again
    }
}

//==============================================================================
void CaptureWorker::step()
{
    if (! historyPrepared)
    {
        // Out of memory at the start: retry once a second, keep the rings flowing meanwhile
        if (nowMs() - lastPrepareAttemptMs >= 1000.0)
            prepareHistory();

        if (! historyPrepared)
        {
            CaptureRecord r;
            float f = 0.0f;

            while (tap.getRecordRing().pop (r)) {}
            while (tap.getAudioRing().pop (f)) {}
            return;
        }
    }

    // Options (non-parameter state). The take gap cannot fail; the audio look-back allocates and is
    // applied after the drain, so a failing allocation never keeps the rings from being drained.
    const auto options = getOptions();
    const std::not_equal_to<double> differs;

    if (differs (options.takeGapSeconds, appliedOptions.takeGapSeconds))
    {
        segmenter.setTakeGapSeconds (options.takeGapSeconds);
        appliedOptions.takeGapSeconds = options.takeGapSeconds;
    }

    // Drain the rings into the history and the segmenter (the anchor of synced takes uses the round trip
    // of the current offset parameters, docs/M3.1-DESIGN.md 3.3)
    closedScratch.clear();
    segmenter.setOffsetSettings (sources.readOffset());
    drainer.drain (tap, history, segmenter, closedScratch);
    const auto clock = drainer.getLastClock();
    const auto now = nowMs();

    if (differs (options.audioHistoryMinutes, appliedOptions.audioHistoryMinutes))
    {
        appliedOptions.audioHistoryMinutes = options.audioHistoryMinutes; // applied (or given up) once

        try
        {
            history.setAudioHistoryMinutes (options.audioHistoryMinutes);
        }
        catch (...)
        {
            // Out of memory: the look-back stays as it was until the option changes again
        }
    }

    if (differs (options.freePreRollMs, appliedOptions.freePreRollMs) || options.wavFormat != appliedOptions.wavFormat)
        rendersDirty = true;

    appliedOptions = options;

    if (const auto offset = sources.readOffset(); ! sameOffset (offset, appliedOffset))
    {
        appliedOffset = offset;
        rendersDirty = true;
    }

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
    releaseRetainedAudioIfUnused();
    updateOpenTakeSnapshot (manualStepping || now - lastSnapshotMs >= openTakeSnapshotIntervalMs);
    updateStatus (clock);

    if (now - lastHeartbeatMs >= heartbeatIntervalSeconds * 1000.0)
    {
        lastHeartbeatMs = now;
        postFileTask ([folder = instanceFolder] { CaptureExporter::touchHeartbeat (folder); });
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

    // The list shows the notes the export has (Raw): a synthetic note-on that the host-time clamp drops
    // (a note that ended just before Play or a split) is not one, and a take without any is not listed
    auto rawOptions = wavRenderOptions (take);
    rawOptions.version = ExportVersion::raw;
    take.noteCount = TakeRenderer::render (take, rawOptions).noteCount;

    if (take.noteCount == 0)
        return;

    const auto span = TakeRenderer::computeAudioSpan (take, wavRenderOptions (take));
    take.audio = audioAvailability (span, take.clockEpoch);

    if (take.audio != TakeAudio::none)
        computeOverview (take, span);

    publish (std::make_shared<const Take> (std::move (take)), span);
}

void CaptureWorker::refineOnsets (Take& take) const
{
    const auto range = history.getAudioRange (take.clockEpoch);

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
        history.readAudio (take.clockEpoch, from, std::span<float> { audio });

        single.events[0] = event;
        TakeRenderer::refineNoteOnsets (single, std::span<const float> { audio }, from);
        event.refinedSample = single.events[0].refinedSample;
    }
}

TakeAudio CaptureWorker::audioAvailability (const AudioSpan& span, uint32_t clockEpoch) const noexcept
{
    const auto range = history.getAudioRange (clockEpoch);
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
        history.readAudio (take.clockEpoch, first, std::span<float> { block.data(), n });

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

    postFileTask ([this, file = state.file]
    {
        const std::scoped_lock lock (fileMutex);
        file.deleteFile();
    });
}

void CaptureWorker::checkWavRenders()
{
    // The offset parameters, the free pre-roll or the WAV format changed: re-render cached WAVs whose span
    // or format differs, while the history still holds their audio (of the take's own clock epoch: after a
    // sample-rate change the clock restarts, old spans never address new audio)
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

            const auto range = history.getAudioRange (e.take->clockEpoch);

            if (range.to <= range.from || range.from > span.startSample)
                continue; // evicted (or an earlier epoch that is gone): keep the WAV as it is

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
    // At most one slice per step, and only when the job's last file task is done: draining keeps its
    // rhythm and never waits on the disk (the file thread does the I/O)
    for (int actions = 0; actions < 4; ++actions)
    {
        if (wavJob == nullptr && ! startNextWavJob())
            return;

        auto& job = *wavJob;

        if (job.busy.load (std::memory_order_acquire))
            return;

        if (job.failed)
        {
            failWavJob();
            continue;
        }

        if (job.stage == WavJob::Stage::finishing)
        {
            completeWavJob();
            continue;
        }

        if (job.stage == WavJob::Stage::opening)
            job.stage = WavJob::Stage::writing; // the open task succeeded

        const auto remaining = job.span.numSamples - job.framesRead;
        const auto n = static_cast<std::size_t> (std::clamp<int64_t> (remaining, 0, wavFramesPerStep));

        if (n == 0)
        {
            job.stage = WavJob::Stage::finishing;
            postJobTask (wavJob, [this] (WavJob& j)
            {
                const std::scoped_lock fileLock (fileMutex);
                j.result = j.writer.finish();

                if (j.result == juce::File())
                    j.failed = true;
            });
            continue; // done at once without a file thread (manual stepping)
        }

        const auto from = job.span.startSample + job.framesRead;
        const auto range = history.getAudioRange (job.take->clockEpoch);

        if (range.from > from || range.to < from + static_cast<int64_t> (n))
            job.truncated = true; // evicted meanwhile (or never captured): silence there

        job.buffer.resize (n);
        history.readAudio (job.take->clockEpoch, from, std::span<float> { job.buffer });
        job.framesRead += static_cast<int64_t> (n);

        postJobTask (wavJob, [] (WavJob& j)
        {
            if (! j.writer.write (std::span<const float> { j.buffer }))
                j.failed = true;
        });
        return;
    }
}

bool CaptureWorker::startNextWavJob()
{
    while (! wavQueue.empty())
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

        auto job = std::make_shared<WavJob>();
        job->takeNumber = number;
        job->take = take;
        job->span = span;
        job->format = format;

        const auto rendered = TakeRenderer::render (*take, wavRenderOptions (*take));
        const auto target = instanceFolder.getChildFile (CaptureExporter::makeWavFileName (*take, rendered));
        wavJob = job;

        // The instance folder is created with its first file (pluginval instances leave nothing behind)
        postJobTask (job, [folder = instanceFolder, target, rate = take->sampleRate, format,
                           frames = static_cast<uint64_t> (span.numSamples)] (WavJob& j)
        {
            if (! folder.createDirectory() || ! j.writer.open (target, rate, format, frames))
            {
                j.failed = true;
                return;
            }

            if (! folder.getChildFile (CaptureExporter::heartbeatFileName).exists())
                CaptureExporter::touchHeartbeat (folder);
        });

        return true;
    }

    return false;
}

void CaptureWorker::completeWavJob()
{
    const auto job = std::move (wavJob);
    TakeWavState previous;

    {
        const std::scoped_lock lock (listMutex);

        for (auto& e : entries)
            if (e.take->number == job->takeNumber)
            {
                previous = e.wav;
                e.wav.kind = TakeWavState::Kind::ready;
                e.wav.file = job->result;

                if (job->truncated && e.take->audio == TakeAudio::full)
                {
                    auto copy = std::make_shared<Take> (*e.take);
                    copy->audio = TakeAudio::truncated;
                    e.take = std::move (copy);
                }
            }
    }

    // A re-render replaced an older cached file under another name (numbered sibling)
    if (previous.kind == TakeWavState::Kind::ready && previous.file != job->result)
        deleteCachedWav (previous);

    bumpVersion();
}

void CaptureWorker::failWavJob()
{
    abortWavJob(); // the take's WAV becomes unavailable
    bumpVersion();
}

void CaptureWorker::abortWavJob()
{
    if (wavJob == nullptr)
        return;

    const auto job = std::move (wavJob);
    const auto number = job->takeNumber;

    // Queued behind the job's running task (the file thread keeps the order); deletes the temporary file
    // and, if it already finished, the result nobody will list
    postFileTask ([this, job]
    {
        job->writer.abort();

        if (job->result != juce::File())
        {
            const std::scoped_lock lock (fileMutex);
            job->result.deleteFile();
        }
    });

    if (number != 0)
    {
        const std::scoped_lock lock (listMutex);

        for (auto& e : entries)
            if (e.take->number == number && e.wav.kind == TakeWavState::Kind::preparing)
                e.wav.kind = TakeWavState::Kind::unavailable;
    }
}

void CaptureWorker::releaseRetainedAudioIfUnused()
{
    if (! history.hasRetainedAudio())
        return;

    const auto epoch = history.getRetainedEpoch();

    if (wavJob != nullptr && wavJob->take->clockEpoch == epoch)
        return;

    {
        const std::scoped_lock lock (listMutex);

        for (const auto& e : entries)
            if (e.take->clockEpoch == epoch && e.wavQueued)
                return;
    }

    history.releaseRetainedAudio();
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
    copy->maxBars = open->maxBars;
    copy->anchorBar = open->anchorBar;
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
    {
        s.recordingTakeNumber = open->number;
        s.openTakeBars = open->maxBars;
        s.openTakeStartBar = open->anchorBar;
        s.openTakeBar = segmenter.getOpenTakeBar();
    }

    s.mode = segmenter.getCaptureMode();
    s.takeMaxBars = segmenter.getTakeMaxBars();
    s.record = segmenter.getRecordStatus();

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
