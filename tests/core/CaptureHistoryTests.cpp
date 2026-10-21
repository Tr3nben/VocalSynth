// CaptureHistory (docs/M3-DESIGN.md 5.1, 16). Owner: agent A.

#include "CaptureTestHelpers.h"

#include "vocalsynth/CaptureHistory.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace vocalsynth;
using namespace vocalsynth::test;
using Catch::Approx;

namespace
{
constexpr auto chunk = static_cast<std::size_t> (CaptureHistory::audioChunkSamples);

CaptureHistory makeHistory (double minutes = 1.0, double sr = 48000.0, int maxBlock = 512)
{
    CaptureHistory h;
    h.prepare (minutes);
    h.addRecord (CaptureRecord::makeReset (0, { sr, maxBlock, true }));
    return h;
}

/** Appends scenarioSignal audio [from, to) in frames of frameSize. */
void addSignal (CaptureHistory& h, int64_t from, int64_t to, int64_t frameSize = 4096)
{
    std::vector<float> frame (static_cast<std::size_t> (frameSize));

    for (auto s = from; s < to; s += frameSize)
    {
        const auto n = std::min (frameSize, to - s);
        for (int64_t i = 0; i < n; ++i)
            frame[static_cast<std::size_t> (i)] = scenarioSignal (s + i);
        h.addAudio (s, std::span<const float> { frame.data(), static_cast<std::size_t> (n) });
    }
}

bool matchesSignal (const CaptureHistory& h, int64_t from, std::size_t n)
{
    std::vector<float> got (n, -1.0f);
    h.readAudio (from, got);

    for (std::size_t i = 0; i < n; ++i)
        if (got[i] != scenarioSignal (from + static_cast<int64_t> (i)))
            return false;

    return true;
}

CaptureRecord event (int64_t sample, int note = 60)
{
    return CaptureRecord::makeEvent (sample, VsEvent::makeNoteOn (0, 1, static_cast<uint8_t> (note), 100), {});
}

CaptureRecord settingsWithRange (int64_t sample, int range)
{
    CaptureSettingsInfo info;
    info.bendRangeSemitones = range;
    return CaptureRecord::makeSettings (sample, info);
}

CaptureRecord playingRecord (int64_t sample, double ppq, double bpm = 120.0)
{
    TransportInfo t;
    t.isPlaying = true;
    t.hasPpq = true;
    t.ppqPosition = ppq;
    t.hasBpm = true;
    t.bpm = bpm;
    return CaptureRecord::makeTransport (sample, t);
}
} // namespace

TEST_CASE ("CaptureHistory: audio capacity from the look-back, capped at maxAudioBytes", "[capturehistory]")
{
    CHECK (makeHistory (1.0, 48000.0).getAudioCapacitySamples() == 44 * chunk);    // 2.88 M -> 44 chunks
    CHECK (makeHistory (10.0, 48000.0).getAudioCapacitySamples() == 440 * chunk);  // 28.8 M
    CHECK (makeHistory (10.0, 44100.0).getAudioCapacitySamples() == 404 * chunk);  // 26.46 M
    CHECK (makeHistory (10.0, 96000.0).getAudioCapacitySamples() == 879 * chunk);  // 57.6 M
    // 192 kHz: capped at 256 MiB of floats = 1024 chunks (5.8 min)
    CHECK (makeHistory (10.0, 192000.0).getAudioCapacitySamples() == 1024 * chunk);
    CHECK (makeHistory (30.0, 192000.0).getAudioCapacitySamples() * sizeof (float) == CaptureHistory::maxAudioBytes);
    // Minutes are clamped to [1, 30]
    CHECK (makeHistory (0.1, 48000.0).getAudioCapacitySamples() == 44 * chunk);
    CHECK (makeHistory (100.0, 48000.0).getAudioCapacitySamples() == makeHistory (30.0, 48000.0).getAudioCapacitySamples());

    // Nothing is allocated before audio arrives
    CHECK (makeHistory (10.0, 192000.0).getAllocatedAudioBytes() == 0);
}

TEST_CASE ("CaptureHistory: lazy chunks, eviction of the oldest audio, reads outside the range", "[capturehistory]")
{
    auto h = makeHistory (1.0);
    CHECK (h.getAudioRange().from == h.getAudioRange().to);

    addSignal (h, 0, 70000, 256);
    CHECK (h.getAllocatedAudioBytes() == 2 * chunk * sizeof (float));
    CHECK (h.getAudioRange().from == 0);
    CHECK (h.getAudioRange().to == 70000);
    CHECK (matchesSignal (h, 0, 70000));
    CHECK (h.getEndSample() == 70000);

    // Past the capacity: the oldest audio is evicted, the chunks are reused
    const auto capacity = static_cast<int64_t> (h.getAudioCapacitySamples());
    addSignal (h, 70000, 3'500'000);
    CHECK (h.getAllocatedAudioBytes() == 44 * chunk * sizeof (float));
    CHECK (h.getAudioRange().to == 3'500'000);
    CHECK (h.getAudioRange().from == 3'500'000 - capacity);
    CHECK (matchesSignal (h, h.getAudioRange().from, 100000));
    CHECK (matchesSignal (h, 3'500'000 - 5000, 5000));

    // Outside the stored range: zeros, and the return value counts only stored samples
    std::vector<float> buffer (1000, -1.0f);
    CHECK (h.readAudio (0, buffer) == 0);
    CHECK (std::all_of (buffer.begin(), buffer.end(), [] (float x) { return x == 0.0f; }));
    CHECK (h.readAudio (3'500'000 - 500, buffer) == 500);
    CHECK (buffer[499] == scenarioSignal (3'500'000 - 1));
    CHECK (buffer[500] == 0.0f);
    CHECK (h.readAudio (h.getAudioRange().from - 10, buffer) == 990);
    CHECK (buffer[9] == 0.0f);
    CHECK (buffer[10] == scenarioSignal (h.getAudioRange().from));
    CHECK (h.getMissingAudioSamples() == 0);
}

TEST_CASE ("CaptureHistory: dropped frames are zero-filled holes; later audio stays aligned", "[capturehistory]")
{
    auto h = makeHistory (1.0);
    addSignal (h, 0, 256, 256);
    addSignal (h, 1024, 1280, 256); // frames 256..1023 were dropped by the tap

    CHECK (h.getMissingAudioSamples() == 768);
    CHECK (h.getAudioRange().from == 0);
    CHECK (h.getAudioRange().to == 1280);

    std::vector<float> got (1280, -1.0f);
    CHECK (h.readAudio (0, got) == 512); // stored audio only
    CHECK (got[255] == scenarioSignal (255));
    CHECK (got[256] == 0.0f);
    CHECK (got[1023] == 0.0f);
    CHECK (got[1024] == scenarioSignal (1024));

    // A hole longer than the whole history starts a new run
    const auto capacity = static_cast<int64_t> (h.getAudioCapacitySamples());
    addSignal (h, 1280 + capacity, 1280 + capacity + 256, 256);
    CHECK (h.getAudioRange().from == 1280 + capacity);
    CHECK (h.getMissingAudioSamples() == 768 + static_cast<uint64_t> (capacity));
    CHECK (matchesSignal (h, 1280 + capacity, 256));
}

TEST_CASE ("CaptureHistory: audio off / capture off end the run at the marker", "[capturehistory]")
{
    const auto kind = GENERATE (CaptureMarkerKind::audioOff, CaptureMarkerKind::captureOff);
    auto h = makeHistory (1.0);
    addSignal (h, 0, 512, 256);
    h.addRecord (CaptureRecord::makeMarker (300, { kind, 8, 30.0 }));
    CHECK (h.getAudioRange().from == 0);
    CHECK (h.getAudioRange().to == 300); // the tap's zero padding after the marker is not audio

    // audioOn later: a new run (the range covers only the newest continuous run)
    addSignal (h, 1024, 1536, 256);
    CHECK (h.getAudioRange().from == 1024);
    CHECK (h.getAudioRange().to == 1536);
    CHECK (h.getMissingAudioSamples() == 0);

    // Other markers (bypass, requests) do not end the run
    h.addRecord (CaptureRecord::makeMarker (1536, { CaptureMarkerKind::bypassOn, 8, 30.0 }));
    h.addRecord (CaptureRecord::makeMarker (1536, { CaptureMarkerKind::newTake, 8, 30.0 }));
    addSignal (h, 1536, 2048, 256);
    CHECK (h.getAudioRange().from == 1024);
    CHECK (h.getAudioRange().to == 2048);

    // setAudioOff directly, twice: the second call does nothing
    h.setAudioOff (1800);
    h.setAudioOff (1000);
    CHECK (h.getAudioRange().to == 1800);
}

TEST_CASE ("CaptureHistory: event ring evicts the oldest records; settings survive eviction", "[capturehistory]")
{
    auto h = makeHistory (1.0);
    h.addRecord (settingsWithRange (10, 7));

    const auto total = static_cast<int64_t> (CaptureHistory::eventCapacity) + 100;
    for (int64_t i = 0; i < total; ++i)
        h.addRecord (event (100 + i * 2));

    // The settings record and the first 100 events were evicted (ring capacity 2^18 incl. the settings)
    CHECK (h.getOldestEventSample() == 100 + 2 * 100);
    CHECK (h.getEndSample() == 100 + 2 * (total - 1) + 1);
    CHECK (h.getSettingsAt (0).bendRangeSemitones == 7); // kept as "settings before the ring"
    CHECK (h.getSettingsAt (1'000'000).bendRangeSemitones == 7);

    const auto x = 100 + 2 * total;
    h.addRecord (settingsWithRange (x, 12));
    CHECK (h.getSettingsAt (x - 1).bendRangeSemitones == 7);
    CHECK (h.getSettingsAt (x).bendRangeSemitones == 12);
    CHECK (h.getSettingsAt (x + 1000).bendRangeSemitones == 12);

    // copyRecords: [from, to), oldest first
    const auto copy = h.copyRecords (1000, 1010);
    REQUIRE (copy.size() == 5);
    CHECK (copy.front().sample == 1000);
    CHECK (copy.back().sample == 1008);
    CHECK (h.copyRecords (0, 100).empty());
    CHECK (h.copyRecords (x, x + 1).size() == 1);

    // Audio and transport records do not use the ring
    const auto oldest = h.getOldestEventSample();
    h.addRecord (CaptureRecord::makeAudio (x + 10, 256));
    h.addRecord (playingRecord (x + 10, 0.0));
    CHECK (h.getOldestEventSample() == oldest);
}

TEST_CASE ("CaptureHistory: timeline segments older than the oldest record or audio are evicted", "[capturehistory]")
{
    auto h = makeHistory (1.0);

    // 200 tempo changes over 2 M samples, events every 5 samples: the ring keeps the newest 2^18 events
    for (int64_t s = 0; s < 2'000'000; s += 5)
    {
        if (s % 10000 == 0)
            h.addRecord (playingRecord (s, static_cast<double> (s) / 24000.0, 120.0 + static_cast<double> ((s / 10000) % 7)));

        h.addRecord (event (s));
    }

    const auto oldest = h.getOldestEventSample();
    CHECK (oldest > 600'000);
    const auto& segments = h.getTimeline().getSegments();
    CHECK (segments.size() < 200);
    CHECK (segments.size() >= 2);
    CHECK (segments.front().startSample <= oldest); // the segment containing the oldest record stays
    CHECK (segments.front().startSample > 0);
    CHECK (h.getTimeline().isPlayingAt (static_cast<double> (oldest)));

    // Audio older than the oldest event holds the timeline back
    auto a = makeHistory (1.0);
    addSignal (a, 0, 4096);
    for (int64_t s = 0; s < 1'000'000; s += 3)
    {
        if (s % 5000 == 0)
            a.addRecord (playingRecord (s, static_cast<double> (s) / 24000.0));
        a.addRecord (event (s));
    }
    CHECK (a.getOldestEventSample() > 200'000);
    CHECK (a.getTimeline().getSegments().front().startSample == 0);
}

TEST_CASE ("CaptureHistory: Clear drops everything before the marker but keeps the state in force", "[capturehistory]")
{
    auto h = makeHistory (1.0);
    h.addRecord (playingRecord (0, 0.0));
    h.addRecord (settingsWithRange (0, 5));
    for (int64_t s = 100; s < 48000; s += 1000)
        h.addRecord (event (s));
    addSignal (h, 0, 48000, 256);
    const auto beatsBefore = h.getTimeline().beatsAt (30000.0);

    h.addRecord (CaptureRecord::makeMarker (24000, { CaptureMarkerKind::clear, 8, 30.0 }));

    CHECK (h.copyRecords (0, 24000).empty());
    CHECK (h.getOldestEventSample() == 24000); // the clear marker itself
    CHECK (h.getSettingsAt (24000).bendRangeSemitones == 5);
    CHECK (h.getSettingsAt (100000).bendRangeSemitones == 5);
    CHECK (h.getTimeline().isPlayingAt (30000.0));
    CHECK (h.getTimeline().beatsAt (30000.0) == Approx (beatsBefore));
    CHECK (h.getAudioRange().from == 24000);
    CHECK (h.getAudioRange().to == 48000);

    std::vector<float> buffer (100, -1.0f);
    CHECK (h.readAudio (23000, buffer) == 0);
    CHECK (buffer[0] == 0.0f);

    // Records and audio after the marker are stored as usual; a frame across the marker keeps only its
    // part after it
    h.addRecord (event (50000));
    CHECK (h.copyRecords (24000, 60000).size() == 2);
    CHECK (matchesSignal (h, 24000, 1000));
}

TEST_CASE ("CaptureHistory: reset records", "[capturehistory]")
{
    auto h = makeHistory (1.0);
    h.addRecord (playingRecord (0, 0.0));
    h.addRecord (event (100));
    addSignal (h, 0, 10000, 256);

    // Same rate (engine restart): everything kept, the max block updated
    h.addRecord (CaptureRecord::makeReset (10240, { 48000.0, 2048, false }));
    CHECK (h.getMaxBlockSize() == 2048);
    CHECK (h.copyRecords (0, 1000).size() == 1);
    CHECK (h.getAudioRange().to == 10000);
    CHECK_FALSE (h.getTimeline().empty());

    // Clearing reset (new rate): everything dropped, the audio capacity follows the rate
    h.addRecord (CaptureRecord::makeReset (0, { 96000.0, 512, true }));
    CHECK (h.getSampleRate() == 96000.0);
    CHECK (h.copyRecords (0, 1'000'000).empty());
    CHECK (h.getAudioRange().from == h.getAudioRange().to);
    CHECK (h.getTimeline().empty());
    CHECK (h.getTimeline().getSampleRate() == 96000.0);
    CHECK (h.getEndSample() == 0);
    CHECK (h.getAudioCapacitySamples() == 88 * chunk); // 1 min at 96 kHz = 5.76 M

    // clear() keeps the allocations and the rate
    addSignal (h, 0, 1000);
    const auto bytes = h.getAllocatedAudioBytes();
    h.clear();
    CHECK (h.getAllocatedAudioBytes() == bytes);
    CHECK (h.getSampleRate() == 96000.0);
    CHECK (h.getAudioRange().from == h.getAudioRange().to);
}

TEST_CASE ("CaptureHistory: shrinking the look-back keeps the newest audio and frees chunks", "[capturehistory]")
{
    auto h = makeHistory (2.0);
    CHECK (h.getAudioCapacitySamples() == 88 * chunk);
    addSignal (h, 0, 5'000'000);
    CHECK (h.getAllocatedAudioBytes() == 77 * chunk * sizeof (float)); // 5 M samples touch 77 chunks

    h.setAudioHistoryMinutes (1.0);
    const auto capacity = static_cast<int64_t> (h.getAudioCapacitySamples());
    CHECK (capacity == static_cast<int64_t> (44 * chunk));
    CHECK (h.getAllocatedAudioBytes() <= 44 * chunk * sizeof (float));
    CHECK (h.getAudioRange().to == 5'000'000);
    CHECK (h.getAudioRange().from == 5'000'000 - capacity);
    CHECK (matchesSignal (h, 5'000'000 - capacity, 70000));
    CHECK (matchesSignal (h, 5'000'000 - 3000, 3000));

    // Growing again keeps everything and continues seamlessly
    h.setAudioHistoryMinutes (3.0);
    addSignal (h, 5'000'000, 5'100'000);
    CHECK (h.getAudioRange().from == 5'000'000 - capacity);
    CHECK (matchesSignal (h, 5'000'000 - capacity, 200000));
    CHECK (matchesSignal (h, 5'000'000 - 100, 100100));
}

TEST_CASE ("CaptureHistory: the timeline is bounded by its look-back even if an old record stays", "[capturehistory]")
{
    // A quiet session: one settings record at the start stays the oldest record for good, while tempo
    // automation re-syncs the transport 100 times a second (transport records never enter the ring)
    auto h = makeHistory (1.0);
    h.addRecord (settingsWithRange (0, 2));
    const auto lookback = static_cast<int64_t> (CaptureHistory::timelineLookbackSeconds * 48000.0);
    const int64_t step = 480;
    const int64_t end = lookback + 5 * 60 * 48000; // 15 minutes

    for (int64_t s = 0; s < end; s += step)
        h.addRecord (playingRecord (s, static_cast<double> (s) / 24000.0, 120.0 + static_cast<double> ((s / step) % 5) * 0.01));

    CHECK (h.getOldestEventSample() == 0);
    const auto& segments = h.getTimeline().getSegments();
    const auto maxSegments = static_cast<std::size_t> (lookback / step) + 2 * 64 + 2;
    CHECK (segments.size() <= maxSegments);
    CHECK (segments.front().startSample >= end - lookback - 64 * step - step);
    CHECK (h.getTimeline().isPlayingAt (static_cast<double> (end - 1)));

    // Grabs reach at most 120 s / 32 bars back: well inside the look-back
    CHECK (CaptureHistory::timelineLookbackSeconds >= 120.0);
    CHECK (h.getTimeline().isPlayingAt (static_cast<double> (end - 120 * 48000)));
}

TEST_CASE ("CaptureHistory: a clearing reset starts a new clock epoch and keeps the old audio aside", "[capturehistory]")
{
    auto h = makeHistory (1.0);
    const auto first = h.getClockEpoch();
    addSignal (h, 0, 100000);
    const auto bytesBefore = h.getAllocatedAudioBytes();

    // Same-rate reset: same epoch
    h.addRecord (CaptureRecord::makeReset (100000, { 48000.0, 1024, false }));
    CHECK (h.getClockEpoch() == first);
    CHECK_FALSE (h.hasRetainedAudio());

    // New rate: the clock restarts at 0 in a new epoch; the old audio stays readable under its epoch
    h.addRecord (CaptureRecord::makeReset (0, { 44100.0, 512, true }));
    CHECK (h.getClockEpoch() == first + 1);
    REQUIRE (h.hasRetainedAudio());
    CHECK (h.getRetainedEpoch() == first);
    CHECK (h.getAudioRange().from == h.getAudioRange().to);
    CHECK (h.getAudioRange (first).from == 0);
    CHECK (h.getAudioRange (first).to == 100000);

    std::vector<float> got (1000, -1.0f);
    CHECK (h.readAudio (first, 50000, got) == 1000);

    for (std::size_t i = 0; i < got.size(); ++i)
        REQUIRE (got[i] == scenarioSignal (50000 + static_cast<int64_t> (i)));

    // New audio on the new clock never shows through old positions, and the other way round
    std::vector<float> loud (4096, 0.5f);
    h.addAudio (0, loud);
    CHECK (h.readAudio (first, 0, got) == 1000);
    CHECK (got[0] == scenarioSignal (0));
    CHECK (h.readAudio (h.getClockEpoch(), 0, got) == 1000);
    CHECK (got[0] == 0.5f);
    CHECK (h.readAudio (first + 7, 0, got) == 0); // an epoch that is gone: silence
    CHECK (got[0] == 0.0f);
    CHECK (h.getAudioRange (first + 7).from == h.getAudioRange (first + 7).to);
    CHECK (h.getAllocatedAudioBytes() > bytesBefore);

    // Released by the worker once no take needs it
    h.releaseRetainedAudio();
    CHECK_FALSE (h.hasRetainedAudio());
    CHECK (h.getAudioRange (first).from == h.getAudioRange (first).to);
    CHECK (h.readAudio (first, 50000, got) == 0);
    CHECK (h.getAllocatedAudioBytes() == chunk * sizeof (float));
}
