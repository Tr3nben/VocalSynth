#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace vocalsynth
{

// M3.1 take length and Record mode (docs/M3.1-DESIGN.md). Values shared by the core (TakeSegmenter,
// TakeRenderer, CaptureTap), the plugin parameters (plugin/Parameters.h, static_assert-tied) and the UI.
// Every enum value / table index equals the choice index of its parameter: only ever append at the end.

/** Parameter captureMode: how takes are made (docs/M3.1-DESIGN.md 2). */
enum class CaptureMode : uint8_t
{
    background = 0, ///< 0.3.0 behaviour: takes from the transport / free-mode rules, limited by takeMaxBars
    record          ///< fixed-length takes armed with the Record button; no background takes
};

inline constexpr int numCaptureModes = 2;

/** Parameter takeMaxBars: longest background take in bars; 0 = Off (no limit, 0.3.0 behaviour). */
inline constexpr std::array<int, 5> takeMaxBarsChoices { 0, 4, 8, 16, 32 };
inline constexpr int defaultTakeMaxBarsIndex = 3; ///< 16 bars

/** Parameter recordBars: length of a Record-mode take in bars. */
inline constexpr std::array<int, 6> recordBarsChoices { 1, 2, 4, 8, 16, 32 };
inline constexpr int defaultRecordBarsIndex = 3; ///< 8 bars

static_assert (takeMaxBarsChoices[defaultTakeMaxBarsIndex] == 16);
static_assert (recordBarsChoices[defaultRecordBarsIndex] == 8);

/** Bars of a takeMaxBars choice index (clamped into the table; 0 = Off). */
constexpr int takeMaxBarsForIndex (int index) noexcept
{
    const auto i = std::clamp (index, 0, static_cast<int> (takeMaxBarsChoices.size()) - 1);
    return takeMaxBarsChoices[static_cast<std::size_t> (i)];
}

/** Bars of a recordBars choice index (clamped into the table). */
constexpr int recordBarsForIndex (int index) noexcept
{
    const auto i = std::clamp (index, 0, static_cast<int> (recordBarsChoices.size()) - 1);
    return recordBarsChoices[static_cast<std::size_t> (i)];
}

/** Free mode (transport stopped) has no host bar grid: a "bar" is 4 quarter notes at the last known host
    tempo (TakeSegmenter's host bpm, Take::freeBpm), else TakeTimeline::fallbackBpm (120), as the free
    export is written (4/4 at freeBpm). */
inline constexpr double freeModeBeatsPerBar = 4.0;

/** Samples of `bars` free-mode bars: llround (bars * 4 * 60 * sampleRate / bpm). bpm <= 0 or not finite ->
    120, below 1 -> 1; sampleRate <= 0 or not finite -> 0; bars <= 0 -> 0. Pure, real-time safe. */
int64_t freeModeBarsToSamples (int bars, double sampleRate, double bpm) noexcept;

/** Record-mode state for the Capture tab's status line and Record / Cancel button
    (TakeSegmenter::getRecordStatus(), published by the worker every step; docs/M3.1-DESIGN.md 4). */
struct RecordStatus
{
    enum class Phase : uint8_t
    {
        idle,           ///< not armed (or Background mode)
        waitingForPlay, ///< armed while the transport is stopped: starts at the bar line where playback starts
        countIn,        ///< armed while playing: starts at bar startBar ("Starts at bar X - n beats")
        recording       ///< "Recording bar barInTake/bars"
    };

    Phase phase = Phase::idle;
    int bars = 0;                 ///< N of the armed / running recording (from the arm marker)
    int64_t startBar = 0;         ///< countIn / recording: FL bar number X of the first bar; 0 = not known yet
    int barInTake = 0;            ///< recording: 1..bars
    double beatsToStart = 0.0;    ///< countIn: quarter notes from the newest sample to bar X (> 0)
    int takeNumber = 0;           ///< recording: number of the take being recorded
    // No defaulted operator== on purpose: it would compare beatsToStart exactly and trip -Wfloat-equal in
    // the plugin (docs/M3-DESIGN.md 19); the panel compares its status text.
};

} // namespace vocalsynth
