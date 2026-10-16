// CapturePanel, the "Capture" tab (docs/M3-DESIGN.md 14, 16). Owner from the implementation phase on: agent C.
// Planned cases: status texts (synced / free / off, overflow only when non-zero), take rows newest first,
// repaint only on change (<= 10 Hz), drag handles available / preparing, buttons send the tap requests,
// Clear asks first, export options persist in CaptureState, editor page switch incl. the Capture page.

#include "CapturePanel.h"
#include "PluginProcessor.h"

#include <catch2/catch_test_macros.hpp>

using namespace vocalsynth;

TEST_CASE ("CapturePanel: scaffold compiles", "[plugin][capturepanel]")
{
    VocalSynthProcessor processor (Variant::fx);
    CapturePanel panel (processor);
    panel.runTimerTickForTests();
    CHECK (panel.getNumTakeRows() == 0);
}
