#include "SynthSettingsBuilder.h"

namespace vocalsynth
{

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

// Scaffold: the defaults. Agent D implements the copy.
synth::SynthSettings SynthSettingsBuilder::build() const noexcept
{
    return synth::SynthSettings {}.sanitised();
}

synth::SynthSettings SynthSettingsBuilder::fromPlain (const PlainValues&) noexcept
{
    return synth::SynthSettings {}.sanitised();
}

SynthSettingsBuilder::PlainValues SynthSettingsBuilder::toPlain (const synth::SynthSettings&) noexcept
{
    return {};
}

int SynthSettingsBuilder::indexOf (const juce::String& parameterId)
{
    return params::getSynthParameterIds().indexOf (parameterId);
}

} // namespace vocalsynth
