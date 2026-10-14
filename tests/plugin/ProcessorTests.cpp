#include "Parameters.h"
#include "PluginProcessor.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

using vocalsynth::Variant;
using vocalsynth::VocalSynthProcessor;

namespace
{

void setParameter (VocalSynthProcessor& p, const juce::ParameterID& id, float normalisedValue)
{
    auto* param = p.getParameters().getParameter (id.getParamID());
    REQUIRE (param != nullptr);
    param->setValueNotifyingHost (normalisedValue);
}

void fillWithSine (juce::AudioBuffer<float>& buffer, double sampleRate, int startSample)
{
    for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
        for (int i = 0; i < buffer.getNumSamples(); ++i)
            buffer.setSample (ch, i,
                              0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * 220.0
                                                       * (startSample + i) / sampleRate));
}

/** Runs one second of a 220 Hz sine through the processor in blocks of the given size.
    Returns the largest absolute difference between input and output and the MIDI event count. */
struct RunResult
{
    float maxDifferenceFromInput = 0.0f;
    float maxOutput = 0.0f;
    int midiEvents = 0;
};

RunResult runSine (VocalSynthProcessor& p, double sampleRate, int blockSize)
{
    p.setRateAndBufferSizeDetails (sampleRate, blockSize);
    p.prepareToPlay (sampleRate, blockSize);

    RunResult result;
    juce::AudioBuffer<float> buffer (2, blockSize), input (2, blockSize);
    juce::MidiBuffer midi;
    const auto totalSamples = (int) sampleRate;

    for (int pos = 0; pos < totalSamples; pos += blockSize)
    {
        const auto n = juce::jmin (blockSize, totalSamples - pos);
        buffer.setSize (2, n, false, false, true);
        input.setSize (2, n, false, false, true);
        fillWithSine (input, sampleRate, pos);
        buffer.makeCopyOf (input, true);
        midi.clear();

        p.processBlock (buffer, midi);

        result.midiEvents += midi.getNumEvents();
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < n; ++i)
            {
                const auto out = buffer.getSample (ch, i);
                result.maxOutput = juce::jmax (result.maxOutput, std::abs (out));
                result.maxDifferenceFromInput =
                    juce::jmax (result.maxDifferenceFromInput, std::abs (out - input.getSample (ch, i)));
            }
    }

    p.releaseResources();
    return result;
}

} // namespace

TEST_CASE ("Both variants identify themselves correctly", "[plugin]")
{
    VocalSynthProcessor fx (Variant::fx);
    VocalSynthProcessor instrument (Variant::instrument);

    CHECK (fx.getName() == "VocalSynth FX");
    CHECK (instrument.getName() == "VocalSynth Instrument");

    for (auto* p : { &fx, &instrument })
    {
        CHECK (p->producesMidi());
        CHECK_FALSE (p->acceptsMidi());
        CHECK_FALSE (p->isMidiEffect()); // a MIDI effect would have no audio input
        CHECK (p->getTailLengthSeconds() == 0.0);
        CHECK (p->getLatencySamples() == 0); // analysis delay must not delay the audio
        CHECK (p->getProgramName (0).isNotEmpty()); // required by Steinberg's validator
    }
}

TEST_CASE ("Bus layouts", "[plugin]")
{
    const auto mono = juce::AudioChannelSet::mono();
    const auto stereo = juce::AudioChannelSet::stereo();
    const auto disabled = juce::AudioChannelSet::disabled();

    auto layout = [] (juce::AudioChannelSet in, juce::AudioChannelSet out)
    {
        juce::AudioProcessor::BusesLayout l;
        l.inputBuses.add (in);
        l.outputBuses.add (out);
        return l;
    };

    SECTION ("FX passes the mic through, so input and output must match")
    {
        VocalSynthProcessor fx (Variant::fx);
        CHECK (fx.getTotalNumInputChannels() == 2);
        CHECK (fx.getTotalNumOutputChannels() == 2);
        CHECK (fx.checkBusesLayoutSupported (layout (stereo, stereo)));
        CHECK (fx.checkBusesLayoutSupported (layout (mono, mono)));
        CHECK_FALSE (fx.checkBusesLayoutSupported (layout (mono, stereo)));
        CHECK_FALSE (fx.checkBusesLayoutSupported (layout (disabled, stereo)));
    }

    SECTION ("Instrument has an optional sidechain input")
    {
        VocalSynthProcessor instrument (Variant::instrument);
        CHECK (instrument.getBus (true, 0)->getName() == "Sidechain");
        CHECK (instrument.checkBusesLayoutSupported (layout (stereo, stereo)));
        CHECK (instrument.checkBusesLayoutSupported (layout (mono, stereo)));
        CHECK (instrument.checkBusesLayoutSupported (layout (disabled, stereo)));
        CHECK_FALSE (instrument.checkBusesLayoutSupported (layout (stereo, disabled)));
    }
}

TEST_CASE ("Processing is independent of sample rate and block size", "[plugin][realtime]")
{
    // FL Studio sends small and changing block sizes, including single samples
    const auto sampleRate = GENERATE (44100.0, 48000.0, 96000.0);
    const auto blockSize = GENERATE (1, 7, 64, 480, 512, 4096);
    CAPTURE (sampleRate, blockSize);

    SECTION ("FX passes the voice through unchanged and emits no MIDI yet")
    {
        VocalSynthProcessor fx (Variant::fx);
        const auto r = runSine (fx, sampleRate, blockSize);
        CHECK (r.maxDifferenceFromInput == 0.0f);
        CHECK (r.midiEvents == 0);
    }

    SECTION ("FX with Mute Dry outputs silence")
    {
        VocalSynthProcessor fx (Variant::fx);
        setParameter (fx, vocalsynth::params::muteDry, 1.0f);
        CHECK (runSine (fx, sampleRate, blockSize).maxOutput == 0.0f);
    }

    SECTION ("Instrument outputs silence until the internal engine exists")
    {
        VocalSynthProcessor instrument (Variant::instrument);
        const auto r = runSine (instrument, sampleRate, blockSize);
        CHECK (r.maxOutput == 0.0f);
        CHECK (r.midiEvents == 0);
    }
}

TEST_CASE ("Input level is metered for the UI", "[plugin]")
{
    VocalSynthProcessor fx (Variant::fx);
    fx.prepareToPlay (48000.0, 256);

    juce::AudioBuffer<float> buffer (2, 256);
    juce::MidiBuffer midi;
    fillWithSine (buffer, 48000.0, 0);
    fx.processBlock (buffer, midi);

    const auto peak = fx.takeInputPeak();
    CHECK (peak > 0.4f);
    CHECK (peak <= 0.5f);
    CHECK (fx.takeInputPeak() == 0.0f); // reading resets the meter
}

TEST_CASE ("State survives save and restore", "[plugin][state]")
{
    juce::MemoryBlock state;
    {
        VocalSynthProcessor fx (Variant::fx);
        setParameter (fx, vocalsynth::params::muteDry, 1.0f);
        fx.getStateInformation (state);
    }

    VocalSynthProcessor restored (Variant::fx);
    restored.setStateInformation (state.getData(), (int) state.getSize());
    auto* param = restored.getParameters().getParameter (vocalsynth::params::muteDry.getParamID());
    CHECK (param->getValue() == 1.0f);

    SECTION ("Garbage state is ignored instead of crashing")
    {
        const char garbage[] = "not a VocalSynth state";
        restored.setStateInformation (garbage, (int) sizeof (garbage));
        CHECK (param->getValue() == 1.0f);
    }
}
