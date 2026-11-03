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
    explicit SynthSettingsBuilder (juce::AudioProcessorValueTreeState& apvts);

    synth::SynthSettings build() const noexcept;

private:
    /** In params::getSynthParameterIds() order; never null after construction. */
    std::array<std::atomic<float>*, params::numSynthParameters> raw {};
};

} // namespace vocalsynth
