#include "TestClipExporter.h"

#include "Parameters.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>

namespace vocalsynth::TestClipExporter
{

namespace
{
constexpr int renderBlockSize = 480;
constexpr float wavGain = 0.501187f; // -6 dBFS

/** Same bpm sanitising as TestPatternGenerator, so ticks match the rendered sample positions. */
double sanitiseBpm (double bpm) noexcept
{
    if (! std::isfinite (bpm))
        return 120.0;

    return juce::jlimit (TestPatternGenerator::minBpm, TestPatternGenerator::maxBpm, bpm);
}

TestPattern patternToRender (TestPattern pattern) noexcept
{
    const auto index = static_cast<int> (pattern);

    if (index <= static_cast<int> (TestPattern::off) || index > static_cast<int> (TestPattern::all))
        return TestPattern::notes;

    return pattern;
}

juce::String patternName (TestPattern pattern)
{
    return params::testPatternChoices[static_cast<int> (patternToRender (pattern))];
}

juce::MidiMessage toMidiMessage (const VsEvent& e)
{
    const auto channel = juce::jlimit (1, 16, (int) e.channel);

    switch (e.type)
    {
        case VsEventType::noteOn:
            if (e.data2 == 0)
                return juce::MidiMessage::noteOff (channel, e.data1 & 0x7f);

            return juce::MidiMessage::noteOn (channel, e.data1 & 0x7f, (juce::uint8) (e.data2 & 0x7f));

        case VsEventType::noteOff:
            return juce::MidiMessage::noteOff (channel, e.data1 & 0x7f);

        case VsEventType::pitchBend:
            return juce::MidiMessage::pitchWheel (channel, juce::jlimit (0, 16383, (int) e.bend + 8192));

        case VsEventType::controlChange:
            return juce::MidiMessage::controllerEvent (channel, e.data1 & 0x7f, e.data2 & 0x7f);
    }

    return juce::MidiMessage::noteOff (channel, e.data1 & 0x7f);
}

/** Writes via a temporary file next to target and moves it into place. FL may still hold the file of
    an earlier drag open; then a numbered sibling is used instead. writeContent (const File&) must
    create, write and close the file and return true on success. */
template <typename WriteFn>
juce::File writeAtomically (const juce::File& target, WriteFn&& writeContent)
{
    if (target == juce::File())
        return {};

    juce::TemporaryFile temp (target);

    if (! writeContent (temp.getFile()) || ! temp.getFile().existsAsFile())
        return {};

    if (temp.overwriteTargetFileWithTemporary())
        return target;

    const auto sibling = target.getNonexistentSibling (true);

    if (temp.getFile().moveFileTo (sibling))
        return sibling;

    return {};
}
} // namespace

juce::File getExportDirectory()
{
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("VocalSynth");
    dir.createDirectory();
    return dir;
}

juce::MidiMessageSequence renderTestPattern (const TestPatternSettings& settings)
{
    auto renderSettings = settings;
    renderSettings.pattern = patternToRender (settings.pattern);
    renderSettings.bpm = sanitiseBpm (settings.bpm);

    const auto bpm = renderSettings.bpm;
    const auto samplesPerBeat = 60.0 * renderSampleRate / bpm;
    const auto totalBeats = (double) numBars * TestPatternGenerator::beatsPerBar;
    const auto totalSamples = (int64_t) std::ceil (totalBeats * samplesPerBeat);
    const auto ticksPerSample = bpm / 60.0 / renderSampleRate * (double) ticksPerQuarterNote;

    juce::MidiMessageSequence sequence;

    auto trackName = juce::MidiMessage::textMetaEvent (3, "VocalSynth test pattern - " + patternName (settings.pattern));
    trackName.setTimeStamp (0.0);
    sequence.addEvent (trackName);

    auto tempo = juce::MidiMessage::tempoMetaEvent (juce::roundToInt (60.0e6 / bpm));
    tempo.setTimeStamp (0.0);
    sequence.addEvent (tempo);

    auto timeSignature = juce::MidiMessage::timeSignatureMetaEvent (4, 4);
    timeSignature.setTimeStamp (0.0);
    sequence.addEvent (timeSignature);

    TestPatternGenerator generator;
    generator.prepare (renderSampleRate);

    VsEventBuffer<1024> events;

    const auto addEvents = [&] (int64_t blockStart, bool atEnd)
    {
        for (const auto& e : events)
        {
            double tick = (double) endTick;

            if (! atEnd)
                tick = juce::jmin ((double) endTick,
                                   (double) juce::roundToInt ((double) (blockStart + e.sampleOffset) * ticksPerSample));

            auto message = toMidiMessage (e);
            message.setTimeStamp (tick);
            sequence.addEvent (message);
        }
    };

    for (int64_t start = 0; start < totalSamples; start += renderBlockSize)
    {
        const auto numSamples = (int) juce::jmin ((int64_t) renderBlockSize, totalSamples - start);
        events.clear();
        generator.process (numSamples, renderSettings, events);
        addEvents (start, false);
    }

    // Anything still sounding (held notes, bend) ends exactly at the clip end
    events.clear();
    generator.flush (0, events);
    addEvents (totalSamples, true);

    sequence.updateMatchedPairs();

    auto endOfTrack = juce::MidiMessage::endOfTrack();
    endOfTrack.setTimeStamp ((double) endTick);
    sequence.addEvent (endOfTrack);

    return sequence;
}

juce::File exportMidi (const TestPatternSettings& settings)
{
    juce::MidiFile midiFile;
    midiFile.setTicksPerQuarterNote (ticksPerQuarterNote);
    midiFile.addTrack (renderTestPattern (settings));

    const auto fileName = "VocalSynth-Test-" + patternName (settings.pattern).removeCharacters (" ") + ".mid";

    return writeAtomically (getExportDirectory().getChildFile (fileName),
                            [&] (const juce::File& file)
                            {
                                juce::FileOutputStream out (file);

                                if (! out.openedOk() || ! midiFile.writeTo (out, 1))
                                    return false;

                                out.flush();
                                return out.getStatus().wasOk();
                            });
}

juce::File exportWav()
{
    const auto numSamples = (int) std::lround (wavLengthSeconds * renderSampleRate);

    juce::AudioBuffer<float> buffer (1, numSamples);
    auto* data = buffer.getWritePointer (0);
    const auto phaseIncrement = juce::MathConstants<double>::twoPi * wavFrequencyHz / renderSampleRate;

    for (int i = 0; i < numSamples; ++i)
        data[i] = wavGain * (float) std::sin (phaseIncrement * (double) i);

    return writeAtomically (getExportDirectory().getChildFile ("VocalSynth-Test-440Hz.wav"),
                            [&] (const juce::File& file)
                            {
                                auto fileStream = std::make_unique<juce::FileOutputStream> (file);

                                if (! fileStream->openedOk())
                                    return false;

                                std::unique_ptr<juce::OutputStream> stream = std::move (fileStream);

                                const auto options = juce::AudioFormatWriterOptions()
                                                         .withSampleRate (renderSampleRate)
                                                         .withNumChannels (1)
                                                         .withBitsPerSample (wavBitDepth);

                                // The writer owns the stream; destroying it finalises the header and
                                // closes the file before it is moved into place
                                auto writer = juce::WavAudioFormat().createWriterFor (stream, options);

                                if (writer == nullptr)
                                    return false;

                                return writer->writeFromAudioSampleBuffer (buffer, 0, numSamples);
                            });
}

} // namespace vocalsynth::TestClipExporter
