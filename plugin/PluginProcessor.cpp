#include "PluginProcessor.h"

#include "Parameters.h"
#include "PluginEditor.h"

namespace vocalsynth
{

VocalSynthProcessor::BusesProperties VocalSynthProcessor::makeBusesProperties (Variant v)
{
    const auto stereo = juce::AudioChannelSet::stereo();

    if (v == Variant::instrument)
        return BusesProperties().withInput ("Sidechain", stereo, true).withOutput ("Output", stereo, true);

    return BusesProperties().withInput ("Input", stereo, true).withOutput ("Output", stereo, true);
}

VocalSynthProcessor::VocalSynthProcessor (Variant v)
    : AudioProcessor (makeBusesProperties (v)),
      variant (v),
      apvts (*this, nullptr, "PARAMS", params::createLayout())
{
    muteDryParam = apvts.getRawParameterValue (params::muteDry.getParamID());
    jassert (muteDryParam != nullptr);
}

const juce::String VocalSynthProcessor::getName() const
{
    return variant == Variant::instrument ? "VocalSynth Instrument" : "VocalSynth FX";
}

//==============================================================================
void VocalSynthProcessor::prepareToPlay (double, int)
{
    // All sample-rate dependent DSP state will be (re)allocated here (see SKILL.md, Lifecycle).
    reset();
}

void VocalSynthProcessor::releaseResources() {}

void VocalSynthProcessor::reset()
{
    inputPeak.store (0.0f, std::memory_order_relaxed);
}

bool VocalSynthProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto isMonoOrStereo = [] (const juce::AudioChannelSet& set)
    { return set == juce::AudioChannelSet::mono() || set == juce::AudioChannelSet::stereo(); };

    const auto& out = layouts.getMainOutputChannelSet();
    const auto& in = layouts.getMainInputChannelSet();

    if (! isMonoOrStereo (out))
        return false;

    if (variant == Variant::instrument)
        return in.isDisabled() || isMonoOrStereo (in);

    // FX: the mic signal passes through, so input and output must match
    return in == out;
}

//==============================================================================
void VocalSynthProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    // No MIDI input is accepted; the buffer is ours to fill (nothing yet in M0)
    midi.clear();

    const auto numSamples = buffer.getNumSamples();
    const auto numInputs = getTotalNumInputChannels();
    const auto numOutputs = getTotalNumOutputChannels();

    // Input metering for the UI (and for the M1 sidechain test)
    float peak = 0.0f;
    for (int ch = 0; ch < numInputs; ++ch)
        peak = juce::jmax (peak, buffer.getMagnitude (ch, 0, numSamples));

    if (peak > inputPeak.load (std::memory_order_relaxed))
        inputPeak.store (peak, std::memory_order_relaxed);

    for (int ch = numInputs; ch < numOutputs; ++ch)
        buffer.clear (ch, 0, numSamples);

    // The instrument has no sound engine yet (M6); the FX passes the voice through unless muted
    const bool muteDry = muteDryParam->load (std::memory_order_relaxed) >= 0.5f;

    if (variant == Variant::instrument || muteDry)
        buffer.clear();
}

void VocalSynthProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    // Later: flush note-offs for all active notes here so nothing hangs while bypassed
    midi.clear();

    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    if (variant == Variant::instrument)
        buffer.clear();
}

//==============================================================================
juce::AudioProcessorEditor* VocalSynthProcessor::createEditor()
{
    return new VocalSynthEditor (*this);
}

void VocalSynthProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void VocalSynthProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

} // namespace vocalsynth
