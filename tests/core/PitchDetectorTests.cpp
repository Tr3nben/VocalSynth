#include "vocalsynth/PitchDetector.h"

#include "vocalsynth/Decimator.h"
#include "vocalsynth/Framer.h"
#include "vocalsynth/InputConditioner.h"
#include "vocalsynth/NoteSegmenter.h"
#include "vocalsynth/VoiceSettings.h"

#include "TestSignals.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

using Catch::Matchers::WithinAbs;
using namespace vocalsynth;

namespace
{

// Extra helpers for this file (TestSignals.h is shared and frozen)

constexpr std::array analysisRates { 16000.0, 17640.0, 22050.0 }; // 48/96, 88.2 and 44.1 kHz hosts
constexpr std::array hostRates { 44100.0, 48000.0, 96000.0 };

/** Harmonic-rich signals are tested only with at least this many analysis samples per period. */
constexpr double minLagsPerPeriodForHarmonics = 10.0;

double centsBetween (double f, double reference) { return 1200.0 * std::log2 (f / reference); }

/** Upper limit of the accuracy requirement: +-10 cents up to 1047 Hz (spec), +-25 above (Whistle). */
double toleranceCentsFor (double f0) { return f0 <= 1047.0 ? 10.0 : 25.0; }

/** Band-limited sawtooth-like stack (harmonics 1..64 with 1/k amplitudes, cut at Nyquist), for
    signals synthesised directly at the analysis rate (a naive saw would alias there). */
test::Signal bandLimitedSaw (double sampleRate, double f0, double seconds, float amplitude = 0.5f)
{
    return test::harmonicStack (sampleRate, f0, seconds, 1, 64, amplitude);
}

/** Test grid of a range: f0 min, f0 max and 6 geometrically spaced pitches in between. */
std::vector<double> gridFor (const VoiceRangeInfo& range)
{
    std::vector<double> grid;

    for (int i = 0; i <= 7; ++i)
        grid.push_back (range.f0MinHz * std::pow (range.f0MaxHz / range.f0MinHz, static_cast<double> (i) / 7.0));

    grid.back() = range.f0MaxHz; // exact limits (pow rounding)
    return grid;
}

struct Detectors
{
    YinPitchDetector yin;
    MpmPitchDetector mpm;

    explicit Detectors (double fa)
    {
        const auto maxWindow = Framer::windowSamplesFor (fa, lowestF0MinHz);
        yin.prepare (fa, maxWindow);
        mpm.prepare (fa, maxWindow);
    }

    PitchDetector& get (PitchAlgorithm algorithm)
    {
        return algorithm == PitchAlgorithm::mpm ? static_cast<PitchDetector&> (mpm) : static_cast<PitchDetector&> (yin);
    }
};

const char* nameOf (PitchAlgorithm a) { return a == PitchAlgorithm::mpm ? "MPM" : "YIN"; }

/** Analyses numFrames windows of the range's length taken from a signal at the analysis rate, at
    start offsets spread over (and not aligned with) the signal's period. */
std::vector<PitchFrame> analyseFrames (PitchDetector& detector, double fa, const test::Signal& signal,
                                       const VoiceRangeInfo& range, int numFrames = 6)
{
    const auto w = static_cast<std::size_t> (Framer::windowSamplesFor (fa, range.f0MinHz));
    REQUIRE (signal.size() >= w + static_cast<std::size_t> (numFrames) * 37);
    std::vector<PitchFrame> frames;

    for (int i = 0; i < numFrames; ++i)
    {
        const auto start = static_cast<std::size_t> (i) * 37;
        frames.push_back (detector.analyse (std::span<const float> (signal).subspan (start, w), range.f0MinHz, range.f0MaxHz));
    }

    return frames;
}

/** Seconds of signal needed for analyseFrames(). */
double secondsFor (double fa, const VoiceRangeInfo& range, int numFrames = 6)
{
    return (Framer::windowSamplesFor (fa, range.f0MinHz) + numFrames * 37 + 8) / fa;
}

/** Host-rate front end: InputConditioner -> Decimator -> Framer (window of the range); returns the
    detector output of every hop after `skipSeconds`. */
std::vector<PitchFrame> analyseThroughFrontEnd (PitchDetector& detector, double hostRate, const test::Signal& signal,
                                                const VoiceRangeInfo& range, double skipSeconds)
{
    InputConditioner conditioner;
    Decimator decimator;
    Framer framer;
    conditioner.prepare (hostRate);
    decimator.prepare (hostRate);
    const auto fa = decimator.getOutputSampleRate();
    framer.prepare (fa, Framer::windowSamplesFor (fa, lowestF0MinHz));
    framer.setWindowSamples (Framer::windowSamplesFor (fa, range.f0MinHz));
    detector.prepare (fa, framer.getMaxWindowSamples());

    std::vector<PitchFrame> frames;
    const auto skip = test::numSamplesFor (skipSeconds, hostRate);

    for (std::size_t n = 0; n < signal.size(); ++n)
    {
        float y = 0.0f;

        if (! decimator.pushSample (conditioner.processSample (signal[n]), y) || ! framer.pushSample (y))
            continue;

        const auto frame = detector.analyse (framer.getWindow(), range.f0MinHz, range.f0MaxHz);

        if (n >= skip)
            frames.push_back (frame);
    }

    return frames;
}

/** Checks that every frame is voiced, confident and within tolerance of f0. */
void checkAccurate (const std::vector<PitchFrame>& frames, double f0, double toleranceCents)
{
    REQUIRE (! frames.empty());

    for (const auto& frame : frames)
    {
        REQUIRE (frame.frequencyHz > 0.0f);
        REQUIRE (std::abs (centsBetween (frame.frequencyHz, f0)) <= toleranceCents);
        REQUIRE (frame.confidence >= NoteSegmenter::confidenceOn);
    }
}

} // namespace

TEST_CASE ("rmsDb helpers", "[pitchdetector]")
{
    CHECK (computeRmsDb ({}) == PitchFrame::silenceDb);

    const std::vector<float> zeros (100, 0.0f);
    CHECK (computeRmsDb (zeros) == PitchFrame::silenceDb);

    const std::vector<float> ones (100, 1.0f);
    CHECK_THAT (computeRmsDb (ones), WithinAbs (0.0, 1.0e-6));

    const std::vector<float> tiny (100, 1.0e-9f); // -180 dB -> floor
    CHECK (computeRmsDb (tiny) == PitchFrame::silenceDb);

    const auto fullScale = test::sine (16000.0, 250.0, 0.1, 1.0f);
    CHECK_THAT (computeRmsDb (fullScale), WithinAbs (-3.01, 0.02));

    CHECK (rmsSamplesFor (16000.0, 110.0) == 146);
    CHECK (rmsSamplesFor (22050.0, 65.0) == 340);
    CHECK (rmsSamplesFor (0.0, 65.0) == 1);
}

TEST_CASE ("PitchDetector: silence is unvoiced", "[pitchdetector]")
{
    for (const auto fa : analysisRates)
    {
        Detectors detectors (fa);

        for (const auto algorithm : { PitchAlgorithm::yin, PitchAlgorithm::mpm })
            for (const auto& range : voiceRangeTable)
            {
                CAPTURE (fa, nameOf (algorithm), range.name);
                const auto window = test::silence (fa, Framer::windowSamplesFor (fa, range.f0MinHz) / fa + 0.01);
                const auto frame = detectors.get (algorithm).analyse (
                    std::span<const float> (window).first (static_cast<std::size_t> (Framer::windowSamplesFor (fa, range.f0MinHz))),
                    range.f0MinHz, range.f0MaxHz);
                CHECK (frame.frequencyHz == 0.0f);
                CHECK (frame.confidence == 0.0f);
                CHECK (frame.rmsDb == PitchFrame::silenceDb);
            }

        // Below the silence floor (-90 dB) the frame is unvoiced without a search, even if periodic
        const auto quiet = test::sine (fa, 220.0, 0.05, test::sineAmplitudeForRmsDb (-100.0));

        for (const auto algorithm : { PitchAlgorithm::yin, PitchAlgorithm::mpm })
        {
            const auto frame = detectors.get (algorithm).analyse (std::span<const float> (quiet).first (294), 110.0, 523.0);
            CHECK (frame.frequencyHz == 0.0f);
            CHECK (frame.rmsDb < PitchDetector::silenceFloorDb);
        }
    }
}

TEST_CASE ("PitchDetector: rmsDb of a -20 dB sine", "[pitchdetector]")
{
    for (const auto fa : analysisRates)
    {
        Detectors detectors (fa);

        for (const auto algorithm : { PitchAlgorithm::yin, PitchAlgorithm::mpm })
            for (const auto& range : voiceRangeTable)
            {
                CAPTURE (fa, nameOf (algorithm), range.name);
                const auto f0 = std::sqrt (range.f0MinHz * range.f0MaxHz);
                const auto signal = test::sine (fa, f0, secondsFor (fa, range), test::sineAmplitudeForRmsDb (-20.0));

                for (const auto& frame : analyseFrames (detectors.get (algorithm), fa, signal, range))
                    CHECK_THAT (frame.rmsDb, WithinAbs (-20.0, 0.5));
            }
    }
}

TEST_CASE ("PitchDetector: sines and saws within +-10 cents in every range (analysis rate)", "[pitchdetector]")
{
    const auto algorithm = GENERATE (PitchAlgorithm::yin, PitchAlgorithm::mpm);

    for (const auto fa : analysisRates)
    {
        Detectors detectors (fa);
        auto& detector = detectors.get (algorithm);

        for (const auto& range : voiceRangeTable)
            for (const auto f0 : gridFor (range))
            {
                CAPTURE (nameOf (algorithm), fa, range.name, f0);
                const auto seconds = secondsFor (fa, range);

                checkAccurate (analyseFrames (detector, fa, test::sine (fa, f0, seconds), range), f0, toleranceCentsFor (f0));

                if (fa / f0 >= minLagsPerPeriodForHarmonics) // see the strong-2nd-harmonic test
                    checkAccurate (analyseFrames (detector, fa, bandLimitedSaw (fa, f0, seconds), range), f0,
                                   toleranceCentsFor (f0));

                // Quiet input (-40 dBFS) is still accurate: the detectors are level independent
                checkAccurate (analyseFrames (detector, fa, test::sine (fa, f0, seconds, test::sineAmplitudeForRmsDb (-40.0)), range),
                               f0, toleranceCentsFor (f0));
            }
    }
}

TEST_CASE ("PitchDetector: sines and saws within +-10 cents through the front end at 44.1/48/96 kHz", "[pitchdetector]")
{
    const auto algorithm = GENERATE (PitchAlgorithm::yin, PitchAlgorithm::mpm);
    YinPitchDetector yin;
    MpmPitchDetector mpm;
    auto& detector = algorithm == PitchAlgorithm::mpm ? static_cast<PitchDetector&> (mpm) : static_cast<PitchDetector&> (yin);

    for (const auto hostRate : hostRates)
        for (const auto& range : voiceRangeTable)
            for (const auto f0 : { range.f0MinHz, std::sqrt (range.f0MinHz * range.f0MaxHz), range.f0MaxHz })
            {
                CAPTURE (nameOf (algorithm), hostRate, range.name, f0);

                // 60 ms: the window (<= 31 ms) and the conditioner have settled after 40 ms
                checkAccurate (analyseThroughFrontEnd (detector, hostRate, test::sine (hostRate, f0, 0.06), range, 0.04), f0,
                               toleranceCentsFor (f0));

                // Band-limited saw at the host rate (harmonics up to the host Nyquist): the decimator
                // removes what would fold into the band from the analysis rate's point of view.
                checkAccurate (analyseThroughFrontEnd (detector, hostRate, bandLimitedSaw (hostRate, f0, 0.06), range, 0.04),
                               f0, toleranceCentsFor (f0));

                // Naive (aliasing) saw at the host rate, only in the middle of the range: its own host-rate
                // aliases are in-band and inharmonic unless the period is a whole number of host samples
                // (e.g. harmonic 43 of 1118 Hz at 48 kHz folds to 74 Hz; 500 Hz at 44.1 kHz = 88.2
                // samples). Near the range limits, where a period fills most of the analysis window, they
                // move the estimate by up to +-17 cents, which the former clamp to [f0 min, f0 max] hid
                // at the exact limits only (TestSignals.h). A real voice has no such aliases.
                if (f0 > range.f0MinHz && f0 < range.f0MaxHz && f0 <= 1047.0)
                    checkAccurate (analyseThroughFrontEnd (detector, hostRate, test::saw (hostRate, f0, 0.06), range, 0.04), f0,
                                   toleranceCentsFor (f0));
            }
}

TEST_CASE ("PitchDetector: Whistle range up to 2500 Hz", "[pitchdetector]")
{
    const auto& range = voiceRangeInfo (VoiceRange::whistle);

    for (const auto fa : analysisRates)
    {
        Detectors detectors (fa);

        for (const auto algorithm : { PitchAlgorithm::yin, PitchAlgorithm::mpm })
            for (double f0 = 500.0; f0 <= 2500.0; f0 *= std::exp2 (1.0 / 12.0)) // every semitone
            {
                CAPTURE (fa, nameOf (algorithm), f0);
                checkAccurate (analyseFrames (detectors.get (algorithm), fa, test::sine (fa, f0, secondsFor (fa, range)), range),
                               f0, toleranceCentsFor (f0));
            }
    }
}

TEST_CASE ("PitchDetector: missing fundamental and strong 2nd harmonic give f0, not 2 f0", "[pitchdetector]")
{
    const auto algorithm = GENERATE (PitchAlgorithm::yin, PitchAlgorithm::mpm);

    for (const auto fa : analysisRates)
    {
        Detectors detectors (fa);
        auto& detector = detectors.get (algorithm);

        for (const auto& range : voiceRangeTable)
            for (const auto f0 : gridFor (range))
            {
                CAPTURE (nameOf (algorithm), fa, range.name, f0);
                const auto seconds = secondsFor (fa, range);
                const auto tolerance = toleranceCentsFor (f0);

                // All three cases only with >= 10 lags per period (f0 <= 1.6 kHz at 16 kHz; covers the
                // spec's 65-1047 Hz): with a 6-lag period the 3-point interpolation of a lobe sharpened
                // by strong harmonics falls short of the 2 T lobe, which then wins (octave down).
                // Whistling is close to a sine (Whistle test).
                if (fa / f0 < minLagsPerPeriodForHarmonics)
                    continue;

                // Harmonics 2..8 only: the period is still 1 / f0
                checkAccurate (analyseFrames (detector, fa, test::harmonicStack (fa, f0, seconds, 2, 8), range), f0, tolerance);

                // sin f0 + 3 sin 2 f0 (the design's margin case: YIN d'(T/2) ~ 0.2, MPM n(T/2) ~ 0.8)
                checkAccurate (analyseFrames (detector, fa, test::harmonicStack (fa, f0, seconds, 1, 2, 0.5f, 6.0), range), f0,
                               tolerance);

                // A voice-like stack with a 3x boosted 2nd harmonic
                checkAccurate (analyseFrames (detector, fa, test::harmonicStack (fa, f0, seconds, 1, 8, 0.5f, 3.0), range), f0,
                               tolerance);
            }
    }
}

TEST_CASE ("PitchDetector: white and pink noise have low confidence", "[pitchdetector]")
{
    const auto algorithm = GENERATE (PitchAlgorithm::yin, PitchAlgorithm::mpm);

    for (const auto fa : analysisRates)
    {
        Detectors detectors (fa);
        auto& detector = detectors.get (algorithm);

        for (const auto& range : voiceRangeTable)
        {
            const auto w = static_cast<std::size_t> (Framer::windowSamplesFor (fa, range.f0MinHz));
            const auto hop = static_cast<std::size_t> (Framer::hopSamplesFor (fa));
            const std::size_t numFrames = 400;
            const auto seconds = static_cast<double> (w + numFrames * hop) / fa + 0.01;

            const std::array<std::pair<const char*, test::Signal>, 2> noises { {
                { "white", test::whiteNoise (fa, seconds, 0.3f, 1234) },
                { "pink", test::pinkNoise (fa, seconds, 0.5f, 99) },
            } };

            for (const auto& [noiseName, noise] : noises)
            {
                CAPTURE (nameOf (algorithm), fa, range.name, noiseName);
                std::size_t confident = 0;

                for (std::size_t i = 0; i < numFrames; ++i)
                {
                    const auto frame = detector.analyse (std::span<const float> (noise).subspan (i * hop, w), range.f0MinHz,
                                                         range.f0MaxHz);
                    REQUIRE (frame.rmsDb > -30.0f); // loud noise: only the confidence can reject it

                    if (frame.frequencyHz > 0.0f && frame.confidence >= NoteSegmenter::confidenceOn)
                        ++confident;
                }

                CHECK (confident * 100 <= numFrames); // >= 99 % of the frames below confidenceOn
            }
        }
    }
}

TEST_CASE ("PitchDetector edge rule: tones outside the range are unvoiced", "[pitchdetector]")
{
    const auto algorithm = GENERATE (PitchAlgorithm::yin, PitchAlgorithm::mpm);

    for (const auto fa : analysisRates)
    {
        Detectors detectors (fa);
        auto& detector = detectors.get (algorithm);

        for (const auto& range : voiceRangeTable)
            for (const auto f0 : { range.f0MinHz * std::exp2 (-1.5 / 12.0), range.f0MaxHz * std::exp2 (1.5 / 12.0) })
            {
                CAPTURE (nameOf (algorithm), fa, range.name, f0);
                const auto seconds = secondsFor (fa, range, 12);

                // Harmonic-rich tones with fewer than 10 lags per period (only above the Whistle range)
                // are not tested: see the strong-2nd-harmonic test above
                std::vector<test::Signal> signals { test::sine (fa, f0, seconds) };

                if (fa / f0 >= minLagsPerPeriodForHarmonics)
                    signals.push_back (bandLimitedSaw (fa, f0, seconds));

                // Not the range limit and not an octave: unvoiced. YIN's fallback (no dip below the
                // threshold -> global minimum) may report a weak pitch inside the range when the d' of a
                // tone below the range is a plateau up to maxLag (saw at 59.6 Hz in Bass: d' ~ 0.355 from
                // lag 236 to 248, minimum at 245 by 0.001). Such a frame satisfies the edge rule literally,
                // but it has confidence ~0.65 < confidenceOn, so it can never start a note; at least
                // 80 % of the frames must still be unvoiced.
                for (const auto& signal : signals)
                {
                    const auto frames = analyseFrames (detector, fa, signal, range, 12);
                    std::size_t unvoicedFrames = 0;

                    for (const auto& frame : frames)
                    {
                        CHECK (frame.rmsDb > -20.0f);

                        if (frame.frequencyHz == 0.0f)
                        {
                            CHECK (frame.confidence == 0.0f);
                            ++unvoicedFrames;
                            continue;
                        }

                        CHECK (algorithm == PitchAlgorithm::yin);
                        CHECK (frame.confidence < NoteSegmenter::confidenceOn);
                        CHECK (std::abs (centsBetween (frame.frequencyHz, f0)) < 300.0); // not an octave
                    }

                    CHECK (unvoicedFrames * 10 >= frames.size() * 8);
                }
            }

        // Concrete design cases: 60 Hz in Bass is not a confident 65 Hz, 1.1 kHz in Soprano is not 550 Hz
        const auto& bass = voiceRangeInfo (VoiceRange::bass);
        const auto& soprano = voiceRangeInfo (VoiceRange::soprano);

        for (const auto& frame : analyseFrames (detector, fa, test::sine (fa, 60.0, secondsFor (fa, bass)), bass))
            CHECK (frame.frequencyHz == 0.0f);

        for (const auto& frame : analyseFrames (detector, fa, bandLimitedSaw (fa, 1100.0, secondsFor (fa, soprano)), soprano))
            CHECK (frame.frequencyHz == 0.0f);
    }
}

TEST_CASE ("PitchDetector is deterministic and stateless between frames", "[pitchdetector]")
{
    const auto fa = 16000.0;
    Detectors detectors (fa);
    const auto& tenor = voiceRangeInfo (VoiceRange::tenor);
    const auto& bass = voiceRangeInfo (VoiceRange::bass);
    const auto signal = bandLimitedSaw (fa, 220.0, 0.1);
    const auto noise = test::whiteNoise (fa, 0.1, 0.3f, 5);

    for (const auto algorithm : { PitchAlgorithm::yin, PitchAlgorithm::mpm })
    {
        auto& detector = detectors.get (algorithm);
        const auto window = std::span<const float> (signal).first (294);
        const auto first = detector.analyse (window, tenor.f0MinHz, tenor.f0MaxHz);

        // Other frames (different range, noise) in between change nothing
        detector.analyse (std::span<const float> (noise).first (496), bass.f0MinHz, bass.f0MaxHz);
        detector.analyse (std::span<const float> (signal).first (66), 500.0, 2500.0);
        CHECK (detector.analyse (window, tenor.f0MinHz, tenor.f0MaxHz) == first);

        // A window longer than prepared is cut to the newest prepared samples instead of overrunning
        const auto longSignal = bandLimitedSaw (fa, 220.0, 0.2);
        const auto frame = detector.analyse (longSignal, tenor.f0MinHz, tenor.f0MaxHz);
        CHECK (std::isfinite (frame.frequencyHz));

        // Degenerate arguments never crash and are unvoiced
        CHECK (detector.analyse ({}, tenor.f0MinHz, tenor.f0MaxHz).frequencyHz == 0.0f);
        CHECK (detector.analyse (std::span<const float> (signal).first (8), tenor.f0MinHz, tenor.f0MaxHz).frequencyHz == 0.0f);
        CHECK (detector.analyse (window, 0.0, tenor.f0MaxHz).frequencyHz == 0.0f);
        CHECK (detector.analyse (window, 400.0, 300.0).frequencyHz == 0.0f);
    }
}

TEST_CASE ("PitchDetector: a frame after a longer range gives the same result as a fresh detector", "[pitchdetector]")
{
    // Below-range tones in Tenor can leave YIN's chosen lag at maxLag (global-minimum fallback); the
    // sub-multiple check then read d'(maxLag + 1), which this frame never wrote: 0 from prepare() in a
    // fresh detector, a stale value after a Wide or Bass frame (f = 93.25 Hz: 207 Hz vs 0 Hz).
    const auto& tenor = voiceRangeInfo (VoiceRange::tenor);
    const auto& wide = voiceRangeInfo (VoiceRange::wide);

    for (const auto fa : analysisRates)
    {
        const auto tenorWindow = static_cast<std::size_t> (Framer::windowSamplesFor (fa, tenor.f0MinHz));
        const auto wideWindow = static_cast<std::size_t> (Framer::windowSamplesFor (fa, wide.f0MinHz));
        const auto wideSignal = test::saw (fa, 60.0, 2.0 * wideWindow / fa);

        for (const auto algorithm : { PitchAlgorithm::yin, PitchAlgorithm::mpm })
        {
            int differences = 0;

            for (double f = 80.0; f < 110.0; f += 0.25)
                for (const auto h2 : { 0.5, 1.0, 3.0 })
                {
                    const auto signal = test::harmonicStack (fa, f, 2.0 * tenorWindow / fa + 0.01, 1, 8, 0.5f, h2);

                    for (std::size_t start = 0; start < tenorWindow; start += 61)
                    {
                        const auto window = std::span<const float> (signal).subspan (start, tenorWindow);
                        Detectors fresh (fa), used (fa);
                        used.get (algorithm).analyse (std::span<const float> (wideSignal).first (wideWindow), wide.f0MinHz,
                                                      wide.f0MaxHz);

                        if (! (fresh.get (algorithm).analyse (window, tenor.f0MinHz, tenor.f0MaxHz)
                               == used.get (algorithm).analyse (window, tenor.f0MinHz, tenor.f0MaxHz)))
                            ++differences;
                    }
                }

            CAPTURE (fa, nameOf (algorithm));
            CHECK (differences == 0);
        }
    }
}

TEST_CASE ("Front end + detectors are exactly block-size invariant", "[pitchdetector][invariance]")
{
    // InputConditioner -> Decimator -> Framer -> YIN/MPM driven in host blocks of different sizes:
    // the frames, and the absolute host sample that completed each hop, must be bit-identical.
    struct HopFrame
    {
        int64_t hopSample;
        PitchFrame yin, mpm;
        bool operator== (const HopFrame&) const = default;
    };

    const auto run = [] (double hostRate, const test::Signal& signal, int blockSize)
    {
        InputConditioner conditioner;
        Decimator decimator;
        Framer framer;
        YinPitchDetector yin;
        MpmPitchDetector mpm;
        conditioner.prepare (hostRate);
        decimator.prepare (hostRate);
        const auto fa = decimator.getOutputSampleRate();
        const auto maxWindow = Framer::windowSamplesFor (fa, lowestF0MinHz);
        framer.prepare (fa, maxWindow);
        yin.prepare (fa, maxWindow);
        mpm.prepare (fa, maxWindow);

        std::vector<HopFrame> frames;
        std::vector<float> block (static_cast<std::size_t> (blockSize));
        int64_t position = 0;

        for (std::size_t start = 0; start < signal.size(); start += static_cast<std::size_t> (blockSize))
        {
            const auto len = std::min (static_cast<std::size_t> (blockSize), signal.size() - start);
            std::copy_n (signal.begin() + static_cast<std::ptrdiff_t> (start), len, block.begin());

            // (The range stays constant: a range change applies per host block, so its frame depends
            // on the block split by design; FramerTests covers changes of the window length.)
            conditioner.process (block.data(), block.data(), static_cast<int> (len));

            for (std::size_t i = 0; i < len; ++i)
            {
                float y = 0.0f;

                if (! decimator.pushSample (block[i], y) || ! framer.pushSample (y))
                    continue;

                const auto& range = voiceRangeInfo (VoiceRange::wide);
                frames.push_back ({ position + static_cast<int64_t> (i), yin.analyse (framer.getWindow(), range.f0MinHz, range.f0MaxHz),
                                    mpm.analyse (framer.getWindow(), range.f0MinHz, range.f0MaxHz) });
            }

            position += static_cast<int64_t> (len);
        }

        return frames;
    };

    for (const auto hostRate : hostRates)
    {
        CAPTURE (hostRate);

        // Silence, a glide over the Wide range, noise and a saw: voiced and unvoiced frames
        const auto glide = test::glissando (hostRate, 80.0, 900.0, 0.3);
        const auto saw = test::saw (hostRate, 147.0, 0.1);
        const auto noise = test::whiteNoise (hostRate, 0.05, 0.2f, 42);
        const auto pause = test::silence (hostRate, 0.03);
        const auto signal = test::concat ({ pause, glide, noise, saw, pause });

        const auto expected = run (hostRate, signal, 4096);
        REQUIRE (expected.size() > 200);

        // The hop grid: completions at n = (m hop - 1) D (docs/M2-DESIGN.md 3.10)
        const auto d = Decimator::factorFor (hostRate);
        const auto hop = Framer::hopSamplesFor (hostRate / static_cast<double> (d));

        for (std::size_t m = 0; m < expected.size(); ++m)
            REQUIRE (expected[m].hopSample == (static_cast<int64_t> (m + 1) * hop - 1) * d);

        const auto voicedFrames = std::count_if (expected.begin(), expected.end(), [] (const HopFrame& f)
                                                 { return f.yin.frequencyHz > 0.0f && f.mpm.frequencyHz > 0.0f; });
        CHECK (voicedFrames > 100);

        for (const int blockSize : { 1, 7, 64, 480, 512 })
        {
            CAPTURE (blockSize);
            REQUIRE (run (hostRate, signal, blockSize) == expected);
        }
    }
}
