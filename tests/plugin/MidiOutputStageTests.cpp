#include "MidiOutputStage.h"
#include "VirtualMidiPort.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <juce_events/juce_events.h>

#include <algorithm>
#include <array>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace vocalsynth;

namespace
{
    struct TimedMessage
    {
        int offset;
        juce::MidiMessage message;
    };

    std::vector<TimedMessage> collect (const juce::MidiBuffer& buffer)
    {
        std::vector<TimedMessage> result;
        for (const auto metadata : buffer)
            result.push_back ({ metadata.samplePosition, metadata.getMessage() });
        return result;
    }

    template <std::size_t N>
    VsEventBuffer<N> makeEvents (std::initializer_list<VsEvent> list)
    {
        VsEventBuffer<N> events;
        for (const auto& e : list)
            REQUIRE (events.push (e));
        return events;
    }

    /** Runs the message loop until pred() is true or timeoutMs elapsed. Returns pred(). */
    template <typename Pred>
    bool pumpUntil (Pred&& pred, int timeoutMs)
    {
        const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;

        while (! pred() && juce::Time::getMillisecondCounter() < end)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (5);

        return pred();
    }
} // namespace

//==============================================================================
TEST_CASE ("MidiOutputStage converts every event type", "[midi][output]")
{
    MidiOutputStage stage;
    juce::MidiBuffer midi;

    const auto events = makeEvents<16> ({
        VsEvent::makeNoteOn (0, 1, 60, 100),
        VsEvent::makePitchBend (1, 2, -8192),
        VsEvent::makePitchBend (2, 2, 0),
        VsEvent::makePitchBend (3, 2, 8191),
        VsEvent::makeControlChange (4, 16, 7, 127),
        VsEvent::makeNoteOff (5, 1, 60),
    });

    stage.write (events, 64, midi);
    const auto out = collect (midi);
    REQUIRE (out.size() == 6);

    CHECK (out[0].offset == 0);
    CHECK (out[0].message.isNoteOn());
    CHECK (out[0].message.getChannel() == 1);
    CHECK (out[0].message.getNoteNumber() == 60);
    CHECK (out[0].message.getVelocity() == 100);

    CHECK (out[1].message.isPitchWheel());
    CHECK (out[1].message.getChannel() == 2);
    CHECK (out[1].message.getPitchWheelValue() == 0);
    CHECK (out[2].message.getPitchWheelValue() == 8192);
    CHECK (out[3].message.getPitchWheelValue() == 16383);

    CHECK (out[4].offset == 4);
    CHECK (out[4].message.isController());
    CHECK (out[4].message.getChannel() == 16);
    CHECK (out[4].message.getControllerNumber() == 7);
    CHECK (out[4].message.getControllerValue() == 127);

    CHECK (out[5].offset == 5);
    CHECK (out[5].message.isNoteOff());
    CHECK (out[5].message.getNoteNumber() == 60);

    CHECK (stage.getNumActiveNotes() == 0);
}

TEST_CASE ("MidiOutputStage clamps offsets into the chunk and adds startOffset", "[midi][output]")
{
    MidiOutputStage stage;

    SECTION ("offsets clamped to [0, numSamples - 1]")
    {
        juce::MidiBuffer midi;
        const auto events = makeEvents<8> ({
            VsEvent::makeControlChange (-5, 1, 1, 1),
            VsEvent::makeControlChange (0, 1, 1, 2),
            VsEvent::makeControlChange (63, 1, 1, 3),
            VsEvent::makeControlChange (100, 1, 1, 4),
        });

        stage.write (events, 64, midi);
        const auto out = collect (midi);
        REQUIRE (out.size() == 4);
        CHECK (out[0].offset == 0);
        CHECK (out[1].offset == 0);
        CHECK (out[2].offset == 63);
        CHECK (out[3].offset == 63);

        // Order of events is kept (CC values 1..4)
        for (size_t i = 0; i < out.size(); ++i)
            CHECK (out[i].message.getControllerValue() == (int) i + 1);
    }

    SECTION ("chunk start offset")
    {
        juce::MidiBuffer midi;
        const auto events = makeEvents<4> ({
            VsEvent::makeNoteOn (-1, 1, 60, 90),
            VsEvent::makeNoteOff (4095, 1, 60),
            VsEvent::makeNoteOn (5000, 1, 62, 90),
        });

        stage.write (events, 4096, midi, 4096);
        const auto out = collect (midi);
        REQUIRE (out.size() == 3);
        CHECK (out[0].offset == 4096);
        CHECK (out[1].offset == 8191);
        CHECK (out[2].offset == 8191);
    }

    SECTION ("one-sample chunk")
    {
        juce::MidiBuffer midi;
        const auto events = makeEvents<4> ({ VsEvent::makeNoteOn (3, 1, 60, 90) });
        stage.write (events, 1, midi, 7);
        const auto out = collect (midi);
        REQUIRE (out.size() == 1);
        CHECK (out[0].offset == 7);
    }

    SECTION ("an empty chunk writes nothing and changes no state")
    {
        juce::MidiBuffer midi;
        const auto events = makeEvents<4> ({ VsEvent::makeNoteOn (0, 1, 60, 90) });
        stage.write (events, 0, midi);
        stage.write (events, -3, midi);
        CHECK (midi.isEmpty());
        CHECK (stage.getNumActiveNotes() == 0);
        CHECK (stage.getSentCounts().noteOns == 0);
    }
}

TEST_CASE ("MidiOutputStage pairs note-ons and note-offs", "[midi][output]")
{
    MidiOutputStage stage;
    juce::MidiBuffer midi;

    SECTION ("duplicate note-on gets a note-off first")
    {
        const auto events = makeEvents<4> ({
            VsEvent::makeNoteOn (0, 3, 64, 100),
            VsEvent::makeNoteOn (10, 3, 64, 80),
        });
        stage.write (events, 32, midi);
        const auto out = collect (midi);
        REQUIRE (out.size() == 3);
        CHECK (out[0].message.isNoteOn());
        CHECK (out[1].offset == 10);
        CHECK (out[1].message.isNoteOff());
        CHECK (out[1].message.getNoteNumber() == 64);
        CHECK (out[2].offset == 10);
        CHECK (out[2].message.isNoteOn());
        CHECK (out[2].message.getVelocity() == 80);
        CHECK (stage.isNoteActive (3, 64));
        CHECK (stage.getNumActiveNotes() == 1);
    }

    SECTION ("note-off for an inactive note is dropped")
    {
        const auto events = makeEvents<4> ({
            VsEvent::makeNoteOff (0, 1, 60),
            VsEvent::makeNoteOn (1, 1, 60, 100),
            VsEvent::makeNoteOff (2, 2, 60), // other channel: not active there
            VsEvent::makeNoteOff (3, 1, 60),
        });
        stage.write (events, 32, midi);
        const auto out = collect (midi);
        REQUIRE (out.size() == 2);
        CHECK (out[0].message.isNoteOn());
        CHECK (out[1].message.isNoteOff());
        CHECK (out[1].offset == 3);
        CHECK (out[1].message.getChannel() == 1);
        CHECK (stage.getNumActiveNotes() == 0);
    }

    SECTION ("note-on with velocity 0 is a note-off")
    {
        const auto events = makeEvents<4> ({
            VsEvent::makeNoteOn (0, 1, 60, 0), // nothing active: dropped
            VsEvent::makeNoteOn (1, 1, 60, 100),
            VsEvent::makeNoteOn (2, 1, 60, 0),
        });
        stage.write (events, 32, midi);
        const auto out = collect (midi);
        REQUIRE (out.size() == 2);
        CHECK (out[0].message.isNoteOn());
        CHECK (out[1].message.isNoteOff());
        CHECK_FALSE (stage.isNoteActive (1, 60));
    }

    SECTION ("note state persists across blocks")
    {
        stage.write (makeEvents<2> ({ VsEvent::makeNoteOn (5, 1, 72, 100) }), 16, midi);
        CHECK (stage.isNoteActive (1, 72));

        midi.clear();
        stage.write (makeEvents<2> ({ VsEvent::makeNoteOff (0, 1, 72) }), 16, midi);
        const auto out = collect (midi);
        REQUIRE (out.size() == 1);
        CHECK (out[0].message.isNoteOff());
        CHECK_FALSE (stage.isNoteActive (1, 72));
    }
}

TEST_CASE ("MidiOutputStage clamps channel and data values", "[midi][output]")
{
    MidiOutputStage stage;
    juce::MidiBuffer midi;

    const auto events = makeEvents<4> ({
        VsEvent::makeNoteOn (0, 0, 200, 255),   // channel 0 -> 1, note 200 -> 127, velocity -> 127
        VsEvent::makeControlChange (1, 17, 130, 200), // channel 17 -> 16, cc 130 -> 127, value -> 127
        VsEvent::makePitchBend (2, 99, -32768), // -> channel 16, -8192
    });

    stage.write (events, 8, midi);
    const auto out = collect (midi);
    REQUIRE (out.size() == 3);

    CHECK (out[0].message.getChannel() == 1);
    CHECK (out[0].message.getNoteNumber() == 127);
    CHECK (out[0].message.getVelocity() == 127);
    CHECK (stage.isNoteActive (1, 127));

    CHECK (out[1].message.getChannel() == 16);
    CHECK (out[1].message.getControllerNumber() == 127);
    CHECK (out[1].message.getControllerValue() == 127);

    CHECK (out[2].message.getChannel() == 16);
    CHECK (out[2].message.getPitchWheelValue() == 0);
    CHECK_FALSE (stage.isBendCentred (16));

    // Queries with out-of-range arguments are safe
    CHECK_FALSE (stage.isNoteActive (0, 60));
    CHECK_FALSE (stage.isNoteActive (17, 60));
    CHECK_FALSE (stage.isNoteActive (1, -1));
    CHECK_FALSE (stage.isNoteActive (1, 128));
    CHECK (stage.isBendCentred (0));
    CHECK (stage.isBendCentred (17));
}

TEST_CASE ("MidiOutputStage flushAllNotes ends every note and centres the bend", "[midi][output]")
{
    MidiOutputStage stage;
    juce::MidiBuffer midi;

    const auto events = makeEvents<8> ({
        VsEvent::makeNoteOn (0, 1, 60, 100),
        VsEvent::makeNoteOn (0, 1, 67, 100),
        VsEvent::makeNoteOn (0, 5, 48, 100),
        VsEvent::makePitchBend (1, 5, 4000),
        VsEvent::makePitchBend (1, 9, 100),
        VsEvent::makePitchBend (2, 9, 0), // channel 9 centred again
        VsEvent::makeNoteOn (3, 16, 127, 1),
    });
    stage.write (events, 16, midi);
    CHECK (stage.getNumActiveNotes() == 4);
    CHECK_FALSE (stage.isBendCentred (5));
    CHECK (stage.isBendCentred (9));

    juce::MidiBuffer flushMidi;
    stage.flushAllNotes (7, flushMidi);
    const auto out = collect (flushMidi);

    int noteOffs = 0;
    int bendCentres = 0;
    for (const auto& m : out)
    {
        CHECK (m.offset == 7);

        if (m.message.isNoteOff())
        {
            ++noteOffs;
            CHECK (m.message.getVelocity() == 0);
        }
        else if (m.message.isPitchWheel())
        {
            ++bendCentres;
            CHECK (m.message.getChannel() == 5);
            CHECK (m.message.getPitchWheelValue() == 8192);
        }
        else
        {
            FAIL ("unexpected message in flush: " << m.message.getDescription());
        }
    }

    CHECK (noteOffs == 4);
    CHECK (bendCentres == 1);
    CHECK (stage.getNumActiveNotes() == 0);
    for (int ch = 1; ch <= 16; ++ch)
        CHECK (stage.isBendCentred (ch));

    // Channel 5: the note-off comes before the bend centre
    for (size_t i = 0; i < out.size(); ++i)
        if (out[i].message.isPitchWheel())
            for (size_t j = i + 1; j < out.size(); ++j)
                CHECK_FALSE ((out[j].message.isNoteOff() && out[j].message.getChannel() == 5));

    SECTION ("a second flush emits nothing")
    {
        juce::MidiBuffer again;
        stage.flushAllNotes (0, again);
        CHECK (again.isEmpty());
    }

    SECTION ("a note-off from the generator after a flush is dropped")
    {
        juce::MidiBuffer after;
        stage.write (makeEvents<2> ({ VsEvent::makeNoteOff (0, 1, 60) }), 16, after);
        CHECK (after.isEmpty());
    }
}

TEST_CASE ("MidiOutputStage flush edge cases", "[midi][output]")
{
    MidiOutputStage stage;

    SECTION ("nothing active -> nothing emitted")
    {
        juce::MidiBuffer midi;
        stage.flushAllNotes (0, midi);
        CHECK (midi.isEmpty());
        CHECK (stage.getSentCounts().noteOffs == 0);
    }

    SECTION ("negative offset is clamped to 0")
    {
        juce::MidiBuffer midi;
        stage.write (makeEvents<2> ({ VsEvent::makeNoteOn (0, 1, 60, 100) }), 4, midi);
        midi.clear();
        stage.flushAllNotes (-10, midi);
        const auto out = collect (midi);
        REQUIRE (out.size() == 1);
        CHECK (out[0].offset == 0);
        CHECK (out[0].message.isNoteOff());
    }

    SECTION ("bend only")
    {
        juce::MidiBuffer midi;
        stage.write (makeEvents<2> ({ VsEvent::makePitchBend (0, 3, -1) }), 4, midi);
        midi.clear();
        stage.flushAllNotes (0, midi);
        const auto out = collect (midi);
        REQUIRE (out.size() == 1);
        CHECK (out[0].message.isPitchWheel());
        CHECK (out[0].message.getChannel() == 3);
        CHECK (out[0].message.getPitchWheelValue() == 8192);
    }

    SECTION ("all 2048 notes on all channels")
    {
        juce::MidiBuffer midi;
        VsEventBuffer<2048> events;
        for (int ch = 1; ch <= 16; ++ch)
            for (int note = 0; note < 128; ++note)
                REQUIRE (events.push (VsEvent::makeNoteOn (0, (uint8_t) ch, (uint8_t) note, 100)));

        stage.write (events, 1, midi);
        CHECK (stage.getNumActiveNotes() == 2048);

        juce::MidiBuffer flushMidi;
        stage.flushAllNotes (0, flushMidi);
        CHECK (flushMidi.getNumEvents() == 2048);
        CHECK (stage.getNumActiveNotes() == 0);
    }
}

TEST_CASE ("MidiOutputStage reset forgets state without emitting", "[midi][output]")
{
    MidiOutputStage stage;
    juce::MidiBuffer midi;

    stage.write (makeEvents<4> ({ VsEvent::makeNoteOn (0, 1, 60, 100), VsEvent::makePitchBend (1, 1, 300) }), 8, midi);
    CHECK (stage.getNumActiveNotes() == 1);
    CHECK_FALSE (stage.isBendCentred (1));

    const auto countsBefore = stage.getSentCounts();
    stage.reset();
    CHECK (stage.getNumActiveNotes() == 0);
    CHECK (stage.isBendCentred (1));

    juce::MidiBuffer flushMidi;
    stage.flushAllNotes (0, flushMidi);
    CHECK (flushMidi.isEmpty());

    // reset() does not touch the sent counters
    const auto countsAfter = stage.getSentCounts();
    CHECK (countsAfter.noteOns == countsBefore.noteOns);
    CHECK (countsAfter.pitchBends == countsBefore.pitchBends);
}

TEST_CASE ("MidiOutputStage counts every message it sends", "[midi][output]")
{
    MidiOutputStage stage;
    juce::MidiBuffer midi;

    const auto events = makeEvents<16> ({
        VsEvent::makeNoteOn (0, 1, 60, 100),
        VsEvent::makeNoteOn (1, 1, 60, 100), // +1 implicit note-off
        VsEvent::makeNoteOff (2, 1, 61),     // dropped, not counted
        VsEvent::makePitchBend (3, 1, 10),
        VsEvent::makePitchBend (4, 1, 20),
        VsEvent::makeControlChange (5, 1, 1, 64),
        VsEvent::makeNoteOn (6, 2, 50, 100),
    });
    stage.write (events, 16, midi);

    auto counts = stage.getSentCounts();
    CHECK (counts.noteOns == 3);
    CHECK (counts.noteOffs == 1);
    CHECK (counts.pitchBends == 2);
    CHECK (counts.controlChanges == 1);
    CHECK (midi.getNumEvents() == 7);

    juce::MidiBuffer flushMidi;
    stage.flushAllNotes (0, flushMidi); // 2 note-offs + 1 bend centre
    counts = stage.getSentCounts();
    CHECK (counts.noteOns == 3);
    CHECK (counts.noteOffs == 3);
    CHECK (counts.pitchBends == 3);
    CHECK (counts.controlChanges == 1);
}

TEST_CASE ("MidiOutputStage keeps pairs valid for any block split", "[midi][output]")
{
    // Random-ish stream of note events over 10000 samples, split into blocks of different sizes:
    // the output must always be properly paired and every offset inside its block.
    for (const int blockSize : { 1, 7, 64, 480, 512, 4096 })
    {
        MidiOutputStage stage;
        std::array<std::array<bool, 128>, 16> on {};
        juce::Random rng (1234);
        int noteOns = 0, noteOffs = 0;

        for (int start = 0; start < 10000; start += blockSize)
        {
            const int n = std::min (blockSize, 10000 - start);
            VsEventBuffer<64> events;

            // Producers append in time order (VsEventBuffer contract)
            std::array<int, 3> offsets { rng.nextInt (n), rng.nextInt (n), rng.nextInt (n) };
            std::sort (offsets.begin(), offsets.end());

            for (const auto offset : offsets)
            {
                const auto ch = (uint8_t) (1 + rng.nextInt (2));
                const auto note = (uint8_t) (60 + rng.nextInt (4));
                events.push (rng.nextBool() ? VsEvent::makeNoteOn (offset, ch, note, 100)
                                            : VsEvent::makeNoteOff (offset, ch, note));
            }

            juce::MidiBuffer midi;
            stage.write (events, n, midi);

            for (const auto metadata : midi)
            {
                REQUIRE (metadata.samplePosition >= 0);
                REQUIRE (metadata.samplePosition < n);
                const auto m = metadata.getMessage();
                auto& state = on[(size_t) m.getChannel() - 1][(size_t) m.getNoteNumber()];

                if (m.isNoteOn())
                {
                    REQUIRE_FALSE (state); // never a duplicate note-on
                    state = true;
                    ++noteOns;
                }
                else if (m.isNoteOff())
                {
                    REQUIRE (state); // never an unmatched note-off
                    state = false;
                    ++noteOffs;
                }
            }
        }

        juce::MidiBuffer flushMidi;
        stage.flushAllNotes (0, flushMidi);
        noteOffs += flushMidi.getNumEvents();

        CHECK (noteOns > 0);
        CHECK (noteOns == noteOffs);
        CHECK (stage.getNumActiveNotes() == 0);
    }
}

//==============================================================================
TEST_CASE ("ShortMidiFifo counts overflows and never blocks", "[midi][fifo]")
{
    ShortMidiFifo<8> fifo;
    const uint8_t noteOn[] { 0x90, 60, 100 };

    for (int i = 0; i < ShortMidiFifo<8>::usableCapacity; ++i)
        REQUIRE (fifo.push (noteOn, 3));

    CHECK (fifo.getNumReady() == 7);
    CHECK (fifo.getOverflowCount() == 0);

    CHECK_FALSE (fifo.push (noteOn, 3));
    CHECK_FALSE (fifo.push (noteOn, 3));
    CHECK (fifo.getOverflowCount() == 2);

    int drained = 0;
    CHECK (fifo.drain ([&] (const auto&) { ++drained; }) == 7);
    CHECK (drained == 7);
    CHECK (fifo.getNumReady() == 0);

    // Space again; the overflow counter is never reset
    CHECK (fifo.push (noteOn, 3));
    CHECK (fifo.getOverflowCount() == 2);
}

TEST_CASE ("ShortMidiFifo rejects invalid messages without counting an overflow", "[midi][fifo]")
{
    ShortMidiFifo<8> fifo;
    const uint8_t sysex[] { 0xf0, 0x7e, 0x7f, 0x06, 0xf7 };

    CHECK_FALSE (fifo.push (nullptr, 3));
    CHECK_FALSE (fifo.push (sysex, 0));
    CHECK_FALSE (fifo.push (sysex, -1));
    CHECK_FALSE (fifo.push (sysex, 4));
    CHECK_FALSE (fifo.push (sysex, 5));
    CHECK (fifo.getNumReady() == 0);
    CHECK (fifo.getOverflowCount() == 0);
}

TEST_CASE ("ShortMidiFifo keeps order and content across wrap-around", "[midi][fifo]")
{
    ShortMidiFifo<8> fifo;
    int nextToPush = 0;
    int nextExpected = 0;

    for (int round = 0; round < 20; ++round)
    {
        const int toPush = 1 + round % 6;
        for (int i = 0; i < toPush; ++i)
        {
            const uint8_t msg[] { 0xb0, 1, (uint8_t) (nextToPush % 128) };
            REQUIRE (fifo.push (msg, 3));
            ++nextToPush;
        }

        // 1- and 2-byte messages keep their size
        const uint8_t programChange[] { 0xc3, 5 };
        REQUIRE (fifo.push (programChange, 2));

        fifo.drain ([&] (const auto& m)
        {
            if (m.size == 2)
            {
                CHECK (m.bytes[0] == 0xc3);
                CHECK (m.bytes[1] == 5);
                return;
            }

            CHECK (m.size == 3);
            CHECK (m.bytes[0] == 0xb0);
            CHECK (m.bytes[2] == (uint8_t) (nextExpected % 128));
            ++nextExpected;
        });
    }

    CHECK (nextExpected == nextToPush);

    const uint8_t clock[] { 0xf8 };
    REQUIRE (fifo.push (clock, 1));
    fifo.discardAll();
    CHECK (fifo.getNumReady() == 0);
}

TEST_CASE ("ShortMidiFifo works with a concurrent producer and consumer", "[midi][fifo]")
{
    auto fifo = std::make_unique<VirtualMidiPort::Fifo>();
    constexpr int total = 200000;
    static_assert (total < (1 << 18));
    std::atomic<bool> producerDone { false };
    int pushed = 0;

    std::thread producer ([&]
    {
        for (int i = 0; i < total; ++i)
        {
            // 18-bit counter in channel nibble + two data bytes
            const uint8_t msg[] { (uint8_t) (0xb0 | ((i >> 14) & 0x0f)), (uint8_t) ((i >> 7) & 0x7f),
                                  (uint8_t) (i & 0x7f) };
            if (fifo->push (msg, 3))
                ++pushed;
        }
        producerDone.store (true);
    });

    // The consumer checks that the received values are strictly increasing (push order, with gaps
    // only where a push overflowed).
    int lastValue = -1;
    int received = 0;
    bool ordered = true;

    const auto consume = [&]
    {
        fifo->drain ([&] (const VirtualMidiPort::Message& m)
        {
            const int value = ((m.bytes[0] & 0x0f) << 14) | (m.bytes[1] << 7) | m.bytes[2];

            if (value <= lastValue)
                ordered = false;

            lastValue = value;
            ++received;
        });
    };

    while (! producerDone.load())
        consume();

    producer.join();
    consume();

    CHECK (ordered);
    CHECK (received == pushed);
    CHECK ((uint32_t) (total - pushed) == fifo->getOverflowCount());
}

//==============================================================================
TEST_CASE ("VirtualMidiPort constructs and destructs cleanly while disabled", "[midi][virtualport]")
{
    for (int i = 0; i < 20; ++i)
    {
        VirtualMidiPort port ("VocalSynth Unit Test");
        CHECK (port.getStatus() == VirtualMidiPort::Status::off);
        CHECK_FALSE (port.isEnabled());
        CHECK (port.getDeviceName().isEmpty());

        // Not open: dropped, no overflow counted
        CHECK_FALSE (port.push (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100)));
        CHECK (port.getOverflowCount() == 0);
    }
}

TEST_CASE ("MidiOutputStage mirrors into a disabled VirtualMidiPort without side effects", "[midi][virtualport]")
{
    VirtualMidiPort port ("VocalSynth Unit Test");
    MidiOutputStage stage;
    stage.setVirtualPort (&port);

    juce::MidiBuffer midi;
    stage.write (makeEvents<4> ({ VsEvent::makeNoteOn (0, 1, 60, 100), VsEvent::makeNoteOff (1, 1, 60) }), 4, midi);
    CHECK (midi.getNumEvents() == 2);
    CHECK (port.getOverflowCount() == 0);
    CHECK (port.getStatus() == VirtualMidiPort::Status::off);
}

//==============================================================================
// VirtualMidiPort with an injected fake device (no MIDI service needed)

namespace
{
    bool onMessageThread() { return juce::MessageManager::existsAndIsCurrentThread(); }

    /** Shared between the test, the factory and every fake device (devices may outlive a port briefly). */
    struct FakeDeviceLog
    {
        juce::CriticalSection lock;
        std::vector<juce::MidiMessage> sent;
        juce::StringArray createdNames;

        std::atomic<int> factoryCalls { 0 };
        std::atomic<int> created { 0 };
        std::atomic<int> destroyed { 0 };
        std::atomic<bool> createdOffMessageThread { false };
        std::atomic<bool> destroyedOffMessageThread { false };
        std::atomic<bool> sentOnMessageThread { false };
        std::atomic<bool> failCreation { false };
        std::atomic<bool> throwOnCreation { false };

        std::vector<juce::MidiMessage> getSent()
        {
            const juce::ScopedLock sl (lock);
            return sent;
        }

        size_t numSent()
        {
            const juce::ScopedLock sl (lock);
            return sent.size();
        }
    };

    class FakeDevice final : public VirtualMidiPort::Device
    {
    public:
        explicit FakeDevice (std::shared_ptr<FakeDeviceLog> l) : log (std::move (l)) { ++log->created; }

        ~FakeDevice() override
        {
            if (! onMessageThread())
                log->destroyedOffMessageThread = true;

            ++log->destroyed;
        }

        void sendMessageNow (const juce::MidiMessage& message) override
        {
            if (onMessageThread())
                log->sentOnMessageThread = true;

            const juce::ScopedLock sl (log->lock);
            log->sent.push_back (message);
        }

    private:
        std::shared_ptr<FakeDeviceLog> log;
    };

    VirtualMidiPort::DeviceFactory makeFakeFactory (std::shared_ptr<FakeDeviceLog> log)
    {
        return [log] (const juce::String& name) -> std::unique_ptr<VirtualMidiPort::Device>
        {
            ++log->factoryCalls;

            if (! onMessageThread())
                log->createdOffMessageThread = true;

            if (log->throwOnCreation)
                throw std::runtime_error ("fake device creation failed");

            if (log->failCreation)
                return nullptr;

            {
                const juce::ScopedLock sl (log->lock);
                log->createdNames.add (name);
            }

            return std::make_unique<FakeDevice> (log);
        };
    }

    bool sameBytes (const juce::MidiMessage& a, const juce::MidiMessage& b)
    {
        return a.getRawDataSize() == b.getRawDataSize()
               && std::equal (a.getRawData(), a.getRawData() + a.getRawDataSize(), b.getRawData());
    }

    void checkThreadRules (const FakeDeviceLog& log)
    {
        CHECK_FALSE (log.createdOffMessageThread.load());
        CHECK_FALSE (log.destroyedOffMessageThread.load());
        CHECK_FALSE (log.sentOnMessageThread.load());
    }

    constexpr int fakeTimeoutMs = 4000;
} // namespace

TEST_CASE ("VirtualMidiPort creates and destroys the device on the message thread and sends on the port thread",
           "[midi][virtualport]")
{
    using Status = VirtualMidiPort::Status;
    auto log = std::make_shared<FakeDeviceLog>();

    {
        VirtualMidiPort port ("VocalSynth Fake Lifecycle", makeFakeFactory (log));
        port.setEnabled (true);

        REQUIRE (pumpUntil ([&] { return port.getStatus() == Status::open; }, fakeTimeoutMs));
        CHECK (port.getDeviceName() == "VocalSynth Fake Lifecycle");
        CHECK (log->created == 1);

        const std::vector<juce::MidiMessage> pushed {
            juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100),
            juce::MidiMessage::controllerEvent (1, 1, 64),
            juce::MidiMessage::pitchWheel (1, 10000),
            juce::MidiMessage::noteOff (1, 60),
            juce::MidiMessage::programChange (1, 5), // 2-byte message
            juce::MidiMessage::pitchWheel (1, 8192),
        };

        for (const auto& m : pushed)
            REQUIRE (port.push (m));

        REQUIRE (pumpUntil ([&] { return log->numSent() >= pushed.size(); }, fakeTimeoutMs));

        const auto sent = log->getSent();
        REQUIRE (sent.size() == pushed.size());
        for (size_t i = 0; i < sent.size(); ++i)
            CHECK (sameBytes (sent[i], pushed[i]));

        port.setEnabled (false);
        REQUIRE (pumpUntil ([&] { return port.getStatus() == Status::off && log->destroyed == 1; }, fakeTimeoutMs));

        // Nothing was left sounding: closing adds no messages
        CHECK (log->numSent() == pushed.size());
        CHECK (port.getDeviceName().isEmpty());
        CHECK_FALSE (port.push (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100)));

        // Re-enable: a fresh device with the same (released) name
        port.setEnabled (true);
        REQUIRE (pumpUntil ([&] { return port.getStatus() == Status::open; }, fakeTimeoutMs));
        CHECK (port.getDeviceName() == "VocalSynth Fake Lifecycle");
        CHECK (log->created == 2);
    }

    // The destructor destroyed the second device (on this, the message thread)
    CHECK (log->created == log->destroyed);
    checkThreadRules (*log);
}

TEST_CASE ("VirtualMidiPort closing sends note-offs and bend centre for what is still sounding", "[midi][virtualport]")
{
    using Status = VirtualMidiPort::Status;
    auto log = std::make_shared<FakeDeviceLog>();
    auto port = std::make_unique<VirtualMidiPort> ("VocalSynth Fake Close", makeFakeFactory (log));
    port->setEnabled (true);
    REQUIRE (pumpUntil ([&] { return port->getStatus() == Status::open; }, fakeTimeoutMs));

    const uint8_t noteOnVelocityZero[] { 0x92, 64, 0 }; // channel 3, counts as a note-off

    REQUIRE (port->push (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100)));  // stays on
    REQUIRE (port->push (juce::MidiMessage::noteOn (3, 64, (juce::uint8) 90)));
    REQUIRE (port->push (noteOnVelocityZero, 3));                                 // 64 off again
    REQUIRE (port->push (juce::MidiMessage::noteOn (1, 67, (juce::uint8) 80)));
    REQUIRE (port->push (juce::MidiMessage::noteOff (1, 67)));                    // 67 off again
    REQUIRE (port->push (juce::MidiMessage::pitchWheel (2, 12000)));              // stays bent
    REQUIRE (port->push (juce::MidiMessage::pitchWheel (4, 3000)));
    REQUIRE (port->push (juce::MidiMessage::pitchWheel (4, 8192)));               // centred again
    REQUIRE (port->push (juce::MidiMessage::noteOn (16, 127, (juce::uint8) 1)));  // stays on
    REQUIRE (port->push (juce::MidiMessage::controllerEvent (16, 1, 100)));       // CC: no closing message
    constexpr size_t numPushed = 10;

    REQUIRE (pumpUntil ([&] { return log->numSent() >= numPushed; }, fakeTimeoutMs));

    SECTION ("on disable")
    {
        port->setEnabled (false);
        REQUIRE (pumpUntil ([&] { return log->destroyed == 1; }, fakeTimeoutMs));
    }

    SECTION ("on destruction while enabled")
    {
        port.reset(); // runs on the message thread: destroys the device directly
        CHECK (log->destroyed == 1);
    }

    const auto sent = log->getSent();
    REQUIRE (sent.size() == numPushed + 3);

    // Notes per channel ascending, then the bend centres
    CHECK (sent[numPushed + 0].isNoteOff());
    CHECK (sent[numPushed + 0].getChannel() == 1);
    CHECK (sent[numPushed + 0].getNoteNumber() == 60);

    CHECK (sent[numPushed + 1].isNoteOff());
    CHECK (sent[numPushed + 1].getChannel() == 16);
    CHECK (sent[numPushed + 1].getNoteNumber() == 127);

    CHECK (sent[numPushed + 2].isPitchWheel());
    CHECK (sent[numPushed + 2].getChannel() == 2);
    CHECK (sent[numPushed + 2].getPitchWheelValue() == 8192);

    checkThreadRules (*log);
}

TEST_CASE ("MidiOutputStage mirrors into an open VirtualMidiPort in order, including flushAllNotes",
           "[midi][virtualport]")
{
    using Status = VirtualMidiPort::Status;
    auto log = std::make_shared<FakeDeviceLog>();
    VirtualMidiPort port ("VocalSynth Fake Mirror", makeFakeFactory (log));
    port.setEnabled (true);
    REQUIRE (pumpUntil ([&] { return port.getStatus() == Status::open; }, fakeTimeoutMs));

    MidiOutputStage stage;
    stage.setVirtualPort (&port);

    juce::MidiBuffer midi;
    stage.write (makeEvents<8> ({ VsEvent::makeNoteOn (0, 1, 60, 100),
                                  VsEvent::makePitchBend (2, 1, 4000),
                                  VsEvent::makeControlChange (3, 1, 7, 99),
                                  VsEvent::makeNoteOn (4, 2, 62, 90),
                                  VsEvent::makeNoteOn (5, 1, 60, 110) }), // re-strike: note-off first
                 16, midi);

    juce::MidiBuffer flushMidi;
    stage.flushAllNotes (0, flushMidi);

    std::vector<juce::MidiMessage> expected;
    for (const auto& t : collect (midi))
        expected.push_back (t.message);
    for (const auto& t : collect (flushMidi))
        expected.push_back (t.message);

    REQUIRE (expected.size() >= 9); // 6 written + 2 note-offs + 1 bend centre

    REQUIRE (pumpUntil ([&] { return log->numSent() >= expected.size(); }, fakeTimeoutMs));

    const auto sent = log->getSent();
    REQUIRE (sent.size() == expected.size());
    for (size_t i = 0; i < sent.size(); ++i)
    {
        CAPTURE (i);
        CHECK (sameBytes (sent[i], expected[i]));
    }

    // The flush already ended everything: closing adds nothing
    port.setEnabled (false);
    REQUIRE (pumpUntil ([&] { return log->destroyed == 1; }, fakeTimeoutMs));
    CHECK (log->numSent() == expected.size());
    checkThreadRules (*log);
}

TEST_CASE ("VirtualMidiPort numbers the names of several instances and reuses freed names", "[midi][virtualport]")
{
    using Status = VirtualMidiPort::Status;
    auto log = std::make_shared<FakeDeviceLog>();
    const juce::String base ("VocalSynth Fake Names");

    auto first = std::make_unique<VirtualMidiPort> (base, makeFakeFactory (log));
    VirtualMidiPort second (base, makeFakeFactory (log));
    VirtualMidiPort third (base, makeFakeFactory (log));

    first->setEnabled (true);
    REQUIRE (pumpUntil ([&] { return first->getStatus() == Status::open; }, fakeTimeoutMs));
    second.setEnabled (true);
    REQUIRE (pumpUntil ([&] { return second.getStatus() == Status::open; }, fakeTimeoutMs));
    third.setEnabled (true);
    REQUIRE (pumpUntil ([&] { return third.getStatus() == Status::open; }, fakeTimeoutMs));

    CHECK (first->getDeviceName() == base);
    CHECK (second.getDeviceName() == base + " 2");
    CHECK (third.getDeviceName() == base + " 3");

    // Destroying the first frees its name for the next instance
    first.reset();
    CHECK (log->destroyed == 1);

    VirtualMidiPort fourth (base, makeFakeFactory (log));
    fourth.setEnabled (true);
    REQUIRE (pumpUntil ([&] { return fourth.getStatus() == Status::open; }, fakeTimeoutMs));
    CHECK (fourth.getDeviceName() == base);

    checkThreadRules (*log);
}

TEST_CASE ("VirtualMidiPort reports a failing or throwing factory as failed and retries", "[midi][virtualport]")
{
    using Status = VirtualMidiPort::Status;
    auto log = std::make_shared<FakeDeviceLog>();
    const bool throws = GENERATE (false, true);
    CAPTURE (throws);
    log->failCreation = ! throws;
    log->throwOnCreation = throws;

    VirtualMidiPort port ("VocalSynth Fake Failure", makeFakeFactory (log));
    port.setEnabled (true);

    REQUIRE (pumpUntil ([&] { return port.getStatus() == Status::failed; }, fakeTimeoutMs));
    CHECK (log->factoryCalls == 1);
    CHECK (port.getDeviceName().isEmpty());
    CHECK_FALSE (port.push (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100)));
    CHECK (port.getOverflowCount() == 0);

    // Not retried before retryIntervalMs ...
    pumpUntil ([] { return false; }, 300);
    CHECK (log->factoryCalls == 1);

    // ... but after it; the failed name was released, so the retry gets the base name again
    log->failCreation = false;
    log->throwOnCreation = false;
    REQUIRE (pumpUntil ([&] { return port.getStatus() == Status::open; }, VirtualMidiPort::retryIntervalMs + fakeTimeoutMs));
    CHECK (log->factoryCalls == 2);
    CHECK (port.getDeviceName() == "VocalSynth Fake Failure");

    // Disabling leaves 'failed'/'open' for 'off'
    port.setEnabled (false);
    REQUIRE (pumpUntil ([&] { return port.getStatus() == Status::off && log->destroyed == 1; }, fakeTimeoutMs));
    checkThreadRules (*log);
}

TEST_CASE ("VirtualMidiPort without a running message loop reports failed and opens once the loop runs",
           "[midi][virtualport]")
{
    using Status = VirtualMidiPort::Status;
    auto log = std::make_shared<FakeDeviceLog>();
    VirtualMidiPort port ("VocalSynth Fake No Loop", makeFakeFactory (log));
    port.setEnabled (true);

    // The message loop is not pumped: nobody creates the device
    const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) (VirtualMidiPort::retryIntervalMs + 2000);
    while (port.getStatus() != Status::failed && juce::Time::getMillisecondCounter() < end)
        juce::Thread::sleep (20);

    CHECK (port.getStatus() == Status::failed);
    CHECK (log->factoryCalls == 0);

    // The pending request is served as soon as the message thread runs
    REQUIRE (pumpUntil ([&] { return port.getStatus() == Status::open; }, fakeTimeoutMs));
    CHECK (log->factoryCalls == 1);
    checkThreadRules (*log);
}

TEST_CASE ("VirtualMidiPort disabled or destroyed while a request is pending creates nothing", "[midi][virtualport]")
{
    using Status = VirtualMidiPort::Status;
    auto log = std::make_shared<FakeDeviceLog>();

    SECTION ("disabled before the message thread answers")
    {
        VirtualMidiPort port ("VocalSynth Fake Pending", makeFakeFactory (log));
        port.setEnabled (true);
        juce::Thread::sleep (200); // the port thread posts its request; the loop is not pumped
        port.setEnabled (false);
        juce::Thread::sleep (200);

        pumpUntil ([] { return false; }, 300);
        CHECK (log->factoryCalls == 0);
        CHECK (port.getStatus() == Status::off);

        // A later enable still works
        port.setEnabled (true);
        REQUIRE (pumpUntil ([&] { return port.getStatus() == Status::open; }, fakeTimeoutMs));
        CHECK (log->factoryCalls == 1);
    }

    SECTION ("destroyed before the message thread answers")
    {
        {
            VirtualMidiPort port ("VocalSynth Fake Pending", makeFakeFactory (log));
            port.setEnabled (true);
            juce::Thread::sleep (200);
        }

        pumpUntil ([] { return false; }, 300); // a stale async update must not fire
        CHECK (log->factoryCalls == 0);
    }

    CHECK (log->created == log->destroyed);
    checkThreadRules (*log);
}

TEST_CASE ("VirtualMidiPort reports open or failed when enabled, never crashes", "[midi][virtualport][system]")
{
    // Device creation needs a MIDI service (ALSA sequencer, CoreMIDI, Windows MIDI Services). In a
    // container or on CI it usually fails: then the status must be 'failed'. Both are accepted.
    using Status = VirtualMidiPort::Status;

    auto port = std::make_unique<VirtualMidiPort> ("VocalSynth Unit Test");
    port->setEnabled (true);

    REQUIRE (pumpUntil ([&] { return port->getStatus() != Status::off; }, 8000));
    const auto status = port->getStatus();
    CHECK ((status == Status::open || status == Status::failed));

    if (status == Status::open)
    {
        CHECK (port->getDeviceName() == "VocalSynth Unit Test");
        CHECK (port->push (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100))); // left hanging on purpose:
        // closing must send the note-off

        // A second instance in the same process gets a numbered name
        VirtualMidiPort second ("VocalSynth Unit Test");
        second.setEnabled (true);

        if (pumpUntil ([&] { return second.getStatus() == Status::open; }, 8000))
            CHECK (second.getDeviceName() == "VocalSynth Unit Test 2");
    }
    else
    {
        CHECK (port->getDeviceName().isEmpty());
        CHECK_FALSE (port->push (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100)));
        CHECK (port->getOverflowCount() == 0);
    }

    // Disabling goes back to 'off'
    port->setEnabled (false);
    CHECK (pumpUntil ([&] { return port->getStatus() == Status::off; }, 8000));
    CHECK (port->getDeviceName().isEmpty());

    // Destruction while enabled is clean too
    port->setEnabled (true);
    pumpUntil ([&] { return port->getStatus() != Status::off; }, 8000);
    port.reset();
    SUCCEED();
}
