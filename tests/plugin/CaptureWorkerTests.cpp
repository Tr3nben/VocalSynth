// CaptureWorker (docs/M3-DESIGN.md 5, 12, 15, 16): lifecycle, draining, take list, WAV cache, re-render,
// idle close, lazy folder + heartbeat, makeTransportInfo. The processor-level cases (10-minute take,
// pluginval-like instances) are in ProcessorTests.cpp.

#include "CaptureExporter.h"
#include "CaptureTestUtils.h"
#include "CaptureWorker.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

using namespace vocalsynth;
using vocalsynth::test::noteOffAt;
using vocalsynth::test::noteOnAt;
using vocalsynth::test::TapFeeder;
using vocalsynth::test::TimedEvent;

namespace
{
constexpr double rate = 48000.0;

/** A fresh instance folder below the capture root, deleted at the end of the test. */
struct TestFolder
{
    juce::File folder = CaptureExporter::getInstanceFolder ("test" + CaptureExporter::makeInstanceId().substring (4));
    ~TestFolder() { CaptureExporter::deleteInstanceFolder (folder); }
};

/** A free take: note 60 from 0.1 s to 0.6 s, then nothing for gapSeconds + 1 s. */
std::vector<TimedEvent> freeNote (int64_t at = (int64_t) (0.1 * rate), int note = 60)
{
    return { noteOnAt (at, note), noteOffAt (at + (int64_t) (0.5 * rate), note) };
}

float sine (int64_t pos)
{
    return 0.25f * (float) std::sin (juce::MathConstants<double>::twoPi * 330.0 * (double) pos / rate);
}

void stepUntilWavDone (CaptureWorker& worker, int takeNumber, int maxSteps = 200)
{
    for (int i = 0; i < maxSteps && worker.getWavState (takeNumber).kind == TakeWavState::Kind::preparing; ++i)
        worker.runStepForTests();
}
} // namespace

TEST_CASE ("makeTransportInfo reads the host playhead", "[plugin][captureworker]")
{
    const auto none = makeTransportInfo (nullptr);
    CHECK_FALSE (none.isPlaying);
    CHECK_FALSE (none.hasPpq);
    CHECK_FALSE (none.hasBpm);
    CHECK_FALSE (none.hasTimeSignature);
    CHECK_FALSE (none.hasBarStart);

    vocalsynth::test::FakePlayHead head;
    head.bpm = 97.5;
    head.numerator = 7;
    head.denominator = 8;
    head.playStartSample = 0;
    head.playStartPpq = 10.0;
    head.blockStart = 48000;

    const auto t = makeTransportInfo (&head);
    CHECK (t.isPlaying);
    CHECK (t.hasPpq);
    CHECK (t.ppqPosition == Catch::Approx (10.0 + 97.5 / 60.0));
    CHECK (t.hasBpm);
    CHECK (t.bpm == Catch::Approx (97.5));
    CHECK (t.hasTimeSignature);
    CHECK (t.timeSigNumerator == 7);
    CHECK (t.timeSigDenominator == 8);
    CHECK (t.hasBarStart);
    CHECK (t.ppqPositionOfLastBarStart == Catch::Approx (10.5)); // bars of 3.5 quarter notes

    head.reportBarStart = false;
    CHECK_FALSE (makeTransportInfo (&head).hasBarStart);
}

TEST_CASE ("CaptureWorker lifecycle: no thread before start, quick suspend / resume / stop", "[plugin][captureworker][lifecycle]")
{
    TestFolder dir;
    CaptureTap tap;
    tap.prepare (rate, 512);
    CaptureWorker worker (tap, dir.folder);
    CHECK_FALSE (worker.isRunning());
    CHECK_FALSE (worker.isStarted());
    worker.suspend(); // no-op before start
    worker.resume();

    for (int round = 0; round < 20; ++round)
    {
        worker.start();
        CHECK (worker.isRunning());
        worker.start(); // idempotent

        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        worker.suspend();
        tap.prepare (round % 2 == 0 ? 44100.0 : 48000.0, 256); // the rings may be reallocated while parked
        worker.resume();
        CHECK (juce::Time::getMillisecondCounterHiRes() - t0 < 500.0);

        TapFeeder feeder (tap);
        feeder.runTo (4800, 480);
        std::this_thread::sleep_for (std::chrono::milliseconds (15));

        if (round == 19)
        {
            const auto t1 = juce::Time::getMillisecondCounterHiRes();
            worker.stop();
            CHECK (juce::Time::getMillisecondCounterHiRes() - t1 < 200.0);
            CHECK_FALSE (worker.isRunning());
            worker.stop(); // idempotent
        }
    }

    // Nothing was written: the instance folder is created with the first file only
    CHECK_FALSE (dir.folder.exists());
}

TEST_CASE ("CaptureWorker with manual stepping starts no thread", "[plugin][captureworker][lifecycle]")
{
    TestFolder dir;
    CaptureTap tap;
    tap.prepare (rate, 512);
    CaptureWorker worker (tap, dir.folder);
    worker.setManualSteppingForTests (true);
    worker.start();
    CHECK (worker.isStarted());
    CHECK_FALSE (worker.isRunning());

    TapFeeder feeder (tap);
    feeder.runTo (4800, 480);
    worker.suspend(); // drains on this thread
    CHECK (tap.getRecordRing().size() == 0);
    worker.resume();
    CHECK (worker.getStatus().started);
    CHECK (worker.getStatus().state == TakeSegmenter::State::freeWaiting);
}

TEST_CASE ("CaptureWorker: a free take closes after the gap; Clear empties the list", "[plugin][captureworker]")
{
    TestFolder dir;
    CaptureTap tap;
    tap.prepare (rate, 512);
    CaptureWorker worker (tap, dir.folder);
    worker.setManualSteppingForTests (true);
    worker.start();

    TapFeeder feeder (tap);
    feeder.worker = &worker;
    feeder.settings.captureAudio = false;
    const auto events = freeNote();

    feeder.runTo ((int64_t) (0.3 * rate), 512, events);
    worker.runStepForTests();

    // Recording: status and the open-take snapshot (every step with manual stepping)
    auto status = worker.getStatus();
    CHECK (status.state == TakeSegmenter::State::freeRecording);
    CHECK (status.recordingTakeNumber == 1);
    REQUIRE (worker.getOpenTake() != nullptr);
    CHECK (worker.getOpenTake()->number == 1);
    CHECK (worker.getTakes().empty());

    const auto versionBefore = worker.getTakeListVersion();
    feeder.runTo ((int64_t) (0.6 * rate + 3.2 * rate), 512, events); // gap 3 s after the note-off
    worker.runStepForTests();

    auto takes = worker.getTakes();
    REQUIRE (takes.size() == 1);
    CHECK (takes.front()->number == 1);
    CHECK (takes.front()->mode == TakeMode::free);
    CHECK (takes.front()->closed);
    CHECK (takes.front()->noteCount == 1);
    CHECK (takes.front()->audio == TakeAudio::none);
    CHECK (takes.front()->wallClockStartMs > 0);
    CHECK (worker.getOpenTake() == nullptr);
    CHECK (worker.getTakeListVersion() != versionBefore);
    CHECK (worker.getWavState (1).kind == TakeWavState::Kind::unavailable);
    CHECK (worker.getTake (1) == takes.front());
    CHECK (worker.getTake (2) == nullptr);
    CHECK (worker.getStatus().state == TakeSegmenter::State::freeWaiting);
    CHECK (worker.getStatus().numTakes == 1);
    CHECK (worker.getStatus().takeBytes > 0);

    // Idle steps change nothing
    const auto versionIdle = worker.getTakeListVersion();
    worker.runStepForTests();
    worker.runStepForTests();
    CHECK (worker.getTakeListVersion() == versionIdle);

    // Clear (a marker at the next block): the list is emptied, the numbering continues
    tap.requestClear();
    feeder.runTo (feeder.position + 4800, 480);
    worker.runStepForTests();
    CHECK (worker.getTakes().empty());

    const auto again = freeNote (feeder.position + 4800, 62);
    feeder.runTo (feeder.position + (int64_t) (4.5 * rate), 512, again);
    worker.runStepForTests();
    takes = worker.getTakes();
    REQUIRE (takes.size() == 1);
    CHECK (takes.front()->number == 2);
}

TEST_CASE ("CaptureWorker keeps at most 32 takes, newest first", "[plugin][captureworker]")
{
    TestFolder dir;
    CaptureTap tap;
    tap.prepare (rate, 512);
    CaptureWorker worker (tap, dir.folder);
    worker.setManualSteppingForTests (true);
    CaptureWorkerOptions options;
    options.takeGapSeconds = TakeSegmenter::minTakeGapSeconds;
    worker.setOptions (options);
    worker.start();

    TapFeeder feeder (tap);
    feeder.worker = &worker;
    feeder.settings.captureAudio = false;

    std::vector<TimedEvent> events;
    const auto period = (int64_t) (1.2 * rate);

    for (int i = 0; i < 35; ++i)
    {
        const auto at = 1000 + i * period;
        events.push_back (noteOnAt (at, 60 + i % 12));
        events.push_back (noteOffAt (at + 4800, 60 + i % 12));
    }

    feeder.runTo (35 * period + (int64_t) rate, 512, events);
    worker.runStepForTests();

    const auto takes = worker.getTakes();
    REQUIRE (takes.size() == CaptureWorker::maxTakes);
    CHECK (takes.front()->number == 35);
    CHECK (takes.back()->number == 4);

    for (std::size_t i = 1; i < takes.size(); ++i)
        CHECK (takes[i - 1]->number == takes[i]->number + 1);
}

TEST_CASE ("CaptureWorker bounds the events of all listed takes", "[plugin][captureworker]")
{
    TestFolder dir;
    CaptureTap tap;
    tap.prepare (rate, 512);
    CaptureWorker worker (tap, dir.folder);
    worker.setManualSteppingForTests (true);
    worker.start();

    TapFeeder feeder (tap);
    feeder.worker = &worker;
    feeder.stepEverySamples = 2048;
    feeder.settings.captureAudio = false;

    // One held note with a pitch bend on every sample: the segmenter splits at its event limit
    std::vector<TimedEvent> events { noteOnAt (100, 60) };
    const int64_t total = (int64_t) (CaptureWorker::maxTotalTakeEvents + CaptureWorker::maxTotalTakeEvents / 4);

    for (int64_t s = 200; s < 200 + total; ++s)
    {
        TimedEvent e;
        e.sample = s;
        e.event.type = VsEventType::pitchBend;
        e.event.bend = (int16_t) ((s % 2 == 0) ? 100 : -100);
        events.push_back (e);
    }

    events.push_back (noteOffAt (200 + total, 60));
    feeder.runTo (200 + total + (int64_t) (4.0 * rate), 512, events);
    worker.runStepForTests();

    const auto takes = worker.getTakes();
    REQUIRE (takes.size() >= 1);
    std::size_t sum = 0;

    for (const auto& t : takes)
        sum += t->events.size();

    CAPTURE (takes.size(), sum);
    CHECK (sum <= CaptureWorker::maxTotalTakeEvents);
    CHECK (takes.front()->number > (int) takes.size()); // the oldest parts were dropped
    CHECK (worker.getStatus().overflow.records == 0);
}

TEST_CASE ("CaptureWorker renders the WAV cache in slices; stop aborts a running job", "[plugin][captureworker][wav]")
{
    TestFolder dir;
    CaptureTap tap;
    tap.prepare (rate, 512);
    CaptureWorker worker (tap, dir.folder);
    worker.setManualSteppingForTests (true);
    worker.start();

    TapFeeder feeder (tap);
    feeder.worker = &worker;
    feeder.audio = sine;

    // A 5 s note: its WAV needs several slices of 65536 frames
    const std::vector<TimedEvent> events { noteOnAt (4800, 64), noteOffAt (4800 + (int64_t) (5.0 * rate), 64) };
    feeder.runTo ((int64_t) (5.1 * rate + 3.2 * rate), 512, events);

    // Closed, WAV queued (the folder is created when the job opens its file)
    auto takes = worker.getTakes();
    REQUIRE (takes.size() == 1);
    CHECK (takes.front()->audio == TakeAudio::full);
    CHECK_FALSE (takes.front()->overview.empty());

    worker.runStepForTests();
    CHECK (worker.getWavState (1).kind == TakeWavState::Kind::preparing);
    CHECK (dir.folder.isDirectory());
    CHECK (dir.folder.getChildFile (CaptureExporter::heartbeatFileName).existsAsFile());

    SECTION ("finished: ready, length = span, sample 0 = the span start")
    {
        stepUntilWavDone (worker, 1);
        const auto state = worker.getWavState (1);
        REQUIRE (state.kind == TakeWavState::Kind::ready);

        double wavRate = 0.0;
        int bits = 0;
        const auto audio = vocalsynth::test::readWav (state.file, wavRate, bits);
        const auto span = TakeRenderer::computeAudioSpan (*takes.front(), RenderOptions {});
        CHECK (audio.getNumSamples() == span.numSamples);
        CHECK (span.startSample == takes.front()->events.front().estimatedSample);
        CHECK (wavRate == Catch::Approx (rate));
        CHECK (bits == 24);

        for (int i = 0; i < 1000; i += 37)
            CHECK (audio.getSample (0, i) == Catch::Approx (sine (span.startSample + i)).margin (1.0e-6));

        // Only the WAV and the heartbeat in the folder: no temporary files left
        CHECK (dir.folder.getNumberOfChildFiles (juce::File::findFiles) == 2);
    }

    SECTION ("stop during the job: the temporary file is deleted")
    {
        worker.stop();
        CHECK (worker.getWavState (1).kind == TakeWavState::Kind::unavailable);
        CHECK (dir.folder.getNumberOfChildFiles (juce::File::findFiles) == 1); // the heartbeat
    }

    SECTION ("Clear deletes the cached WAV")
    {
        stepUntilWavDone (worker, 1);
        const auto file = worker.getWavState (1).file;
        REQUIRE (file.existsAsFile());
        tap.requestClear();
        feeder.runTo (feeder.position + 4800, 480);
        worker.runStepForTests();
        CHECK (worker.getTakes().empty());
        CHECK_FALSE (file.exists());
    }
}

TEST_CASE ("CaptureWorker joins a running WAV job quickly (thread)", "[plugin][captureworker][wav][lifecycle]")
{
    TestFolder dir;
    CaptureTap tap;
    tap.prepare (rate, 512);
    CaptureWorker worker (tap, dir.folder);
    worker.start();

    TapFeeder feeder (tap);
    feeder.audio = sine;
    const std::vector<TimedEvent> events { noteOnAt (4800, 64), noteOffAt (4800 + (int64_t) (20.0 * rate), 64) };

    // Fed faster than real time; the thread drains every 10 ms (the rings hold > 2 s)
    while (feeder.position < (int64_t) (24.0 * rate))
    {
        feeder.runTo (feeder.position + (int64_t) (0.5 * rate), 512, events);
        std::this_thread::sleep_for (std::chrono::milliseconds (40));
    }

    for (int i = 0; i < 100 && worker.getWavState (1).kind != TakeWavState::Kind::preparing; ++i)
        std::this_thread::sleep_for (std::chrono::milliseconds (5));

    CHECK (worker.getWavState (1).kind == TakeWavState::Kind::preparing); // 20 s = 15 slices: still running
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    worker.stop();
    CHECK (juce::Time::getMillisecondCounterHiRes() - t0 < 100.0);
    CHECK (tap.getOverflowCounts().records == 0);
    CHECK (tap.getOverflowCounts().audioSamples == 0);
    CHECK (dir.folder.getNumberOfChildFiles (juce::File::findFiles) == 1); // the heartbeat, no temp file
}

TEST_CASE ("CaptureWorker re-renders a synced WAV when the offset parameters change", "[plugin][captureworker][wav]")
{
    TestFolder dir;
    CaptureTap tap;
    tap.prepare (rate, 1024);
    std::atomic<float> mode { 0.0f }, offsetMs { 0.0f }, fineMs { 0.0f };
    CaptureWorkerSources sources;
    sources.offsetMode = &mode;
    sources.offsetMs = &offsetMs;
    sources.fineOffsetMs = &fineMs;
    sources.observedBlockSize = [] { return 1024; };
    CaptureWorker worker (tap, dir.folder, sources);
    worker.setManualSteppingForTests (true);
    worker.start();

    TapFeeder feeder (tap);
    feeder.worker = &worker;
    feeder.audio = sine;
    const int64_t playStart = 48000, playStop = 48000 + 4 * 24000;
    feeder.transport = [=] (int64_t s) { return vocalsynth::test::playingTransport (s, rate, 120.0, playStart, 0.0, playStop); };
    const std::vector<TimedEvent> events { noteOnAt (playStart + 3000, 60), noteOffAt (playStart + 30000, 60) };
    feeder.runTo (playStop + (int64_t) rate, 512, events);
    worker.runStepForTests();

    const auto takes = worker.getTakes();
    REQUIRE (takes.size() == 1);
    CHECK (takes.front()->mode == TakeMode::synced);
    CHECK (takes.front()->observedBlockSize == 1024);
    stepUntilWavDone (worker, 1);
    const auto first = worker.getWavState (1);
    REQUIRE (first.kind == TakeWavState::Kind::ready);
    const auto firstSize = first.file.getSize();

    // Auto (2 x 1024) -> Manual 100 ms: the WAV span moves (start and end) by the new round trip
    mode = 1.0f;
    offsetMs = 100.0f;
    worker.runStepForTests();
    CHECK (worker.getWavState (1).kind == TakeWavState::Kind::preparing);
    stepUntilWavDone (worker, 1);
    const auto second = worker.getWavState (1);
    REQUIRE (second.kind == TakeWavState::Kind::ready);
    CHECK (second.file.existsAsFile());

    double wavRate = 0.0;
    int bits = 0;
    const auto audio = vocalsynth::test::readWav (second.file, wavRate, bits);
    RenderOptions o;
    o.roundTripSamples = 4800;
    const auto span = TakeRenderer::computeAudioSpan (*takes.front(), o);
    CHECK (audio.getNumSamples() == span.numSamples);
    CHECK (TakeRenderer::roundTripFor (*takes.front(), sources.readOffset()) == 4800);
    CHECK (firstSize > 0);
    CHECK (dir.folder.getNumberOfChildFiles (juce::File::findFiles, "*.wav") == 1); // the old one is gone
}

TEST_CASE ("CaptureWorker closes a free take when the host stops processing", "[plugin][captureworker]")
{
    TestFolder dir;
    CaptureTap tap;
    tap.prepare (rate, 512);
    CaptureWorker worker (tap, dir.folder);
    worker.setManualSteppingForTests (true);
    CaptureWorkerOptions options;
    options.takeGapSeconds = TakeSegmenter::minTakeGapSeconds;
    worker.setOptions (options);
    worker.start();

    TapFeeder feeder (tap);
    feeder.worker = &worker;
    feeder.settings.captureAudio = false;
    feeder.runTo (24000, 512, { noteOnAt (1000, 60), noteOffAt (12000, 60) });
    worker.runStepForTests();
    CHECK (worker.getTakes().empty());
    CHECK (worker.getStatus().state == TakeSegmenter::State::freeRecording);

    // No more blocks: after takeGapSeconds of wall-clock time the take closes anyway
    std::this_thread::sleep_for (std::chrono::milliseconds (600));
    worker.runStepForTests();
    REQUIRE (worker.getTakes().size() == 1);
    CHECK (worker.getTakes().front()->noteCount == 1);
}

TEST_CASE ("CaptureWorker deletes stale instance folders at start", "[plugin][captureworker][files]")
{
    TestFolder dir;
    TestFolder stale;
    TestFolder fresh;
    REQUIRE (stale.folder.createDirectory());
    REQUIRE (fresh.folder.createDirectory());
    const auto oldHeartbeat = stale.folder.getChildFile (CaptureExporter::heartbeatFileName);
    REQUIRE (oldHeartbeat.replaceWithText ("x"));
    REQUIRE (oldHeartbeat.setLastModificationTime (juce::Time::getCurrentTime() - juce::RelativeTime::hours (25.0)));
    CaptureExporter::touchHeartbeat (fresh.folder);

    CaptureTap tap;
    tap.prepare (rate, 512);
    CaptureWorker worker (tap, dir.folder);
    worker.setManualSteppingForTests (true);
    worker.start();

    CHECK_FALSE (stale.folder.exists());
    CHECK (fresh.folder.isDirectory());
}

//==============================================================================
// M3 review fixes (RT-1, RT-3, F1, F4, F7)

TEST_CASE ("CaptureWorker keeps draining while the file lock is held (slow WAV copy)", "[plugin][captureworker][wav][realtime]")
{
    TestFolder dir;
    CaptureTap tap;
    tap.prepare (rate, 512);
    CaptureWorker worker (tap, dir.folder);
    CaptureWorkerOptions options;
    options.takeGapSeconds = TakeSegmenter::minTakeGapSeconds;
    worker.setOptions (options);
    worker.start();

    TapFeeder feeder (tap);
    feeder.audio = sine;

    {
        // persistWav() on the message thread copies a big WAV to a USB drive meanwhile: the take below
        // closes, its WAV job writes its slices and then waits for the lock to finish. Nothing may stop
        // the drain: 8 s of audio arrive within ~0.7 s of wall time (the rings hold ~2.7 s).
        const auto lock = worker.holdFileLockForTests();
        const auto events = freeNote();

        while (feeder.position < (int64_t) (8.0 * rate))
        {
            feeder.runTo (feeder.position + (int64_t) (0.5 * rate), 512, events);
            std::this_thread::sleep_for (std::chrono::milliseconds (40));
        }

        CHECK (tap.getOverflowCounts().records == 0);
        CHECK (tap.getOverflowCounts().audioSamples == 0);
        REQUIRE (worker.getTakes().size() == 1);
        CHECK (worker.getWavState (1).kind == TakeWavState::Kind::preparing); // finishing waits for the lock
    }

    for (int i = 0; i < 400 && worker.getWavState (1).kind == TakeWavState::Kind::preparing; ++i)
        std::this_thread::sleep_for (std::chrono::milliseconds (5));

    const auto state = worker.getWavState (1);
    CHECK (state.kind == TakeWavState::Kind::ready);
    CHECK (state.file.existsAsFile());
    worker.stop();
}

TEST_CASE ("CaptureWorker: suspend() right after resume() still drains what was pushed in between", "[plugin][captureworker][lifecycle]")
{
    TestFolder dir;
    CaptureTap tap;
    tap.prepare (rate, 512);
    CaptureWorker worker (tap, dir.folder);
    worker.start();
    TapFeeder feeder (tap);
    int64_t next = 1000;

    // prepareToPlay -> a few blocks -> prepareToPlay in a tight loop (pluginval, a busy host)
    for (int round = 0; round < 200; ++round)
    {
        worker.suspend();
        tap.prepare (rate, 512); // same rate: the clock and the tap's note state continue
        worker.resume();

        feeder.runTo (feeder.position + 2048, 512, { noteOnAt (next, 60), noteOffAt (next + 500, 60) });
        next = feeder.position + 1000;

        worker.suspend(); // must not return before the worker drained the rings once more
        INFO ("round " << round);
        REQUIRE (tap.getRecordRing().size() == 0);
        worker.resume();
    }

    worker.stop();
}

TEST_CASE ("CaptureWorker: the open take's note count is live", "[plugin][captureworker]")
{
    TestFolder dir;
    CaptureTap tap;
    tap.prepare (rate, 512);
    CaptureWorker worker (tap, dir.folder);
    worker.setManualSteppingForTests (true);
    worker.start();

    TapFeeder feeder (tap);
    feeder.worker = &worker;
    feeder.settings.captureAudio = false;
    feeder.runTo (24000, 512, { noteOnAt (1000, 60), noteOffAt (6000, 60), noteOnAt (12000, 62) });
    worker.runStepForTests();

    const auto open = worker.getOpenTake();
    REQUIRE (open != nullptr);
    CHECK (open->noteCount == 2);
    CHECK (worker.getStatus().recordingTakeNumber == open->number);
}

TEST_CASE ("CaptureWorker: a take whose export has no note is not listed", "[plugin][captureworker]")
{
    TestFolder dir;
    CaptureTap tap;
    tap.prepare (rate, 512); // Auto round trip: 2 x 512
    CaptureWorker worker (tap, dir.folder);
    worker.setManualSteppingForTests (true);
    worker.start();

    // A note hummed before Play ends 10 ms after Play (compensated: before it). The synced take opens with
    // that note (synthetic note-on) and gets nothing else: its export would be empty.
    TapFeeder feeder (tap);
    feeder.worker = &worker;
    feeder.settings.captureAudio = false;
    const int64_t playStart = 48000, playStop = 48000 + 4 * 24000;
    feeder.transport = [=] (int64_t s) { return vocalsynth::test::playingTransport (s, rate, 120.0, playStart, 0.0, playStop); };
    feeder.runTo (playStop + (int64_t) rate, 512, { noteOnAt (1000, 60), noteOffAt (playStart + 480, 60) });
    worker.runStepForTests();

    const auto takes = worker.getTakes();
    REQUIRE (takes.size() == 1);
    CHECK (takes.front()->mode == TakeMode::free); // the hummed note before Play
    CHECK (takes.front()->noteCount == 1);
}

TEST_CASE ("CaptureWorker: a sample-rate change keeps the open take's audio and never re-renders old takes",
           "[plugin][captureworker][wav]")
{
    TestFolder dir;
    CaptureTap tap;
    tap.prepare (rate, 512);
    std::atomic<float> mode { 0.0f }, offsetMs { 0.0f }, fineMs { 0.0f };
    CaptureWorkerSources sources;
    sources.offsetMode = &mode;
    sources.offsetMs = &offsetMs;
    sources.fineOffsetMs = &fineMs;
    CaptureWorker worker (tap, dir.folder, sources);
    worker.setManualSteppingForTests (true);
    worker.start();

    // A synced take is still open (transport playing) when the user changes FL's sample rate
    TapFeeder feeder (tap);
    feeder.worker = &worker;
    feeder.audio = sine;
    const int64_t playStart = 48000;
    feeder.transport = [=] (int64_t s) { return vocalsynth::test::playingTransport (s, rate, 120.0, playStart, 0.0); };
    feeder.runTo (playStart + 3 * 24000, 512, { noteOnAt (playStart + 3000, 60), noteOffAt (playStart + 30000, 60) });
    REQUIRE (worker.getTakes().empty());

    worker.suspend();
    tap.prepare (44100.0, 512); // clearing reset: the clock restarts at 0
    worker.resume();

    TapFeeder after (tap); // the new clock, transport stopped, other audio
    after.worker = &worker;
    after.audio = [] (int64_t) { return 0.5f; };
    after.runTo ((int64_t) (0.5 * 44100.0), 512);
    worker.runStepForTests();

    // The take the reset closed still has its audio (of its own clock epoch) and gets its WAV
    const auto takes = worker.getTakes();
    REQUIRE (takes.size() == 1);
    const auto& take = *takes.front();
    CHECK (take.mode == TakeMode::synced);
    CHECK (take.audio != TakeAudio::none);
    CHECK_FALSE (take.overview.empty());
    stepUntilWavDone (worker, take.number);
    const auto first = worker.getWavState (take.number);
    REQUIRE (first.kind == TakeWavState::Kind::ready);

    double wavRate = 0.0;
    int bits = 0;
    const auto audio = vocalsynth::test::readWav (first.file, wavRate, bits);
    CHECK (wavRate == Catch::Approx (rate));
    RenderOptions o;
    o.roundTripSamples = TakeRenderer::roundTripFor (take, sources.readOffset());
    const auto span = TakeRenderer::computeAudioSpan (take, o);
    REQUIRE (audio.getNumSamples() == span.numSamples);

    for (int i = 0; i < 2000; i += 41)
        CHECK (audio.getSample (0, i) == Catch::Approx (sine (span.startSample + i)).margin (1.0e-6));

    // An offset change later must not re-render it from the new clock (the old epoch's audio is gone)
    after.runTo (after.position + (int64_t) (0.5 * 44100.0), 512);
    worker.runStepForTests();
    mode = 1.0f;
    offsetMs = 100.0f;
    worker.runStepForTests();
    const auto second = worker.getWavState (take.number);
    CHECK (second.kind == TakeWavState::Kind::ready);
    CHECK (second.file == first.file);
    CHECK (second.file.existsAsFile());
}
