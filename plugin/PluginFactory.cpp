#include "PluginProcessor.h"

// Called by the JUCE plugin wrappers. JucePlugin_IsSynth comes from the CMake target
// (VocalSynthFX or VocalSynthInstrument), so each product gets its own variant.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new vocalsynth::VocalSynthProcessor (JucePlugin_IsSynth ? vocalsynth::Variant::instrument
                                                                   : vocalsynth::Variant::fx);
}
