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
      apvts (*this, nullptr, "PARAMS", params::createLayout()),
      virtualMidiPort (v == Variant::instrument ? "VocalSynth Instrument" : "VocalSynth FX")
{
    static_assert (static_cast<int> (TestPattern::all) == 4, "testPattern choice index == TestPattern");

    muteDryParam = apvts.getRawParameterValue (params::muteDry.getParamID());
    testPatternParam = apvts.getRawParameterValue (params::testPattern.getParamID());
    midiChannelParam = apvts.getRawParameterValue (params::midiChannel.getParamID());
    testTempoParam = apvts.getRawParameterValue (params::testTempo.getParamID());
    ccNumberParam = apvts.getRawParameterValue (params::ccNumber.getParamID());
    monitorToneParam = apvts.getRawParameterValue (params::monitorTone.getParamID());
    virtualMidiOutParam = apvts.getRawParameterValue (params::virtualMidiOut.getParamID());

    jassert (muteDryParam != nullptr && testPatternParam != nullptr && midiChannelParam != nullptr
             && testTempoParam != nullptr && ccNumberParam != nullptr && monitorToneParam != nullptr
             && virtualMidiOutParam != nullptr);

    midiOutput.setVirtualPort (&virtualMidiPort);
}

const juce::String VocalSynthProcessor::getName() const
{
    return variant == Variant::instrument ? "VocalSynth Instrument" : "VocalSynth FX";
}

//==============================================================================
void VocalSynthProcessor::prepareToPlay (double sampleRate, int)
{
    // All sample-rate dependent DSP state is (re)initialised here (see SKILL.md, Lifecycle).
    // midiOutput is NOT reset: it must still know which notes the downstream synth holds, so the
    // first processBlock after this can turn them off (flushPending, docs/M1-DESIGN.md 4.4).
    generator.prepare (sampleRate);
    monitorSynth.setOutputEnabled (monitorToneParam->load (std::memory_order_relaxed) >= 0.5f);
    monitorSynth.prepare (sampleRate);
    currentSampleRate.store (sampleRate, std::memory_order_relaxed);
    currentTestNote.store (-1, std::memory_order_relaxed);
    reset();
}

void VocalSynthProcessor::releaseResources()
{
    flushPending.store (true, std::memory_order_relaxed);
}

void VocalSynthProcessor::reset()
{
    // Called by the host from a non-audio thread while processing is suspended: only flag the flush,
    // the next processBlock emits the note-offs and restarts the pattern
    flushPending.store (true, std::memory_order_relaxed);
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

    // No MIDI input is accepted; the buffer is ours to fill
    midi.clear();

    const auto numSamples = buffer.getNumSamples();
    blockSizeHistogram.record (numSamples);
    virtualMidiPort.setEnabled (virtualMidiOutParam->load (std::memory_order_relaxed) >= 0.5f);

    // No MIDI is allowed in a 0-sample block; a pending flush waits for the next real block
    if (numSamples <= 0)
        return;

    const auto settings = getTestPatternSettings();

    // The synth runs in every block (it keeps tracking held notes while muted, docs/M1-DESIGN.md 3.4).
    // Set the switch before anything else touches the synth, so a reset() (prepare/bypass) jumps the
    // output gain to the right target instead of ramping down from the default (an audible blip with
    // Monitor Tone off).
    monitorSynth.setOutputEnabled (monitorToneParam->load (std::memory_order_relaxed) >= 0.5f);

    if (flushPending.exchange (false, std::memory_order_relaxed))
    {
        // Before anything of this block is generated, so the restarted pattern's first note-on at
        // offset 0 comes after these note-offs (docs/M1-DESIGN.md 4.4)
        midiOutput.flushAllNotes (0, midi);
        generator.reset();

        // Release instead of reset: if the tone is audible (Restart pattern), it fades out over
        // 60 ms or is retuned by the restarted pattern's first note-on, without a click.
        // After prepareToPlay the synth is already silent (prepare() resets it).
        monitorSynth.releaseAll();
    }

    const auto numInputs = getTotalNumInputChannels();
    const auto numOutputs = juce::jmin (getTotalNumOutputChannels(), buffer.getNumChannels());

    // Input metering for the UI (and for the M1 sidechain test), before the audio is touched
    float peak = 0.0f;
    for (int ch = 0; ch < juce::jmin (numInputs, buffer.getNumChannels()); ++ch)
        peak = juce::jmax (peak, buffer.getMagnitude (ch, 0, numSamples));

    if (peak > inputPeak.load (std::memory_order_relaxed))
        inputPeak.store (peak, std::memory_order_relaxed);

    for (int ch = numInputs; ch < buffer.getNumChannels(); ++ch)
        buffer.clear (ch, 0, numSamples);

    // FX: the voice passes through unless muted. Instrument: the monitor tone is its only output.
    const bool muteDry = muteDryParam->load (std::memory_order_relaxed) >= 0.5f;

    if (variant == Variant::instrument || muteDry)
        buffer.clear();

    // Output layouts are mono or stereo (isBusesLayoutSupported)
    const auto numToneChannels = juce::jmin (numOutputs, maxToneChannels);
    std::array<float*, maxToneChannels> channels {};

    for (int start = 0; start < numSamples; start += maxChunkSamples)
    {
        const auto length = juce::jmin (maxChunkSamples, numSamples - start);

        events.clear();
        generator.process (length, settings, events);
        midiOutput.write (events, length, midi, start);

        for (int ch = 0; ch < numToneChannels; ++ch)
            channels[(size_t) ch] = buffer.getWritePointer (ch, start);

        monitorSynth.process (events, channels.data(), numToneChannels, length);
    }

    currentTestNote.store (generator.currentNote(), std::memory_order_relaxed);
}

void VocalSynthProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    midi.clear();

    const auto numSamples = buffer.getNumSamples();
    blockSizeHistogram.record (numSamples);
    virtualMidiPort.setEnabled (virtualMidiOutParam->load (std::memory_order_relaxed) >= 0.5f);
    currentTestNote.store (-1, std::memory_order_relaxed);

    if (numSamples >= 1)
    {
        // Nothing may hang while bypassed. Repeated bypassed blocks emit nothing (nothing is active
        // any more), and the pattern restarts from its beginning after un-bypass.
        midiOutput.flushAllNotes (0, midi);
        generator.reset();
        monitorSynth.setOutputEnabled (monitorToneParam->load (std::memory_order_relaxed) >= 0.5f);
        monitorSynth.reset();
        flushPending.store (false, std::memory_order_relaxed);
    }

    // Audio: the FX passes the voice through, the Instrument is silent
    for (int ch = getTotalNumInputChannels(); ch < buffer.getNumChannels(); ++ch)
        buffer.clear (ch, 0, numSamples);

    if (variant == Variant::instrument)
        buffer.clear();
}

//==============================================================================
BlockSizeHistogram::Snapshot VocalSynthProcessor::getBlockSizeHistogram() const noexcept
{
    return blockSizeHistogram.snapshot();
}

void VocalSynthProcessor::resetBlockSizeHistogram() noexcept
{
    blockSizeHistogram.reset();
}

double VocalSynthProcessor::getCurrentSampleRate() const noexcept
{
    return currentSampleRate.load (std::memory_order_relaxed);
}

int VocalSynthProcessor::getCurrentTestNote() const noexcept
{
    return currentTestNote.load (std::memory_order_relaxed);
}

TestPatternSettings VocalSynthProcessor::getTestPatternSettings() const noexcept
{
    TestPatternSettings s;
    s.pattern = static_cast<TestPattern> (
        juce::jlimit (0, 4, juce::roundToInt (testPatternParam->load (std::memory_order_relaxed))));
    s.channel = juce::jlimit (1, 16, juce::roundToInt (midiChannelParam->load (std::memory_order_relaxed)));
    s.bpm = (double) testTempoParam->load (std::memory_order_relaxed);
    s.ccNumber = juce::jlimit (1, 119, juce::roundToInt (ccNumberParam->load (std::memory_order_relaxed)));
    return s;
}

VirtualMidiPort::Status VocalSynthProcessor::getVirtualMidiStatus() const noexcept
{
    return virtualMidiPort.getStatus();
}

uint32_t VocalSynthProcessor::getVirtualMidiOverflowCount() const noexcept
{
    return virtualMidiPort.getOverflowCount();
}

juce::String VocalSynthProcessor::getVirtualMidiDeviceName() const
{
    return virtualMidiPort.getDeviceName();
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
    {
        if (xml->hasTagName (apvts.state.getType()))
        {
            apvts.replaceState (juce::ValueTree::fromXml (*xml));

            // The values changed outside any host automation: tell the host to re-read them.
            // CLAP maps this to params->rescan (CLAP_PARAM_RESCAN_VALUES), which the CLAP spec
            // asks for after a state load (checked by clap-validator's state-reproducibility tests).
            // VST3/AU/Standalone ignore it here because the plugin has no program parameter.
            updateHostDisplay (ChangeDetails().withProgramChanged (true));
        }
    }
}

} // namespace vocalsynth
