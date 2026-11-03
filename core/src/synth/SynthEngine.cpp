#include "vocalsynth/synth/SynthEngine.h"

#include "vocalsynth/synth/WavetableBuilder.h"

#include <algorithm>

namespace vocalsynth::synth
{

// Scaffold (docs/SYNTH-DESIGN.md 2, 3): prepares every module, outputs silence. Agent C implements.

SynthEngine::SynthEngine() = default;
SynthEngine::~SynthEngine() = default;

void SynthEngine::prepare (double newSampleRate, int)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    fallbackSine = WavetableBuilder::makeSineTable();
    allocator.prepare (sampleRate);

    for (std::size_t i = 0; i < voices.size(); ++i)
        voices[i].prepare (sampleRate, 0x9e3779b9u ^ static_cast<uint32_t> (i + 1));

    for (std::size_t i = 0; i < lfos.size(); ++i)
        lfos[i].prepare (sampleRate, 0x85ebca6bu ^ static_cast<uint32_t> (i + 1));

    chorus.prepare (sampleRate);
    delay.prepare (sampleRate);
    reverb.prepare (sampleRate);
    reset();
}

void SynthEngine::reset() noexcept
{
    samplePosition = 0;
    tables.fill (nullptr);
    allocator.reset();

    for (auto& v : voices)
        v.reset();

    for (auto& l : lfos)
        l.reset();

    chorus.reset();
    delay.reset();
    reverb.reset();
    hasSettings = false;
    bendTarget = 0.0f;
    bendSmoothed = 0.0f;
    expressionValue = {};
    releasePending = false;
}

void SynthEngine::releaseAll() noexcept
{
    releasePending = true;
}

void SynthEngine::setWavetable (int oscIndex, const Wavetable* table) noexcept
{
    if (oscIndex == 0 || oscIndex == 1)
        tables[static_cast<std::size_t> (oscIndex)] = table;
}

TablesInUse SynthEngine::getTablesInUse (int oscIndex) const noexcept
{
    if (oscIndex == 0 || oscIndex == 1)
        return { tables[static_cast<std::size_t> (oscIndex)], nullptr };

    return {};
}

void SynthEngine::process (std::span<const VsEvent>, std::span<const VsEventTiming>, const ExpressionBuffer*,
                           const SynthSettings&, const SynthTransport&, float* left, float* right,
                           int numSamples) noexcept
{
    if (numSamples <= 0)
        return;

    if (left != nullptr)
        std::fill (left, left + numSamples, 0.0f);

    if (right != nullptr)
        std::fill (right, right + numSamples, 0.0f);

    samplePosition += numSamples;
}

SynthEngineStats SynthEngine::getStats() const noexcept
{
    return {};
}

} // namespace vocalsynth::synth
