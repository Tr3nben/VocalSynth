#include "vocalsynth/CaptureHistory.h"

#include <algorithm>
#include <cmath>

namespace vocalsynth
{

static_assert ((CaptureHistory::eventCapacity & (CaptureHistory::eventCapacity - 1)) == 0);

namespace
{
constexpr auto chunkSize = static_cast<int64_t> (CaptureHistory::audioChunkSamples);

/** Timeline segments are evicted in batches (erasing the front of a vector one by one would be quadratic). */
constexpr std::size_t timelineEvictBatch = 64;
} // namespace

void CaptureHistory::prepare (double audioHistoryMinutes)
{
    if (ring.size() != eventCapacity)
        ring.assign (eventCapacity, CaptureRecord {});

    audioMinutes = std::clamp (audioHistoryMinutes, minAudioMinutes, maxAudioMinutes);
    clear();
    configureAudio();
}

void CaptureHistory::setAudioHistoryMinutes (double minutes)
{
    audioMinutes = std::clamp (minutes, minAudioMinutes, maxAudioMinutes);
    configureAudio();
}

void CaptureHistory::clear()
{
    ringStart = 0;
    ringCount = 0;
    settingsBeforeRing = {};
    timeline.reset (sampleRate);
    audioRange = {};
    audioRunActive = false;
    audioFloor = 0;
    holes.clear();
    missingAudioSamples = 0;
    endSample = 0;
}

//==============================================================================
void CaptureHistory::addRecord (const CaptureRecord& record)
{
    switch (record.type)
    {
        case CaptureRecordType::reset:
        {
            const auto reset = record.asReset();
            maxBlockSize = reset.maxBlockSize;

            if (reset.clearsHistory)
            {
                sampleRate = reset.sampleRate > 0.0 ? reset.sampleRate : 48000.0;
                clear();
                configureAudio();
            }
            return;
        }

        case CaptureRecordType::audio:
            return; // the worker pops the frame and calls addAudio()

        case CaptureRecordType::transport:
            timeline.addTransport (record.sample, record.asTransport());
            break;

        case CaptureRecordType::marker:
        {
            const auto kind = record.asMarker().kind;

            if (kind == CaptureMarkerKind::clear)
                applyClear (record.sample);
            else if (kind == CaptureMarkerKind::audioOff || kind == CaptureMarkerKind::captureOff)
                setAudioOff (record.sample);

            pushRing (record);
            break;
        }

        case CaptureRecordType::settings:
        case CaptureRecordType::event:
        case CaptureRecordType::gap:
            pushRing (record);
            break;
    }

    endSample = std::max (endSample, record.sample + 1);
    evictTimeline();
}

void CaptureHistory::pushRing (const CaptureRecord& record)
{
    if (ring.size() != eventCapacity)
        ring.assign (eventCapacity, CaptureRecord {}); // used without prepare(): allocate on first use

    if (ringCount == eventCapacity)
    {
        const auto& oldest = ringAt (0);

        if (oldest.type == CaptureRecordType::settings)
            settingsBeforeRing = oldest.asSettings();

        ringStart = (ringStart + 1) & (eventCapacity - 1);
        --ringCount;
    }

    ring[(ringStart + ringCount) & (eventCapacity - 1)] = record;
    ++ringCount;
}

void CaptureHistory::applyClear (int64_t markerSample)
{
    // Keep the state in force at the marker (bend range, tempo, bar grid) for the next take
    settingsBeforeRing = getSettingsAt (markerSample);
    ringCount = 0;
    timeline.evictBefore (markerSample);

    audioFloor = std::max (audioFloor, markerSample);
    audioRange.from = std::min (std::max (audioRange.from, audioFloor), audioRange.to);
    pruneHoles();
}

void CaptureHistory::evictTimeline()
{
    const auto& segments = timeline.getSegments();

    if (segments.size() <= timelineEvictBatch)
        return;

    auto limit = getOldestEventSample();

    if (audioRange.from < audioRange.to)
        limit = std::min (limit, audioRange.from);

    const auto it = std::upper_bound (segments.begin(), segments.end(), limit,
                                      [] (int64_t s, const TimelineSegment& seg) { return s < seg.startSample; });
    const auto evictable = static_cast<std::size_t> (std::max<std::ptrdiff_t> (0, (it - segments.begin()) - 1));

    if (evictable >= timelineEvictBatch)
        timeline.evictBefore (limit);
}

//==============================================================================
void CaptureHistory::configureAudio()
{
    const auto wanted = std::min (audioMinutes * 60.0 * sampleRate, static_cast<double> (maxAudioBytes / sizeof (float)));
    const auto numChunks = static_cast<std::size_t> (std::max (1.0, std::ceil (wanted / static_cast<double> (chunkSize))));

    if (numChunks == chunks.size())
        return;

    // Remap the newest audio that still fits into the new ring of chunks (worker thread, allocates)
    Chunks newChunks (numChunks);
    const auto capacity = static_cast<int64_t> (numChunks) * chunkSize;

    if (audioRange.from < audioRange.to)
    {
        const auto keepFrom = std::max (audioRange.from, audioRange.to - capacity);
        std::vector<float> piece (static_cast<std::size_t> (chunkSize));

        for (auto s = keepFrom; s < audioRange.to; s += chunkSize)
        {
            const auto n = static_cast<std::size_t> (std::min (chunkSize, audioRange.to - s));
            readChunks (chunks, s, std::span<float> { piece.data(), n });
            writeAudio (newChunks, s, std::span<const float> { piece.data(), n });
        }

        audioRange.from = keepFrom;
        pruneHoles();
    }

    chunks = std::move (newChunks);
}

void CaptureHistory::writeAudio (Chunks& target, int64_t startSample, std::span<const float> samples)
{
    const auto numChunks = static_cast<int64_t> (target.size());
    std::size_t done = 0;

    while (done < samples.size())
    {
        const auto s = startSample + static_cast<int64_t> (done);
        const auto offset = s % chunkSize;
        const auto n = std::min (samples.size() - done, static_cast<std::size_t> (chunkSize - offset));
        auto& chunk = target[static_cast<std::size_t> ((s / chunkSize) % numChunks)];

        if (chunk == nullptr)
            chunk = std::make_unique<float[]> (static_cast<std::size_t> (chunkSize));

        std::copy_n (samples.data() + done, n, chunk.get() + offset);
        done += n;
    }
}

void CaptureHistory::readChunks (const Chunks& source, int64_t startSample, std::span<float> destination) noexcept
{
    const auto numChunks = static_cast<int64_t> (source.size());
    std::size_t done = 0;

    while (done < destination.size())
    {
        const auto s = startSample + static_cast<int64_t> (done);
        const auto offset = s % chunkSize;
        const auto n = std::min (destination.size() - done, static_cast<std::size_t> (chunkSize - offset));
        const auto& chunk = source[static_cast<std::size_t> ((s / chunkSize) % numChunks)];

        if (chunk != nullptr)
            std::copy_n (chunk.get() + offset, n, destination.data() + done);
        else
            std::fill_n (destination.data() + done, n, 0.0f);

        done += n;
    }
}

void CaptureHistory::addAudio (int64_t startSample, std::span<const float> samples)
{
    const auto end = startSample + static_cast<int64_t> (samples.size());

    if (samples.empty() || startSample < 0 || end <= audioFloor || chunks.empty())
        return;

    const auto capacity = static_cast<int64_t> (getAudioCapacitySamples());

    if (! audioRunActive || audioRange.from == audioRange.to)
    {
        audioRange = { startSample, startSample };
        holes.clear();
        audioRunActive = true;
    }
    else if (startSample > audioRange.to)
    {
        // Dropped frames: zero-fill the hole so later audio stays at its sample position
        const auto hole = startSample - audioRange.to;
        missingAudioSamples += static_cast<uint64_t> (hole);

        if (hole >= capacity)
        {
            audioRange = { startSample, startSample };
            holes.clear();
        }
        else
        {
            const std::vector<float> zeros (static_cast<std::size_t> (std::min (hole, chunkSize)), 0.0f);

            for (auto s = audioRange.to; s < startSample; s += chunkSize)
                writeAudio (chunks, s,
                            std::span<const float> { zeros.data(), static_cast<std::size_t> (std::min (chunkSize, startSample - s)) });

            holes.push_back ({ audioRange.to, startSample });
        }
    }

    writeAudio (chunks, startSample, samples);
    audioRange.to = std::max (audioRange.to, end);
    audioRange.from = std::max ({ audioRange.from, audioRange.to - capacity, audioFloor });
    pruneHoles();
    endSample = std::max (endSample, end);
}

void CaptureHistory::setAudioOff (int64_t sample)
{
    if (! audioRunActive)
        return;

    // The zero padding of the tap's last partial frame after the marker is not stored audio
    audioRange.to = std::clamp (sample, audioRange.from, audioRange.to);
    audioRunActive = false;
    pruneHoles();
}

void CaptureHistory::pruneHoles() noexcept
{
    std::erase_if (holes, [this] (const AudioRange& h) { return h.to <= audioRange.from || h.from >= audioRange.to; });
}

CaptureHistory::AudioRange CaptureHistory::getAudioRange() const noexcept
{
    return audioRange;
}

std::size_t CaptureHistory::readAudio (int64_t fromSample, std::span<float> destination) const noexcept
{
    std::fill (destination.begin(), destination.end(), 0.0f);

    const auto lo = std::max (fromSample, audioRange.from);
    const auto hi = std::min (fromSample + static_cast<int64_t> (destination.size()), audioRange.to);

    if (lo >= hi || chunks.empty())
        return 0;

    readChunks (chunks, lo, destination.subspan (static_cast<std::size_t> (lo - fromSample), static_cast<std::size_t> (hi - lo)));

    auto stored = hi - lo;

    for (const auto& h : holes)
        stored -= std::max<int64_t> (0, std::min (hi, h.to) - std::max (lo, h.from));

    return static_cast<std::size_t> (stored);
}

//==============================================================================
int64_t CaptureHistory::getOldestEventSample() const noexcept
{
    return ringCount > 0 ? ringAt (0).sample : endSample;
}

int64_t CaptureHistory::getEndSample() const noexcept
{
    return endSample;
}

std::size_t CaptureHistory::lowerBoundIndex (int64_t sample) const noexcept
{
    std::size_t lo = 0;
    std::size_t hi = ringCount;

    while (lo < hi)
    {
        const auto mid = lo + (hi - lo) / 2;

        if (ringAt (mid).sample < sample)
            lo = mid + 1;
        else
            hi = mid;
    }

    return lo;
}

std::size_t CaptureHistory::upperBoundIndex (int64_t sample) const noexcept
{
    std::size_t lo = 0;
    std::size_t hi = ringCount;

    while (lo < hi)
    {
        const auto mid = lo + (hi - lo) / 2;

        if (ringAt (mid).sample <= sample)
            lo = mid + 1;
        else
            hi = mid;
    }

    return lo;
}

std::vector<CaptureRecord> CaptureHistory::copyRecords (int64_t fromSample, int64_t toSample) const
{
    std::vector<CaptureRecord> result;
    const auto first = lowerBoundIndex (fromSample);
    const auto last = lowerBoundIndex (toSample);

    if (last > first)
    {
        result.reserve (last - first);

        for (auto i = first; i < last; ++i)
            result.push_back (ringAt (i));
    }

    return result;
}

CaptureSettingsInfo CaptureHistory::getSettingsAt (int64_t sample) const noexcept
{
    for (auto i = upperBoundIndex (sample); i > 0; --i)
    {
        const auto& r = ringAt (i - 1);

        if (r.type == CaptureRecordType::settings)
            return r.asSettings();
    }

    return settingsBeforeRing;
}

std::size_t CaptureHistory::getAudioCapacitySamples() const noexcept
{
    return chunks.size() * static_cast<std::size_t> (audioChunkSamples);
}

std::size_t CaptureHistory::getAllocatedAudioBytes() const noexcept
{
    const auto allocated = static_cast<std::size_t> (
        std::count_if (chunks.begin(), chunks.end(), [] (const auto& c) { return c != nullptr; }));
    return allocated * static_cast<std::size_t> (audioChunkSamples) * sizeof (float);
}

} // namespace vocalsynth
