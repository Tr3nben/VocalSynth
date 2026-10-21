#pragma once

// Helpers for the M3 plugin capture tests (docs/M3-DESIGN.md 16): a fake host playhead, a synthetic sung
// phrase, a feeder that drives a CaptureTap directly, and MIDI / WAV file readers. Header-only.

#include "CaptureWorker.h"
#include "vocalsynth/CaptureTap.h"
#include "vocalsynth/VsEvent.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace vocalsynth::test
{

/** A host transport as a function of the block start sample: stopped until playStartSample, then playing
    at bpm from playStartPpq, stopped again from playStopSample (ppq stays where it stopped). */
class FakePlayHead final : public juce::AudioPlayHead
{
public:
    double sampleRate = 48000.0;
    double bpm = 120.0;
    int numerator = 4;
    int denominator = 4;
    int64_t playStartSample = -1; ///< -1 = never plays
    int64_t playStopSample = -1;  ///< -1 = never stops
    double playStartPpq = 0.0;
    bool reportBarStart = true;
    int64_t blockStart = 0;       ///< set by the runner before each block

    bool isPlayingAt (int64_t sample) const noexcept
    {
        return playStartSample >= 0 && sample >= playStartSample && (playStopSample < 0 || sample < playStopSample);
    }

    double ppqAt (int64_t sample) const noexcept
    {
        if (playStartSample < 0 || sample < playStartSample)
            return playStartPpq;

        const auto end = playStopSample >= 0 ? juce::jmin (sample, playStopSample) : sample;
        return playStartPpq + static_cast<double> (end - playStartSample) / sampleRate * bpm / 60.0;
    }

    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        const auto ppq = ppqAt (blockStart);
        const auto barLength = 4.0 * numerator / denominator;
        info.setIsPlaying (isPlayingAt (blockStart));
        info.setPpqPosition (ppq);
        info.setBpm (bpm);
        info.setTimeSignature (juce::AudioPlayHead::TimeSignature { numerator, denominator });

        if (reportBarStart)
            info.setPpqPositionOfLastBarStart (std::floor (ppq / barLength + 1.0e-9) * barLength);

        return info;
    }
};

/** One sung note in host time (before the simulated audio round trip). */
struct SungNote
{
    int64_t startSample = 0;
    int64_t lengthSamples = 0;
    double frequencyHz = 220.0;
};

/** Voice-like tone (band-limited saw, 10 ms ramps) for the notes, heard by the plugin inputDelay samples
    after host time (the sound card's round trip). */
inline float sungPhrase (const std::vector<SungNote>& notes, int64_t position, double sampleRate, int64_t inputDelay)
{
    const auto hostSample = position - inputDelay;

    for (const auto& n : notes)
    {
        if (hostSample < n.startSample || hostSample >= n.startSample + n.lengthSamples)
            continue;

        const auto u = static_cast<double> (hostSample - n.startSample) / sampleRate;
        const auto remaining = static_cast<double> (n.startSample + n.lengthSamples - hostSample) / sampleRate;
        constexpr double ramp = 0.01;
        const auto phase = juce::MathConstants<double>::twoPi * n.frequencyHz * u;

        double saw = 0.0;
        for (int k = 1; k * n.frequencyHz < 5000.0; ++k)
            saw += std::sin (static_cast<double> (k) * phase) / static_cast<double> (k);

        const auto envelope = juce::jmin (1.0, u / ramp, remaining / ramp);
        return static_cast<float> (0.2 * envelope * saw);
    }

    return 0.0f;
}

/** Drives a processor like a host with a FakePlayHead, a mono input function (copied to every input
    channel) and a manually stepped capture worker (steps at least every stepEverySamples). */
class CaptureHost
{
public:
    CaptureHost (juce::AudioProcessor& processor, CaptureWorker& captureWorker, double sampleRate, int maxBlockSize)
        : p (processor), worker (captureWorker), rate (sampleRate)
    {
        playHead.sampleRate = sampleRate;
        p.setPlayHead (&playHead);
        p.setRateAndBufferSizeDetails (sampleRate, maxBlockSize);
        p.prepareToPlay (sampleRate, maxBlockSize);
        buffer.setSize (juce::jmax (2, p.getTotalNumInputChannels(), p.getTotalNumOutputChannels()), maxBlockSize);
    }

    ~CaptureHost() { p.setPlayHead (nullptr); }

    void block (int n, bool bypassed = false)
    {
        buffer.setSize (buffer.getNumChannels(), n, false, false, true);

        for (int i = 0; i < n; ++i)
        {
            const auto x = input != nullptr ? input (position + i) : 0.0f;

            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                buffer.setSample (ch, i, x);
        }

        playHead.blockStart = position;
        midi.clear();

        if (bypassed)
            p.processBlockBypassed (buffer, midi);
        else
            p.processBlock (buffer, midi);

        for (const auto meta : midi)
        {
            const auto m = meta.getMessage();

            if (m.isNoteOn())
                ++noteOns;
            else if (m.isNoteOff())
                ++noteOffs;
        }

        position += n;
        sinceStep += n;

        if (sinceStep >= stepEverySamples)
        {
            sinceStep = 0;
            worker.runStepForTests();
        }
    }

    /** Processes until position reaches end (blocks of blockSize, the last one shorter). */
    void runTo (int64_t end, int blockSize)
    {
        while (position < end)
            block (static_cast<int> (juce::jmin<int64_t> (blockSize, end - position)));
    }

    void steps (int count)
    {
        for (int i = 0; i < count; ++i)
            worker.runStepForTests();
    }

    juce::AudioProcessor& p;
    CaptureWorker& worker;
    FakePlayHead playHead;
    std::function<float (int64_t)> input;
    juce::AudioBuffer<float> buffer;
    juce::MidiBuffer midi;
    double rate;
    int64_t position = 0;
    int64_t sinceStep = 0;
    int64_t stepEverySamples = 2048;
    int noteOns = 0;
    int noteOffs = 0;
};

/** An event at an absolute sample for TapFeeder. */
struct TimedEvent
{
    int64_t sample = 0;
    VsEvent event;
};

inline TimedEvent noteOnAt (int64_t sample, int note, int velocity = 100, int channel = 1)
{
    VsEvent e;
    e.type = VsEventType::noteOn;
    e.channel = static_cast<uint8_t> (channel);
    e.data1 = static_cast<uint8_t> (note);
    e.data2 = static_cast<uint8_t> (velocity);
    return { sample, e };
}

inline TimedEvent noteOffAt (int64_t sample, int note, int channel = 1)
{
    VsEvent e;
    e.type = VsEventType::noteOff;
    e.channel = static_cast<uint8_t> (channel);
    e.data1 = static_cast<uint8_t> (note);
    return { sample, e };
}

/** Drives a CaptureTap directly (the plugin's call order: beginBlock, processChunk), as the audio thread. */
class TapFeeder
{
public:
    explicit TapFeeder (CaptureTap& t) : tap (t) {}

    /** Feeds [position, end) in blocks; events at their absolute samples; audio from audio (nullptr:
        silence); the transport from transport (default: stopped, nothing valid). */
    void runTo (int64_t end, int blockSize, const std::vector<TimedEvent>& events = {})
    {
        std::vector<float> mono (static_cast<std::size_t> (blockSize));
        std::vector<VsEvent> blockEvents;

        while (position < end)
        {
            const auto n = static_cast<int> (juce::jmin<int64_t> (blockSize, end - position));
            blockEvents.clear();

            for (const auto& e : events)
                if (e.sample >= position && e.sample < position + n)
                {
                    auto ev = e.event;
                    ev.sampleOffset = static_cast<int> (e.sample - position);
                    blockEvents.push_back (ev);
                }

            for (int i = 0; i < n; ++i)
                mono[static_cast<std::size_t> (i)] = audio != nullptr ? audio (position + i) : 0.0f;

            tap.beginBlock (transport != nullptr ? transport (position) : TransportInfo {}, settings);
            tap.processChunk (audio != nullptr ? mono.data() : nullptr, n, blockEvents, {});
            position += n;

            if (worker != nullptr && (sinceStep += n) >= stepEverySamples)
            {
                sinceStep = 0;
                worker->runStepForTests();
            }
        }
    }

    CaptureTap& tap;
    CaptureBlockSettings settings;
    std::function<TransportInfo (int64_t)> transport;
    std::function<float (int64_t)> audio;
    CaptureWorker* worker = nullptr; ///< stepped manually while feeding, if set
    int64_t stepEverySamples = 4096;
    int64_t sinceStep = 0;
    int64_t position = 0;
};

/** Transport playing at bpm from sample playStart (ppq startPpq), stopped before. */
inline TransportInfo playingTransport (int64_t blockStart, double sampleRate, double bpm, int64_t playStart,
                                       double startPpq, int64_t playStop = -1)
{
    TransportInfo t;
    t.hasBpm = true;
    t.bpm = bpm;
    t.hasTimeSignature = true;
    t.hasPpq = true;
    const auto playing = blockStart >= playStart && (playStop < 0 || blockStart < playStop);
    const auto end = playStop >= 0 ? juce::jmin (blockStart, playStop) : blockStart;
    t.isPlaying = playing;
    t.ppqPosition = blockStart < playStart ? startPpq : startPpq + static_cast<double> (end - playStart) / sampleRate * bpm / 60.0;
    t.hasBarStart = true;
    t.ppqPositionOfLastBarStart = std::floor (t.ppqPosition / 4.0 + 1.0e-9) * 4.0;
    return t;
}

/** A note in a .mid file. */
struct FileNote
{
    int note = 0;
    int channel = 1;
    double onTick = 0.0;
    double offTick = 0.0;
};

/** Reads a .mid with juce::MidiFile; returns false if unreadable. */
inline bool readMidiFile (const juce::File& file, juce::MidiFile& midiFile)
{
    juce::FileInputStream in (file);
    return in.openedOk() && midiFile.readFrom (in);
}

/** Note-on / note-off pairs of the first track (ticks). */
inline std::vector<FileNote> notesOf (const juce::MidiFile& midiFile)
{
    std::vector<FileNote> notes;

    if (midiFile.getNumTracks() < 1)
        return notes;

    juce::MidiMessageSequence seq (*midiFile.getTrack (0));
    seq.updateMatchedPairs();

    for (int i = 0; i < seq.getNumEvents(); ++i)
    {
        const auto* e = seq.getEventPointer (i);

        if (! e->message.isNoteOn())
            continue;

        FileNote n;
        n.note = e->message.getNoteNumber();
        n.channel = e->message.getChannel();
        n.onTick = e->message.getTimeStamp();
        n.offTick = e->noteOffObject != nullptr ? e->noteOffObject->message.getTimeStamp() : -1.0;
        notes.push_back (n);
    }

    return notes;
}

/** Reads a mono WAV fully (nullptr reader: empty buffer, rate 0). */
inline juce::AudioBuffer<float> readWav (const juce::File& file, double& sampleRate, int& bitsPerSample)
{
    juce::WavAudioFormat wav;
    sampleRate = 0.0;
    bitsPerSample = 0;
    auto reader = std::unique_ptr<juce::AudioFormatReader> (wav.createReaderFor (new juce::FileInputStream (file), true));

    if (reader == nullptr)
        return {};

    sampleRate = reader->sampleRate;
    bitsPerSample = static_cast<int> (reader->bitsPerSample);
    juce::AudioBuffer<float> buffer (static_cast<int> (reader->numChannels), static_cast<int> (reader->lengthInSamples));
    reader->read (&buffer, 0, buffer.getNumSamples(), 0, true, true);
    return buffer;
}

} // namespace vocalsynth::test
