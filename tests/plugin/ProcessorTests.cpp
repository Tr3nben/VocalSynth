#include "Parameters.h"
#include "PluginProcessor.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <cmath>
#include <cstdint>
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

void setPattern (VocalSynthProcessor& p, vocalsynth::TestPattern pattern)
{
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

    /** Processes one block of n samples. The input is a quiet 220 Hz sine unless silentInput. */
    void block (int n, bool bypassed = false)
    {
        buffer.setSize (2, n, false, false, true);

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < n; ++i)
                buffer.setSample (ch, i,
                                  silentInput ? 0.0f
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

    juce::AudioBuffer<float> buffer;
    juce::MidiBuffer midi;
    int64_t position = 0;

    std::vector<TimedMessage> messages;
    std::vector<TimedMessage> lastBlock;
    int badOffsets = 0;
    int unsortedEvents = 0;
    int nonFiniteSamples = 0;
    float maxOutput = 0.0f;
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
