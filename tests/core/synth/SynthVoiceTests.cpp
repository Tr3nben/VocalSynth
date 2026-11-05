// SynthVoice: control protocol (any split of a tick bit-identical), pitch, glide time, legato without restart,
// correction without glide, steal fade 2 ms, exact 0 after release, sources, osc on/off, robustness, benchmark.
// Test plan: docs/SYNTH-DESIGN.md 21. Owner: agent B (Voice).

#include "vocalsynth/synth/SynthVoice.h"

#include "SynthTestUtils.h"

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <span>
#include <string>
#include <tuple>
#include <vector>

using namespace vocalsynth::synth;
using Catch::Matchers::WithinAbs;
namespace st = vocalsynth::synth::test;

namespace
{

constexpr int tick = controlTickSamples;

VoiceCommand command (VoiceCommandType type, int note, int velocity = 127, bool glide = false, int glideFrom = -1)
{
    VoiceCommand c;
    c.type = type;
    c.note = static_cast<uint8_t> (note);
    c.velocity = static_cast<uint8_t> (velocity);
    c.glide = glide;
    c.glideFromNote = static_cast<int8_t> (glideFrom);
    return c;
}

struct TimedCommand
{
    int64_t position;
    VoiceCommand command;
};

/** A minimal engine around one voice: control ticks at absolute multiples of 16, commands at their sample
    (before the tick at a tick boundary), render between control positions and host block ends. */
struct Rig
{
    SynthVoice voice;
    SynthSettings settings;
    ModSourceValues global {};
    float bend = 0.0f;
    double rate = 48000.0;
    std::vector<TimedCommand> commands;
    /** Called at every tick before the voice's controlTick (global sources, bend, settings). */
    void (*perTick) (Rig&, int tickIndex) = nullptr;
    std::vector<int> activeAtTick;

    explicit Rig (double sampleRate = 48000.0, const Wavetable* table = &st::sineTable()) : rate (sampleRate)
    {
        voice.prepare (rate, 0x1234u);
        voice.setTables (table, table);
        settings.ampVelocityPercent = 0.0f;
        settings.osc[0].levelPercent = 100.0f;
    }

    st::Stereo run (int numSamples, std::span<const int> blocks = {})
    {
        st::Stereo out (static_cast<std::size_t> (numSamples));
        std::size_t next = 0;
        std::size_t blockIndex = 0;
        int pos = 0;
        VoiceContext context { &settings, &global, bend };

        while (pos < numSamples)
        {
            const int block = blocks.empty() ? numSamples : blocks[blockIndex++ % blocks.size()];
            const int end = std::min (numSamples, pos + block);

            while (pos < end)
            {
                if (pos % tick == 0)
                {
                    if (perTick != nullptr)
                        perTick (*this, pos / tick);

                    context = { &settings, &global, bend };
                }

                while (next < commands.size() && commands[next].position == pos)
                    voice.apply (commands[next++].command, context, tick - pos % tick);

                if (pos % tick == 0)
                {
                    voice.controlTick (context);
                    activeAtTick.push_back (voice.isActive() ? 1 : 0);
                }

                int stop = std::min (end, pos + (tick - pos % tick));

                if (next < commands.size())
                    stop = std::min (stop, static_cast<int> (commands[next].position));

                voice.render (out.left.data() + pos, out.right.data() + pos, stop - pos);
                pos = stop;
            }
        }

        return out;
    }
};

double rmsOf (const std::vector<float>& x, std::size_t from, std::size_t to)
{
    return st::rms (std::span<const float> (x.data() + from, to - from));
}

double peakOf (const std::vector<float>& x, std::size_t from, std::size_t to)
{
    return st::peakAbs (std::span<const float> (x.data() + from, to - from));
}

/** Frequency from the positive zero crossings of a sine in [from, to) (linear interpolation). */
double zeroCrossingFrequency (const std::vector<float>& x, std::size_t from, std::size_t to, double rate)
{
    double first = -1.0, last = -1.0;
    int count = 0;

    for (std::size_t i = from + 1; i < to; ++i)
    {
        if (x[i - 1] < 0.0f && x[i] >= 0.0f)
        {
            const double t = static_cast<double> (i - 1) + x[i - 1] / (x[i - 1] - x[i]);

            if (first < 0.0)
                first = t;
            else
                ++count;

            last = t;
        }
    }

    return count > 0 ? rate * count / (last - first) : 0.0;
}

void lfoPerTick (Rig& rig, int t)
{
    rig.global[static_cast<std::size_t> (ModSource::lfo1)] = static_cast<float> (std::sin (0.05 * t));
    rig.global[static_cast<std::size_t> (ModSource::voiceLevel)] = static_cast<float> (0.5 + 0.5 * std::sin (0.013 * t));
    rig.bend = static_cast<float> (0.3 * std::sin (0.021 * t));
}

} // namespace

TEST_CASE ("SynthVoice: inactive after prepare / reset, renders nothing", "[synth][voice]")
{
    Rig rig;
    CHECK_FALSE (rig.voice.isActive());
    CHECK (rig.voice.getNote() == -1);
    const auto out = rig.run (1000);
    CHECK (std::all_of (out.left.begin(), out.left.end(), [] (float v) { return v == 0.0f; }));
}

TEST_CASE ("SynthVoice: pitch of note, octave / semi / fine, bend and the Pitch destination", "[synth][voice]")
{
    const auto rate = GENERATE (44100.0, 48000.0, 96000.0);
    CAPTURE (rate);

    const auto measure = [rate] (int note, auto&& configure)
    {
        Rig rig (rate);
        configure (rig);
        rig.commands = { { 0, command (VoiceCommandType::start, note) } };
        const auto out = rig.run (static_cast<int> (rate * 0.2) + 70000);
        return st::measureFrequency (std::span<const float> (out.left).subspan (static_cast<std::size_t> (rate * 0.1)), rate);
    };

    CHECK (std::abs (st::cents (measure (69, [] (Rig&) {}), 440.0)) < 0.1);
    CHECK (std::abs (st::cents (measure (57, [] (Rig& r) { r.settings.osc[0].octave = 1; }), 440.0)) < 0.1);
    CHECK (std::abs (st::cents (measure (60, [] (Rig& r) { r.settings.osc[0].semitones = 9; }), 440.0)) < 0.1);
    CHECK (std::abs (st::cents (measure (69, [] (Rig& r) { r.settings.osc[0].fineCents = 37.0f; }), 440.0) - 37.0) < 0.1);
    CHECK (std::abs (st::cents (measure (69, [] (Rig& r) { r.bend = -2.0f; }), 440.0) + 200.0) < 0.1);

    // Pitch destination: +100 % of a source at 0.5 = +6 semitones for the whole voice
    CHECK (std::abs (st::cents (measure (63, [] (Rig& r) {
                                    r.settings.mod[0] = { ModSource::lfo1, ModDest::pitch, 100.0f };
                                    r.global[static_cast<std::size_t> (ModSource::lfo1)] = 0.5f;
                                }),
                                440.0))
           < 0.1);
    // Osc A Pitch destination: only Osc A
    CHECK (std::abs (st::cents (measure (57, [] (Rig& r) {
                                    r.settings.mod[0] = { ModSource::lfo1, ModDest::oscAPitch, 100.0f };
                                    r.global[static_cast<std::size_t> (ModSource::lfo1)] = 1.0f;
                                }),
                                440.0))
           < 0.1);
}

TEST_CASE ("SynthVoice: any host block split is bit-identical (commands mid-tick, modulation, glide)", "[synth][voice]")
{
    const auto rate = GENERATE (44100.0, 48000.0, 96000.0);
    CAPTURE (rate);
    const int total = static_cast<int> (rate * 0.8);

    const auto render = [&] (std::span<const int> blocks)
    {
        Rig rig (rate, &st::sawTable());
        auto& s = rig.settings;
        s.osc[0].unison = 3;
        s.osc[0].phaseRandPercent = 60.0f;
        s.osc[1].on = true;
        s.osc[1].octave = -1;
        s.subLevelPercent = 30.0f;
        s.noiseLevelPercent = 10.0f;
        s.noiseColorPercent = 50.0f;
        s.filterCutoffHz = 800.0f;
        s.filterResonancePercent = 40.0f;
        s.filterDriveDb = 6.0f;
        s.filterEnvAmountPercent = 50.0f;
        s.filterKeytrackPercent = 50.0f;
        s.env[0] = { 7.0f, 50.0f, 60.0f, 80.0f };
        s.env[1] = { 3.0f, 120.0f, 20.0f, 60.0f };
        s.glideTimeMs = 60.0f;
        s.ampVelocityPercent = 50.0f;
        s.mod[0] = { ModSource::lfo1, ModDest::oscAPosition, 30.0f };
        s.mod[1] = { ModSource::env3, ModDest::pan, 50.0f };
        s.mod[2] = { ModSource::voiceLevel, ModDest::ampLevel, -40.0f };
        s.mod[3] = { ModSource::lfo1, ModDest::filterCutoff, 20.0f };
        rig.perTick = lfoPerTick;
        const auto at = [rate] (double seconds) { return static_cast<int64_t> (seconds * rate) | 5; }; // mid-tick
        rig.commands = { { at (0.0), command (VoiceCommandType::start, 48, 90) },
                         { at (0.1), command (VoiceCommandType::legato, 55, 70, true) },
                         { at (0.2), command (VoiceCommandType::correction, 53, 70) },
                         { at (0.3), command (VoiceCommandType::start, 60, 100, true) },
                         { at (0.4), command (VoiceCommandType::steal, 50, 60) },
                         { at (0.402), command (VoiceCommandType::steal, 52, 60) },
                         { at (0.55), command (VoiceCommandType::release, 52) },
                         { at (0.65), command (VoiceCommandType::start, 64, 127, true, 52) } };
        return rig.run (total, blocks);
    };

    const auto reference = render ({});
    CHECK (st::rms (std::span<const float> (reference.left)) > 0.01);

    for (const int block : { 1, 7, 64, 480, 512, 4096 })
    {
        CAPTURE (block);
        const int blocks[] = { block };
        const auto out = render (blocks);
        CHECK (out.left == reference.left);
        CHECK (out.right == reference.right);
    }

    const int odd[] = { 3, 100, 1, 17, 4096, 33 };
    const auto out = render (odd);
    CHECK (out.left == reference.left);
}

TEST_CASE ("SynthVoice: the output only depends on the command position relative to the tick grid", "[synth][voice]")
{
    // The voice output only depends on the command's position relative to the tick grid
    const auto render = [] (int64_t start)
    {
        Rig rig (48000.0, &st::sawTable());
        rig.settings.filterCutoffHz = 2000.0f;
        rig.commands = { { start, command (VoiceCommandType::start, 57) },
                         { start + 4000, command (VoiceCommandType::release, 57) } };
        return rig.run (static_cast<int> (start) + 20000);
    };

    const auto a = render (32 + 5);
    const auto b = render (64 + 5);

    for (std::size_t i = 0; i < 19000; ++i)
        REQUIRE (a.left[i + 37] == b.left[i + 69]);
}

TEST_CASE ("SynthVoice: glide time is constant and linear in semitones", "[synth][voice]")
{
    Rig rig;
    rig.settings.glideTimeMs = 100.0f;
    rig.commands = { { 0, command (VoiceCommandType::start, 57) },
                     { 9600, command (VoiceCommandType::legato, 69, 127, true) } };
    const auto out = rig.run (48000);

    // Before: A3 220 Hz; middle of the glide (50 ms): note 63; after 100 ms: A4 440 Hz
    CHECK_THAT (zeroCrossingFrequency (out.left, 4800, 9600, 48000.0), WithinAbs (220.0, 0.2));
    const auto mid = zeroCrossingFrequency (out.left, 9600 + 2400 - 300, 9600 + 2400 + 300, 48000.0);
    CHECK (std::abs (st::cents (mid, 440.0 * std::exp2 (-6.0 / 12.0))) < 25.0);
    CHECK_THAT (zeroCrossingFrequency (out.left, 9600 + 4800 + tick, 30000, 48000.0), WithinAbs (440.0, 0.2));
    // Still gliding shortly before the end
    CHECK (zeroCrossingFrequency (out.left, 9600 + 4000, 9600 + 4500, 48000.0) < 430.0);

    // A mid-tick command that does not change the pitch (here a release) leaves a running glide alone
    Rig rel;
    rel.settings.glideTimeMs = 100.0f;
    rel.settings.env[0].releaseMs = 2000.0f;
    rel.commands = { { 0, command (VoiceCommandType::start, 57) },
                     { 9600, command (VoiceCommandType::legato, 69, 127, true) },
                     { 9600 + 1205, command (VoiceCommandType::release, 69) } };
    const auto withRelease = rel.run (48000);

    for (std::size_t w = 9600 + 1600; w < 9600 + 4800; w += 400)
        CHECK_THAT (zeroCrossingFrequency (withRelease.left, w, w + 400, 48000.0),
                    WithinAbs (zeroCrossingFrequency (out.left, w, w + 400, 48000.0), 0.05));

    // Poly glide from another note's pitch
    Rig poly;
    poly.settings.glideTimeMs = 50.0f;
    poly.commands = { { 0, command (VoiceCommandType::start, 69, 127, true, 57) } };
    const auto p = poly.run (12000);
    CHECK (zeroCrossingFrequency (p.left, 0, 600, 48000.0) < 250.0);
    CHECK_THAT (zeroCrossingFrequency (p.left, 3000, 12000, 48000.0), WithinAbs (440.0, 0.2));
}

TEST_CASE ("SynthVoice: legato keeps the envelopes, a start retriggers them from the current value", "[synth][voice]")
{
    const auto render = [] (VoiceCommandType change)
    {
        Rig rig;
        rig.settings.env[0] = { 5.0f, 200.0f, 30.0f, 100.0f };
        rig.commands = { { 0, command (VoiceCommandType::start, 60) } };

        if (change != VoiceCommandType::release) // release = "no change" reference
            rig.commands.push_back ({ 4805, command (change, 64) });

        return rig.run (24000);
    };

    const auto reference = render (VoiceCommandType::release);
    const auto legato = render (VoiceCommandType::legato);
    const auto retrigger = render (VoiceCommandType::start);

    // Legato: the amplitude follows the unchanged envelope (sine amplitude does not depend on the pitch)
    for (std::size_t w = 4800; w + 960 <= 24000; w += 960)
        CHECK_THAT (peakOf (legato.left, w, w + 960), WithinAbs (peakOf (reference.left, w, w + 960), 0.02));

    // Retrigger: back up to 1 (attack from the current value), and no step at the change
    CHECK (peakOf (retrigger.left, 4800 + 240, 4800 + 960) > 0.9);
    CHECK (peakOf (reference.left, 4800 + 240, 4800 + 960) < 0.85);
    double maxStep = 0.0;

    for (std::size_t i = 4700; i < 5300; ++i)
        maxStep = std::max (maxStep, static_cast<double> (std::abs (retrigger.left[i] - retrigger.left[i - 1])));

    CHECK (maxStep < 0.06); // a 262 / 330 Hz sine at <= 1: at most 0.043 per sample
}

TEST_CASE ("SynthVoice: a correction jumps without glide and keeps velocity and random value", "[synth][voice]")
{
    Rig rig;
    rig.settings.glideTimeMs = 500.0f;
    rig.settings.ampVelocityPercent = 100.0f;
    rig.commands = { { 0, command (VoiceCommandType::start, 57, 64) },
                     { 2405, command (VoiceCommandType::correction, 69, 127, true) } };
    const auto out = rig.run (12000);
    CHECK_THAT (zeroCrossingFrequency (out.left, 2405 + 2 * tick, 12000, 48000.0), WithinAbs (440.0, 0.2));
    CHECK (rig.voice.getNote() == 69);
    CHECK_THAT (rig.voice.getSourceValue (ModSource::velocity), WithinAbs (64.0 / 127.0, 1e-6));
    // Velocity 64 at 100 % amp velocity: about half level, still after the correction
    CHECK_THAT (peakOf (out.left, 6000, 12000), WithinAbs (64.0 / 127.0, 0.02));

    // Random per note: new at start / legato / steal, kept by a correction
    Rig r2;
    r2.commands = { { 0, command (VoiceCommandType::start, 60) } };
    r2.run (64);
    const auto first = r2.voice.getSourceValue (ModSource::randomPerNote);
    r2.commands = { { 64, command (VoiceCommandType::correction, 61) } };
    r2.run (128);
    CHECK (r2.voice.getSourceValue (ModSource::randomPerNote) == first);
}

TEST_CASE ("SynthVoice: steal fades out over 2 ms, then starts the pending note clean", "[synth][voice]")
{
    Rig rig;
    rig.settings.env[0] = { 0.0f, 0.0f, 100.0f, 50.0f };
    const int64_t stealAt = 4803;
    rig.commands = { { 0, command (VoiceCommandType::start, 60) }, { stealAt, command (VoiceCommandType::steal, 72) } };
    const auto out = rig.run (9600);
    const auto fadeLength = static_cast<int64_t> (std::ceil (VoiceAllocator::stealFadeSeconds * 48000.0)); // 96

    // Linear fade: |out| below the fade line
    for (int64_t k = 0; k < fadeLength; ++k)
    {
        const auto i = static_cast<std::size_t> (stealAt + k);
        REQUIRE (std::abs (out.left[i]) <= 1.0f - static_cast<float> (k + 1) / static_cast<float> (fadeLength) + 1e-5f);
    }

    // Silent from the fade end to the next tick, then the new note starts (attack 0: at once)
    const auto fadeEnd = stealAt + fadeLength;
    const auto nextTick = (fadeEnd + tick - 1) / tick * tick;

    for (auto i = fadeEnd; i < nextTick; ++i)
        CHECK (out.left[static_cast<std::size_t> (i)] == 0.0f);

    CHECK (rig.voice.getNote() == 72);
    CHECK_THAT (zeroCrossingFrequency (out.left, static_cast<std::size_t> (nextTick + 480), 9600, 48000.0),
                WithinAbs (523.25, 0.3));

    // The pending note released during the fade: the voice ends silent after the fade
    Rig r2;
    r2.commands = { { 0, command (VoiceCommandType::start, 60) },
                    { 1000, command (VoiceCommandType::steal, 72) },
                    { 1050, command (VoiceCommandType::release, 72) } };
    const auto o2 = r2.run (3000);
    CHECK_FALSE (r2.voice.isActive());
    CHECK (peakOf (o2.left, 1000 + 96, 3000) == 0.0);
}

TEST_CASE ("SynthVoice: exact 0 and inactive after the release", "[synth][voice]")
{
    const auto rate = GENERATE (44100.0, 96000.0);
    Rig rig (rate, &st::sawTable());
    rig.settings.osc[0].unison = 4;
    rig.settings.subLevelPercent = 50.0f;
    rig.settings.noiseLevelPercent = 20.0f;
    rig.settings.filterResonancePercent = 90.0f;
    rig.settings.env[0].releaseMs = 30.0f;
    rig.commands = { { 0, command (VoiceCommandType::start, 45) }, { 2001, command (VoiceCommandType::release, 45) } };
    const auto total = 2001 + static_cast<int> (0.03 * rate) + 4 * tick;
    const auto out = rig.run (total + 1000);
    CHECK_FALSE (rig.voice.isActive());
    CHECK (rig.voice.isReleased() == false);

    // Inactive from the first tick after the release end + one ramp tick; output exactly 0 after that
    const auto lastActive = std::find (rig.activeAtTick.begin() + 2, rig.activeAtTick.end(), 0) - rig.activeAtTick.begin();
    const auto silentFrom = static_cast<std::size_t> (lastActive * tick);
    CHECK (silentFrom <= static_cast<std::size_t> (total));
    CHECK (peakOf (out.left, silentFrom, out.size()) == 0.0);
    CHECK (peakOf (out.right, silentFrom, out.size()) == 0.0);
    CHECK (peakOf (out.left, 0, 2000) > 0.1);

    // Released state while the release runs
    Rig r2;
    r2.commands = { { 0, command (VoiceCommandType::start, 45) }, { 100, command (VoiceCommandType::release, 45) } };
    r2.run (200);
    CHECK (r2.voice.isActive());
    CHECK (r2.voice.isReleased());
}

TEST_CASE ("SynthVoice: per-voice sources", "[synth][voice]")
{
    Rig rig;
    rig.settings.env[1] = { 0.0f, 0.0f, 40.0f, 100.0f };
    rig.settings.env[2] = { 0.0f, 0.0f, 70.0f, 100.0f };
    rig.global[static_cast<std::size_t> (ModSource::lfo1)] = 0.5f;
    rig.commands = { { 0, command (VoiceCommandType::start, 90, 100) } };
    rig.run (64);
    CHECK_THAT (rig.voice.getSourceValue (ModSource::velocity), WithinAbs (100.0 / 127.0, 1e-6));
    CHECK_THAT (rig.voice.getSourceValue (ModSource::note), WithinAbs (0.5, 1e-6));
    CHECK_THAT (rig.voice.getSourceValue (ModSource::env2), WithinAbs (0.4, 1e-6));
    CHECK_THAT (rig.voice.getSourceValue (ModSource::env3), WithinAbs (0.7, 1e-6));
    const auto r = rig.voice.getSourceValue (ModSource::randomPerNote);
    CHECK (r >= -1.0f);
    CHECK (r <= 1.0f);
    CHECK (rig.voice.getSourceValue (ModSource::lfo1) == 0.0f); // global sources are not the voice's
    CHECK (rig.voice.getSourceValue (ModSource::off) == 0.0f);

    rig.commands = { { 64, command (VoiceCommandType::start, 0) } };
    rig.run (128);
    CHECK (rig.voice.getSourceValue (ModSource::note) == -1.0f);
    CHECK (rig.voice.getSourceValue (ModSource::randomPerNote) != r);
}

TEST_CASE ("SynthVoice: oscillator switched off / on mid-note ramps (no click), phases frozen", "[synth][voice]")
{
    Rig rig;
    rig.settings.env[0] = { 0.0f, 0.0f, 100.0f, 50.0f };
    rig.perTick = [] (Rig& r, int t) { r.settings.osc[0].on = ! (t >= 300 && t < 600); };
    rig.commands = { { 0, command (VoiceCommandType::start, 60) } };
    const auto out = rig.run (16 * 900);
    double maxStep = 0.0;

    for (std::size_t i = 1; i < out.size(); ++i)
        maxStep = std::max (maxStep, static_cast<double> (std::abs (out.left[i] - out.left[i - 1])));

    CHECK (maxStep < 0.1); // one-tick gain ramp (1/16 per sample) on top of the sine's own slope
    CHECK (peakOf (out.left, 16 * 310, 16 * 600) < 1e-6); // only the filter's tail after the one-tick ramp
    CHECK (peakOf (out.left, 16 * 650, 16 * 900) > 0.9);
}

TEST_CASE ("SynthVoice: filter, amp gain and velocity", "[synth][voice]")
{
    const auto level = [] (auto&& configure)
    {
        Rig rig (48000.0, &st::sawTable());
        configure (rig);
        rig.commands = { { 0, command (VoiceCommandType::start, 57, 127) } };
        const auto out = rig.run (24000);
        return rmsOf (out.left, 9600, 24000);
    };

    const auto open = level ([] (Rig& r) { r.settings.filterOn = false; });
    const auto lp = level ([] (Rig& r) { r.settings.filterCutoffHz = 100.0f; r.settings.filterMode = FilterMode::lp24; });
    const auto opened = level ([] (Rig& r) {
        r.settings.filterCutoffHz = 100.0f; // + 5 octaves from Env 2 (sustain 1) = 3.2 kHz
        r.settings.filterEnvAmountPercent = 100.0f;
        r.settings.env[1].sustainPercent = 100.0f;
    });
    const auto gain = level ([] (Rig& r) { r.settings.filterOn = false; r.settings.ampGainDb = -6.0f; });
    const auto ampMod = level ([] (Rig& r) {
        r.settings.filterOn = false;
        r.settings.mod[0] = { ModSource::velocity, ModDest::ampLevel, 100.0f }; // x 2
    });

    CHECK (lp < 0.1 * open);
    CHECK (opened > 0.9 * open);
    CHECK_THAT (gain / open, WithinAbs (std::pow (10.0, -6.0 / 20.0), 0.01));
    CHECK_THAT (ampMod / open, WithinAbs (2.0, 0.02));
}

TEST_CASE ("SynthVoice: non-finite settings and inputs stay finite", "[synth][voice]")
{
    Rig rig (48000.0, &st::sawTable());
    rig.settings.filterCutoffHz = std::numeric_limits<float>::quiet_NaN();
    rig.settings.osc[0].positionPercent = std::numeric_limits<float>::infinity();
    rig.settings.glideTimeMs = std::numeric_limits<float>::quiet_NaN();
    rig.settings.mod[0] = { ModSource::lfo1, ModDest::filterCutoff, 100.0f };
    rig.global[static_cast<std::size_t> (ModSource::lfo1)] = std::numeric_limits<float>::quiet_NaN();
    rig.bend = std::numeric_limits<float>::infinity();
    rig.commands = { { 0, command (VoiceCommandType::start, 60) }, { 500, command (VoiceCommandType::legato, 70, 100, true) } };
    const auto out = rig.run (4800);
    CHECK (std::all_of (out.left.begin(), out.left.end(), [] (float v) { return std::isfinite (v); }));
    CHECK (std::all_of (out.right.begin(), out.right.end(), [] (float v) { return std::isfinite (v); }));
}

TEST_CASE ("SynthVoice: reset forgets the tables, deactivation keeps them", "[synth][voice]")
{
    Rig rig;
    rig.settings.env[0].releaseMs = 1.0f;
    rig.commands = { { 0, command (VoiceCommandType::start, 60) }, { 100, command (VoiceCommandType::release, 60) } };
    rig.run (2000);
    CHECK_FALSE (rig.voice.isActive());
    CHECK (rig.voice.getTablesInUse (0).current == &st::sineTable());
    rig.voice.reset();
    CHECK (rig.voice.getTablesInUse (0).current == nullptr);
    CHECK (rig.voice.getTablesInUse (1).current == nullptr);
}

TEST_CASE ("SynthVoice: benchmark", "[synth][voice][!benchmark]")
{
    const auto bench = [] (int unison, bool twoOsc, bool extras)
    {
        Rig rig (48000.0, &st::sawTable());
        rig.settings.osc[0].unison = unison;
        rig.settings.osc[1].on = twoOsc;
        rig.settings.osc[1].unison = unison;
        rig.settings.filterMode = FilterMode::lp24;
        rig.settings.filterCutoffHz = 2000.0f;
        rig.settings.filterEnvAmountPercent = 30.0f;
        rig.settings.env[1] = { 5.0f, 2000.0f, 0.0f, 200.0f };
        rig.settings.mod[0] = { ModSource::lfo1, ModDest::oscAPosition, 20.0f };
        rig.settings.mod[1] = { ModSource::env3, ModDest::filterCutoff, 20.0f };

        if (extras)
        {
            rig.settings.subLevelPercent = 30.0f;
            rig.settings.noiseLevelPercent = 10.0f;
        }

        rig.perTick = lfoPerTick;
        rig.commands = { { 0, command (VoiceCommandType::start, 57) } };
        return rig;
    };

    for (const auto& [name, unison, two, extras] : { std::tuple { "1 osc x 1, LP24", 1, false, false },
                                                     std::tuple { "2 osc x 4 + sub + noise, LP24", 4, true, true },
                                                     std::tuple { "2 osc x 8 + sub + noise, LP24", 8, true, true } })
    {
        auto rig = bench (unison, two, extras);
        rig.run (480);
        rig.commands.clear();

        BENCHMARK (std::string ("voice ") + name + ", 4800 samples")
        {
            const auto out = rig.run (4800);
            return out.left[100];
        };
    }

    // Filter + envelopes + modulation only (no oscillator, noise as the source)
    auto rig = bench (1, false, true);
    rig.settings.osc[0].on = false;
    rig.run (480);
    rig.commands.clear();

    BENCHMARK ("voice without oscillators (noise + LP24 + envelopes + mod), 4800 samples")
    {
        const auto out = rig.run (4800);
        return out.left[100];
    };
}
