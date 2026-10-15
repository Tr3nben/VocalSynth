#include "PluginProcessor.h"

#include "Parameters.h"
#include "PluginEditor.h"

#include <cmath>

namespace vocalsynth
{

int16_t monitorBendFor (int16_t bend, int rangeSemitones) noexcept
{
    if (rangeSemitones == static_cast<int> (MonitorSynth::bendRangeSemitones))
        return bend;

    // MonitorSynth reads value / 8192 * 2 semitones; the synth reads value / 8192 * range
    const auto scaled = static_cast<double> (bend) * static_cast<double> (rangeSemitones)
                        / MonitorSynth::bendRangeSemitones;
    return static_cast<int16_t> (juce::jlimit (-8192L, 8191L, std::lround (scaled)));
}

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

    sourceParam = apvts.getRawParameterValue (params::source.getParamID());
    voiceRangeParam = apvts.getRawParameterValue (params::voiceRange.getParamID());
    pitchAlgorithmParam = apvts.getRawParameterValue (params::pitchAlgorithm.getParamID());
    keyParam = apvts.getRawParameterValue (params::key.getParamID());
    scaleParam = apvts.getRawParameterValue (params::scale.getParamID());
    stickinessParam = apvts.getRawParameterValue (params::stickiness.getParamID());
    gateThresholdParam = apvts.getRawParameterValue (params::gateThreshold.getParamID());
    minNoteLengthParam = apvts.getRawParameterValue (params::minNoteLength.getParamID());
    octaveShiftParam = apvts.getRawParameterValue (params::octaveShift.getParamID());
    bendModeParam = apvts.getRawParameterValue (params::bendMode.getParamID());
    bendRangeParam = apvts.getRawParameterValue (params::bendRange.getParamID());
    velocitySensitivityParam = apvts.getRawParameterValue (params::velocitySensitivity.getParamID());
    legatoParam = apvts.getRawParameterValue (params::legato.getParamID());

    jassert (sourceParam != nullptr && voiceRangeParam != nullptr && pitchAlgorithmParam != nullptr
             && keyParam != nullptr && scaleParam != nullptr && stickinessParam != nullptr
             && gateThresholdParam != nullptr && minNoteLengthParam != nullptr && octaveShiftParam != nullptr
             && bendModeParam != nullptr && bendRangeParam != nullptr && velocitySensitivityParam != nullptr
             && legatoParam != nullptr);

    // Never resized: processBlock downmixes chunks of at most maxChunkSamples
    monoBuffer.assign ((size_t) maxChunkSamples, 0.0f);
    lastSource = getSource();

    midiOutput.setVirtualPort (&virtualMidiPort);
}

const juce::String VocalSynthProcessor::getName() const
{
    return variant == Variant::instrument ? "VocalSynth Instrument" : "VocalSynth FX";
}

//==============================================================================
void VocalSynthProcessor::prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock)
{
    // All sample-rate dependent DSP state is (re)initialised here (see SKILL.md, Lifecycle).
    // midiOutput is NOT reset: it must still know which notes the downstream synth holds, so the
    // first processBlock after this can turn them off (flushPending, docs/M1-DESIGN.md 4.4).
    generator.prepare (sampleRate);
    voicePipeline.prepare (sampleRate, maximumExpectedSamplesPerBlock); // allocates (allowed here)
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

    // Settings are plain values built from the raw atomics: no locks, no allocation
    const auto source = getSource();
    const auto settings = getTestPatternSettings();
    const auto voiceSettings = getVoiceSettings();
    const bool voiceMode = source == params::Source::voice;

    // The synth runs in every block (it keeps tracking held notes while muted, docs/M1-DESIGN.md 3.4).
    // Set the switch before anything else touches the synth, so a reset() (prepare/bypass) jumps the
    // output gain to the right target instead of ramping down from the default (an audible blip with
    // Monitor Tone off).
    monitorSynth.setOutputEnabled (monitorToneParam->load (std::memory_order_relaxed) >= 0.5f);

    // A source switch ends whatever the other source left sounding, then both sources restart
    const bool sourceChanged = source != lastSource;
    lastSource = source;

    if (flushPending.exchange (false, std::memory_order_relaxed) || sourceChanged)
    {
        // Before anything of this block is generated, so the restarted pattern's first note-on at
        // offset 0 comes after these note-offs (docs/M1-DESIGN.md 4.4)
        midiOutput.flushAllNotes (0, midi);
        generator.reset();
        voicePipeline.reset(); // silent; the note-offs came from midiOutput

        // Release instead of reset: if the tone is audible (Restart pattern), it fades out over
        // 60 ms or is retuned by the restarted pattern's first note-on, without a click.
        // After prepareToPlay the synth is already silent (prepare() resets it).
        monitorSynth.releaseAll();
    }

    const auto numInputs = getTotalNumInputChannels();
    const auto numOutputs = juce::jmin (getTotalNumOutputChannels(), buffer.getNumChannels());

    const auto numInputChannels = juce::jmin (numInputs, buffer.getNumChannels());

    // Input metering for the UI (and for the M1 sidechain test), before the audio is touched
    float peak = 0.0f;
    for (int ch = 0; ch < numInputChannels; ++ch)
        peak = juce::jmax (peak, buffer.getMagnitude (ch, 0, numSamples));

    if (peak > inputPeak.load (std::memory_order_relaxed))
        inputPeak.store (peak, std::memory_order_relaxed);

    for (int ch = numInputs; ch < buffer.getNumChannels(); ++ch)
        buffer.clear (ch, 0, numSamples);

    // FX: the voice passes through unless muted. Instrument: the monitor tone is its only output.
    // The buffer is cleared per chunk, after the chunk's input has been downmixed for the analysis.
    const bool muteDry = muteDryParam->load (std::memory_order_relaxed) >= 0.5f;
    const bool clearAudio = variant == Variant::instrument || muteDry;

    // Output layouts are mono or stereo (isBusesLayoutSupported)
    const auto numToneChannels = juce::jmin (numOutputs, maxToneChannels);
    std::array<float*, maxToneChannels> channels {};
    const auto monitorBendRange = bendRangeSemitones (voiceSettings.bendRange);

    for (int start = 0; start < numSamples; start += maxChunkSamples)
    {
        const auto length = juce::jmin (maxChunkSamples, numSamples - start);

        events.clear();

        if (voiceMode)
        {
            // FX: main input bus. Instrument: its only input bus is the sidechain (M1, T3).
            const auto* mono = downmixInput (buffer, numInputChannels, start, length);
            voicePipeline.process (mono, length, voiceSettings, events);
        }
        else
        {
            generator.process (length, settings, events);
        }

        if (clearAudio)
            buffer.clear (start, length);

        midiOutput.write (events, length, midi, start);

        for (int ch = 0; ch < numToneChannels; ++ch)
            channels[(size_t) ch] = buffer.getWritePointer (ch, start);

        // The monitor tone follows whatever the source sends. MonitorSynth bends +-2 semitones; the
        // test patterns assume that, the voice bends use the selected Bend Range.
        if (voiceMode && monitorBendRange != (int) MonitorSynth::bendRangeSemitones)
        {
            prepareMonitorEvents (monitorBendRange);
            monitorSynth.process (monitorEvents, channels.data(), numToneChannels, length);
        }
        else
        {
            monitorSynth.process (events, channels.data(), numToneChannels, length);
        }
    }

    currentTestNote.store (voiceMode ? -1 : generator.currentNote(), std::memory_order_relaxed);
}

const float* VocalSynthProcessor::downmixInput (const juce::AudioBuffer<float>& buffer, int numInputChannels,
                                                int start, int length) noexcept
{
    if (numInputChannels <= 0 || length <= 0)
        return nullptr;

    jassert (length <= (int) monoBuffer.size());
    auto* mono = monoBuffer.data();

    juce::FloatVectorOperations::copy (mono, buffer.getReadPointer (0, start), length);

    for (int ch = 1; ch < numInputChannels; ++ch)
        juce::FloatVectorOperations::add (mono, buffer.getReadPointer (ch, start), length);

    if (numInputChannels > 1)
        juce::FloatVectorOperations::multiply (mono, 1.0f / (float) numInputChannels, length);

    return mono;
}

void VocalSynthProcessor::prepareMonitorEvents (int rangeSemitones) noexcept
{
    monitorEvents.clear();

    for (auto e : events)
    {
        if (e.type == VsEventType::pitchBend)
            e.bend = monitorBendFor (e.bend, rangeSemitones);

        monitorEvents.push (e); // same capacity as events: never full
    }
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
        voicePipeline.reset();
        monitorSynth.setOutputEnabled (monitorToneParam->load (std::memory_order_relaxed) >= 0.5f);
        monitorSynth.reset();
        flushPending.store (false, std::memory_order_relaxed);
        lastSource = getSource(); // everything is flushed already
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

VoiceSettings VocalSynthProcessor::getVoiceSettings() const noexcept
{
    const auto index = [] (const std::atomic<float>* param) noexcept
    { return juce::roundToInt (param->load (std::memory_order_relaxed)); };
    const auto value = [] (const std::atomic<float>* param) noexcept
    { return param->load (std::memory_order_relaxed); };

    VoiceSettings s;
    s.range = static_cast<VoiceRange> (index (voiceRangeParam));
    s.algorithm = static_cast<PitchAlgorithm> (index (pitchAlgorithmParam));
    s.key = index (keyParam);
    s.scale = static_cast<Scale> (index (scaleParam));
    s.stickinessPercent = value (stickinessParam);
    s.gateThresholdDb = value (gateThresholdParam);
    s.minNoteLengthMs = value (minNoteLengthParam);
    s.octaveShift = index (octaveShiftParam);
    s.bendMode = static_cast<BendMode> (index (bendModeParam));
    s.bendRange = static_cast<BendRange> (index (bendRangeParam));
    s.velocitySensitivityPercent = value (velocitySensitivityParam);
    s.legato = value (legatoParam) >= 0.5f;
    s.channel = index (midiChannelParam);
    return sanitise (s);
}

params::Source VocalSynthProcessor::getSource() const noexcept
{
    const auto index = juce::roundToInt (sourceParam->load (std::memory_order_relaxed));
    return static_cast<params::Source> (juce::jlimit (0, params::numSources - 1, index));
}

double VocalSynthProcessor::getVoiceLatencyMs() const noexcept
{
    const auto rate = getCurrentSampleRate();
    return VoicePipeline::computeAlgorithmicLatencyMs (rate > 0.0 ? rate : 48000.0, getVoiceSettings().range);
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
