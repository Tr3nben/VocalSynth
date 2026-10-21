// M3.1 take length and Record mode (docs/M3.1-DESIGN.md): choice tables, free-mode bar length. Owner from the
// implementation phase on: the CAPTURE agent (add the TakeSegmenter limit / Record cases to
// TakeSegmenterTests.cpp or here, tag [capturemode]).

#include "vocalsynth/CaptureMode.h"

#include <catch2/catch_test_macros.hpp>

#include <limits>

using namespace vocalsynth;

TEST_CASE ("Capture mode choices: tables, defaults and clamped lookups", "[capturemode]")
{
    CHECK (numCaptureModes == 2);
    CHECK (static_cast<int> (CaptureMode::background) == 0);
    CHECK (static_cast<int> (CaptureMode::record) == 1);

    CHECK (takeMaxBarsForIndex (0) == 0); // Off
    CHECK (takeMaxBarsForIndex (1) == 4);
    CHECK (takeMaxBarsForIndex (defaultTakeMaxBarsIndex) == 16);
    CHECK (takeMaxBarsForIndex (4) == 32);
    CHECK (takeMaxBarsForIndex (-1) == 0);
    CHECK (takeMaxBarsForIndex (99) == 32);

    CHECK (recordBarsForIndex (0) == 1);
    CHECK (recordBarsForIndex (defaultRecordBarsIndex) == 8);
    CHECK (recordBarsForIndex (5) == 32);
    CHECK (recordBarsForIndex (-7) == 1);
    CHECK (recordBarsForIndex (6) == 32);
}

TEST_CASE ("Free-mode bars: 4 quarter notes at the host tempo, else 120 BPM", "[capturemode]")
{
    // 16 bars at 120 BPM, 48 kHz: 16 x 2 s = 32 s
    CHECK (freeModeBarsToSamples (16, 48000.0, 120.0) == 32 * 48000);
    CHECK (freeModeBarsToSamples (1, 44100.0, 90.0) == 117600); // 8/3 s
    CHECK (freeModeBarsToSamples (3, 48000.0, 97.5) == 354462); // 7.3846 s, rounded
    CHECK (freeModeBarsToSamples (4, 48000.0, 0.0) == 4 * 2 * 48000);
    CHECK (freeModeBarsToSamples (4, 48000.0, -10.0) == 4 * 2 * 48000);
    CHECK (freeModeBarsToSamples (4, 48000.0, std::numeric_limits<double>::quiet_NaN()) == 4 * 2 * 48000);
    CHECK (freeModeBarsToSamples (1, 48000.0, 1.0e-9) == 4 * 60 * 48000); // clamped to 1 BPM
    CHECK (freeModeBarsToSamples (0, 48000.0, 120.0) == 0);                // Off
    CHECK (freeModeBarsToSamples (-2, 48000.0, 120.0) == 0);
    CHECK (freeModeBarsToSamples (8, 0.0, 120.0) == 0);
    CHECK (freeModeBarsToSamples (8, std::numeric_limits<double>::infinity(), 120.0) == 0);
}

TEST_CASE ("RecordStatus defaults to idle", "[capturemode]")
{
    const RecordStatus status;
    CHECK (status.phase == RecordStatus::Phase::idle);
    CHECK (status.bars == 0);
    CHECK (status.startBar == 0);
    CHECK (status.barInTake == 0);
    CHECK (status.takeNumber == 0);
}
