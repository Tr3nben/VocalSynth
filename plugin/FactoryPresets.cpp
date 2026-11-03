#include "FactoryPresets.h"

namespace vocalsynth
{

// Scaffold (docs/SYNTH-DESIGN.md 14.3): only "Init". Agent F adds the 23 sound presets.

namespace
{
constexpr std::array<FactoryPreset, 1> presets { {
    { "Init", PresetCategory::init, "", "basic-shapes", "basic-shapes", {} },
} };
} // namespace

int FactoryPresets::getNumPresets() noexcept
{
    return static_cast<int> (presets.size());
}

const FactoryPreset& FactoryPresets::getPreset (int index) noexcept
{
    const auto i = index < 0 ? 0 : (index >= getNumPresets() ? getNumPresets() - 1 : index);
    return presets[static_cast<std::size_t> (i)];
}

} // namespace vocalsynth
