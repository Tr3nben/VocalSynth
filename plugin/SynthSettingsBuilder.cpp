#include "SynthSettingsBuilder.h"

#include <cmath>
#include <type_traits>

namespace vocalsynth
{

namespace
{

/** Calls visit (index, field) for every synth parameter's SynthSettings field, index = position in
    params::getSynthParameterIds() (docs/SYNTH-DESIGN.md 15). The ONE place that ties the parameter order to
    the struct: fromPlain and toPlain both go through it. Settings is SynthSettings or const SynthSettings. */
template <typename Settings, typename Visitor>
void visitFields (Settings& s, Visitor&& visit) noexcept
{
    int i = 0;

    // Global / mix
    visit (i++, s.synthOn);
    visit (i++, s.synthVolumeDb);
    visit (i++, s.dryLevelDb);

    // Play mode
    visit (i++, s.playMode);
    visit (i++, s.glideTimeMs);
    visit (i++, s.glideMode);
    visit (i++, s.ampVelocityPercent);
    visit (i++, s.ampGainDb);

    // Oscillators
    for (auto& o : s.osc)
    {
        visit (i++, o.on);
        visit (i++, o.positionPercent);
        visit (i++, o.octave);
        visit (i++, o.semitones);
        visit (i++, o.fineCents);
        visit (i++, o.levelPercent);
        visit (i++, o.panPercent);
        visit (i++, o.unison);
        visit (i++, o.detuneCents);
        visit (i++, o.spreadPercent);
        visit (i++, o.phaseRandPercent);
    }

    // Sub / noise
    visit (i++, s.subLevelPercent);
    visit (i++, s.subOctave);
    visit (i++, s.subShape);
    visit (i++, s.noiseLevelPercent);
    visit (i++, s.noiseColorPercent);

    // Filter
    visit (i++, s.filterOn);
    visit (i++, s.filterMode);
    visit (i++, s.filterCutoffHz);
    visit (i++, s.filterResonancePercent);
    visit (i++, s.filterDriveDb);
    visit (i++, s.filterKeytrackPercent);
    visit (i++, s.filterEnvAmountPercent);

    // Envelopes
    for (auto& e : s.env)
    {
        visit (i++, e.attackMs);
        visit (i++, e.decayMs);
        visit (i++, e.sustainPercent);
        visit (i++, e.releaseMs);
    }

    // LFOs
    for (auto& l : s.lfo)
    {
        visit (i++, l.shape);
        visit (i++, l.rateHz);
        visit (i++, l.sync);
        visit (i++, l.retrigger);
        visit (i++, l.fadeMs);
    }

    // Mod matrix
    for (auto& m : s.mod)
    {
        visit (i++, m.source);
        visit (i++, m.dest);
        visit (i++, m.amountPercent);
    }

    // FX
    visit (i++, s.chorus.on);
    visit (i++, s.chorus.rateHz);
    visit (i++, s.chorus.depthPercent);
    visit (i++, s.chorus.mixPercent);
    visit (i++, s.delay.on);
    visit (i++, s.delay.timeMs);
    visit (i++, s.delay.sync);
    visit (i++, s.delay.feedbackPercent);
    visit (i++, s.delay.pingPong);
    visit (i++, s.delay.mixPercent);
    visit (i++, s.reverb.on);
    visit (i++, s.reverb.sizePercent);
    visit (i++, s.reverb.dampingPercent);
    visit (i++, s.reverb.mixPercent);

    jassert (i == params::numSynthParameters);
}

/** Rounds a finite plain value to an int without overflow (sanitised() clamps to the real range). */
int roundedInt (float v) noexcept
{
    constexpr float bound = 1.0e6f;
    const auto clamped = v < -bound ? -bound : (v > bound ? bound : v);
    return static_cast<int> (std::lround (clamped));
}

/** One plain value into its field; a non-finite value leaves the field at its default. */
template <typename Field>
void assignPlain (Field& field, float v) noexcept
{
    if (! std::isfinite (v))
        return;

    if constexpr (std::is_same_v<Field, bool>)
        field = v >= 0.5f;
    else if constexpr (std::is_same_v<Field, float>)
        field = v;
    else if constexpr (std::is_same_v<Field, int>)
        field = roundedInt (v);
    else
    {
        static_assert (std::is_enum_v<Field>);
        field = static_cast<Field> (roundedInt (v)); // scoped enum (fixed underlying int); sanitised() checks
    }
}

template <typename Field>
float toPlainValue (const Field& field) noexcept
{
    if constexpr (std::is_same_v<Field, bool>)
        return field ? 1.0f : 0.0f;
    else if constexpr (std::is_same_v<Field, float>)
        return field;
    else if constexpr (std::is_same_v<Field, int>)
        return static_cast<float> (field);
    else
    {
        static_assert (std::is_enum_v<Field>);
        return static_cast<float> (static_cast<int> (field));
    }
}

} // namespace

SynthSettingsBuilder::SynthSettingsBuilder (juce::AudioProcessorValueTreeState& apvts)
{
    const auto ids = params::getSynthParameterIds();
    jassert (ids.size() == params::numSynthParameters);

    for (int i = 0; i < ids.size() && i < params::numSynthParameters; ++i)
    {
        raw[static_cast<std::size_t> (i)] = apvts.getRawParameterValue (ids[i]);
        jassert (raw[static_cast<std::size_t> (i)] != nullptr);
    }
}

synth::SynthSettings SynthSettingsBuilder::build() const noexcept
{
    // A missing pointer (layout without the synth parameters: impossible after construction, jassert above)
    // keeps that parameter's default: NaN means "no value" to fromPlain.
    PlainValues plain;

    for (std::size_t i = 0; i < raw.size(); ++i)
        plain[i] = raw[i] != nullptr ? raw[i]->load (std::memory_order_relaxed) : std::nanf ("");

    return fromPlain (plain);
}

synth::SynthSettings SynthSettingsBuilder::fromPlain (const PlainValues& plain) noexcept
{
    synth::SynthSettings s;
    visitFields (s, [&plain] (int i, auto& field) { assignPlain (field, plain[static_cast<std::size_t> (i)]); });
    return s.sanitised();
}

SynthSettingsBuilder::PlainValues SynthSettingsBuilder::toPlain (const synth::SynthSettings& settings) noexcept
{
    PlainValues plain {};
    visitFields (settings, [&plain] (int i, const auto& field) { plain[static_cast<std::size_t> (i)] = toPlainValue (field); });
    return plain;
}

int SynthSettingsBuilder::indexOf (const juce::String& parameterId)
{
    static const juce::StringArray ids = params::getSynthParameterIds();
    return ids.indexOf (parameterId);
}

} // namespace vocalsynth
