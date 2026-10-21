// Detection-delay compensation end to end (docs/M3-DESIGN.md 7, 8, 16). Owner: agent B.
//
// Synthetic voiced onsets go through the real capture path: VoicePipeline (timing side output) ->
// CaptureTap -> CaptureDrain (CaptureHistory + TakeSegmenter) -> TakeRenderer::refineNoteOnsets ->
// TakeRenderer::render. Acceptance (spec 6): the exported MIDI onset vs the onset in the exported WAV
// (both relative to tick 0 / WAV sample 0) <= 1 hop median, p95 <= 2 hops, every range x YIN / MPM x
// 44.1 / 48 / 96 kHz, with the round trip 0 and Auto 2048 (4096 samples: MIDI and WAV move together).
// Plus the end-to-end block-size invariance gate: byte-identical .mid (and identical WAV audio) for host
// blocks {1, 7, 64, 480, 512, 4096}.

#include "vocalsynth/CaptureDrain.h"
#include "vocalsynth/CaptureHistory.h"
#include "vocalsynth/CaptureTap.h"
#include "vocalsynth/Pitch.h"
#include "vocalsynth/SmfWriter.h"
#include "vocalsynth/TakeRenderer.h"
#include "vocalsynth/TakeSegmenter.h"
#include "vocalsynth/VoicePipeline.h"

#include "TestSignals.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

using namespace vocalsynth;

namespace
{
constexpr double sampleRates[] = { 44100.0, 48000.0, 96000.0 };
constexpr VoiceRange allRanges[] = { VoiceRange::bass,    VoiceRange::baritone, VoiceRange::tenor, VoiceRange::alto,
                                     VoiceRange::soprano, VoiceRange::whistle,  VoiceRange::wide };
constexpr double testBpm = 120.0;

const char* algorithmName (PitchAlgorithm a) { return a == PitchAlgorithm::mpm ? "MPM" : "YIN"; }

/** Host transport: playing at testBpm from ppq 0 in [playFrom, playTo), else stopped (or no playhead). */
struct TransportPlan
{
    int64_t playFrom = 0;
    int64_t playTo = 0;    ///< playFrom == playTo: never playing
    bool hostHasPlayhead = true;

    TransportInfo at (int64_t sample, double sr) const
    {
        TransportInfo t;

        if (! hostHasPlayhead)
            return t;

        const auto playing = sample >= playFrom && sample < playTo;
        const auto ppq = testBpm / 60.0 * static_cast<double> (std::clamp (sample, playFrom, playTo) - playFrom) / sr;
        t.isPlaying = playing;
        t.hasPpq = true;
        t.ppqPosition = ppq;
        t.hasBpm = true;
        t.bpm = testBpm;
        t.hasTimeSignature = true;
        t.timeSigNumerator = 4;
        t.timeSigDenominator = 4;
        t.hasBarStart = true;
        t.ppqPositionOfLastBarStart = std::floor ((ppq + 1.0e-9) / 4.0) * 4.0;
        return t;
    }
};

/** The processor's capture path without JUCE (docs/M3-DESIGN.md 2, 4): per host block beginBlock, then
    chunks of <= 4096 samples through the pipeline and the tap; the "worker" drains every 2048 samples. */
class CaptureRig
{
public:
    CaptureRig (double sampleRate, int maxBlock = 4096) : sr (sampleRate)
    {
        pipeline.prepare (sr, maxBlock);
        tap.prepare (sr, maxBlock);
        history->prepare (CaptureHistory::minAudioMinutes);
    }

    /** Processes input (capture clock = input index) in blocks of blockSize. */
    void run (std::span<const float> input, const VoiceSettings& settings, int blockSize, const TransportPlan& transport)
    {
        CaptureBlockSettings block;
        block.settings.bendMode = settings.bendMode;
        block.settings.bendRangeSemitones = bendRangeSemitones (settings.bendRange);
        block.settings.channel = settings.channel;
        block.settings.minNoteLengthMs = settings.minNoteLengthMs;

        int64_t lastDrain = 0;

        for (std::size_t pos = 0; pos < input.size();)
        {
            const auto n = static_cast<int> (std::min<std::size_t> (static_cast<std::size_t> (blockSize), input.size() - pos));
            const auto clock = static_cast<int64_t> (pos);
            tap.beginBlock (transport.at (clock, sr), block);

            for (int done = 0; done < n;)
            {
                const auto chunk = std::min (4096, n - done);
                const auto* mono = input.data() + pos + static_cast<std::size_t> (done);
                events.clear();
                timings.clear();
                pipeline.process (mono, chunk, settings, events, timings);
                tap.processChunk (mono, chunk, events.view(), timings.view());
                done += chunk;
            }

            pos += static_cast<std::size_t> (n);

            if (static_cast<int64_t> (pos) - lastDrain >= 2048)
            {
                drainer.drain (tap, *history, segmenter, closed);
                lastDrain = static_cast<int64_t> (pos);
            }
        }

        drainer.drain (tap, *history, segmenter, closed);
    }

    /** What the worker does before publishing a closed take: onset refinement on the dry audio. */
    void refine (Take& take) const
    {
        const auto from = take.startSample - static_cast<int64_t> (sr); // a bar origin may lie before the start
        std::vector<float> audio (static_cast<std::size_t> (std::max<int64_t> (0, take.captureEndSample - from)));
        history->readAudio (from, audio);
        TakeRenderer::refineNoteOnsets (take, audio, from);
    }

    std::vector<float> readAudio (const AudioSpan& span) const
    {
        std::vector<float> audio (static_cast<std::size_t> (span.numSamples));
        history->readAudio (span.startSample, audio);
        return audio;
    }

    double sr;
    VoicePipeline pipeline;
    CaptureTap tap;
    std::unique_ptr<CaptureHistory> history = std::make_unique<CaptureHistory>();
    TakeSegmenter segmenter;
    CaptureDrain drainer;
    std::vector<Take> closed;

private:
    VsEventBuffer<1024> events;
    VsEventTimingBuffer<1024> timings;
};

/** A test melody: tones after silence (true onsets known), with a faint noise floor. */
struct Melody
{
    test::Signal signal;
    std::vector<int64_t> onsets; ///< capture-clock sample of each tone start
    std::vector<int> notes;      ///< nearest MIDI note of each tone
};

Melody onsetMelody (double sr, VoiceRange range, int64_t start, uint64_t seed)
{
    const auto& info = voiceRangeInfo (range);
    const auto lo = frequencyToMidiNote (info.f0MinHz);
    const auto hi = frequencyToMidiNote (info.f0MaxHz);
    Melody m;
    m.signal = test::silence (sr, static_cast<double> (start) / sr);
    constexpr int numTones = 12;
    const double levels[] = { -30.0, -18.0, -8.0 };

    for (int k = 0; k < numTones; ++k)
    {
        // Pitches across the range (half a semitone inside its edges), three levels, sine / saw / voice-like
        const auto note = static_cast<int> (std::round (lo + 0.5 + (hi - lo - 1.0) * (k % 6) / 5.0));
        const auto f = test::midiToHz (note);
        const auto amplitude = test::sineAmplitudeForRmsDb (levels[k % 3]);
        test::Signal tone;

        switch (k % 3)
        {
            case 0: tone = test::sine (sr, f, 0.3, amplitude); break;
            case 1: tone = test::saw (sr, f, 0.3, amplitude * 0.8f); break;
            default:
                tone = test::oscillator (sr, 0.3, amplitude * 0.7f, [f] (double) { return f; },
                                         [] (double phase)
                                         {
                                             return std::sin (2.0 * std::numbers::pi * phase) + 0.5 * std::sin (4.0 * std::numbers::pi * phase)
                                                    + 0.25 * std::sin (6.0 * std::numbers::pi * phase);
                                         });
                break;
        }

        // A short fade-out (no click that could look like an onset), onsets at arbitrary sample phases
        const auto fade = test::numSamplesFor (0.005, sr);

        for (std::size_t i = 0; i < fade; ++i)
            tone[tone.size() - 1 - i] *= static_cast<float> (i) / static_cast<float> (fade);

        m.onsets.push_back (static_cast<int64_t> (m.signal.size()));
        m.notes.push_back (note);
        m.signal.insert (m.signal.end(), tone.begin(), tone.end());
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        const auto gap = 0.17 + 0.05 * static_cast<double> ((seed >> 33) % 1000) / 1000.0;
        m.signal.resize (m.signal.size() + test::numSamplesFor (gap, sr), 0.0f);
    }

    test::mixInto (m.signal, test::whiteNoise (sr, static_cast<double> (m.signal.size()) / sr, 0.0005f, seed)); // ~ -69 dBFS
    return m;
}

struct RenderedNote
{
    int64_t startTick = 0;
    int pitch = 0;
};

std::vector<RenderedNote> noteStarts (const RenderedTake& r)
{
    std::vector<RenderedNote> out;

    for (const auto& e : r.events)
        if (e.type == TickEventType::noteOn)
            out.push_back ({ e.tick, e.data1 });

    return out;
}

double percentile (std::vector<double> v, double q)
{
    if (v.empty())
        return 0.0;

    std::sort (v.begin(), v.end());
    const auto index = q <= 0.0 ? std::size_t { 0 } : static_cast<std::size_t> (std::ceil (q * static_cast<double> (v.size()))) - 1;
    return v[std::min (index, v.size() - 1)];
}

/** |MIDI onset - audio onset| in hops for every tone of the melody, measured in the rendered take: the
    note's tick vs the tone's position in the take's WAV span (tick 0 = WAV sample 0, constant tempo).
    Fails the test if a tone has no note of its pitch near it. */
std::vector<double> onsetErrorsInHops (const Take& take, const RenderOptions& options, const Melody& m, double sr, double hop)
{
    const auto r = TakeRenderer::render (take, options);
    const auto starts = noteStarts (r);
    const auto samplesPerTick = take.mode == TakeMode::synced ? 60.0 / testBpm * sr / static_cast<double> (r.ticksPerQuarterNote)
                                                              : 60.0 / take.freeBpm * sr / static_cast<double> (r.ticksPerQuarterNote);
    std::vector<double> errors;
    CHECK (starts.size() == m.onsets.size());

    for (std::size_t k = 0; k < m.onsets.size(); ++k)
    {
        CAPTURE (k, m.notes[k], m.onsets[k]);
        // Where the tone starts in the WAV (the capture clock is the audio as it arrived)
        const auto wavIndex = static_cast<double> (m.onsets[k] - r.audio.startSample);
        auto best = 1.0e300;

        for (const auto& n : starts)
            if (n.pitch == m.notes[k])
                best = std::min (best, std::abs (static_cast<double> (n.startTick) * samplesPerTick - wavIndex));

        REQUIRE (best < 0.1 * sr); // the note exists, near its tone
        errors.push_back (best / hop);
    }

    return errors;
}
} // namespace

//==============================================================================
TEST_CASE ("Compensation: exported MIDI onsets line up with the exported WAV within 1 hop (median), 2 hops (p95)",
           "[capturecompensation]")
{
    std::printf ("[capturecompensation] |MIDI - WAV onset| in hops, synced take, Clean / Raw, rt 0 and 4096:\n");
    std::printf ("  %-8s %-3s %6s %8s %8s %8s %8s %8s\n", "range", "alg", "rate", "median", "p95", "max", "raw p95", "raw max");

    for (const auto range : allRanges)
        for (const auto algorithm : { PitchAlgorithm::yin, PitchAlgorithm::mpm })
            for (const auto sr : sampleRates)
            {
                CAPTURE (voiceRangeInfo (range).name, algorithmName (algorithm), sr);
                VoiceSettings s;
                s.range = range;
                s.algorithm = algorithm;

                // Transport playing from sample 0; the first tone in bar 1 after 0.3 s
                const auto melody = onsetMelody (sr, range, static_cast<int64_t> (0.3 * sr), 11);
                auto input = melody.signal;
                const auto playTo = static_cast<int64_t> (input.size());
                input.resize (input.size() + test::numSamplesFor (1.0, sr), 0.0f); // the capture tail after Stop

                CaptureRig rig (sr, 2048);
                rig.run (input, s, 512, { 0, playTo, true });
                REQUIRE (rig.closed.size() == 1);
                auto take = rig.closed.front();
                REQUIRE (take.mode == TakeMode::synced);
                CHECK (take.maxBlockSize == 2048);
                rig.refine (take);

                const auto hop = static_cast<double> (rig.pipeline.getHopInputSamples());
                std::vector<double> clean;
                std::vector<double> raw;

                for (const auto rt : { int64_t { 0 }, int64_t { 4096 } })
                {
                    CAPTURE (rt);
                    RenderOptions o;
                    o.roundTripSamples = rt;
                    const auto c = onsetErrorsInHops (take, o, melody, sr, hop);
                    clean.insert (clean.end(), c.begin(), c.end());
                    o.version = ExportVersion::raw;
                    const auto w = onsetErrorsInHops (take, o, melody, sr, hop);
                    raw.insert (raw.end(), w.begin(), w.end());
                }

                CHECK (percentile (clean, 0.5) <= 1.0);
                CHECK (percentile (clean, 0.95) <= 2.0);
                CHECK (percentile (raw, 0.5) <= 1.0);
                CHECK (percentile (raw, 0.95) <= 2.0);

                if (sr == 48000.0)
                    std::printf ("  %-8s %-3s %6.0f %8.2f %8.2f %8.2f %8.2f %8.2f\n", voiceRangeInfo (range).name, algorithmName (algorithm),
                                 sr, percentile (clean, 0.5), percentile (clean, 0.95), percentile (clean, 1.0), percentile (raw, 0.95),
                                 percentile (raw, 1.0));
            }
}

TEST_CASE ("Compensation: free takes (no playhead) line up with their WAV as well", "[capturecompensation]")
{
    for (const auto sr : sampleRates)
        for (const auto range : { VoiceRange::bass, VoiceRange::tenor, VoiceRange::soprano })
        {
            CAPTURE (sr, voiceRangeInfo (range).name);
            VoiceSettings s;
            s.range = range;
            const auto melody = onsetMelody (sr, range, static_cast<int64_t> (0.5 * sr), 5);
            auto input = melody.signal;
            input.resize (input.size() + test::numSamplesFor (TakeSegmenter::defaultTakeGapSeconds + 0.5, sr), 0.0f);

            CaptureRig rig (sr, 512);
            rig.run (input, s, 480, { 0, 0, false });
            REQUIRE (rig.closed.size() == 1);
            auto take = rig.closed.front();
            REQUIRE (take.mode == TakeMode::free);
            CHECK (take.freeBpm == TakeTimeline::fallbackBpm);
            rig.refine (take);

            const auto hop = static_cast<double> (rig.pipeline.getHopInputSamples());
            RenderOptions o;
            o.roundTripSamples = 4096; // never applies to free takes
            const auto errors = onsetErrorsInHops (take, o, melody, sr, hop);
            CHECK (percentile (errors, 0.5) <= 1.0);
            CHECK (percentile (errors, 0.95) <= 2.0);

            // Tick 0 is the first note-on, WAV sample 0 its onset
            const auto r = TakeRenderer::render (take, o);
            REQUIRE (! r.events.empty());
            CHECK (std::abs (r.audio.startSample - melody.onsets.front()) <= static_cast<int64_t> (hop));
        }
}

TEST_CASE ("Compensation: legato note changes and provisional corrections", "[capturecompensation]")
{
    int corrections = 0;

    for (const auto sr : sampleRates)
        for (const auto algorithm : { PitchAlgorithm::yin, PitchAlgorithm::mpm })
        {
            CAPTURE (sr, algorithmName (algorithm));
            VoiceSettings s;
            s.range = VoiceRange::tenor;
            s.algorithm = algorithm;

            // A3 -> C4 -> D4 phase-continuous (note changes 35 ms + a window late), then a scoop into A3
            // (a provisional note corrected by the full window)
            const auto start = test::numSamplesFor (0.4, sr);
            const auto len = test::numSamplesFor (0.4, sr);
            auto input = test::concat ({ test::silence (sr, 0.4),
                                         test::toneSequence (sr, { { 220.0, 0.4 }, { 261.63, 0.4 }, { 293.66, 0.4 } }, 0.3f),
                                         test::silence (sr, 0.3) });
            const auto scoopStart = static_cast<int64_t> (input.size());
            auto scoop = test::oscillator (
                sr, 0.4, 0.3f, [] (double t) { return midiNoteToFrequency (57.0 - 1.0 * std::max (0.0, 1.0 - t / 0.025)); },
                [] (double phase) { return std::sin (2.0 * std::numbers::pi * phase) + 0.4 * std::sin (4.0 * std::numbers::pi * phase); });
            input.insert (input.end(), scoop.begin(), scoop.end());
            input.resize (input.size() + test::numSamplesFor (0.3, sr), 0.0f);
            const auto playTo = static_cast<int64_t> (input.size());
            input.resize (input.size() + test::numSamplesFor (1.0, sr), 0.0f);

            CaptureRig rig (sr, 2048);
            rig.run (input, s, 256, { 0, playTo, true });
            REQUIRE (rig.closed.size() == 1);
            auto take = rig.closed.front();
            rig.refine (take);

            const auto hop = static_cast<double> (rig.pipeline.getHopInputSamples());
            const auto samplesPerTick = 60.0 / testBpm * sr / 960.0;
            RenderOptions o;
            o.roundTripSamples = 4096;
            const auto r = TakeRenderer::render (take, o);
            const auto starts = noteStarts (r);
            REQUIRE (starts.size() >= 4);

            const auto wavTick = [&] (int64_t sample) { return static_cast<double> (sample - r.audio.startSample) / samplesPerTick; };
            const auto findStart = [&] (int pitch, int64_t near)
            {
                auto best = 1.0e300;

                for (const auto& n : starts)
                    if (n.pitch == pitch)
                        best = std::min (best, std::abs (static_cast<double> (n.startTick) - wavTick (near)) * samplesPerTick);

                return best / hop;
            };

            // The note changes: within 4 hops of where the pitch changed (live: 35 ms T_min + a window later)
            CHECK (findStart (57, static_cast<int64_t> (start)) <= 1.0);
            CHECK (findStart (60, static_cast<int64_t> (start + len)) <= 4.0);
            CHECK (findStart (62, static_cast<int64_t> (start + 2 * len)) <= 4.0);

            // The scoop starts with a provisional G#3. If the full window corrected it (flag correction),
            // Clean exports one A3 from the start of the sound; if the G#3 lasted into a normal note change
            // (noteChange), Clean removes it as a ghost note (< 80 ms) and the A3 starts at the change. Raw
            // keeps both as sent.
            const auto corrected = std::any_of (take.events.begin(), take.events.end(), [&] (const CapturedEvent& e)
                                                { return e.sample > scoopStart && (e.timing.flags & EventTimingFlags::correction) != 0; });
            corrections += corrected ? 1 : 0;

            if (corrected)
                CHECK (findStart (57, scoopStart) <= 2.0);
            else
                CHECK (findStart (57, scoopStart) <= 0.03 * sr / hop); // the G#3 lasted < 30 ms

            CHECK (std::count_if (starts.begin(), starts.end(), [&] (const RenderedNote& n)
                                  { return std::abs (static_cast<double> (n.startTick) - wavTick (scoopStart)) * samplesPerTick < 0.1 * sr; })
                   == 1);
            o.version = ExportVersion::raw;
            CHECK (noteStarts (TakeRenderer::render (take, o)).size() == starts.size() + 1);
        }

    CHECK (corrections >= 1); // the correction merge is exercised end to end
}

TEST_CASE ("Compensation: byte-identical .mid and identical WAV audio for every host block size", "[capturecompensation]")
{
    // Transport changes on block boundaries of every compared size (430080 = lcm (7, 64, 480, 512, 4096)):
    // playing [0, stop), then stopped: the synced take, its capture tail, then a free take. The pipeline
    // and the tap run with every event timing; the takes are rendered Raw / Clean x quantize off / 1/16 at 50 %.
    for (const auto sr : sampleRates)
    {
        CAPTURE (sr);
        const int64_t stop = sr > 50000.0 ? 2 * 430080 : 430080;
        VoiceSettings s;
        s.range = VoiceRange::tenor;
        s.bendMode = BendMode::trueBend;
        s.legato = true;

        auto synced = onsetMelody (sr, VoiceRange::tenor, static_cast<int64_t> (0.2 * sr), 3).signal;
        auto vib = test::vibrato (sr, 233.08, 50.0, 5.5, 0.6, 0.3f);
        synced.insert (synced.end(), vib.begin(), vib.end());
        REQUIRE (static_cast<int64_t> (synced.size()) < stop - static_cast<int64_t> (0.3 * sr));
        synced.resize (static_cast<std::size_t> (stop + static_cast<int64_t> (sr)), 0.0f); // past the capture tail
        const auto free = test::toneSequence (sr, { { 196.0, 0.3 }, { 0.0, 0.2 }, { 246.94, 0.4 } }, 0.3f);
        auto input = synced;
        input.insert (input.end(), free.begin(), free.end());
        input.resize (input.size() + test::numSamplesFor (TakeSegmenter::defaultTakeGapSeconds + 0.6, sr), 0.0f);

        struct Result
        {
            std::vector<std::vector<uint8_t>> files;
            std::vector<std::vector<float>> audio;
            std::vector<Take> takes;
        };

        const auto runWith = [&] (int block)
        {
            Result result;
            CaptureRig rig (sr, 4096); // the host's prepared maximum, whatever it sends
            rig.run (input, s, block, { 0, stop, true });

            for (auto take : rig.closed)
            {
                rig.refine (take);

                for (const auto version : { ExportVersion::raw, ExportVersion::clean })
                    for (const auto grid : { QuantizeGrid::off, QuantizeGrid::sixteenth })
                    {
                        RenderOptions o;
                        o.version = version;
                        o.quantize = grid;
                        o.quantizeStrengthPercent = 50.0;
                        o.roundTripSamples = TakeRenderer::roundTripFor (take, CaptureOffsetSettings {});
                        const auto r = TakeRenderer::render (take, o);
                        SmfOptions smf;
                        smf.trackName = "VocalSynth Take " + std::to_string (take.number);
                        result.files.push_back (SmfWriter::write (r, smf));

                        if (version == ExportVersion::clean && grid == QuantizeGrid::off)
                            result.audio.push_back (rig.readAudio (r.audio));
                    }

                result.takes.push_back (std::move (take));
            }

            return result;
        };

        const auto reference = runWith (4096);
        REQUIRE (reference.takes.size() == 2);
        CHECK (reference.takes[0].mode == TakeMode::synced);
        CHECK (reference.takes[1].mode == TakeMode::free);
        CHECK (reference.takes[0].noteCount >= 12);

        for (const auto block : { 1, 7, 64, 480, 512 })
        {
            CAPTURE (block);
            const auto run = runWith (block);
            REQUIRE (run.takes.size() == reference.takes.size());

            for (std::size_t i = 0; i < run.takes.size(); ++i)
            {
                CAPTURE (i);
                CHECK (run.takes[i].startSample == reference.takes[i].startSample);
                CHECK (run.takes[i].endSample == reference.takes[i].endSample);
                CHECK (run.takes[i].captureEndSample == reference.takes[i].captureEndSample);
                CHECK (run.takes[i].events == reference.takes[i].events);
            }

            CHECK (run.files == reference.files);
            CHECK (run.audio == reference.audio);
        }
    }
}
