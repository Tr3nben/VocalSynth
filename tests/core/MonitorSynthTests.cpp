#include "vocalsynth/MonitorSynth.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <vector>

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using namespace vocalsynth;

namespace
{

/** An event at an absolute sample position of a test run. */
struct TimedEvent
{
    long long position;
    VsEvent event;
};

/** Renders numSamples of stereo output, split into blocks of blockSize, feeding each event in the
    block that contains it (with a block-relative offset). Returns the left channel; checks that both
    channels are identical. */
std::vector<float> render (MonitorSynth& synth, const std::vector<TimedEvent>& events, int numSamples,
                           int blockSize, float initialValue = 0.0f)
{
    std::vector<float> left ((size_t) numSamples, initialValue);
    std::vector<float> right ((size_t) numSamples, initialValue);
    std::size_t next = 0;

    for (int start = 0; start < numSamples; start += blockSize)
    {
        const auto len = std::min (blockSize, numSamples - start);
        VsEventBuffer<64> buffer;

        while (next < events.size() && events[next].position < start + len)
        {
            auto e = events[next].event;
            e.sampleOffset = (int) (events[next].position - start);
            REQUIRE (buffer.push (e));
            ++next;
        }

        float* channels[] = { left.data() + start, right.data() + start };
        synth.process (buffer, channels, 2, len);
    }

    REQUIRE (left == right);
    return left;
}

bool allFinite (const std::vector<float>& v)
{
    return std::all_of (v.begin(), v.end(), [] (float x) { return std::isfinite (x); });
}

float peakAbs (const std::vector<float>& v, std::size_t from = 0, std::size_t to = (std::size_t) -1)
{
    to = std::min (to, v.size());
    float peak = 0.0f;

    for (auto i = from; i < to; ++i)
        peak = std::max (peak, std::abs (v[i]));

    return peak;
}

/** Positive-going zero crossings in [from, to), as a frequency estimate. */
double estimateFrequency (const std::vector<float>& v, std::size_t from, std::size_t to, double sampleRate)
{
    int crossings = 0;
    std::size_t first = 0, last = 0;

    for (auto i = from + 1; i < to; ++i)
    {
        if (v[i - 1] < 0.0f && v[i] >= 0.0f)
        {
            if (crossings == 0)
                first = i;
            last = i;
            ++crossings;
        }
    }

    if (crossings < 2)
        return 0.0;

    return (crossings - 1) * sampleRate / (double) (last - first);
}

constexpr double sr = 48000.0;

} // namespace

TEST_CASE ("MonitorSynth is exactly silent without notes", "[monitor]")
{
    MonitorSynth synth;
    synth.prepare (sr);

    const auto out = render (synth, {}, 48000, 512);
    CHECK (std::all_of (out.begin(), out.end(), [] (float x) { return x == 0.0f; }));
    CHECK_FALSE (synth.isActive());
}

TEST_CASE ("MonitorSynth adds into existing content", "[monitor]")
{
    MonitorSynth synth;
    synth.prepare (sr);

    SECTION ("idle: content untouched")
    {
        const auto out = render (synth, {}, 4096, 256, 0.5f);
        CHECK (std::all_of (out.begin(), out.end(), [] (float x) { return x == 0.5f; }));
    }

    SECTION ("note: content plus voice")
    {
        const std::vector<TimedEvent> events { { 0, VsEvent::makeNoteOn (0, 1, 69, 100) } };

        MonitorSynth reference;
        reference.prepare (sr);
        const auto voice = render (reference, events, 4800, 480, 0.0f);
        const auto mixed = render (synth, events, 4800, 480, 0.5f);

        for (std::size_t i = 0; i < voice.size(); ++i)
            REQUIRE (mixed[i] == 0.5f + voice[i]);

        CHECK (peakAbs (voice) > 0.2f);
    }
}

TEST_CASE ("MonitorSynth produces a bounded, finite tone for a note", "[monitor]")
{
    const auto sampleRate = GENERATE (8000.0, 44100.0, 48000.0, 96000.0, 192000.0);
    const auto note = GENERATE (0, 21, 60, 69, 127);
    CAPTURE (sampleRate, note);

    MonitorSynth synth;
    synth.prepare (sampleRate);

    const std::vector<TimedEvent> events { { 0, VsEvent::makeNoteOn (0, 1, (uint8_t) note, 100) },
                                           { 100, VsEvent::makePitchBend (0, 1, 8191) },
                                           { 200, VsEvent::makePitchBend (0, 1, -8192) } };

    const auto out = render (synth, events, (int) sampleRate / 2, 512);
    CHECK (allFinite (out));
    CHECK (peakAbs (out) <= MonitorSynth::peakGain + 1e-6f);
    CHECK (synth.isActive());

    if (note >= 21) // very low notes do not reach the peak within half a second of a sine period
        CHECK (peakAbs (out) > 0.9f * MonitorSynth::peakGain);
}

TEST_CASE ("MonitorSynth attack takes about 5 ms", "[monitor]")
{
    MonitorSynth synth;
    synth.prepare (sr);

    const auto out = render (synth, { { 0, VsEvent::makeNoteOn (0, 1, 69, 100) } }, 4800, 64);
    const auto attackSamples = (std::size_t) (MonitorSynth::attackSeconds * sr); // 240

    // During the first half of the attack the level is at most half the peak.
    CHECK (peakAbs (out, 0, attackSamples / 2) <= 0.5f * MonitorSynth::peakGain + 1e-6f);
    // After the attack, a full sine period reaches the peak.
    CHECK_THAT (peakAbs (out, attackSamples, attackSamples + 120), WithinAbs (MonitorSynth::peakGain, 0.002));
}

TEST_CASE ("MonitorSynth release reaches exactly 0 after about 60 ms", "[monitor]")
{
    MonitorSynth synth;
    synth.prepare (sr);

    const long long offAt = 4800;
    const auto releaseSamples = (long long) (MonitorSynth::releaseSeconds * sr); // 2880

    const std::vector<TimedEvent> events { { 0, VsEvent::makeNoteOn (0, 1, 69, 100) },
                                           { offAt, VsEvent::makeNoteOff (0, 1, 69) } };

    const auto out = render (synth, events, 24000, 480);
    CHECK (allFinite (out));

    // Still clearly audible half-way through the release.
    CHECK (peakAbs (out, (std::size_t) (offAt + releaseSamples / 2 - 100), (std::size_t) (offAt + releaseSamples / 2))
           > 0.3f * MonitorSynth::peakGain);

    // Exactly zero a few samples after the 60 ms release.
    const auto silentFrom = (std::size_t) (offAt + releaseSamples + 2);
    CHECK (std::all_of (out.begin() + (long) silentFrom, out.end(), [] (float x) { return x == 0.0f; }));
    CHECK_FALSE (synth.isActive());

    // And it stays silent (and adds nothing) afterwards.
    const auto after = render (synth, {}, 4096, 4096, 0.25f);
    CHECK (std::all_of (after.begin(), after.end(), [] (float x) { return x == 0.25f; }));
}

TEST_CASE ("MonitorSynth ignores note-offs for other notes and CCs", "[monitor]")
{
    MonitorSynth synth;
    synth.prepare (sr);

    const std::vector<TimedEvent> events { { 0, VsEvent::makeNoteOn (0, 1, 60, 100) },
                                           { 100, VsEvent::makeNoteOff (0, 1, 62) },
                                           { 200, VsEvent::makeControlChange (0, 1, 1, 64) } };

    const auto out = render (synth, events, 9600, 512);
    CHECK (synth.isActive());
    CHECK (peakAbs (out, 4800, 9600) > 0.9f * MonitorSynth::peakGain);
}

TEST_CASE ("MonitorSynth note-on with velocity 0 releases the note", "[monitor]")
{
    MonitorSynth synth;
    synth.prepare (sr);

    const std::vector<TimedEvent> events { { 0, VsEvent::makeNoteOn (0, 1, 60, 100) },
                                           { 1000, VsEvent::makeNoteOn (0, 1, 60, 0) } };

    const auto out = render (synth, events, 9600, 512);
    CHECK_FALSE (synth.isActive());
    CHECK (out.back() == 0.0f);
}

TEST_CASE ("MonitorSynth pitch follows note and bend", "[monitor]")
{
    MonitorSynth synth;
    synth.prepare (sr);

    const std::vector<TimedEvent> events { { 0, VsEvent::makeNoteOn (0, 1, 69, 100) },
                                           { 48000, VsEvent::makePitchBend (0, 1, 8191) },
                                           { 96000, VsEvent::makePitchBend (0, 1, -8192) },
                                           { 144000, VsEvent::makePitchBend (0, 1, 0) },
                                           { 144000, VsEvent::makeNoteOff (0, 1, 69) },
                                           { 144000, VsEvent::makeNoteOn (0, 1, 81, 100) } };

    const auto out = render (synth, events, 192000, 480);

    const auto up = 440.0 * std::exp2 ((2.0 * 8191.0 / 8192.0) / 12.0);
    const auto down = 440.0 * std::exp2 (-2.0 / 12.0);

    CHECK_THAT (estimateFrequency (out, 1000, 48000, sr), WithinRel (440.0, 0.002));
    CHECK_THAT (estimateFrequency (out, 49000, 96000, sr), WithinRel (up, 0.002));
    CHECK_THAT (estimateFrequency (out, 97000, 144000, sr), WithinRel (down, 0.002));
    // Legato note change: retuned (no restart of the envelope) to A5.
    CHECK_THAT (estimateFrequency (out, 145000, 192000, sr), WithinRel (880.0, 0.002));
    CHECK (peakAbs (out, 144000, 144100) > 0.9f * MonitorSynth::peakGain);
}

TEST_CASE ("MonitorSynth output is block-size invariant", "[monitor]")
{
    const auto sampleRate = GENERATE (44100.0, 48000.0);
    CAPTURE (sampleRate);

    // Notes, legato, re-strike, bends, release into silence and a new note from silence.
    const std::vector<TimedEvent> events { { 0, VsEvent::makeNoteOn (0, 1, 60, 100) },
                                           { 3000, VsEvent::makePitchBend (0, 1, 4000) },
                                           { 3001, VsEvent::makePitchBend (0, 1, 4100) },
                                           { 5000, VsEvent::makeNoteOff (0, 1, 60) },
                                           { 5000, VsEvent::makeNoteOn (0, 1, 60, 100) },
                                           { 9000, VsEvent::makeNoteOff (0, 1, 60) },
                                           { 9100, VsEvent::makeNoteOn (0, 1, 67, 100) },
                                           { 12000, VsEvent::makePitchBend (0, 1, -8192) },
                                           { 15000, VsEvent::makeNoteOff (0, 1, 67) },
                                           { 15000, VsEvent::makePitchBend (0, 1, 0) },
                                           { 22000, VsEvent::makeNoteOn (0, 1, 72, 100) },
                                           { 26000, VsEvent::makeNoteOff (0, 1, 72) } };

    constexpr int length = 40000;

    MonitorSynth reference;
    reference.prepare (sampleRate);
    const auto expected = render (reference, events, length, length);
    CHECK (allFinite (expected));
    CHECK (peakAbs (expected) > 0.2f);

    const auto blockSize = GENERATE (1, 7, 64, 480, 512, 4096);
    CAPTURE (blockSize);

    MonitorSynth synth;
    synth.prepare (sampleRate);
    const auto out = render (synth, events, length, blockSize);

    REQUIRE (out.size() == expected.size());

    for (std::size_t i = 0; i < out.size(); ++i)
    {
        CAPTURE (i);
        REQUIRE (out[i] == expected[i]);
    }
}

TEST_CASE ("MonitorSynth with output disabled writes nothing", "[monitor]")
{
    MonitorSynth synth;
    synth.prepare (sr);
    synth.setOutputEnabled (false);
    synth.reset();
    CHECK_FALSE (synth.isOutputEnabled());

    const auto out = render (synth, { { 0, VsEvent::makeNoteOn (0, 1, 69, 100) } }, 9600, 512, 0.125f);
    CHECK (std::all_of (out.begin(), out.end(), [] (float x) { return x == 0.125f; }));

    // The voice keeps tracking the held note while muted.
    CHECK (synth.isActive());
}

TEST_CASE ("MonitorSynth enabled during a held note sounds within about 5 ms", "[monitor]")
{
    MonitorSynth synth;
    synth.prepare (sr);
    synth.setOutputEnabled (false);

    render (synth, { { 0, VsEvent::makeNoteOn (0, 1, 60, 100) } }, 9600, 512);

    synth.setOutputEnabled (true);
    const auto out = render (synth, {}, 4800, 512);
    const auto rampSamples = (std::size_t) (MonitorSynth::attackSeconds * sr);

    CHECK (allFinite (out));
    // Ramps in: the first samples are quiet, after the ramp a full period reaches the peak.
    CHECK (peakAbs (out, 0, 24) < 0.1f * MonitorSynth::peakGain);
    CHECK_THAT (peakAbs (out, rampSamples, rampSamples + 200), WithinAbs (MonitorSynth::peakGain, 0.002));

    SECTION ("and ramps out again when disabled")
    {
        synth.setOutputEnabled (false);
        const auto faded = render (synth, {}, 4800, 512, 0.0f);
        CHECK (peakAbs (faded, 0, 24) > 0.0f);
        CHECK (std::all_of (faded.begin() + (long) rampSamples + 1, faded.end(), [] (float x) { return x == 0.0f; }));
    }
}

TEST_CASE ("MonitorSynth reset silences immediately", "[monitor]")
{
    MonitorSynth synth;
    synth.prepare (sr);

    render (synth, { { 0, VsEvent::makeNoteOn (0, 1, 60, 100) } }, 4800, 512);
    REQUIRE (synth.isActive());

    synth.reset();
    CHECK_FALSE (synth.isActive());

    const auto out = render (synth, {}, 4800, 512);
    CHECK (std::all_of (out.begin(), out.end(), [] (float x) { return x == 0.0f; }));
}

TEST_CASE ("MonitorSynth releaseAll fades out without a click and centres the bend", "[monitor]")
{
    MonitorSynth synth;
    synth.prepare (sr);

    // Note 69 bent fully up, sounding at full level
    const auto before = render (synth,
                                { { 0, VsEvent::makeNoteOn (0, 1, 69, 100) },
                                  { 0, VsEvent::makePitchBend (0, 1, 8191) } },
                                4800, 480);
    REQUIRE (synth.isActive());

    synth.releaseAll();
    CHECK (synth.isActive()); // release tail, not a cut

    const auto releaseSamples = (std::size_t) (MonitorSynth::releaseSeconds * sr);
    const auto out = render (synth, {}, 4800, 480);
    CHECK (allFinite (out));

    // Continuous across the release start: no jump bigger than one sine step at full level
    const auto maxStep = (double) MonitorSynth::peakGain * 2.0 * std::numbers::pi * 500.0 / sr;
    CHECK (std::abs (out.front() - before.back()) <= (float) maxStep);
    for (std::size_t i = 1; i < out.size(); ++i)
        REQUIRE (std::abs (out[i] - out[i - 1]) <= (float) maxStep);

    CHECK (peakAbs (out, 0, 100) > 0.8f * MonitorSynth::peakGain);
    CHECK (std::all_of (out.begin() + (long) (releaseSamples + 2), out.end(), [] (float x) { return x == 0.0f; }));
    CHECK_FALSE (synth.isActive());

    // The bend was centred: a new note-on plays at its unbent pitch (440 Hz -> 109.09 samples/cycle)
    const auto retuned = render (synth, { { 0, VsEvent::makeNoteOn (0, 1, 69, 100) } }, 48000, 512);
    int upwardCrossings = 0;
    for (std::size_t i = 4800; i < retuned.size(); ++i)
        if (retuned[i - 1] < 0.0f && retuned[i] >= 0.0f)
            ++upwardCrossings;
    CHECK (std::abs (upwardCrossings - 396) <= 1); // 440 Hz over 0.9 s
}

TEST_CASE ("MonitorSynth handles empty blocks, null channels and out-of-range offsets", "[monitor]")
{
    MonitorSynth synth;
    synth.prepare (sr);

    VsEventBuffer<4> events;
    events.push (VsEvent::makeNoteOn (0, 1, 69, 100));

    // A 0-sample block applies the events without rendering anything.
    synth.process (events, nullptr, 0, 0);
    CHECK (synth.isActive());

    // No channels: state advances, nothing is written.
    synth.process (VsEventBuffer<4> {}, nullptr, 0, 512);
    CHECK (synth.isActive());

    // An offset past the block end is clamped into it.
    std::array<float, 64> data {};
    float* channels[] = { data.data() };
    VsEventBuffer<4> late;
    late.push (VsEvent::makeNoteOff (1000, 1, 69));
    synth.process (late, channels, 1, (int) data.size());

    std::array<float, 4096> tail {};
    float* tailChannels[] = { tail.data() };
    synth.process (VsEventBuffer<4> {}, tailChannels, 1, (int) tail.size());
    CHECK_FALSE (synth.isActive());
    CHECK (tail.back() == 0.0f);
}
