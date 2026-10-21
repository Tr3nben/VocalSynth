#include "CaptureExporter.h"
#include "CaptureTestUtils.h"
#include "Parameters.h"
#include "PluginProcessor.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>

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

/** The M1 tests check the test pattern, which only runs with source = Test Pattern (M2 default: Voice). */
void useTestPatternSource (VocalSynthProcessor& p)
{
    setParameter (p, vocalsynth::params::source, 1.0f); // choice index 1 of 2
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

    SECTION ("FX passes the voice through unchanged; test pattern Off emits no MIDI")
    {
        VocalSynthProcessor fx (Variant::fx);
        useTestPatternSource (fx);
        const auto r = runSine (fx, sampleRate, blockSize);
        CHECK (r.maxDifferenceFromInput == 0.0f);
        CHECK (r.midiEvents == 0);
    }

    SECTION ("FX in Voice mode passes the voice through unchanged")
    {
        VocalSynthProcessor fx (Variant::fx);
        CHECK (runSine (fx, sampleRate, blockSize).maxDifferenceFromInput == 0.0f);
    }

    SECTION ("FX with Mute Dry outputs silence")
    {
        VocalSynthProcessor fx (Variant::fx);
        setParameter (fx, vocalsynth::params::muteDry, 1.0f);
        CHECK (runSine (fx, sampleRate, blockSize).maxOutput == 0.0f);

        useTestPatternSource (fx);
        CHECK (runSine (fx, sampleRate, blockSize).maxOutput == 0.0f);
    }

    SECTION ("Instrument outputs silence until the internal engine exists")
    {
        VocalSynthProcessor voiceInstrument (Variant::instrument);
        CHECK (runSine (voiceInstrument, sampleRate, blockSize).maxOutput == 0.0f); // Voice, Monitor Tone off

        // A fresh instance: the voiced sine above may have left a note that the next prepare flushes
        VocalSynthProcessor instrument (Variant::instrument);
        useTestPatternSource (instrument);
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

//==============================================================================
// M1 routing test build: test pattern, MIDI output, monitor tone, bypass (docs/M1-DESIGN.md 4.4, 6)
//==============================================================================

namespace
{

/** Sets a parameter by its plain (unnormalised) value, e.g. a choice index or a BPM value. */
void setPlainValue (VocalSynthProcessor& p, const juce::ParameterID& id, float plainValue)
{
    auto* param = p.getParameters().getParameter (id.getParamID());
    REQUIRE (param != nullptr);
    param->setValueNotifyingHost (param->convertTo0to1 (plainValue));
}

float getPlainValue (VocalSynthProcessor& p, const juce::ParameterID& id)
{
    auto* param = p.getParameters().getParameter (id.getParamID());
    REQUIRE (param != nullptr);
    return param->convertFrom0to1 (param->getValue());
}

/** Selects a test pattern and source = Test Pattern (the M1 tests below expect pattern MIDI). */
void setPattern (VocalSynthProcessor& p, vocalsynth::TestPattern pattern)
{
    setPlainValue (p, vocalsynth::params::source, (float) static_cast<int> (vocalsynth::params::Source::testPattern));
    setPlainValue (p, vocalsynth::params::testPattern, (float) static_cast<int> (pattern));
}

/** One MIDI message at its absolute sample position since the runner started. */
struct TimedMessage
{
    int64_t position = 0;
    std::array<int, 3> bytes {};
    int size = 0;

    bool operator== (const TimedMessage&) const = default;

    juce::MidiMessage toMidiMessage() const
    {
        return juce::MidiMessage (bytes[0], size > 1 ? bytes[1] : 0, size > 2 ? bytes[2] : 0);
    }
};

/** Drives a processor block by block like a host, collects all MIDI with absolute positions and
    checks the per-block MidiBuffer contract (offsets inside the block, sorted) without slow
    per-event Catch assertions. */
class HostRunner
{
public:
    HostRunner (VocalSynthProcessor& processor, double sampleRate, int maxBlockSize)
        : p (processor), rate (sampleRate)
    {
        p.setRateAndBufferSizeDetails (sampleRate, maxBlockSize);
        p.prepareToPlay (sampleRate, maxBlockSize);
        buffer.setSize (2, maxBlockSize);
    }

    /** Processes one block of n samples. The input is a 220 Hz sine (peak 0.5) unless silentInput or
        inputSignal (channel, absolute position) is set. */
    void block (int n, bool bypassed = false)
    {
        buffer.setSize (2, n, false, false, true);

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < n; ++i)
                buffer.setSample (ch, i,
                                  inputSignal != nullptr ? inputSignal (ch, position + i)
                                  : silentInput          ? 0.0f
                                                         : 0.5f
                                                      * (float) std::sin (juce::MathConstants<double>::twoPi
                                                                          * 220.0 * (double) (position + i) / rate));

        midi.clear();
        lastBlock.clear();

        if (bypassed)
            p.processBlockBypassed (buffer, midi);
        else
            p.processBlock (buffer, midi);

        int lastOffset = 0;
        for (const auto meta : midi)
        {
            if (meta.samplePosition < 0 || meta.samplePosition >= n)
                ++badOffsets;
            if (meta.samplePosition < lastOffset)
                ++unsortedEvents;
            lastOffset = meta.samplePosition;

            TimedMessage m;
            m.position = position + meta.samplePosition;
            m.size = juce::jmin (meta.numBytes, 3);
            for (int i = 0; i < m.size; ++i)
                m.bytes[(size_t) i] = meta.data[i];
            if (meta.numBytes > 3)
                ++badOffsets; // only short messages are expected

            messages.push_back (m);
            lastBlock.push_back (m);
        }

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < n; ++i)
            {
                const auto s = buffer.getSample (ch, i);
                if (! std::isfinite (s))
                    ++nonFiniteSamples;
                if (s != 0.0f && (firstAudiblePosition < 0 || position + i < firstAudiblePosition))
                    firstAudiblePosition = position + i;
                maxOutput = juce::jmax (maxOutput, std::abs (s));
            }

        position += n;
    }

    /** Processes totalSamples in blocks of blockSize (the last one may be shorter). */
    void run (int64_t totalSamples, int blockSize, bool bypassed = false)
    {
        for (int64_t done = 0; done < totalSamples;)
        {
            const auto n = (int) juce::jmin ((int64_t) blockSize, totalSamples - done);
            block (n, bypassed);
            done += n;
        }
    }

    int64_t seconds (double s) const { return (int64_t) std::llround (s * rate); }

    VocalSynthProcessor& p;
    const double rate;
    bool silentInput = false;
    std::function<float (int channel, int64_t position)> inputSignal;

    juce::AudioBuffer<float> buffer;
    juce::MidiBuffer midi;
    int64_t position = 0;

    std::vector<TimedMessage> messages;
    std::vector<TimedMessage> lastBlock;
    int badOffsets = 0;
    int unsortedEvents = 0;
    int nonFiniteSamples = 0;
    float maxOutput = 0.0f;
    int64_t firstAudiblePosition = -1; ///< first absolute position with a non-zero output sample, -1 = none
};

struct PairingResult
{
    int noteOns = 0;
    int noteOffs = 0;
    int duplicateNoteOns = 0; ///< note-on for a note that is already sounding
    int orphanNoteOffs = 0;   ///< note-off for a note that is not sounding
    int stillActive = 0;      ///< notes without a note-off at the end
    int pitchBends = 0;
    int controlChanges = 0;
    bool bendCentredAtEnd = true;
};

PairingResult checkPairing (const std::vector<TimedMessage>& messages)
{
    PairingResult r;
    std::array<std::array<bool, 128>, 16> active {};
    std::array<bool, 16> bendOff {};

    for (const auto& t : messages)
    {
        const auto m = t.toMidiMessage();
        const auto ch = (size_t) (m.getChannel() - 1);

        if (m.isNoteOn())
        {
            ++r.noteOns;
            auto& a = active[ch][(size_t) m.getNoteNumber()];
            if (a)
                ++r.duplicateNoteOns;
            a = true;
        }
        else if (m.isNoteOff())
        {
            ++r.noteOffs;
            auto& a = active[ch][(size_t) m.getNoteNumber()];
            if (! a)
                ++r.orphanNoteOffs;
            a = false;
        }
        else if (m.isPitchWheel())
        {
            ++r.pitchBends;
            bendOff[ch] = m.getPitchWheelValue() != 8192;
        }
        else if (m.isController())
        {
            ++r.controlChanges;
        }
    }

    for (const auto& channel : active)
        for (auto a : channel)
            r.stillActive += a ? 1 : 0;

    for (auto b : bendOff)
        r.bendCentredAtEnd = r.bendCentredAtEnd && ! b;

    return r;
}

const char* patternName (vocalsynth::TestPattern pattern)
{
    switch (pattern)
    {
        case vocalsynth::TestPattern::off:           return "off";
        case vocalsynth::TestPattern::notes:         return "notes";
        case vocalsynth::TestPattern::pitchBend:     return "pitchBend";
        case vocalsynth::TestPattern::controlChange: return "controlChange";
        case vocalsynth::TestPattern::all:           return "all";
    }
    return "?";
}

/** Long enough to cover the interesting parts: two bend/CC cycles and a re-strike; for 'all' every
    segment boundary (16 and 24 beats at 200 bpm = 4.8 s and 7.2 s). */
double runSecondsFor (vocalsynth::TestPattern pattern)
{
    return pattern == vocalsynth::TestPattern::all ? 8.0 : 2.6;
}

float tempoFor (vocalsynth::TestPattern pattern)
{
    return pattern == vocalsynth::TestPattern::all ? 200.0f : 120.0f;
}

std::vector<TimedMessage> runReference (Variant variant, vocalsynth::TestPattern pattern, double sampleRate)
{
    VocalSynthProcessor p (variant);
    setPattern (p, pattern);
    setPlainValue (p, vocalsynth::params::testTempo, tempoFor (pattern));
    HostRunner host (p, sampleRate, 4096);
    host.run (host.seconds (runSecondsFor (pattern)), 4096);
    return host.messages;
}

} // namespace

TEST_CASE ("Test patterns produce valid, paired MIDI for any block size", "[plugin][midi][realtime]")
{
    using vocalsynth::TestPattern;

    const auto pattern = GENERATE (TestPattern::notes, TestPattern::pitchBend, TestPattern::controlChange,
                                   TestPattern::all);
    const auto sampleRate = 48000.0;
    const auto reference = runReference (Variant::fx, pattern, sampleRate);

    // 10000 exceeds maxChunkSamples, so the processor splits that block internally
    const auto blockSize = GENERATE (1, 7, 64, 480, 512, 4096, 10000);
    CAPTURE (patternName (pattern), blockSize);

    VocalSynthProcessor p (Variant::fx);
    setPattern (p, pattern);
    setPlainValue (p, vocalsynth::params::testTempo, tempoFor (pattern));
    HostRunner host (p, sampleRate, blockSize);
    host.run (host.seconds (runSecondsFor (pattern)), blockSize);

    CHECK (host.badOffsets == 0);
    CHECK (host.unsortedEvents == 0);
    CHECK (host.nonFiniteSamples == 0);

    // Block-size invariance: exactly the same messages at the same absolute positions
    CHECK (host.messages.size() == reference.size());
    CHECK (host.messages == reference);

    const auto running = checkPairing (host.messages);
    CHECK (running.noteOns > 0);
    CHECK (running.duplicateNoteOns == 0);
    CHECK (running.orphanNoteOffs == 0);

    if (pattern == TestPattern::pitchBend || pattern == TestPattern::all)
        CHECK (running.pitchBends > 0);
    if (pattern == TestPattern::controlChange || pattern == TestPattern::all)
        CHECK (running.controlChanges > 0);

    SECTION ("switching the pattern off releases every note and centres the bend")
    {
        setPattern (p, TestPattern::off);
        host.block (blockSize);

        const auto r = checkPairing (host.messages);
        CHECK (r.stillActive == 0);
        CHECK (r.orphanNoteOffs == 0);
        CHECK (r.duplicateNoteOns == 0);
        CHECK (r.bendCentredAtEnd);
        CHECK (r.noteOns == r.noteOffs);
        CHECK (host.badOffsets == 0);

        // Off stays silent
        const auto count = host.messages.size();
        host.run (host.seconds (0.5), blockSize);
        CHECK (host.messages.size() == count);
        CHECK (p.getCurrentTestNote() == -1);
    }

    SECTION ("bypassing releases every note and centres the bend")
    {
        host.block (blockSize, true);

        const auto r = checkPairing (host.messages);
        CHECK (r.stillActive == 0);
        CHECK (r.orphanNoteOffs == 0);
        CHECK (r.bendCentredAtEnd);
        CHECK (r.noteOns == r.noteOffs);
        CHECK (host.badOffsets == 0);
        CHECK (p.getCurrentTestNote() == -1);

        // Further bypassed blocks emit nothing
        const auto count = host.messages.size();
        host.run (host.seconds (0.2), blockSize, true);
        CHECK (host.messages.size() == count);
    }
}

TEST_CASE ("Varying host block sizes give the same MIDI as fixed blocks", "[plugin][midi][realtime]")
{
    using vocalsynth::TestPattern;

    const auto pattern = GENERATE (TestPattern::notes, TestPattern::pitchBend, TestPattern::controlChange,
                                   TestPattern::all);
    CAPTURE (patternName (pattern));

    const auto sampleRate = 44100.0;
    const auto reference = runReference (Variant::fx, pattern, sampleRate);

    VocalSynthProcessor p (Variant::fx);
    setPattern (p, pattern);
    setPlainValue (p, vocalsynth::params::testTempo, tempoFor (pattern));
    HostRunner host (p, sampleRate, 8192);

    // FL Studio changes the block size from block to block
    const int sizes[] = { 1, 512, 3, 4096, 64, 8192, 17, 480, 1, 1, 2000, 7 };
    const auto total = host.seconds (runSecondsFor (pattern));

    for (size_t i = 0; host.position < total; ++i)
        host.block ((int) juce::jmin ((int64_t) sizes[i % std::size (sizes)], total - host.position));

    CHECK (host.badOffsets == 0);
    CHECK (host.unsortedEvents == 0);
    CHECK (host.messages == reference);
}

TEST_CASE ("The Instrument variant sends the same MIDI as the FX", "[plugin][midi]")
{
    const auto fx = runReference (Variant::fx, vocalsynth::TestPattern::all, 48000.0);
    const auto instrument = runReference (Variant::instrument, vocalsynth::TestPattern::all, 48000.0);

    CHECK_FALSE (fx.empty());
    CHECK (instrument == fx);
}

TEST_CASE ("Notes pattern timing at 48 kHz / 120 bpm", "[plugin][midi]")
{
    VocalSynthProcessor p (Variant::fx);
    setPattern (p, vocalsynth::TestPattern::notes);
    HostRunner host (p, 48000.0, 480);
    host.run (48000, 480); // one second = 4 eighth notes

    std::vector<TimedMessage> noteOns;
    for (const auto& m : host.messages)
        if (m.toMidiMessage().isNoteOn())
            noteOns.push_back (m);

    REQUIRE (noteOns.size() == 4);
    const int expectedNotes[] = { 60, 62, 64, 65 };
    for (size_t i = 0; i < 4; ++i)
    {
        CHECK (noteOns[i].position == (int64_t) i * 12000); // one eighth note at 120 bpm
        CHECK (noteOns[i].bytes[1] == expectedNotes[i]);
        CHECK (noteOns[i].bytes[2] == 100);
        CHECK (noteOns[i].toMidiMessage().getChannel() == 1);
    }
}

TEST_CASE ("MIDI channel and CC number parameters are used", "[plugin][midi]")
{
    VocalSynthProcessor p (Variant::fx);
    setPattern (p, vocalsynth::TestPattern::controlChange);
    setPlainValue (p, vocalsynth::params::midiChannel, 10.0f);
    setPlainValue (p, vocalsynth::params::ccNumber, 74.0f);

    HostRunner host (p, 48000.0, 512);
    host.run (24000, 512);

    int ccs = 0;
    for (const auto& t : host.messages)
    {
        const auto m = t.toMidiMessage();
        CHECK (m.getChannel() == 10);
        if (m.isController())
        {
            ++ccs;
            CHECK (m.getControllerNumber() == 74);
        }
    }
    CHECK (ccs > 10);

    SECTION ("changing the channel while a note is held moves the pattern without a stuck note")
    {
        setPlainValue (p, vocalsynth::params::midiChannel, 3.0f);
        host.block (512);

        REQUIRE (host.lastBlock.size() >= 2);
        const auto first = host.lastBlock[0].toMidiMessage();
        CHECK (first.isNoteOff());
        CHECK (first.getChannel() == 10);
        CHECK (host.lastBlock[0].position == host.position - 512);

        bool newNoteOn = false;
        for (const auto& t : host.lastBlock)
        {
            const auto m = t.toMidiMessage();
            if (m.isNoteOn())
            {
                newNoteOn = true;
                CHECK (m.getChannel() == 3);
            }
        }
        CHECK (newNoteOn);

        setPattern (p, vocalsynth::TestPattern::off);
        host.block (512);
        CHECK (checkPairing (host.messages).stillActive == 0);
    }
}

TEST_CASE ("The first block after prepareToPlay or reset flushes held notes", "[plugin][midi]")
{
    VocalSynthProcessor p (Variant::fx);
    setPattern (p, vocalsynth::TestPattern::pitchBend);
    HostRunner host (p, 48000.0, 256);
    host.run (2560, 256); // note 60 held, bend > 0

    REQUIRE (p.getCurrentTestNote() == 60);
    REQUIRE_FALSE (checkPairing (host.messages).bendCentredAtEnd);

    const auto restart = GENERATE (0, 1, 2);
    CAPTURE (restart);

    if (restart == 0)
        p.prepareToPlay (48000.0, 256);
    else if (restart == 1)
        p.reset();
    else
        p.releaseResources();

    SECTION ("a 0-sample block emits nothing and keeps the flush pending")
    {
        host.block (0);
        CHECK (host.lastBlock.empty());
    }

    host.block (256);
    const auto& b = host.lastBlock;

    // note-off, bend centre, then the restarted pattern's note-on, all at offset 0
    REQUIRE (b.size() >= 3);
    const auto blockStart = host.position - 256;
    CHECK (b[0].toMidiMessage().isNoteOff());
    CHECK (b[0].toMidiMessage().getNoteNumber() == 60);
    CHECK (b[0].position == blockStart);
    CHECK (b[1].toMidiMessage().isPitchWheel());
    CHECK (b[1].toMidiMessage().getPitchWheelValue() == 8192);
    CHECK (b[1].position == blockStart);
    CHECK (b[2].toMidiMessage().isNoteOn());
    CHECK (b[2].toMidiMessage().getNoteNumber() == 60);
    CHECK (b[2].position == blockStart);

    const auto r = checkPairing (host.messages);
    CHECK (r.duplicateNoteOns == 0);
    CHECK (r.orphanNoteOffs == 0);
}

TEST_CASE ("requestFlush restarts the pattern from its beginning", "[plugin][midi]")
{
    VocalSynthProcessor p (Variant::fx);
    setPattern (p, vocalsynth::TestPattern::notes);
    HostRunner host (p, 48000.0, 500);
    host.run (15000, 500); // second note (62) sounding: 12000..21000

    REQUIRE (p.getCurrentTestNote() == 62);
    p.requestFlush();
    host.block (500);

    const auto& b = host.lastBlock;
    REQUIRE (b.size() == 2);
    CHECK (b[0].toMidiMessage().isNoteOff());
    CHECK (b[0].toMidiMessage().getNoteNumber() == 62);
    CHECK (b[1].toMidiMessage().isNoteOn());
    CHECK (b[1].toMidiMessage().getNoteNumber() == 60);
    CHECK (b[0].position == 15000);
    CHECK (b[1].position == 15000);
    CHECK (p.getCurrentTestNote() == 60);
}

TEST_CASE ("The pattern restarts after un-bypass", "[plugin][midi]")
{
    VocalSynthProcessor p (Variant::fx);
    setPattern (p, vocalsynth::TestPattern::notes);
    HostRunner host (p, 48000.0, 512);
    host.run (15000, 512);
    host.run (2048, 512, true);
    host.block (512);

    REQUIRE_FALSE (host.lastBlock.empty());
    CHECK (host.lastBlock[0].toMidiMessage().isNoteOn());
    CHECK (host.lastBlock[0].toMidiMessage().getNoteNumber() == 60);
    CHECK (host.lastBlock[0].position == host.position - 512);
    CHECK (checkPairing (host.messages).duplicateNoteOns == 0);
}

TEST_CASE ("Bypass keeps the audio behaviour of each variant", "[plugin]")
{
    SECTION ("FX passes the voice through")
    {
        VocalSynthProcessor fx (Variant::fx);
        setParameter (fx, vocalsynth::params::muteDry, 1.0f);
        fx.prepareToPlay (48000.0, 256);
        juce::AudioBuffer<float> buffer (2, 256);
        fillWithSine (buffer, 48000.0, 0);
        juce::AudioBuffer<float> input;
        input.makeCopyOf (buffer);
        juce::MidiBuffer midi;
        fx.processBlockBypassed (buffer, midi);

        float maxDifference = 0.0f;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 256; ++i)
                maxDifference = juce::jmax (maxDifference, std::abs (buffer.getSample (ch, i) - input.getSample (ch, i)));

        CHECK (maxDifference == 0.0f);
    }

    SECTION ("Instrument is silent")
    {
        VocalSynthProcessor instrument (Variant::instrument);
        setParameter (instrument, vocalsynth::params::monitorTone, 1.0f);
        instrument.prepareToPlay (48000.0, 256);
        juce::AudioBuffer<float> buffer (2, 256);
        fillWithSine (buffer, 48000.0, 0);
        juce::MidiBuffer midi;
        instrument.processBlockBypassed (buffer, midi);
        CHECK (buffer.getMagnitude (0, 256) == 0.0f);
        CHECK (buffer.getMagnitude (1, 256) == 0.0f);
    }
}

TEST_CASE ("Monitor tone is audible only when enabled", "[plugin][audio]")
{
    const auto blockSize = GENERATE (1, 64, 512, 10000);
    CAPTURE (blockSize);

    SECTION ("Instrument: silent with Monitor Tone off, even while MIDI is sent")
    {
        VocalSynthProcessor instrument (Variant::instrument);
        setPattern (instrument, vocalsynth::TestPattern::notes);
        HostRunner host (instrument, 48000.0, blockSize);
        host.run (24000, blockSize);
        CHECK_FALSE (host.messages.empty());
        CHECK (host.maxOutput == 0.0f);
    }

    SECTION ("Instrument: the tone is its only output")
    {
        VocalSynthProcessor instrument (Variant::instrument);
        setPattern (instrument, vocalsynth::TestPattern::notes);
        setParameter (instrument, vocalsynth::params::monitorTone, 1.0f);
        HostRunner host (instrument, 48000.0, blockSize);
        host.run (24000, blockSize);
        CHECK (host.maxOutput > 0.2f);
        CHECK (host.maxOutput <= 0.26f); // about -12 dBFS, no sidechain leaking through
        CHECK (host.nonFiniteSamples == 0);
    }

    SECTION ("Instrument: Monitor Tone on but pattern Off is silent")
    {
        VocalSynthProcessor instrument (Variant::instrument);
        setPattern (instrument, vocalsynth::TestPattern::off);
        setParameter (instrument, vocalsynth::params::monitorTone, 1.0f);
        HostRunner host (instrument, 48000.0, blockSize);
        host.run (24000, blockSize);
        CHECK (host.maxOutput == 0.0f);
    }

    SECTION ("FX with Mute Dry: only the tone")
    {
        VocalSynthProcessor fx (Variant::fx);
        setPattern (fx, vocalsynth::TestPattern::notes);
        setParameter (fx, vocalsynth::params::muteDry, 1.0f);

        {
            HostRunner host (fx, 48000.0, blockSize);
            host.run (24000, blockSize);
            CHECK (host.maxOutput == 0.0f);
        }

        setParameter (fx, vocalsynth::params::monitorTone, 1.0f);
        HostRunner host (fx, 48000.0, blockSize);
        host.run (24000, blockSize);
        CHECK (host.maxOutput > 0.2f);
        CHECK (host.maxOutput <= 0.26f);
    }

    SECTION ("FX without Mute Dry: the tone is added to the voice")
    {
        VocalSynthProcessor fx (Variant::fx);
        setPattern (fx, vocalsynth::TestPattern::notes);
        setParameter (fx, vocalsynth::params::monitorTone, 1.0f);
        HostRunner host (fx, 48000.0, blockSize);
        host.run (24000, blockSize);
        CHECK (host.maxOutput > 0.5f); // voice peak 0.5 plus the tone
        CHECK (host.maxOutput <= 0.76f);
    }
}

TEST_CASE ("Switching Monitor Tone on during a held note is audible at once", "[plugin][audio]")
{
    VocalSynthProcessor instrument (Variant::instrument);
    setPattern (instrument, vocalsynth::TestPattern::pitchBend);
    HostRunner host (instrument, 48000.0, 480);
    host.run (24000, 480);
    REQUIRE (host.maxOutput == 0.0f);
    REQUIRE (instrument.getCurrentTestNote() == 60);

    setParameter (instrument, vocalsynth::params::monitorTone, 1.0f);
    host.run (960, 480); // 20 ms, within the same bar (no re-strike)
    CHECK (host.maxOutput > 0.1f);
}

TEST_CASE ("Restart pattern while the monitor tone sounds does not click", "[plugin][audio]")
{
    const auto blockSize = GENERATE (1, 64, 500);
    CAPTURE (blockSize);

    VocalSynthProcessor instrument (Variant::instrument);
    setPattern (instrument, vocalsynth::TestPattern::notes);
    setParameter (instrument, vocalsynth::params::monitorTone, 1.0f);
    HostRunner host (instrument, 48000.0, blockSize);
    host.run (15000, blockSize); // note 62 sounding at full level
    REQUIRE (instrument.getCurrentTestNote() == 62);

    // Record the last output sample before the flush and the whole block after it
    const auto lastBefore = host.buffer.getSample (0, host.buffer.getNumSamples() - 1);
    instrument.requestFlush();

    std::vector<float> after;
    after.push_back (lastBefore);
    for (int done = 0; done < 2400; done += blockSize)
    {
        host.block (blockSize);
        for (int i = 0; i < host.buffer.getNumSamples(); ++i)
            after.push_back (host.buffer.getSample (0, i));
    }

    CHECK (instrument.getCurrentTestNote() == 60);

    // A cut-off sine would jump by up to 0.25; a continuous one moves at most peak * 2 pi f / sr
    float maxJump = 0.0f;
    for (std::size_t i = 1; i < after.size(); ++i)
        maxJump = juce::jmax (maxJump, std::abs (after[i] - after[i - 1]));

    CHECK (maxJump < 0.25f * juce::MathConstants<float>::twoPi * 300.0f / 48000.0f);
    CHECK (host.maxOutput <= 0.26f);
}

TEST_CASE ("UI accessors report the processing state", "[plugin][ui]")
{
    VocalSynthProcessor p (Variant::fx);
    CHECK (p.getCurrentSampleRate() == 0.0);
    CHECK (p.getCurrentTestNote() == -1);

    setPattern (p, vocalsynth::TestPattern::notes);
    HostRunner host (p, 44100.0, 512);
    CHECK (p.getCurrentSampleRate() == 44100.0);

    host.run (512 * 10, 512);
    host.block (7);
    host.block (0);

    const auto h = p.getBlockSizeHistogram();
    CHECK (h.totalBlocks == 11);
    CHECK (h.lastBlockSize == 7);
    CHECK (h.minBlockSize == 7);
    CHECK (h.maxBlockSize == 512);
    CHECK (h.emptyBlocks == 1);

    p.resetBlockSizeHistogram();
    CHECK (p.getBlockSizeHistogram().totalBlocks == 0);

    CHECK (p.getCurrentTestNote() == 60);

    const auto pairing = checkPairing (host.messages);
    const auto sent = p.getMidiSentCounts();
    CHECK (sent.noteOns == (uint32_t) pairing.noteOns);
    CHECK (sent.noteOffs == (uint32_t) pairing.noteOffs);

    const auto settings = p.getTestPatternSettings();
    CHECK (settings.pattern == vocalsynth::TestPattern::notes);
    CHECK (settings.channel == 1);
    CHECK (settings.bpm == 120.0);
    CHECK (settings.ccNumber == 1);

    CHECK (p.getVirtualMidiStatus() == vocalsynth::VirtualMidiPort::Status::off);
    CHECK (p.getVirtualMidiOverflowCount() == 0);
}

TEST_CASE ("Parameter defaults", "[plugin][state]")
{
    VocalSynthProcessor p (Variant::fx);
    using namespace vocalsynth::params;

    CHECK (getPlainValue (p, testPattern) == 0.0f);
    CHECK (getPlainValue (p, midiChannel) == 1.0f);
    CHECK (getPlainValue (p, testTempo) == Catch::Approx (120.0f));
    CHECK (getPlainValue (p, ccNumber) == 1.0f);
    CHECK (getPlainValue (p, monitorTone) == 0.0f);
    CHECK (getPlainValue (p, virtualMidiOut) == 0.0f);
    CHECK (getPlainValue (p, muteDry) == 0.0f);
}

TEST_CASE ("State of the M1 parameters survives save and restore", "[plugin][state]")
{
    using namespace vocalsynth::params;
    const auto variant = GENERATE (Variant::fx, Variant::instrument);

    juce::MemoryBlock state;
    {
        VocalSynthProcessor p (variant);
        setPlainValue (p, testPattern, 3.0f);
        setPlainValue (p, midiChannel, 10.0f);
        setPlainValue (p, testTempo, 137.5f);
        setPlainValue (p, ccNumber, 74.0f);
        setPlainValue (p, monitorTone, 1.0f);
        setPlainValue (p, virtualMidiOut, 1.0f);
        p.getStateInformation (state);
    }

    VocalSynthProcessor restored (variant);
    restored.setStateInformation (state.getData(), (int) state.getSize());

    CHECK (getPlainValue (restored, testPattern) == 3.0f);
    CHECK (getPlainValue (restored, midiChannel) == 10.0f);
    CHECK (getPlainValue (restored, testTempo) == Catch::Approx (137.5f).margin (0.001));
    CHECK (getPlainValue (restored, ccNumber) == 74.0f);
    CHECK (getPlainValue (restored, monitorTone) == 1.0f);
    CHECK (getPlainValue (restored, virtualMidiOut) == 1.0f);

    const auto settings = restored.getTestPatternSettings();
    CHECK (settings.pattern == vocalsynth::TestPattern::controlChange);
    CHECK (settings.channel == 10);
    CHECK (settings.ccNumber == 74);
    CHECK (settings.bpm == Catch::Approx (137.5).margin (0.001));
}

TEST_CASE ("An M1 state (no M2 parameters) loads with Test Pattern and M2 defaults", "[plugin][state]")
{
    using namespace vocalsynth::params;
    const auto variant = GENERATE (Variant::fx, Variant::instrument);

    // What 0.1.x wrote: only the M1 parameters, no "source"
    const auto m1State = [] (int pattern)
    {
        juce::XmlElement xml ("PARAMS");
        const auto add = [&xml] (const char* id, double value)
        {
            auto* child = xml.createNewChildElement ("PARAM");
            child->setAttribute ("id", id);
            child->setAttribute ("value", value);
        };
        add ("muteDry", 1.0);
        add ("testPattern", (double) pattern);
        add ("midiChannel", 10.0);
        add ("testTempo", 137.5);
        add ("ccNumber", 74.0);
        add ("monitorTone", 1.0);
        add ("virtualMidiOut", 0.0);

        juce::MemoryBlock block;
        juce::AudioProcessor::copyXmlToBinary (xml, block);
        return block;
    };

    // The M2 parameters, which an M1 state must reset to their defaults
    const juce::ParameterID* m2Parameters[] = { &voiceRange, &pitchAlgorithm, &key, &scale, &stickiness,
                                                &gateThreshold, &minNoteLength, &octaveShift, &bendMode,
                                                &bendRange, &velocitySensitivity, &legato };

    const auto checkM2Defaults = [&] (VocalSynthProcessor& p)
    {
        for (const auto* id : m2Parameters)
        {
            CAPTURE (id->getParamID());
            auto* param = p.getParameters().getParameter (id->getParamID());
            REQUIRE (param != nullptr);
            CHECK (param->getValue() == Catch::Approx (param->getDefaultValue()).margin (1.0e-6));
        }
    };

    // Changed M2 settings (and source) before the load: the result must not depend on them
    const auto changeM2Settings = [] (VocalSynthProcessor& p)
    {
        setPlainValue (p, source, (float) static_cast<int> (Source::voice));
        setPlainValue (p, voiceRange, 0.0f); // Bass
        setPlainValue (p, key, 2.0f);        // D
        setPlainValue (p, scale, 2.0f);      // Natural Minor
        setPlainValue (p, stickiness, 90.0f);
        setPlainValue (p, octaveShift, -2.0f);
        setPlainValue (p, bendMode, 2.0f);
        setPlainValue (p, legato, 1.0f);
    };

    const auto changedBefore = GENERATE (false, true);
    CAPTURE (changedBefore);

    SECTION ("a saved test pattern keeps playing")
    {
        VocalSynthProcessor p (variant);
        if (changedBefore)
            changeM2Settings (p);

        const auto state = m1State (static_cast<int> (vocalsynth::TestPattern::notes));
        p.setStateInformation (state.getData(), (int) state.getSize());

        CHECK (getPlainValue (p, source) == 1.0f); // Test Pattern
        CHECK (getPlainValue (p, testPattern) == 1.0f);
        CHECK (getPlainValue (p, muteDry) == 1.0f);
        CHECK (getPlainValue (p, midiChannel) == 10.0f);
        CHECK (getPlainValue (p, testTempo) == Catch::Approx (137.5f).margin (0.001));
        CHECK (getPlainValue (p, ccNumber) == 74.0f);
        CHECK (getPlainValue (p, monitorTone) == 1.0f);
        CHECK (getPlainValue (p, virtualMidiOut) == 0.0f);
        checkM2Defaults (p);

        // ... and really plays: the first pattern note at the start
        HostRunner host (p, 48000.0, 512);
        host.run (4096, 512);
        CHECK (p.getCurrentTestNote() == 60);
        CHECK (p.getVoiceSnapshot().frameCounter == 0);
    }

    SECTION ("pattern Off selects Voice")
    {
        VocalSynthProcessor p (variant);
        if (changedBefore)
        {
            changeM2Settings (p);
            setPlainValue (p, source, (float) static_cast<int> (Source::testPattern));
        }

        const auto state = m1State (static_cast<int> (vocalsynth::TestPattern::off));
        p.setStateInformation (state.getData(), (int) state.getSize());

        CHECK (getPlainValue (p, source) == 0.0f); // Voice
        CHECK (getPlainValue (p, testPattern) == 0.0f);
        checkM2Defaults (p);
    }

    SECTION ("a state saved by this version is not migrated")
    {
        juce::MemoryBlock state;
        {
            VocalSynthProcessor p (variant);
            changeM2Settings (p); // source = Voice with the pattern on: kept as saved
            setPlainValue (p, testPattern, 1.0f);
            p.getStateInformation (state);
        }

        VocalSynthProcessor restored (variant);
        restored.setStateInformation (state.getData(), (int) state.getSize());
        CHECK (getPlainValue (restored, source) == 0.0f); // Voice
        CHECK (getPlainValue (restored, testPattern) == 1.0f);
        CHECK (getPlainValue (restored, voiceRange) == 0.0f);
        CHECK (getPlainValue (restored, key) == 2.0f);
        CHECK (getPlainValue (restored, scale) == 2.0f);
        CHECK (getPlainValue (restored, stickiness) == Catch::Approx (90.0f).margin (0.001));
        CHECK (getPlainValue (restored, octaveShift) == Catch::Approx (-2.0f));
        CHECK (getPlainValue (restored, bendMode) == 2.0f);
        CHECK (getPlainValue (restored, legato) == 1.0f);
    }
}

//==============================================================================
// M2 pitch -> MIDI integration (docs/M2-DESIGN.md 4.2). These tests are structural: they never depend on
// what the pitch detection finds (that is tested in the core tests), only on how the processor feeds the
// VoicePipeline and routes its events.
//==============================================================================

namespace
{

using vocalsynth::VoicePipeline;
using vocalsynth::VoiceRange;
namespace params = vocalsynth::params;

void setSource (VocalSynthProcessor& p, params::Source source)
{
    setPlainValue (p, params::source, (float) static_cast<int> (source));
}

/** Stereo test input: two tones with silence around them, different level per channel so the downmix
    (average of the channels) matters. The second tone (E4 + 28 cents) keeps a True-mode bend off centre. */
float voiceTestSignal (int channel, int64_t position, double sampleRate)
{
    const auto t = (double) position / sampleRate;
    const auto level = channel == 0 ? 0.4 : 0.2;
    double f = 0.0;

    if (t >= 0.05 && t < 0.45)
        f = 220.0;
    else if (t >= 0.6 && t < 0.9)
        f = 335.0;

    if (f <= 0.0)
        return 0.0f;

    return (float) (level * std::sin (juce::MathConstants<double>::twoPi * f * t));
}

/** VoiceSettings::operator== compares floats, which -Wfloat-equal (JUCE's warning set) flags in this TU. */
bool sameSettings (const vocalsynth::VoiceSettings& a, const vocalsynth::VoiceSettings& b)
{
    return a.range == b.range && a.algorithm == b.algorithm && a.key == b.key && a.scale == b.scale
           && juce::exactlyEqual (a.stickinessPercent, b.stickinessPercent)
           && juce::exactlyEqual (a.gateThresholdDb, b.gateThresholdDb)
           && juce::exactlyEqual (a.minNoteLengthMs, b.minNoteLengthMs) && a.octaveShift == b.octaveShift
           && a.bendMode == b.bendMode && a.bendRange == b.bendRange
           && juce::exactlyEqual (a.velocitySensitivityPercent, b.velocitySensitivityPercent)
           && a.legato == b.legato && a.channel == b.channel;
}

TimedMessage toTimedMessage (const juce::MidiMessage& m, int64_t position)
{
    TimedMessage t;
    t.position = position;
    t.size = juce::jmin (m.getRawDataSize(), 3);

    for (int i = 0; i < t.size; ++i)
        t.bytes[(size_t) i] = m.getRawData()[i];

    return t;
}

/** What the processor must send for a mono signal: the same VoicePipeline, fed directly in 4096-sample
    calls (the pipeline is block-size invariant), its events converted to MIDI at absolute positions. */
std::vector<TimedMessage> runReferencePipeline (double sampleRate, const vocalsynth::VoiceSettings& settings,
                                                int64_t totalSamples, vocalsynth::VoiceAnalysisSnapshot& snapshot)
{
    // processBlock runs with flush-to-zero (juce::ScopedNoDenormals); so must the reference
    juce::ScopedNoDenormals noDenormals;

    VoicePipeline pipeline;
    pipeline.prepare (sampleRate, 4096);

    std::vector<TimedMessage> messages;
    std::vector<float> mono (4096);
    vocalsynth::VsEventBuffer<1024> events;

    for (int64_t start = 0; start < totalSamples; start += 4096)
    {
        const auto n = (int) juce::jmin ((int64_t) 4096, totalSamples - start);

        for (int i = 0; i < n; ++i)
            mono[(size_t) i] = (voiceTestSignal (0, start + i, sampleRate) + voiceTestSignal (1, start + i, sampleRate))
                               * 0.5f;

        events.clear();
        pipeline.process (mono.data(), n, settings, events);

        for (const auto& e : events)
        {
            const auto position = start + e.sampleOffset;
            const auto ch = (int) e.channel;

            switch (e.type)
            {
                case vocalsynth::VsEventType::noteOn:
                    messages.push_back (toTimedMessage (juce::MidiMessage::noteOn (ch, e.data1, (juce::uint8) e.data2), position));
                    break;
                case vocalsynth::VsEventType::noteOff:
                    messages.push_back (toTimedMessage (juce::MidiMessage::noteOff (ch, e.data1), position));
                    break;
                case vocalsynth::VsEventType::pitchBend:
                    messages.push_back (toTimedMessage (juce::MidiMessage::pitchWheel (ch, e.bend + 8192), position));
                    break;
                case vocalsynth::VsEventType::controlChange:
                    messages.push_back (toTimedMessage (juce::MidiMessage::controllerEvent (ch, e.data1, e.data2), position));
                    break;
            }
        }
    }

    snapshot = pipeline.getSnapshot();
    return messages;
}

} // namespace

TEST_CASE ("Voice settings are built from the parameters", "[plugin][voice]")
{
    VocalSynthProcessor p (Variant::fx);

    SECTION ("defaults equal the core defaults (channel 1)")
    {
        CHECK (p.getSource() == params::Source::voice);
        CHECK (sameSettings (p.getVoiceSettings(), vocalsynth::VoiceSettings {}));
    }

    SECTION ("every parameter maps onto its field")
    {
        setPlainValue (p, params::voiceRange, 0.0f);
        setPlainValue (p, params::pitchAlgorithm, 0.0f);
        setPlainValue (p, params::key, 9.0f);
        setPlainValue (p, params::scale, 6.0f);
        setPlainValue (p, params::stickiness, 12.0f);
        setPlainValue (p, params::gateThreshold, -30.0f);
        setPlainValue (p, params::minNoteLength, 200.0f);
        setPlainValue (p, params::octaveShift, -2.0f);
        setPlainValue (p, params::bendMode, 2.0f);
        setPlainValue (p, params::bendRange, 1.0f);
        setPlainValue (p, params::velocitySensitivity, 25.0f);
        setPlainValue (p, params::legato, 1.0f);
        setPlainValue (p, params::midiChannel, 7.0f);
        setSource (p, params::Source::testPattern);

        const auto s = p.getVoiceSettings();
        CHECK (p.getSource() == params::Source::testPattern);
        CHECK (s.range == VoiceRange::bass);
        CHECK (s.algorithm == vocalsynth::PitchAlgorithm::yin);
        CHECK (s.key == 9);
        CHECK (s.scale == vocalsynth::Scale::blues);
        CHECK (s.stickinessPercent == Catch::Approx (12.0f).margin (0.01));
        CHECK (s.gateThresholdDb == Catch::Approx (-30.0f).margin (0.01));
        CHECK (s.minNoteLengthMs == Catch::Approx (200.0f).margin (0.01));
        CHECK (s.octaveShift == -2);
        CHECK (s.bendMode == vocalsynth::BendMode::trueBend);
        CHECK (s.bendRange == vocalsynth::BendRange::semitones12);
        CHECK (s.velocitySensitivityPercent == Catch::Approx (25.0f).margin (0.01));
        CHECK (s.legato);
        CHECK (s.channel == 7);
        CHECK (sameSettings (s, vocalsynth::sanitise (s)));
    }
}

TEST_CASE ("Analysis latency is reported to the UI, not to the host", "[plugin][voice]")
{
    VocalSynthProcessor p (Variant::fx);

    // Before the first prepareToPlay: 48 kHz, Tenor
    CHECK (juce::exactlyEqual (p.getVoiceLatencyMs(), VoicePipeline::computeAlgorithmicLatencyMs (48000.0, VoiceRange::tenor)));

    const auto sampleRate = GENERATE (44100.0, 48000.0, 96000.0);
    CAPTURE (sampleRate);
    p.prepareToPlay (sampleRate, 512);
    CHECK (p.getLatencySamples() == 0);

    for (int r = 0; r < vocalsynth::numVoiceRanges; ++r)
    {
        const auto range = static_cast<VoiceRange> (r);
        CAPTURE (r);
        setPlainValue (p, params::voiceRange, (float) r);
        const auto expected = VoicePipeline::computeAlgorithmicLatencyMs (sampleRate, range);
        CHECK (juce::exactlyEqual (p.getVoiceLatencyMs(), expected));
        CHECK (expected > 0.0);
    }

    CHECK (VoicePipeline::computeAlgorithmicLatencyMs (sampleRate, VoiceRange::tenor) <= VoicePipeline::tenorLatencyBudgetMs);
    CHECK (VoicePipeline::computeAlgorithmicLatencyMs (sampleRate, VoiceRange::bass) <= VoicePipeline::bassLatencyBudgetMs);
    CHECK (p.getLatencySamples() == 0);
}

TEST_CASE ("Voice mode sends exactly the VoicePipeline's events for the downmixed input", "[plugin][voice][realtime]")
{
    const auto sampleRate = GENERATE (44100.0, 48000.0, 96000.0);
    const auto variant = GENERATE (Variant::fx, Variant::instrument);
    const auto blockSize = GENERATE (1, 7, 64, 480, 512, 4096, 10000);
    CAPTURE (sampleRate, variant == Variant::fx ? "fx" : "instrument", blockSize);

    VocalSynthProcessor p (variant);
    setPlainValue (p, params::midiChannel, 5.0f);
    setPlainValue (p, params::bendMode, 2.0f);  // True: also routes pitch bends
    setPlainValue (p, params::bendRange, 1.0f); // +-12
    setPlainValue (p, params::monitorTone, 1.0f);
    setPattern (p, vocalsynth::TestPattern::all);
    setSource (p, params::Source::voice); // the pattern must be ignored in Voice mode

    // Ends while the second tone sounds, so the sections below switch with a voice note held
    const auto totalSamples = (int64_t) std::llround (0.8 * sampleRate);
    vocalsynth::VoiceAnalysisSnapshot expectedSnapshot;
    const auto expected = runReferencePipeline (sampleRate, p.getVoiceSettings(), totalSamples, expectedSnapshot);

    HostRunner host (p, sampleRate, blockSize);
    host.inputSignal = [sampleRate] (int ch, int64_t pos) { return voiceTestSignal (ch, pos, sampleRate); };
    host.run (totalSamples, blockSize);

    CHECK (host.badOffsets == 0);
    CHECK (host.unsortedEvents == 0);
    CHECK (host.nonFiniteSamples == 0);
    CHECK (host.messages == expected);
    CHECK (p.getCurrentTestNote() == -1);

    // The UI snapshot is the pipeline's state after the last analysed frame, whatever the block split
    const auto snapshot = p.getVoiceSnapshot();
    CHECK (snapshot.frameCounter == expectedSnapshot.frameCounter);
    CHECK (snapshot.frameCounter > 0);
    CHECK (juce::exactlyEqual (snapshot.frequencyHz, expectedSnapshot.frequencyHz));
    CHECK (juce::exactlyEqual (snapshot.confidence, expectedSnapshot.confidence));
    CHECK (juce::exactlyEqual (snapshot.rmsDb, expectedSnapshot.rmsDb));
    CHECK (snapshot.voiced == expectedSnapshot.voiced);
    CHECK (snapshot.activeNote == expectedSnapshot.activeNote);

    const auto r = checkPairing (host.messages);
    CHECK (r.duplicateNoteOns == 0);
    CHECK (r.orphanNoteOffs == 0);
    CHECK (r.controlChanges == 0); // the CC sweep of the pattern is not running

    for (const auto& m : host.messages)
        CHECK (m.toMidiMessage().getChannel() == 5);

    // Both tones gave a note; the second one is held at the end, with the True bend off centre
    REQUIRE (r.noteOns == 2);
    REQUIRE (r.stillActive == 1);
    REQUIRE_FALSE (r.bendCentredAtEnd);
    const auto heldNote = snapshot.activeNote;
    REQUIRE (heldNote == 64);

    // Monitor Tone follows the sung notes: silent before the first note-on, audible after it (the dry
    // voice is muted in the Instrument; in the FX it passes through, so only check the Instrument)
    if (variant == Variant::instrument)
    {
        const auto firstNoteOn = std::find_if (host.messages.begin(), host.messages.end(),
                                               [] (const TimedMessage& t) { return t.toMidiMessage().isNoteOn(); });
        REQUIRE (firstNoteOn != host.messages.end());
        CHECK (host.maxOutput > 0.0f);
        CHECK (host.firstAudiblePosition >= firstNoteOn->position);
    }

    SECTION ("switching to Test Pattern ends every voice note and starts the pattern at the block start")
    {
        setSource (p, params::Source::testPattern);
        host.block (blockSize);

        const auto blockStart = host.position - blockSize;
        bool seenNoteOn = false, seenVoiceNoteOff = false;

        for (const auto& t : host.lastBlock)
        {
            const auto m = t.toMidiMessage();

            if (! seenNoteOn)
            {
                CHECK (t.position == blockStart);
                CHECK ((m.isNoteOff() || m.isPitchWheel() || m.isNoteOn()));
                seenVoiceNoteOff = seenVoiceNoteOff || (m.isNoteOff() && m.getNoteNumber() == heldNote);
            }

            if (m.isNoteOn() && ! seenNoteOn)
            {
                seenNoteOn = true;
                CHECK (m.getNoteNumber() == 60); // the "all" pattern restarted from its beginning
            }
        }

        CHECK (seenNoteOn);
        CHECK (seenVoiceNoteOff); // the held voice note ended before the pattern started

        if (blockSize < 9000) // the first note lasts 9000 samples at 120 bpm
            CHECK (p.getCurrentTestNote() == 60);

        setPattern (p, vocalsynth::TestPattern::off);
        host.block (blockSize);
        const auto after = checkPairing (host.messages);
        CHECK (after.stillActive == 0);
        CHECK (after.duplicateNoteOns == 0);
        CHECK (after.orphanNoteOffs == 0);
        CHECK (after.bendCentredAtEnd);
    }

    SECTION ("bypass ends every voice note")
    {
        host.block (blockSize, true);
        CHECK (host.lastBlock.size() == 2); // note-off of the held note + bend centre
        const auto after = checkPairing (host.messages);
        CHECK (after.stillActive == 0);
        CHECK (after.orphanNoteOffs == 0);
        CHECK (after.bendCentredAtEnd);
        CHECK (p.getVoiceSnapshot().activeNote == -1);
        CHECK_FALSE (p.getVoiceSnapshot().voiced);
    }
}

TEST_CASE ("Voice mode with silence sends no MIDI", "[plugin][voice][realtime]")
{
    const auto sampleRate = GENERATE (44100.0, 48000.0, 96000.0);
    const auto blockSize = GENERATE (1, 7, 64, 480, 512, 4096, 10000);
    CAPTURE (sampleRate, blockSize);

    VocalSynthProcessor p (Variant::instrument);
    setPlainValue (p, params::monitorTone, 1.0f);
    setPlainValue (p, params::testPattern, (float) static_cast<int> (vocalsynth::TestPattern::notes)); // ignored

    HostRunner host (p, sampleRate, blockSize);
    host.silentInput = true;
    host.run (host.seconds (1.0), blockSize);

    CHECK (host.messages.empty());
    CHECK (host.maxOutput == 0.0f);
    CHECK (p.getMidiSentCounts().noteOns == 0);
    CHECK (p.getCurrentTestNote() == -1);

    // The analysis runs (frames are counted) and reports silence
    const auto snapshot = p.getVoiceSnapshot();
    CHECK (snapshot.frameCounter > 0);
    CHECK_FALSE (snapshot.voiced);
    CHECK (snapshot.activeNote == -1);
    CHECK (snapshot.frequencyHz == 0.0f);
}

TEST_CASE ("Test Pattern mode does not run the voice analysis", "[plugin][voice]")
{
    VocalSynthProcessor p (Variant::fx);
    setPattern (p, vocalsynth::TestPattern::notes);
    HostRunner host (p, 48000.0, 512);
    host.run (18000, 512); // 220 Hz sine input; the second note (62) sounds from 12000 to 21000

    CHECK (p.getVoiceSnapshot().frameCounter == 0);
    CHECK (p.getCurrentTestNote() == 62);
}

TEST_CASE ("Switching from Test Pattern to Voice flushes the pattern notes", "[plugin][voice][midi]")
{
    const auto blockSize = GENERATE (1, 64, 512, 10000);
    CAPTURE (blockSize);

    VocalSynthProcessor p (Variant::fx);
    setPattern (p, vocalsynth::TestPattern::pitchBend);
    HostRunner host (p, 48000.0, blockSize);
    host.run (2560, blockSize); // note 60 held, bend > 0
    REQUIRE (p.getCurrentTestNote() == 60);
    REQUIRE_FALSE (checkPairing (host.messages).bendCentredAtEnd);

    setSource (p, params::Source::voice);
    host.silentInput = true;
    host.block (blockSize);

    const auto& b = host.lastBlock;
    REQUIRE (b.size() == 2);
    const auto blockStart = host.position - blockSize;
    CHECK (b[0].toMidiMessage().isNoteOff());
    CHECK (b[0].toMidiMessage().getNoteNumber() == 60);
    CHECK (b[0].position == blockStart);
    CHECK (b[1].toMidiMessage().isPitchWheel());
    CHECK (b[1].toMidiMessage().getPitchWheelValue() == 8192);
    CHECK (b[1].position == blockStart);
    CHECK (p.getCurrentTestNote() == -1);

    // Silence in Voice mode: nothing more
    const auto count = host.messages.size();
    host.run (host.seconds (0.5), blockSize);
    CHECK (host.messages.size() == count);

    const auto r = checkPairing (host.messages);
    CHECK (r.stillActive == 0);
    CHECK (r.bendCentredAtEnd);
}

TEST_CASE ("Voice settings changes and restarts never leave a note hanging", "[plugin][voice][midi]")
{
    const auto blockSize = GENERATE (7, 480, 4096);
    CAPTURE (blockSize);

    VocalSynthProcessor p (Variant::fx);
    setPlainValue (p, params::bendMode, 1.0f); // Anchored
    HostRunner host (p, 48000.0, blockSize);

    // G3 with +-60 cents vibrato at 5.5 Hz: one held note (stickiness 0.7 semitones), and Anchored bends
    // beyond the 30-cent dead zone, so changes also happen while the bend is off centre
    host.inputSignal = [] (int, int64_t pos)
    {
        const auto t = (double) pos / 48000.0;
        const auto twoPi = juce::MathConstants<double>::twoPi;
        const auto depth = std::exp2 (60.0 / 1200.0) - 1.0;
        return 0.3f * (float) std::sin (twoPi * 196.0 * (t + depth * (1.0 - std::cos (twoPi * 5.5 * t)) / (twoPi * 5.5)));
    };

    // Changes that flush in the pipeline (range, key, scale, algorithm, channel) and ones that do not
    const std::function<void()> changes[] = {
        [&] { setPlainValue (p, params::voiceRange, 0.0f); },
        [&] { setPlainValue (p, params::key, 2.0f); },
        [&] { setPlainValue (p, params::scale, 1.0f); },
        [&] { setPlainValue (p, params::pitchAlgorithm, 0.0f); },
        [&] { setPlainValue (p, params::midiChannel, 3.0f); },
        [&] { setPlainValue (p, params::octaveShift, 2.0f); },
        [&] { setPlainValue (p, params::legato, 1.0f); },
        [&] { setPlainValue (p, params::gateThreshold, -60.0f); },
        [&] { p.reset(); },
        [&] { p.prepareToPlay (48000.0, blockSize); },
        [&] { p.requestFlush(); },
    };

    int changesWithBendOffCentre = 0;

    const auto requireHeldNote = [&]
    {
        host.run (host.seconds (0.25), blockSize);
        REQUIRE (p.getVoiceSnapshot().activeNote >= 0); // every change below happens during a note
        REQUIRE (checkPairing (host.messages).stillActive == 1);
        changesWithBendOffCentre += checkPairing (host.messages).bendCentredAtEnd ? 0 : 1;
    };

    for (const auto& change : changes)
    {
        requireHeldNote();
        change();
    }

    requireHeldNote();
    setSource (p, params::Source::testPattern); // pattern Off: flushes and stays silent
    host.block (blockSize);

    CHECK (host.badOffsets == 0);
    CHECK (host.unsortedEvents == 0);

    const auto r = checkPairing (host.messages);
    CHECK (r.duplicateNoteOns == 0);
    CHECK (r.orphanNoteOffs == 0);
    CHECK (r.stillActive == 0);
    CHECK (r.bendCentredAtEnd);
    CHECK (r.noteOns == r.noteOffs);
    CHECK (r.pitchBends > 0);
    CHECK (changesWithBendOffCentre > 0);
    CHECK (p.getVoiceSnapshot().activeNote == -1);
}

namespace
{

/** Sung-like A3: band-limited saw at 220 Hz (harmonics 1/k up to 5 kHz) with 5.5 Hz vibrato of +-30 cents,
    20 ms attack and release ramps, from toneStart to toneEnd seconds; silence around it. The FX gets it on
    both main input channels, the Instrument on both sidechain channels (its only input bus). */
constexpr double sungToneStart = 0.10;
constexpr double sungToneEnd = 0.70;

float sungSaw (int64_t position, double sampleRate)
{
    const auto t = (double) position / sampleRate;

    if (t < sungToneStart || t >= sungToneEnd)
        return 0.0f;

    constexpr double f0 = 220.0, vibratoHz = 5.5, ramp = 0.02;
    const auto depth = std::exp2 (30.0 / 1200.0) - 1.0;
    const auto twoPi = juce::MathConstants<double>::twoPi;
    const auto u = t - sungToneStart;
    const auto phase = twoPi * f0 * (u + depth * (1.0 - std::cos (twoPi * vibratoHz * u)) / (twoPi * vibratoHz));

    double saw = 0.0;
    for (int k = 1; k * f0 < 5000.0; ++k)
        saw += std::sin ((double) k * phase) / (double) k;

    const auto envelope = juce::jmin (1.0, u / ramp, (sungToneEnd - t) / ramp);
    return (float) (0.2 * envelope * saw);
}

} // namespace

TEST_CASE ("End to end: a sung A3 in Voice mode / Tenor gives note 57 on and off", "[plugin][voice][midi]")
{
    const auto variant = GENERATE (Variant::fx, Variant::instrument);
    const auto sampleRate = GENERATE (44100.0, 48000.0);
    CAPTURE (variant == Variant::fx ? "fx" : "instrument", sampleRate);

    std::vector<TimedMessage> first;

    for (const auto blockSize : { 64, 480 })
    {
        CAPTURE (blockSize);

        VocalSynthProcessor p (variant);
        setSource (p, params::Source::voice);
        setPlainValue (p, params::voiceRange, (float) static_cast<int> (VoiceRange::tenor));

        HostRunner host (p, sampleRate, blockSize);
        host.inputSignal = [sampleRate] (int, int64_t pos) { return sungSaw (pos, sampleRate); };
        host.run (host.seconds (1.2), blockSize); // 0.5 s of silence after the tone

        CHECK (host.badOffsets == 0);
        CHECK (host.unsortedEvents == 0);

        std::vector<TimedMessage> notes;
        for (const auto& m : host.messages)
            if (m.toMidiMessage().isNoteOnOrOff())
                notes.push_back (m);

        REQUIRE (notes.size() == 2);
        const auto on = notes[0].toMidiMessage();
        const auto off = notes[1].toMidiMessage();

        CHECK (on.isNoteOn());
        CHECK (on.getNoteNumber() == 57);
        CHECK (on.getChannel() == 1);
        CHECK (on.getVelocity() > 0);
        CHECK (off.isNoteOff());
        CHECK (off.getNoteNumber() == 57);

        // Note-on within the Tenor latency budget (spec: <= 30 ms), note-off after the tone has ended
        const auto onsetMs = 1000.0 * ((double) notes[0].position / sampleRate - sungToneStart);
        const auto releaseMs = 1000.0 * ((double) notes[1].position / sampleRate - sungToneEnd);
        CAPTURE (onsetMs, releaseMs);
        CHECK (onsetMs > 0.0);
        CHECK (onsetMs <= 30.0);
        CHECK (releaseMs > -20.0); // the release ramp starts 20 ms before toneEnd
        CHECK (releaseMs < 100.0);

        const auto r = checkPairing (host.messages);
        CHECK (r.stillActive == 0);
        CHECK (r.pitchBends == 0); // Bend Mode Off (default)
        CHECK (p.getVoiceSnapshot().activeNote == -1);

        // Block-size invariant: both block sizes give the same MIDI at the same absolute positions
        if (first.empty())
            first = host.messages;
        else
            CHECK (host.messages == first);
    }
}

TEST_CASE ("Monitor tone bends are rescaled to the selected bend range", "[plugin][voice][audio]")
{
    using vocalsynth::monitorBendFor;

    CHECK (monitorBendFor (0, 12) == 0);
    CHECK (monitorBendFor (1234, 2) == 1234);   // +-2: unchanged (test patterns, Bend Range +-2)
    CHECK (monitorBendFor (-8192, 2) == -8192);
    CHECK (monitorBendFor (683, 12) == 4098);   // ~1 semitone at +-12 -> ~1 semitone at +-2
    CHECK (monitorBendFor (-683, 12) == -4098);
    CHECK (monitorBendFor (171, 48) == 4104);   // ~1 semitone at +-48
    CHECK (monitorBendFor (2000, 12) == 8191);  // > 2 semitones: clamped
    CHECK (monitorBendFor (-2000, 24) == -8192);
}

//==============================================================================
// v0.2.1: own DSP load and recent host block size for the UI

TEST_CASE ("DspLoadMeter averages over time and lets the peak decay", "[plugin][ui]")
{
    vocalsynth::DspLoadMeter meter;
    CHECK_FALSE (meter.getReading().valid);

    meter.prepare (48000.0);
    meter.addBlock (0.0, 480); // 0 % is a valid measurement
    CHECK (meter.getReading().valid);
    CHECK (meter.getReading().averageProportion == 0.0f);

    meter.requestReset();
    CHECK_FALSE (meter.getReading().valid);

    // 10 ms blocks at 10 % load: the first block after the reset sets the values directly
    meter.addBlock (0.001, 480);
    CHECK (meter.getReading().averageProportion == Catch::Approx (0.1f));
    CHECK (meter.getReading().peakProportion == Catch::Approx (0.1f));

    // One second of idle blocks: average falls to 1/e, the peak only by exp(-1/3)
    for (int i = 0; i < 100; ++i)
        meter.addBlock (0.0, 480);

    CHECK (meter.getReading().averageProportion
           == Catch::Approx (0.1 * std::exp (-1.0 / vocalsynth::DspLoadMeter::averageTimeConstantSeconds)).epsilon (0.01));
    CHECK (meter.getReading().peakProportion
           == Catch::Approx (0.1 * std::exp (-1.0 / vocalsynth::DspLoadMeter::peakDecayTimeConstantSeconds)).epsilon (0.01));

    // A single slow block shows in the peak at once
    meter.addBlock (0.01, 480);
    CHECK (meter.getReading().peakProportion == Catch::Approx (1.0f));
    CHECK (meter.getReading().averageProportion < 0.1f);

    // The peak never falls below the average, also with a fluctuating load (seen on a CI runner):
    // a slowly decaying high load followed by blocks just below the decayed peak
    {
        vocalsynth::DspLoadMeter m;
        m.prepare (48000.0);
        m.addBlock (0.005, 480); // 50 %
        bool peakBelowAverage = false;

        for (int i = 0; i < 2000; ++i)
        {
            const auto r = m.getReading();
            m.addBlock (0.0099 * (double) r.peakProportion, 480); // load = 99 % of the current peak
            const auto after = m.getReading();
            peakBelowAverage = peakBelowAverage || after.peakProportion < after.averageProportion;
        }

        CHECK_FALSE (peakBelowAverage);
    }

    // The averaging does not depend on the block size: 1 s of 1-sample blocks equals 1 s of 4800-sample blocks
    vocalsynth::DspLoadMeter tiny, large;
    tiny.prepare (48000.0);
    large.prepare (48000.0);
    tiny.addBlock (0.0, 1);
    large.addBlock (0.0, 4800);
    for (int i = 0; i < 48000; ++i)
        tiny.addBlock (0.05 / 48000.0, 1);
    for (int i = 0; i < 10; ++i)
        large.addBlock (0.05 * 0.1, 4800);
    CHECK (tiny.getReading().averageProportion == Catch::Approx (large.getReading().averageProportion).epsilon (0.02));

    // Nonsense input is ignored
    meter.addBlock (-1.0, 480);
    meter.addBlock (0.001, 0);
    CHECK (meter.getReading().peakProportion == Catch::Approx (1.0f));
}

TEST_CASE ("RecentBlockSize follows the largest recent host block", "[plugin][ui]")
{
    vocalsynth::RecentBlockSize recent;
    recent.prepare (48000.0);
    CHECK (recent.get() == 0);

    recent.record (0); // empty blocks do not count
    CHECK (recent.get() == 0);

    for (int i = 0; i < 10; ++i)
        recent.record (i % 3 == 0 ? 2048 : 512); // FL splitting its buffer
    CHECK (recent.get() == 2048);

    // After the buffer was made smaller, the display follows within two windows
    for (int i = 0; i < 2 * 48000 / 256 + 2; ++i)
        recent.record (256);
    CHECK (recent.get() == 256);
}

TEST_CASE ("The processor reports its DSP load and recent block size", "[plugin][ui]")
{
    const auto variant = GENERATE (Variant::fx, Variant::instrument);
    const auto source = GENERATE (0, 1); // Voice, Test Pattern
    CAPTURE (variant == Variant::fx ? "fx" : "instrument", source);

    VocalSynthProcessor p (variant);
    setParameter (p, vocalsynth::params::source, (float) source);
    p.prepareToPlay (48000.0, 512);
    CHECK_FALSE (p.getDspLoad().valid);
    CHECK (p.getRecentMaxBlockSize() == 0);

    juce::AudioBuffer<float> buffer (2, 512);
    juce::MidiBuffer midi;
    for (int pos = 0; pos < 48000; pos += 512)
    {
        fillWithSine (buffer, 48000.0, pos);
        midi.clear();
        p.processBlock (buffer, midi);
    }

    const auto load = p.getDspLoad();
    CHECK (load.valid);
    CHECK (load.averageProportion > 0.0f);
    CHECK (load.averageProportion < 1.0f); // far below real time, even in a Debug build
    CHECK (load.peakProportion >= load.averageProportion);
    CHECK (std::isfinite (load.peakProportion));
    CHECK (p.getRecentMaxBlockSize() == 512);

    // Bypassed blocks are measured too; prepareToPlay starts afresh
    p.processBlockBypassed (buffer, midi);
    CHECK (p.getDspLoad().valid);
    p.resetDspLoad();
    CHECK_FALSE (p.getDspLoad().valid);
    p.prepareToPlay (44100.0, 2048);
    CHECK (p.getRecentMaxBlockSize() == 0);
    p.releaseResources();
}

//==============================================================================
// M3 capture in the processor (docs/M3-DESIGN.md 2, 4, 15, 16). The worker is stepped manually
// (setManualSteppingForTests), so nothing races the test thread.
//==============================================================================

namespace
{
namespace captureE2E
{
constexpr double rate = 48000.0;
constexpr int preparedBlock = 2048;
constexpr int64_t transportStart = 61440;          // multiple of lcm (480, 2048): same blocks for both sizes
constexpr int64_t transportStop = 61440 + 122880;  // 5.12 beats at 120 BPM
constexpr int64_t samplesPerBeat = 24000;           // 120 BPM at 48 kHz
constexpr int64_t roundTrip = 2 * preparedBlock;    // what the sound card adds (Auto = 2 x the prepared block)

std::vector<vocalsynth::test::SungNote> phrase()
{
    // Quarter notes A3 (220 Hz = MIDI 57) on beats 1-4 of the bar where the transport starts
    std::vector<vocalsynth::test::SungNote> notes;

    for (int beat = 0; beat < 4; ++beat)
        notes.push_back ({ transportStart + beat * samplesPerBeat, 16800, 220.0 });

    return notes;
}

struct Result
{
    std::vector<std::shared_ptr<const vocalsynth::Take>> takes;
    juce::MemoryBlock midiBytes;
    juce::File midiFile;
};

/** Sings the phrase with the transport playing from bar 5 (ppq 16), voice arriving roundTrip late. */
Result singPhrase (Variant variant, int blockSize, params::Source source = params::Source::voice)
{
    VocalSynthProcessor p (variant);
    p.getCaptureWorker().setManualSteppingForTests (true);
    setPlainValue (p, params::source, (float) static_cast<int> (source));
    setPlainValue (p, params::voiceRange, (float) static_cast<int> (vocalsynth::VoiceRange::tenor));

    vocalsynth::test::CaptureHost host (p, p.getCaptureWorker(), rate, preparedBlock);
    host.playHead.bpm = 120.0;
    host.playHead.playStartSample = transportStart;
    host.playHead.playStopSample = transportStop;
    host.playHead.playStartPpq = 16.0; // bar 5 in 4/4

    const auto notes = phrase();
    host.input = [notes] (int64_t pos) { return vocalsynth::test::sungPhrase (notes, pos, rate, roundTrip); };
    host.runTo (transportStop + (int64_t) rate, blockSize); // 1 s after Stop: the capture tail (0.55 s) is over
    host.steps (4);

    Result r;
    r.takes = p.getCaptureWorker().getTakes();

    if (! r.takes.empty())
    {
        const auto folder = juce::File::createTempFile ("vs-capture-e2e");
        r.midiFile = vocalsynth::CaptureExporter::writeMidi (*r.takes.front(), p.getRenderOptionsFor (*r.takes.front()),
                                                             vocalsynth::SmfFormat::type1, folder);
        r.midiFile.loadFileAsData (r.midiBytes);
    }

    return r;
}
} // namespace captureE2E
} // namespace

TEST_CASE ("Capture end to end: a sung phrase with the transport playing gives one take at the right bar",
           "[plugin][capture][voice]")
{
    using namespace captureE2E;
    const auto variant = GENERATE (Variant::fx, Variant::instrument);
    CAPTURE (variant == Variant::fx ? "fx" : "instrument");

    const auto r = singPhrase (variant, preparedBlock);
    REQUIRE (r.takes.size() == 1);
    const auto& take = *r.takes.front();
    CHECK (take.mode == vocalsynth::TakeMode::synced);
    CHECK (take.kind == vocalsynth::TakeKind::automatic);
    CHECK (take.number == 1);
    CHECK (take.maxBlockSize == preparedBlock);
    CHECK (take.sampleRate == Catch::Approx (rate));
    CHECK (take.observedBlockSize == preparedBlock);
    CHECK (take.startSample == transportStart);
    CHECK (take.endSample == transportStop);
    CHECK_FALSE (take.incomplete);
    CHECK (take.audio == vocalsynth::TakeAudio::full);
    CHECK (take.noteCount == 4);
    CHECK (take.wallClockStartMs > 0);

    // The file: bar number in the name (decision 1), Clean by default, readable by juce::MidiFile
    REQUIRE (r.midiFile.existsAsFile());
    CHECK (r.midiFile.getFileName().contains ("_Take01_Bar005_Clean.mid"));
    juce::MidiFile midiFile;
    REQUIRE (vocalsynth::test::readMidiFile (r.midiFile, midiFile));
    CHECK (midiFile.getTimeFormat() == 960);

    // Notes on the beats: the sound card's round trip (Auto, 2 x 2048) and the detection delay are removed
    const auto notes = vocalsynth::test::notesOf (midiFile);
    REQUIRE (notes.size() == 4);

    for (std::size_t i = 0; i < notes.size(); ++i)
    {
        CAPTURE (i, notes[i].onTick, notes[i].offTick);
        CHECK (notes[i].note == 57);
        CHECK (notes[i].channel == 1);
        CHECK (notes[i].onTick == Catch::Approx (960.0 * (double) i).margin (12.0)); // 6 ms at 120 BPM
        CHECK (notes[i].offTick > notes[i].onTick + 500.0);
        CHECK (notes[i].offTick < notes[i].onTick + 960.0);
    }

    r.midiFile.getParentDirectory().deleteRecursively();
}

TEST_CASE ("Capture end to end: the exported .mid does not depend on the host block size", "[plugin][capture][realtime]")
{
    using namespace captureE2E;
    const auto reference = singPhrase (Variant::fx, preparedBlock);
    REQUIRE (reference.takes.size() == 1);

    for (const auto blockSize : { 480, 64 })
    {
        CAPTURE (blockSize);
        const auto r = singPhrase (Variant::fx, blockSize);
        REQUIRE (r.takes.size() == 1);
        CHECK (r.midiBytes.getSize() > 0);
        CHECK (r.midiBytes == reference.midiBytes);
        r.midiFile.getParentDirectory().deleteRecursively();
    }

    reference.midiFile.getParentDirectory().deleteRecursively();
}

TEST_CASE ("Capture end to end: the WAV starts at tick 0 and is kept in the takes folder", "[plugin][capture][audio]")
{
    using namespace captureE2E;
    VocalSynthProcessor p (Variant::fx);
    p.getCaptureWorker().setManualSteppingForTests (true);

    const auto takesFolder = juce::File::createTempFile ("vs-takes");
    auto state = p.getCaptureState();
    state.takesFolder = takesFolder.getFullPathName();
    p.setCaptureState (state);

    vocalsynth::test::CaptureHost host (p, p.getCaptureWorker(), rate, preparedBlock);
    host.playHead.playStartSample = transportStart;
    host.playHead.playStopSample = transportStop;
    host.playHead.playStartPpq = 16.0;
    const auto notes = phrase();
    host.input = [notes] (int64_t pos) { return vocalsynth::test::sungPhrase (notes, pos, rate, roundTrip); };
    host.runTo (transportStop + (int64_t) rate, preparedBlock);

    auto& worker = p.getCaptureWorker();
    const auto takes = worker.getTakes();
    REQUIRE (takes.size() == 1);
    const auto number = takes.front()->number;

    // The worker writes the cache in slices of CaptureWorker::wavFramesPerStep
    for (int i = 0; i < 20 && worker.getWavState (number).kind == vocalsynth::TakeWavState::Kind::preparing; ++i)
        worker.runStepForTests();

    const auto cached = worker.getWavState (number);
    REQUIRE (cached.kind == vocalsynth::TakeWavState::Kind::ready);
    CHECK (cached.file.getParentDirectory() == worker.getInstanceFolder());
    CHECK (cached.file.getFileName().endsWith ("_Take01_Bar005_Voice.wav"));
    CHECK (worker.getInstanceFolder().getChildFile (vocalsynth::CaptureExporter::heartbeatFileName).existsAsFile());

    double wavRate = 0.0;
    int bits = 0;
    const auto audio = vocalsynth::test::readWav (cached.file, wavRate, bits);
    CHECK (wavRate == Catch::Approx (rate));
    CHECK (bits == 24);
    REQUIRE (audio.getNumChannels() == 1);

    const auto span = vocalsynth::TakeRenderer::computeAudioSpan (*takes.front(), p.getRenderOptionsFor (*takes.front()));
    CHECK (audio.getNumSamples() == span.numSamples);

    // WAV sample 0 = tick 0 = beat 1 of bar 5: the first sung note starts there, the second one beat later
    const auto firstLoud = [&audio] (int from)
    {
        for (int i = from; i < audio.getNumSamples(); ++i)
            if (std::abs (audio.getSample (0, i)) > 1.0e-3f)
                return i;

        return -1;
    };

    CHECK (firstLoud (0) >= 0);
    CHECK (firstLoud (0) < 100);
    CHECK (std::abs (firstLoud (20000) - (int) samplesPerBeat) < 100);

    // Dragging moves it into the takes folder (FL references dropped WAVs); a second drag reuses it
    const auto persisted = worker.persistWav (number, takesFolder);
    REQUIRE (persisted.existsAsFile());
    CHECK (persisted.getParentDirectory() == takesFolder);
    CHECK_FALSE (cached.file.exists());
    CHECK (worker.getWavState (number).kind == vocalsynth::TakeWavState::Kind::persisted);
    CHECK (worker.persistWav (number, takesFolder) == persisted);

    host.p.releaseResources();
    takesFolder.deleteRecursively();
}

namespace
{
namespace captureMelody
{
// Integration test of M3 0.3.0: FX, Voice mode (Tenor), a sung melody A3 C4 E4 A3 at 120 BPM with the host
// playhead starting at bar 3, the voice arriving 2 x 2048 samples late (Auto offset from the prepared block),
// host blocks 2048 / 480 / 64. Everything is real code: VoicePipeline, CaptureTap, worker, renderer, exporter.
constexpr double rate = 48000.0;
constexpr int preparedBlock = 2048;
constexpr int64_t samplesPerBeat = 24000;            // 120 BPM at 48 kHz
constexpr double samplesPerTick = 24000.0 / 960.0;   // 25 samples per tick at 960 PPQ
constexpr int64_t transportStart = 61440;            // multiple of lcm (64, 480, 2048): same transport per block size
constexpr int64_t transportStop = transportStart + 245760; // 10.24 beats
constexpr int64_t roundTrip = 2 * preparedBlock;
constexpr double sixtyFourthTicks = 960.0 / 16.0;   // a 1/64 note = 1/16 beat

struct Intended
{
    double beat; // beats after the start of bar 3
    int note;
    double frequencyHz;
    int64_t lengthSamples;
};

// Beat 2, 3 and 4 of bar 3 and beat 1 of bar 4: the take starts at the bar line (bar 3), the first note one beat in
constexpr std::array<Intended, 4> melody { { { 1.0, 57, 220.0, 18000 },
                                             { 2.0, 60, 261.6255653005986, 18000 },
                                             { 3.0, 64, 329.6275569128699, 18000 },
                                             { 4.0, 57, 220.0, 36000 } } };

struct Result
{
    std::size_t numTakes = 0;
    juce::String midiName, wavName;
    juce::MemoryBlock midiBytes;
    std::vector<vocalsynth::test::FileNote> notes;
    juce::AudioBuffer<float> wav;
    int64_t spanSamples = -1;
    double wavRate = 0.0;
};

Result sing (int blockSize)
{
    VocalSynthProcessor p (Variant::fx);
    p.getCaptureWorker().setManualSteppingForTests (true);
    setPlainValue (p, vocalsynth::params::source, (float) static_cast<int> (vocalsynth::params::Source::voice));
    setPlainValue (p, vocalsynth::params::voiceRange, (float) static_cast<int> (vocalsynth::VoiceRange::tenor));
    REQUIRE (p.getCaptureState() == vocalsynth::CaptureState {}); // defaults: Clean, no quantize, Auto offset

    vocalsynth::test::CaptureHost host (p, p.getCaptureWorker(), rate, preparedBlock);
    host.playHead.bpm = 120.0;
    host.playHead.playStartSample = transportStart;
    host.playHead.playStopSample = transportStop;
    host.playHead.playStartPpq = 8.0; // bar 3 in 4/4

    std::vector<vocalsynth::test::SungNote> sung;
    for (const auto& n : melody)
        sung.push_back ({ transportStart + (int64_t) (n.beat * (double) samplesPerBeat), n.lengthSamples, n.frequencyHz });

    host.input = [sung] (int64_t pos) { return vocalsynth::test::sungPhrase (sung, pos, rate, roundTrip); };
    host.runTo (transportStop + (int64_t) rate, blockSize);
    host.steps (4);

    auto& worker = p.getCaptureWorker();
    const auto takes = worker.getTakes();
    Result r;
    r.numTakes = takes.size();

    if (takes.size() != 1)
        return r;

    const auto& take = *takes.front();
    const auto folder = juce::File::createTempFile ("vs-capture-melody");
    const auto midiFile = vocalsynth::CaptureExporter::writeMidi (take, p.getRenderOptionsFor (take),
                                                                  vocalsynth::SmfFormat::type1, folder);
    r.midiName = midiFile.getFileName();
    midiFile.loadFileAsData (r.midiBytes);
    juce::MidiFile parsed;

    if (vocalsynth::test::readMidiFile (midiFile, parsed) && parsed.getTimeFormat() == 960)
        r.notes = vocalsynth::test::notesOf (parsed);

    folder.deleteRecursively();

    for (int i = 0; i < 40 && worker.getWavState (take.number).kind == vocalsynth::TakeWavState::Kind::preparing; ++i)
        worker.runStepForTests();

    const auto wavState = worker.getWavState (take.number);

    if (wavState.kind == vocalsynth::TakeWavState::Kind::ready)
    {
        r.wavName = wavState.file.getFileName();
        int bits = 0;
        r.wav = vocalsynth::test::readWav (wavState.file, r.wavRate, bits);
        r.spanSamples = vocalsynth::TakeRenderer::computeAudioSpan (take, p.getRenderOptionsFor (take)).numSamples;
    }

    p.releaseResources();
    return r;
}

/** First sample at or after from whose magnitude exceeds the threshold (-1: none). */
int firstLoud (const juce::AudioBuffer<float>& audio, int from)
{
    for (int i = juce::jmax (0, from); i < audio.getNumSamples(); ++i)
        if (std::abs (audio.getSample (0, i)) > 1.0e-3f)
            return i;

    return -1;
}
} // namespace captureMelody
} // namespace

TEST_CASE ("Capture end to end: a sung melody from bar 3 exports in order, on the beats, block-size invariant",
           "[plugin][capture][voice][e2e]")
{
    using namespace captureMelody;
    const auto reference = sing (preparedBlock);
    REQUIRE (reference.numTakes == 1);
    CHECK (reference.midiName.contains ("_Take01_Bar003_Clean.mid"));
    CHECK (reference.wavName.contains ("_Take01_Bar003_Voice.wav"));

    // The notes in order, at the intended beats after removing the round trip and the detection delay
    REQUIRE (reference.notes.size() == melody.size());

    for (std::size_t i = 0; i < melody.size(); ++i)
    {
        const auto& n = reference.notes[i];
        const auto intendedTick = melody[i].beat * 960.0;
        CAPTURE (i, n.note, n.onTick, n.offTick, intendedTick);
        CHECK (n.note == melody[i].note);
        CHECK (n.channel == 1);
        CHECK (std::abs (n.onTick - intendedTick) <= sixtyFourthTicks);
        CHECK (n.offTick > n.onTick);

        if (i > 0)
            CHECK (n.onTick > reference.notes[i - 1].onTick);
    }

    // The WAV: same origin (sample 0 = tick 0 = bar 3 beat 1), length = audio span, each sung note where the
    // MIDI note is (within a 1/64 note)
    REQUIRE (reference.wav.getNumChannels() == 1);
    CHECK (reference.wavRate == Catch::Approx (rate));
    CHECK ((int64_t) reference.wav.getNumSamples() == reference.spanSamples);

    int searchFrom = 0;

    for (std::size_t i = 0; i < melody.size(); ++i)
    {
        const auto onset = firstLoud (reference.wav, searchFrom);
        const auto intended = melody[i].beat * (double) samplesPerBeat;
        CAPTURE (i, onset, intended, reference.notes[i].onTick * samplesPerTick);
        REQUIRE (onset >= 0);
        CHECK (std::abs ((double) onset - intended) < 100.0); // the dry voice is only shifted by the round trip
        CHECK (std::abs ((double) onset - reference.notes[i].onTick * samplesPerTick) <= sixtyFourthTicks * samplesPerTick);
        searchFrom = onset + (int) melody[i].lengthSamples + 100;
    }

    // Byte-identical .mid and identical WAV audio for host blocks 480 and 64
    for (const auto blockSize : { 480, 64 })
    {
        CAPTURE (blockSize);
        const auto r = sing (blockSize);
        REQUIRE (r.numTakes == 1);
        CHECK (r.midiBytes.getSize() > 0);
        CHECK (r.midiBytes == reference.midiBytes);
        REQUIRE (r.wav.getNumSamples() == reference.wav.getNumSamples());

        CHECK (std::memcmp (r.wav.getReadPointer (0), reference.wav.getReadPointer (0),
                            sizeof (float) * (std::size_t) r.wav.getNumSamples())
               == 0);
    }
}

TEST_CASE ("Capture in Test Pattern mode records the pattern MIDI as sent", "[plugin][capture][midi]")
{
    using namespace captureE2E;
    VocalSynthProcessor p (Variant::fx);
    p.getCaptureWorker().setManualSteppingForTests (true);
    setPattern (p, vocalsynth::TestPattern::notes);
    setPlainValue (p, params::captureAudio, 0.0f);

    vocalsynth::test::CaptureHost host (p, p.getCaptureWorker(), rate, 512);
    host.playHead.playStartSample = 0;
    host.playHead.playStopSample = 4 * 4 * samplesPerBeat; // 4 bars
    host.runTo (host.playHead.playStopSample + (int64_t) rate, 512);

    const auto takes = p.getCaptureWorker().getTakes();
    REQUIRE (takes.size() == 1);
    const auto& take = *takes.front();
    CHECK (take.settings.testPattern);
    CHECK (take.settings.bendMode == vocalsynth::BendMode::trueBend);
    CHECK (take.audio == vocalsynth::TakeAudio::none);
    CHECK (p.getCaptureWorker().getWavState (take.number).kind == vocalsynth::TakeWavState::Kind::unavailable);

    // Every note-on the output sent while the transport played is in the take (Raw = what was sent)
    int capturedOns = 0;

    for (const auto& e : take.events)
        if (e.event.type == vocalsynth::VsEventType::noteOn && ! e.synthetic)
            ++capturedOns;

    CHECK (capturedOns > 0);
    CHECK (capturedOns <= host.noteOns);

    auto options = p.getRenderOptionsFor (take);
    options.version = vocalsynth::ExportVersion::raw;
    const auto rendered = vocalsynth::TakeRenderer::render (take, options);
    CHECK (rendered.originBar == 1);

    // Notes sent in the capture tail after Stop (and so after the take's end in host time) are not exported
    CHECK (rendered.noteCount <= capturedOns);
    CHECK (rendered.noteCount >= capturedOns - 3);

    // The pattern is generated at the block's playhead (no input latency): every note-on sent while the
    // transport played sits exactly where FL played it, not a round trip (2 x 512) earlier
    REQUIRE (options.roundTripSamples == 1024);
    std::vector<int64_t> expected;

    for (const auto& e : take.events)
        if (e.event.type == vocalsynth::VsEventType::noteOn && e.event.data2 > 0 && ! e.synthetic && e.sample < take.endSample)
            expected.push_back (std::llround ((double) (e.sample - take.startSample) / (double) samplesPerBeat * 960.0));

    std::vector<int64_t> exported;

    for (const auto& e : rendered.events)
        if (e.type == vocalsynth::TickEventType::noteOn)
            exported.push_back (e.tick);

    std::sort (expected.begin(), expected.end());
    CHECK (exported == expected);
}

TEST_CASE ("Capture off records nothing; capture on again starts recording", "[plugin][capture]")
{
    using namespace captureE2E;
    VocalSynthProcessor p (Variant::fx);
    p.getCaptureWorker().setManualSteppingForTests (true);
    setPattern (p, vocalsynth::TestPattern::notes);
    setPlainValue (p, params::captureEnabled, 0.0f);

    vocalsynth::test::CaptureHost host (p, p.getCaptureWorker(), rate, 512);
    host.runTo ((int64_t) (6.0 * rate), 512); // free mode: notes, then nothing

    CHECK (host.noteOns > 0);
    CHECK (p.getCaptureWorker().getTakes().empty());
    CHECK (p.getCaptureWorker().getStatus().state == vocalsynth::TakeSegmenter::State::off);

    setPlainValue (p, params::captureEnabled, 1.0f);
    host.runTo (host.position + (int64_t) (2.0 * rate), 512);
    CHECK (p.getCaptureWorker().getStatus().state == vocalsynth::TakeSegmenter::State::freeRecording);

    // Pattern off: the gap (3 s) closes the free take
    setPattern (p, vocalsynth::TestPattern::off);
    host.runTo (host.position + (int64_t) (5.0 * rate), 512);
    const auto takes = p.getCaptureWorker().getTakes();
    REQUIRE (takes.size() == 1);
    CHECK (takes.front()->mode == vocalsynth::TakeMode::free);
}

TEST_CASE ("Capture: bypassed blocks are silence in the take, the flush note-offs are recorded", "[plugin][capture]")
{
    using namespace captureE2E;
    VocalSynthProcessor p (Variant::fx);
    p.getCaptureWorker().setManualSteppingForTests (true);
    setPattern (p, vocalsynth::TestPattern::notes);

    vocalsynth::test::CaptureHost host (p, p.getCaptureWorker(), rate, 512);
    host.playHead.playStartSample = 0;
    host.playHead.playStopSample = (int64_t) (4.0 * rate);
    host.runTo ((int64_t) (1.0 * rate), 512);

    // Bypass for half a second, then on again
    for (int i = 0; i < 47; ++i)
        host.block (512, true);

    host.runTo (host.playHead.playStopSample + (int64_t) rate, 512);

    const auto takes = p.getCaptureWorker().getTakes();
    REQUIRE (takes.size() == 1);
    const auto& take = *takes.front();
    CHECK_FALSE (take.incomplete); // a bypass is no data loss
    CHECK (take.audio == vocalsynth::TakeAudio::full);

    // Paired: every note-on of the take has its note-off (the bypass flush ended the held note)
    std::array<int, 128> open {};
    for (const auto& e : take.events)
    {
        if (e.event.type == vocalsynth::VsEventType::noteOn && e.event.data2 > 0)
            ++open[e.event.data1];
        else if (e.event.type == vocalsynth::VsEventType::noteOff || e.event.type == vocalsynth::VsEventType::noteOn)
            open[e.event.data1] = juce::jmax (0, open[e.event.data1] - 1);
    }

    CHECK (std::all_of (open.begin(), open.end(), [] (int n) { return n == 0; }));
}

TEST_CASE ("Capture: prepareToPlay at the same rate keeps the history, a new rate clears it", "[plugin][capture]")
{
    using namespace captureE2E;
    VocalSynthProcessor p (Variant::fx);
    p.getCaptureWorker().setManualSteppingForTests (true);
    setPattern (p, vocalsynth::TestPattern::notes);

    {
        vocalsynth::test::CaptureHost host (p, p.getCaptureWorker(), rate, 512);
        host.playHead.playStartSample = 0;
        host.playHead.playStopSample = (int64_t) (2.0 * rate);
        host.runTo ((int64_t) (3.0 * rate), 512);
    }

    REQUIRE (p.getCaptureWorker().getTakes().size() == 1);

    // FL restarts its engine (same rate, new buffer): takes stay
    p.releaseResources();
    p.prepareToPlay (rate, 1024);
    p.getCaptureWorker().runStepForTests();
    CHECK (p.getCaptureWorker().getTakes().size() == 1);

    // A new sample rate restarts the clock; listed takes stay (they own their data)
    p.prepareToPlay (44100.0, 1024);
    p.getCaptureWorker().runStepForTests();
    CHECK (p.getCaptureWorker().getTakes().size() == 1);
}

TEST_CASE ("Capture: 10 minutes of capture keep the overflow counters at 0", "[plugin][capture][long]")
{
    using namespace captureE2E;
    const auto source = GENERATE (params::Source::testPattern, params::Source::voice);
    CAPTURE ((int) source);

    VocalSynthProcessor p (Variant::fx);
    p.getCaptureWorker().setManualSteppingForTests (true);
    setPlainValue (p, params::takeMaxBars, 0.0f); // Off: one 10-minute take (the default cuts after 16 bars)

    if (source == params::Source::testPattern)
        setPattern (p, vocalsynth::TestPattern::all); // notes, bends and CCs
    else
        setPlainValue (p, params::bendMode, (float) static_cast<int> (vocalsynth::BendMode::trueBend));

    vocalsynth::test::CaptureHost host (p, p.getCaptureWorker(), rate, 512);
    host.stepEverySamples = 480; // the worker drains every 10 ms
    host.playHead.playStartSample = 0;
    host.playHead.playStopSample = (int64_t) (600.0 * rate);

    // Voice: a slow vibrato tone, sung in 2 s phrases (bends all the time in True mode)
    host.input = [] (int64_t pos)
    {
        const auto t = (double) pos / rate;

        if (std::fmod (t, 2.0) > 1.6)
            return 0.0f;

        const auto f = 220.0 * std::exp2 (0.3 * std::sin (juce::MathConstants<double>::twoPi * 0.7 * t) / 12.0);
        return (float) (0.2 * std::sin (juce::MathConstants<double>::twoPi * f * t));
    };

    host.runTo (host.playHead.playStopSample + (int64_t) rate, 512);

    const auto status = p.getCaptureWorker().getStatus();
    CHECK (status.overflow.records == 0);
    CHECK (status.overflow.audioSamples == 0);

    const auto takes = p.getCaptureWorker().getTakes();
    REQUIRE (takes.size() == 1);
    CHECK (takes.front()->mode == vocalsynth::TakeMode::synced);
    CHECK_FALSE (takes.front()->incomplete);
    CHECK (takes.front()->noteCount > 100);
    CHECK (takes.front()->audio == vocalsynth::TakeAudio::full);
    CHECK (status.audioHistoryBytes <= vocalsynth::CaptureHistory::maxAudioBytes);
    CHECK (status.audioHistoryBytes >= (std::size_t) (590.0 * rate) * sizeof (float));
}

TEST_CASE ("Processor CPU sanity check with capture on: worst 128-sample block at 48 kHz", "[plugin][capture][cpu]")
{
    // Spec (docs/M3-SPEC.md, Tests): with the capture tap running (Voice, Wide, True bends, audio on,
    // transport playing) the worst block stays < 20 % of a 128-sample block at 48 kHz (Release). Per block
    // the minimum over 3 runs (a preemption hits one run, not all); Debug builds only report.
    constexpr double sr = 48000.0;
    constexpr int blockSize = 128;
    constexpr int numBlocks = (int) (4.0 * sr) / blockSize;
    std::vector<double> best ((std::size_t) numBlocks, 1.0e9);
    juce::Random noise (11);

    for (int run = 0; run < 3; ++run)
    {
        VocalSynthProcessor p (Variant::fx);
        p.getCaptureWorker().setManualSteppingForTests (true);
        setPlainValue (p, params::source, (float) static_cast<int> (params::Source::voice));
        setPlainValue (p, params::voiceRange, (float) static_cast<int> (vocalsynth::VoiceRange::wide));
        setPlainValue (p, params::bendMode, (float) static_cast<int> (vocalsynth::BendMode::trueBend));
        REQUIRE (p.isCaptureEnabled());
        REQUIRE (p.isCaptureAudioEnabled());

        vocalsynth::test::FakePlayHead head;
        head.sampleRate = sr;
        head.playStartSample = 0;
        p.setPlayHead (&head);
        p.setRateAndBufferSizeDetails (sr, blockSize);
        p.prepareToPlay (sr, blockSize);

        juce::AudioBuffer<float> buffer (2, blockSize);
        juce::MidiBuffer midi;

        for (int b = 0; b < numBlocks; ++b)
        {
            const auto pos = (int64_t) b * blockSize;

            for (int i = 0; i < blockSize; ++i)
            {
                // 65 Hz saw (longest window, every frame voiced) + a little noise, as the pipeline check
                const auto phase = std::fmod ((double) (pos + i) * 65.0 / sr, 1.0);
                const auto x = (float) (0.3 * (2.0 * phase - 1.0)) + 0.01f * (noise.nextFloat() * 2.0f - 1.0f);
                buffer.setSample (0, i, x);
                buffer.setSample (1, i, x);
            }

            head.blockStart = pos;
            midi.clear();
            const auto t0 = std::chrono::steady_clock::now();
            p.processBlock (buffer, midi);
            const auto t1 = std::chrono::steady_clock::now();
            best[(std::size_t) b] = juce::jmin (best[(std::size_t) b], std::chrono::duration<double> (t1 - t0).count());

            if (b % 4 == 3) // the worker drains every ~10 ms (outside the measured block)
                p.getCaptureWorker().runStepForTests();
        }

        p.setPlayHead (nullptr);
        CHECK (p.getCaptureWorker().getStatus().overflow.records == 0);
        CHECK (p.getCaptureWorker().getStatus().overflow.audioSamples == 0);
    }

    const auto blockSeconds = blockSize / sr;
    double worst = 0.0, mean = 0.0;

    for (const auto t : best)
    {
        worst = juce::jmax (worst, t / blockSeconds);
        mean += t / blockSeconds;
    }

    mean /= (double) numBlocks;
    std::printf ("[processor][cpu] Voice Wide True bends + capture, 48 kHz, 128-sample blocks: worst %.2f %%, mean %.2f %% of the block\n",
                 100.0 * worst, 100.0 * mean);

#ifdef NDEBUG
    CHECK (worst < 0.20);
    CHECK (mean < 0.10);
#endif
}

TEST_CASE ("Capture worker: many instances start and stop quickly and leave no files (pluginval-like)",
           "[plugin][capture][lifecycle]")
{
    const auto start = juce::Time::getMillisecondCounterHiRes();
    juce::StringArray folders;

    for (int i = 0; i < 40; ++i)
    {
        VocalSynthProcessor p (i % 2 == 0 ? Variant::fx : Variant::instrument);
        CHECK_FALSE (p.getCaptureWorker().isRunning()); // no thread before prepareToPlay
        const auto sampleRate = i % 3 == 0 ? 44100.0 : 48000.0;
        p.prepareToPlay (sampleRate, 512);
        CHECK (p.getCaptureWorker().isRunning());

        juce::AudioBuffer<float> buffer (2, 512);
        juce::MidiBuffer midi;
        buffer.clear();

        for (int b = 0; b < 8; ++b)
            p.processBlock (buffer, midi);

        p.releaseResources();
        CHECK (p.getCaptureWorker().isRunning()); // survives FL's engine stop
        p.prepareToPlay (96000.0, 256);           // new rate: suspend, reallocate, resume
        p.processBlock (buffer, midi);
        p.releaseResources();
        folders.add (p.getCaptureWorker().getInstanceFolder().getFullPathName());
    }

    const auto elapsedMs = juce::Time::getMillisecondCounterHiRes() - start;
    CAPTURE (elapsedMs);
    CHECK (elapsedMs < 20000.0);

    for (const auto& f : folders)
        CHECK_FALSE (juce::File (f).exists());
}
