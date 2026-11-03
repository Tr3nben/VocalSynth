#pragma once

#include "SynthParameters.h"
#include "vocalsynth/synth/SynthSettings.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>

namespace vocalsynth
{

/** APVTS raw atomics -> synth::SynthSettings (docs/SYNTH-DESIGN.md 4, 15), like getVoiceSettings() does for
    the voice parameters. The constructor looks up every synth parameter's raw value pointer once (message
    thread, may allocate); build() is a straight copy of the plain values (choices / ints rounded, bools
    >= 0.5) followed by sanitised(): any thread, no allocation, no lock. bendRangeSemitones stays at its
    default: the processor sets it from the voice settings. Owner: agent D. */
class SynthSettingsBuilder
{
public:
    /** Plain values of every synth parameter in params::getSynthParameterIds() order. */
    using PlainValues = std::array<float, params::numSynthParameters>;

    explicit SynthSettingsBuilder (juce::AudioProcessorValueTreeState& apvts);

    /** fromPlain (the raw atomics' current values). */
    synth::SynthSettings build() const noexcept;

    /** Plain values -> settings (choices / ints rounded, bools >= 0.5, then sanitised()); no APVTS needed:
        golden renders (FactoryPresets values by ID) and tests. Real-time safe. */
    static synth::SynthSettings fromPlain (const PlainValues& plain) noexcept;

    /** The inverse for every synth parameter (fromPlain (toPlain (s)) == s.sanitised()); toPlain
        (SynthSettings {}) = the parameter defaults. */
    static PlainValues toPlain (const synth::SynthSettings& settings) noexcept;

    /** Index of a synth parameter ID in PlainValues, -1 if it is not one. Message thread (string compare). */
    static int indexOf (const juce::String& parameterId);

private:
    /** In params::getSynthParameterIds() order; never null after construction. */
    std::array<std::atomic<float>*, params::numSynthParameters> raw {};
};

} // namespace vocalsynth
