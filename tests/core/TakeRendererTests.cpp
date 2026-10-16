// TakeRenderer (docs/M3-DESIGN.md 16). Owner from the implementation phase on: agent B.
// Planned cases: exact ticks at 60 / 97.5 / 120 / 174 BPM x PPQ 96 / 480 / 960, tempo change, 3/4 and 7/8 origins, origin after a loop wrap, free mode at host bpm and 120 fallback, round trip (Auto, max block 2048 at 48 kHz = 85.3 ms), quantize grids incl. triplets x strength 0/50/100 x ends, pairing invariants, Clean ghost / transition / octave stray / correction merge, Raw unchanged, bends on/off/asPlugin + RPN range, CCs, endTick rules, audio span, refineNoteOnsets.

#include "vocalsynth/TakeRenderer.h"

#include <catch2/catch_test_macros.hpp>

using namespace vocalsynth;

TEST_CASE ("TakeRenderer: scaffold compiles", "[takerenderer]")
{
    const auto rendered = TakeRenderer::render (Take {}, RenderOptions {});
    CHECK (rendered.ticksPerQuarterNote == 960);
}
