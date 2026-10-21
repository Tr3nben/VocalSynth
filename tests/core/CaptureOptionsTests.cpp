// Capture options: round trip, quantize grid, sanitising (docs/M3-DESIGN.md 8, 9). Owner from the
// implementation phase on: agent B.

#include "vocalsynth/CaptureOptions.h"

#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <string>

using namespace vocalsynth;

TEST_CASE ("Capture round trip: Auto = 2 x max block + fine offset, Manual, Off", "[captureoptions]")
{
    // The user's ASIO buffer: 2048 samples at 48 kHz -> 4096 samples = 85.3 ms
    CHECK (computeRoundTripSamples (CaptureOffsetMode::automatic, 0.0, 0.0, 2048, 48000.0) == 4096);
    CHECK (computeRoundTripSamples (CaptureOffsetMode::automatic, 250.0, 10.0, 2048, 48000.0) == 4096 + 480);
    CHECK (computeRoundTripSamples (CaptureOffsetMode::automatic, 0.0, -100.0, 256, 48000.0) == 0); // clamped >= 0
    // fine offset clamped to -100 ms: 4096 - 4800 < 0 -> 0
    CHECK (computeRoundTripSamples (CaptureOffsetMode::automatic, 0.0, -1000.0, 2048, 48000.0) == 0);
    CHECK (computeRoundTripSamples (CaptureOffsetMode::automatic, 0.0, -50.0, 2048, 48000.0) == 4096 - 2400);
    CHECK (computeRoundTripSamples (CaptureOffsetMode::manual, 85.3, 50.0, 2048, 48000.0) == 4094);
    CHECK (computeRoundTripSamples (CaptureOffsetMode::manual, 1000.0, 0.0, 2048, 48000.0) == 14400); // 300 ms max
    CHECK (computeRoundTripSamples (CaptureOffsetMode::off, 100.0, 50.0, 2048, 48000.0) == 0);
    CHECK (computeRoundTripSamples (CaptureOffsetMode::manual, std::numeric_limits<double>::quiet_NaN(), 0.0, 0, 48000.0) == 0);
    CHECK (computeRoundTripSamples (CaptureOffsetMode::automatic, 0.0, 0.0, 2048, 0.0) == 0);
}

TEST_CASE ("Quantize grid ticks and tags", "[captureoptions]")
{
    CHECK (quantizeGridTicks (QuantizeGrid::off, 960) == 0);
    CHECK (quantizeGridTicks (QuantizeGrid::quarter, 960) == 960);
    CHECK (quantizeGridTicks (QuantizeGrid::eighth, 960) == 480);
    CHECK (quantizeGridTicks (QuantizeGrid::eighthTriplet, 960) == 320);
    CHECK (quantizeGridTicks (QuantizeGrid::sixteenth, 960) == 240);
    CHECK (quantizeGridTicks (QuantizeGrid::sixteenthTriplet, 960) == 160);
    CHECK (quantizeGridTicks (QuantizeGrid::thirtySecond, 960) == 120);
    CHECK (quantizeGridTicks (QuantizeGrid::sixteenthTriplet, 96) == 16);

    CHECK (std::string (quantizeGridTag (QuantizeGrid::sixteenth)) == "Q16");
    CHECK (std::string (quantizeGridTag (QuantizeGrid::eighthTriplet)) == "Q8T");
    CHECK (std::string (quantizeGridTag (QuantizeGrid::off)).empty());
}

TEST_CASE ("Bend export follows the plugin's bend mode by default (decision 5)", "[captureoptions]")
{
    STATIC_REQUIRE_FALSE (bendsEnabled (BendExport::asPlugin, BendMode::off));
    STATIC_REQUIRE (bendsEnabled (BendExport::asPlugin, BendMode::trueBend));
    STATIC_REQUIRE (bendsEnabled (BendExport::on, BendMode::off));
    STATIC_REQUIRE_FALSE (bendsEnabled (BendExport::off, BendMode::anchored));
}

TEST_CASE ("RenderOptions defaults = decisions, sanitise clamps", "[captureoptions]")
{
    const RenderOptions defaults;
    CHECK (defaults.version == ExportVersion::clean);
    CHECK (defaults.quantize == QuantizeGrid::off);
    CHECK (defaults.bends == BendExport::asPlugin);
    CHECK (defaults.ticksPerQuarterNote == 960);
    CHECK (sanitise (defaults) == defaults);

    RenderOptions bad;
    bad.version = static_cast<ExportVersion> (7);
    bad.quantize = static_cast<QuantizeGrid> (99);
    bad.quantizeStrengthPercent = std::numeric_limits<double>::quiet_NaN();
    bad.ticksPerQuarterNote = 1;
    bad.cleanMinNoteMs = -5.0;
    bad.roundTripSamples = -10;

    const auto s = sanitise (bad);
    CHECK (s.version == ExportVersion::clean);
    CHECK (s.quantize == QuantizeGrid::thirtySecond);
    CHECK (s.quantizeStrengthPercent == 100.0);
    CHECK (s.ticksPerQuarterNote == minTicksPerQuarterNote);
    CHECK (s.cleanMinNoteMs == 0.0);
    CHECK (s.roundTripSamples == 0);
}

TEST_CASE ("Capture round trip from CaptureOffsetSettings and its upper bound", "[captureoptions]")
{
    CHECK (computeRoundTripSamples (CaptureOffsetSettings {}, 2048, 48000.0) == 4096);
    CHECK (computeRoundTripSamples (CaptureOffsetSettings { CaptureOffsetMode::manual, 85.3, 50.0 }, 2048, 48000.0)
           == 4094);
    CHECK (computeRoundTripSamples (CaptureOffsetSettings { CaptureOffsetMode::off, 85.3, 50.0 }, 2048, 48000.0)
           == 0);

    // max (2 x 2048 + 100 ms, 300 ms) at 48 kHz = max (8896, 14400)
    CHECK (maxRoundTripSamples (2048, 48000.0) == 14400);
    // 2 x 8192 + 4410 = 20794 > 300 ms at 44.1 kHz (13230)
    CHECK (maxRoundTripSamples (8192, 44100.0) == 20794);
    CHECK (maxRoundTripSamples (2048, 0.0) == 0);
}
