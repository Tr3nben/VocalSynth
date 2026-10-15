#include "Parameters.h"
#include "PluginProcessor.h"
#include "TestClipExporter.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <set>

using namespace vocalsynth;

namespace
{
TestPatternSettings makeSettings (TestPattern pattern, int channel = 1, double bpm = 120.0, int cc = 1)
{
    TestPatternSettings s;
    s.pattern = pattern;
    s.channel = channel;
    s.bpm = bpm;
    s.ccNumber = cc;
    return s;
}

int expectedNoteOns (TestPattern pattern)
{
    switch (pattern)
    {
        case TestPattern::pitchBend:
        case TestPattern::controlChange: return 4;
        case TestPattern::off:
        case TestPattern::notes:
        case TestPattern::all: return 32;
    }

    return 0;
}

struct SequenceStats
{
    int noteOns = 0;
    int noteOffs = 0;
    int unmatchedNoteOns = 0;
    int pitchBends = 0;
    int controlChanges = 0;
    int tempoEvents = 0;
    int trackNames = 0;
    double lastTick = 0.0;
    bool timesSorted = true;
    std::set<int> channels;
    std::set<int> ccNumbers;
};

SequenceStats analyse (const juce::MidiMessageSequence& seq)
{
    SequenceStats stats;
    double previous = 0.0;

    for (int i = 0; i < seq.getNumEvents(); ++i)
    {
        const auto* holder = seq.getEventPointer (i);
        const auto& m = holder->message;

        stats.lastTick = juce::jmax (stats.lastTick, m.getTimeStamp());
        stats.timesSorted = stats.timesSorted && m.getTimeStamp() >= previous;
        previous = m.getTimeStamp();

        if (m.isNoteOn())
        {
            ++stats.noteOns;

            if (holder->noteOffObject == nullptr)
                ++stats.unmatchedNoteOns;
        }
        else if (m.isNoteOff())
        {
            ++stats.noteOffs;
        }
        else if (m.isPitchWheel())
        {
            ++stats.pitchBends;
        }
        else if (m.isController())
        {
            ++stats.controlChanges;
            stats.ccNumbers.insert (m.getControllerNumber());
        }
        else if (m.isTempoMetaEvent())
        {
            ++stats.tempoEvents;
        }
        else if (m.isTrackNameEvent())
        {
            ++stats.trackNames;
        }

        if (m.getChannel() > 0)
            stats.channels.insert (m.getChannel());
    }

    return stats;
}

juce::MidiFile readMidiFile (const juce::File& file)
{
    juce::MidiFile midi;
    juce::FileInputStream in (file);
    REQUIRE (in.openedOk());
    REQUIRE (midi.readFrom (in));
    return midi;
}
} // namespace

TEST_CASE ("TestClipExporter renders 4 bars with the expected note-ons", "[exporter]")
{
    const auto pattern = GENERATE (TestPattern::off, TestPattern::notes, TestPattern::pitchBend,
                                   TestPattern::controlChange, TestPattern::all);
    const auto bpm = GENERATE (120.0, 137.3, 60.0, 200.0);
    CAPTURE (static_cast<int> (pattern), bpm);

    const auto seq = TestClipExporter::renderTestPattern (makeSettings (pattern, 1, bpm));
    const auto stats = analyse (seq);

    CHECK (stats.noteOns == expectedNoteOns (pattern));
    CHECK (stats.noteOffs == stats.noteOns);
    CHECK (stats.unmatchedNoteOns == 0);
    CHECK (stats.tempoEvents == 1);
    CHECK (stats.trackNames == 1);
    CHECK (stats.timesSorted);
    CHECK (stats.lastTick <= (double) TestClipExporter::endTick);

    // The clip ends exactly at 4 bars
    const auto& last = seq.getEventPointer (seq.getNumEvents() - 1)->message;
    CHECK (last.isEndOfTrackMetaEvent());
    CHECK (last.getTimeStamp() == Catch::Approx ((double) TestClipExporter::endTick));

    if (pattern == TestPattern::pitchBend)
        CHECK (stats.pitchBends > 100);

    if (pattern == TestPattern::controlChange)
        CHECK (stats.controlChanges > 100);
}

TEST_CASE ("TestClipExporter notes sit on the eighth-note grid in ticks", "[exporter]")
{
    const auto bpm = GENERATE (120.0, 137.3, 93.0);
    CAPTURE (bpm);

    const auto seq = TestClipExporter::renderTestPattern (makeSettings (TestPattern::notes, 1, bpm));
    constexpr int scale[] = { 60, 62, 64, 65, 67, 69, 71, 72 };
    constexpr int eighthTicks = TestClipExporter::ticksPerQuarterNote / 2;

    int index = 0;

    for (int i = 0; i < seq.getNumEvents(); ++i)
    {
        const auto* holder = seq.getEventPointer (i);

        if (! holder->message.isNoteOn())
            continue;

        CAPTURE (index);
        CHECK (holder->message.getNoteNumber() == scale[index % 8]);
        CHECK (holder->message.getVelocity() == 100);
        // One sample at 48 kHz is well below one tick at these tempi; allow rounding by one tick
        CHECK (std::abs (holder->message.getTimeStamp() - (double) (index * eighthTicks)) <= 1.0);

        REQUIRE (holder->noteOffObject != nullptr);
        const auto length = holder->noteOffObject->message.getTimeStamp() - holder->message.getTimeStamp();
        CHECK (std::abs (length - 0.75 * eighthTicks) <= 1.0); // gate 75 %
        ++index;
    }

    CHECK (index == 32);
}

TEST_CASE ("TestClipExporter respects channel and CC number", "[exporter]")
{
    const auto seq = TestClipExporter::renderTestPattern (makeSettings (TestPattern::controlChange, 5, 120.0, 74));
    const auto stats = analyse (seq);

    CHECK (stats.channels == std::set<int> { 5 });
    CHECK (stats.ccNumbers == std::set<int> { 74 });
    CHECK (stats.noteOns == 4);
}

TEST_CASE ("TestClipExporter held patterns end with note-off and bend centre at the clip end", "[exporter]")
{
    const auto seq = TestClipExporter::renderTestPattern (makeSettings (TestPattern::pitchBend));

    bool sawNoteOffAtEnd = false;
    int lastBendValue = -1;

    for (int i = 0; i < seq.getNumEvents(); ++i)
    {
        const auto& m = seq.getEventPointer (i)->message;

        if (m.isNoteOff() && m.getTimeStamp() == Catch::Approx ((double) TestClipExporter::endTick))
            sawNoteOffAtEnd = true;

        if (m.isPitchWheel())
        {
            CHECK (m.getPitchWheelValue() >= 0);
            CHECK (m.getPitchWheelValue() <= 16383);
            lastBendValue = m.getPitchWheelValue();
        }
    }

    CHECK (sawNoteOffAtEnd);
    CHECK (lastBendValue == 8192);
}

TEST_CASE ("TestClipExporter writes a readable MIDI file", "[exporter][file]")
{
    const auto pattern = GENERATE (TestPattern::notes, TestPattern::pitchBend, TestPattern::controlChange,
                                   TestPattern::all, TestPattern::off);
    CAPTURE (static_cast<int> (pattern));

    const auto file = TestClipExporter::exportMidi (makeSettings (pattern, 3, 120.0));
    REQUIRE (file.existsAsFile());
    CHECK (file.getParentDirectory() == TestClipExporter::getExportDirectory());
    CHECK (file.hasFileExtension ("mid"));
    CHECK (file.getFileName().startsWith ("VocalSynth-Test-"));

    const auto midi = readMidiFile (file);
    CHECK (midi.getTimeFormat() == TestClipExporter::ticksPerQuarterNote);
    REQUIRE (midi.getNumTracks() == 1);

    juce::MidiMessageSequence tempos;
    midi.findAllTempoEvents (tempos);
    REQUIRE (tempos.getNumEvents() == 1);
    CHECK (tempos.getEventPointer (0)->message.getTimeStamp() == 0.0);
    CHECK (tempos.getEventPointer (0)->message.getTempoSecondsPerQuarterNote() == Catch::Approx (0.5));

    auto track = *midi.getTrack (0);
    track.updateMatchedPairs();
    const auto stats = analyse (track);

    CHECK (stats.noteOns == expectedNoteOns (pattern));
    CHECK (stats.unmatchedNoteOns == 0);
    CHECK (stats.trackNames == 1);
    CHECK (stats.lastTick <= (double) TestClipExporter::endTick);
    CHECK (stats.channels == std::set<int> { 3 });
    CHECK (midi.getLastTimestamp() <= (double) TestClipExporter::endTick);
}

TEST_CASE ("TestClipExporter overwrites the previous export", "[exporter][file]")
{
    const auto first = TestClipExporter::exportMidi (makeSettings (TestPattern::notes, 1, 120.0));
    const auto second = TestClipExporter::exportMidi (makeSettings (TestPattern::notes, 2, 120.0));

    REQUIRE (first.existsAsFile());
    REQUIRE (second.existsAsFile());
    CHECK (first == second);

    const auto stats = analyse (*readMidiFile (second).getTrack (0));
    CHECK (stats.channels == std::set<int> { 2 });

    // No temporary files are left behind
    const auto leftovers =
        TestClipExporter::getExportDirectory().findChildFiles (juce::File::findFiles, false, "*_temp*");
    CHECK (leftovers.isEmpty());
}

TEST_CASE ("TestClipExporter writes a readable 440 Hz WAV", "[exporter][file]")
{
    const auto file = TestClipExporter::exportWav();
    REQUIRE (file.existsAsFile());
    CHECK (file.getFileName() == "VocalSynth-Test-440Hz.wav");

    juce::WavAudioFormat format;
    std::unique_ptr<juce::AudioFormatReader> reader (
        format.createReaderFor (new juce::FileInputStream (file), true));
    REQUIRE (reader != nullptr);

    CHECK (reader->sampleRate == Catch::Approx (48000.0));
    CHECK (reader->bitsPerSample == 24);
    CHECK (reader->numChannels == 1);
    CHECK (reader->lengthInSamples == 96000);

    juce::AudioBuffer<float> buffer (1, (int) reader->lengthInSamples);
    REQUIRE (reader->read (&buffer, 0, buffer.getNumSamples(), 0, true, false));

    // -6 dBFS peak
    CHECK (buffer.getMagnitude (0, 0, buffer.getNumSamples()) == Catch::Approx (0.501).margin (0.005));

    // 440 Hz for 2 s: 880 upward zero crossings
    int upwardCrossings = 0;
    const auto* data = buffer.getReadPointer (0);

    for (int i = 1; i < buffer.getNumSamples(); ++i)
        if (data[i - 1] < 0.0f && data[i] >= 0.0f)
            ++upwardCrossings;

    CHECK (std::abs (upwardCrossings - 880) <= 1);
}
