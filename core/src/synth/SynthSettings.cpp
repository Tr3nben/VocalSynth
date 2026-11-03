#include "vocalsynth/synth/SynthSettings.h"

namespace vocalsynth::synth
{

// Scaffold (docs/SYNTH-DESIGN.md 4): returns the values unchanged. Agent B implements the clamping.

SynthSettings SynthSettings::sanitised() const noexcept
{
    return *this;
}

} // namespace vocalsynth::synth
