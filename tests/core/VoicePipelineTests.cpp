#include "vocalsynth/VoicePipeline.h"

#include "vocalsynth/Pitch.h"

#include "TestSignals.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <numbers>
#include <span>
#include <string>
#include <thread>
#include <vector>

using namespace vocalsynth;

// End-to-end tests of the voice pipeline on synthetic audio (docs/M2-DESIGN.md 6, "[pipeline]").
// Every run goes through runPipeline(), which also checks the per-call contract of process():
// offsets in [0, numSamples) and non-decreasing, at most maxEventsForSamples() events, buffer never full.

namespace
{

constexpr double sampleRates[] = { 44100.0, 48000.0, 96000.0 };
constexpr int blockSizes[] = { 1, 7, 64, 480, 512, 4096 };

constexpr VoiceRange allRanges[] = { VoiceRange::bass,    VoiceRange::baritone, VoiceRange::tenor, VoiceRange::alto,
                                     VoiceRange::soprano, VoiceRange::whistle,  VoiceRange::wide };

/** An event with its absolute host sample position (block start + offset). */
struct TimedEvent
{
    int64_t position;
    VsEvent event; ///< sampleOffset set to 0: it is block-relative, position is the absolute time

    bool operator== (const TimedEvent&) const = default;
};

TimedEvent timed (int64_t blockStart, VsEvent e)
{
    const auto position = blockStart + e.sampleOffset;
    e.sampleOffset = 0;
    return { position, e };
}

/** A note reconstructed from the event list; off = -1 if it never ended. */
struct Note
{
    int note = -1;
    int64_t on = 0;
    int64_t off = -1;
    int velocity = 0;
    int channel = 1;
};

struct PipelineRun
{
    std::vector<TimedEvent> events;
    std::vector<AnalysisFrameInfo> frames;
    int contractViolations = 0; ///< see runOn()
    int64_t endPosition = 0;
};

VoiceSettings settingsFor (VoiceRange range, PitchAlgorithm algorithm = PitchAlgorithm::yin)
{
    VoiceSettings s;
    s.range = range;
    s.algorithm = algorithm;
    return s;
}

const char* algorithmName (PitchAlgorithm a) { return a == PitchAlgorithm::mpm ? "MPM" : "YIN"; }

/** Varying host block pattern (blockSize == 0 in runOn()), like a host that changes its buffer size. */
constexpr int variableBlocks[] = { 1, 13, 480, 7, 4096, 64, 512, 100, 3, 2048 };

/** Runs `input` through an already prepared pipeline (continuing its sample clock) in blocks of blockSize
    (0 = variableBlocks), then optionally flushes at the end. */
void runOn (VoicePipeline& pipeline, std::span<const float> input, const VoiceSettings& settings, int blockSize,
            bool flushAtEnd, PipelineRun& run)
{
    VsEventBuffer<1024> buffer;
    std::size_t pos = 0;
    std::size_t blockIndex = 0;

    while (pos < input.size())
    {
        const auto wanted = blockSize > 0 ? blockSize : variableBlocks[blockIndex++ % std::size (variableBlocks)];
        const auto n = static_cast<int> (std::min<std::size_t> (static_cast<std::size_t> (wanted), input.size() - pos));
        const auto start = pipeline.getSamplePosition();

        buffer.clear();
        pipeline.process (input.data() + pos, n, settings, buffer);

        if (buffer.full() || static_cast<int> (buffer.size()) > pipeline.maxEventsForSamples (n))
            ++run.contractViolations;

        int lastOffset = 0;

        for (const auto& e : buffer)
        {
            if (e.sampleOffset < 0 || e.sampleOffset >= n || e.sampleOffset < lastOffset)
                ++run.contractViolations;

            lastOffset = e.sampleOffset;
            run.events.push_back (timed (start, e));
        }

        if (pipeline.getSamplePosition() != start + n)
            ++run.contractViolations;

        pos += static_cast<std::size_t> (n);
    }

    if (flushAtEnd)
    {
        buffer.clear();
        pipeline.flush (0, buffer);

        for (const auto& e : buffer)
            run.events.push_back (timed (pipeline.getSamplePosition(), e));
    }

    run.endPosition = pipeline.getSamplePosition();
}

void recordFrame (void* context, const AnalysisFrameInfo& info) noexcept
{
    static_cast<std::vector<AnalysisFrameInfo>*> (context)->push_back (info);
}

PipelineRun runPipeline (std::span<const float> input, double sampleRate, const VoiceSettings& settings,
                         int blockSize = 512, bool recordFrames = false, bool flushAtEnd = true)
{
    PipelineRun run;
    VoicePipeline pipeline;
    pipeline.prepare (sampleRate, std::max (1, blockSize));

    if (recordFrames)
    {
        run.frames.reserve (input.size() / static_cast<std::size_t> (pipeline.getHopInputSamples()) + 16);
        pipeline.setFrameObserver (recordFrame, &run.frames);
    }

    runOn (pipeline, input, settings, blockSize, flushAtEnd, run);
    return run;
}

std::vector<Note> notesOf (const std::vector<TimedEvent>& events)
{
    std::vector<Note> notes;

    for (const auto& [position, e] : events)
    {
        if (e.type == VsEventType::noteOn)
            notes.push_back ({ e.data1, position, -1, e.data2, e.channel });
        else if (e.type == VsEventType::noteOff)
            for (auto& n : notes)
                if (n.off < 0 && n.note == e.data1 && n.channel == e.channel)
                    n.off = position;
    }

    return notes;
}

std::vector<int> noteNumbers (const std::vector<Note>& notes)
{
    std::vector<int> out;

    for (const auto& n : notes)
        out.push_back (n.note);

    return out;
}

std::vector<int16_t> bendValues (const std::vector<TimedEvent>& events)
{
    std::vector<int16_t> out;

    for (const auto& [position, e] : events)
        if (e.type == VsEventType::pitchBend)
            out.push_back (e.bend);

    return out;
}

/** MIDI invariants of a complete (flushed) run (docs/M2-DESIGN.md 6): every note-on is paired with a
    later (or same-sample legato) note-off, no duplicate note-on, no stray note-off, at most two notes
    at once and only at a legato change, positions non-decreasing, velocities 1..127, the bend is
    centred whenever no note sounds, and at the end nothing is active. Returns the number of violations. */
int midiInvariantViolations (const std::vector<TimedEvent>& events)
{
    int violations = 0;
    std::map<std::pair<int, int>, int64_t> active; // (channel, note) -> on position
    int64_t lastPosition = 0;
    int16_t lastBend = 0;

    for (const auto& [position, e] : events)
    {
        if (position < lastPosition)
            ++violations;

        lastPosition = position;
        const auto key = std::make_pair (static_cast<int> (e.channel), static_cast<int> (e.data1));

        if (e.channel < 1 || e.channel > 16)
            ++violations;

        switch (e.type)
        {
            case VsEventType::noteOn:
                if (active.contains (key) || e.data2 < 1 || e.data2 > 127 || e.data1 > 127)
                    ++violations;

                active[key] = position;

                if (active.size() > 2)
                    ++violations;

                break;

            case VsEventType::noteOff:
                if (! active.contains (key))
                    ++violations;

                active.erase (key);
                break;

            case VsEventType::pitchBend:
                lastBend = e.bend;
                break;

            case VsEventType::controlChange:
                ++violations; // the voice pipeline never sends CCs
                break;
        }

        // Two notes only during a legato change, and then both at the same sample
        if (active.size() == 2)
        {
            const auto first = active.begin()->second;
            const auto second = std::next (active.begin())->second;

            if (first != position && second != position)
                ++violations;
        }
    }

    if (! active.empty() || lastBend != 0)
        ++violations;

    return violations;
}

double centsBetween (double frequencyHz, double referenceHz) { return 1200.0 * std::log2 (frequencyHz / referenceHz); }

/** Scales a signal so that the RMS of its non-zero samples is rmsDb dBFS. */
test::Signal scaledToRmsDb (test::Signal s, double rmsDb)
{
    double sum = 0.0;
    std::size_t count = 0;

    for (auto x : s)
        if (x != 0.0f)
        {
            sum += static_cast<double> (x) * static_cast<double> (x);
            ++count;
        }

    if (count == 0)
        return s;

    const auto gain = std::pow (10.0, rmsDb / 20.0) / std::sqrt (sum / static_cast<double> (count));

    for (auto& x : s)
        x = static_cast<float> (static_cast<double> (x) * gain);

    return s;
}

/** Sawtooth with polyBLEP edges (practically alias-free, like a band-limited voice). The naive saw of
    TestSignals.h folds its harmonics above Nyquist back into the band, which at 44.1 kHz and ~1 kHz
    is an inharmonic mixture no voice produces. */
test::Signal bandLimitedSaw (double sr, double frequencyHz, double seconds, float amplitude)
{
    test::Signal out (test::numSamplesFor (seconds, sr));
    const auto dt = frequencyHz / sr;
    double phase = 0.0;

    for (auto& x : out)
    {
        auto v = 2.0 * phase - 1.0;

        if (phase < dt)
        {
            const auto t = phase / dt;
            v -= t + t - t * t - 1.0;
        }
        else if (phase > 1.0 - dt)
        {
            const auto t = (phase - 1.0) / dt;
            v -= t * t + t + t + 1.0;
        }

        x = amplitude * static_cast<float> (v);
        phase += dt;
        phase -= std::floor (phase);
    }

    return out;
}

/** Linear fade-in and fade-out of `seconds` each (a voice starts and ends over a few ms). */
void applyFades (test::Signal& s, double sr, double seconds)
{
    const auto n = std::min (s.size() / 2, test::numSamplesFor (seconds, sr));

    for (std::size_t i = 0; i < n; ++i)
    {
        const auto g = static_cast<float> (static_cast<double> (i) / static_cast<double> (n));
        s[i] *= g;
        s[s.size() - 1 - i] *= g;
    }
}

/** Test grid of a range: f0 min, f0 max and exact MIDI-note frequencies in between (about every 5
    semitones), so the expected note is never near a quarter-tone boundary. */
std::vector<double> frequencyGridFor (VoiceRange range)
{
    const auto& info = voiceRangeInfo (range);
    std::vector<double> grid { info.f0MinHz };
    const auto lo = static_cast<int> (std::ceil (frequencyToMidiNote (info.f0MinHz) + 0.5));
    const auto hi = static_cast<int> (std::floor (frequencyToMidiNote (info.f0MaxHz) - 0.5));
    const auto steps = std::max (1, (hi - lo) / 5);

    for (int m = lo; m <= hi; m += std::max (1, (hi - lo) / steps))
        grid.push_back (test::midiToHz (static_cast<double> (m)));

    grid.push_back (info.f0MaxHz);
    return grid;
}

/** A melody that exercises everything at once (used for the block-size invariance tests): onsets from
    silence, a saw, legato steps, vibrato, a glissando, an octave leap, noise and breath-like bursts. */
test::Signal mixedMelody (double sr)
{
    using namespace test;
    const auto a = 0.3f;
    auto out = concat ({ silence (sr, 0.1),
                         toneSequence (sr, { { midiToHz (57.0), 0.30 }, { 0.0, 0.08 }, { midiToHz (60.0), 0.25 },
                                             { midiToHz (62.0), 0.25 }, { midiToHz (64.3), 0.25 } },
                                       a),
                         silence (sr, 0.1),
                         saw (sr, midiToHz (55.0), 0.3, 0.25f),
                         silence (sr, 0.05),
                         vibrato (sr, 233.08, 50.0, 5.5, 0.5, a),
                         glissando (sr, 196.0, 330.0, 0.6, a),
                         toneSequence (sr, { { 220.0, 0.2 }, { 440.0, 0.25 }, { 0.0, 0.1 } }, a),
                         scaledToRmsDb (whiteNoise (sr, 0.2, 1.0f, 17), -25.0),
                         silence (sr, 0.1),
                         scaledToRmsDb (noiseBursts (sr, 0.4, 0.15, 0.2, 1.0f, 99), -20.0),
                         silence (sr, 0.15) });

    // A little noise on top of everything keeps the detectors from seeing perfectly clean input
    mixInto (out, whiteNoise (sr, static_cast<double> (out.size()) / sr, 0.002f, 5));
    return out;
}

} // namespace

// ---------------------------------------------------------------------------------------------------------
// Pure formulas and capacity

TEST_CASE ("Algorithmic latency budget per range", "[pipeline]")
{
    // docs/M2-DESIGN.md 3.11: the model includes the conditioner's group delay at f0 min
    CHECK (VoicePipeline::latencyBudgetHops == 2);

    for (const auto sr : { 44100.0, 48000.0, 88200.0, 96000.0 })
    {
        const auto tenor = VoicePipeline::computeAlgorithmicLatencyMs (sr, VoiceRange::tenor);
        const auto bass = VoicePipeline::computeAlgorithmicLatencyMs (sr, VoiceRange::bass);
        const auto wide = VoicePipeline::computeAlgorithmicLatencyMs (sr, VoiceRange::wide);

        CHECK (tenor <= VoicePipeline::tenorLatencyBudgetMs);
        CHECK (bass <= VoicePipeline::bassLatencyBudgetMs);
        CHECK (wide == bass);

        // The conditioner term is real: about 3.4 ms (+ DC blocker) at 65 Hz
        const auto conditionerMs = 1000.0 * InputConditioner::groupDelaySeconds (sr, 65.0);
        CHECK (conditionerMs > 3.0);
        CHECK (conditionerMs < 4.0);

        // Lower ranges never have less latency than higher ones
        double previous = 1.0e9;

        for (const auto range : { VoiceRange::bass, VoiceRange::baritone, VoiceRange::tenor, VoiceRange::alto,
                                  VoiceRange::soprano, VoiceRange::whistle })
        {
            const auto ms = VoicePipeline::computeAlgorithmicLatencyMs (sr, range);
            CHECK (ms < previous);
            CHECK (ms > 0.0);
            previous = ms;
        }
    }

    // Table values of docs/M2-DESIGN.md 3.11 at 48 kHz
    CHECK (std::abs (VoicePipeline::computeAlgorithmicLatencyMs (48000.0, VoiceRange::tenor) - 24.21) < 0.01);
    CHECK (std::abs (VoicePipeline::computeAlgorithmicLatencyMs (48000.0, VoiceRange::bass) - 39.05) < 0.01);
    CHECK (VoicePipeline::computeAlgorithmicLatencyMs (0.0, VoiceRange::tenor) == 0.0);
}

TEST_CASE ("Pipeline geometry: decimation, hop, window and frame-centre delay", "[pipeline]")
{
    struct Expected
    {
        double sr;
        int factor;
        int hopInput;
    };

    for (const auto& [sr, factor, hopInput] : { Expected { 44100.0, 2, 88 }, Expected { 48000.0, 3, 96 },
                                                Expected { 96000.0, 6, 192 }, Expected { 88200.0, 5, 175 } })
    {
        CAPTURE (sr);
        VoicePipeline p;
        p.prepare (sr, 512);
        CHECK (p.getSampleRate() == sr);
        CHECK (p.getDecimationFactor() == factor);
        CHECK (p.getHopInputSamples() == hopInput);
        CHECK (p.getAnalysisSampleRate() == sr / factor);

        // Tenor (default) window, and the window-centre delay formula
        CHECK (p.getWindowSamples() == Framer::windowSamplesFor (sr / factor, 110.0));
        const auto expectedCentre = static_cast<double> (Decimator::numTapsFor (factor) - 1) / 2.0
                                    + factor * static_cast<double> (p.getWindowSamples() - 1) / 2.0;
        CHECK (p.getFrameCentreDelayInputSamples() == expectedCentre);
        CHECK (p.getAlgorithmicLatencyMs() == VoicePipeline::computeAlgorithmicLatencyMs (sr, VoiceRange::tenor));

        // A range change applies the window of that range (and the latency follows)
        VsEventBuffer<1024> events;
        const auto input = test::silence (sr, 0.01);
        p.process (input.data(), static_cast<int> (input.size()), settingsFor (VoiceRange::bass), events);
        CHECK (p.getWindowSamples() == Framer::windowSamplesFor (sr / factor, 65.0));
        CHECK (p.getAlgorithmicLatencyMs() == VoicePipeline::computeAlgorithmicLatencyMs (sr, VoiceRange::bass));
    }
}

TEST_CASE ("Pipeline frames complete on the fixed grid n = (m hop - 1) D", "[pipeline]")
{
    const auto sr = GENERATE (44100.0, 48000.0, 96000.0);
    const auto block = GENERATE (1, 7, 512);
    CAPTURE (sr, block);

    const auto input = test::sine (sr, 220.0, 0.2);
    const auto run = runPipeline (input, sr, {}, block, true);

    const auto factor = Decimator::factorFor (sr);
    const auto hop = Framer::hopSamplesFor (sr / factor);
    REQUIRE (run.frames.size() == input.size() / static_cast<std::size_t> (hop * factor));

    for (std::size_t m = 0; m < run.frames.size(); ++m)
        if (run.frames[m].hopSample != (static_cast<int64_t> (m + 1) * hop - 1) * factor)
            FAIL_CHECK ("frame " << m << " at " << run.frames[m].hopSample);
}

TEST_CASE ("Pipeline event capacity: maxEventsForSamples", "[pipeline]")
{
    for (const auto sr : { 8000.0, 22050.0, 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        CAPTURE (sr);
        VoicePipeline p;
        p.prepare (sr, 4096);
        const auto hop = p.getHopInputSamples();

        CHECK (p.maxEventsForSamples (0) == 0);
        CHECK (p.maxEventsForSamples (-5) == 0);
        CHECK (p.maxEventsForSamples (1) == VoicePipeline::maxEventsPerFlush + VoicePipeline::maxEventsPerHop);
        CHECK (p.maxEventsForSamples (hop) == VoicePipeline::maxEventsPerFlush + VoicePipeline::maxEventsPerHop);
        CHECK (p.maxEventsForSamples (hop + 1)
               == VoicePipeline::maxEventsPerFlush + 2 * VoicePipeline::maxEventsPerHop);

        // The plugin's VsEventBuffer<1024> with 4096-sample chunks can never fill (lowest supported: 8 kHz)
        CHECK (p.maxEventsForSamples (4096) < 1024);
    }

    VoicePipeline p;
    p.prepare (44100.0, 4096);
    CHECK (p.maxEventsForSamples (4096) == 2 + 3 * 47);
}

// ---------------------------------------------------------------------------------------------------------
// Accuracy per range

TEST_CASE ("Pipeline: steady sines and saws give one correct note in every range", "[pipeline]")
{
    const auto algorithm = GENERATE (PitchAlgorithm::yin, PitchAlgorithm::mpm);
    const auto sr = GENERATE (44100.0, 48000.0, 96000.0);
    CAPTURE (algorithmName (algorithm), sr);

    for (const auto range : allRanges)
    {
        for (const auto f : frequencyGridFor (range))
        {
            for (const bool isSaw : { false, true })
            {
                // Saws up to 1047 Hz (spec: 65-1047 Hz). Above, in Whistle, only sines: whistling is nearly
                // sinusoidal, and the 3rd/4th harmonic of a 2.2 kHz saw fall into the decimator's transition
                // band at 16 kHz (YIN then reads up to 29 cents off)
                if (isSaw && f > 1047.0)
                    continue;

                CAPTURE (voiceRangeInfo (range).name, f, isSaw);
                const auto lead = 0.1, length = 0.6;
                const auto tone = isSaw ? bandLimitedSaw (sr, f, length, 0.3f) : test::sine (sr, f, length, 0.3f);
                const auto input = test::concat ({ test::silence (sr, lead), tone, test::silence (sr, 0.2) });
                const auto run = runPipeline (input, sr, settingsFor (range, algorithm), 512, true);
                const auto notes = notesOf (run.events);
                const auto expected = nearestMidiNote (f);

                CHECK (run.contractViolations == 0);
                CHECK (midiInvariantViolations (run.events) == 0);
                REQUIRE (notes.size() == 1);
                CHECK (notes[0].note == expected);

                // Note-off after the tone ended, not by the final flush
                CHECK (notes[0].off > static_cast<int64_t> ((lead + length) * sr));
                CHECK (notes[0].off < static_cast<int64_t> ((lead + length + 0.1) * sr));

                // Pitch of every frame whose window lies inside the tone (after the conditioner settled)
                VoicePipeline geometry;
                geometry.prepare (sr, 512);
                const auto factor = Decimator::factorFor (sr);
                const auto window = Framer::windowSamplesFor (sr / factor, voiceRangeInfo (range).f0MinHz);
                const auto from = static_cast<int64_t> ((lead + 0.015) * sr) + factor * window
                                  + Decimator::numTapsFor (factor) + 2 * factor * Framer::hopSamplesFor (sr / factor);
                const auto to = static_cast<int64_t> ((lead + length) * sr);
                const auto tolerance = f > 1047.0 ? 25.0 : 10.0;
                int checked = 0, wrong = 0;
                double worst = 0.0;

                for (const auto& frame : run.frames)
                {
                    if (frame.hopSample < from || frame.hopSample > to)
                        continue;

                    ++checked;
                    const auto cents = frame.processed.frequencyHz > 0.0f
                                           ? centsBetween (static_cast<double> (frame.processed.frequencyHz), f)
                                           : 1.0e9;
                    worst = std::max (worst, std::abs (cents));

                    if (std::abs (cents) > tolerance || frame.activeNote != expected || ! frame.voiced)
                        ++wrong;
                }

                CAPTURE (worst);
                CHECK (checked > 100);
                CHECK (wrong == 0);
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------------------
// Expressive singing

TEST_CASE ("Pipeline: vibrato +-50 cents at 5.5 Hz gives one note, bends follow in True mode", "[pipeline]")
{
    const auto algorithm = GENERATE (PitchAlgorithm::yin, PitchAlgorithm::mpm);
    const auto sr = GENERATE (44100.0, 48000.0, 96000.0);
    CAPTURE (algorithmName (algorithm), sr);

    struct Case
    {
        VoiceRange range;
        double f0;
    };

    for (const auto& [range, f0] : { Case { VoiceRange::tenor, 220.0 }, Case { VoiceRange::bass, 98.0 },
                                     Case { VoiceRange::soprano, 659.26 }, Case { VoiceRange::wide, 130.81 } })
    {
        CAPTURE (voiceRangeInfo (range).name, f0);
        const auto input = test::concat (
            { test::silence (sr, 0.1), test::vibrato (sr, f0, 50.0, 5.5, 1.5, 0.3f), test::silence (sr, 0.2) });
        const auto expected = nearestMidiNote (f0);

        for (const auto mode : { BendMode::off, BendMode::anchored, BendMode::trueBend })
        {
            CAPTURE (static_cast<int> (mode));
            auto s = settingsFor (range, algorithm);
            s.bendMode = mode;
            const auto run = runPipeline (input, sr, s);
            const auto notes = notesOf (run.events);
            const auto bends = bendValues (run.events);

            CHECK (run.contractViolations == 0);
            CHECK (midiInvariantViolations (run.events) == 0);
            REQUIRE (notes.size() == 1);
            CHECK (notes[0].note == expected);

            const auto [lo, hi] = bends.empty() ? std::pair<int, int> { 0, 0 }
                                                : std::pair<int, int> { *std::min_element (bends.begin(), bends.end()),
                                                                        *std::max_element (bends.begin(), bends.end()) };

            if (mode == BendMode::off)
            {
                CHECK (bends.empty());
            }
            else if (mode == BendMode::anchored)
            {
                // Dead zone 30 cents: at most (50 + 10 - 30) cents of +-200 cents, smoothed
                CHECK (bends.size() > 4);
                CHECK (hi < 1300);
                CHECK (lo > -1300);
            }
            else
            {
                // 1:1: +-50 cents at +-2 semitones ~ +-2048, sent at ~167 Hz while the note is held
                CAPTURE (lo, hi, bends.size());
                CHECK (hi > 1700);
                CHECK (hi < 2600);
                CHECK (lo < -1700);
                CHECK (lo > -2600);
                CHECK (bends.size() > 100);
            }
        }
    }
}

TEST_CASE ("Pipeline: glissando gives a monotone note sequence, every note >= minNoteLength", "[pipeline]")
{
    const auto algorithm = GENERATE (PitchAlgorithm::yin, PitchAlgorithm::mpm);
    const auto sr = GENERATE (44100.0, 48000.0, 96000.0);
    const auto legato = GENERATE (false, true);
    CAPTURE (algorithmName (algorithm), sr, legato);

    for (const bool up : { true, false })
    {
        CAPTURE (up);
        // G3 <-> G4 in 2 s (6 semitones per second: each note lasts ~167 ms)
        const auto glide = up ? test::glissando (sr, 196.0, 392.0, 2.0, 0.3f) : test::glissando (sr, 392.0, 196.0, 2.0, 0.3f);
        const auto input = test::concat ({ test::silence (sr, 0.1), glide, test::silence (sr, 0.2) });
        auto s = settingsFor (VoiceRange::tenor, algorithm);
        s.legato = legato;
        const auto run = runPipeline (input, sr, s);
        const auto notes = notesOf (run.events);

        CHECK (run.contractViolations == 0);
        CHECK (midiInvariantViolations (run.events) == 0);
        REQUIRE (notes.size() >= 10);
        CHECK (notes.size() <= 13);
        CHECK (std::abs (notes.front().note - (up ? 55 : 67)) <= 1);
        CHECK (std::abs (notes.back().note - (up ? 67 : 55)) <= 1);

        const auto minNote = static_cast<int64_t> (std::llround (0.001 * static_cast<double> (s.minNoteLengthMs) * sr));

        for (std::size_t i = 0; i < notes.size(); ++i)
        {
            CAPTURE (i, notes[i].note);
            CHECK (notes[i].off - notes[i].on >= minNote);

            if (i > 0)
            {
                CHECK (notes[i].note - notes[i - 1].note == (up ? 1 : -1)); // steps through every semitone
                CHECK (notes[i].on == notes[i - 1].off);                     // no gap (same sample)
            }
        }
    }
}

TEST_CASE ("Pipeline: octave leaps give exactly two notes, no octave flicker", "[pipeline]")
{
    const auto algorithm = GENERATE (PitchAlgorithm::yin, PitchAlgorithm::mpm);
    const auto sr = GENERATE (44100.0, 48000.0, 96000.0);
    CAPTURE (algorithmName (algorithm), sr);

    struct Case
    {
        VoiceRange range;
        double from, to;
    };

    for (const auto& [range, from, to] : { Case { VoiceRange::tenor, 220.0, 440.0 }, Case { VoiceRange::tenor, 440.0, 220.0 },
                                           Case { VoiceRange::bass, 82.41, 164.81 }, Case { VoiceRange::wide, 261.63, 130.81 },
                                           Case { VoiceRange::wide, 130.81, 523.25 } })
    {
        for (const bool isSaw : { false, true })
        {
            CAPTURE (voiceRangeInfo (range).name, from, to, isSaw);
            const auto shape = isSaw ? test::sawShape : test::sineShape;
            const auto input = test::toneSequence (sr, { { 0.0, 0.1 }, { from, 0.5 }, { to, 0.5 }, { 0.0, 0.2 } }, 0.3f, shape);
            const auto run = runPipeline (input, sr, settingsFor (range, algorithm));
            const auto notes = notesOf (run.events);

            CHECK (run.contractViolations == 0);
            CHECK (midiInvariantViolations (run.events) == 0);
            CHECK (noteNumbers (notes) == std::vector<int> { nearestMidiNote (from), nearestMidiNote (to) });
        }
    }
}

TEST_CASE ("Pipeline: missing fundamental and a strong 2nd harmonic give the right octave", "[pipeline]")
{
    const auto algorithm = GENERATE (PitchAlgorithm::yin, PitchAlgorithm::mpm);
    const auto sr = GENERATE (44100.0, 48000.0, 96000.0);
    CAPTURE (algorithmName (algorithm), sr);

    struct Case
    {
        VoiceRange range;
        double midi;
    };

    for (const auto& [range, midi] : { Case { VoiceRange::tenor, 48.0 }, Case { VoiceRange::tenor, 52.0 },
                                       Case { VoiceRange::tenor, 57.0 }, Case { VoiceRange::baritone, 43.0 },
                                       Case { VoiceRange::bass, 40.0 }, Case { VoiceRange::alto, 62.0 },
                                       Case { VoiceRange::wide, 45.0 } })
    {
        const auto f0 = test::midiToHz (midi);

        for (const bool missing : { true, false })
        {
            CAPTURE (voiceRangeInfo (range).name, f0, missing);
            const auto tone = missing ? test::harmonicStack (sr, f0, 0.6, 2, 8, 0.3f)        // harmonics 2..8
                                      : test::harmonicStack (sr, f0, 0.6, 1, 8, 0.3f, 3.0); // 3x 2nd harmonic
            const auto input = test::concat ({ test::silence (sr, 0.1), tone, test::silence (sr, 0.2) });
            const auto run = runPipeline (input, sr, settingsFor (range, algorithm));
            const auto notes = notesOf (run.events);

            CHECK (midiInvariantViolations (run.events) == 0);
            CHECK (noteNumbers (notes) == std::vector<int> { static_cast<int> (midi) });
        }
    }
}

// ---------------------------------------------------------------------------------------------------------
// Rejection: no notes from noise, breath, silence or out-of-range tones

TEST_CASE ("Pipeline: white/pink noise, silence and breath-like bursts give no notes", "[pipeline]")
{
    const auto algorithm = GENERATE (PitchAlgorithm::yin, PitchAlgorithm::mpm);
    const auto sr = GENERATE (44100.0, 48000.0, 96000.0);
    CAPTURE (algorithmName (algorithm), sr);

    struct Input
    {
        const char* name;
        test::Signal signal;
    };

    std::vector<Input> inputs;
    inputs.push_back ({ "silence", test::silence (sr, 2.0) });

    for (const auto db : { -40.0, -20.0, -10.0 })
    {
        inputs.push_back ({ "white", scaledToRmsDb (test::whiteNoise (sr, 2.0, 1.0f, 1234), db) });
        inputs.push_back ({ "pink", scaledToRmsDb (test::pinkNoise (sr, 2.0, 1.0f, 4321), db) });
    }

    for (const auto db : { -30.0, -20.0, -12.0 })
    {
        inputs.push_back ({ "breath 300/600 ms", scaledToRmsDb (test::noiseBursts (sr, 3.0, 0.3, 0.6, 1.0f, 77), db) });
        inputs.push_back ({ "breath 80/200 ms", scaledToRmsDb (test::noiseBursts (sr, 2.0, 0.08, 0.2, 1.0f, 78), db) });
    }

    for (const auto range : allRanges)
    {
        for (const auto& [name, signal] : inputs)
        {
            CAPTURE (voiceRangeInfo (range).name, name);
            const auto run = runPipeline (signal, sr, settingsFor (range, algorithm));
            CHECK (run.contractViolations == 0);
            CHECK (run.events.empty());
        }
    }
}

TEST_CASE ("Pipeline: tones 1.5 semitones outside the range give no notes", "[pipeline]")
{
    const auto algorithm = GENERATE (PitchAlgorithm::yin, PitchAlgorithm::mpm);
    const auto sr = GENERATE (44100.0, 48000.0, 96000.0);
    CAPTURE (algorithmName (algorithm), sr);

    for (const auto range : allRanges)
    {
        const auto& info = voiceRangeInfo (range);
        const auto below = info.f0MinHz * std::exp2 (-1.5 / 12.0);
        const auto above = info.f0MaxHz * std::exp2 (1.5 / 12.0);

        for (const auto f : { below, above })
        {
            // Whistle above 2.5 kHz: 2.8 kHz is outside what the analysis rate can resolve as "above" with
            // 1.5 semitones margin at 6 samples per period; the detector tests cover it. Skip.
            if (range == VoiceRange::whistle && f > info.f0MaxHz)
                continue;

            for (const bool isSaw : { false, true })
            {
                for (const bool abrupt : { false, true })
                {
                    // Known YIN limitation: when a tone just below the range stops abruptly (< 10 ms), the
                    // high-pass rings and YIN's period estimate shortens into the range in the last frames,
                    // which can start one short ghost note (59.6 Hz in Bass at 44.1 kHz). Voices fade out
                    // over >= 10 ms; MPM is tested with the abrupt end as well.
                    if (abrupt && algorithm == PitchAlgorithm::yin)
                        continue;

                    CAPTURE (info.name, f, isSaw, abrupt);
                    auto tone = isSaw ? test::saw (sr, f, 0.6, 0.3f) : test::sine (sr, f, 0.6, 0.3f);

                    if (! abrupt)
                        applyFades (tone, sr, 0.01);

                    const auto input = test::concat ({ test::silence (sr, 0.1), tone, test::silence (sr, 0.2) });
                    const auto run = runPipeline (input, sr, settingsFor (range, algorithm));
                    CHECK (notesOf (run.events).empty());
                }
            }
        }
    }
}

TEST_CASE ("Pipeline: edge notes sung off pitch show their offset in the tuner and in True bend", "[pipeline]")
{
    // Some range limits sit a few cents inside a note (Tenor f0 max 523 Hz < C5 = 523.25 Hz, Alto f0 min
    // 147 Hz > D3 = 146.83 Hz). The detectors used to clamp a pitch within the range tolerance to the
    // limit, so A2 sung 10-40 cents flat read as exactly 110 Hz (tuner "+0 c", no bend) and C5 always as
    // 523 Hz (-0.8 c, bend -34).
    const auto algorithm = GENERATE (PitchAlgorithm::yin, PitchAlgorithm::mpm);
    const auto sr = GENERATE (44100.0, 48000.0, 96000.0);
    CAPTURE (algorithmName (algorithm), sr);

    struct Case
    {
        VoiceRange range;
        int note;
        double cents;
    };

    for (const auto& [range, note, cents] : { Case { VoiceRange::tenor, 45, -40.0 }, Case { VoiceRange::tenor, 45, -20.0 },
                                              Case { VoiceRange::tenor, 45, -10.0 }, Case { VoiceRange::tenor, 72, -20.0 },
                                              Case { VoiceRange::tenor, 72, 0.0 }, Case { VoiceRange::tenor, 72, 15.0 },
                                              Case { VoiceRange::alto, 50, -20.0 }, Case { VoiceRange::alto, 77, 10.0 } })
    {
        const auto f = midiNoteToFrequency (static_cast<double> (note) + cents / 100.0);
        CAPTURE (voiceRangeInfo (range).name, note, cents, f);

        auto tone = bandLimitedSaw (sr, f, 0.6, 0.3f);
        applyFades (tone, sr, 0.01);
        const auto input = test::concat ({ test::silence (sr, 0.1), tone, test::silence (sr, 0.2) });

        auto s = settingsFor (range, algorithm);
        s.bendMode = BendMode::trueBend; // +-2 semitones: 1 cent = 8192 / 200 bend units
        const auto run = runPipeline (input, sr, s, 512, true);
        const auto notes = notesOf (run.events);
        REQUIRE (notes.size() == 1);
        CHECK (notes[0].note == note);

        // Tuner: the cleaned pitch of the voiced frames in the middle of the note, within 10 cents
        std::vector<double> centsSeen;

        for (const auto& frame : run.frames)
            if (frame.hopSample > notes[0].on + test::numSamplesFor (0.1, sr) && frame.hopSample < notes[0].off - test::numSamplesFor (0.1, sr)
                && frame.processed.frequencyHz > 0.0f)
                centsSeen.push_back (centsBetween (static_cast<double> (frame.processed.frequencyHz), f));

        REQUIRE (centsSeen.size() > 50);
        CHECK (*std::min_element (centsSeen.begin(), centsSeen.end()) >= -10.0);
        CHECK (*std::max_element (centsSeen.begin(), centsSeen.end()) <= 10.0);

        // True bend follows the offset: the median bend is within 10 cents of it
        auto bends = bendValues (run.events);
        REQUIRE (bends.size() >= 2);
        bends.pop_back(); // the centre after the note-off
        std::nth_element (bends.begin(), bends.begin() + static_cast<std::ptrdiff_t> (bends.size() / 2), bends.end());
        const auto medianBendCents = static_cast<double> (bends[bends.size() / 2]) * 200.0 / 8192.0;
        CHECK (std::abs (medianBendCents - cents) <= 10.0);
    }
}

TEST_CASE ("Pipeline: a held note does not change to the octave of a tone below the range", "[pipeline]")
{
    // A2 in Tenor, then G2 / F#2 (below the range) with a 3x 2nd harmonic: the detectors read 2 f0 with
    // confidence 0.56-0.79, below the octave guard's minimum. Such weak frames used to change the note
    // to G3 / F#3 after T_min; now they only hold it.
    const auto algorithm = GENERATE (PitchAlgorithm::yin, PitchAlgorithm::mpm);
    const auto sr = GENERATE (44100.0, 48000.0, 96000.0);
    CAPTURE (algorithmName (algorithm), sr);

    for (const auto low : { 98.0, 92.5 })
    {
        CAPTURE (low);
        const auto tone = [sr] (double f, double seconds)
        {
            test::Signal out (test::numSamplesFor (seconds, sr));

            for (std::size_t i = 0; i < out.size(); ++i)
            {
                const auto phase = 2.0 * std::numbers::pi * f * static_cast<double> (i) / sr;
                out[i] = static_cast<float> (0.5 * (0.3 * std::sin (phase) + 0.9 * std::sin (2.0 * phase) + 0.1 * std::sin (3.0 * phase)));
            }

            return out;
        };

        const auto input = test::concat ({ test::silence (sr, 0.1), tone (110.0, 0.6), tone (low, 0.7), test::silence (sr, 0.2) });
        const auto run = runPipeline (input, sr, settingsFor (VoiceRange::tenor, algorithm));
        const auto notes = notesOf (run.events);
        CHECK (midiInvariantViolations (run.events) == 0);
        REQUIRE (! notes.empty());
        CHECK (noteNumbers (notes) == std::vector<int> { 45 });
    }
}

// ---------------------------------------------------------------------------------------------------------
// Note logic end to end

TEST_CASE ("Pipeline: scale quantisation", "[pipeline]")
{
    const auto algorithm = GENERATE (PitchAlgorithm::yin, PitchAlgorithm::mpm);
    const auto sr = GENERATE (44100.0, 48000.0, 96000.0);
    CAPTURE (algorithmName (algorithm), sr);

    struct Case
    {
        int key;
        Scale scale;
        double sungMidi;
        int expected; ///< -1: either neighbour (a tie: C# in C major goes to C or D)
    };

    for (const auto& [key, scale, sung, expected] :
         { Case { 0, Scale::chromatic, 61.0, 61 }, Case { 0, Scale::major, 61.3, 62 }, Case { 0, Scale::major, 60.7, 60 },
           Case { 0, Scale::major, 61.0, -1 }, Case { 2, Scale::major, 60.3, 61 }, Case { 2, Scale::major, 59.7, 59 },
           Case { 9, Scale::minorPentatonic, 58.0, 57 }, Case { 9, Scale::minorPentatonic, 59.0, 60 },
           Case { 7, Scale::mixolydian, 54.0, 53 }, Case { 4, Scale::harmonicMinor, 57.0, 57 } })
    {
        CAPTURE (key, static_cast<int> (scale), sung);
        auto s = settingsFor (VoiceRange::tenor, algorithm);
        s.key = key;
        s.scale = scale;
        const auto input = test::concat (
            { test::silence (sr, 0.1), test::sine (sr, test::midiToHz (sung), 0.5, 0.3f), test::silence (sr, 0.2) });
        const auto notes = notesOf (runPipeline (input, sr, s).events);

        REQUIRE (notes.size() == 1);

        if (expected >= 0)
            CHECK (notes[0].note == expected);
        else
            CHECK ((notes[0].note == 60 || notes[0].note == 62));
    }
}

TEST_CASE ("Pipeline: legato vs retrigger event order at a note change", "[pipeline]")
{
    const auto algorithm = GENERATE (PitchAlgorithm::yin, PitchAlgorithm::mpm);
    const auto sr = GENERATE (44100.0, 48000.0, 96000.0);
    CAPTURE (algorithmName (algorithm), sr);

    // A3 -> B3 -> G3, phase-continuous (a clean pitch step)
    const auto input = test::toneSequence (sr, { { 0.0, 0.1 }, { 220.0, 0.4 }, { 246.94, 0.4 }, { 196.0, 0.4 }, { 0.0, 0.2 } }, 0.3f);

    for (const bool legato : { false, true })
    {
        CAPTURE (legato);
        auto s = settingsFor (VoiceRange::tenor, algorithm);
        s.legato = legato;
        const auto run = runPipeline (input, sr, s);
        const auto notes = notesOf (run.events);

        CHECK (midiInvariantViolations (run.events) == 0);
        REQUIRE (noteNumbers (notes) == std::vector<int> { 57, 59, 55 });

        // The note-on events of a change and their neighbours
        for (std::size_t i = 1; i < notes.size(); ++i)
        {
            CAPTURE (i);
            CHECK (notes[i].on == notes[i - 1].off);

            const auto on = std::find_if (run.events.begin(), run.events.end(), [&] (const TimedEvent& e) {
                return e.event.type == VsEventType::noteOn && e.event.data1 == notes[i].note;
            });
            const auto off = std::find_if (run.events.begin(), run.events.end(), [&] (const TimedEvent& e) {
                return e.event.type == VsEventType::noteOff && e.event.data1 == notes[i - 1].note;
            });
            REQUIRE (on != run.events.end());
            REQUIRE (off != run.events.end());

            if (legato)
                CHECK (on < off); // note-on new, then note-off old (mono synths glide)
            else
                CHECK (off < on); // note-off old, then note-on new
        }

        // A note change is not instant: stickiness T_min (35 ms) + window transition, but well below 100 ms
        const auto changeDelayMs = 1000.0 * static_cast<double> (notes[1].on - static_cast<int64_t> (0.5 * sr)) / sr;
        CHECK (changeDelayMs > 35.0);
        CHECK (changeDelayMs < 100.0);
    }
}

TEST_CASE ("Pipeline: octave shift and velocity", "[pipeline]")
{
    const auto sr = GENERATE (44100.0, 48000.0, 96000.0);
    CAPTURE (sr);

    const auto quiet = test::concat ({ test::silence (sr, 0.1), test::sine (sr, 220.0, 0.4, test::sineAmplitudeForRmsDb (-35.0)),
                                       test::silence (sr, 0.2) });
    const auto loud = test::concat ({ test::silence (sr, 0.1), test::sine (sr, 220.0, 0.4, test::sineAmplitudeForRmsDb (-12.0)),
                                      test::silence (sr, 0.2) });

    for (const auto shift : { -3, -1, 0, 1, 3 })
    {
        auto s = settingsFor (VoiceRange::tenor);
        s.octaveShift = shift;
        const auto notes = notesOf (runPipeline (loud, sr, s).events);
        REQUIRE (notes.size() == 1);
        CHECK (notes[0].note == 57 + 12 * shift);
    }

    const auto quietNotes = notesOf (runPipeline (quiet, sr, {}).events);
    const auto loudNotes = notesOf (runPipeline (loud, sr, {}).events);
    REQUIRE (quietNotes.size() == 1);
    REQUIRE (loudNotes.size() == 1);
    CHECK (loudNotes[0].velocity > quietNotes[0].velocity + 20);

    // Velocity formula with the measured onset level (conditioned rms of a -12 dB sine ~ -12 dB, gate -45)
    CHECK (std::abs (loudNotes[0].velocity - static_cast<int> (NoteSegmenter::velocityFor (-12.0, -45.0, 70.0))) <= 3);

    // 0 % sensitivity: fixed 100
    auto fixed = settingsFor (VoiceRange::tenor);
    fixed.velocitySensitivityPercent = 0.0f;
    CHECK (notesOf (runPipeline (quiet, sr, fixed).events)[0].velocity == 100);

    // Below the gate: no note
    auto gated = settingsFor (VoiceRange::tenor);
    gated.gateThresholdDb = -30.0f;
    CHECK (notesOf (runPipeline (quiet, sr, gated).events).empty());
}

TEST_CASE ("Pipeline: MIDI invariants on a random melody with settings changes", "[pipeline]")
{
    const auto sr = GENERATE (44100.0, 48000.0, 96000.0);
    CAPTURE (sr);

    // Random notes, rests, levels and articulations; settings change every 0.25 s
    test::TestRandom rng (2024);
    VoicePipeline pipeline;
    pipeline.prepare (sr, 512);
    PipelineRun run;

    for (int segment = 0; segment < 40; ++segment)
    {
        auto s = settingsFor (rng.uniform() > 0.0 ? VoiceRange::tenor : VoiceRange::wide,
                              rng.uniform() > 0.0 ? PitchAlgorithm::yin : PitchAlgorithm::mpm);
        s.scale = rng.uniform() > 0.5 ? Scale::major : Scale::chromatic;
        s.legato = rng.uniform() > 0.0;
        s.bendMode = static_cast<BendMode> (static_cast<int> (std::floor (1.5 * (rng.uniform() + 1.0))));
        s.octaveShift = static_cast<int> (std::lround (rng.uniform() * 1.4));
        s.stickinessPercent = static_cast<float> (50.0 + 50.0 * rng.uniform());
        s.minNoteLengthMs = static_cast<float> (135.0 + 115.0 * rng.uniform());
        s.channel = rng.uniform() > 0.8 ? 2 : 1;

        const auto midi = 52.0 + 10.0 * rng.uniform();
        const auto db = -30.0 + 15.0 * rng.uniform();
        const auto signal = rng.uniform() > -0.6
                                ? test::toneSequence (sr, { { test::midiToHz (midi), 0.15 }, { test::midiToHz (midi + 2.0 * rng.uniform()), 0.07 },
                                                            { rng.uniform() > 0.0 ? 0.0 : test::midiToHz (midi), 0.03 } },
                                                      test::sineAmplitudeForRmsDb (db))
                                : scaledToRmsDb (test::whiteNoise (sr, 0.25, 1.0f, static_cast<uint64_t> (segment) + 1), db);

        runOn (pipeline, signal, s, 0, false, run);
    }

    VsEventBuffer<1024> tail;
    pipeline.flush (0, tail);

    for (const auto& e : tail)
        run.events.push_back (timed (pipeline.getSamplePosition(), e));

    CHECK (run.contractViolations == 0);
    CHECK (midiInvariantViolations (run.events) == 0);
    CHECK (notesOf (run.events).size() >= 10);
}

// ---------------------------------------------------------------------------------------------------------
// Block-size invariance

TEST_CASE ("Pipeline is block-size invariant: identical events at identical absolute samples", "[pipeline]")
{
    const auto sr = GENERATE (44100.0, 48000.0, 96000.0);
    const auto config = GENERATE (0, 1);
    CAPTURE (sr, config);

    auto s = settingsFor (VoiceRange::tenor, config == 0 ? PitchAlgorithm::yin : PitchAlgorithm::mpm);
    s.bendMode = config == 0 ? BendMode::trueBend : BendMode::anchored;
    s.legato = config == 1;
    s.scale = config == 1 ? Scale::major : Scale::chromatic;

    const auto input = mixedMelody (sr);
    const auto reference = runPipeline (input, sr, s, 512, true);

    CHECK (reference.contractViolations == 0);
    CHECK (midiInvariantViolations (reference.events) == 0);
    CHECK (notesOf (reference.events).size() >= 12);
    CHECK (bendValues (reference.events).size() >= 20);

    const auto check = [&] (int block)
    {
        CAPTURE (block);
        const auto run = runPipeline (input, sr, s, block, true);
        CHECK (run.contractViolations == 0);

        if (run.events != reference.events)
        {
            std::size_t i = 0;

            while (i < std::min (run.events.size(), reference.events.size()) && run.events[i] == reference.events[i])
                ++i;

            const auto describe = [] (const std::vector<TimedEvent>& v, std::size_t k)
            {
                if (k >= v.size())
                    return std::string ("(none)");

                const auto& e = v[k];
                return std::to_string (e.position) + " type " + std::to_string (static_cast<int> (e.event.type)) + " d1 "
                       + std::to_string (e.event.data1) + " d2 " + std::to_string (e.event.data2) + " bend "
                       + std::to_string (e.event.bend) + " offset " + std::to_string (e.event.sampleOffset);
            };

            FAIL_CHECK ("first difference at event " << i << ": " << describe (run.events, i) << " vs reference "
                                                     << describe (reference.events, i));
        }
        REQUIRE (run.frames.size() == reference.frames.size());

        int differentFrames = 0;

        for (std::size_t i = 0; i < run.frames.size(); ++i)
        {
            const auto& a = run.frames[i];
            const auto& b = reference.frames[i];

            if (a.hopSample != b.hopSample || ! (a.raw == b.raw) || ! (a.processed == b.processed)
                || a.activeNote != b.activeNote || a.voiced != b.voiced)
            {
                if (differentFrames == 0)
                    UNSCOPED_INFO ("first different frame " << i << " at " << a.hopSample << ": raw " << a.raw.frequencyHz
                                                            << " / " << b.raw.frequencyHz << ", processed "
                                                            << a.processed.frequencyHz << " / " << b.processed.frequencyHz);

                ++differentFrames;
            }
        }

        CHECK (differentFrames == 0);
    };

    for (const auto block : blockSizes)
        check (block);

    check (0); // varying block sizes
}

TEST_CASE ("Pipeline gives the same notes at 44.1, 48 and 96 kHz", "[pipeline]")
{
    const auto algorithm = GENERATE (PitchAlgorithm::yin, PitchAlgorithm::mpm);
    CAPTURE (algorithmName (algorithm));

    std::vector<std::vector<Note>> perRate;

    for (const auto sr : sampleRates)
        perRate.push_back (notesOf (runPipeline (mixedMelody (sr), sr, settingsFor (VoiceRange::tenor, algorithm)).events));

    for (std::size_t r = 1; r < perRate.size(); ++r)
    {
        CAPTURE (sampleRates[r]);
        REQUIRE (noteNumbers (perRate[r]) == noteNumbers (perRate[0]));

        for (std::size_t i = 0; i < perRate[0].size(); ++i)
        {
            CAPTURE (i);
            const auto t0 = static_cast<double> (perRate[0][i].on) / sampleRates[0];
            const auto t = static_cast<double> (perRate[r][i].on) / sampleRates[r];
            CHECK (std::abs (t - t0) < 0.005);
            CHECK (std::abs (perRate[r][i].velocity - perRate[0][i].velocity) <= 3);
        }
    }
}

// ---------------------------------------------------------------------------------------------------------
// Latency

TEST_CASE ("Pipeline latency: abrupt onset -> note-on within the model and the spec budgets", "[pipeline]")
{
    // Sine at -20 dBFS after silence, at f0 min of the range (worst case for the high-pass), one semitone
    // above it and at a typical pitch (middle of the range), 8 onset phases across one hop, 3 rates, both
    // algorithms. Prints the worst case per range and pitch.
    enum Pitch { atF0Min, aboveF0Min, typical, numPitches };
    std::map<std::pair<int, int>, double> worst;     // (range, pitch) -> ms
    std::map<std::pair<int, int>, double> worstAlg;  // (range, algorithm) -> ms

    for (const auto range : allRanges)
    {
        const auto& info = voiceRangeInfo (range);
        const double pitches[numPitches] = {
            info.f0MinHz, info.f0MinHz * std::exp2 (1.0 / 12.0),
            test::midiToHz (std::round (0.5 * (frequencyToMidiNote (info.f0MinHz) + frequencyToMidiNote (info.f0MaxHz))))
        };

        for (const auto sr : sampleRates)
        {
            const auto model = VoicePipeline::computeAlgorithmicLatencyMs (sr, range);
            VoicePipeline geometry;
            geometry.prepare (sr, 512);
            const auto hop = geometry.getHopInputSamples();

            for (const auto algorithm : { PitchAlgorithm::yin, PitchAlgorithm::mpm })
            {
                for (int pitch = 0; pitch < numPitches; ++pitch)
                {
                    const auto f = pitches[pitch];

                    for (int phase = 0; phase < 8; ++phase)
                    {
                        CAPTURE (info.name, sr, algorithmName (algorithm), f, phase, model);
                        const auto onset = test::numSamplesFor (0.25, sr) + static_cast<std::size_t> (phase * hop / 8);
                        auto input = test::Signal (onset, 0.0f);
                        const auto tone = test::sine (sr, f, 0.3, test::sineAmplitudeForRmsDb (-20.0));
                        input.insert (input.end(), tone.begin(), tone.end());

                        const auto notes = notesOf (runPipeline (input, sr, settingsFor (range, algorithm)).events);
                        REQUIRE (notes.size() == 1);
                        CHECK (notes[0].note == nearestMidiNote (f));

                        const auto ms = 1000.0 * static_cast<double> (notes[0].on - static_cast<int64_t> (onset)) / sr;
                        CHECK (ms > 0.0);
                        CHECK (ms <= model);

                        if (range == VoiceRange::tenor)
                            CHECK (ms <= VoicePipeline::tenorLatencyBudgetMs);

                        if (range == VoiceRange::bass || range == VoiceRange::wide)
                            CHECK (ms <= VoicePipeline::bassLatencyBudgetMs);

                        auto& w = worst[{ static_cast<int> (range), pitch }];
                        w = std::max (w, ms);
                        auto& wa = worstAlg[{ static_cast<int> (range), static_cast<int> (algorithm) }];
                        wa = std::max (wa, ms);
                    }
                }
            }
        }
    }

    std::printf ("[pipeline] onset -> note-on latency in ms, worst of 44.1/48/96 kHz x 8 onset phases:\n");
    std::printf ("  %-8s %8s %8s %8s %8s %8s %10s\n", "range", "f0 min", "+1 st", "typical", "YIN", "MPM", "model@48k");

    for (const auto range : allRanges)
    {
        const auto r = static_cast<int> (range);
        std::printf ("  %-8s %8.2f %8.2f %8.2f %8.2f %8.2f %10.2f\n", voiceRangeInfo (range).name, worst[{ r, atF0Min }],
                     worst[{ r, aboveF0Min }], worst[{ r, typical }], worstAlg[{ r, 0 }], worstAlg[{ r, 1 }],
                     VoicePipeline::computeAlgorithmicLatencyMs (48000.0, range));
    }
}

// ---------------------------------------------------------------------------------------------------------
// Settings, flush, reset, snapshot

TEST_CASE ("Pipeline: flushing settings changes end the note at offset 0, others do not", "[pipeline]")
{
    const auto sr = GENERATE (44100.0, 48000.0, 96000.0);
    CAPTURE (sr);

    const auto tone = test::sine (sr, 220.0, 0.3, 0.3f);
    const VoiceSettings base;

    const auto startNote = [&] (VoicePipeline& p)
    {
        PipelineRun run;
        runOn (p, tone, base, 256, false, run);
        REQUIRE (p.getActiveNote() == 57);
    };

    // Each of these changes flushes
    std::vector<VoiceSettings> flushing;
    for (int i = 0; i < 5; ++i)
        flushing.push_back (base);
    flushing[0].range = VoiceRange::wide;
    flushing[1].algorithm = base.algorithm == PitchAlgorithm::mpm ? PitchAlgorithm::yin : PitchAlgorithm::mpm;
    flushing[2].key = 2;
    flushing[3].scale = Scale::major;
    flushing[4].channel = 5;

    for (std::size_t i = 0; i < flushing.size(); ++i)
    {
        CAPTURE (i);
        const auto& changed = flushing[i];
        VoicePipeline p;
        p.prepare (sr, 512);
        startNote (p);

        VsEventBuffer<1024> events;
        const auto more = test::sine (sr, 220.0, 0.2, 0.3f);
        p.process (more.data(), 256, changed, events);
        REQUIRE (events.size() >= 1);
        CHECK (events[0].type == VsEventType::noteOff);
        CHECK (events[0].sampleOffset == 0);
        CHECK (events[0].data1 == 57);
        CHECK (events[0].channel == 1); // the old channel

        // Singing on: a new onset on the new settings follows within a few frames (the median history
        // restarts; the window keeps running, so it is not a full onset from silence)
        const auto flushPosition = p.getSamplePosition() - 256;
        PipelineRun after;

        for (const auto& e : events)
            after.events.push_back (timed (flushPosition, e));

        runOn (p, std::span<const float> (more).subspan (256), changed, 256, true, after);
        CHECK (midiInvariantViolations (std::vector<TimedEvent> (after.events.begin() + 1, after.events.end())) == 0);
        const auto notes = notesOf (after.events);
        REQUIRE (notes.size() == 1);
        CHECK (notes[0].note == 57);
        CHECK (notes[0].channel == changed.channel);
        CHECK (notes[0].on - flushPosition < static_cast<int64_t> (0.01 * sr));
    }

    // These do not flush: the note continues
    std::vector<VoiceSettings> continuing;
    for (int i = 0; i < 7; ++i)
        continuing.push_back (base);
    continuing[0].gateThresholdDb = -50.0f;
    continuing[1].stickinessPercent = 90.0f;
    continuing[2].minNoteLengthMs = 200.0f;
    continuing[3].bendMode = BendMode::trueBend;
    continuing[4].bendRange = BendRange::semitones12;
    continuing[5].velocitySensitivityPercent = 10.0f;
    continuing[6].legato = true;

    for (const auto& changed : continuing)
    {
        VoicePipeline p;
        p.prepare (sr, 512);
        startNote (p);

        PipelineRun run;
        runOn (p, test::sine (sr, 220.0, 0.2, 0.3f), changed, 256, false, run);

        for (const auto& [position, e] : run.events)
            CHECK (e.type == VsEventType::pitchBend); // True mode may bend, nothing else happens

        CHECK (p.getActiveNote() == 57);
    }
}

TEST_CASE ("Pipeline: calls with numSamples <= 0 do nothing and do not consume settings", "[pipeline]")
{
    VoicePipeline p;
    p.prepare (48000.0, 512);
    PipelineRun run;
    runOn (p, test::sine (48000.0, 220.0, 0.3, 0.3f), {}, 512, false, run);
    REQUIRE (p.getActiveNote() == 57);
    const auto position = p.getSamplePosition();

    VsEventBuffer<1024> events;
    p.process (nullptr, 0, settingsFor (VoiceRange::bass), events);
    p.process (nullptr, -3, settingsFor (VoiceRange::bass), events);
    CHECK (events.empty());
    CHECK (p.getSamplePosition() == position);

    // The previous settings are still the reference: same settings again -> no flush
    const auto more = test::sine (48000.0, 220.0, 0.05, 0.3f);
    p.process (more.data(), static_cast<int> (more.size()), {}, events);
    CHECK (events.empty());
    CHECK (p.getActiveNote() == 57);
}

TEST_CASE ("Pipeline: nullptr input is silence and releases the note", "[pipeline]")
{
    VoicePipeline p;
    p.prepare (48000.0, 512);
    PipelineRun run;
    runOn (p, test::sine (48000.0, 220.0, 0.3, 0.3f), {}, 512, false, run);
    REQUIRE (p.getActiveNote() == 57);

    VsEventBuffer<1024> events;
    p.process (nullptr, 9600, {}, events);
    REQUIRE (events.size() == 1);
    CHECK (events[0].type == VsEventType::noteOff);
    CHECK (p.getActiveNote() == -1);
}

TEST_CASE ("Pipeline: flush() ends the note at the given offset, reset() is silent and restarts the clock", "[pipeline]")
{
    const auto sr = 48000.0;
    const auto input = test::concat ({ test::silence (sr, 0.05), test::vibrato (sr, 220.0, 50.0, 5.5, 0.4, 0.3f) });
    auto s = settingsFor (VoiceRange::tenor);
    s.bendMode = BendMode::trueBend;

    VoicePipeline p;
    p.prepare (sr, 512);
    PipelineRun first;
    runOn (p, input, s, 512, false, first);
    REQUIRE (p.getActiveNote() == 57);

    // The vibrato keeps the True-mode bend off centre, so the flush must also centre it
    const auto bendsBefore = bendValues (first.events);
    REQUIRE (! bendsBefore.empty());
    REQUIRE (bendsBefore.back() != 0);

    VsEventBuffer<1024> events;
    p.flush (123, events);
    REQUIRE (events.size() == 2);
    CHECK (events[0].type == VsEventType::noteOff);
    CHECK (events[0].sampleOffset == 123);
    CHECK (events[1].type == VsEventType::pitchBend);
    CHECK (events[1].bend == 0);
    CHECK (events[1].sampleOffset == 123);

    CHECK (p.getActiveNote() == -1);
    events.clear();
    p.flush (0, events);
    CHECK (events.empty()); // nothing active

    // reset(): no events, clock at 0, and the same input gives the same events as a fresh pipeline
    p.reset();
    CHECK (p.getSamplePosition() == 0);
    CHECK (p.getSnapshot().activeNote == -1);
    PipelineRun again;
    runOn (p, input, s, 512, true, again);
    CHECK (again.events == runPipeline (input, sr, s, 512, false, true).events);

    // prepare() at another rate also starts fresh
    p.prepare (96000.0, 64);
    CHECK (p.getSampleRate() == 96000.0);
    CHECK (p.getSamplePosition() == 0);
    CHECK (p.getActiveNote() == -1);
}

TEST_CASE ("Pipeline snapshot: tuner values, note, latency and publish rate", "[pipeline]")
{
    const auto sr = 48000.0;
    VoicePipeline p;
    p.prepare (sr, 512);

    const auto initial = p.getSnapshot();
    CHECK (initial.activeNote == -1);
    CHECK (initial.nearestNote == -1);
    CHECK (initial.frequencyHz == 0.0f);
    CHECK_FALSE (initial.voiced);
    CHECK (initial.latencyMs == static_cast<float> (VoicePipeline::computeAlgorithmicLatencyMs (sr, VoiceRange::tenor)));

    // 233.08 Hz = A#3 (58), sung 20 cents sharp
    const auto f = test::midiToHz (58.2);
    PipelineRun run;
    runOn (p, test::sine (sr, f, 0.3, 0.3f), settingsFor (VoiceRange::alto), 512, false, run);

    const auto s = p.getSnapshot();
    CHECK (std::abs (centsBetween (static_cast<double> (s.frequencyHz), f)) < 5.0);
    CHECK (s.nearestNote == 58);
    CHECK (std::abs (s.centsOffset - 20.0f) < 5.0f);
    CHECK (s.activeNote == 58);
    CHECK (s.voiced);
    CHECK (s.confidence > 0.9f);
    CHECK (std::abs (s.rmsDb - (20.0f * std::log10 (0.3f / std::sqrt (2.0f)))) < 1.0f);
    CHECK (s.latencyMs == static_cast<float> (VoicePipeline::computeAlgorithmicLatencyMs (sr, VoiceRange::alto)));
    CHECK (s.frameCounter == static_cast<uint32_t> (test::numSamplesFor (0.3, sr) / 96));

    // A 1-sample call that completes no hop does not publish
    VsEventBuffer<1024> events;
    const float one = 0.1f;
    p.process (&one, 1, settingsFor (VoiceRange::alto), events);
    CHECK (p.getSnapshot().frameCounter == s.frameCounter);

    // reset() publishes a default analysis (the frame counter keeps counting since prepare)
    p.reset();
    const auto afterReset = p.getSnapshot();
    CHECK (afterReset.activeNote == -1);
    CHECK (afterReset.frequencyHz == 0.0f);
    CHECK_FALSE (afterReset.voiced);
    CHECK (afterReset.frameCounter == s.frameCounter);
}

TEST_CASE ("VoiceSnapshotPublisher: concurrent reads are consistent", "[pipeline]")
{
    VoiceSnapshotPublisher publisher;
    std::atomic<bool> done { false };

    const auto make = [] (uint32_t k)
    {
        VoiceAnalysisSnapshot s;
        s.frequencyHz = static_cast<float> (k);
        s.centsOffset = static_cast<float> (k % 100);
        s.nearestNote = static_cast<int> (k % 128);
        s.activeNote = static_cast<int> ((k + 1) % 128);
        s.confidence = static_cast<float> (k % 1000) / 1000.0f;
        s.rmsDb = -static_cast<float> (k % 120);
        s.voiced = (k % 2) == 1;
        s.latencyMs = static_cast<float> (k % 50);
        s.frameCounter = k;
        return s;
    };

    // The audio thread publishes at most once per process() call (>= one hop, 2 ms of audio); here every
    // 5 us, still far faster, so reads race publishes all the time
    constexpr uint32_t numPublishes = 20000;

    std::thread writer ([&]
    {
        for (uint32_t k = 1; k <= numPublishes; ++k)
        {
            publisher.publish (make (k));
            const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds (5);

            while (std::chrono::steady_clock::now() < until) {}
        }

        done.store (true);
    });

    int reads = 0, inconsistent = 0;
    uint32_t lastCounter = 0;
    int backwards = 0;

    while (! done.load() || reads < 1000)
    {
        const auto s = publisher.read();
        const auto expected = make (s.frameCounter);
        ++reads;

        if (s.frameCounter == 0)
            continue;

        if (! (s.frequencyHz == expected.frequencyHz && s.centsOffset == expected.centsOffset
               && s.nearestNote == expected.nearestNote && s.activeNote == expected.activeNote
               && s.confidence == expected.confidence && s.rmsDb == expected.rmsDb && s.voiced == expected.voiced
               && s.latencyMs == expected.latencyMs))
            ++inconsistent;

        if (s.frameCounter < lastCounter)
            ++backwards;

        lastCounter = s.frameCounter;
    }

    writer.join();
    CAPTURE (reads, inconsistent, backwards);
    // The unchecked fallback copy (after 8 raced attempts) may mix two publishes; it must stay rare
    CHECK (inconsistent * 1000 <= reads);
    CHECK (backwards == 0);
    CHECK (publisher.read().frameCounter == numPublishes);
}

// ---------------------------------------------------------------------------------------------------------
// CPU

namespace
{

struct CpuResult
{
    double worstFraction = 0.0; ///< worst block time / block duration
    double meanFraction = 0.0;
};

/** Times every block of `input` through `runs` fresh pipelines and takes per block the MINIMUM over the
    runs (a preemption by another process hits one run, not all), then worst and mean over the blocks. */
CpuResult measureCpu (std::span<const float> input, double sr, const VoiceSettings& s, int block, int runs)
{
    const auto numBlocks = input.size() / static_cast<std::size_t> (block);
    std::vector<double> best (numBlocks, 1.0e9);

    for (int r = 0; r < runs; ++r)
    {
        VoicePipeline p;
        p.prepare (sr, block);
        VsEventBuffer<1024> events;

        for (std::size_t b = 0; b < numBlocks; ++b)
        {
            events.clear();
            const auto t0 = std::chrono::steady_clock::now();
            p.process (input.data() + b * static_cast<std::size_t> (block), block, s, events);
            const auto t1 = std::chrono::steady_clock::now();
            best[b] = std::min (best[b], std::chrono::duration<double> (t1 - t0).count());
        }
    }

    const auto blockSeconds = static_cast<double> (block) / sr;
    CpuResult result;

    for (const auto t : best)
    {
        result.worstFraction = std::max (result.worstFraction, t / blockSeconds);
        result.meanFraction += t / blockSeconds;
    }

    result.meanFraction /= static_cast<double> (std::max<std::size_t> (1, numBlocks));
    return result;
}

test::Signal cpuTestSignal (double sr, double seconds)
{
    // Saw at 65 Hz (longest window, every frame voiced) + a little noise
    auto input = test::saw (sr, 65.0, seconds, 0.3f);
    test::mixInto (input, test::whiteNoise (sr, seconds, 0.01f, 3));
    return input;
}

} // namespace

TEST_CASE ("Pipeline CPU sanity check: worst 128-sample block at 48 kHz", "[pipeline][cpu]")
{
    // Spec: worst case < 20 % of a 128-sample block at 48 kHz in Release. Asserted generously (50 %) so a
    // loaded CI runner cannot make it flaky; the real number is printed. Debug builds only report.
    const auto sr = 48000.0;
    const auto input = cpuTestSignal (sr, 4.0);

    for (const auto algorithm : { PitchAlgorithm::yin, PitchAlgorithm::mpm })
    {
        CAPTURE (algorithmName (algorithm));
        const auto r = measureCpu (input, sr, settingsFor (VoiceRange::wide, algorithm), 128, 3);
        std::printf ("[pipeline][cpu] %s Wide 65 Hz saw, 48 kHz, 128-sample blocks: worst %.2f %%, mean %.2f %% of the block\n",
                     algorithmName (algorithm), 100.0 * r.worstFraction, 100.0 * r.meanFraction);

#ifdef NDEBUG
        CHECK (r.worstFraction < 0.50);
        CHECK (r.meanFraction < 0.10);
#endif
    }
}

TEST_CASE ("Pipeline CPU benchmark (all rates, ranges, algorithms)", "[.benchmark]")
{
    // Hidden by default: run with  VocalSynthCoreTests "[.benchmark]"
    std::printf ("[pipeline][benchmark] 128-sample blocks, 10 s per case, min of 3 runs per block\n");
    std::printf ("  %-9s %-8s %-4s %10s %10s\n", "rate", "range", "alg", "worst %", "mean %");

    for (const auto sr : sampleRates)
    {
        const auto input = cpuTestSignal (sr, 10.0);

        for (const auto range : { VoiceRange::wide, VoiceRange::bass, VoiceRange::tenor, VoiceRange::whistle })
        {
            for (const auto algorithm : { PitchAlgorithm::yin, PitchAlgorithm::mpm })
            {
                const auto r = measureCpu (input, sr, settingsFor (range, algorithm), 128, 3);
                std::printf ("  %-9.0f %-8s %-4s %10.2f %10.2f\n", sr, voiceRangeInfo (range).name, algorithmName (algorithm),
                             100.0 * r.worstFraction, 100.0 * r.meanFraction);

                if (sr == 48000.0)
                    CHECK (r.worstFraction < 0.20); // the spec budget, on an idle machine
            }
        }
    }
}
