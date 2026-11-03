#include "vocalsynth/synth/ModMatrix.h"

namespace vocalsynth::synth
{

// Scaffold (docs/SYNTH-DESIGN.md 9): no modulation. Agent B implements.

void ModMatrix::compute (const std::array<ModSlotSettings, numModSlots>&, const ModSourceValues&,
                         ModDestValues& out) noexcept
{
    out.fill (0.0f);
}

void ModMatrix::computeGlobal (const std::array<ModSlotSettings, numModSlots>&, const ModSourceValues&,
                               ModDestValues& out) noexcept
{
    out.fill (0.0f);
}

void ModMatrix::computePerVoice (const std::array<ModSlotSettings, numModSlots>&, const ModSourceValues&,
                                 ModDestValues& out) noexcept
{
    out.fill (0.0f);
}

} // namespace vocalsynth::synth
