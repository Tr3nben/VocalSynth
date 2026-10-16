#include "vocalsynth/CaptureOptions.h"

#include <algorithm>
#include <cmath>

namespace vocalsynth
{

namespace
{
double clampFinite (double value, double lo, double hi, double fallback) noexcept
{
    return std::isfinite (value) ? std::clamp (value, lo, hi) : fallback;
}

template <typename Enum>
Enum clampEnum (Enum value, int count) noexcept
{
    return static_cast<Enum> (std::clamp (static_cast<int> (value), 0, count - 1));
}
} // namespace

int64_t computeRoundTripSamples (CaptureOffsetMode mode, double offsetMs, double fineOffsetMs, int maxBlockSize,
                                 double sampleRate) noexcept
{
    if (! (sampleRate > 0.0))
        return 0;

    const auto msToSamples = [sampleRate] (double ms) noexcept
    { return static_cast<int64_t> (std::llround (ms * sampleRate / 1000.0)); };

    int64_t samples = 0;

    switch (mode)
    {
        case CaptureOffsetMode::automatic:
            samples = 2 * static_cast<int64_t> (std::max (0, maxBlockSize))
                      + msToSamples (clampFinite (fineOffsetMs, minCaptureFineOffsetMs, maxCaptureFineOffsetMs, 0.0));
            break;

        case CaptureOffsetMode::manual:
            samples = msToSamples (clampFinite (offsetMs, minCaptureOffsetMs, maxCaptureOffsetMs, 0.0));
            break;

        case CaptureOffsetMode::off:
            break;
    }

    return std::max (int64_t { 0 }, samples);
}

int64_t quantizeGridTicks (QuantizeGrid grid, int ticksPerQuarterNote) noexcept
{
    const auto ppq = static_cast<int64_t> (std::max (1, ticksPerQuarterNote));

    switch (grid)
    {
        case QuantizeGrid::off: return 0;
        case QuantizeGrid::quarter: return ppq;
        case QuantizeGrid::eighth: return ppq / 2;
        case QuantizeGrid::eighthTriplet: return ppq / 3;
        case QuantizeGrid::sixteenth: return ppq / 4;
        case QuantizeGrid::sixteenthTriplet: return ppq / 6;
        case QuantizeGrid::thirtySecond: return ppq / 8;
    }

    return 0;
}

const char* quantizeGridTag (QuantizeGrid grid) noexcept
{
    switch (grid)
    {
        case QuantizeGrid::off: return "";
        case QuantizeGrid::quarter: return "Q4";
        case QuantizeGrid::eighth: return "Q8";
        case QuantizeGrid::eighthTriplet: return "Q8T";
        case QuantizeGrid::sixteenth: return "Q16";
        case QuantizeGrid::sixteenthTriplet: return "Q16T";
        case QuantizeGrid::thirtySecond: return "Q32";
    }

    return "";
}

RenderOptions sanitise (const RenderOptions& options) noexcept
{
    const RenderOptions defaults;
    auto o = options;
    o.version = clampEnum (o.version, numExportVersions);
    o.quantize = clampEnum (o.quantize, numQuantizeGrids);
    o.bends = clampEnum (o.bends, numBendExports);
    o.quantizeStrengthPercent = clampFinite (o.quantizeStrengthPercent, 0.0, 100.0, defaults.quantizeStrengthPercent);
    o.ticksPerQuarterNote = std::clamp (o.ticksPerQuarterNote, minTicksPerQuarterNote, maxTicksPerQuarterNote);
    o.cleanMinNoteMs = clampFinite (o.cleanMinNoteMs, 0.0, maxCleanMinNoteMs, defaults.cleanMinNoteMs);
    o.freePreRollMs = clampFinite (o.freePreRollMs, 0.0, maxFreePreRollMs, defaults.freePreRollMs);
    o.roundTripSamples = std::max (int64_t { 0 }, o.roundTripSamples);
    return o;
}

} // namespace vocalsynth
