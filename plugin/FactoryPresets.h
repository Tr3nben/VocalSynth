#pragma once

#include <array>
#include <span>
#include <string_view>

namespace vocalsynth
{

/** Preset categories (docs/SYNTH-DESIGN.md 14.2). Index == position in presetCategoryNames; stored by NAME in
    .vspreset files. Only append. */
enum class PresetCategory
{
    init = 0,
    lead,
    bass,
    pad,
    pluckKeys,
    vocal,
    fx
};
inline constexpr int numPresetCategories = 7;
inline constexpr std::array<std::string_view, numPresetCategories> presetCategoryNames {
    "Init", "Lead", "Bass", "Pad", "Pluck/Keys", "Vocal", "FX"
};

/** One parameter of a factory preset: synth parameter ID (params::isPresetParameter) and PLAIN value. */
struct FactoryPresetValue
{
    std::string_view id;
    float value = 0.0f;
};

/** A factory preset as data in code (docs/SYNTH-DESIGN.md 14.3): only the values that differ from the
    parameter defaults; every other preset parameter is set to its default when the preset loads. */
struct FactoryPreset
{
    std::string_view name;
    PresetCategory category = PresetCategory::init;
    std::string_view tags;        ///< comma-separated, lower case ("voice,bright")
    std::string_view tableA;      ///< factory table id of Osc A (FactoryWavetables), e.g. "basic-shapes"
    std::string_view tableB;      ///< factory table id of Osc B
    std::span<const FactoryPresetValue> values;
};

/** The factory presets (24 in 0.4.0, index 0 = "Init" = all defaults). Pure data, any thread.
    Owner: agent F. */
class FactoryPresets
{
public:
    static int getNumPresets() noexcept;

    /** index clamped to 0..getNumPresets() - 1. */
    static const FactoryPreset& getPreset (int index) noexcept;
};

} // namespace vocalsynth
