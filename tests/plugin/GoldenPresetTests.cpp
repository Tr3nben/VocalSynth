// Golden audio: every factory preset's feature fingerprint vs tests/data/golden/presets-0.4.json, peak <= -1 dBFS, no DC / silence / NaN.
// Test plan: docs/SYNTH-DESIGN.md 21 / 21.1. Owner: agent F (Sound / presets).
//
// The fingerprint (21.1, binding): SynthEngine directly at 48 kHz, prepare (48000, 4096), process() in chunks of
// 512, settings = SynthSettingsBuilder::fromPlain of the defaults overwritten by the preset (synthOn on,
// synthVolume -6 dB, bendRangeSemitones 2), FactoryWavetables::build of the preset's tables, 120 BPM 4/4, no
// bends, no timings; sequences held / legato / chord (Poly presets) with a scripted expression; per 100 ms
// window RMS, peak, spectral centroid and flatness. Features, not sample hashes: clang and MSVC do not compute
// bit-identically, the tolerances absorb that.
//
// Regenerate the JSON (Linux, clang, Release) after an intended sound change:
//     VOCALSYNTH_WRITE_GOLDEN=1 ./VocalSynthPluginTests "GoldenPreset: fingerprints match*"
// then review the diff of tests/data/golden/presets-0.4.json (which windows moved, by how much) and commit it.
//
// Level matching of the presets (FactoryPresets.cpp): loudnessTargetLufs +- loudnessToleranceLu, measured by
// presetLoudness() below, checked by "GoldenPreset: presets are level-matched". The hidden test
// "[.][presetmeasure]" prints loudness, peaks and CPU per preset (the numbers used to set ampGain).

#include "FactoryPresets.h"
#include "SynthParameters.h"
#include "SynthSettingsBuilder.h"
#include "vocalsynth/ExpressionFrame.h"
#include "vocalsynth/VsEvent.h"
#include "vocalsynth/synth/FactoryWavetables.h"
#include "vocalsynth/synth/RealFft.h"
#include "vocalsynth/synth/SynthEngine.h"

#include <juce_core/juce_core.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

namespace
{
using namespace vocalsynth;
namespace sy = vocalsynth::synth;

constexpr double sampleRate = 48000.0;
constexpr int chunkSamples = 512;
constexpr int windowSamples = 4800;  // 100 ms
constexpr int frameHopSamples = 96;  // one expression frame every 2 ms
constexpr int fftOrder = 13;         // 8192
constexpr double floorDb = -100.0;

// Level matching (FactoryPresets.cpp): every preset except Init
constexpr double loudnessTargetLufs = -16.0;
constexpr double loudnessToleranceLu = 1.0;
constexpr double peakLimitDbfs = -1.0;

int64_t seconds (double s)
{
    return static_cast<int64_t> (std::llround (s * sampleRate));
}

//==============================================================================
// Settings and tables

sy::SynthSettings settingsOf (const FactoryPreset& preset)
{
    auto plain = SynthSettingsBuilder::toPlain (sy::SynthSettings {});

    for (const auto& v : preset.values)
    {
        const auto index = SynthSettingsBuilder::indexOf (juce::String (std::string (v.id)));
        REQUIRE (index >= 0);
        plain[static_cast<std::size_t> (index)] = v.value;
    }

    plain[static_cast<std::size_t> (SynthSettingsBuilder::indexOf (params::synthOn.getParamID()))] = 1.0f;
    plain[static_cast<std::size_t> (SynthSettingsBuilder::indexOf (params::synthVolume.getParamID()))] = -6.0f;

    auto settings = SynthSettingsBuilder::fromPlain (plain);
    settings.bendRangeSemitones = 2;
    return settings;
}

/** Factory tables, built once per process (shared by every render of this test binary). */
const sy::Wavetable& factoryTable (std::string_view id)
{
    static std::map<std::string, std::unique_ptr<sy::Wavetable>, std::less<>> cache;

    if (const auto it = cache.find (id); it != cache.end())
        return *it->second;

    const auto table = sy::FactoryWavetables::fromId (id);
    REQUIRE (table.has_value());
    auto result = sy::FactoryWavetables::build (*table);
    REQUIRE (result.ok());
    return *cache.emplace (std::string (id), std::move (result.table)).first->second;
}

bool isPoly (const sy::SynthSettings& s)
{
    return s.playMode == sy::PlayMode::poly;
}

//==============================================================================
// Sequences and expression (21.1)

struct TimedEvent
{
    int64_t position = 0;
    VsEvent event;
};

enum class ExpressionKind
{
    golden,  ///< 21.1: level ramp, brightness t / T, noisiness raised cosine, pitch +-30 cents at 5 Hz
    singing, ///< level matching: constant "typical singing" (level 0.7, brightness 0.5, noisiness 0.2)
    rest     ///< nullptr: the engine's rest expression (Test Pattern)
};

struct Sequence
{
    std::string name;
    std::vector<TimedEvent> events; ///< in time order; same position: event order
    int64_t heldEnd = 0;            ///< the last note-off (the notes are held without a gap from 0)
    int64_t length = 0;             ///< incl. the 1 s tail
};

Sequence heldSequence()
{
    Sequence s { "held", {}, seconds (2.0), seconds (3.0) };
    s.events.push_back ({ 0, VsEvent::makeNoteOn (0, 1, 60, 100) });
    s.events.push_back ({ s.heldEnd, VsEvent::makeNoteOff (0, 1, 60) });
    return s;
}

Sequence legatoSequence()
{
    // The pipeline's legato order: note-on of the new note, then note-off of the old one at the same sample
    Sequence s { "legato", {}, seconds (1.0), seconds (2.0) };
    const uint8_t notes[] { 60, 64, 67, 72 };

    for (int i = 0; i < 4; ++i)
    {
        const auto at = seconds (0.25 * i);
        s.events.push_back ({ at, VsEvent::makeNoteOn (0, 1, notes[i], 100) });

        if (i > 0)
            s.events.push_back ({ at, VsEvent::makeNoteOff (0, 1, notes[i - 1]) });
    }

    s.events.push_back ({ s.heldEnd, VsEvent::makeNoteOff (0, 1, 72) });
    return s;
}

Sequence chordSequence (std::initializer_list<uint8_t> notes, const char* name)
{
    Sequence s { name, {}, seconds (1.5), seconds (2.5) };

    for (const auto n : notes)
        s.events.push_back ({ 0, VsEvent::makeNoteOn (0, 1, n, 100) });

    for (const auto n : notes)
        s.events.push_back ({ s.heldEnd, VsEvent::makeNoteOff (0, 1, n) });

    return s;
}

ExpressionFrame expressionAt (ExpressionKind kind, const Sequence& s, int64_t position)
{
    const double t = static_cast<double> (position) / sampleRate;
    const double heldEnd = static_cast<double> (s.heldEnd) / sampleRate;
    const double total = static_cast<double> (s.length) / sampleRate;
    ExpressionFrame f;

    if (kind == ExpressionKind::singing)
    {
        const double level = t < heldEnd ? 0.7 : std::max (0.0, 0.7 * (1.0 - (t - heldEnd) / 0.1));
        f.level = static_cast<float> (level);
        f.brightness = 0.5f;
        f.noisiness = 0.2f;
        return f;
    }

    const double endLevel = std::min (1.0, heldEnd / 0.5);
    const double level = t < heldEnd ? std::min (1.0, t / 0.5) : std::max (0.0, endLevel * (1.0 - (t - heldEnd) / 0.1));
    f.level = static_cast<float> (level);
    f.brightness = static_cast<float> (t / total);
    f.noisiness = static_cast<float> (0.5 - 0.5 * std::cos (2.0 * std::numbers::pi * t / total));
    f.pitchCents = static_cast<float> (30.0 * std::sin (2.0 * std::numbers::pi * 5.0 * t));
    return f;
}

//==============================================================================
// Render

struct Render
{
    std::vector<float> left, right;
    double processSeconds = 0.0; ///< wall time inside SynthEngine::process
    int numCalls = 0;
};

Render render (const FactoryPreset& preset, const Sequence& sequence, ExpressionKind kind, int chunk = chunkSamples)
{
    const auto settings = settingsOf (preset);
    const auto& tableA = factoryTable (preset.tableA);
    const auto& tableB = factoryTable (preset.tableB);
    const sy::SynthTransport transport { 120.0, 4.0 };

    auto engine = std::make_unique<sy::SynthEngine>();
    engine->prepare (sampleRate, sy::SynthEngine::maxChunkSamples);

    Render r;
    r.left.assign (static_cast<std::size_t> (sequence.length), 0.0f);
    r.right.assign (static_cast<std::size_t> (sequence.length), 0.0f);

    std::vector<VsEvent> events;
    ExpressionBuffer frames;
    std::size_t nextEvent = 0;

    for (int64_t start = 0; start < sequence.length; start += chunk)
    {
        const auto n = static_cast<int> (std::min<int64_t> (chunk, sequence.length - start));
        events.clear();

        while (nextEvent < sequence.events.size() && sequence.events[nextEvent].position < start + n)
        {
            auto e = sequence.events[nextEvent++].event;
            e.sampleOffset = static_cast<int> (sequence.events[nextEvent - 1].position - start);
            events.push_back (e);
        }

        frames.clear();

        for (int64_t p = (start + frameHopSamples - 1) / frameHopSamples * frameHopSamples; p < start + n;
             p += frameHopSamples)
        {
            auto f = expressionAt (kind, sequence, p);
            f.sampleOffset = static_cast<int> (p - start);
            frames.push (f);
        }

        engine->setWavetable (0, &tableA);
        engine->setWavetable (1, &tableB);

        const auto t0 = std::chrono::steady_clock::now();
        engine->process (events, {}, kind == ExpressionKind::rest ? nullptr : &frames, settings, transport,
                         r.left.data() + start, r.right.data() + start, n);
        r.processSeconds += std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
        ++r.numCalls;
    }

    return r;
}

//==============================================================================
// Features (21.1)

struct Features
{
    double rms = floorDb, peak = floorDb, centroid = 0.0, flatness = 0.0;
};

double toDb (double linear)
{
    return linear > 0.0 ? std::max (floorDb, 20.0 * std::log10 (linear)) : floorDb;
}

std::vector<Features> fingerprint (const Render& r)
{
    sy::RealFft fft (fftOrder);
    const int n = fft.getSize();
    std::vector<double> input (static_cast<std::size_t> (n));
    std::vector<std::complex<double>> bins (static_cast<std::size_t> (fft.getNumBins()));
    std::vector<double> hann (windowSamples);

    for (int i = 0; i < windowSamples; ++i)
        hann[static_cast<std::size_t> (i)] = 0.5 - 0.5 * std::cos (2.0 * std::numbers::pi * i / (windowSamples - 1));

    const double binHz = sampleRate / n;
    const int firstBin = static_cast<int> (std::ceil (20.0 / binHz));
    const int lastBin = static_cast<int> (std::floor (20000.0 / binHz));

    std::vector<Features> result;

    for (std::size_t start = 0; start + windowSamples <= r.left.size(); start += windowSamples)
    {
        double sumSquares = 0.0, peak = 0.0;
        std::fill (input.begin(), input.end(), 0.0);

        for (int i = 0; i < windowSamples; ++i)
        {
            const auto j = start + static_cast<std::size_t> (i);
            const double m = 0.5 * (static_cast<double> (r.left[j]) + static_cast<double> (r.right[j]));
            sumSquares += m * m;
            peak = std::max ({ peak, std::abs (static_cast<double> (r.left[j])), std::abs (static_cast<double> (r.right[j])) });
            input[static_cast<std::size_t> (i)] = m * hann[static_cast<std::size_t> (i)];
        }

        fft.forward (input, bins);

        double sumPower = 0.0, sumWeighted = 0.0, sumLog = 0.0;

        for (int k = firstBin; k <= lastBin; ++k)
        {
            const double p = std::max (1.0e-20, std::norm (bins[static_cast<std::size_t> (k)]));
            sumPower += p;
            sumWeighted += p * k * binHz;
            sumLog += std::log (p);
        }

        const double count = lastBin - firstBin + 1;
        Features f;
        f.rms = toDb (std::sqrt (sumSquares / windowSamples));
        f.peak = toDb (peak);
        f.centroid = sumWeighted / sumPower;
        f.flatness = std::exp (sumLog / count) / (sumPower / count);
        result.push_back (f);
    }

    return result;
}

double peakDb (const Render& r)
{
    float peak = 0.0f;

    for (std::size_t i = 0; i < r.left.size(); ++i)
        peak = std::max ({ peak, std::abs (r.left[i]), std::abs (r.right[i]) });

    return toDb (peak);
}

bool allFinite (const Render& r)
{
    for (std::size_t i = 0; i < r.left.size(); ++i)
        if (! std::isfinite (r.left[i]) || ! std::isfinite (r.right[i]))
            return false;

    return true;
}

double meanOf (const std::vector<float>& x)
{
    double sum = 0.0;

    for (const auto v : x)
        sum += v;

    return x.empty() ? 0.0 : sum / static_cast<double> (x.size());
}

//==============================================================================
// Loudness for the level matching: ITU-R BS.1770 K-weighting (48 kHz coefficients), maximum momentary
// loudness (400 ms windows, 100 ms hop, ungated), L and R summed. A single held C4 with the "singing" expression.

struct Biquad
{
    double b0, b1, b2, a1, a2;
    double z1 = 0.0, z2 = 0.0;

    double process (double x) noexcept
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

std::vector<double> kWeightedSquares (const std::vector<float>& x)
{
    Biquad shelf { 1.53512485958697, -2.69169618940638, 1.19839281085285, -1.69065929318241, 0.73248077421585 };
    Biquad highPass { 1.0, -2.0, 1.0, -1.99004745483398, 0.99007225036621 };
    std::vector<double> out (x.size());

    for (std::size_t i = 0; i < x.size(); ++i)
    {
        const double y = highPass.process (shelf.process (static_cast<double> (x[i])));
        out[i] = y * y;
    }

    return out;
}

double maxMomentaryLufs (const Render& r)
{
    const auto l = kWeightedSquares (r.left);
    const auto rr = kWeightedSquares (r.right);
    const auto blockLength = static_cast<std::size_t> (seconds (0.4));
    const auto hop = static_cast<std::size_t> (seconds (0.1));
    double best = -200.0;

    for (std::size_t start = 0; start + blockLength <= l.size(); start += hop)
    {
        double sum = 0.0;

        for (std::size_t i = start; i < start + blockLength; ++i)
            sum += l[i] + rr[i];

        const double power = sum / static_cast<double> (blockLength);
        best = std::max (best, power > 0.0 ? -0.691 + 10.0 * std::log10 (power) : -200.0);
    }

    return best;
}

double presetLoudness (const FactoryPreset& preset)
{
    return maxMomentaryLufs (render (preset, heldSequence(), ExpressionKind::singing));
}

//==============================================================================
// Golden file

juce::File goldenFile()
{
    return juce::File (juce::String::fromUTF8 (VOCALSYNTH_TEST_DATA_DIR)).getChildFile ("golden").getChildFile ("presets-0.4.json");
}

bool writeGoldenRequested()
{
    const char* value = std::getenv ("VOCALSYNTH_WRITE_GOLDEN");
    return value != nullptr && std::string (value) == "1";
}

struct PresetFingerprints
{
    std::string name;
    std::vector<Features> held, legato, chord; ///< chord empty for non-Poly presets
};

PresetFingerprints fingerprintsOf (const FactoryPreset& preset)
{
    PresetFingerprints p;
    p.name = std::string (preset.name);
    p.held = fingerprint (render (preset, heldSequence(), ExpressionKind::golden));
    p.legato = fingerprint (render (preset, legatoSequence(), ExpressionKind::golden));

    if (isPoly (settingsOf (preset)))
        p.chord = fingerprint (render (preset, chordSequence ({ 60, 64, 67 }, "chord"), ExpressionKind::golden));

    return p;
}

juce::String formatWindows (const std::vector<Features>& windows)
{
    juce::StringArray rows;

    for (const auto& f : windows)
        rows.add ("[" + juce::String (f.rms, 2) + ", " + juce::String (f.peak, 2) + ", " + juce::String (f.centroid, 1)
                  + ", " + juce::String (f.flatness, 4) + "]");

    return "[\n        " + rows.joinIntoString (",\n        ") + "\n      ]";
}

juce::String toJson (const std::vector<PresetFingerprints>& all)
{
    juce::StringArray presets;

    for (const auto& p : all)
    {
        juce::String entry = "    {\n      \"name\": " + juce::JSON::toString (juce::String (p.name)) + ",\n";
        entry << "      \"held\": " << formatWindows (p.held) << ",\n";
        entry << "      \"legato\": " << formatWindows (p.legato);

        if (! p.chord.empty())
            entry << ",\n      \"chord\": " << formatWindows (p.chord);

        entry << "\n    }";
        presets.add (entry);
    }

    return "{\n  \"version\": 1,\n  \"sampleRate\": 48000,\n  \"window\": 4800,\n  \"presets\": [\n"
           + presets.joinIntoString (",\n") + "\n  ]\n}\n";
}

std::vector<Features> parseWindows (const juce::var& array)
{
    std::vector<Features> result;

    if (const auto* rows = array.getArray())
        for (const auto& row : *rows)
            if (const auto* v = row.getArray(); v != nullptr && v->size() == 4)
                result.push_back ({ static_cast<double> ((*v)[0]), static_cast<double> ((*v)[1]),
                                    static_cast<double> ((*v)[2]), static_cast<double> ((*v)[3]) });

    return result;
}

void compareWindows (const std::string& preset, const char* sequence, const std::vector<Features>& reference,
                     const std::vector<Features>& actual)
{
    CAPTURE (preset, sequence);
    REQUIRE (actual.size() == reference.size());

    for (std::size_t w = 0; w < actual.size(); ++w)
    {
        const auto& ref = reference[w];
        const auto& got = actual[w];
        CAPTURE (w, ref.rms, got.rms, ref.peak, got.peak, ref.centroid, got.centroid, ref.flatness, got.flatness);
        CHECK (std::abs (got.rms - ref.rms) <= 1.5);
        CHECK (std::abs (got.peak - ref.peak) <= 1.5);

        if (ref.rms > -60.0 && got.rms > -60.0)
        {
            CHECK (std::abs (got.centroid - ref.centroid) <= 0.10 * ref.centroid);
            CHECK (std::abs (got.flatness - ref.flatness) <= 0.05);
        }
    }
}

const char* playModeName (const sy::SynthSettings& s)
{
    return params::playModeNames[static_cast<std::size_t> (s.playMode)];
}
} // namespace

//==============================================================================

TEST_CASE ("GoldenPreset: fingerprints match tests/data/golden/presets-0.4.json", "[plugin][synth][golden]")
{
    std::vector<PresetFingerprints> all;

    for (int i = 0; i < FactoryPresets::getNumPresets(); ++i)
        all.push_back (fingerprintsOf (FactoryPresets::getPreset (i)));

    const auto file = goldenFile();

    if (writeGoldenRequested())
    {
        REQUIRE (file.getParentDirectory().createDirectory());
        REQUIRE (file.replaceWithText (toJson (all), false, false, "\n"));
        WARN ("VOCALSYNTH_WRITE_GOLDEN=1: rewrote " << file.getFullPathName());
        return;
    }

    REQUIRE (file.existsAsFile());
    const auto json = juce::JSON::parse (file);
    REQUIRE (static_cast<int> (json.getProperty ("version", 0)) == 1);
    REQUIRE (static_cast<int> (json.getProperty ("sampleRate", 0)) == 48000);
    REQUIRE (static_cast<int> (json.getProperty ("window", 0)) == windowSamples);

    const auto* presets = json.getProperty ("presets", {}).getArray();
    REQUIRE (presets != nullptr);
    CHECK (presets->size() == static_cast<int> (all.size()));

    for (const auto& p : all)
    {
        const juce::var* entry = nullptr;

        for (const auto& candidate : *presets)
            if (candidate.getProperty ("name", {}).toString() == juce::String (p.name))
                entry = &candidate;

        CAPTURE (p.name);
        REQUIRE (entry != nullptr);
        compareWindows (p.name, "held", parseWindows (entry->getProperty ("held", {})), p.held);
        compareWindows (p.name, "legato", parseWindows (entry->getProperty ("legato", {})), p.legato);
        compareWindows (p.name, "chord", parseWindows (entry->getProperty ("chord", {})), p.chord);
    }
}

TEST_CASE ("GoldenPreset: every preset is finite, DC-free, audible and peaks below -1 dBFS", "[plugin][synth][golden]")
{
    for (int i = 0; i < FactoryPresets::getNumPresets(); ++i)
    {
        const auto& preset = FactoryPresets::getPreset (i);
        CAPTURE (preset.name);

        std::vector<std::pair<std::string, Render>> renders;
        renders.emplace_back ("held", render (preset, heldSequence(), ExpressionKind::golden));
        renders.emplace_back ("legato", render (preset, legatoSequence(), ExpressionKind::golden));
        renders.emplace_back ("held rest", render (preset, heldSequence(), ExpressionKind::rest));
        renders.emplace_back ("held singing", render (preset, heldSequence(), ExpressionKind::singing));

        if (isPoly (settingsOf (preset)))
        {
            renders.emplace_back ("chord", render (preset, chordSequence ({ 60, 64, 67 }, "chord"), ExpressionKind::golden));
            renders.emplace_back ("chord 4", render (preset, chordSequence ({ 60, 64, 67, 72 }, "chord4"), ExpressionKind::golden));
        }

        for (const auto& [name, r] : renders)
        {
            CAPTURE (name);
            REQUIRE (allFinite (r));
            CHECK (peakDb (r) <= peakLimitDbfs);
            CHECK (std::abs (meanOf (r.left)) < 1.0e-3);
            CHECK (std::abs (meanOf (r.right)) < 1.0e-3);

            const auto windows = fingerprint (r);
            CHECK (std::any_of (windows.begin(), windows.end(), [] (const Features& f) { return f.rms > -40.0; }));
        }
    }
}

TEST_CASE ("GoldenPreset: presets are level-matched with ampGain", "[plugin][synth][golden]")
{
    for (int i = 1; i < FactoryPresets::getNumPresets(); ++i)
    {
        const auto& preset = FactoryPresets::getPreset (i);
        const auto loudness = presetLoudness (preset);
        CAPTURE (preset.name, loudness);
        CHECK (std::abs (loudness - loudnessTargetLufs) <= loudnessToleranceLu);
    }
}

TEST_CASE ("GoldenPreset: measure loudness, peaks and CPU per preset", "[.][presetmeasure]")
{
    std::printf ("%-16s %-6s %8s %8s %8s %8s %8s %8s %8s\n", "preset", "mode", "LUFS", "pkHeld", "pkLeg", "pkRest",
                 "pkChd4", "cpu%", "cpuChd%");

    double heaviest = 0.0;
    std::string heaviestName;

    for (int i = 0; i < FactoryPresets::getNumPresets(); ++i)
    {
        const auto& preset = FactoryPresets::getPreset (i);
        const auto settings = settingsOf (preset);
        const auto loudness = presetLoudness (preset);
        const auto held = render (preset, heldSequence(), ExpressionKind::golden);
        const auto legato = render (preset, legatoSequence(), ExpressionKind::golden);
        const auto rest = render (preset, heldSequence(), ExpressionKind::rest);

        // CPU: 48 kHz, blocks of 128 (1 % = 26.7 us per block), best of 3 runs of the legato phrase
        const auto cpuOf = [&preset] (const Sequence& s)
        {
            double best = 1.0e9;

            for (int run = 0; run < 3; ++run)
            {
                const auto r = render (preset, s, ExpressionKind::golden, 128);
                best = std::min (best, r.processSeconds / r.numCalls / (128.0 / sampleRate) * 100.0);
            }

            return best;
        };

        const double cpu = cpuOf (legatoSequence());
        double chordPeak = floorDb, cpuChord = 0.0;

        if (isPoly (settings))
        {
            chordPeak = peakDb (render (preset, chordSequence ({ 60, 64, 67, 72 }, "chord4"), ExpressionKind::golden));
            cpuChord = cpuOf (chordSequence ({ 60, 64, 67, 72 }, "chord4"));
        }

        if (std::max (cpu, cpuChord) > heaviest)
        {
            heaviest = std::max (cpu, cpuChord);
            heaviestName = std::string (preset.name);
        }

        std::printf ("%-16s %-6s %8.2f %8.2f %8.2f %8.2f %8.2f %8.3f %8.3f\n", std::string (preset.name).c_str(),
                     playModeName (settings), loudness, peakDb (held), peakDb (legato), peakDb (rest), chordPeak, cpu,
                     cpuChord);
    }

    std::printf ("heaviest: %s %.3f %%\n", heaviestName.c_str(), heaviest);
    SUCCEED();
}
