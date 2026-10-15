#pragma once

#include "MidiOutputStage.h"
#include "VirtualMidiPort.h"
#include "vocalsynth/BlockSizeHistogram.h"
#include "vocalsynth/MonitorSynth.h"
#include "vocalsynth/TestPatternGenerator.h"
#include "vocalsynth/VsEvent.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>

namespace vocalsynth
{

/** Which product this processor instance behaves as. Both are built from the same code. */
enum class Variant
{
    fx,        ///< Audio effect: main audio in/out (the mic), MIDI out.
    instrument ///< Instrument: sidechain audio in, audio out (internal engine later), MIDI out.
};

class VocalSynthProcessor final : public juce::AudioProcessor
{
public:
    explicit VocalSynthProcessor (Variant variant);
    ~VocalSynthProcessor() override = default;

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

    VirtualMidiPort::Status getVirtualMidiStatus() const noexcept;
    uint32_t getVirtualMidiOverflowCount() const noexcept;
    juce::String getVirtualMidiDeviceName() const; // message thread

    /** MIDI messages written to the host MidiBuffer since construction, per type. Any thread. */
    MidiOutputStage::SentCounts getMidiSentCounts() const noexcept { return midiOutput.getSentCounts(); }

    /** "Restart pattern" button: the next processBlock sends note-offs for everything that is active
        and restarts the test pattern from its beginning (same path as after reset()). Any thread. */
    void requestFlush() noexcept { flushPending.store (true, std::memory_order_relaxed); }

    static constexpr std::size_t eventBufferCapacity = 1024;

    /** processBlock runs the generator, output stage and monitor synth in chunks of at most this many
        samples, so even huge host blocks can never overflow the event buffer (a dropped note-off
        would be a stuck note). Worst case: about 40 events per chunk at 44.1 kHz, about 220 at 8 kHz
        (docs/M1-DESIGN.md 4.4), far below eventBufferCapacity. */
    static constexpr int maxChunkSamples = 4096;

    /** The monitor tone is added to at most this many output channels (layouts are mono or stereo). */
    static constexpr int maxToneChannels = 2;

private:
    static BusesProperties makeBusesProperties (Variant variant);

    const Variant variant;
    juce::AudioProcessorValueTreeState apvts;
    std::atomic<float>* muteDryParam = nullptr;
    std::atomic<float>* testPatternParam = nullptr;
    std::atomic<float>* midiChannelParam = nullptr;
    std::atomic<float>* testTempoParam = nullptr;
    std::atomic<float>* ccNumberParam = nullptr;
    std::atomic<float>* monitorToneParam = nullptr;
    std::atomic<float>* virtualMidiOutParam = nullptr;

    // Audio-thread state (prepared in prepareToPlay, no allocation in processBlock)
    TestPatternGenerator generator;
    VsEventBuffer<eventBufferCapacity> events;
    MonitorSynth monitorSynth;
    MidiOutputStage midiOutput;
    VirtualMidiPort virtualMidiPort; // owns its own thread; destroyed before midiOutput (declared after)

    // Set by reset()/prepareToPlay()/releaseResources(); the next processBlock emits the note-offs
    std::atomic<bool> flushPending { true };

    // Audio -> UI
    BlockSizeHistogram blockSizeHistogram;
    std::atomic<double> currentSampleRate { 0.0 };
    std::atomic<int> currentTestNote { -1 };

    std::atomic<float> inputPeak { 0.0f };
    static_assert (std::atomic<float>::is_always_lock_free);
    static_assert (std::atomic<double>::is_always_lock_free);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VocalSynthProcessor)
};

} // namespace vocalsynth
