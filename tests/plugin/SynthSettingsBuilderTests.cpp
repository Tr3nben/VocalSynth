// SynthSettingsBuilder: every parameter reaches its field (plain values, choices, bools).
// Test plan: docs/SYNTH-DESIGN.md 21. Owner: agent D (Plugin data).

#include "PluginProcessor.h"
#include "SynthParameters.h"
#include "SynthSettingsBuilder.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <random>
#include <type_traits>

using vocalsynth::SynthSettingsBuilder;
using vocalsynth::Variant;
using vocalsynth::VocalSynthProcessor;
namespace synth = vocalsynth::synth;
namespace params = vocalsynth::params;

namespace
{
template <typename T>
float asFloat (T v)
{
    if constexpr (std::is_same_v<T, bool>)
        return v ? 1.0f : 0.0f;
    else if constexpr (std::is_enum_v<T>)
        return static_cast<float> (static_cast<int> (v));
    else
        return static_cast<float> (v);
}

using Getter = std::function<float (const synth::SynthSettings&)>;

/** The field of every synth parameter, written by NAME (independent of the builder's visiting order). */
std::map<juce::String, Getter> fieldsById()
{
    std::map<juce::String, Getter> f;
#define VS_FIELD(id, member) f[id] = [] (const synth::SynthSettings& s) { return asFloat (s.member); }
    VS_FIELD ("synthOn", synthOn);
    VS_FIELD ("synthVolume", synthVolumeDb);
    VS_FIELD ("dryLevel", dryLevelDb);
    VS_FIELD ("playMode", playMode);
    VS_FIELD ("glideTime", glideTimeMs);
    VS_FIELD ("glideMode", glideMode);
    VS_FIELD ("ampVelocity", ampVelocityPercent);
    VS_FIELD ("ampGain", ampGainDb);
    VS_FIELD ("subLevel", subLevelPercent);
    VS_FIELD ("subOctave", subOctave);
    VS_FIELD ("subShape", subShape);
    VS_FIELD ("noiseLevel", noiseLevelPercent);
    VS_FIELD ("noiseColor", noiseColorPercent);
    VS_FIELD ("filterOn", filterOn);
    VS_FIELD ("filterMode", filterMode);
    VS_FIELD ("filterCutoff", filterCutoffHz);
    VS_FIELD ("filterResonance", filterResonancePercent);
    VS_FIELD ("filterDrive", filterDriveDb);
    VS_FIELD ("filterKeytrack", filterKeytrackPercent);
    VS_FIELD ("filterEnvAmount", filterEnvAmountPercent);
    VS_FIELD ("chorusOn", chorus.on);
    VS_FIELD ("chorusRate", chorus.rateHz);
    VS_FIELD ("chorusDepth", chorus.depthPercent);
    VS_FIELD ("chorusMix", chorus.mixPercent);
    VS_FIELD ("delayOn", delay.on);
    VS_FIELD ("delayTime", delay.timeMs);
    VS_FIELD ("delaySync", delay.sync);
    VS_FIELD ("delayFeedback", delay.feedbackPercent);
    VS_FIELD ("delayPingPong", delay.pingPong);
    VS_FIELD ("delayMix", delay.mixPercent);
    VS_FIELD ("reverbOn", reverb.on);
    VS_FIELD ("reverbSize", reverb.sizePercent);
    VS_FIELD ("reverbDamping", reverb.dampingPercent);
    VS_FIELD ("reverbMix", reverb.mixPercent);
#undef VS_FIELD

    for (std::size_t i = 0; i < 2; ++i)
    {
        const juce::String p = i == 0 ? "oscA" : "oscB";
        f[p + "On"] = [i] (const synth::SynthSettings& s) { return asFloat (s.osc[i].on); };
        f[p + "Position"] = [i] (const synth::SynthSettings& s) { return asFloat (s.osc[i].positionPercent); };
        f[p + "Octave"] = [i] (const synth::SynthSettings& s) { return asFloat (s.osc[i].octave); };
        f[p + "Semi"] = [i] (const synth::SynthSettings& s) { return asFloat (s.osc[i].semitones); };
        f[p + "Fine"] = [i] (const synth::SynthSettings& s) { return asFloat (s.osc[i].fineCents); };
        f[p + "Level"] = [i] (const synth::SynthSettings& s) { return asFloat (s.osc[i].levelPercent); };
        f[p + "Pan"] = [i] (const synth::SynthSettings& s) { return asFloat (s.osc[i].panPercent); };
        f[p + "Unison"] = [i] (const synth::SynthSettings& s) { return asFloat (s.osc[i].unison); };
        f[p + "Detune"] = [i] (const synth::SynthSettings& s) { return asFloat (s.osc[i].detuneCents); };
        f[p + "Spread"] = [i] (const synth::SynthSettings& s) { return asFloat (s.osc[i].spreadPercent); };
        f[p + "PhaseRand"] = [i] (const synth::SynthSettings& s) { return asFloat (s.osc[i].phaseRandPercent); };
    }

    for (std::size_t i = 0; i < 3; ++i)
    {
        const auto p = "env" + juce::String (static_cast<int> (i) + 1);
        f[p + "Attack"] = [i] (const synth::SynthSettings& s) { return asFloat (s.env[i].attackMs); };
        f[p + "Decay"] = [i] (const synth::SynthSettings& s) { return asFloat (s.env[i].decayMs); };
        f[p + "Sustain"] = [i] (const synth::SynthSettings& s) { return asFloat (s.env[i].sustainPercent); };
        f[p + "Release"] = [i] (const synth::SynthSettings& s) { return asFloat (s.env[i].releaseMs); };
    }

    for (std::size_t i = 0; i < 2; ++i)
    {
        const auto p = "lfo" + juce::String (static_cast<int> (i) + 1);
        f[p + "Shape"] = [i] (const synth::SynthSettings& s) { return asFloat (s.lfo[i].shape); };
        f[p + "Rate"] = [i] (const synth::SynthSettings& s) { return asFloat (s.lfo[i].rateHz); };
        f[p + "Sync"] = [i] (const synth::SynthSettings& s) { return asFloat (s.lfo[i].sync); };
        f[p + "Retrig"] = [i] (const synth::SynthSettings& s) { return asFloat (s.lfo[i].retrigger); };
        f[p + "Fade"] = [i] (const synth::SynthSettings& s) { return asFloat (s.lfo[i].fadeMs); };
    }

    for (std::size_t i = 0; i < 8; ++i)
    {
        const auto p = "mod" + juce::String (static_cast<int> (i) + 1);
        f[p + "Source"] = [i] (const synth::SynthSettings& s) { return asFloat (s.mod[i].source); };
        f[p + "Dest"] = [i] (const synth::SynthSettings& s) { return asFloat (s.mod[i].dest); };
        f[p + "Amount"] = [i] (const synth::SynthSettings& s) { return asFloat (s.mod[i].amountPercent); };
    }

    return f;
}

juce::RangedAudioParameter& getParameter (VocalSynthProcessor& p, const juce::String& id)
{
    auto* param = p.getParameters().getParameter (id);
    REQUIRE (param != nullptr);
    return *param;
}

/** A plain value different from the parameter's default (any parameter type). */
float nonDefaultNormalised (juce::RangedAudioParameter& param)
{
    const auto def = param.getDefaultValue();
    const auto steps = param.getNumSteps();

    if (steps > 1 && steps < 1000)
    {
        // Discrete: one step away from the default
        const auto step = 1.0f / static_cast<float> (steps - 1);
        return def + step <= 1.0f + 1.0e-6f ? def + step : def - step;
    }

    return def < 0.5f ? 0.8f : 0.2f;
}

/** Field by field within 1e-5 relative (a skewed / logarithmic range does not give its default back bit-exactly
    through the normalised value the APVTS starts from). */
bool nearlyEqual (float a, float b)
{
    return std::abs (a - b) <= 1.0e-5f * std::max (1.0f, std::abs (b));
}

bool allFieldsNear (const std::map<juce::String, Getter>& fields, const synth::SynthSettings& a,
                    const synth::SynthSettings& b, const juce::String& except = {})
{
    for (const auto& [id, getter] : fields)
    {
        if (id != except && ! nearlyEqual (getter (a), getter (b)))
        {
            UNSCOPED_INFO ("field " << id << ": " << getter (a) << " != " << getter (b));
            return false;
        }
    }

    return true;
}

/** Bit-identical in every field (the plugin code is built with -Wfloat-equal: no defaulted ==). */
bool sameSettings (const synth::SynthSettings& a, const synth::SynthSettings& b)
{
    static const auto fields = fieldsById();

    for (const auto& [id, getter] : fields)
    {
        if (std::bit_cast<uint32_t> (getter (a)) != std::bit_cast<uint32_t> (getter (b)))
        {
            UNSCOPED_INFO ("field " << id << ": " << getter (a) << " != " << getter (b));
            return false;
        }
    }

    return a.bendRangeSemitones == b.bendRangeSemitones;
}

/** Random plain values inside each parameter's range (choices / ints / bools exact). */
SynthSettingsBuilder::PlainValues randomPlain (VocalSynthProcessor& p, std::mt19937& rng)
{
    SynthSettingsBuilder::PlainValues plain {};
    const auto ids = params::getSynthParameterIds();
    std::uniform_real_distribution<float> unit (0.0f, 1.0f);

    for (int i = 0; i < ids.size(); ++i)
    {
        auto& param = getParameter (p, ids[i]);
        plain[static_cast<std::size_t> (i)] = param.convertFrom0to1 (param.convertTo0to1 (param.convertFrom0to1 (unit (rng))));
    }

    return plain;
}
} // namespace

TEST_CASE ("SynthSettingsBuilder: every synth parameter reaches its own field", "[plugin][synth][settings]")
{
    VocalSynthProcessor p (Variant::fx);
    const SynthSettingsBuilder builder (p.getParameters());
    const auto fields = fieldsById();
    const auto ids = params::getSynthParameterIds();
    const synth::SynthSettings defaults {};

    REQUIRE (fields.size() == static_cast<std::size_t> (params::numSynthParameters));
    REQUIRE (ids.size() == params::numSynthParameters);

    for (const auto& id : ids)
        REQUIRE (fields.count (id) == 1);

    // A new instance plays the synth (synthOn on = its parameter default, decision E4), while the core
    // SynthSettings {} keeps synthOn off; every other field is at its default. synthOn off for the comparison.
    CHECK (getParameter (p, params::synthOn.getParamID()).getValue() == 1.0f);
    getParameter (p, params::synthOn.getParamID()).setValueNotifyingHost (0.0f);

    CHECK (allFieldsNear (fields, builder.build(), defaults));

    for (const auto& id : ids)
    {
        CAPTURE (id);
        auto& param = getParameter (p, id);
        // synthOn: the parameter default is on, the core default off (docs/SYNTH-DESIGN.md 24): "non-default" and
        // "back" are relative to the core SynthSettings {} here
        const bool isSynthOn = id == params::synthOn.getParamID();
        param.setValueNotifyingHost (isSynthOn ? 1.0f : nonDefaultNormalised (param));
        const auto expected = param.convertFrom0to1 (param.getValue());
        const auto s = builder.build();

        CHECK (fields.at (id) (s) == Catch::Approx (expected).epsilon (1.0e-5).margin (1.0e-4));
        CHECK (fields.at (id) (s) != Catch::Approx (fields.at (id) (defaults)).margin (1.0e-6));

        // ... and nothing else moved
        CHECK (allFieldsNear (fields, s, defaults, id));

        param.setValueNotifyingHost (isSynthOn ? 0.0f : param.getDefaultValue());
    }

    CHECK (allFieldsNear (fields, builder.build(), defaults));
}

TEST_CASE ("SynthSettingsBuilder: toPlain of the defaults is the layout's defaults; indexOf", "[plugin][synth][settings]")
{
    VocalSynthProcessor p (Variant::instrument);
    const auto ids = params::getSynthParameterIds();
    const auto plain = SynthSettingsBuilder::toPlain (synth::SynthSettings {});

    for (int i = 0; i < ids.size(); ++i)
    {
        CAPTURE (ids[i]);
        auto& param = getParameter (p, ids[i]);
        // synthOn: the parameter default is on (new instances, E4), the core default off (docs/SYNTH-DESIGN.md 24)
        if (ids[i] == params::synthOn.getParamID())
            CHECK (plain[static_cast<std::size_t> (i)] == 0.0f);
        else
            CHECK (plain[static_cast<std::size_t> (i)] == Catch::Approx (param.convertFrom0to1 (param.getDefaultValue())).epsilon (1.0e-5).margin (1.0e-4));

        CHECK (SynthSettingsBuilder::indexOf (ids[i]) == i);
    }

    CHECK (SynthSettingsBuilder::indexOf ("gateThreshold") == -1);
    CHECK (SynthSettingsBuilder::indexOf ("") == -1);
    CHECK (SynthSettingsBuilder::indexOf ("SynthOn") == -1);
    CHECK (sameSettings (SynthSettingsBuilder::fromPlain (plain), synth::SynthSettings {}));
}

TEST_CASE ("SynthSettingsBuilder: fromPlain (toPlain (s)) == s.sanitised()", "[plugin][synth][settings]")
{
    VocalSynthProcessor p (Variant::fx);
    std::mt19937 rng (20261105);

    for (int n = 0; n < 200; ++n)
    {
        // Valid settings: exact round trip
        const auto s = SynthSettingsBuilder::fromPlain (randomPlain (p, rng));
        CHECK (sameSettings (SynthSettingsBuilder::fromPlain (SynthSettingsBuilder::toPlain (s)), s));
        CHECK (sameSettings (s, s.sanitised()));
    }

    // Garbage in every kind of field
    synth::SynthSettings bad;
    bad.synthVolumeDb = std::numeric_limits<float>::quiet_NaN();
    bad.filterCutoffHz = 1.0e9f;
    bad.osc[0].unison = 99;
    bad.osc[1].octave = -40;
    bad.playMode = static_cast<synth::PlayMode> (7);
    bad.mod[3].dest = static_cast<synth::ModDest> (-2);
    bad.lfo[1].rateHz = -std::numeric_limits<float>::infinity();
    bad.env[2].releaseMs = 1.0e7f;
    bad.delay.sync = static_cast<synth::TempoDivision> (15);

    CHECK (sameSettings (SynthSettingsBuilder::fromPlain (SynthSettingsBuilder::toPlain (bad)), bad.sanitised()));
}

TEST_CASE ("SynthSettingsBuilder: fromPlain rounds choices and ints, thresholds bools, rejects non-finite values", "[plugin][synth][settings]")
{
    using P = SynthSettingsBuilder;
    const auto at = [] (const char* id) { return static_cast<std::size_t> (P::indexOf (id)); };
    auto plain = P::toPlain ({});

    plain[at ("playMode")] = 1.6f;            // -> Poly (2)
    plain[at ("oscAUnison")] = 3.4f;          // -> 3
    plain[at ("oscBSemi")] = -2.5f;           // -> -3 (lround: away from zero)
    plain[at ("oscBOn")] = 0.5f;              // -> on
    plain[at ("filterOn")] = 0.49f;           // -> off
    plain[at ("filterCutoff")] = std::numeric_limits<float>::quiet_NaN(); // -> default
    plain[at ("mod2Source")] = 1.0e20f;       // out of every range -> default
    plain[at ("lfo1Shape")] = 6.0f;           // Smooth Random
    plain[at ("ampGain")] = 99.0f;            // clamped to +12 dB

    const auto s = P::fromPlain (plain);
    CHECK (s.playMode == synth::PlayMode::poly);
    CHECK (s.osc[0].unison == 3);
    CHECK (s.osc[1].semitones == -3);
    CHECK (s.osc[1].on);
    CHECK_FALSE (s.filterOn);
    CHECK (juce::exactlyEqual (s.filterCutoffHz, synth::SynthSettings {}.filterCutoffHz));
    CHECK (s.mod[1].source == synth::ModSource::off);
    CHECK (s.lfo[0].shape == synth::LfoShape::smoothRandom);
    CHECK (s.ampGainDb == 12.0f);
    CHECK (s.bendRangeSemitones == 2);
}

TEST_CASE ("SynthSettingsBuilder: build follows automation of the raw values", "[plugin][synth][settings]")
{
    VocalSynthProcessor p (Variant::fx);
    const SynthSettingsBuilder builder (p.getParameters());
    auto& cutoff = getParameter (p, params::filterCutoff.getParamID());
    auto& mode = getParameter (p, params::filterMode.getParamID());

    cutoff.setValueNotifyingHost (cutoff.convertTo0to1 (440.0f));
    mode.setValueNotifyingHost (mode.convertTo0to1 (3.0f));

    const auto s = builder.build();
    CHECK (s.filterCutoffHz == Catch::Approx (440.0f).epsilon (1.0e-4));
    CHECK (s.filterMode == synth::FilterMode::hp);
}
