// SynthEngine (docs/SYNTH-DESIGN.md 2, 3, 10, 11, 12.1, 16.4): exact block-size invariance (also with FX, tails,
// synthOn off / on, table changes and real VoicePipeline event streams), no allocation on the audio path, silence
// after the tails, bend ranges and the bend jump from silence, expression interpolation and rest expression,
// table crossfade without click, table pointer protocol (reset forgets, retire simulation with real frees),
// synthOn fade, play-mode change, LFO retrigger, correction flag, global destinations, NaN robustness, the voice
// deactivation table fix, benchmarks (typical / worst / silence) and the Poly unison-cap decision.
// Test plan: docs/SYNTH-DESIGN.md 21. Owner: agent C (Engine + FX + Expression).

#include "vocalsynth/synth/SynthEngine.h"

#include "vocalsynth/VoicePipeline.h"

#include "../TestSignals.h"
#include "SynthTestUtils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <random>
#include <set>
#include <vector>

#if defined(__GLIBC__)
 #include <malloc.h>
#endif

using namespace vocalsynth;
using namespace vocalsynth::synth;
using Catch::Matchers::WithinAbs;
namespace st = vocalsynth::synth::test;
namespace sig = vocalsynth::test;

//==============================================================================
// Allocation counter for the audio path. tests/core/CaptureTapTests.cpp already replaces the global operator new
// of this binary (its counter is private to that file), and its replacement calls std::malloc - so this file
// counts one level lower, at malloc itself (glibc: an executable may define malloc / calloc / realloc and forward
// to __libc_*). Counting is armed per thread around the real-time calls only. Not available on other C
// libraries or under sanitizers (they own malloc): the test then reports a skip.
#if defined(__GLIBC__) && ! defined(__SANITIZE_ADDRESS__) && ! defined(__SANITIZE_THREAD__)
 #if defined(__has_feature)
  #if ! __has_feature(address_sanitizer) && ! __has_feature(thread_sanitizer) && ! __has_feature(memory_sanitizer)
   #define VOCALSYNTH_COUNT_MALLOC 1
  #endif
 #else
  #define VOCALSYNTH_COUNT_MALLOC 1
 #endif
#endif

#if defined(VOCALSYNTH_COUNT_MALLOC)
extern "C" void* __libc_malloc (std::size_t size) noexcept;
extern "C" void* __libc_calloc (std::size_t count, std::size_t size) noexcept;
extern "C" void* __libc_realloc (void* p, std::size_t size) noexcept;

namespace
{
constinit thread_local bool countingMallocs = false;
constinit thread_local std::size_t mallocCount = 0;
} // namespace

extern "C" void* malloc (std::size_t size) noexcept
{
    if (countingMallocs)
        ++mallocCount;

    return __libc_malloc (size);
}

extern "C" void* calloc (std::size_t count, std::size_t size) noexcept
{
    if (countingMallocs)
        ++mallocCount;

    return __libc_calloc (count, size);
}

extern "C" void* realloc (void* p, std::size_t size) noexcept
{
    if (countingMallocs)
        ++mallocCount;

    return __libc_realloc (p, size);
}
#endif

namespace
{
#if defined(VOCALSYNTH_COUNT_MALLOC)
struct CountMallocs
{
    CountMallocs() noexcept
    {
        mallocCount = 0;
        countingMallocs = true;
    }
    ~CountMallocs() { countingMallocs = false; }
    CountMallocs (const CountMallocs&) = delete;
    CountMallocs& operator= (const CountMallocs&) = delete;
    std::size_t count() const noexcept { return mallocCount; }
};
#endif

constexpr std::array<int, 6> hostBlocks { 1, 7, 64, 480, 512, 4096 };
constexpr std::array<int, 9> varyingBlocks { 1, 300, 17, 4096, 64, 2, 511, 1000, 7 };

//==============================================================================
// Scenes: events, expression frames, settings and tables at ABSOLUTE positions, rendered in any host split

struct TimedEvent
{
    int position = 0;
    VsEvent event;
    uint8_t flags = 0;
};

struct TimedFrame
{
    int position = 0;
    ExpressionFrame frame;
};

struct Scene
{
    double sr = 48000.0;
    int length = 0;
    std::vector<TimedEvent> events;
    std::vector<TimedFrame> frames;
    bool voiceMode = false; ///< expression buffer given (Voice mode) or nullptr (Test Pattern)
    std::vector<std::pair<int, SynthSettings>> settings;                  ///< from that position on
    std::vector<std::pair<int, std::array<const Wavetable*, 2>>> tables;  ///< from that position on
    std::vector<int> releaseAllAt;
    SynthTransport transport { 128.0, 4.0 };

    void on (double seconds, int note, int velocity = 100, uint8_t flags = 0)
    {
        events.push_back ({ at (seconds), VsEvent::makeNoteOn (0, 1, static_cast<uint8_t> (note), static_cast<uint8_t> (velocity)), flags });
    }

    void off (double seconds, int note, uint8_t flags = 0)
    {
        events.push_back ({ at (seconds), VsEvent::makeNoteOff (0, 1, static_cast<uint8_t> (note)), flags });
    }

    void bend (double seconds, int value)
    {
        events.push_back ({ at (seconds), VsEvent::makePitchBend (0, 1, static_cast<int16_t> (value)), 0 });
    }

    int at (double seconds) const { return static_cast<int> (std::lround (seconds * sr)); }

    void sortEvents()
    {
        std::stable_sort (events.begin(), events.end(), [] (const TimedEvent& a, const TimedEvent& b) { return a.position < b.position; });
    }
};

template <typename T>
const T& latest (const std::vector<std::pair<int, T>>& list, int position, const T& fallback)
{
    const T* found = &fallback;

    for (const auto& [start, value] : list)
        if (start <= position)
            found = &value;

    return *found;
}

int blockAt (int block, std::size_t index)
{
    return block > 0 ? block : varyingBlocks[index % varyingBlocks.size()];
}

/** Renders a scene in host blocks of `block` (0 = varying), every block cut at the scene's change points (so
    that every split sees the same settings from the same absolute sample on). */
st::Stereo render (SynthEngine& engine, const Scene& scene, int block)
{
    st::Stereo out (static_cast<std::size_t> (scene.length));
    std::vector<int> cuts;

    for (const auto& s : scene.settings)
        cuts.push_back (s.first);

    for (const auto& t : scene.tables)
        cuts.push_back (t.first);

    cuts.insert (cuts.end(), scene.releaseAllAt.begin(), scene.releaseAllAt.end());
    std::sort (cuts.begin(), cuts.end());

    std::vector<VsEvent> events;
    std::vector<VsEventTiming> timings;
    events.reserve (scene.events.size() + 1);
    timings.reserve (scene.events.size() + 1);
    ExpressionBuffer frames;
    std::size_t nextEvent = 0, nextFrame = 0, index = 0;
    const SynthSettings defaults {};
    const std::array<const Wavetable*, 2> noTables {};
    int pos = 0;

    while (pos < scene.length)
    {
        int n = std::min (blockAt (block, index++), scene.length - pos);

        for (const auto c : cuts)
            if (c > pos && c < pos + n)
                n = c - pos;

        if (std::find (scene.releaseAllAt.begin(), scene.releaseAllAt.end(), pos) != scene.releaseAllAt.end())
            engine.releaseAll();

        const auto& t = latest (scene.tables, pos, noTables);
        engine.setWavetable (0, t[0]);
        engine.setWavetable (1, t[1]);

        events.clear();
        timings.clear();
        frames.clear();

        while (nextEvent < scene.events.size() && scene.events[nextEvent].position < pos + n)
        {
            auto e = scene.events[nextEvent].event;
            e.sampleOffset = scene.events[nextEvent].position - pos;
            events.push_back (e);
            timings.push_back ({ 0, EventTimingKind::none, scene.events[nextEvent].flags });
            ++nextEvent;
        }

        while (nextFrame < scene.frames.size() && scene.frames[nextFrame].position < pos + n)
        {
            auto f = scene.frames[nextFrame++].frame;
            f.sampleOffset = scene.frames[nextFrame - 1].position - pos;
            frames.push(f);
        }

        engine.process (events, timings, scene.voiceMode ? &frames : nullptr, latest (scene.settings, pos, defaults),
                        scene.transport, out.left.data() + pos, out.right.data() + pos, n);
        (void) engine.getTablesInUse (0); // as the processor does at every block end
        (void) engine.getTablesInUse (1);
        pos += n;
    }

    return out;
}

st::Stereo render (const Scene& scene, int block)
{
    SynthEngine engine;
    engine.prepare (scene.sr, SynthEngine::maxChunkSamples);
    return render (engine, scene, block);
}

SynthSettings basicSettings()
{
    SynthSettings s;
    s.synthOn = true;
    s.synthVolumeDb = 0.0f;
    s.ampVelocityPercent = 0.0f;
    return s;
}

/** Plain sine voice (the built-in sine table, no filter): the pitch is easy to measure. */
SynthSettings sineSettings()
{
    auto s = basicSettings();
    s.filterOn = false;
    s.osc[0].levelPercent = 100.0f;
    s.env[0] = { 0.0f, 0.0f, 100.0f, 5.0f };
    return s;
}

/** The busy patch of the invariance tests: everything that has state is in use. */
SynthSettings busySettings()
{
    auto s = basicSettings();
    s.playMode = PlayMode::poly;
    s.glideTimeMs = 80.0f;
    s.glideMode = GlideMode::always;
    s.ampVelocityPercent = 60.0f;
    s.ampGainDb = -3.0f;
    s.synthVolumeDb = -6.0f;
    s.osc[0] = { true, 30.0f, 0, 0, 7.0f, 60.0f, -20.0f, 3, 30.0f, 80.0f, 50.0f };
    s.osc[1] = { true, 70.0f, -1, 7, -5.0f, 40.0f, 30.0f, 2, 15.0f, 40.0f, 100.0f };
    s.subLevelPercent = 25.0f;
    s.subShape = SubShape::square;
    s.noiseLevelPercent = 8.0f;
    s.noiseColorPercent = 50.0f;
    s.filterMode = FilterMode::lp24;
    s.filterCutoffHz = 2500.0f;
    s.filterResonancePercent = 35.0f;
    s.filterDriveDb = 6.0f;
    s.filterKeytrackPercent = 50.0f;
    s.filterEnvAmountPercent = 40.0f;
    s.env[0] = { 5.0f, 200.0f, 70.0f, 250.0f };
    s.env[1] = { 2.0f, 400.0f, 10.0f, 300.0f };
    s.env[2] = { 50.0f, 150.0f, 50.0f, 100.0f };
    s.lfo[0] = { LfoShape::triangle, 5.0f, TempoDivision::d1_8, true, 100.0f };
    s.lfo[1] = { LfoShape::sampleHold, 7.0f, TempoDivision::off, false, 0.0f };
    s.mod[0] = { ModSource::lfo1, ModDest::oscAPosition, 40.0f };
    s.mod[1] = { ModSource::lfo2, ModDest::filterCutoff, 20.0f };
    s.mod[2] = { ModSource::env2, ModDest::oscBPitch, 10.0f };
    s.mod[3] = { ModSource::voiceLevel, ModDest::ampLevel, -30.0f };
    s.mod[4] = { ModSource::voiceBrightness, ModDest::reverbMix, 50.0f };
    s.mod[5] = { ModSource::velocity, ModDest::delayMix, 20.0f };
    s.mod[6] = { ModSource::lfo1, ModDest::lfo2Rate, 50.0f };
    s.mod[7] = { ModSource::voicePitch, ModDest::pitch, 8.33f };
    s.chorus = { true, 0.8f, 60.0f, 40.0f };
    s.delay = { true, 375.0f, TempoDivision::d1_16, 50.0f, true, 30.0f };
    s.reverb = { true, 30.0f, 40.0f, 30.0f };
    return s;
}

/** Expression frames every 2 ms (rounded to samples) with smooth synthetic values. */
void addFrames (Scene& scene, double from, double to)
{
    const int hop = std::max (1, static_cast<int> (std::lround (0.002 * scene.sr)));

    for (int p = scene.at (from); p < scene.at (to); p += hop)
    {
        const double t = static_cast<double> (p) / scene.sr;
        ExpressionFrame f;
        f.level = static_cast<float> (0.5 + 0.5 * std::sin (2.0 * std::numbers::pi * 1.3 * t));
        f.brightness = static_cast<float> (0.5 + 0.5 * std::sin (2.0 * std::numbers::pi * 0.7 * t));
        f.noisiness = static_cast<float> (0.5 - 0.5 * std::cos (2.0 * std::numbers::pi * 0.4 * t));
        f.pitchCents = static_cast<float> (30.0 * std::sin (2.0 * std::numbers::pi * 5.0 * t));
        scene.frames.push_back ({ p, f });
    }
}

/** Notes (chords, steals, legato lines with corrections), bends, settings phases (Poly -> Legato -> synthOn off /
    on -> Mono), table changes during notes (one queued during a running crossfade), a flush, long FX tails. */
Scene invarianceScene (double sr, bool voiceMode)
{
    Scene s;
    s.sr = sr;
    s.length = s.at (7.0); // the FDN needs 2 x RT60 (0.86 s) to reach -120 dB after the delay's tail
    s.voiceMode = voiceMode;

    auto poly = busySettings();
    auto legato = poly;
    legato.playMode = PlayMode::legato;
    legato.glideMode = GlideMode::legatoOnly;
    legato.glideTimeMs = 120.0f;
    legato.filterCutoffHz = 6000.0f;
    legato.osc[0].unison = 5;
    auto off = legato;
    off.synthOn = false;
    auto mono = legato;
    mono.playMode = PlayMode::mono;
    mono.lfo[0].sync = TempoDivision::bar1;
    s.settings = { { 0, poly }, { s.at (1.5), legato }, { s.at (2.2), off }, { s.at (2.35), mono } };

    s.tables = { { 0, { &st::sawTable(), &st::squareTable() } },
                 { s.at (1.1), { &st::sawToSquareTable(), &st::squareTable() } },
                 { s.at (1.103), { &st::squareTable(), &st::squareTable() } }, // queued: arrives mid-crossfade
                 { s.at (1.25), { &st::squareTable(), nullptr } } };           // nullptr = the built-in sine
    s.releaseAllAt = { s.at (1.45) };

    // Poly: a chord, more notes than voices (steals), bends, offs
    for (const auto note : { 48, 55, 60, 64 })
        s.on (0.05, note, 90 + note % 30);

    s.bend (0.06, 1500);
    s.bend (0.20, -3000);

    for (int i = 0; i < 16; ++i)
        s.on (0.3 + 0.02 * i, 50 + 2 * i, 40 + 5 * i);

    s.off (0.62, 48);
    s.off (0.7, 55);

    for (int i = 0; i < 16; i += 2)
        s.off (0.8 + 0.01 * i, 50 + 2 * i);

    s.on (1.0, 72, 127);
    s.on (1.0, 76, 60); // two notes at one sample
    s.bend (1.05, 0);

    // Legato line in the pipeline orders (on + off at one sample), corrections with the flag and by time
    s.on (1.55, 60);
    s.on (1.70, 62);
    s.off (1.70, 60);
    s.off (1.80, 62);
    s.on (1.80, 64, 100, EventTimingFlags::correction);
    s.on (1.82, 66);
    s.off (1.82, 64);

    for (int i = 0; i < 40; ++i)
        s.bend (1.83 + 0.005 * i, static_cast<int> (4000.0 * std::sin (0.3 * i)));

    s.off (2.1, 66);

    // During synthOn off: ignored; after it: Mono notes
    s.on (2.25, 70);
    s.off (2.3, 70);
    s.on (2.45, 57);
    s.on (2.6, 59);
    s.off (2.6, 57);
    s.off (3.0, 59);
    s.sortEvents();

    if (voiceMode)
        addFrames (s, 0.0, 3.1);

    return s;
}

void requireEqual (const st::Stereo& a, const st::Stereo& b)
{
    REQUIRE (a.size() == b.size());
    std::size_t firstDifference = a.size();

    for (std::size_t i = 0; i < a.size(); ++i)
        if (a.left[i] != b.left[i] || a.right[i] != b.right[i])
        {
            firstDifference = i;
            break;
        }

    CAPTURE (firstDifference);
    CHECK (firstDifference == a.size());
}

double rmsOf (const std::vector<float>& x, double sr, double from, double to)
{
    const auto a = static_cast<std::size_t> (from * sr), b = std::min (x.size(), static_cast<std::size_t> (to * sr));
    return st::rms (std::span<const float> (x.data() + a, b - a));
}

/** Frequency from the positive zero crossings in [from, to) seconds (linear interpolation). */
double zeroCrossingHz (const std::vector<float>& x, double sr, double from, double to)
{
    double first = -1.0, last = -1.0;
    int count = 0;
    const auto a = static_cast<std::size_t> (from * sr) + 1, b = std::min (x.size(), static_cast<std::size_t> (to * sr));

    for (std::size_t i = a; i < b; ++i)
        if (x[i - 1] < 0.0f && x[i] >= 0.0f)
        {
            const double t = static_cast<double> (i - 1) + static_cast<double> (x[i - 1] / (x[i - 1] - x[i]));

            if (first < 0.0)
                first = t;
            else
                ++count;

            last = t;
        }

    return count > 0 ? sr * count / (last - first) : 0.0;
}

//==============================================================================
// The VoicePipeline in front of the engine (Voice mode, like the processor)

sig::Signal sungPhrase (double sr)
{
    auto a = sig::toneSequence (sr, { { 0.0, 0.1 }, { sig::midiToHz (57), 0.3 }, { sig::midiToHz (60), 0.25 },
                                       { sig::midiToHz (64), 0.25 }, { 0.0, 0.2 } },
                                 0.3f, sig::sawShape);
    const auto vib = sig::vibrato (sr, sig::midiToHz (62), 50.0, 5.5, 0.5, 0.25f);
    const auto breath = sig::whiteNoise (sr, 0.2, 0.05f, 11);
    const auto rest = sig::silence (sr, 1.2);
    return sig::concat ({ a, vib, breath, rest });
}

st::Stereo renderWithPipeline (const sig::Signal& input, double sr, const SynthSettings& settings, int block)
{
    VoicePipeline pipeline;
    pipeline.prepare (sr, 4096);
    SynthEngine engine;
    engine.prepare (sr, SynthEngine::maxChunkSamples);
    VsEventBuffer<1024> events;
    VsEventTimingBuffer<1024> timings;
    ExpressionBuffer frames;
    VoiceSettings voice;
    voice.bendMode = BendMode::trueBend;
    voice.bendRange = BendRange::semitones12;
    auto s = settings;
    s.bendRangeSemitones = 12;
    st::Stereo out (input.size());
    std::size_t pos = 0, index = 0;

    while (pos < input.size())
    {
        const auto n = static_cast<int> (std::min<std::size_t> (static_cast<std::size_t> (blockAt (block, index++)), input.size() - pos));
        events.clear();
        timings.clear();
        frames.clear();
        engine.setWavetable (0, &st::sawTable());
        engine.setWavetable (1, &st::squareTable());
        pipeline.process (input.data() + pos, n, voice, events, timings, frames);
        engine.process (events.view(), timings.view(), &frames, s, {}, out.left.data() + pos, out.right.data() + pos, n);
        pos += static_cast<std::size_t> (n);
    }

    return out;
}
} // namespace

//==============================================================================
TEST_CASE ("SynthEngine: bit-identical for every host block split (notes, bends, expression, glide, LFOs, FX, tails, "
           "synthOn off / on, table changes)", "[synth][engine]")
{
    const auto sr = GENERATE (44100.0, 48000.0, 96000.0);
    const auto voiceMode = GENERATE (false, true);
    CAPTURE (sr, voiceMode);
    const auto scene = invarianceScene (sr, voiceMode);
    const auto reference = render (scene, 4096);

    // The scene really exercises the engine: sound in every phase, silence while off, exact 0 after the tails
    CHECK (rmsOf (reference.left, sr, 0.1, 0.6) > 1e-3);
    CHECK (rmsOf (reference.left, sr, 1.6, 2.1) > 1e-3);
    CHECK (rmsOf (reference.left, sr, 2.5, 3.0) > 1e-3);
    CHECK (rmsOf (reference.left, sr, 2.3, 2.34) == 0.0); // synthOn off (fade over), the note at 2.25 ignored
    CHECK (rmsOf (reference.left, sr, 6.5, 7.0) == 0.0);  // every voice and FX tail ended

    for (const auto block : hostBlocks)
    {
        CAPTURE (block);
        requireEqual (render (scene, block), reference);
    }

    CAPTURE (0);
    requireEqual (render (scene, 0), reference);
}

TEST_CASE ("SynthEngine: bit-identical with real VoicePipeline event streams (Voice mode, FX on)", "[synth][engine]")
{
    const auto sr = GENERATE (44100.0, 48000.0, 96000.0);
    const auto mode = GENERATE (PlayMode::legato, PlayMode::poly);
    CAPTURE (sr, static_cast<int> (mode));
    auto settings = busySettings();
    settings.playMode = mode;
    settings.mod[3] = { ModSource::voiceLevel, ModDest::filterCutoff, 50.0f };
    settings.mod[4] = { ModSource::voiceNoisiness, ModDest::noiseLevel, 40.0f };
    const auto input = sungPhrase (sr);
    const auto reference = renderWithPipeline (input, sr, settings, 512);
    CHECK (rmsOf (reference.left, sr, 0.2, 1.0) > 1e-3);

    for (const auto block : { 1, 7, 64, 480, 4096, 0 })
    {
        CAPTURE (block);
        requireEqual (renderWithPipeline (input, sr, settings, block), reference);
    }
}

TEST_CASE ("SynthEngine: no allocation on the audio path", "[synth][engine]")
{
#if defined(VOCALSYNTH_COUNT_MALLOC)
    const double sr = 48000.0;
    auto scene = invarianceScene (sr, true);
    scene.settings[0].second.osc[0].unison = 8;
    scene.settings[0].second.osc[1].unison = 8;
    SynthEngine engine;
    engine.prepare (sr, SynthEngine::maxChunkSamples);
    VoicePipeline pipeline;
    pipeline.prepare (sr, 4096);
    const auto input = sungPhrase (sr);

    // Voice mode like the processor: pipeline with expression -> engine, a table change, releaseAll, reset
    VsEventBuffer<1024> events;
    VsEventTimingBuffer<1024> timings;
    ExpressionBuffer frames;
    std::vector<float> left (4096), right (4096);

    for (const auto block : { 1, 128, 4096 })
    {
        CAPTURE (block);
        std::size_t allocations = 0;
        int pos = 0;
        const auto settings = scene.settings[0].second;
        engine.reset();
        pipeline.reset();

        {
            CountMallocs counter;

            while (pos < static_cast<int> (input.size()))
            {
                const int n = std::min (block, static_cast<int> (input.size()) - pos);
                events.clear();
                timings.clear();
                frames.clear();
                engine.setWavetable (0, pos < 30000 ? &st::sawTable() : &st::sawToSquareTable());
                engine.setWavetable (1, &st::squareTable());
                pipeline.process (input.data() + pos, n, VoiceSettings {}, events, timings, frames);
                engine.process (events.view(), timings.view(), &frames, settings, {}, left.data(), right.data(), n);
                (void) engine.getTablesInUse (0);
                (void) engine.getTablesInUse (1);
                (void) engine.getStats();
                (void) engine.getCurrentExpression();

                if (pos > 40000 && pos < 40000 + block + 1)
                    engine.releaseAll();

                pos += n;
            }

            engine.reset();
            allocations = counter.count();
        }

        CHECK (allocations == 0);
    }

    // The counter itself works (otherwise the checks above would pass vacuously)
    {
        std::vector<float>* volatile probe = nullptr; // volatile: the allocation cannot be elided
        std::size_t counted = 0;

        {
            CountMallocs counter;
            probe = new std::vector<float> (100);
            counted = counter.count();
        }

        delete probe;
        CHECK (counted >= 2);
    }
#else
    SKIP ("allocation counting needs glibc's malloc (not available on this platform / under sanitizers)");
#endif
}

TEST_CASE ("SynthEngine: silence gives exact 0 and no active voice once every tail ended", "[synth][engine]")
{
    const double sr = 48000.0;
    Scene s;
    s.sr = sr;
    s.length = s.at (8.0);
    auto settings = busySettings();
    settings.reverb.sizePercent = 40.0f; // RT60 1.2 s: -120 dB (tail end) 2.4 s after the delay's tail
    s.settings = { { 0, settings } };
    s.tables = { { 0, { &st::sawTable(), &st::squareTable() } } };
    s.on (0.0, 60);
    s.on (0.0, 67);
    s.off (0.5, 60);
    s.off (0.5, 67);
    SynthEngine engine;
    engine.prepare (sr, SynthEngine::maxChunkSamples);
    const auto out = render (engine, s, 512);
    CHECK (rmsOf (out.left, sr, 0.0, 0.5) > 1e-3);
    CHECK (rmsOf (out.left, sr, 0.8, 1.2) > 1e-6); // reverb / delay tail

    const auto lastNonZero = [&]
    {
        std::size_t last = 0;

        for (std::size_t i = 0; i < out.size(); ++i)
            if (out.left[i] != 0.0f || out.right[i] != 0.0f)
                last = i;

        return static_cast<double> (last) / sr;
    }();

    CAPTURE (lastNonZero);
    CHECK (lastNonZero < 7.0);
    CHECK (engine.getStats().activeVoices == 0);
    CHECK (engine.getStats().nanResets == 0);
}

TEST_CASE ("SynthEngine: pitch bend with bendRange 2 / 12 / 48, smoothed while sounding, jumps from silence",
           "[synth][engine]")
{
    const double sr = 48000.0;
    const auto range = GENERATE (2, 12, 48);
    CAPTURE (range);
    auto settings = sineSettings();
    settings.bendRangeSemitones = range;

    SECTION ("a held note follows the bend: bend / 8192 * range")
    {
        Scene s;
        s.sr = sr;
        s.length = s.at (2.5);
        s.settings = { { 0, settings } };
        s.on (0.0, 57); // 220 Hz
        s.bend (0.2, 4096);
        const auto out = render (s, 512);
        const auto expected = 220.0 * std::exp2 (range / 2.0 / 12.0);
        const auto measured = st::measureFrequency (std::span<const float> (out.left.data() + s.at (0.5), 65536 + 1024), sr);
        CHECK (std::abs (st::cents (measured, expected)) < 1.0);

        // 5 ms one-pole: half way after ~3.5 ms, 99 % after ~25 ms (pitch measured by zero crossings)
        const auto early = zeroCrossingHz (out.left, sr, 0.2, 0.2 + 0.004);
        CHECK (early < expected);
    }

    SECTION ("a note from silence starts on the bend (no 5 ms slide from the old value)")
    {
        Scene s;
        s.sr = sr;
        s.length = s.at (1.0);
        s.settings = { { 0, settings } };
        s.on (0.0, 57);
        s.off (0.1, 57);
        s.bend (0.5, -8192);
        s.on (0.5, 69); // 440 Hz bent down by the full range
        const auto out = render (s, 64);
        const auto expected = 440.0 * std::exp2 (-range / 12.0);
        const auto measured = zeroCrossingHz (out.left, sr, 0.5005, 0.5005 + std::max (0.004, 3.0 / expected));
        CHECK (std::abs (st::cents (measured, expected)) < 5.0);
    }
}

TEST_CASE ("SynthEngine: expression frames ramp linearly over the distance to the previous frame", "[synth][engine]")
{
    const double sr = 48000.0;
    SynthEngine engine;
    engine.prepare (sr, SynthEngine::maxChunkSamples);
    const auto settings = sineSettings();
    std::vector<float> left (16), right (16);
    const double slope = 1.0 / 48000.0; // level rises by 1 per second
    std::vector<float> values;

    // Frames every 96 samples at offset 40 of their hop (any phase relative to the tick grid), level = t
    for (int tick = 0; tick < 300; ++tick)
    {
        ExpressionBuffer frames;
        const int start = tick * 16;

        for (int p = start; p < start + 16; ++p)
            if (p % 96 == 40)
                frames.push ({ p - start, static_cast<float> (p * slope), 0.25f, 0.5f, 10.0f });

        engine.process ({}, {}, &frames, settings, {}, left.data(), right.data(), 16);
        values.push_back (engine.getCurrentExpression().level);
    }

    // From the third frame on the value climbs exactly 16 samples' worth per tick (no steps, no plateaus)
    for (std::size_t t = 30; t < values.size(); ++t)
    {
        CAPTURE (t);
        CHECK_THAT (values[t] - values[t - 1], WithinAbs (16.0 * slope, 1e-6));
    }

    CHECK_THAT (engine.getCurrentExpression().brightness, WithinAbs (0.25, 1e-6));
    CHECK_THAT (engine.getCurrentExpression().pitchCents, WithinAbs (10.0, 1e-5));

    // Voice mode before the first frame after reset(): all 0
    engine.reset();
    ExpressionBuffer none;
    engine.process ({}, {}, &none, settings, {}, left.data(), right.data(), 16);
    CHECK (engine.getCurrentExpression() == ExpressionFrame {});
}

TEST_CASE ("SynthEngine: rest expression (Test Pattern) - level 1 while a note is held, smoothed 5 / 50 ms",
           "[synth][engine]")
{
    const double sr = 48000.0;
    SynthEngine engine;
    engine.prepare (sr, SynthEngine::maxChunkSamples);
    const auto settings = sineSettings();
    std::vector<float> left (16), right (16);
    const auto tickSeconds = 16.0 / sr;
    std::vector<float> level;

    for (int tick = 0; tick < 4000; ++tick)
    {
        std::vector<VsEvent> events;

        if (tick == 10)
            events.push_back (VsEvent::makeNoteOn (0, 1, 60, 100));

        if (tick == 400)
            events.push_back (VsEvent::makeNoteOff (0, 1, 60));

        engine.process (events, {}, nullptr, settings, {}, left.data(), right.data(), 16);
        level.push_back (engine.getCurrentExpression().level);
    }

    CHECK (level[9] == 0.0f);
    const auto attack = [&] (int ticks) { return 1.0 - std::exp (-ticks * tickSeconds / 0.005); };
    CHECK_THAT (level[10], WithinAbs (attack (1), 1e-4));
    CHECK_THAT (level[24], WithinAbs (attack (15), 1e-4)); // ~5 ms: 0.63
    CHECK (level[399] == 1.0f);
    const auto release = [&] (int ticks) { return std::exp (-ticks * tickSeconds / 0.050); };
    CHECK_THAT (level[400 + 149], WithinAbs (release (150), 1e-3)); // 50 ms: 0.37
    CHECK (level.back() == 0.0f);
}

TEST_CASE ("SynthEngine: a table change during a note crossfades linearly over 5 ms (no click)", "[synth][engine]")
{
    const auto sr = GENERATE (44100.0, 48000.0, 96000.0);
    CAPTURE (sr);
    auto settings = basicSettings();
    settings.osc[0].levelPercent = 70.0f;
    const auto make = [&] (const Wavetable* from, const Wavetable* to)
    {
        Scene s;
        s.sr = sr;
        s.length = s.at (0.4);
        s.settings = { { 0, settings } };
        s.tables = { { 0, { from, nullptr } }, { s.at (0.2), { to, nullptr } } };
        s.on (0.0, 57);
        return render (s, 128);
    };

    const auto saw = make (&st::sawTable(), &st::sawTable());
    const auto square = make (&st::squareTable(), &st::squareTable());
    const auto change = make (&st::sawTable(), &st::squareTable());
    const auto start = static_cast<std::size_t> (std::ceil (0.2 * sr / 16.0) * 16.0); // the next tick
    const auto fade = static_cast<std::size_t> (std::ceil (0.005 * sr / 16.0) * 16.0);
    double worst = 0.0, peak = 0.0;

    for (std::size_t i = start - 2000; i < start + fade + 2000; ++i)
    {
        const auto w = std::clamp ((static_cast<double> (i) - static_cast<double> (start)) / static_cast<double> (fade), 0.0, 1.0);
        const auto expected = static_cast<double> (saw.left[i]) * (1.0 - w) + static_cast<double> (square.left[i]) * w;
        worst = std::max (worst, std::abs (static_cast<double> (change.left[i]) - expected));
        peak = std::max (peak, std::abs (expected));
    }

    CAPTURE (worst, peak);
    CHECK (worst < 0.01 * peak); // the ideal 5 ms linear crossfade within 1 sample of timing
}

TEST_CASE ("SynthEngine: table pointers - reset() forgets them, getTablesInUse covers silent voices, never the sine",
           "[synth][engine]")
{
    const double sr = 48000.0;
    SynthEngine engine;
    engine.prepare (sr, SynthEngine::maxChunkSamples);
    CHECK (engine.getTablesInUse (0).current == nullptr);
    CHECK (engine.getTablesInUse (1).current == nullptr);

    Scene s;
    s.sr = sr;
    s.length = s.at (0.5);
    auto settings = basicSettings();
    settings.osc[1].on = true;
    settings.env[0].releaseMs = 10.0f;
    s.settings = { { 0, settings } };
    s.tables = { { 0, { &st::sawTable(), nullptr } } };
    s.on (0.0, 60);
    s.off (0.1, 60);
    render (engine, s, 256);

    // Every voice is silent again: the store must still keep the table every voice holds (it would be read by
    // the next note before the next block's setWavetable), the built-in sine of Osc B is never reported
    CHECK (engine.getStats().activeVoices == 0);
    CHECK (engine.getTablesInUse (0).current == &st::sawTable());
    CHECK (engine.getTablesInUse (0).previous == nullptr);
    CHECK (engine.getTablesInUse (1).current == nullptr);

    engine.reset();
    CHECK (engine.getTablesInUse (0).current == nullptr);
    CHECK (engine.getTablesInUse (0).previous == nullptr);

    engine.prepare (sr, 512);
    CHECK (engine.getTablesInUse (0).current == nullptr);
}

TEST_CASE ("SynthEngine: table retire protocol with real frees - no pointer the engine reads is ever released",
           "[synth][engine]")
{
    // A fake WavetableStore (docs/SYNTH-DESIGN.md 12.1) on one thread: between blocks the "worker" publishes
    // new tables (often several within a 5 ms crossfade, while notes start and end), the "audio" blocks
    // acquire / process / release, and garbage collection frees a retired table once the block counter passed
    // retireBlock + 2 and no inUse entry holds it. The tables are > 128 KB and glibc is told to map every
    // such block separately, so a read of a freed table touches unmapped memory and crashes the test.
#if defined(__GLIBC__)
    mallopt (M_MMAP_THRESHOLD, 128 * 1024);
#endif
    const double sr = 48000.0;
    const auto seed = GENERATE (1234u, 99u, 7u, 2026u, 31337u);
    CAPTURE (seed);
    std::mt19937 rng (seed);
    const auto saw = st::cycleFromSeries (st::sawSeries (300));
    const auto square = st::cycleFromSeries (st::squareSeries (300));

    struct Retired
    {
        std::shared_ptr<const Wavetable> table;
        uint64_t retireBlock = 0;
    };

    std::array<std::shared_ptr<const Wavetable>, 2> slots { st::buildTable ({ saw, square }), st::buildTable ({ square, saw }) };
    std::vector<Retired> retired;
    std::array<const Wavetable*, 4> inUse {};
    std::set<const Wavetable*> live { slots[0].get(), slots[1].get() };
    uint64_t blockCounter = 0;
    int freed = 0, published = 0, violations = 0;

    SynthEngine engine;
    engine.prepare (sr, SynthEngine::maxChunkSamples);
    auto settings = basicSettings();
    settings.playMode = PlayMode::poly;
    settings.osc[1].on = true;
    settings.env[0] = { 1.0f, 50.0f, 60.0f, 30.0f };
    std::vector<float> left (512), right (512);
    std::vector<VsEvent> events;
    std::array<bool, 128> held {};

    for (int block = 0; block < 3000; ++block)
    {
        // Worker: publish (sometimes twice in a row, i.e. within one crossfade)
        if (rng() % 3 == 0)
        {
            const auto slot = rng() % 2;
            auto table = st::buildTable ({ rng() % 2 == 0 ? saw : square, rng() % 2 == 0 ? square : saw });
            live.insert (table.get());
            retired.push_back ({ slots[slot], blockCounter });
            slots[slot] = std::move (table);
            ++published;
        }

        // Audio block
        const int n = 16 + static_cast<int> (rng() % 400);
        engine.setWavetable (0, slots[0].get());
        engine.setWavetable (1, slots[1].get());
        events.clear();

        for (int k = 0; k < 2; ++k)
        {
            const auto note = static_cast<uint8_t> (48 + rng() % 24);
            const int offset = static_cast<int> (rng() % static_cast<unsigned> (n));

            if (held[note])
                events.push_back (VsEvent::makeNoteOff (offset, 1, note));
            else
                events.push_back (VsEvent::makeNoteOn (offset, 1, note, 100));

            held[note] = ! held[note];
        }

        std::sort (events.begin(), events.end(), [] (const VsEvent& a, const VsEvent& b) { return a.sampleOffset < b.sampleOffset; });
        engine.process (events, {}, nullptr, settings, {}, left.data(), right.data(), n);

        for (int i = 0; i < n; ++i)
            REQUIRE (std::isfinite (left[static_cast<std::size_t> (i)]));

        const auto a = engine.getTablesInUse (0), b = engine.getTablesInUse (1);
        inUse = { a.current, a.previous, b.current, b.previous };
        ++blockCounter;

        for (const auto* t : inUse)
            violations += t != nullptr && live.count (t) == 0 ? 1 : 0;

        // Garbage collection (worker / message thread)
        for (auto it = retired.begin(); it != retired.end();)
        {
            const bool used = std::find (inUse.begin(), inUse.end(), it->table.get()) != inUse.end();

            if (blockCounter >= it->retireBlock + 2 && ! used)
            {
                if (it->table.use_count() == 1 && it->table != slots[0] && it->table != slots[1])
                {
                    live.erase (it->table.get());
                    ++freed;
                }

                it = retired.erase (it);
            }
            else
            {
                ++it;
            }
        }
    }

    CHECK (violations == 0);
    CHECK (published > 500);
    CHECK (freed > 400);
}

TEST_CASE ("SynthEngine: synthOn off fades over 10 ms, then exact 0; on again starts from silence", "[synth][engine]")
{
    const double sr = 48000.0;
    Scene s;
    s.sr = sr;
    s.length = s.at (1.5);
    auto on = basicSettings();
    on.chorus.on = true;
    on.reverb.on = true;
    auto off = on;
    off.synthOn = false;
    s.settings = { { 0, on }, { s.at (0.5), off }, { s.at (0.8), on } };
    s.on (0.0, 60); // held through the off phase: does not sound again
    s.on (1.0, 64);
    SynthEngine engine;
    engine.prepare (sr, SynthEngine::maxChunkSamples);
    const auto out = render (engine, s, 64);

    CHECK (rmsOf (out.left, sr, 0.4, 0.5) > 1e-3);
    CHECK (rmsOf (out.left, sr, 0.5, 0.505) > 1e-4);        // fading
    CHECK (rmsOf (out.left, sr, 0.5 + 0.011, 1.0) == 0.0);   // off, then on without a note-on
    CHECK (rmsOf (out.left, sr, 1.05, 1.5) > 1e-3);          // the next note-on sounds
}

TEST_CASE ("SynthEngine: a play-mode change and releaseAll() release every voice", "[synth][engine]")
{
    const double sr = 48000.0;
    auto poly = basicSettings();
    poly.playMode = PlayMode::poly;
    poly.env[0].releaseMs = 20.0f;
    auto legato = poly;
    legato.playMode = PlayMode::legato;

    SECTION ("play mode")
    {
        Scene s;
        s.sr = sr;
        s.length = s.at (0.6);
        s.settings = { { 0, poly }, { s.at (0.3), legato } };

        for (const auto note : { 60, 64, 67 })
            s.on (0.0, note);

        SynthEngine engine;
        engine.prepare (sr, SynthEngine::maxChunkSamples);
        const auto out = render (engine, s, 128);
        CHECK (rmsOf (out.left, sr, 0.2, 0.3) > 1e-3);
        CHECK (rmsOf (out.left, sr, 0.45, 0.6) == 0.0);
        CHECK (engine.getStats().activeVoices == 0);
    }

    SECTION ("releaseAll (flush)")
    {
        Scene s;
        s.sr = sr;
        s.length = s.at (0.6);
        s.settings = { { 0, poly } };
        s.releaseAllAt = { s.at (0.3) };

        for (const auto note : { 60, 64, 67 })
            s.on (0.0, note);

        SynthEngine engine;
        engine.prepare (sr, SynthEngine::maxChunkSamples);
        const auto out = render (engine, s, 128);
        CHECK (rmsOf (out.left, sr, 0.2, 0.3) > 1e-3);
        CHECK (rmsOf (out.left, sr, 0.45, 0.6) == 0.0);
        CHECK (engine.getStats().activeVoices == 0);
    }
}

TEST_CASE ("SynthEngine: releaseAll() centres the pitch bend (flush / source switch, review 0.4 R1-1)", "[synth][engine]")
{
    // The processor's flush sends note-offs AND a centre bend to the MIDI port; the restarted sources do not
    // send a bend again (Bend Mode Off). The next note must play its own pitch, not the old bend.
    const double sr = 48000.0;
    const auto range = GENERATE (2, 12);
    CAPTURE (range);
    auto settings = sineSettings();
    settings.bendRangeSemitones = range;

    Scene s;
    s.sr = sr;
    s.length = s.at (1.0);
    s.settings = { { 0, settings } };
    s.bend (0.0, 8191);
    s.on (0.0, 69);
    s.releaseAllAt = { s.at (0.2) };
    s.on (0.5, 69); // no bend event: the source starts from the centre
    s.sortEvents();
    const auto out = render (s, 480);

    CHECK (std::abs (st::cents (zeroCrossingHz (out.left, sr, 0.1, 0.2), 440.0 * std::exp2 (range * 8191.0 / 8192.0 / 12.0))) < 2.0);
    CHECK (std::abs (st::cents (zeroCrossingHz (out.left, sr, 0.6, 0.95), 440.0)) < 1.0);
}

TEST_CASE ("SynthEngine: synthOn off -> on inside the 10 ms fade leaves no hung note (review 0.4 R1-2)", "[synth][engine]")
{
    const double sr = 48000.0;
    const auto block = GENERATE (1, 64, 480);
    CAPTURE (block);
    auto on = sineSettings();
    on.env[0].releaseMs = 50.0f;
    auto off = on;
    off.synthOn = false;

    Scene s;
    s.sr = sr;
    s.length = s.at (2.0);
    s.settings = { { 0, on }, { s.at (0.1), off }, { s.at (0.106), on } }; // a 6 ms off pulse (automation)
    s.on (0.0, 60);
    s.off (0.103, 60); // the singer stops while the synth fades out
    s.on (1.0, 64);
    s.off (1.2, 64);   // Legato: must not fall back to the "held" 60
    s.sortEvents();
    SynthEngine engine;
    engine.prepare (sr, SynthEngine::maxChunkSamples);
    const auto out = render (engine, s, block);

    CHECK (rmsOf (out.left, sr, 0.3, 1.0) == 0.0);
    CHECK (rmsOf (out.left, sr, 1.05, 1.2) > 0.1);
    CHECK (rmsOf (out.left, sr, 1.5, 2.0) == 0.0);
    CHECK (engine.getStats().activeVoices == 0);
}

TEST_CASE ("SynthEngine: Poly - a note released and restarted inside its steal fade sounds (review 0.4 R1-3)", "[synth][engine]")
{
    const double sr = 48000.0;
    auto settings = sineSettings();
    settings.playMode = PlayMode::poly;

    Scene s;
    s.sr = sr;
    s.length = s.at (1.2);
    s.settings = { { 0, settings } };

    for (int i = 0; i < SynthEngine::maxVoices; ++i)
        s.on (0.0, 40 + i); // every voice busy

    // Note 80 steals a voice, ends after 0.5 ms and starts again after 1 ms: both inside the 2 ms fade
    s.events.push_back ({ s.at (0.1), VsEvent::makeNoteOn (0, 1, 80, 100), 0 });
    s.events.push_back ({ s.at (0.1) + 24, VsEvent::makeNoteOff (0, 1, 80), 0 });
    s.events.push_back ({ s.at (0.1) + 48, VsEvent::makeNoteOn (0, 1, 80, 100), 0 });

    for (int i = 0; i < SynthEngine::maxVoices; ++i)
        s.off (0.2, 40 + i);

    s.sortEvents();
    SynthEngine engine;
    engine.prepare (sr, SynthEngine::maxChunkSamples);
    const auto out = render (engine, s, 128);

    // Only note 80 is held after the chord's release tails (5 ms)
    CHECK (engine.getStats().activeVoices == 1);
    CHECK (rmsOf (out.left, sr, 0.4, 1.2) > 0.1);
    CHECK (std::abs (st::cents (zeroCrossingHz (out.left, sr, 0.4, 1.2), 830.61)) < 2.0);
}

TEST_CASE ("SynthEngine: the mod sources reserved for 0.5 / 0.6 contribute 0 (review 0.4 R2-1)", "[synth][engine]")
{
    CHECK (numModSources == 17);
    CHECK_FALSE (isReservedModSource (ModSource::voicePitch));

    for (const auto reserved : { ModSource::vowelX, ModSource::vowelY, ModSource::vowelWeight1, ModSource::vowelWeight2,
                                 ModSource::vowelWeight3 })
    {
        CAPTURE (static_cast<int> (reserved));
        CHECK (isReservedModSource (reserved));
        auto settings = busySettings();
        auto withReserved = settings;
        withReserved.mod[1] = { reserved, ModDest::filterCutoff, 100.0f };
        withReserved.mod[2] = { reserved, ModDest::pitch, -100.0f };
        settings.mod[1] = {};
        settings.mod[2] = {};

        const auto scene = [] (const SynthSettings& s)
        {
            Scene sc;
            sc.length = sc.at (0.4);
            sc.settings = { { 0, s } };
            sc.on (0.0, 60);
            sc.on (0.1, 67);
            sc.off (0.3, 60);
            sc.off (0.3, 67);
            sc.sortEvents();
            return sc;
        };

        requireEqual (render (scene (withReserved), 512), render (scene (settings), 512));
    }
}

TEST_CASE ("SynthEngine: LFO retrigger on note starts (square LFO -> Amp Level)", "[synth][engine]")
{
    // LFO 1 square at 1 Hz, retrigger on, -100 % to Amp Level: +1 in the first half cycle -> gain 0, then 2
    const double sr = 48000.0;
    auto settings = sineSettings();
    settings.lfo[0] = { LfoShape::square, 1.0f, TempoDivision::off, true, 0.0f };
    settings.mod[0] = { ModSource::lfo1, ModDest::ampLevel, -100.0f };
    Scene s;
    s.sr = sr;
    s.length = s.at (2.6);
    s.settings = { { 0, settings } };
    s.on (0.3, 60);
    s.off (1.2, 60);
    s.on (1.53, 62); // a different LFO phase if it ran freely
    const auto out = render (s, 256);

    for (const double start : { 0.3, 1.53 })
    {
        CAPTURE (start);
        CHECK (rmsOf (out.left, sr, start + 0.02, start + 0.48) < 1e-4);
        CHECK (rmsOf (out.left, sr, start + 0.52, start + 0.9) > 0.1);
    }
}

TEST_CASE ("SynthEngine: the correction flag turns a late legato change into a jump without glide", "[synth][engine]")
{
    const double sr = 48000.0;
    auto settings = sineSettings();
    settings.glideTimeMs = 300.0f;
    settings.glideMode = GlideMode::always;
    const auto run = [&] (uint8_t flags)
    {
        Scene s;
        s.sr = sr;
        s.length = s.at (0.6);
        s.settings = { { 0, settings } };
        s.on (0.0, 57);
        s.on (0.2, 64, 100, flags); // > 40 ms after the note-on: only the flag makes it a correction
        s.off (0.2, 57, flags);
        return render (s, 64);
    };

    const auto corrected = run (EventTimingFlags::correction);
    const auto glided = run (0);
    const auto target = 440.0 * std::exp2 ((64 - 69) / 12.0);
    CHECK (std::abs (st::cents (zeroCrossingHz (corrected.left, sr, 0.205, 0.24), target)) < 5.0);
    CHECK (st::cents (zeroCrossingHz (glided.left, sr, 0.205, 0.24), target) < -300.0);
}

TEST_CASE ("SynthEngine: global destinations use the newest voice's sources (Velocity -> Delay Mix)", "[synth][engine]")
{
    const double sr = 48000.0;
    auto settings = sineSettings();
    settings.delay = { true, 300.0f, TempoDivision::off, 0.0f, false, 0.0f };
    const auto run = [&] (float amount)
    {
        auto s2 = settings;
        s2.mod[0] = { ModSource::velocity, ModDest::delayMix, amount };
        Scene s;
        s.sr = sr;
        s.length = s.at (0.5);
        s.settings = { { 0, s2 } };
        s.on (0.0, 60, 127); // velocity 1 -> mix 1 -> no dry signal
        return render (s, 128);
    };

    CHECK (rmsOf (run (100.0f).left, sr, 0.01, 0.29) == 0.0);
    CHECK (rmsOf (run (0.0f).left, sr, 0.01, 0.29) > 0.1);
    CHECK (rmsOf (run (100.0f).left, sr, 0.31, 0.5) > 0.1); // the echo
}

TEST_CASE ("SynthEngine: non-finite settings, frames and odd events stay finite", "[synth][engine]")
{
    const double sr = 48000.0;
    SynthEngine engine;
    engine.prepare (sr, SynthEngine::maxChunkSamples);
    auto settings = busySettings();
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    settings.filterCutoffHz = nan;
    settings.osc[0].positionPercent = std::numeric_limits<float>::infinity();
    settings.delay.timeMs = nan;
    settings.reverb.sizePercent = -nan;
    settings.synthVolumeDb = nan;
    settings.mod[0] = { ModSource::voiceLevel, ModDest::filterCutoff, nan };
    std::vector<float> left (512), right (512);

    for (int block = 0; block < 400; ++block)
    {
        ExpressionBuffer frames;
        frames.push ({ 10, nan, nan, std::numeric_limits<float>::infinity(), nan });
        frames.push ({ -5, 0.5f, 0.5f, 0.5f, 1.0e30f }); // offset out of range
        std::vector<VsEvent> events;

        if (block % 10 == 0)
        {
            events.push_back (VsEvent::makeNoteOn (900, 1, 60, 100)); // offset beyond the block
            events.push_back (VsEvent::makeNoteOn (-3, 1, 64, 0));    // velocity 0 = note-off
            events.push_back (VsEvent::makePitchBend (5, 1, -8192));
            events.push_back (VsEvent::makeControlChange (6, 1, 1, 64)); // CCs are ignored
        }

        if (block % 10 == 5)
            events.push_back (VsEvent::makeNoteOff (100, 1, 60));

        engine.process (events, {}, &frames, settings, { std::numeric_limits<double>::quiet_NaN(), -1.0 }, left.data(),
                        right.data(), 512);

        for (std::size_t i = 0; i < 512; ++i)
        {
            REQUIRE (std::isfinite (left[i]));
            REQUIRE (std::isfinite (right[i]));
        }
    }

    const auto e = engine.getCurrentExpression();
    CHECK (std::isfinite (e.level));
    CHECK (std::abs (e.pitchCents) <= 100.0f); // frames are sanitised
}

TEST_CASE ("SynthVoice: an oscillator that ends inside a table crossfade forgets the faded-out table", "[synth][engine]")
{
    // Agent C's change in SynthVoice::deactivate (docs/SYNTH-DESIGN.md 12.1): the note ends 3 ticks into the
    // 5 ms crossfade; before, the idle voice reported the old table for ever (and a third pointer could appear).
    const double sr = 48000.0;
    const auto tableA = st::buildTable ({ st::cycleFromSeries (st::sawSeries (100)) });
    const auto tableB = st::buildTable ({ st::cycleFromSeries (st::squareSeries (100)) });
    SynthVoice voice;
    voice.prepare (sr, 7u);
    voice.setTables (tableA.get(), tableA.get());
    auto settings = basicSettings();
    settings.env[0] = { 0.0f, 0.0f, 100.0f, 0.0f };
    ModSourceValues global {};
    const VoiceContext context { &settings, &global, 0.0f };
    std::array<float, 16> l {}, r {};
    VoiceCommand start;
    start.note = 60;
    voice.apply (start, context, 16);

    for (int t = 0; t < 10; ++t)
    {
        voice.controlTick (context);
        voice.render (l.data(), r.data(), 16);
    }

    voice.setTables (tableB.get(), tableB.get());
    VoiceCommand release;
    release.type = VoiceCommandType::release;
    release.note = 60;

    for (int t = 0; t < 10 && voice.isActive(); ++t)
    {
        if (t == 3)
        {
            CHECK (voice.getTablesInUse (0).previous == tableA.get()); // crossfading
            voice.apply (release, context, 16);
        }

        voice.controlTick (context);
        voice.render (l.data(), r.data(), 16);
    }

    REQUIRE_FALSE (voice.isActive());
    CHECK (voice.getTablesInUse (0).current == tableB.get());
    CHECK (voice.getTablesInUse (0).previous == nullptr);
    CHECK (voice.getTablesInUse (1).previous == nullptr);

    // The next note reads the newest table, clean
    voice.apply (start, context, 16);
    voice.controlTick (context);
    CHECK (voice.getTablesInUse (0).current == tableB.get());
    CHECK (voice.getTablesInUse (0).previous == nullptr);

    // A table given in the very tick the voice ends (the engine forwards before the voice ticks) is the one
    // the idle voice keeps - not the oscillator's older current table (found by the retire-protocol test)
    const auto tableC = st::buildTable ({ st::cycleFromSeries (st::sawSeries (50)) });
    voice.render (l.data(), r.data(), 16);
    voice.apply (release, context, 16);

    for (int t = 0; t < 10 && voice.isActive(); ++t)
    {
        voice.setTables (tableC.get(), tableC.get());
        voice.controlTick (context);
        voice.render (l.data(), r.data(), 16);
    }

    REQUIRE_FALSE (voice.isActive());
    CHECK (voice.getTablesInUse (0).current == tableC.get());
    CHECK (voice.getTablesInUse (1).current == tableC.get());
    CHECK (voice.getTablesInUse (0).previous == nullptr);
}

//==============================================================================
// CPU (docs/SYNTH-DESIGN.md 18): 48 kHz, block 128, Release; 1 % = 26.7 us per block. Min of several runs.

namespace
{
struct Load
{
    double usPerBlock = 0.0;
    double percent = 0.0;
};

/** Plays `notes` (held), then measures `blocks` blocks of 128 samples (the minimum of 7 runs per block
    position is not meaningful for a moving signal, so: min over 7 repetitions of the whole run). */
Load measure (const SynthSettings& settings, std::initializer_list<int> notes, bool silenceAfterTails = false)
{
    const double sr = 48000.0;
    constexpr int block = 128;
    constexpr int blocks = 750; // 2 s
    std::vector<float> left (block), right (block);
    double best = 1.0e30;

    for (int repetition = 0; repetition < 7; ++repetition)
    {
        SynthEngine engine;
        engine.prepare (sr, SynthEngine::maxChunkSamples);
        engine.setWavetable (0, &st::sawToSquareTable());
        engine.setWavetable (1, &st::squareTable());
        std::vector<VsEvent> events;

        for (const auto n : notes)
            events.push_back (VsEvent::makeNoteOn (0, 1, static_cast<uint8_t> (n), 100));

        engine.process (events, {}, nullptr, settings, {}, left.data(), right.data(), block);

        if (silenceAfterTails)
        {
            events.clear();

            for (const auto n : notes)
                events.push_back (VsEvent::makeNoteOff (0, 1, static_cast<uint8_t> (n)));

            engine.process (events, {}, nullptr, settings, {}, left.data(), right.data(), block);

            for (int i = 0; i < 3000; ++i) // 8 s: every tail ended
                engine.process ({}, {}, nullptr, settings, {}, left.data(), right.data(), block);
        }
        else
        {
            for (int i = 0; i < 40; ++i) // attack phase over
                engine.process ({}, {}, nullptr, settings, {}, left.data(), right.data(), block);
        }

        const auto start = std::chrono::steady_clock::now();

        for (int i = 0; i < blocks; ++i)
            engine.process ({}, {}, nullptr, settings, {}, left.data(), right.data(), block);

        const std::chrono::duration<double, std::micro> us = std::chrono::steady_clock::now() - start;
        best = std::min (best, us.count() / blocks);
    }

    const double blockMicros = 1.0e6 * block / sr;
    return { best, 100.0 * best / blockMicros };
}

SynthSettings typicalSettings()
{
    auto s = basicSettings();
    s.playMode = PlayMode::legato;
    s.osc[0].unison = 4;
    s.osc[1] = s.osc[0];
    s.osc[1].on = true;
    s.osc[1].octave = -1;
    s.filterMode = FilterMode::lp24;
    s.filterCutoffHz = 3000.0f;
    s.filterResonancePercent = 30.0f;
    s.lfo[0].rateHz = 5.0f;
    s.mod[0] = { ModSource::lfo1, ModDest::oscAPosition, 20.0f };
    s.mod[1] = { ModSource::voiceLevel, ModDest::filterCutoff, 30.0f };
    s.chorus.on = s.delay.on = s.reverb.on = true;
    return s;
}

SynthSettings worstSettings()
{
    auto s = typicalSettings();
    s.playMode = PlayMode::poly;
    s.osc[0].unison = 8;
    s.osc[1].unison = 8;
    s.subLevelPercent = 40.0f;
    s.noiseLevelPercent = 10.0f;
    s.noiseColorPercent = 50.0f;
    s.filterDriveDb = 12.0f;
    return s;
}
} // namespace

TEST_CASE ("SynthEngine: CPU typical / worst / silence and the Poly unison-cap decision", "[synth][engine][!benchmark]")
{
    const auto typical = measure (typicalSettings(), { 60 });
    const auto worst = measure (worstSettings(), { 36, 39, 43, 46, 48, 51, 55, 58, 60, 63, 67, 70, 72, 75, 79, 82 });
    const auto silence = measure (worstSettings(), { 60, 64, 67 }, true);
    auto worstUncapped = worstSettings();
    worstUncapped.playMode = PlayMode::poly;

    WARN ("SynthEngine 48 kHz / 128: typical " << typical.usPerBlock << " us = " << typical.percent << " %, worst (Poly 16 x 2 x "
                                                << SynthEngine::polyMaxUnison << " unison) " << worst.usPerBlock << " us = "
                                                << worst.percent << " %, silence " << silence.usPerBlock << " us = "
                                                << silence.percent << " %");
    CHECK (typical.percent <= 3.0);
    CHECK (worst.percent <= 12.0);
    CHECK (silence.percent <= 0.5);
}
