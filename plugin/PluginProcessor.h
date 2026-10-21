#pragma once

#include "CaptureSettings.h"
#include "CaptureWorker.h"
#include "MidiOutputStage.h"
#include "Parameters.h"
#include "VirtualMidiPort.h"
#include "vocalsynth/BlockSizeHistogram.h"
#include "vocalsynth/CaptureTap.h"
#include "vocalsynth/EventTiming.h"
#include "vocalsynth/MonitorSynth.h"
#include "vocalsynth/TestPatternGenerator.h"
#include "vocalsynth/VoicePipeline.h"
#include "vocalsynth/VoiceSettings.h"
#include "vocalsynth/VsEvent.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace vocalsynth
{

/** Which product this processor instance behaves as. Both are built from the same code. */
enum class Variant
{
    fx,        ///< Audio effect: main audio in/out (the mic), MIDI out.
    instrument ///< Instrument: sidechain audio in, audio out (internal engine later), MIDI out.
};

/** Converts a pitch-bend value meant for a synth with a bend range of +-rangeSemitones into the value
    that MonitorSynth (fixed +-MonitorSynth::bendRangeSemitones) needs for the same pitch, clamped to
    -8192..8191. Used in Voice mode so the monitor tone follows True/Anchored bends with ranges above +-2
    (docs/M2-DESIGN.md 3.9). Bends beyond +-2 semitones are clamped (the residual of a held note rarely
    exceeds that; a larger interval starts a new note). */
int16_t monitorBendFor (int16_t bend, int rangeSemitones) noexcept;

/** VocalSynth's own DSP load (v0.2.1): processing time of one processBlock as a share of the block's
    real-time duration (numSamples / sampleRate), so the user can tell VocalSynth apart from the synth
    in FL's CPU meter. addBlock() runs on the audio thread (wait-free, no allocation: plain members +
    relaxed atomics); getReading() and requestReset() on any thread.
    - average: exponential moving average over time (averageTimeConstantSeconds, independent of the
      block size), the first block after a reset sets it directly;
    - peak: largest single block, decaying with peakDecayTimeConstantSeconds (a 3 s fall to 1/e). */
class DspLoadMeter
{
public:
    static constexpr double averageTimeConstantSeconds = 1.0;
    static constexpr double peakDecayTimeConstantSeconds = 3.0;

    struct Reading
    {
        float averageProportion = 0.0f; ///< 0.05 = 5 % of the block time
        float peakProportion = 0.0f;
        bool valid = false;             ///< false until a block was measured (after a reset)
    };

    /** Not concurrent with addBlock() (prepareToPlay). Also resets the reading. */
    void prepare (double newSampleRate) noexcept;

    /** Audio thread. processingSeconds: wall-clock time spent in processBlock for numSamples samples. */
    void addBlock (double processingSeconds, int numSamples) noexcept;

    Reading getReading() const noexcept;

    /** Any thread: the next addBlock() starts the reading afresh. */
    void requestReset() noexcept { resetPending.store (true, std::memory_order_relaxed); }

private:
    double sampleRate = 0.0;
    double average = 0.0;
    double peak = 0.0;
    std::atomic<bool> resetPending { true };
    std::atomic<float> publishedAverage { 0.0f };
    std::atomic<float> publishedPeak { 0.0f };
    std::atomic<bool> publishedValid { false };
};

/** Largest host block of the last window (windowSeconds of audio) or of the window in progress, for the
    buffer / total latency display (v0.2.1). The maximum and not the most frequent size: FL without
    "fixed size buffers" splits its audio buffer into smaller blocks, and the largest block is the one
    that matches the ASIO buffer (it also sets the worst-case delay). After the buffer is made smaller
    the display follows within one to two windows. record(): audio thread (wait-free); get(): any thread. */
class RecentBlockSize
{
public:
    static constexpr double windowSeconds = 1.0;

    /** Not concurrent with record() (prepareToPlay). Clears the values. */
    void prepare (double sampleRate) noexcept;

    void record (int numSamples) noexcept;

    /** 0 before the first non-empty block. */
    int get() const noexcept;

private:
    int64_t windowSamples = 48000;
    int64_t samplesInWindow = 0;
    std::atomic<int> runningMax { 0 };
    std::atomic<int> previousMax { 0 };
};

class VocalSynthProcessor final : public juce::AudioProcessor
{
public:
    explicit VocalSynthProcessor (Variant variant);
    ~VocalSynthProcessor() override;

    //==============================================================================
    void prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override;
    void reset() override;

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;
    void processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;
    using AudioProcessor::processBlock;
    using AudioProcessor::processBlockBypassed;

    //==============================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override;
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return true; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    // A single, named program: Steinberg's validator rejects plugins with unnamed programs
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return "Default"; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==============================================================================
    Variant getVariant() const noexcept { return variant; }
    juce::AudioProcessorValueTreeState& getParameters() noexcept { return apvts; }

    /** Peak level of the incoming voice signal since the last call; read by the UI thread. */
    float takeInputPeak() noexcept { return inputPeak.exchange (0.0f, std::memory_order_relaxed); }

    //==============================================================================
    // M1 routing test build: read by the UI thread (Timer), all lock-free except getVirtualMidiDeviceName().

    /** Host block sizes seen by processBlock. */
    BlockSizeHistogram::Snapshot getBlockSizeHistogram() const noexcept;
    void resetBlockSizeHistogram() noexcept;

    /** Sample rate of the last prepareToPlay(), 0 before the first one. */
    double getCurrentSampleRate() const noexcept;

    /** Note currently held by the test pattern, -1 if none. */
    int getCurrentTestNote() const noexcept;

    /** Current values of the test-pattern parameters (from the raw parameter atomics). Any thread.
        Used by the editor for TestClipExporter::exportMidi(). */
    TestPatternSettings getTestPatternSettings() const noexcept;

    /** Current values of the M2 voice parameters as one VoiceSettings value (from the raw parameter
        atomics, sanitised; channel = midiChannel). Any thread, no allocation. */
    VoiceSettings getVoiceSettings() const noexcept;

    /** What drives the MIDI output (the "source" parameter). Any thread. */
    params::Source getSource() const noexcept;

    VirtualMidiPort::Status getVirtualMidiStatus() const noexcept;
    uint32_t getVirtualMidiOverflowCount() const noexcept;
    juce::String getVirtualMidiDeviceName() const; // message thread

    /** MIDI messages written to the host MidiBuffer since construction, per type. Any thread. */
    MidiOutputStage::SentCounts getMidiSentCounts() const noexcept { return midiOutput.getSentCounts(); }

    /** "Restart pattern" button: the next processBlock sends note-offs for everything that is active
        and restarts the test pattern from its beginning (same path as after reset()). Any thread. */
    void requestFlush() noexcept { flushPending.store (true, std::memory_order_relaxed); }

    //==============================================================================
    // M2 pitch -> MIDI: read by the UI thread (Timer), all lock-free.

    /** Newest tuner values of the voice analysis (seqlock copy, docs/M2-DESIGN.md 3.10). Only updated
        while source = Voice; a flush (source switch, bypass, prepare) publishes an empty snapshot. */
    VoiceAnalysisSnapshot getVoiceSnapshot() const noexcept { return voicePipeline.getSnapshot(); }

    /** Worst-case onset -> note-on delay of the analysis in ms for the current voice range and sample
        rate (48 kHz before the first prepareToPlay). Not reported to the host: the audio is not delayed. */
    double getVoiceLatencyMs() const noexcept;

    /** Largest host block size of the last 1-2 seconds (RecentBlockSize), 0 before the first block. */
    int getRecentMaxBlockSize() const noexcept { return recentBlockSize.get(); }

    /** VocalSynth's own processing time per block as a share of the block duration (DspLoadMeter). */
    DspLoadMeter::Reading getDspLoad() const noexcept { return dspLoad.getReading(); }
    void resetDspLoad() noexcept { dspLoad.requestReset(); }

    //==============================================================================
    // M3 capture (docs/M3-DESIGN.md 2, 13, 15)

    /** The capture worker: take list, WAV cache, status (message thread; see CaptureWorker). */
    CaptureWorker& getCaptureWorker() noexcept { return captureWorker; }
    const CaptureWorker& getCaptureWorker() const noexcept { return captureWorker; }

    /** UI requests (New take / Grab / Clear) go to the tap: requestNewTake(), requestGrab(), requestClear()
        are lock-free (any thread); the next processBlock turns them into markers. */
    CaptureTap& getCaptureTap() noexcept { return captureTap; }

    /** Non-parameter capture settings (export options, grab lengths, free-mode gap, look-back, takes
        folder). Any thread (own mutex; the audio thread never reads them). setCaptureState() also hands
        the worker its options. */
    CaptureState getCaptureState() const;
    void setCaptureState (const CaptureState& state);

    /** The three capture-offset parameters (raw atomics). Any thread. */
    CaptureOffsetSettings getCaptureOffsetSettings() const noexcept;

    /** Export options for a take as the user set them: CaptureState::render with the take's round trip
        (TakeRenderer::roundTripFor with the current offset parameters). Any thread. */
    RenderOptions getRenderOptionsFor (const Take& take) const;

    /** Folder for dragged / saved WAVs (CaptureState::takesFolder, else <Documents>/VocalSynth/Takes). */
    juce::File getTakesFolder() const;

    /** Auto capture offset of the current preparation (2 x the prepared max block, without the fine
        offset) in ms; 0 before the first prepareToPlay. */
    double getAutoCaptureOffsetMs() const noexcept;

    /** Maximum block size of the last prepareToPlay(), 0 before the first one. */
    int getPreparedMaxBlockSize() const noexcept { return preparedMaxBlockSize.load (std::memory_order_relaxed); }

    /** Whether the captureEnabled / captureAudio parameters are on. Any thread. */
    bool isCaptureEnabled() const noexcept;
    bool isCaptureAudioEnabled() const noexcept;

    /** M3.1 parameters (raw atomics, any thread): captureMode, takeMaxBars in bars (0 = Off), recordBars. */
    CaptureMode getCaptureMode() const noexcept;
    int getTakeMaxBars() const noexcept;
    int getRecordBars() const noexcept;

    const juce::String& getInstanceId() const noexcept { return instanceId; }

    static constexpr std::size_t eventBufferCapacity = 1024;

    /** processBlock runs the generator, output stage and monitor synth in chunks of at most this many
        samples, so even huge host blocks can never overflow the event buffer (a dropped note-off
        would be a stuck note). Worst case: about 40 events per chunk at 44.1 kHz, about 220 at 8 kHz
        for the test pattern (docs/M1-DESIGN.md 4.4); VoicePipeline::maxEventsForSamples (4096) is 143
        at 44.1 kHz and 770 at 8 kHz (docs/M2-DESIGN.md 3.10), all below eventBufferCapacity. */
    static constexpr int maxChunkSamples = 4096;

    /** The monitor tone is added to at most this many output channels (layouts are mono or stereo). */
    static constexpr int maxToneChannels = 2;

private:
    static BusesProperties makeBusesProperties (Variant variant);

    /** Averages the input channels of [start, start + length) into monoBuffer and returns it, or
        nullptr if there is no input channel (Instrument with the sidechain disabled = silence). */
    const float* downmixInput (const juce::AudioBuffer<float>& buffer, int numInputs, int start, int length) noexcept;

    /** Voice mode: rescales the pitch bends in events for MonitorSynth (see monitorBendFor()). */
    void prepareMonitorEvents (int bendRangeSemitones) noexcept;

    /** What the capture tap records about this block's switches and settings (raw atomics, no allocation). */
    CaptureBlockSettings makeCaptureBlockSettings (params::Source source, const VoiceSettings& voiceSettings,
                                                   const TestPatternSettings& patternSettings, bool bypassed) const noexcept;

    static CaptureWorkerOptions toWorkerOptions (const CaptureState& state) noexcept;

    const Variant variant;
    juce::AudioProcessorValueTreeState apvts;
    std::atomic<float>* muteDryParam = nullptr;
    std::atomic<float>* testPatternParam = nullptr;
    std::atomic<float>* midiChannelParam = nullptr;
    std::atomic<float>* testTempoParam = nullptr;
    std::atomic<float>* ccNumberParam = nullptr;
    std::atomic<float>* monitorToneParam = nullptr;
    std::atomic<float>* virtualMidiOutParam = nullptr;

    // M2 parameters (raw atomics, read on any thread)
    std::atomic<float>* sourceParam = nullptr;
    std::atomic<float>* voiceRangeParam = nullptr;
    std::atomic<float>* pitchAlgorithmParam = nullptr;
    std::atomic<float>* keyParam = nullptr;
    std::atomic<float>* scaleParam = nullptr;
    std::atomic<float>* stickinessParam = nullptr;
    std::atomic<float>* gateThresholdParam = nullptr;
    std::atomic<float>* minNoteLengthParam = nullptr;
    std::atomic<float>* octaveShiftParam = nullptr;
    std::atomic<float>* bendModeParam = nullptr;
    std::atomic<float>* bendRangeParam = nullptr;
    std::atomic<float>* velocitySensitivityParam = nullptr;
    std::atomic<float>* legatoParam = nullptr;

    // M3 parameters (raw atomics, read on any thread)
    std::atomic<float>* captureEnabledParam = nullptr;
    std::atomic<float>* captureAudioParam = nullptr;
    std::atomic<float>* captureOffsetModeParam = nullptr;
    std::atomic<float>* captureOffsetMsParam = nullptr;
    std::atomic<float>* captureFineOffsetMsParam = nullptr;

    // M3.1 parameters (raw atomics, read on any thread)
    std::atomic<float>* captureModeParam = nullptr;
    std::atomic<float>* takeMaxBarsParam = nullptr;
    std::atomic<float>* recordBarsParam = nullptr;

    // Audio-thread state (prepared in prepareToPlay, no allocation in processBlock)
    TestPatternGenerator generator;
    VsEventBuffer<eventBufferCapacity> events;
    VsEventTimingBuffer<eventBufferCapacity> eventTimings; ///< M3: timing of events[i] (Voice mode), cleared with events
    VsEventBuffer<eventBufferCapacity> monitorEvents; ///< Voice mode: events with bends rescaled for the monitor
    VoicePipeline voicePipeline;
    std::vector<float> monoBuffer;                    ///< maxChunkSamples, allocated in the constructor
    params::Source lastSource = params::Source::voice; ///< audio thread: source of the previous block
    MonitorSynth monitorSynth;
    MidiOutputStage midiOutput;
    VirtualMidiPort virtualMidiPort; // owns its own thread; destroyed before midiOutput (declared after)

    // Set by reset()/prepareToPlay()/releaseResources(); the next processBlock emits the note-offs
    std::atomic<bool> flushPending { true };

    // Audio -> UI
    BlockSizeHistogram blockSizeHistogram;
    RecentBlockSize recentBlockSize;
    DspLoadMeter dspLoad;
    const double secondsPerTick; ///< 1 / juce::Time::getHighResolutionTicksPerSecond()
    std::atomic<double> currentSampleRate { 0.0 };
    std::atomic<int> currentTestNote { -1 };

    std::atomic<float> inputPeak { 0.0f };
    static_assert (std::atomic<float>::is_always_lock_free);
    static_assert (std::atomic<double>::is_always_lock_free);

    // M3 capture. Declared last, the worker after the tap: the worker (which reads the tap's rings) is
    // destroyed first (docs/M3-DESIGN.md 15). Its sources point at members declared above.
    std::atomic<int> preparedMaxBlockSize { 0 };
    const juce::String instanceId;
    mutable std::mutex captureStateMutex; ///< never touched by the audio thread
    CaptureState captureState;
    CaptureTap captureTap;
    CaptureWorker captureWorker;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VocalSynthProcessor)
};

} // namespace vocalsynth
