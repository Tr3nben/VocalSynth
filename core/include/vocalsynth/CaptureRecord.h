#pragma once

#include "vocalsynth/CaptureMode.h"
#include "vocalsynth/EventTiming.h"
#include "vocalsynth/VoiceSettings.h"
#include "vocalsynth/VsEvent.h"

#include <cstdint>
#include <type_traits>

namespace vocalsynth
{

/** Host transport of one block, filled by the plugin from juce::AudioPlayHead::PositionInfo
    (docs/M3-DESIGN.md 4). Plain value, no JUCE. A host without a playhead gives the default value
    (nothing valid, not playing). */
struct TransportInfo
{
    bool isPlaying = false;
    bool hasPpq = false;
    double ppqPosition = 0.0;            ///< quarter notes since the song start at the block's first sample
    bool hasBpm = false;
    double bpm = 120.0;
    bool hasTimeSignature = false;
    int timeSigNumerator = 4;
    int timeSigDenominator = 4;
    bool hasBarStart = false;
    double ppqPositionOfLastBarStart = 0.0;
    bool isLooping = false;              ///< informational only (loop wraps are detected from ppq jumps)

    constexpr bool operator== (const TransportInfo&) const noexcept = default;
};

/** Plugin settings that give recorded events their meaning, recorded on change (docs/M3-DESIGN.md 4). */
struct CaptureSettingsInfo
{
    BendMode bendMode = BendMode::off;
    int bendRangeSemitones = 2;
    int channel = 1;
    float minNoteLengthMs = 60.0f;
    bool testPattern = false;            ///< source = Test Pattern (no detection delay)
    /** M3.1 (docs/M3.1-DESIGN.md 4.2): how takes are made; in the stream so a mode change is timed by the
        audio clock. */
    CaptureMode captureMode = CaptureMode::background;
    /** M3.1: longest background take in bars, 0 = Off. The plain default stays "no limit" (0.3.0 behaviour);
        the plugin sends the parameter (default 16). Travels as one byte (clamped to 0..255). */
    int takeMaxBars = 0;

    constexpr bool operator== (const CaptureSettingsInfo&) const noexcept = default;
};

/** Requests from the UI and lifecycle points, timed by the audio clock (docs/M3-DESIGN.md 4). */
enum class CaptureMarkerKind : uint8_t
{
    newTake,     ///< "New take": closes the current take at the marker, the next one starts there
    grab,        ///< "Grab last ...": grabBars (synced) / grabSeconds (free) in the marker
    clear,       ///< "Clear": drops the history and every take before the marker
    captureOn,   ///< captureEnabled switched on
    captureOff,  ///< captureEnabled switched off (the tap sends note-offs for open notes first)
    audioOn,     ///< captureAudio switched on (audio frames follow)
    audioOff,    ///< captureAudio switched off (no audio frames until audioOn)
    bypassOn,    ///< host bypass started (audio of bypassed blocks is captured as silence)
    bypassOff,
    recordArm,   ///< M3.1 Record button: grabBars = N bars to record (docs/M3.1-DESIGN.md 4)
    recordCancel ///< M3.1 Cancel: disarms, or discards the Record take being recorded
};

enum class CaptureRecordType : uint8_t
{
    reset,       ///< first record after prepare(): sample rate, max block, whether the history is cleared
    transport,   ///< host transport changed (TransportInfo)
    settings,    ///< CaptureSettingsInfo changed
    event,       ///< one VsEvent as sent to the MIDI output (+ VsEventTiming)
    audio,       ///< one audio frame of numSamples mono samples is now in the audio ring
    marker,      ///< CaptureMarkerKind
    gap          ///< records and/or audio were dropped (ring full) since the previous gap record
};

struct ResetRecord
{
    double sampleRate = 48000.0;
    int maxBlockSize = 0;
    bool clearsHistory = true;  ///< first prepare or a new sample rate (the clock restarted at 0)
};

struct EventRecord
{
    VsEvent event;             ///< sampleOffset is 0 (the record's sample is the absolute emit sample)
    VsEventTiming timing;
};

struct MarkerRecord
{
    CaptureMarkerKind kind = CaptureMarkerKind::newTake;
    int grabBars = 8;          ///< grab; recordArm: the bars to record
    double grabSeconds = 30.0; ///< grab only
};

struct GapRecord
{
    uint64_t droppedRecords = 0;      ///< since the previous gap record
    uint64_t droppedAudioSamples = 0; ///< since the previous gap record
};

/** One entry of the CaptureTap record ring: 32 bytes, trivially copyable, plain packed words.
    Use the make... factories and as... accessors (implemented in CaptureRecord.cpp) instead of the raw
    words; the packing is private to CaptureRecord.cpp (docs/M3-DESIGN.md 4.2 lists it).
    `sample` is the absolute sample on the CaptureTap clock (host rate, since the last clearing reset).
    Lossy fields: TransportInfo::ppqPositionOfLastBarStart travels as a float offset from ppqPosition
    (error < 1e-5 beats for bars up to 64 quarter notes), time-signature values are clamped to 1..255,
    channel to 1..16. Everything else round-trips exactly. */
struct CaptureRecord
{
    int64_t sample = 0;
    uint64_t word0 = 0;
    uint64_t word1 = 0;
    uint32_t word2 = 0;
    CaptureRecordType type = CaptureRecordType::gap;
    uint8_t byte0 = 0;
    uint8_t byte1 = 0;
    uint8_t byte2 = 0;

    static CaptureRecord makeReset (int64_t sample, const ResetRecord& r) noexcept; ///< sample 0 when it clears history
    static CaptureRecord makeTransport (int64_t sample, const TransportInfo& info) noexcept;
    static CaptureRecord makeSettings (int64_t sample, const CaptureSettingsInfo& info) noexcept;
    static CaptureRecord makeEvent (int64_t sample, const VsEvent& event, const VsEventTiming& timing) noexcept;
    static CaptureRecord makeAudio (int64_t sample, int numSamples) noexcept;
    static CaptureRecord makeMarker (int64_t sample, const MarkerRecord& marker) noexcept;
    static CaptureRecord makeGap (int64_t sample, const GapRecord& gap) noexcept;

    /** Valid only for the matching type (else a default value). */
    ResetRecord asReset() const noexcept;
    TransportInfo asTransport() const noexcept;
    CaptureSettingsInfo asSettings() const noexcept;
    EventRecord asEvent() const noexcept;
    int asAudioNumSamples() const noexcept;
    MarkerRecord asMarker() const noexcept;
    GapRecord asGap() const noexcept;

    /** The true time estimate of an event record: sample - timing.delaySamples. */
    int64_t estimatedSample() const noexcept;

    constexpr bool operator== (const CaptureRecord&) const noexcept = default;
};

static_assert (sizeof (CaptureRecord) == 32);
static_assert (std::is_trivially_copyable_v<CaptureRecord>);

/** Encoding of a TransportInfo as the tap stores it (the decoded value of makeTransport): the tap compares
    these, so a lossy field cannot cause a record per block. */
TransportInfo quantiseTransport (const TransportInfo& info) noexcept;

} // namespace vocalsynth
