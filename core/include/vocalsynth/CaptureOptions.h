#pragma once

#include "vocalsynth/VoiceSettings.h"

#include <cstdint>

namespace vocalsynth
{

/** Choice of the captureOffsetMode parameter (choice index == enum value, docs/M3-DESIGN.md 8). */
enum class CaptureOffsetMode : uint8_t
{
    automatic = 0, ///< "Auto": 2 x the maximum block size from prepareToPlay + the fine offset
    manual,        ///< "Manual": captureOffsetMs
    off            ///< "Off": 0
};

inline constexpr int numCaptureOffsetModes = 3;

/** Export version (docs/M3-SPEC.md 7). */
enum class ExportVersion : uint8_t
{
    raw = 0, ///< exactly what was sent, compensated
    clean    ///< ghost notes removed, transition notes merged, octave strays removed, provisional corrections merged
};

inline constexpr int numExportVersions = 2;

/** Quantize grid (synced takes only). Index order is the UI order; only append. */
enum class QuantizeGrid : uint8_t
{
    off = 0,
    quarter,
    eighth,
    eighthTriplet,
    sixteenth,
    sixteenthTriplet,
    thirtySecond
};

inline constexpr int numQuantizeGrids = 7;

/** Pitch-bend export. asPlugin = on iff the take's bend mode is not Off (decision 5). */
enum class BendExport : uint8_t
{
    asPlugin = 0,
    off,
    on
};

inline constexpr int numBendExports = 3;

/** Everything the TakeRenderer needs besides the take (docs/M3-DESIGN.md 9). Defaults = decisions 1-6. */
struct RenderOptions
{
    ExportVersion version = ExportVersion::clean;
    QuantizeGrid quantize = QuantizeGrid::off;
    double quantizeStrengthPercent = 100.0; ///< 0..100: how far note starts move towards the grid
    bool quantizeEnds = false;              ///< note ends to the grid as well (length >= half a grid step)
    BendExport bends = BendExport::asPlugin;
    bool controlChanges = true;             ///< export CCs when present (test pattern)
    int ticksPerQuarterNote = 960;
    double cleanMinNoteMs = 80.0;           ///< Clean ghost threshold (never below the take's minNoteLength)
    double freePreRollMs = 0.0;             ///< free takes: tick 0 = first note-on minus this
    int64_t roundTripSamples = 0;           ///< latency compensation (computeRoundTripSamples), synced takes only

    constexpr bool operator== (const RenderOptions&) const noexcept = default;
};

inline constexpr double minCaptureOffsetMs = 0.0;
inline constexpr double maxCaptureOffsetMs = 300.0;
inline constexpr double minCaptureFineOffsetMs = -100.0;
inline constexpr double maxCaptureFineOffsetMs = 100.0;
inline constexpr double maxCleanMinNoteMs = 500.0;
inline constexpr double maxFreePreRollMs = 1000.0;
inline constexpr int minTicksPerQuarterNote = 24;
inline constexpr int maxTicksPerQuarterNote = 9600;

/** Capture timing offset in samples (docs/M3-DESIGN.md 8): automatic = 2 * maxBlockSize + fine,
    manual = offsetMs, off = 0; ms converted with llround (ms * sampleRate / 1000); clamped to >= 0.
    offsetMs / fineOffsetMs are clamped to their ranges, non-finite values count as 0. */
int64_t computeRoundTripSamples (CaptureOffsetMode mode, double offsetMs, double fineOffsetMs, int maxBlockSize,
                                 double sampleRate) noexcept;

/** The three capture-offset parameters as plain values (captureOffsetMode, captureOffsetMs,
    captureFineOffsetMs). The round trip depends on each take's own maxBlockSize and sample rate, so
    callers keep these settings and compute the samples per take (TakeRenderer::roundTripFor). */
struct CaptureOffsetSettings
{
    CaptureOffsetMode mode = CaptureOffsetMode::automatic;
    double offsetMs = 0.0;     ///< Manual
    double fineOffsetMs = 0.0; ///< added in Auto

    constexpr bool operator== (const CaptureOffsetSettings&) const noexcept = default;
};

int64_t computeRoundTripSamples (const CaptureOffsetSettings& offset, int maxBlockSize, double sampleRate) noexcept;

/** Largest round trip any parameter value can give for a take with this max block and rate:
    max (2 * maxBlockSize + maxCaptureFineOffsetMs, maxCaptureOffsetMs) in samples. The TakeSegmenter keeps
    synced takes open this much (plus detection delays) after their end (docs/M3-DESIGN.md 5.2). */
int64_t maxRoundTripSamples (int maxBlockSize, double sampleRate) noexcept;

/** Grid step in ticks: quarter = ppq, eighth = ppq / 2, eighth triplet = ppq / 3, sixteenth = ppq / 4,
    sixteenth triplet = ppq / 6, thirty-second = ppq / 8 (integer division; ppq 960 divides all); off = 0. */
int64_t quantizeGridTicks (QuantizeGrid grid, int ticksPerQuarterNote) noexcept;

/** File-name tag of a grid: "Q4", "Q8", "Q8T", "Q16", "Q16T", "Q32", "" for off. */
const char* quantizeGridTag (QuantizeGrid grid) noexcept;

/** Whether bends are exported for a take recorded with bendMode. */
constexpr bool bendsEnabled (BendExport bends, BendMode bendMode) noexcept
{
    return bends == BendExport::on || (bends == BendExport::asPlugin && bendMode != BendMode::off);
}

/** Clamps every field to its range (enums to their counts, strength 0..100, ms ranges, ppq range, round
    trip >= 0, non-finite -> default). */
RenderOptions sanitise (const RenderOptions& options) noexcept;

} // namespace vocalsynth
