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
    older) are dropped (the segment containing that sample stays), and in any case those older than
    timelineLookbackSeconds before the newest sample: transport records (tempo automation re-syncs up to
    100 per second while playing) never enter the event ring, so an old record that stays in a quiet
    session must not keep the timeline growing (bound: 100 / s * 600 s * ~72 B = ~4.3 MB). Open takes keep
    their own timeline (TakeSegmenter), so eviction never breaks a long take; it only limits how far back
    a grab reaches (a grab reaches at most 120 s or 32 bars, well inside the timeline look-back).

    Clock epochs: a clearing reset (new sample rate) restarts the capture clock at 0 and increments
    getClockEpoch(). Sample positions of an earlier epoch never address the new epoch's audio. The audio
    of the epoch just ended is retained (moved aside, not copied) until releaseRetainedAudio(), so the
    takes that the reset closed can still be refined and get their WAV (readAudio / getAudioRange with
    that epoch); memory meanwhile: the retained chunks plus the new epoch's (lazily allocated) ones.

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
    static constexpr double timelineLookbackSeconds = 600.0;

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

    /** Number of clearing resets since construction (prepare() does not change it). */
    uint32_t getClockEpoch() const noexcept { return clockEpoch; }

    /** getAudioRange() / readAudio() of an epoch: the current one, or the retained previous one (see
        class comment); any other epoch has no audio (empty range, zeros). */
    AudioRange getAudioRange (uint32_t epoch) const noexcept;
    std::size_t readAudio (uint32_t epoch, int64_t fromSample, std::span<float> destination) const noexcept;

    /** Whether the previous epoch's audio is retained, and which epoch it is. */
    bool hasRetainedAudio() const noexcept { return retained.valid; }
    uint32_t getRetainedEpoch() const noexcept { return retained.epoch; }

    /** Frees the retained audio of the previous epoch (the worker: once no take of it needs audio). */
    void releaseRetainedAudio() noexcept;

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
    static std::size_t readStore (const Chunks& source, const AudioRange& range, const std::vector<AudioRange>& gaps,
                                  int64_t fromSample, std::span<float> destination) noexcept;

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

    // Clock epochs (see class comment)
    struct RetainedAudio
    {
        bool valid = false;
        uint32_t epoch = 0;
        Chunks chunks;
        AudioRange range {};
        std::vector<AudioRange> holes;
    };

    uint32_t clockEpoch = 0;
    RetainedAudio retained;
};

} // namespace vocalsynth
