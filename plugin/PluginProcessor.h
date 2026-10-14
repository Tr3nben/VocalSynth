#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

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

private:
    static BusesProperties makeBusesProperties (Variant variant);

    const Variant variant;
    juce::AudioProcessorValueTreeState apvts;
    std::atomic<float>* muteDryParam = nullptr;

    std::atomic<float> inputPeak { 0.0f };
    static_assert (std::atomic<float>::is_always_lock_free);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VocalSynthProcessor)
};

} // namespace vocalsynth
