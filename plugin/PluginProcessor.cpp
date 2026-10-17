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

//==============================================================================
void DspLoadMeter::prepare (double newSampleRate) noexcept
{
    sampleRate = newSampleRate;
    average = 0.0;
    peak = 0.0;
    resetPending.store (true, std::memory_order_relaxed);
    publishedAverage.store (0.0f, std::memory_order_relaxed);
    publishedPeak.store (0.0f, std::memory_order_relaxed);
    publishedValid.store (false, std::memory_order_relaxed);
}

void DspLoadMeter::addBlock (double processingSeconds, int numSamples) noexcept
{
    if (numSamples <= 0 || sampleRate <= 0.0 || ! (processingSeconds >= 0.0))
        return;

    const auto blockSeconds = static_cast<double> (numSamples) / sampleRate;
    const auto load = processingSeconds / blockSeconds;

    if (resetPending.exchange (false, std::memory_order_relaxed))
    {
        average = load;
        peak = load;
    }
    else
    {
        average += (load - average) * (1.0 - std::exp (-blockSeconds / averageTimeConstantSeconds));
        peak = juce::jmax (load, peak * std::exp (-blockSeconds / peakDecayTimeConstantSeconds));
    }

    publishedAverage.store (static_cast<float> (average), std::memory_order_relaxed);
    publishedPeak.store (static_cast<float> (peak), std::memory_order_relaxed);
    publishedValid.store (true, std::memory_order_relaxed);
}

DspLoadMeter::Reading DspLoadMeter::getReading() const noexcept
{
    Reading r;
    r.valid = publishedValid.load (std::memory_order_relaxed) && ! resetPending.load (std::memory_order_relaxed);

    if (r.valid)
    {
        r.averageProportion = publishedAverage.load (std::memory_order_relaxed);
        r.peakProportion = publishedPeak.load (std::memory_order_relaxed);
    }

    return r;
}

void RecentBlockSize::prepare (double sampleRate) noexcept
{
    windowSamples = juce::jmax ((int64_t) 1, (int64_t) std::llround ((sampleRate > 0.0 ? sampleRate : 48000.0) * windowSeconds));
    samplesInWindow = 0;
    runningMax.store (0, std::memory_order_relaxed);
    previousMax.store (0, std::memory_order_relaxed);
}

void RecentBlockSize::record (int numSamples) noexcept
{
    if (numSamples <= 0)
        return;

    if (numSamples > runningMax.load (std::memory_order_relaxed)) // single writer: no CAS needed
        runningMax.store (numSamples, std::memory_order_relaxed);

    samplesInWindow += numSamples;

    if (samplesInWindow >= windowSamples)
    {
        previousMax.store (runningMax.load (std::memory_order_relaxed), std::memory_order_relaxed);
        runningMax.store (0, std::memory_order_relaxed);
        samplesInWindow = 0;
    }
}

int RecentBlockSize::get() const noexcept
{
    return juce::jmax (previousMax.load (std::memory_order_relaxed), runningMax.load (std::memory_order_relaxed));
}

namespace
{
/** Measures the enclosing processBlock for the DspLoadMeter (high-resolution ticks: QueryPerformanceCounter /
    clock_gettime, no allocation, no lock). */
class ScopedLoadMeasurement
{
public:
    ScopedLoadMeasurement (DspLoadMeter& m, int samples, double secondsPerHighResTick) noexcept
        : meter (m),
          numSamples (samples),
          secondsPerTick (secondsPerHighResTick),
          startTicks (juce::Time::getHighResolutionTicks())
    {
    }

    ~ScopedLoadMeasurement()
    {
        const auto elapsed = juce::Time::getHighResolutionTicks() - startTicks;
        meter.addBlock (static_cast<double> (elapsed) * secondsPerTick, numSamples);
    }

    ScopedLoadMeasurement (const ScopedLoadMeasurement&) = delete;
    ScopedLoadMeasurement& operator= (const ScopedLoadMeasurement&) = delete;

private:
    DspLoadMeter& meter;
    const int numSamples;
    const double secondsPerTick;
    const juce::int64 startTicks;
};

/** Brings a saved state up to the current parameter layout before it is loaded. replaceState() keeps
    the CURRENT value of every parameter the state does not mention (JUCE flushes the live values into
    the missing children), so without this the result of loading an older state would depend on what
    the instance was set to before. Missing parameters get their defaults instead.
    M1 states (0.1.x) have no "source": M1 always played its test pattern, so such a state selects
    Test Pattern when its pattern is not Off (the routing test the user saved keeps playing), else
    Voice (the M2 default). */
void migrateState (juce::ValueTree& state, const juce::Array<juce::AudioProcessorParameter*>& parameters)
{
    static const juce::Identifier paramType ("PARAM"), idProperty ("id"), valueProperty ("value");

    const auto childFor = [&state] (const juce::String& id) { return state.getChildWithProperty (idProperty, id); };
    const auto sourceMissing = ! childFor (params::source.getParamID()).isValid();
    const auto savedPattern = childFor (params::testPattern.getParamID());
    const auto patternIndex = savedPattern.isValid() ? juce::roundToInt ((double) savedPattern.getProperty (valueProperty, 0.0))
                                                     : 0;

    for (auto* parameter : parameters)
    {
        auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (parameter);

        if (ranged == nullptr || childFor (ranged->paramID).isValid())
            continue;

        auto child = juce::ValueTree (paramType);
        child.setProperty (idProperty, ranged->paramID, nullptr);
        child.setProperty (valueProperty, ranged->convertFrom0to1 (ranged->getDefaultValue()), nullptr);
        state.appendChild (child, nullptr);
    }

    if (sourceMissing && patternIndex != static_cast<int> (TestPattern::off))
        childFor (params::source.getParamID())
            .setProperty (valueProperty, static_cast<int> (params::Source::testPattern), nullptr);
}
} // namespace

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
      virtualMidiPort (v == Variant::instrument ? "VocalSynth Instrument" : "VocalSynth FX"),
      secondsPerTick (1.0 / static_cast<double> (juce::jmax ((juce::int64) 1, juce::Time::getHighResolutionTicksPerSecond()))),
      instanceId (CaptureExporter::makeInstanceId()),
      captureWorker (captureTap, CaptureExporter::getInstanceFolder (instanceId),
                     CaptureWorkerSources { apvts.getRawParameterValue (params::captureOffsetMode.getParamID()),
                                            apvts.getRawParameterValue (params::captureOffsetMs.getParamID()),
                                            apvts.getRawParameterValue (params::captureFineOffsetMs.getParamID()),
                                            [this] { return recentBlockSize.get(); } })
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

    captureEnabledParam = apvts.getRawParameterValue (params::captureEnabled.getParamID());
    captureAudioParam = apvts.getRawParameterValue (params::captureAudio.getParamID());
    captureOffsetModeParam = apvts.getRawParameterValue (params::captureOffsetMode.getParamID());
    captureOffsetMsParam = apvts.getRawParameterValue (params::captureOffsetMs.getParamID());
    captureFineOffsetMsParam = apvts.getRawParameterValue (params::captureFineOffsetMs.getParamID());

    jassert (captureEnabledParam != nullptr && captureAudioParam != nullptr && captureOffsetModeParam != nullptr
             && captureOffsetMsParam != nullptr && captureFineOffsetMsParam != nullptr);

    captureWorker.setOptions (toWorkerOptions (captureState));

    // Never resized: processBlock downmixes chunks of at most maxChunkSamples
    monoBuffer.assign ((size_t) maxChunkSamples, 0.0f);
    lastSource = getSource();

    midiOutput.setVirtualPort (&virtualMidiPort);
}

VocalSynthProcessor::~VocalSynthProcessor()
{
    // The worker joins within one bounded step (an unfinished WAV job is aborted), then the instance
    // folder (MIDI exports, WAV cache) goes; WAVs moved to the takes folder stay (docs/M3-DESIGN.md 12, 15)
    captureWorker.stop();
    CaptureExporter::deleteInstanceFolder (captureWorker.getInstanceFolder());
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
    recentBlockSize.prepare (sampleRate);
    dspLoad.prepare (sampleRate);

    // Capture (docs/M3-DESIGN.md 15): the worker drains and parks while the tap's rings are (re)allocated;
    // the first call starts the worker thread. The history survives a prepare at the same rate.
    captureWorker.suspend();
    captureTap.prepare (sampleRate, maximumExpectedSamplesPerBlock); // allocates (allowed here)
    preparedMaxBlockSize.store (maximumExpectedSamplesPerBlock, std::memory_order_relaxed);

    if (captureWorker.isStarted())
        captureWorker.resume();
    else
        captureWorker.start();

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
    const auto numSamples = buffer.getNumSamples();
    const ScopedLoadMeasurement loadMeasurement (dspLoad, numSamples, secondsPerTick);
    juce::ScopedNoDenormals noDenormals;

    // No MIDI input is accepted; the buffer is ours to fill
    midi.clear();

    blockSizeHistogram.record (numSamples);
    recentBlockSize.record (numSamples);
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

    // Capture (docs/M3-DESIGN.md 4): reset / transport / settings / markers at the block start
    captureTap.beginBlock (makeTransportInfo (getPlayHead()),
                           makeCaptureBlockSettings (source, voiceSettings, settings, false));
    const bool captureAudio = isCaptureEnabled() && isCaptureAudioEnabled();

    if (flushPending.exchange (false, std::memory_order_relaxed) || sourceChanged)
    {
        // Before anything of this block is generated, so the restarted pattern's first note-on at
        // offset 0 comes after these note-offs (docs/M1-DESIGN.md 4.4)
        midiOutput.flushAllNotes (0, midi);
        captureTap.flushNotes(); // the same note-offs, recorded
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
        eventTimings.clear();

        // FX: main input bus. Instrument: its only input bus is the sidechain (M1, T3). Also downmixed in
        // Test Pattern mode while the capture records the voice.
        const float* mono = voiceMode || captureAudio ? downmixInput (buffer, numInputChannels, start, length) : nullptr;

        if (voiceMode)
            voicePipeline.process (mono, length, voiceSettings, events, eventTimings);
        else
            generator.process (length, settings, events);

        if (clearAudio)
            buffer.clear (start, length);

        midiOutput.write (events, length, midi, start);

        // After the chunk's events are final: exactly what the output stage was given
        captureTap.processChunk (captureAudio ? mono : nullptr, length, events.view(),
                                 voiceMode ? eventTimings.view() : std::span<const VsEventTiming> {});

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
    const auto numSamples = buffer.getNumSamples();
    const ScopedLoadMeasurement loadMeasurement (dspLoad, numSamples, secondsPerTick);
    midi.clear();

    blockSizeHistogram.record (numSamples);
    recentBlockSize.record (numSamples);
    virtualMidiPort.setEnabled (virtualMidiOutParam->load (std::memory_order_relaxed) >= 0.5f);
    currentTestNote.store (-1, std::memory_order_relaxed);

    if (numSamples >= 1)
    {
        // Capture: a bypassed block is recorded as silence (markers bypassOn / bypassOff), no events
        captureTap.beginBlock (makeTransportInfo (getPlayHead()),
                               makeCaptureBlockSettings (getSource(), getVoiceSettings(), getTestPatternSettings(), true));

        // Nothing may hang while bypassed. Repeated bypassed blocks emit nothing (nothing is active
        // any more), and the pattern restarts from its beginning after un-bypass.
        midiOutput.flushAllNotes (0, midi);
        captureTap.flushNotes();
        captureTap.processBypassed (numSamples);
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
CaptureBlockSettings VocalSynthProcessor::makeCaptureBlockSettings (params::Source source,
                                                                    const VoiceSettings& voiceSettings,
                                                                    const TestPatternSettings& patternSettings,
                                                                    bool bypassed) const noexcept
{
    CaptureBlockSettings b;
    b.captureEnabled = isCaptureEnabled();
    b.captureAudio = isCaptureAudioEnabled();
    b.bypassed = bypassed;

    if (source == params::Source::voice)
    {
        b.settings.bendMode = voiceSettings.bendMode;
        b.settings.bendRangeSemitones = bendRangeSemitones (voiceSettings.bendRange);
        b.settings.channel = voiceSettings.channel;
        b.settings.minNoteLengthMs = voiceSettings.minNoteLengthMs;
        b.settings.testPattern = false;
    }
    else
    {
        // The test pattern sends bends with the +-2 range of the pattern and MonitorSynth (docs/M3-DESIGN.md 4)
        b.settings.bendMode = BendMode::trueBend;
        b.settings.bendRangeSemitones = 2;
        b.settings.channel = patternSettings.channel;
        b.settings.minNoteLengthMs = 0.0f;
        b.settings.testPattern = true;
    }

    return b;
}

CaptureWorkerOptions VocalSynthProcessor::toWorkerOptions (const CaptureState& state) noexcept
{
    CaptureWorkerOptions o;
    o.takeGapSeconds = state.takeGapSeconds;
    o.audioHistoryMinutes = state.audioHistoryMinutes;
    o.freePreRollMs = state.render.freePreRollMs;
    o.wavFormat = state.wavFormat;
    return o;
}

CaptureState VocalSynthProcessor::getCaptureState() const
{
    const std::scoped_lock lock (captureStateMutex);
    return captureState;
}

void VocalSynthProcessor::setCaptureState (const CaptureState& state)
{
    // Through the tree so every value is clamped like a loaded state
    const auto sanitised = CaptureState::fromValueTree (state.toValueTree());

    {
        const std::scoped_lock lock (captureStateMutex);
        captureState = sanitised;
    }

    captureWorker.setOptions (toWorkerOptions (sanitised));
}

CaptureOffsetSettings VocalSynthProcessor::getCaptureOffsetSettings() const noexcept
{
    CaptureOffsetSettings o;
    o.mode = static_cast<CaptureOffsetMode> (juce::jlimit (
        0, numCaptureOffsetModes - 1, juce::roundToInt (captureOffsetModeParam->load (std::memory_order_relaxed))));
    o.offsetMs = static_cast<double> (captureOffsetMsParam->load (std::memory_order_relaxed));
    o.fineOffsetMs = static_cast<double> (captureFineOffsetMsParam->load (std::memory_order_relaxed));
    return o;
}

RenderOptions VocalSynthProcessor::getRenderOptionsFor (const Take& take) const
{
    auto options = getCaptureState().render;
    options.roundTripSamples = TakeRenderer::roundTripFor (take, getCaptureOffsetSettings());
    return options;
}

juce::File VocalSynthProcessor::getTakesFolder() const
{
    const auto folder = getCaptureState().takesFolder;

    if (folder.isNotEmpty() && juce::File::isAbsolutePath (folder))
        return juce::File (folder);

    return CaptureExporter::getDefaultTakesFolder();
}

double VocalSynthProcessor::getAutoCaptureOffsetMs() const noexcept
{
    const auto rate = getCurrentSampleRate();
    const auto block = getPreparedMaxBlockSize();

    if (rate <= 0.0 || block <= 0)
        return 0.0;

    return 1000.0 * static_cast<double> (computeRoundTripSamples (CaptureOffsetMode::automatic, 0.0, 0.0, block, rate))
           / rate;
}

bool VocalSynthProcessor::isCaptureEnabled() const noexcept
{
    return captureEnabledParam->load (std::memory_order_relaxed) >= 0.5f;
}

bool VocalSynthProcessor::isCaptureAudioEnabled() const noexcept
{
    return captureAudioParam->load (std::memory_order_relaxed) >= 0.5f;
}

//==============================================================================
juce::AudioProcessorEditor* VocalSynthProcessor::createEditor()
{
    return new VocalSynthEditor (*this);
}

void VocalSynthProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    // Parameters + the non-parameter capture settings as child CAPTURE; takes are never saved (decision 4)
    auto state = apvts.copyState();
    state.removeChild (state.getChildWithName (CaptureState::treeType), nullptr);
    state.appendChild (getCaptureState().toValueTree(), nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void VocalSynthProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
    {
        if (xml->hasTagName (apvts.state.getType()))
        {
            auto state = juce::ValueTree::fromXml (*xml);

            // The capture settings are not parameters: read them, then take the child out of the tree, or
            // the APVTS state would keep it and every save would append one more (docs/M3-DESIGN.md 13).
            // Old projects have none: defaults.
            const auto capture = CaptureState::fromValueTree (state.getChildWithName (CaptureState::treeType));

            for (auto child = state.getChildWithName (CaptureState::treeType); child.isValid();
                 child = state.getChildWithName (CaptureState::treeType))
                state.removeChild (child, nullptr);

            migrateState (state, AudioProcessor::getParameters());
            apvts.replaceState (state);
            setCaptureState (capture);

            // The values changed outside any host automation: tell the host to re-read them.
            // CLAP maps this to params->rescan (CLAP_PARAM_RESCAN_VALUES), which the CLAP spec
            // asks for after a state load (checked by clap-validator's state-reproducibility tests).
            // VST3/AU/Standalone ignore it here because the plugin has no program parameter.
            updateHostDisplay (ChangeDetails().withProgramChanged (true));
        }
    }
}

} // namespace vocalsynth
