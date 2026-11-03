#include "vocalsynth/synth/FactoryWavetables.h"

#include <array>

namespace vocalsynth::synth
{

namespace
{
// Ids and names are final (persisted, docs/SYNTH-DESIGN.md 5.5); frame counts may be tuned by agent A.
constexpr std::array<FactoryTableInfo, numFactoryTables> infos { {
    { "basic-shapes", "Basic Shapes", 64 },
    { "pwm", "PWM", 64 },
    { "harmonic-sweep", "Harmonic Sweep", 64 },
    { "saw-bright-dark", "Saw Bright-Dark", 32 },
    { "formant-vowel", "Formant Vowels", 64 },
    { "sync-sweep", "Sync Sweep", 64 },
    { "fm-2op", "FM 2-Op", 64 },
    { "fold", "Fold", 64 },
    { "organ", "Organ", 32 },
    { "digital", "Digital", 32 },
    { "spectral-random", "Spectral Random", 32 },
    { "bell", "Bell", 32 },
    { "square-stack", "Square Stack", 32 },
    { "choir", "Choir", 64 },
} };
} // namespace

const FactoryTableInfo& FactoryWavetables::getInfo (FactoryTable table) noexcept
{
    const auto index = static_cast<std::size_t> (table);
    return infos[index < infos.size() ? index : 0];
}

std::optional<FactoryTable> FactoryWavetables::fromId (std::string_view id) noexcept
{
    for (std::size_t i = 0; i < infos.size(); ++i)
        if (infos[i].id == id)
            return static_cast<FactoryTable> (i);

    return std::nullopt;
}

// Scaffold: silent frames; agent A implements the generators.
std::vector<float> FactoryWavetables::generateFrames (FactoryTable table)
{
    return std::vector<float> (static_cast<std::size_t> (getInfo (table).numFrames) * Wavetable::frameSize, 0.0f);
}

WavetableBuildResult FactoryWavetables::build (FactoryTable table, const std::atomic<bool>* cancel)
{
    const auto frames = generateFrames (table);
    return WavetableBuilder::build (std::string (getInfo (table).displayName), frames, Wavetable::frameSize, cancel);
}

} // namespace vocalsynth::synth
