// FactoryPresets: 24 presets, categories, IDs valid, values in range, >= 8 use voice sources, tables exist.
// Test plan: docs/SYNTH-DESIGN.md 14.3, 21. Owner: agent F (Sound / presets).

#include "FactoryPresets.h"
#include "PluginProcessor.h"
#include "PresetManager.h"
#include "SynthParameters.h"
#include "SynthSettingsBuilder.h"
#include "WavetableStore.h"
#include "WavetableWorker.h"
#include "vocalsynth/synth/FactoryWavetables.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <set>
#include <string>

using vocalsynth::FactoryPreset;
using vocalsynth::FactoryPresets;
using vocalsynth::PresetCategory;
using vocalsynth::SynthSettingsBuilder;
namespace params = vocalsynth::params;
namespace sy = vocalsynth::synth;

namespace
{
juce::String toString (std::string_view s)
{
    return juce::String::fromUTF8 (s.data(), static_cast<int> (s.size()));
}

/** The preset's settings as the golden test renders them (defaults overwritten by the preset's values). */
sy::SynthSettings settingsOf (const FactoryPreset& preset)
{
    auto plain = SynthSettingsBuilder::toPlain (sy::SynthSettings {});

    for (const auto& v : preset.values)
    {
        const auto index = SynthSettingsBuilder::indexOf (toString (v.id));
        REQUIRE (index >= 0);
        plain[static_cast<std::size_t> (index)] = v.value;
    }

    return SynthSettingsBuilder::fromPlain (plain);
}

bool isVoiceSource (sy::ModSource s)
{
    return s == sy::ModSource::voiceLevel || s == sy::ModSource::voiceBrightness || s == sy::ModSource::voiceNoisiness
           || s == sy::ModSource::voicePitch;
}

bool usesVoiceSource (const sy::SynthSettings& s)
{
    for (const auto& slot : s.mod)
        if (isVoiceSource (slot.source) && slot.dest != sy::ModDest::off && slot.amountPercent != 0.0f)
            return true;

    return false;
}
} // namespace

TEST_CASE ("FactoryPresets: 24 presets, index 0 is Init, out-of-range indices clamp", "[plugin][synth][factorypresets]")
{
    REQUIRE (FactoryPresets::getNumPresets() == 24);
    CHECK (FactoryPresets::getPreset (0).name == "Init");
    CHECK (FactoryPresets::getPreset (-5).name == "Init");
    CHECK (&FactoryPresets::getPreset (1000) == &FactoryPresets::getPreset (23));
}

TEST_CASE ("FactoryPresets: names are unique, tags are lower-case lists", "[plugin][synth][factorypresets]")
{
    std::set<std::string> names;

    for (int i = 0; i < FactoryPresets::getNumPresets(); ++i)
    {
        const auto& p = FactoryPresets::getPreset (i);
        CAPTURE (p.name);
        CHECK (! p.name.empty());
        CHECK (p.name.size() <= 20); // fits the preset bar
        CHECK (names.insert (std::string (p.name)).second);
        CHECK (juce::File::createLegalFileName (toString (p.name)) == toString (p.name)); // saved as a file name

        const auto tags = toString (p.tags);
        CHECK (tags == tags.toLowerCase());
        CHECK (! tags.containsChar (' '));
        CHECK (! tags.startsWithChar (','));
        CHECK (! tags.endsWithChar (','));
        CHECK (! tags.contains (",,"));
    }
}

TEST_CASE ("FactoryPresets: categories follow the roadmap split", "[plugin][synth][factorypresets]")
{
    // Init 1, Lead 6, Bass 5, Pad 3, Pluck/Keys 3, Vocal 4, FX 2 (ROADMAP 3.2, docs/SYNTH-DESIGN.md 14.3)
    constexpr std::array<int, vocalsynth::numPresetCategories> expected { 1, 6, 5, 3, 3, 4, 2 };
    std::array<int, vocalsynth::numPresetCategories> counts {};
    int previous = 0;

    for (int i = 0; i < FactoryPresets::getNumPresets(); ++i)
    {
        const auto& p = FactoryPresets::getPreset (i);
        const auto c = static_cast<int> (p.category);
        CAPTURE (p.name, c);
        REQUIRE (c >= 0);
        REQUIRE (c < vocalsynth::numPresetCategories);
        CHECK (c >= previous); // grouped by category, in category order (the preset menu)
        previous = c;
        ++counts[static_cast<std::size_t> (c)];
    }

    CHECK (counts == expected);
    CHECK (FactoryPresets::getPreset (0).category == PresetCategory::init);
}

TEST_CASE ("FactoryPresets: tables are factory tables", "[plugin][synth][factorypresets]")
{
    for (int i = 0; i < FactoryPresets::getNumPresets(); ++i)
    {
        const auto& p = FactoryPresets::getPreset (i);
        CAPTURE (p.name, p.tableA, p.tableB);
        CHECK (sy::FactoryWavetables::fromId (p.tableA).has_value());
        CHECK (sy::FactoryWavetables::fromId (p.tableB).has_value());
    }
}

TEST_CASE ("FactoryPresets: values are preset parameters, in range, integral where needed, never the default",
           "[plugin][synth][factorypresets]")
{
    vocalsynth::VocalSynthProcessor processor { vocalsynth::Variant::fx };
    const auto defaults = SynthSettingsBuilder::toPlain (sy::SynthSettings {});

    for (int i = 0; i < FactoryPresets::getNumPresets(); ++i)
    {
        const auto& p = FactoryPresets::getPreset (i);
        std::set<std::string> seen;
        CAPTURE (p.name);

        for (const auto& v : p.values)
        {
            const auto id = toString (v.id);
            CAPTURE (id, v.value);

            // The preset subset rule (14.4): never the mix parameters, only synth parameters
            CHECK (id != params::synthOn.getParamID());
            CHECK (id != params::synthVolume.getParamID());
            CHECK (id != params::dryLevel.getParamID());
            REQUIRE (params::isPresetParameter (id));
            CHECK (seen.insert (std::string (v.id)).second);

            auto* param = processor.getParameters().getParameter (id);
            REQUIRE (param != nullptr);
            const auto& range = param->getNormalisableRange();
            CHECK (std::isfinite (v.value));
            CHECK (v.value >= range.start);
            CHECK (v.value <= range.end);

            const bool discrete = dynamic_cast<juce::AudioParameterInt*> (param) != nullptr
                                  || dynamic_cast<juce::AudioParameterChoice*> (param) != nullptr
                                  || dynamic_cast<juce::AudioParameterBool*> (param) != nullptr;

            if (discrete)
                CHECK (v.value == std::round (v.value));

            // The value survives the host's normalised round trip (what PresetManager::load does)
            CHECK (std::abs (param->convertFrom0to1 (param->convertTo0to1 (v.value)) - v.value)
                   <= 1.0e-4f * std::max (1.0f, std::abs (v.value)));

            // Only values that differ from the default are listed (14.3)
            const auto index = SynthSettingsBuilder::indexOf (id);
            REQUIRE (index >= 0);
            CHECK (v.value != defaults[static_cast<std::size_t> (index)]);
        }
    }
}

TEST_CASE ("FactoryPresets: Init is exactly the parameter defaults", "[plugin][synth][factorypresets]")
{
    const auto& init = FactoryPresets::getPreset (0);
    const auto defaultTable = std::string (sy::FactoryWavetables::getInfo (sy::defaultFactoryTable).id);

    CHECK (init.name == "Init");
    CHECK (init.values.empty());
    CHECK (init.tableA == defaultTable);
    CHECK (init.tableB == defaultTable);
    CHECK (settingsOf (init) == sy::SynthSettings {}.sanitised());
    CHECK (settingsOf (init) == sy::SynthSettings {});
}

TEST_CASE ("FactoryPresets: play modes, voice sources and the sound-design rules", "[plugin][synth][factorypresets]")
{
    int withVoiceSources = 0;

    for (int i = 1; i < FactoryPresets::getNumPresets(); ++i)
    {
        const auto& p = FactoryPresets::getPreset (i);
        const auto s = settingsOf (p);
        CAPTURE (p.name);

        // E6: Legato is the default for the monophonic voice; Poly for pads and keys
        if (p.category == PresetCategory::pad || p.category == PresetCategory::pluckKeys)
            CHECK (s.playMode == sy::PlayMode::poly);
        else if (p.category != PresetCategory::fx)
            CHECK (s.playMode != sy::PlayMode::poly);

        // The product's point: every sound reacts to the voice
        if (usesVoiceSource (s))
            ++withVoiceSources;
        else
            FAIL_CHECK ("no voice source");

        // Every used mod slot is complete (source, destination, amount)
        for (const auto& slot : s.mod)
            if (slot.source != sy::ModSource::off || slot.dest != sy::ModDest::off || slot.amountPercent != 0.0f)
            {
                CHECK (slot.source != sy::ModSource::off);
                CHECK (slot.dest != sy::ModDest::off);
                CHECK (slot.amountPercent != 0.0f);
            }

        // Unison above 2 voices only with random start phases (docs/SYNTH-DESIGN.md 6.1)
        for (const auto& o : s.osc)
            if (o.on && o.unison > 2)
                CHECK (o.phaseRandPercent >= 50.0f);

        // Something sounds: an oscillator with level, the sub or the noise
        const bool audible = (s.osc[0].on && s.osc[0].levelPercent > 0.0f) || (s.osc[1].on && s.osc[1].levelPercent > 0.0f)
                             || s.subLevelPercent > 0.0f;
        CHECK (audible);
    }

    CHECK (withVoiceSources >= 8); // docs/SYNTH-DESIGN.md 14.3
}

TEST_CASE ("FactoryPresets: PresetManager loads every factory preset with its tables", "[plugin][synth][factorypresets]")
{
    vocalsynth::VocalSynthProcessor processor { vocalsynth::Variant::fx };
    vocalsynth::WavetableStore store;
    vocalsynth::WavetableWorker worker { store };
    vocalsynth::PresetManager presets { processor.getParameters(), store, worker };
    SynthSettingsBuilder builder { processor.getParameters() };
    const auto synthIds = params::getSynthParameterIds();

    for (int i = FactoryPresets::getNumPresets() - 1; i >= 0; --i) // Init last: from a non-default state
    {
        const auto& p = FactoryPresets::getPreset (i);
        CAPTURE (p.name);
        REQUIRE (presets.loadFactory (i));
        CHECK (presets.getCurrentName() == toString (p.name));
        CHECK_FALSE (presets.isModified());

        const auto expected = SynthSettingsBuilder::toPlain (settingsOf (p));
        const auto loaded = SynthSettingsBuilder::toPlain (builder.build());

        for (int k = 0; k < synthIds.size(); ++k)
        {
            if (! params::isPresetParameter (synthIds[k]))
                continue;

            const auto e = expected[static_cast<std::size_t> (k)];
            const auto l = loaded[static_cast<std::size_t> (k)];
            CAPTURE (synthIds[k], e, l);
            CHECK (std::abs (e - l) <= 1.0e-3f * std::max (1.0f, std::abs (e)));
        }

        const std::array<std::string_view, 2> tables { p.tableA, p.tableB };

        for (int s = 0; s < 2; ++s)
        {
            const auto slot = store.getSlot (s);
            CHECK (slot.reference.toRefString() == "factory:" + toString (tables[static_cast<std::size_t> (s)]));
            CHECK (slot.table != nullptr);
        }
    }
}
