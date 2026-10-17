#pragma once

#include "vocalsynth/CaptureRecord.h"
#include "vocalsynth/TakeTimeline.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace vocalsynth
{

/** The worker's look-back store (docs/M3-DESIGN.md 5.1): a ring of the newest non-audio records (events,
    markers, settings, gaps), the transport history as a TakeTimeline, and the newest audio in fixed-size
    RAM chunks. Owned and used by the capture worker thread only (no internal locking); allocates there,
    never on the audio thread.

    Memory (per instance): event ring eventCapacity * 32 B = 8 MiB, allocated by prepare(). Audio: float,
    audioCapacitySamples() = min (minutes * 60 * sampleRate, maxAudioBytes / 4), rounded up to whole chunks,
    allocated chunk by chunk as the history fills (an instance with capture audio off or a short session
    stays small) and reused once full: 48 kHz 10 min = 115 MB, 44.1 kHz 106 MB, 96 kHz 230 MB, 192 kHz capped
    at 256 MiB (5.8 min).

    Eviction: oldest first. Event records and audio are evicted independently (getOldestEventSample(),
    getAudioRange()). Timeline segments older than the oldest retained event or audio sample (whichever is
    older) are dropped (the segment containing that sample stays). Open takes keep their own timeline
    (TakeSegmenter), so eviction never breaks a long take; it only limits how far back a grab reaches.

    Audio is addressed by capture-clock sample. addAudio() must be contiguous with the previous frame; a
    forward jump (dropped frames) zero-fills the hole and counts it as missing; audio off (marker audioOff
    or captureOff, or setAudioOff()) ends the current run at the marker sample (the zero padding of the
    tap's last partial frame after it is not stored audio): getAudioRange() covers only the newest
    continuous run of captured audio. Bypassed blocks arrive as silence frames and do not end the run. */
class CaptureHistory
{
public:
    static constexpr std::size_t eventCapacity = std::size_t { 1 } << 18; ///< records (~22 min of True bends)
    static constexpr int audioChunkSamples = 1 << 16;                    ///< 256 KiB per chunk
    static constexpr std::size_t maxAudioBytes = std::size_t { 256 } << 20;
    static constexpr double defaultAudioMinutes = 10.0;
    static constexpr double minAudioMinutes = 1.0;
    static constexpr double maxAudioMinutes = 30.0;

    /** Worker thread. Allocates the event ring (NOT real-time; may throw std::bad_alloc) and sets the audio
        look-back (minutes clamped to [minAudioMinutes, maxAudioMinutes]). Clears everything. */
    void prepare (double audioHistoryMinutes);

    /** Changes the audio look-back; shrinking frees the oldest chunks, growing allocates lazily. */
    void setAudioHistoryMinutes (double minutes);

    /** Forgets all records, timeline segments and audio (keeps the allocations). Sample rate unchanged. */
    void clear();

    /** Stores one record in clock order: reset (sets rate / max block; clears the history if
        clearsHistory), transport (-> timeline), event / marker / settings / gap (-> ring, oldest evicted),
        marker audioOff (-> setAudioOff), marker clear (drops every record, timeline segment and audio sample
        before the marker; the transport and settings state in force at the marker are kept, so the next
        take still knows tempo, bar grid and bend range). Audio records are ignored here: the worker pops
        the frame from the audio ring and calls addAudio(). */
    void addRecord (const CaptureRecord& record);

    /** Appends one audio frame starting at startSample (see class comment). */
    void addAudio (int64_t startSample, std::span<const float> samples);

    /** Ends the current audio run at sample (captureAudio off). */
    void setAudioOff (int64_t sample);

    struct AudioRange
    {
        int64_t from = 0; ///< oldest sample of the newest continuous run that is still stored
        int64_t to = 0;   ///< one past the newest stored sample (from == to: no audio)
    };

    AudioRange getAudioRange() const noexcept;

    /** Copies audio [fromSample, fromSample + destination.size()) into destination; samples outside the
        stored range (or in zero-filled holes) are 0. Returns the number of samples that were stored audio. */
    std::size_t readAudio (int64_t fromSample, std::span<float> destination) const noexcept;

    /** Sample of the oldest record still in the ring (getEndSample() if empty). */
    int64_t getOldestEventSample() const noexcept;

    /** One past the newest sample any stored record or audio refers to. */
    int64_t getEndSample() const noexcept;

    /** Copies the stored records with sample in [fromSample, toSample), oldest first (allocates). */
    std::vector<CaptureRecord> copyRecords (int64_t fromSample, int64_t toSample) const;

    /** Settings in force at sample (the newest settings record at or before it, else the default). */
    CaptureSettingsInfo getSettingsAt (int64_t sample) const noexcept;

    const TakeTimeline& getTimeline() const noexcept { return timeline; }
    double getSampleRate() const noexcept { return sampleRate; }
    int getMaxBlockSize() const noexcept { return maxBlockSize; }

    std::size_t getAudioCapacitySamples() const noexcept;
    std::size_t getAllocatedAudioBytes() const noexcept;
    uint64_t getMissingAudioSamples() const noexcept { return missingAudioSamples; } ///< zero-filled holes

private:
    using Chunks = std::vector<std::unique_ptr<float[]>>;

    const CaptureRecord& ringAt (std::size_t index) const noexcept
    {
        return ring[(ringStart + index) & (eventCapacity - 1)];
    }

    void pushRing (const CaptureRecord& record);
    std::size_t lowerBoundIndex (int64_t sample) const noexcept; ///< first ring index with sample >= sample
    std::size_t upperBoundIndex (int64_t sample) const noexcept; ///< first ring index with sample > sample
    void applyClear (int64_t markerSample);
    void configureAudio();
    void evictTimeline();
    void pruneHoles() noexcept;
    static void writeAudio (Chunks& target, int64_t startSample, std::span<const float> samples);
    static void readChunks (const Chunks& source, int64_t startSample, std::span<float> destination) noexcept;

    double sampleRate = 48000.0;
    int maxBlockSize = 0;
    double audioMinutes = defaultAudioMinutes;

    // Event ring (eventCapacity, a power of two)
    std::vector<CaptureRecord> ring;
    std::size_t ringStart = 0;
    std::size_t ringCount = 0;
    CaptureSettingsInfo settingsBeforeRing {}; ///< settings in force at the oldest stored record

    TakeTimeline timeline;

    // Audio chunks: sample s lives in chunk (s / audioChunkSamples) % chunks.size(); allocated lazily
    Chunks chunks;
    AudioRange audioRange {};
    bool audioRunActive = false;               ///< false after audio off: the next frame starts a new run
    int64_t audioFloor = 0;                    ///< nothing before the last clear marker is audio
    std::vector<AudioRange> holes;             ///< zero-filled holes inside audioRange (dropped frames)
    uint64_t missingAudioSamples = 0;
    int64_t endSample = 0;
};

} // namespace vocalsynth
