#pragma once

// Synthetic evaluation corpus for vs-eval --self-test / --write-corpus (docs/M2-DESIGN.md 5).
// Header-only (no CMake change needed), deterministic, built from tests/core/TestSignals.h.
//
// Every item is named "<range>_<content>_<rate>k" so that vs-eval --range auto (the default) picks the
// voice range from the file name when the corpus is written to disk and evaluated from there.

#include "Metrics.h"

#include "TestSignals.h"
#include "vocalsynth/VoiceSettings.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <numbers>
#include <string>
#include <utility>
#include <vector>

namespace vseval
{

struct CorpusItem
{
    std::string name;
    vocalsynth::VoiceRange range = vocalsynth::VoiceRange::tenor;
    double sampleRate = 48000.0;
    std::vector<float> samples;
    LabelFile labels; ///< empty notes = no voice at all (noise, breath, silence): 0 notes expected
};

namespace corpus
{

using vocalsynth::test::Signal;

/** One step of a melody: midi < 0 is a rest. */
struct Step
{
    double midi;
    double seconds;
    double levelDb = -14.0; ///< RMS of the note in dBFS (sine-equivalent amplitude)
};

enum class Shape { sine, saw, missingFundamental, strongSecond };

inline std::string rateTag (double sr)
{
    char b[16];
    std::snprintf (b, sizeof (b), "%.0fk", std::floor (sr / 1000.0));
    return b;
}

inline std::string itemName (vocalsynth::VoiceRange range, const std::string& content, double sr)
{
    return std::string (vocalsynth::voiceRangeInfo (range).name) + "_" + content + "_" + rateTag (sr);
}

/** A melody of steady notes; legato steps are phase-continuous (no click). Labels = the notes. */
inline CorpusItem melody (vocalsynth::VoiceRange range, const std::string& content, double sr, const std::vector<Step>& steps,
                          Shape shape)
{
    using namespace vocalsynth::test;
    CorpusItem item { itemName (range, content, sr), range, sr, {}, {} };
    double t = 0.0, phase = 0.0;

    for (const auto& step : steps)
    {
        const auto n = numSamplesFor (step.seconds, sr);

        if (step.midi < 0.0)
        {
            item.samples.insert (item.samples.end(), n, 0.0f);
        }
        else if (shape == Shape::sine || shape == Shape::saw)
        {
            const auto amplitude = sineAmplitudeForRmsDb (step.levelDb);
            const auto f = midiToHz (step.midi);

            for (std::size_t i = 0; i < n; ++i)
            {
                item.samples.push_back (amplitude * static_cast<float> (shape == Shape::saw ? sawShape (phase) : sineShape (phase)));
                phase += f / sr;
                phase -= std::floor (phase);
            }
        }
        else
        {
            const auto amplitude = sineAmplitudeForRmsDb (step.levelDb);
            const auto note = shape == Shape::missingFundamental
                                  ? harmonicStack (sr, midiToHz (step.midi), step.seconds, 2, 8, amplitude)
                                  : harmonicStack (sr, midiToHz (step.midi), step.seconds, 1, 8, amplitude, 3.0);
            item.samples.insert (item.samples.end(), note.begin(), note.end());
        }

        if (step.midi >= 0.0)
            item.labels.notes.push_back ({ t, t + static_cast<double> (n) / sr, static_cast<int> (std::lround (step.midi)) });

        t += static_cast<double> (n) / sr;
    }

    return item;
}

/** Notes with vibrato (+-depthCents at 5.5 Hz) separated by rests. */
inline CorpusItem vibratoNotes (vocalsynth::VoiceRange range, double sr, const std::vector<double>& notes, double depthCents)
{
    using namespace vocalsynth::test;
    CorpusItem item { itemName (range, "vibrato", sr), range, sr, {}, {} };
    auto rest = silence (sr, 0.15);
    item.samples = rest;

    for (const auto midi : notes)
    {
        const auto onset = static_cast<double> (item.samples.size()) / sr;
        const auto note = vibrato (sr, midiToHz (midi), depthCents, 5.5, 0.8, sineAmplitudeForRmsDb (-14.0));
        item.samples.insert (item.samples.end(), note.begin(), note.end());
        item.labels.notes.push_back ({ onset, static_cast<double> (item.samples.size()) / sr, static_cast<int> (std::lround (midi)) });
        item.samples.insert (item.samples.end(), rest.begin(), rest.end());
    }

    return item;
}

/** Exponential glide; labels per semitone (where the glide is within +-0.5 st of the note), pitch only. */
inline CorpusItem glide (vocalsynth::VoiceRange range, double sr, double fromMidi, double toMidi, double seconds)
{
    using namespace vocalsynth::test;
    CorpusItem item { itemName (range, "glissando", sr), range, sr, {}, {} };
    const auto lead = 0.15;
    item.samples = concat ({ silence (sr, lead), glissando (sr, midiToHz (fromMidi), midiToHz (toMidi), seconds, 0.28f),
                             silence (sr, 0.2) });
    item.labels.pitchOnly = true;
    const auto rate = (toMidi - fromMidi) / seconds; // semitones per second
    const auto direction = rate > 0.0 ? 1 : -1;

    for (auto k = static_cast<int> (std::lround (fromMidi));; k += direction)
    {
        const auto timeAt = [&] (double midi) { return lead + std::clamp ((midi - fromMidi) / rate, 0.0, seconds); };
        const auto a = timeAt (k - 0.5 * direction), b = timeAt (k + 0.5 * direction);

        if (b > a)
            item.labels.notes.push_back ({ a, b, k });

        if (k == static_cast<int> (std::lround (toMidi)))
            break;
    }

    return item;
}

inline CorpusItem noiseItem (vocalsynth::VoiceRange range, const std::string& content, double sr, Signal samples)
{
    return { itemName (range, content, sr), range, sr, std::move (samples), {} };
}

inline Signal scaledToRmsDb (Signal s, double rmsDb)
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

/** Formants (Hz) of a vowel, with bandwidths 80 / 100 / 120 Hz. */
struct Vowel
{
    const char* name;
    double f1, f2, f3;
};

inline constexpr Vowel vowelA { "a", 730.0, 1090.0, 2440.0 };
inline constexpr Vowel vowelI { "i", 270.0, 2290.0, 3010.0 };
inline constexpr Vowel vowelU { "u", 300.0, 870.0, 2240.0 };
inline constexpr Vowel vowelO { "o", 570.0, 840.0, 2410.0 };

/** Voice-like signal: harmonic source with -6 dB/octave tilt (glottal -12 dB/oct + lip radiation),
    vibrato (5.5 Hz), slow random jitter (+-0.3 %), through a cascade of three formant resonators, with
    25 ms attack / 40 ms release per note, then white noise at snrDb below the voiced RMS. Legato steps
    (no rest in between) glide over 30 ms. Labels: the notes (step boundaries). */
inline CorpusItem voiceLike (vocalsynth::VoiceRange range, double sr, const std::vector<Step>& steps, const Vowel& vowel,
                             double vibratoCents, double snrDb, uint64_t seed)
{
    using namespace vocalsynth::test;
    CorpusItem item { itemName (range, std::string ("voice-") + vowel.name + "-snr" + std::to_string (static_cast<int> (snrDb)), sr),
                      range, sr, {}, {} };

    TestRandom rng (seed);
    double phase = 0.0, jitter = 0.0, t = 0.0;
    const auto jitterPole = std::exp (-2.0 * std::numbers::pi * 20.0 / sr); // ~20 Hz random drift
    double previousMidi = -1.0;
    Signal voiced;

    for (std::size_t s = 0; s < steps.size(); ++s)
    {
        const auto& step = steps[s];
        const auto n = numSamplesFor (step.seconds, sr);
        const auto nextIsNote = s + 1 < steps.size() && steps[s + 1].midi >= 0.0;
        const auto startsFromRest = previousMidi < 0.0;

        if (step.midi >= 0.0)
            item.labels.notes.push_back ({ t, t + static_cast<double> (n) / sr, static_cast<int> (std::lround (step.midi)) });

        for (std::size_t i = 0; i < n; ++i)
        {
            if (step.midi < 0.0)
            {
                voiced.push_back (0.0f);
                continue;
            }

            const auto local = static_cast<double> (i) / sr;
            const auto glide = ! startsFromRest && local < 0.03 ? previousMidi + (step.midi - previousMidi) * (local / 0.03) : step.midi;
            jitter = jitterPole * jitter + (1.0 - jitterPole) * rng.uniform() * 0.12;
            const auto vib = vibratoCents / 100.0 * std::sin (2.0 * std::numbers::pi * 5.5 * (t + local));
            const auto f0 = midiToHz (glide + vib) * (1.0 + jitter);

            double v = 0.0;

            for (int k = 1; static_cast<double> (k) * f0 < std::min (8000.0, 0.45 * sr); ++k)
                v += std::sin (2.0 * std::numbers::pi * static_cast<double> (k) * phase) / static_cast<double> (k);

            phase += f0 / sr;
            phase -= std::floor (phase);

            // Envelope: attack from a rest, release into a rest
            auto env = 1.0;

            if (startsFromRest && local < 0.025)
                env = 0.5 - 0.5 * std::cos (std::numbers::pi * local / 0.025);

            const auto remaining = step.seconds - local;

            if (! nextIsNote && remaining < 0.04)
                env *= 0.5 - 0.5 * std::cos (std::numbers::pi * std::max (0.0, remaining) / 0.04);

            voiced.push_back (static_cast<float> (env * v * std::pow (10.0, step.levelDb / 20.0)));
        }

        previousMidi = step.midi;
        t += static_cast<double> (n) / sr;
    }

    // Formant cascade (two-pole resonators, unity gain at DC removed by the pipeline's high-pass anyway)
    for (const auto& [f, bandwidth] : { std::pair { vowel.f1, 80.0 }, std::pair { vowel.f2, 100.0 }, std::pair { vowel.f3, 120.0 } })
    {
        if (f >= 0.45 * sr)
            continue;

        const auto r = std::exp (-std::numbers::pi * bandwidth / sr);
        const auto a1 = -2.0 * r * std::cos (2.0 * std::numbers::pi * f / sr);
        const auto a2 = r * r;
        const auto gain = 1.0 + a1 + a2; // unity gain at DC
        double y1 = 0.0, y2 = 0.0;

        for (auto& x : voiced)
        {
            const auto y = gain * static_cast<double> (x) - a1 * y1 - a2 * y2;
            y2 = y1;
            y1 = y;
            x = static_cast<float> (y);
        }
    }

    // Level: the loudest step at its levelDb (sine-equivalent RMS over the voiced samples), then noise
    item.samples = scaledToRmsDb (voiced, -16.0);
    double sum = 0.0;
    std::size_t count = 0;

    for (auto x : item.samples)
        if (x != 0.0f)
        {
            sum += static_cast<double> (x) * static_cast<double> (x);
            ++count;
        }

    const auto voicedRms = count > 0 ? std::sqrt (sum / static_cast<double> (count)) : 0.0;
    const auto noiseAmplitude = static_cast<float> (voicedRms * std::pow (10.0, -snrDb / 20.0) * std::sqrt (3.0));
    mixInto (item.samples, whiteNoise (sr, static_cast<double> (item.samples.size()) / sr, noiseAmplitude, seed + 1));
    return item;
}

/** Five notes spread over the range (10/30/50/70/90 % of its MIDI span), separated by short rests. */
inline std::vector<Step> rangeMelody (vocalsynth::VoiceRange range, double noteSeconds)
{
    const auto& info = vocalsynth::voiceRangeInfo (range);
    const auto lo = 12.0 * std::log2 (info.f0MinHz / 440.0) + 69.0;
    const auto hi = 12.0 * std::log2 (info.f0MaxHz / 440.0) + 69.0;
    std::vector<Step> steps { { -1.0, 0.15 } };
    const double levels[] = { -12.0, -18.0, -10.0, -24.0, -14.0 };
    int i = 0;

    for (const auto fraction : { 0.1, 0.3, 0.5, 0.7, 0.9 })
    {
        steps.push_back ({ std::round (lo + fraction * (hi - lo)), noteSeconds, levels[i++] });
        steps.push_back ({ -1.0, 0.12 });
    }

    return steps;
}

} // namespace corpus

/** The synthetic corpus (docs/M2-DESIGN.md 5): per-range melodies (sines and saws), legato lines,
    vibrato, glissando, octave leaps, missing fundamental / strong 2nd harmonic, and noise, breath and
    silence without any voice. Mixed sample rates. A -66 dBFS noise floor is added to every voiced item. */
inline std::vector<CorpusItem> buildSyntheticCorpus()
{
    using namespace vocalsynth;
    using namespace corpus;
    std::vector<CorpusItem> items;

    const VoiceRange ranges[] = { VoiceRange::bass,    VoiceRange::baritone, VoiceRange::tenor, VoiceRange::alto,
                                  VoiceRange::soprano, VoiceRange::whistle,  VoiceRange::wide };

    for (const auto range : ranges)
    {
        items.push_back (melody (range, "melody-sine", 48000.0, rangeMelody (range, 0.5), Shape::sine));

        // Whistling is nearly sinusoidal: the Whistle "saw" item is a second sine melody at another rate
        items.push_back (melody (range, range == VoiceRange::whistle ? "melody-sine" : "melody-saw",
                                 range == VoiceRange::whistle ? 96000.0 : 44100.0, rangeMelody (range, 0.4),
                                 range == VoiceRange::whistle ? Shape::sine : Shape::saw));
    }

    // Legato lines (note changes without a rest: stickiness T_min applies)
    items.push_back (melody (VoiceRange::tenor, "legato", 96000.0,
                             { { -1, 0.15 }, { 55, 0.35 }, { 57, 0.35 }, { 59, 0.35 }, { 60, 0.35 }, { 62, 0.35 }, { 59, 0.35 }, { -1, 0.2 } },
                             Shape::saw));
    items.push_back (melody (VoiceRange::bass, "legato", 48000.0,
                             { { -1, 0.15 }, { 40, 0.4 }, { 43, 0.4 }, { 45, 0.4 }, { 47, 0.4 }, { -1, 0.2 } }, Shape::sine));

    // Vibrato (+-40 cents: the frame metric uses +-50 cents around the label note)
    items.push_back (vibratoNotes (VoiceRange::tenor, 48000.0, { 55, 59, 62 }, 40.0));
    items.push_back (vibratoNotes (VoiceRange::baritone, 44100.0, { 45, 50 }, 40.0));
    items.push_back (vibratoNotes (VoiceRange::soprano, 96000.0, { 72, 76 }, 40.0));

    // Glissandi (pitch only)
    items.push_back (glide (VoiceRange::tenor, 48000.0, 55.0, 67.0, 2.0));
    items.push_back (glide (VoiceRange::bass, 44100.0, 52.0, 40.0, 2.0));

    // Octave leaps
    items.push_back (melody (VoiceRange::tenor, "octave-leaps", 48000.0,
                             { { -1, 0.15 }, { 57, 0.5 }, { 69, 0.5 }, { 57, 0.5 }, { -1, 0.2 } }, Shape::sine));
    items.push_back (melody (VoiceRange::wide, "octave-leaps", 44100.0,
                             { { -1, 0.15 }, { 48, 0.5 }, { 60, 0.5 }, { -1, 0.1 }, { 72, 0.5 }, { -1, 0.2 } }, Shape::saw));

    // Missing fundamental and strong 2nd harmonic
    items.push_back (melody (VoiceRange::tenor, "missing-fundamental", 48000.0,
                             { { -1, 0.15 }, { 48, 0.5 }, { -1, 0.1 }, { 52, 0.5 }, { -1, 0.1 }, { 57, 0.5 }, { -1, 0.2 } },
                             Shape::missingFundamental));
    items.push_back (melody (VoiceRange::bass, "missing-fundamental", 44100.0,
                             { { -1, 0.15 }, { 40, 0.5 }, { -1, 0.1 }, { 45, 0.5 }, { -1, 0.2 } }, Shape::missingFundamental));
    items.push_back (melody (VoiceRange::tenor, "strong-second", 96000.0,
                             { { -1, 0.15 }, { 50, 0.5 }, { -1, 0.1 }, { 57, 0.5 }, { -1, 0.2 } }, Shape::strongSecond));

    for (auto& item : items)
        test::mixInto (item.samples, test::whiteNoise (item.sampleRate, static_cast<double> (item.samples.size()) / item.sampleRate,
                                                       0.00087f, 4242)); // ~ -66 dBFS RMS

    // Voice-like phrases (formants, jitter, vibrato, attack/release, noise): rests and legato steps mixed
    const auto line = [] (double transpose)
    {
        return std::vector<Step> { { -1, 0.15 },
                                   { 50 + transpose, 0.45, -14 }, { -1, 0.1 },
                                   { 55 + transpose, 0.4, -18 },  { 57 + transpose, 0.4, -12 }, { -1, 0.12 },
                                   { 59 + transpose, 0.45, -16 }, { 62 + transpose, 0.35, -14 }, { 64 + transpose, 0.35, -14 },
                                   { -1, 0.12 },
                                   { 57 + transpose, 0.6, -22 },  { -1, 0.2 } };
    };

    items.push_back (voiceLike (VoiceRange::tenor, 48000.0, line (0), vowelA, 30.0, 30.0, 101));
    items.push_back (voiceLike (VoiceRange::tenor, 44100.0, line (0), vowelI, 30.0, 20.0, 102));
    items.push_back (voiceLike (VoiceRange::tenor, 96000.0, line (0), vowelU, 20.0, 15.0, 103));
    items.push_back (voiceLike (VoiceRange::tenor, 48000.0, line (0), vowelO, 30.0, 10.0, 104));
    items.push_back (voiceLike (VoiceRange::bass, 48000.0, line (-14), vowelA, 30.0, 20.0, 105));
    items.push_back (voiceLike (VoiceRange::bass, 44100.0, line (-12), vowelO, 20.0, 20.0, 106));
    items.push_back (voiceLike (VoiceRange::baritone, 48000.0, line (-7), vowelO, 30.0, 20.0, 107));
    items.push_back (voiceLike (VoiceRange::alto, 48000.0, line (9), vowelI, 30.0, 20.0, 108));
    items.push_back (voiceLike (VoiceRange::soprano, 44100.0, line (17), vowelA, 30.0, 20.0, 109));
    items.push_back (voiceLike (VoiceRange::wide, 48000.0,
                                { { -1, 0.15 }, { 45, 0.45, -14 }, { -1, 0.1 }, { 52, 0.4 }, { 60, 0.4 }, { -1, 0.1 },
                                  { 67, 0.4, -18 }, { 72, 0.4 }, { -1, 0.1 }, { 57, 0.5 }, { -1, 0.2 } },
                                vowelA, 30.0, 20.0, 110));

    // No voice: 0 notes expected
    items.push_back (noiseItem (VoiceRange::tenor, "white-noise", 48000.0, scaledToRmsDb (test::whiteNoise (48000.0, 2.0, 1.0f, 11), -20.0)));
    items.push_back (noiseItem (VoiceRange::bass, "pink-noise", 44100.0, scaledToRmsDb (test::pinkNoise (44100.0, 2.0, 1.0f, 12), -20.0)));
    items.push_back (noiseItem (VoiceRange::wide, "pink-noise", 96000.0, scaledToRmsDb (test::pinkNoise (96000.0, 2.0, 1.0f, 13), -12.0)));
    items.push_back (noiseItem (VoiceRange::tenor, "breath", 48000.0,
                                scaledToRmsDb (test::noiseBursts (48000.0, 3.0, 0.3, 0.6, 1.0f, 14), -20.0)));
    items.push_back (noiseItem (VoiceRange::bass, "breath", 44100.0,
                                scaledToRmsDb (test::noiseBursts (44100.0, 3.0, 0.08, 0.2, 1.0f, 15), -20.0)));
    items.push_back (noiseItem (VoiceRange::whistle, "breath", 96000.0,
                                scaledToRmsDb (test::noiseBursts (96000.0, 3.0, 0.3, 0.6, 1.0f, 16), -14.0)));
    items.push_back (noiseItem (VoiceRange::tenor, "silence", 48000.0, test::silence (48000.0, 1.0)));

    return items;
}

} // namespace vseval
