// CaptureExporter and ExportFiles (docs/M3-DESIGN.md 12, 16). Owner from the implementation phase on: agent C.
// Planned cases: .mid readable by juce::MidiFile (PPQ, tempo / time-signature meta at tick 0, RPN bend range
// when bends are on, end-of-track rule, pairing invariants), byte-identical files for identical takes, WAV
// readable by juce::WavAudioFormat (24-bit / float, length = audio span, sample 0 = tick 0), file names,
// temp + move with a locked target (numbered sibling), persistWav (move, copy fallback, never overwrites),
// heartbeat + stale-folder cleanup (24 h, keeps the own folder), deleteInstanceFolder.

#include "CaptureExporter.h"
#include "ExportFiles.h"

#include <catch2/catch_test_macros.hpp>

using namespace vocalsynth;

TEST_CASE ("ExportFiles: writeBytesAtomically writes the bytes", "[plugin][captureexporter]")
{
    const juce::TemporaryFile temp (".bin");
    const char bytes[] = { 'V', 'S', '3' };
    const auto file = ExportFiles::writeBytesAtomically (temp.getFile(), bytes, sizeof (bytes));
    REQUIRE (file == temp.getFile());
    CHECK (file.getSize() == 3);
}

TEST_CASE ("CaptureExporter: folders", "[plugin][captureexporter]")
{
    CHECK (CaptureExporter::getCaptureRoot().isDirectory());
    CHECK (CaptureExporter::getInstanceFolder ("abc").getParentDirectory() == CaptureExporter::getCaptureRoot());
    CHECK (CaptureExporter::getDefaultTakesFolder().getFileName() == "Takes");
    CHECK (CaptureExporter::makeInstanceId().length() == 16);
}
