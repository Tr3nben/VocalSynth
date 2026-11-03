#include "vocalsynth/synth/SynthVoice.h"

namespace vocalsynth::synth
{

// Scaffold (docs/SYNTH-DESIGN.md 10.3): never active, silent. Agent B implements.

void SynthVoice::prepare (double newSampleRate, uint32_t seed)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    random = seed != 0 ? seed : 1u;

    for (auto& o : osc)
        o.prepare (sampleRate);

    sub.prepare (sampleRate);
    noise.prepare (sampleRate, random);
    filter.prepare (sampleRate);

    for (auto& e : env)
        e.prepare (sampleRate);

    reset();
}

void SynthVoice::reset() noexcept
{
    note = -1;
    velocity = 0.0f;
    randomValue = 0.0f;
    pitch = 0.0f;
    glideStart = 0.0f;
    glideTarget = 0.0f;
    glideProgress = 1.0;
    ampGain = 0.0f;
    ampStep = 0.0f;
    stealFadeSamples = 0;
    hasPendingStart = false;
}

void SynthVoice::setTables (const Wavetable* oscA, const Wavetable* oscB) noexcept
{
    osc[0].setTable (oscA);
    osc[1].setTable (oscB);
}

TablesInUse SynthVoice::getTablesInUse (int oscIndex) const noexcept
{
    return osc[oscIndex == 0 ? 0u : 1u].getTablesInUse();
}

void SynthVoice::apply (const VoiceCommand&, const VoiceContext&, int) noexcept {}

void SynthVoice::controlTick (const VoiceContext&) noexcept {}

void SynthVoice::render (float*, float*, int) noexcept {}

bool SynthVoice::isActive() const noexcept
{
    return false;
}

bool SynthVoice::isReleased() const noexcept
{
    return false;
}

float SynthVoice::getSourceValue (ModSource) const noexcept
{
    return 0.0f;
}

uint32_t SynthVoice::getNanResetCount() const noexcept
{
    return filter.getNanResetCount();
}

} // namespace vocalsynth::synth
