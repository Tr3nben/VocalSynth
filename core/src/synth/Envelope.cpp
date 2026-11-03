#include "vocalsynth/synth/Envelope.h"

namespace vocalsynth::synth
{

// Scaffold (docs/SYNTH-DESIGN.md 8.1): stays idle at 0. Agent B implements.

void Envelope::prepare (double newSampleRate) noexcept
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    reset();
}

void Envelope::reset() noexcept
{
    stage = Stage::idle;
    value = 0.0f;
    segmentStart = 0.0f;
    segmentTime = 0.0;
}

void Envelope::noteOn (float startValue) noexcept
{
    segmentStart = startValue;
}

void Envelope::noteOff (float startValue) noexcept
{
    segmentStart = startValue;
}

float Envelope::advance (const EnvelopeSettings&, int) noexcept
{
    return value;
}

} // namespace vocalsynth::synth
