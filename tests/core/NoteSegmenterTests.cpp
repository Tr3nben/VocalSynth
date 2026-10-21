#include "vocalsynth/Decimator.h"
#include "vocalsynth/Framer.h"
#include "vocalsynth/NoteSegmenter.h"
#include "vocalsynth/Pitch.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <numbers>
#include <tuple>
#include <utility>
#include <vector>

using namespace vocalsynth;

namespace
{

/** An event with the absolute host sample of the frame that produced it. */
struct TimedEvent
{
    int64_t position;
    VsEvent event;

    bool operator== (const TimedEvent&) const = default;
};

PitchFrame voicedAt (double midiPitch, float confidence = 0.95f, float rmsDb = -20.0f)
{
    return { static_cast<float> (midiNoteToFrequency (midiPitch)), confidence, rmsDb };
}

PitchFrame unvoiced (float rmsDb = -120.0f) { return { 0.0f, 0.0f, rmsDb }; }

/** Drives a NoteSegmenter + BendGenerator on the pipeline's frame grid without any DSP: frame m completes
    at host sample m * hop - 1 (docs/M2-DESIGN.md 3.10), offsets are simulated for blocks of blockSize
    samples. Checks per frame that every event carries that frame's offset and that a frame emits at most
    maxEventsPerFrame events. */
struct SegmenterRig
{
    double sampleRate = 48000.0;
    int hop = 96;
    int blockSize = 512;
    VoiceSettings settings;
    NoteSegmenter segmenter;
    BendGenerator bend;
    int64_t nextFrameSample = 0;
    std::vector<TimedEvent> events;

    explicit SegmenterRig (VoiceSettings s = {}, double sr = 48000.0, int block = 512)
        : sampleRate (sr), hop (hostHopFor (sr)), blockSize (block)
    {
        segmenter.prepare (sampleRate);
        segmenter.setOnsetTiming (hop, provisionalTimeoutHops * static_cast<int64_t> (hop));
        bend.prepare (sampleRate, static_cast<double> (hop) / sampleRate);
        apply (s);
        nextFrameSample = hop - 1;
    }

    /** Provisional-note timeout of the rig (the pipeline uses window + 3 hops, ~12 hops in Tenor). */
    static constexpr int provisionalTimeoutHops = 10;

    void apply (const VoiceSettings& s)
    {
        settings = sanitise (s);
        segmenter.setSettings (settings);
        bend.setMode (settings.bendMode, settings.bendRange);
    }

    /** Host samples per hop as the pipeline produces them: decimation factor * analysis-rate hop. */
    static int hostHopFor (double sr)
    {
        const auto factor = Decimator::factorFor (sr);
        return factor * Framer::hopSamplesFor (sr / static_cast<double> (factor));
    }

    int offsetOf (int64_t position) const { return static_cast<int> (position % blockSize); }

    /** Processes one frame; returns the events it produced. */
    std::vector<VsEvent> frame (const PitchFrame& f)
    {
        VsEventBuffer<16> buffer;
        const auto position = nextFrameSample;
        segmenter.processFrame (f, position, offsetOf (position), bend, buffer);
        nextFrameSample += hop;

        REQUIRE (buffer.size() <= static_cast<std::size_t> (NoteSegmenter::maxEventsPerFrame));
        std::vector<VsEvent> produced;

        for (const auto& e : buffer)
        {
            REQUIRE (e.sampleOffset == offsetOf (position));
            events.push_back ({ position, e });
            produced.push_back (e);
        }

        return produced;
    }

    /** One FastOnsetDetector frame half a hop before the next full frame (the pipeline's 1 ms grid).
        Checks the offsets and maxEventsPerOnsetFrame. */
    std::vector<VsEvent> onsetFrame (const PitchFrame& f)
    {
        VsEventBuffer<16> buffer;
        const auto position = nextFrameSample - hop / 2;
        segmenter.processOnsetFrame (f, position, offsetOf (position), bend, buffer);

        REQUIRE (buffer.size() <= static_cast<std::size_t> (NoteSegmenter::maxEventsPerOnsetFrame));
        std::vector<VsEvent> produced;

        for (const auto& e : buffer)
        {
            REQUIRE (e.sampleOffset == offsetOf (position));
            events.push_back ({ position, e });
            produced.push_back (e);
        }

        return produced;
    }

    /** count hops of (fast frame, full frame). */
    void hops (const PitchFrame& fast, const PitchFrame& full, int count)
    {
        for (int i = 0; i < count; ++i)
        {
            onsetFrame (fast);
            frame (full);
        }
    }

    void frames (const PitchFrame& f, int count)
    {
        for (int i = 0; i < count; ++i)
            frame (f);
    }

    /** Frames for at least the given duration. */
    void framesFor (const PitchFrame& f, double ms) { frames (f, framesIn (ms)); }

    int framesIn (double ms) const
    {
        return static_cast<int> (std::ceil (ms * 0.001 * sampleRate / static_cast<double> (hop)));
    }

    /** Position of the next frame. */
    int64_t now() const { return nextFrameSample; }

    void flush()
    {
        VsEventBuffer<16> buffer;
        const auto position = nextFrameSample;
        segmenter.flush (position, offsetOf (position), bend, buffer);

        for (const auto& e : buffer)
        {
            REQUIRE (e.sampleOffset == offsetOf (position));
            events.push_back ({ position, e });
        }
    }

    std::vector<TimedEvent> ofType (VsEventType type) const
    {
        std::vector<TimedEvent> result;

        for (const auto& e : events)
            if (e.event.type == type)
                result.push_back (e);

        return result;
    }

    std::vector<TimedEvent> noteOns() const { return ofType (VsEventType::noteOn); }
    std::vector<TimedEvent> noteOffs() const { return ofType (VsEventType::noteOff); }
    std::vector<TimedEvent> bends() const { return ofType (VsEventType::pitchBend); }
};

/** MIDI invariants (docs/M2-DESIGN.md 6): every note-off matches a sounding note, no note-on of a note
    that is already sounding, positions non-decreasing, at most one note sounding outside a legato
    hand-over (both notes only within the same sample), and nothing left sounding at the end. */
void checkMidiInvariants (const std::vector<TimedEvent>& events, bool requireAllOff = true)
{
    std::map<std::pair<int, int>, int64_t> sounding; // (channel, note) -> since
    int64_t lastPosition = std::numeric_limits<int64_t>::min();
    int64_t overlapPosition = -1;

    for (const auto& e : events)
    {
        REQUIRE (e.position >= lastPosition);

        if (e.position != lastPosition)
            REQUIRE (sounding.size() <= 1); // an overlap must be resolved at the same sample

        lastPosition = e.position;
        const auto key = std::make_pair (static_cast<int> (e.event.channel), static_cast<int> (e.event.data1));

        REQUIRE (e.event.channel >= 1);
        REQUIRE (e.event.channel <= 16);

        switch (e.event.type)
        {
            case VsEventType::noteOn:
                REQUIRE (e.event.data1 <= 127);
                REQUIRE (e.event.data2 >= 1);
                REQUIRE (e.event.data2 <= 127);
                REQUIRE (sounding.count (key) == 0); // no duplicate note-on
                sounding[key] = e.position;

                if (sounding.size() > 1)
                    overlapPosition = e.position;

                break;

            case VsEventType::noteOff:
                REQUIRE (sounding.count (key) == 1); // pairing
                sounding.erase (key);
                break;

            case VsEventType::pitchBend:
                REQUIRE (e.event.bend >= -8192);
                REQUIRE (e.event.bend <= 8191);
                break;

            case VsEventType::controlChange:
                FAIL ("the segmenter never sends CCs");
                break;
        }
    }

    (void) overlapPosition;

    if (requireAllOff)
        REQUIRE (sounding.empty());
}

VoiceSettings withStickiness (float percent)
{
    VoiceSettings s;
    s.stickinessPercent = percent;
    return s;
}

} // namespace

TEST_CASE ("NoteSegmenter mapping formulas", "[notesegmenter]")
{
    const auto sticky = NoteSegmenter::stickinessFromPercent (50.0);
    CHECK (sticky.hysteresisSemitones == 0.2);
    CHECK (sticky.minChangeMs == 35.0);
    CHECK (NoteSegmenter::velocityFor (-20.0, -45.0, 0.0) == 100);

    const auto none = NoteSegmenter::stickinessFromPercent (0.0);
    CHECK (none.hysteresisSemitones == 0.0);
    CHECK (none.minChangeMs == 10.0);
    const auto full = NoteSegmenter::stickinessFromPercent (100.0);
    CHECK (full.hysteresisSemitones == 0.4);
    CHECK (full.minChangeMs == 60.0);
    CHECK (NoteSegmenter::stickinessFromPercent (-20.0).minChangeMs == 10.0);
    CHECK (NoteSegmenter::stickinessFromPercent (500.0).minChangeMs == 60.0);
    CHECK (NoteSegmenter::stickinessFromPercent (std::numeric_limits<double>::quiet_NaN()).minChangeMs == 35.0);
}

TEST_CASE ("NoteSegmenter velocity formula", "[notesegmenter]")
{
    // 0 % sensitivity: fixed 100 whatever the level
    for (double rms = -80.0; rms <= 0.0; rms += 1.0)
        CHECK (NoteSegmenter::velocityFor (rms, -45.0, 0.0) == 100);

    // 100 %: 20 at the gate, 127 at gate + 36 dB and above, linear in between
    CHECK (NoteSegmenter::velocityFor (-45.0, -45.0, 100.0) == 20);
    CHECK (NoteSegmenter::velocityFor (-60.0, -45.0, 100.0) == 20);
    CHECK (NoteSegmenter::velocityFor (-9.0, -45.0, 100.0) == 127);
    CHECK (NoteSegmenter::velocityFor (0.0, -45.0, 100.0) == 127);
    CHECK (NoteSegmenter::velocityFor (-27.0, -45.0, 100.0) == 74); // 20 + 107 * 0.5 = 73.5 -> 74

    // 70 % (default) at gate + 18 dB: 0.3 * 100 + 0.7 * 73.5 = 81.45 -> 81 (docs/M2-DESIGN.md 3.8.5)
    CHECK (NoteSegmenter::velocityFor (-27.0, -45.0, 70.0) == 81);

    // Monotone in level for any sensitivity > 0, always 1..127
    for (double s : { 10.0, 50.0, 70.0, 100.0 })
    {
        int previous = 0;

        for (double rms = -70.0; rms <= 0.0; rms += 0.25)
        {
            const int v = NoteSegmenter::velocityFor (rms, -45.0, s);
            CHECK (v >= previous);
            CHECK (v >= 1);
            CHECK (v <= 127);
            previous = v;
        }
    }

    // Invalid input
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    CHECK (NoteSegmenter::velocityFor (nan, -45.0, 100.0) == 20);
    CHECK (NoteSegmenter::velocityFor (-20.0, -45.0, nan) == NoteSegmenter::velocityFor (-20.0, -45.0, 70.0));
    CHECK (NoteSegmenter::velocityFor (-20.0, -45.0, 250.0) == NoteSegmenter::velocityFor (-20.0, -45.0, 100.0));
    CHECK (NoteSegmenter::velocityFor (-20.0, -45.0, -5.0) == 100);
}

TEST_CASE ("NoteSegmenter: rig uses the pipeline's 2 ms hop grid", "[notesegmenter]")
{
    CHECK (SegmenterRig ({}, 44100.0).hop == 88);
    CHECK (SegmenterRig ({}, 48000.0).hop == 96);
    CHECK (SegmenterRig ({}, 96000.0).hop == 192);
}

TEST_CASE ("NoteSegmenter: silence, noise-like and quiet frames never start a note", "[notesegmenter]")
{
    SegmenterRig rig;

    rig.frames (unvoiced(), 200);
    rig.frames (unvoiced (-20.0f), 200);                  // loud but aperiodic
    rig.frames (voicedAt (60.0, 0.79f, -20.0f), 200);     // just below confidenceOn
    rig.frames (voicedAt (60.0, 0.95f, -40.01f), 200);    // just below the gate (default -40 dB)
    rig.frames (voicedAt (60.0, 0.70f, -42.0f), 200);     // weak but never strong
    rig.frames ({ std::numeric_limits<float>::quiet_NaN(), 0.95f, -20.0f }, 20);
    rig.frames ({ 261.6f, std::numeric_limits<float>::quiet_NaN(), -20.0f }, 20);
    rig.frames ({ 261.6f, 0.95f, std::numeric_limits<float>::quiet_NaN() }, 20);
    rig.frames ({ -261.6f, 0.95f, -20.0f }, 20);
    rig.frames ({ std::numeric_limits<float>::infinity(), 0.95f, -20.0f }, 20);

    CHECK (rig.events.empty());
    CHECK (rig.segmenter.getActiveNote() == -1);
    CHECK_FALSE (rig.segmenter.isFrameVoiced());
}

TEST_CASE ("NoteSegmenter: onset at the first strong frame, exactly at the gate and confidence limits",
           "[notesegmenter]")
{
    REQUIRE (NoteSegmenter::onsetConfirmFrames == 1);

    const auto gate = GENERATE (-70.0f, -45.0f, -10.0f);
    VoiceSettings s;
    s.gateThresholdDb = gate;
    SegmenterRig rig (s);

    rig.frames (unvoiced(), 10);
    const auto onsetPosition = rig.now();
    const auto produced = rig.frame (voicedAt (64.1, NoteSegmenter::confidenceOn, gate));

    REQUIRE (produced.size() == 1); // bend off: only the note-on
    CHECK (produced[0].type == VsEventType::noteOn);
    CHECK (produced[0].data1 == 64);
    CHECK (produced[0].channel == 1);
    CHECK (rig.events[0].position == onsetPosition);
    CHECK (rig.segmenter.getActiveNote() == 64);
    CHECK (rig.segmenter.getQuantisedNote() == 64);
    CHECK (rig.segmenter.isFrameVoiced());
}

TEST_CASE ("NoteSegmenter: gate hysteresis and confidence on/off while a note is held", "[notesegmenter]")
{
    VoiceSettings s;
    s.gateThresholdDb = -45.0f;
    s.minNoteLengthMs = 20.0f;
    SegmenterRig rig (s);

    rig.frames (voicedAt (60.0), 20);
    REQUIRE (rig.noteOns().size() == 1);

    SECTION ("weak frames keep the note: confidence >= 0.6 and rms >= gate - 3 dB")
    {
        rig.frames (voicedAt (60.0, NoteSegmenter::confidenceOff, -48.0f), 200);
        rig.frames (voicedAt (60.0, 0.7f, -47.0f), 200);
        CHECK (rig.noteOffs().empty());
        CHECK (rig.segmenter.isFrameVoiced());
    }

    SECTION ("confidence below 0.6 counts as unvoiced")
    {
        rig.frames (voicedAt (60.0, 0.59f, -20.0f), NoteSegmenter::releaseFrames);
        CHECK (rig.noteOffs().size() == 1);
        CHECK_FALSE (rig.segmenter.isFrameVoiced());
    }

    SECTION ("level below gate - 3 dB counts as unvoiced")
    {
        rig.frames (voicedAt (60.0, 0.95f, -48.01f), NoteSegmenter::releaseFrames);
        CHECK (rig.noteOffs().size() == 1);
    }

    SECTION ("the same weak frames do not start a note from idle")
    {
        rig.frames (unvoiced(), 10);
        REQUIRE (rig.noteOffs().size() == 1);
        rig.frames (voicedAt (60.0, 0.7f, -47.0f), 200);
        CHECK (rig.noteOns().size() == 1);
    }
}

TEST_CASE ("NoteSegmenter: weak frames hold the note but never change it", "[notesegmenter]")
{
    // Detector octave errors mostly come with confidence in [confidenceOff, confidenceOn), and the octave
    // guard does not check such frames: a held A2 followed by a G2 below the Tenor range (read as G3 at
    // 0.6-0.79) used to change to an octave-high G3 after T_min.
    VoiceSettings s;
    s.minNoteLengthMs = 20.0f;
    SegmenterRig rig (s);
    rig.frames (voicedAt (45.0), 50);
    REQUIRE (rig.noteOns().size() == 1);

    SECTION ("weak frames far from the note: held, no change, no bend")
    {
        rig.frame (voicedAt (55.0, NoteSegmenter::confidenceOff));
        rig.framesFor (voicedAt (55.0, 0.79f), 500.0);
        CHECK (rig.noteOns().size() == 1);
        CHECK (rig.noteOffs().empty());
        CHECK (rig.segmenter.getActiveNote() == 45);
        CHECK (rig.segmenter.isFrameVoiced());
    }

    SECTION ("weak frames break the T_min run of strong ones")
    {
        const auto tMin = NoteSegmenter::stickinessFromPercent (50.0).minChangeMs;
        const auto almost = rig.framesIn (tMin) - 2;

        for (int i = 0; i < 20; ++i)
        {
            rig.frames (voicedAt (55.0), almost);
            rig.frame (voicedAt (55.0, 0.75f));
        }

        CHECK (rig.noteOns().size() == 1);
        CHECK (rig.segmenter.getActiveNote() == 45);
    }

    SECTION ("strong frames still change the note after T_min")
    {
        rig.framesFor (voicedAt (55.0, NoteSegmenter::confidenceOn), 200.0);
        const auto ons = rig.noteOns();
        REQUIRE (ons.size() == 2);
        CHECK (ons[1].event.data1 == 55);
    }

    rig.flush();
    checkMidiInvariants (rig.events);
}

TEST_CASE ("NoteSegmenter: release after releaseFrames unvoiced frames, gaps shorter than that are bridged",
           "[notesegmenter]")
{
    REQUIRE (NoteSegmenter::releaseFrames == 3);
    VoiceSettings s;
    s.minNoteLengthMs = 20.0f;
    SegmenterRig rig (s);

    rig.frames (voicedAt (62.0), 30);

    // Two unvoiced frames, then voice again: no note-off, and the counter starts again
    for (int i = 0; i < 20; ++i)
    {
        rig.frames (unvoiced(), NoteSegmenter::releaseFrames - 1);
        rig.frames (voicedAt (62.0), 1);
    }

    CHECK (rig.noteOffs().empty());

    rig.frames (unvoiced(), NoteSegmenter::releaseFrames - 1);
    CHECK (rig.noteOffs().empty());
    CHECK (rig.segmenter.getActiveNote() == 62); // still held, no bend updates
    const auto offPosition = rig.now();
    rig.frame (unvoiced());

    const auto offs = rig.noteOffs();
    REQUIRE (offs.size() == 1);
    CHECK (offs[0].position == offPosition);
    CHECK (offs[0].event.data1 == 62);
    CHECK (rig.segmenter.getActiveNote() == -1);
    checkMidiInvariants (rig.events);
}

TEST_CASE ("NoteSegmenter: minNoteLength defers the note-off but never the note-on", "[notesegmenter]")
{
    const auto sr = GENERATE (44100.0, 48000.0, 96000.0);
    const auto minMs = GENERATE (20.0f, 60.0f, 250.0f);
    VoiceSettings s;
    s.minNoteLengthMs = minMs;
    SegmenterRig rig (s, sr);
    const auto minSamples = std::llround (static_cast<double> (minMs) * 0.001 * sr);
    CAPTURE (sr, minMs);

    rig.frames (unvoiced(), 5);
    const auto onPosition = rig.now();
    rig.frame (voicedAt (60.0));
    REQUIRE (rig.noteOns().size() == 1);
    CHECK (rig.noteOns()[0].position == onPosition); // not delayed by minNoteLength

    SECTION ("a short blip ends exactly when the note has lasted minNoteLength")
    {
        rig.frames (voicedAt (60.0), 1);
        rig.frames (unvoiced(), 200);

        const auto offs = rig.noteOffs();
        REQUIRE (offs.size() == 1);
        const auto duration = offs[0].position - onPosition;
        CHECK (duration >= minSamples);
        CHECK (duration < minSamples + rig.hop);
        // and never before releaseFrames unvoiced frames
        CHECK (offs[0].position - (onPosition + 2 * rig.hop) >= (NoteSegmenter::releaseFrames - 1) * rig.hop);
    }

    SECTION ("voice returning during the deferral continues the note")
    {
        const auto waitFrames = std::max (NoteSegmenter::releaseFrames,
                                          static_cast<int> (minSamples / rig.hop) - 2);

        if (minSamples > static_cast<int64_t> (NoteSegmenter::releaseFrames + 3) * rig.hop)
        {
            rig.frames (unvoiced(), waitFrames);
            CHECK (rig.noteOffs().empty());
            rig.frames (voicedAt (60.0), 500);
            CHECK (rig.noteOffs().empty());
            CHECK (rig.noteOns().size() == 1);
        }
    }

    rig.flush();
    checkMidiInvariants (rig.events);
}

TEST_CASE ("NoteSegmenter: stickiness boundary 0.5 + h and T_min", "[notesegmenter]")
{
    const auto percent = GENERATE (0.0f, 50.0f, 100.0f);
    const auto sr = GENERATE (44100.0, 48000.0, 96000.0);
    const auto sticky = NoteSegmenter::stickinessFromPercent (static_cast<double> (percent));
    const auto threshold = 0.5 + sticky.hysteresisSemitones;
    const auto tMinSamples = std::llround (sticky.minChangeMs * 0.001 * sr);
    auto s = withStickiness (percent);
    s.minNoteLengthMs = 20.0f; // shorter than every T_min, so T_min decides
    CAPTURE (percent, sr);

    SECTION ("just below the threshold: never changes")
    {
        SegmenterRig rig (s, sr);
        rig.frames (voicedAt (60.0), 50);
        rig.framesFor (voicedAt (60.0 + threshold - 0.01), 1000.0);
        rig.framesFor (voicedAt (60.0 - threshold + 0.01), 1000.0);
        CHECK (rig.noteOns().size() == 1);
        CHECK (rig.segmenter.getActiveNote() == 60);
    }

    SECTION ("just above the threshold: changes after exactly T_min")
    {
        SegmenterRig rig (s, sr);
        rig.frames (voicedAt (60.0), 50);
        const auto conditionStart = rig.now();
        rig.framesFor (voicedAt (60.0 + threshold + 0.01), 200.0);

        const auto ons = rig.noteOns();
        REQUIRE (ons.size() == 2);
        CHECK (ons[1].event.data1 == 61);
        const auto waited = ons[1].position - conditionStart;
        CHECK (waited >= tMinSamples);
        CHECK (waited < tMinSamples + rig.hop);
    }

    SECTION ("downwards as well")
    {
        SegmenterRig rig (s, sr);
        rig.frames (voicedAt (60.0), 50);
        rig.framesFor (voicedAt (60.0 - threshold - 0.01), 200.0);
        const auto ons = rig.noteOns();
        REQUIRE (ons.size() == 2);
        CHECK (ons[1].event.data1 == 59);
    }

    SECTION ("the timer restarts when the condition fails for one frame")
    {
        SegmenterRig rig (s, sr);
        rig.frames (voicedAt (60.0), 50);
        const auto almost = static_cast<int> (tMinSamples / rig.hop) - 1; // frames < T_min

        for (int i = 0; i < 10; ++i)
        {
            rig.frames (voicedAt (60.0 + threshold + 0.05), almost);
            rig.frame (voicedAt (60.0));
        }

        CHECK (rig.noteOns().size() == 1);

        // an unvoiced frame (held note) also breaks the run
        for (int i = 0; i < 10; ++i)
        {
            rig.frames (voicedAt (60.0 + threshold + 0.05), almost);
            rig.frame (unvoiced());
        }

        CHECK (rig.noteOns().size() == 1);
        CHECK (rig.noteOffs().empty());
    }

    SECTION ("a large jump also waits T_min, then goes straight to the nearest note")
    {
        SegmenterRig rig (s, sr);
        rig.frames (voicedAt (60.0), 50);
        const auto conditionStart = rig.now();
        rig.framesFor (voicedAt (67.2), 200.0);
        const auto ons = rig.noteOns();
        REQUIRE (ons.size() == 2);
        CHECK (ons[1].event.data1 == 67);
        CHECK (ons[1].position - conditionStart >= tMinSamples);
        CHECK (ons[1].position - conditionStart < tMinSamples + rig.hop);
    }
}

TEST_CASE ("NoteSegmenter: change waits for minNoteLength as well as T_min", "[notesegmenter]")
{
    VoiceSettings s; // T_min 35 ms
    s.minNoteLengthMs = 150.0f;
    SegmenterRig rig (s);

    const auto onPosition = rig.now();
    rig.frame (voicedAt (60.0));
    rig.framesFor (voicedAt (62.0), 300.0); // condition holds from the 2nd frame

    const auto ons = rig.noteOns();
    REQUIRE (ons.size() == 2);
    CHECK (ons[1].event.data1 == 62);
    const auto age = ons[1].position - onPosition;
    CHECK (age >= std::llround (0.150 * rig.sampleRate));
    CHECK (age < std::llround (0.150 * rig.sampleRate) + rig.hop);

    // the new note restarts the age: a change back needs another 150 ms
    const auto secondOn = ons[1].position;
    rig.framesFor (voicedAt (60.0), 300.0);
    const auto all = rig.noteOns();
    REQUIRE (all.size() == 3);
    CHECK (all[2].position - secondOn >= std::llround (0.150 * rig.sampleRate));
}

TEST_CASE ("NoteSegmenter: vibrato of +-50 cents gives one note", "[notesegmenter]")
{
    // One note needs the peak |p - q| = 0.5 + |centre offset| to stay below 0.5 + h, i.e. h > |offset|:
    // 25 % (h = 0.1) only for a centred vibrato, 50 % (h = 0.2) and 100 % (h = 0.4) also 15 cents off.
    const auto [percent, centre] = GENERATE (table<float, double> ({ { 25.0f, 60.0 },
                                                                      { 50.0f, 60.0 },
                                                                      { 50.0f, 60.15 },
                                                                      { 50.0f, 59.85 },
                                                                      { 100.0f, 60.0 },
                                                                      { 100.0f, 60.15 },
                                                                      { 100.0f, 59.85 } }));
    const auto sr = GENERATE (44100.0, 48000.0, 96000.0);
    auto s = withStickiness (percent);
    s.bendMode = BendMode::trueBend;
    SegmenterRig rig (s, sr);
    CAPTURE (percent, centre, sr);

    // 2 s of 5.5 Hz vibrato: the change threshold 0.5 + h is never reached
    const auto hopSeconds = static_cast<double> (rig.hop) / sr;
    const auto n = rig.framesIn (2000.0);

    for (int i = 0; i < n; ++i)
    {
        const auto t = static_cast<double> (i) * hopSeconds;
        rig.frame (voicedAt (centre + 0.5 * std::sin (2.0 * std::numbers::pi * 5.5 * t)));
    }

    rig.frames (unvoiced(), 50);

    REQUIRE (rig.noteOns().size() == 1);
    CHECK (rig.noteOns()[0].event.data1 == 60);
    CHECK (rig.noteOffs().size() == 1);

    // True bend follows: about +-(50 + offset) cents = +-2048 + ... at +-2 semitones
    int16_t lo = 0;
    int16_t hi = 0;

    for (const auto& b : rig.bends())
    {
        lo = std::min (lo, b.event.bend);
        hi = std::max (hi, b.event.bend);
    }

    const auto offsetCents = 100.0 * (centre - 60.0);
    CHECK (std::abs (static_cast<double> (hi) - BendGenerator::centsToBend (50.0 + offsetCents, 2)) < 120.0);
    CHECK (std::abs (static_cast<double> (lo) - BendGenerator::centsToBend (-50.0 + offsetCents, 2)) < 120.0);
    CHECK (rig.bends().back().event.bend == 0); // centre after the note-off
    checkMidiInvariants (rig.events);
}

TEST_CASE ("NoteSegmenter: stickiness in scales with gaps uses the midpoint between allowed notes",
           "[notesegmenter]")
{
    VoiceSettings s;
    s.scale = Scale::majorPentatonic; // C D E G A
    s.minNoteLengthMs = 20.0f;        // default stickiness: h = 0.2, change must win by 0.4

    SECTION ("E -> G: midpoint 65.5, change needs p >= 65.7")
    {
        SegmenterRig rig (s);
        rig.frames (voicedAt (64.0), 20);
        rig.framesFor (voicedAt (65.65), 300.0);
        CHECK (rig.noteOns().size() == 1);
        rig.framesFor (voicedAt (65.75), 300.0);
        REQUIRE (rig.noteOns().size() == 2);
        CHECK (rig.noteOns()[1].event.data1 == 67);
    }

    SECTION ("a note outside the scale is quantised at the onset")
    {
        SegmenterRig rig (s);
        rig.frames (voicedAt (66.0), 20); // F# -> G
        REQUIRE (rig.noteOns().size() == 1);
        CHECK (rig.noteOns()[0].event.data1 == 67);
        CHECK (rig.segmenter.getQuantizer().getScale() == Scale::majorPentatonic);
    }

    SECTION ("key transposes the scale")
    {
        s.key = 2; // D major pentatonic: D E F# A B
        SegmenterRig rig (s);
        rig.frames (voicedAt (66.0), 20);
        REQUIRE (rig.noteOns().size() == 1);
        CHECK (rig.noteOns()[0].event.data1 == 66);
        rig.frames (unvoiced(), 50);
        // F lies between E and F#. Not an exact tie: PitchFrame carries float Hz, so MIDI 65.0 comes back
        // ~1e-6 st off (exact ties are tested in ScaleQuantizerTests). Just below / above F instead.
        rig.frames (voicedAt (64.9), 20);
        REQUIRE (rig.noteOns().size() == 2);
        CHECK (rig.noteOns()[1].event.data1 == 64);
        rig.frames (unvoiced(), 50);
        rig.frames (voicedAt (65.1), 20);
        REQUIRE (rig.noteOns().size() == 3);
        CHECK (rig.noteOns()[2].event.data1 == 66);
    }
}

TEST_CASE ("NoteSegmenter: glissando steps monotonically through the notes", "[notesegmenter]")
{
    VoiceSettings s;
    s.minNoteLengthMs = 60.0f;
    SegmenterRig rig (s);

    // 60 -> 67 in 2 s (3.5 semitones per second), then hold
    const auto n = rig.framesIn (2000.0);

    for (int i = 0; i <= n; ++i)
        rig.frame (voicedAt (60.0 + 7.0 * static_cast<double> (i) / static_cast<double> (n)));

    rig.framesFor (voicedAt (67.0), 300.0);
    rig.frames (unvoiced(), 50);

    const auto ons = rig.noteOns();
    REQUIRE (ons.size() == 8);

    for (std::size_t i = 0; i < ons.size(); ++i)
    {
        CHECK (ons[i].event.data1 == 60 + i);

        if (i > 0)
            CHECK (ons[i].position - ons[i - 1].position >= std::llround (0.060 * rig.sampleRate));
    }

    checkMidiInvariants (rig.events);
}

TEST_CASE ("NoteSegmenter: legato vs retrigger event order", "[notesegmenter]")
{
    const auto bendMode = GENERATE (BendMode::off, BendMode::trueBend);
    VoiceSettings s;
    s.minNoteLengthMs = 20.0f;
    s.bendMode = bendMode;

    SECTION ("retrigger: note-off old, bend, note-on new at the same sample")
    {
        s.legato = false;
        SegmenterRig rig (s);
        rig.frames (voicedAt (60.2), 30);
        const auto before = rig.events.size();
        rig.framesFor (voicedAt (62.3), 100.0);

        std::vector<TimedEvent> change (rig.events.begin() + static_cast<std::ptrdiff_t> (before), rig.events.end());
        // drop rate-limited bend updates before the change (there are none in this case: the old note is held
        // at constant pitch until the change, apart from bend updates of 62.3 vs 60 which clamp)
        const auto onIt = std::find_if (change.begin(), change.end(),
                                        [] (const TimedEvent& e) { return e.event.type == VsEventType::noteOn; });
        REQUIRE (onIt != change.end());
        const auto at = onIt->position;
        std::vector<VsEventType> order;

        for (const auto& e : change)
            if (e.position == at)
                order.push_back (e.event.type);

        if (bendMode == BendMode::off)
            CHECK (order == std::vector<VsEventType> { VsEventType::noteOff, VsEventType::noteOn });
        else
            CHECK (order == std::vector<VsEventType> { VsEventType::noteOff, VsEventType::pitchBend, VsEventType::noteOn });

        rig.flush();
        checkMidiInvariants (rig.events);
    }

    SECTION ("legato: bend, note-on new, note-off old at the same sample")
    {
        s.legato = true;
        SegmenterRig rig (s);
        rig.frames (voicedAt (60.2), 30);
        rig.framesFor (voicedAt (62.3), 100.0);

        const auto ons = rig.noteOns();
        REQUIRE (ons.size() == 2);
        const auto at = ons[1].position;
        std::vector<VsEventType> order;
        std::vector<int> notes;

        for (const auto& e : rig.events)
        {
            if (e.position == at)
            {
                order.push_back (e.event.type);
                notes.push_back (e.event.data1);
            }
        }

        if (bendMode == BendMode::off)
        {
            CHECK (order == std::vector<VsEventType> { VsEventType::noteOn, VsEventType::noteOff });
            CHECK (notes == std::vector<int> { 62, 60 });
        }
        else
        {
            CHECK (order == std::vector<VsEventType> { VsEventType::pitchBend, VsEventType::noteOn, VsEventType::noteOff });
            CHECK (notes == std::vector<int> { 0, 62, 60 });
        }

        rig.flush();
        checkMidiInvariants (rig.events);
    }
}

TEST_CASE ("NoteSegmenter: note start and end order with bends", "[notesegmenter]")
{
    VoiceSettings s;
    s.bendMode = BendMode::trueBend;
    s.bendRange = BendRange::semitones12;
    s.minNoteLengthMs = 20.0f;
    s.channel = 5;
    SegmenterRig rig (s);

    rig.frame (voicedAt (60.3));
    REQUIRE (rig.events.size() == 2);
    CHECK (rig.events[0].event.type == VsEventType::pitchBend);
    CHECK (rig.events[0].event.bend == BendGenerator::centsToBend (30.0, 12));
    CHECK (rig.events[0].event.channel == 5);
    CHECK (rig.events[1].event.type == VsEventType::noteOn);
    CHECK (rig.events[1].event.channel == 5);
    CHECK (rig.events[0].position == rig.events[1].position);

    rig.frames (voicedAt (60.3), 30);
    const auto before = rig.events.size();
    rig.frames (unvoiced(), 10);
    REQUIRE (rig.events.size() == before + 2);
    CHECK (rig.events[before].event.type == VsEventType::noteOff);
    CHECK (rig.events[before].event.channel == 5);
    CHECK (rig.events[before + 1].event.type == VsEventType::pitchBend);
    CHECK (rig.events[before + 1].event.bend == 0);
    CHECK (rig.events[before].position == rig.events[before + 1].position);

    // Anchored: a note within the dead zone sends no bend at all
    VoiceSettings a = s;
    a.bendMode = BendMode::anchored;
    SegmenterRig anchored (a);
    anchored.frames (voicedAt (60.2), 50);
    anchored.frames (unvoiced(), 10);
    CHECK (anchored.bends().empty());
    CHECK (anchored.noteOns().size() == 1);
    CHECK (anchored.noteOffs().size() == 1);
}

TEST_CASE ("NoteSegmenter: octave shift", "[notesegmenter]")
{
    VoiceSettings s;
    s.minNoteLengthMs = 250.0f; // shift changes must not wait for it
    s.stickinessPercent = 100.0f;

    SECTION ("onset is shifted by 12 * shift, bend stays relative to the quantised note")
    {
        for (int shift = -3; shift <= 3; ++shift)
        {
            s.octaveShift = shift;
            s.bendMode = BendMode::trueBend;
            SegmenterRig rig (s);
            rig.frames (voicedAt (60.25), 5);
            REQUIRE (rig.noteOns().size() == 1);
            CHECK (rig.noteOns()[0].event.data1 == 60 + 12 * shift);
            CHECK (rig.segmenter.getActiveNote() == 60 + 12 * shift);
            CHECK (rig.segmenter.getQuantisedNote() == 60);
            REQUIRE (rig.bends().size() == 1);
            CHECK (rig.bends()[0].event.bend == BendGenerator::centsToBend (25.0, 2));
        }
    }

    SECTION ("a shifted note outside 0..127 is not played")
    {
        s.octaveShift = 3;
        SegmenterRig high (s);
        high.frames (voicedAt (92.0), 50); // 92 + 36 = 128
        CHECK (high.events.empty());
        CHECK_FALSE (high.segmenter.isFrameVoiced());
        high.frames (voicedAt (91.0), 5); // 127: fine
        REQUIRE (high.noteOns().size() == 1);
        CHECK (high.noteOns()[0].event.data1 == 127);

        s.octaveShift = -3;
        SegmenterRig low (s);
        low.frames (voicedAt (35.0), 50); // -1
        CHECK (low.events.empty());
        low.frames (voicedAt (36.0), 5); // 0
        REQUIRE (low.noteOns().size() == 1);
        CHECK (low.noteOns()[0].event.data1 == 0);
    }

    SECTION ("shift change while held: immediate change, same q, legato/retrigger order")
    {
        const auto legato = GENERATE (false, true);
        s.legato = legato;
        SegmenterRig rig (s);
        rig.frames (voicedAt (60.0), 3); // note only 6 ms old: minNoteLength / T_min do not apply

        auto shifted = s;
        shifted.octaveShift = 1;
        rig.apply (shifted);
        CHECK (rig.events.size() == 1); // setSettings never emits
        const auto changePosition = rig.now();
        const auto produced = rig.frame (voicedAt (60.0));

        REQUIRE (produced.size() == 2);

        if (legato)
        {
            CHECK (produced[0].type == VsEventType::noteOn);
            CHECK (produced[0].data1 == 72);
            CHECK (produced[1].type == VsEventType::noteOff);
            CHECK (produced[1].data1 == 60);
        }
        else
        {
            CHECK (produced[0].type == VsEventType::noteOff);
            CHECK (produced[0].data1 == 60);
            CHECK (produced[1].type == VsEventType::noteOn);
            CHECK (produced[1].data1 == 72);
        }

        CHECK (rig.events.back().position == changePosition);
        CHECK (rig.segmenter.getActiveNote() == 72);
        CHECK (rig.segmenter.getQuantisedNote() == 60);

        // and back down
        rig.apply (s);
        rig.frame (voicedAt (60.0));
        CHECK (rig.segmenter.getActiveNote() == 60);
        CHECK (rig.noteOns().size() == 3);

        rig.flush();
        checkMidiInvariants (rig.events);
    }

    SECTION ("shift change while the note is being released waits for voice, out-of-range ends at once")
    {
        SegmenterRig rig (s);
        rig.frames (voicedAt (100.0), 3);
        rig.frame (unvoiced()); // held (release counting, minNoteLength)

        auto shifted = s;
        shifted.octaveShift = 1;
        rig.apply (shifted);
        rig.frame (unvoiced());
        CHECK (rig.noteOns().size() == 1); // not retriggered while unvoiced

        shifted.octaveShift = 3; // 136: outside 0..127
        rig.apply (shifted);
        const auto offPosition = rig.now();
        const auto produced = rig.frame (unvoiced());
        REQUIRE (produced.size() == 1);
        CHECK (produced[0].type == VsEventType::noteOff);
        CHECK (produced[0].data1 == 100);
        CHECK (rig.events.back().position == offPosition);
        CHECK (rig.segmenter.getActiveNote() == -1);

        // stays silent while the sung note cannot be played with this shift
        rig.frames (voicedAt (100.0), 100);
        CHECK (rig.noteOns().size() == 1);
        checkMidiInvariants (rig.events);
    }

    SECTION ("shift into out-of-range while voiced: immediate note-off and bend centre")
    {
        s.bendMode = BendMode::trueBend;
        SegmenterRig rig (s);
        rig.frames (voicedAt (120.3), 3);
        auto shifted = s;
        shifted.octaveShift = 1;
        rig.apply (shifted);
        const auto produced = rig.frame (voicedAt (120.3));

        REQUIRE (produced.size() == 2);
        CHECK (produced[0].type == VsEventType::noteOff);
        CHECK (produced[0].data1 == 120);
        CHECK (produced[1].type == VsEventType::pitchBend);
        CHECK (produced[1].bend == 0);
        checkMidiInvariants (rig.events);
    }

    SECTION ("a change towards a note the shift cannot play releases the note instead")
    {
        s.octaveShift = 3;
        s.minNoteLengthMs = 20.0f;
        SegmenterRig rig (s);
        rig.frames (voicedAt (91.0), 20); // 127
        REQUIRE (rig.noteOns().size() == 1);
        rig.frames (voicedAt (93.0), 20); // 129: treated as unvoiced -> release
        CHECK (rig.noteOns().size() == 1);
        CHECK (rig.noteOffs().size() == 1);
        checkMidiInvariants (rig.events);
    }
}

TEST_CASE ("NoteSegmenter: velocity from the onset RMS and sensitivity", "[notesegmenter]")
{
    const auto sensitivity = GENERATE (0.0f, 70.0f, 100.0f);
    const auto rms = GENERATE (-40.0f, -30.0f, -9.0f, -3.0f); // from the default gate (-40 dB) up
    VoiceSettings s;
    const auto gate = static_cast<double> (s.gateThresholdDb);
    s.velocitySensitivityPercent = sensitivity;
    s.minNoteLengthMs = 20.0f;
    s.stickinessPercent = 0.0f;
    SegmenterRig rig (s);
    CAPTURE (sensitivity, rms);

    rig.frames (voicedAt (60.0, 0.95f, rms), 1);
    rig.frames (voicedAt (60.0, 0.95f, -10.0f), 20); // later level changes do not matter
    REQUIRE (rig.noteOns().size() == 1);
    const auto expected = NoteSegmenter::velocityFor (static_cast<double> (rms), gate, static_cast<double> (sensitivity));
    CHECK (rig.noteOns()[0].event.data2 == expected);

    if (sensitivity == 0.0f)
        CHECK (rig.noteOns()[0].event.data2 == 100);

    // A note change takes the level of the change frame
    rig.framesFor (voicedAt (65.0, 0.95f, -40.0f), 100.0);
    REQUIRE (rig.noteOns().size() == 2);
    CHECK (rig.noteOns()[1].event.data2
           == NoteSegmenter::velocityFor (-40.0, gate, static_cast<double> (sensitivity)));
}

TEST_CASE ("NoteSegmenter: flush ends the note at the given offset and returns to idle", "[notesegmenter]")
{
    VoiceSettings s;
    s.bendMode = BendMode::trueBend;
    SegmenterRig rig (s);

    rig.flush(); // idle: nothing
    CHECK (rig.events.empty());

    rig.frames (voicedAt (57.4), 10);
    REQUIRE (rig.noteOns().size() == 1);
    const auto flushPosition = rig.now();
    rig.flush();

    const auto offs = rig.noteOffs();
    REQUIRE (offs.size() == 1);
    CHECK (offs[0].position == flushPosition);
    CHECK (offs[0].event.data1 == 57);
    CHECK (rig.events.back().event.type == VsEventType::pitchBend);
    CHECK (rig.events.back().event.bend == 0);
    CHECK (rig.bend.getLastValue() == 0);
    CHECK (rig.segmenter.getActiveNote() == -1);
    CHECK (rig.segmenter.getQuantisedNote() == -1);
    CHECK_FALSE (rig.segmenter.isFrameVoiced());

    rig.flush(); // twice: nothing more
    CHECK (rig.noteOffs().size() == 1);

    // A new onset works right away (the next strong frame)
    rig.frame (voicedAt (57.4));
    CHECK (rig.noteOns().size() == 2);
    rig.flush();
    checkMidiInvariants (rig.events);

    // flush uses the channel of the note, even if the settings changed since
    rig.frame (voicedAt (57.0));
    auto other = s;
    other.channel = 9;
    rig.apply (other);
    rig.flush();
    CHECK (rig.noteOffs().back().event.channel == 1);
    checkMidiInvariants (rig.events);
}

TEST_CASE ("NoteSegmenter: reset forgets the note without emitting", "[notesegmenter]")
{
    SegmenterRig rig;
    rig.frames (voicedAt (60.0), 10);
    REQUIRE (rig.segmenter.getActiveNote() == 60);
    const auto before = rig.events.size();
    rig.segmenter.reset();
    CHECK (rig.events.size() == before);
    CHECK (rig.segmenter.getActiveNote() == -1);
}

TEST_CASE ("NoteSegmenter: setSettings never emits", "[notesegmenter]")
{
    VoiceSettings s;
    s.bendMode = BendMode::trueBend;
    SegmenterRig rig (s);
    rig.frames (voicedAt (60.4), 10);
    const auto before = rig.events.size();

    for (int i = 0; i < 20; ++i)
    {
        VoiceSettings t;
        t.key = i % 12;
        t.scale = static_cast<Scale> (i % numScales);
        t.octaveShift = i % 7 - 3;
        t.legato = (i % 2) == 0;
        t.bendMode = static_cast<BendMode> (i % numBendModes);
        t.stickinessPercent = static_cast<float> (i * 5);
        rig.apply (t);
    }

    CHECK (rig.events.size() == before);
}

TEST_CASE ("NoteSegmenter: timing is in host samples, the same in ms at every rate", "[notesegmenter]")
{
    // Onset frame, T_min and minNoteLength measured in ms agree across rates within one hop (2 ms)
    std::vector<double> changeMs;
    std::vector<double> offMs;

    for (double sr : { 44100.0, 48000.0, 96000.0 })
    {
        VoiceSettings s;
        s.minNoteLengthMs = 100.0f;
        SegmenterRig rig (s, sr);
        const auto start = rig.now();
        rig.frame (voicedAt (60.0));
        rig.framesFor (voicedAt (61.0), 300.0);
        rig.frame (unvoiced());
        rig.framesFor (unvoiced(), 300.0);

        const auto ons = rig.noteOns();
        const auto offs = rig.noteOffs();
        REQUIRE (ons.size() == 2);
        REQUIRE (offs.size() == 2);
        changeMs.push_back (1000.0 * static_cast<double> (ons[1].position - start) / sr);
        offMs.push_back (1000.0 * static_cast<double> (offs[1].position - ons[1].position) / sr);
    }

    for (std::size_t i = 1; i < changeMs.size(); ++i)
    {
        CHECK (std::abs (changeMs[i] - changeMs[0]) <= 2.1);
        CHECK (std::abs (offMs[i] - offMs[0]) <= 2.1);
    }

    CHECK (changeMs[0] >= 100.0); // minNoteLength (100) > T_min (35)
    CHECK (offMs[0] >= 100.0);
}

TEST_CASE ("NoteSegmenter: events depend only on absolute samples, not on the block size", "[notesegmenter]")
{
    std::vector<std::vector<TimedEvent>> runs;

    for (int blockSize : { 1, 7, 64, 480, 512, 4096 })
    {
        VoiceSettings s;
        s.bendMode = BendMode::anchored;
        s.legato = true;
        SegmenterRig rig (s, 48000.0, blockSize);

        for (int i = 0; i < 3000; ++i)
        {
            const auto t = static_cast<double> (i) * 0.002;
            const auto phrase = (i / 400) % 2 == 0;
            rig.frame (phrase ? voicedAt (60.0 + 4.0 * std::sin (t * 1.3) + 0.4 * std::sin (t * 34.0)) : unvoiced());
        }

        rig.flush();
        checkMidiInvariants (rig.events);

        // Compare with offsets normalised away (they differ by design), positions must be identical
        auto normalised = rig.events;

        for (auto& e : normalised)
            e.event.sampleOffset = 0;

        runs.push_back (normalised);
    }

    REQUIRE (runs[0].size() > 10);

    for (std::size_t i = 1; i < runs.size(); ++i)
        CHECK (runs[i] == runs[0]);
}

TEST_CASE ("NoteSegmenter: MIDI invariants under random frame sequences and settings changes", "[notesegmenter]")
{
    // Deterministic xorshift64* (no <random> distributions: platform-independent)
    uint64_t state = 0x9e3779b97f4a7c15ull;
    const auto next = [&state]
    {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        return state * 0x2545f4914f6cdd1dull;
    };
    const auto uniform = [&next] { return static_cast<double> (next() >> 11) * 0x1.0p-53; };

    const auto seedIndex = GENERATE (range (0, 12));
    for (int i = 0; i < seedIndex * 7 + 1; ++i)
        next();

    VoiceSettings s;
    s.bendMode = static_cast<BendMode> (seedIndex % numBendModes);
    s.legato = (seedIndex % 2) == 1;
    SegmenterRig rig (s, seedIndex % 3 == 0 ? 44100.0 : (seedIndex % 3 == 1 ? 48000.0 : 96000.0),
                      seedIndex % 4 == 0 ? 7 : 512);

    double pitch = 30.0 + 70.0 * uniform();
    std::size_t maxSounding = 0;

    for (int i = 0; i < 20000; ++i)
    {
        // Random walk with jumps, octave errors, dropouts, borderline levels and confidences
        const auto r = uniform();

        if (r < 0.01)
            pitch = 20.0 + 90.0 * uniform();
        else if (r < 0.02)
            pitch += uniform() < 0.5 ? 12.0 : -12.0;
        else
            pitch += (uniform() - 0.5) * 0.4;

        pitch = std::clamp (pitch, 10.0, 120.0);
        const auto voicedProbability = (i / 500) % 3 == 0 ? 0.3 : 0.92;
        const auto confidence = static_cast<float> (0.5 + 0.5 * uniform());
        const auto level = static_cast<float> (-55.0 + 40.0 * uniform());
        rig.frame (uniform() < voicedProbability ? voicedAt (pitch, confidence, level) : unvoiced (level));

        // Occasional settings changes of every non-flushing kind (the pipeline flushes for the others)
        if (uniform() < 0.003)
        {
            auto t = rig.settings;
            t.octaveShift = static_cast<int> (next() % 7) - 3;
            t.legato = (next() % 2) == 0;
            t.stickinessPercent = static_cast<float> (100.0 * uniform());
            t.minNoteLengthMs = static_cast<float> (20.0 + 230.0 * uniform());
            t.gateThresholdDb = static_cast<float> (-70.0 + 60.0 * uniform());
            t.velocitySensitivityPercent = static_cast<float> (100.0 * uniform());
            t.bendMode = static_cast<BendMode> (next() % numBendModes);
            t.bendRange = static_cast<BendRange> (next() % numBendRanges);
            rig.apply (t);
        }

        if (uniform() < 0.0005)
            rig.flush();

        maxSounding = std::max (maxSounding, rig.segmenter.getActiveNote() >= 0 ? std::size_t { 1 } : std::size_t { 0 });
    }

    rig.flush();
    CAPTURE (seedIndex);
    REQUIRE (rig.noteOns().size() > 20);
    checkMidiInvariants (rig.events);
    CHECK (rig.noteOns().size() == rig.noteOffs().size());
    CHECK (rig.segmenter.getActiveNote() == -1);
    CHECK (rig.bend.getLastValue() == 0);
    CHECK (maxSounding == 1);

    // Offsets lie inside the block, and after the events of a frame without a sounding note the bend is
    // back at the centre (note end / flush -> bend centre in the same frame)
    int sounding = 0;
    int lastBend = 0;

    for (std::size_t i = 0; i < rig.events.size(); ++i)
    {
        const auto& e = rig.events[i];
        REQUIRE (e.event.sampleOffset >= 0);
        REQUIRE (e.event.sampleOffset < rig.blockSize);

        if (e.event.type == VsEventType::noteOn)
            ++sounding;
        else if (e.event.type == VsEventType::noteOff)
            --sounding;
        else if (e.event.type == VsEventType::pitchBend)
            lastBend = e.event.bend;

        const auto lastOfFrame = i + 1 == rig.events.size() || rig.events[i + 1].position != e.position;

        if (lastOfFrame && sounding == 0)
            REQUIRE (lastBend == 0);
    }
}

// ---------------------------------------------------------------------------------------------------------
// Provisional onsets (FastOnsetDetector frames, docs/M2-DESIGN.md 11)

TEST_CASE ("NoteSegmenter provisional onset: two agreeing fast frames start the note at the second", "[notesegmenter][fastonset]")
{
    REQUIRE (NoteSegmenter::fastOnsetConfirmFrames == 2);
    SegmenterRig rig;
    const auto gate = static_cast<double> (rig.settings.gateThresholdDb);

    rig.frame (unvoiced (-20.0f));
    CHECK (rig.onsetFrame (voicedAt (60.0, 0.99f, -20.0f)).empty());
    rig.frame (unvoiced (-20.0f)); // the full window is still filling
    const auto position = rig.now() - rig.hop / 2;
    const auto produced = rig.onsetFrame (voicedAt (60.1, NoteSegmenter::fastConfidenceOn, -18.0f));

    REQUIRE (produced.size() == 1); // bend off: only the note-on
    CHECK (produced[0].type == VsEventType::noteOn);
    CHECK (produced[0].data1 == 60);
    CHECK (produced[0].data2 == NoteSegmenter::velocityFor (-18.0, gate, 70.0)); // max rmsDb of both frames
    CHECK (rig.events[0].position == position);
    CHECK (rig.segmenter.getActiveNote() == 60);
    CHECK (rig.segmenter.isProvisional());
    CHECK (rig.segmenter.isFrameVoiced());

    // While a note is on, fast frames are ignored
    CHECK (rig.onsetFrame (voicedAt (67.0)).empty());
    CHECK (rig.onsetFrame (voicedAt (67.0)).empty());
}

TEST_CASE ("NoteSegmenter provisional onset: weak, quiet, single, alternating or disagreeing fast frames start nothing",
           "[notesegmenter][fastonset]")
{
    const auto quietDb = VoiceSettings {}.gateThresholdDb - 0.01f;

    for (const auto& [name, a, b] : { std::tuple { "below fastConfidenceOn", voicedAt (60.0, 0.949f), voicedAt (60.0, 0.949f) },
                                      std::tuple { "below the gate", voicedAt (60.0, 0.99f, quietDb), voicedAt (60.0, 0.99f, quietDb) },
                                      std::tuple { "alternating", voicedAt (60.0, 0.99f), unvoiced (-20.0f) },
                                      std::tuple { "disagreeing by 25 cents", voicedAt (60.0, 0.99f), voicedAt (60.25, 0.99f) } })
    {
        CAPTURE (name);
        SegmenterRig rig;

        for (int i = 0; i < 20; ++i)
        {
            rig.onsetFrame (a);
            rig.frame (unvoiced (-20.0f));
            rig.onsetFrame (b);
            rig.frame (unvoiced (-20.0f));
        }

        CHECK (rig.events.empty());
    }

    // Within fastMaxSpreadCents they agree
    SegmenterRig rig;
    rig.onsetFrame (voicedAt (60.0, 0.99f));
    rig.frame (unvoiced (-20.0f));
    rig.onsetFrame (voicedAt (60.19, 0.99f));
    CHECK (rig.noteOns().size() == 1);
}

TEST_CASE ("NoteSegmenter provisional onset: no start from a pitch near a note boundary", "[notesegmenter][fastonset]")
{
    // The frame that would start the provisional note must quantise to the same note at p +-
    // fastNoteMarginSemitones (v0.2.1: a vibrato trough at the onset read 45 cents flat and started a
    // wrong-semitone provisional note). The run goes on: the next clear frame within fastMaxSpreadCents
    // starts the note at once.
    REQUIRE (NoteSegmenter::fastNoteMarginSemitones == 0.15);

    struct Case
    {
        Scale scale;
        double pitch;
        int expected;         ///< -1: no provisional note from pitch
        double settled = 0.0; ///< for expected == -1: a clear pitch within fastMaxSpreadCents afterwards ...
        int settledNote = -1; ///< ... and the note it starts
    };

    // Chromatic boundaries at x.5; C major between C and D at 61.0, between E and F at 64.5
    for (const auto& c : { Case { Scale::chromatic, 60.36, -1, 60.3, 60 }, Case { Scale::chromatic, 59.64, -1, 59.7, 60 },
                           Case { Scale::chromatic, 60.34, 60 }, Case { Scale::chromatic, 59.66, 60 },
                           Case { Scale::major, 61.1, -1, 61.2, 62 }, Case { Scale::major, 61.2, 62 },
                           Case { Scale::major, 64.4, -1, 64.3, 64 }, Case { Scale::major, 64.3, 64 } })
    {
        CAPTURE (static_cast<int> (c.scale), c.pitch);
        VoiceSettings s;
        s.scale = c.scale;
        SegmenterRig rig (s);

        for (int i = 0; i < 8; ++i)
        {
            rig.onsetFrame (voicedAt (c.pitch, 0.99f));
            rig.frame (unvoiced (-20.0f)); // the full window is still filling
        }

        if (c.expected < 0)
        {
            CHECK (rig.events.empty());
            const auto produced = rig.onsetFrame (voicedAt (c.settled, 0.99f));
            REQUIRE (produced.size() == 1);
            CHECK (produced[0].data1 == c.settledNote);
        }
        else
        {
            const auto ons = rig.noteOns();
            REQUIRE (ons.size() == 1);
            CHECK (ons[0].event.data1 == c.expected);
        }

        CHECK (rig.segmenter.isProvisional());
    }

    // A boundary pitch far from the settled one restarts the run (fastMaxSpreadCents)
    SegmenterRig rig;
    rig.onsetFrame (voicedAt (60.45, 0.99f));
    rig.frame (unvoiced (-20.0f));
    rig.onsetFrame (voicedAt (60.45, 0.99f));
    rig.frame (unvoiced (-20.0f));
    CHECK (rig.onsetFrame (voicedAt (60.2, 0.99f)).empty());
    rig.frame (unvoiced (-20.0f));
    CHECK (rig.onsetFrame (voicedAt (60.2, 0.99f)).size() == 1);
}

TEST_CASE ("NoteSegmenter provisional note: ignored for one hop, then confirmed or corrected at once by a strong full frame",
           "[notesegmenter][fastonset]")
{
    const auto legato = GENERATE (false, true);
    CAPTURE (legato);
    VoiceSettings s;
    s.legato = legato;
    s.minNoteLengthMs = 250.0f;  // the correction ignores minNoteLength ...
    s.stickinessPercent = 100.0f; // ... and T_min

    SECTION ("the full window reads another note (an octave below): immediate change")
    {
        SegmenterRig rig (s);
        rig.onsetFrame (voicedAt (64.0));
        rig.frame (unvoiced (-20.0f));
        rig.onsetFrame (voicedAt (64.0));
        REQUIRE (rig.segmenter.isProvisional());

        CHECK (rig.frame (voicedAt (52.0)).empty()); // half a hop after the onset: ignored
        CHECK (rig.segmenter.isProvisional());

        const auto position = rig.now();
        const auto produced = rig.frame (voicedAt (52.0));
        REQUIRE (produced.size() == 2);
        CHECK (produced[legato ? 0 : 1].type == VsEventType::noteOn);
        CHECK (produced[legato ? 0 : 1].data1 == 52);
        CHECK (produced[legato ? 1 : 0].type == VsEventType::noteOff);
        CHECK (produced[legato ? 1 : 0].data1 == 64);
        CHECK (rig.events.back().position == position);
        CHECK_FALSE (rig.segmenter.isProvisional());
        CHECK (rig.segmenter.getActiveNote() == 52);

        // From now on a normal note: the next change needs T_min and minNoteLength again
        rig.framesFor (voicedAt (55.0), 50.0);
        CHECK (rig.segmenter.getActiveNote() == 52);
        rig.framesFor (voicedAt (55.0), 300.0);
        CHECK (rig.segmenter.getActiveNote() == 55);
    }

    SECTION ("the full window agrees: the note simply continues")
    {
        SegmenterRig rig (s);
        rig.onsetFrame (voicedAt (64.0));
        rig.frame (unvoiced (-20.0f));
        rig.onsetFrame (voicedAt (64.0));
        rig.frame (unvoiced (-20.0f));
        rig.frames (voicedAt (64.2, 0.7f), 3); // weak frames hold the note, no confirmation yet
        CHECK (rig.segmenter.isProvisional());
        CHECK (rig.frame (voicedAt (64.2)).empty());
        CHECK_FALSE (rig.segmenter.isProvisional());
        CHECK (rig.noteOns().size() == 1);
        CHECK (rig.noteOffs().empty());
    }

    SECTION ("while settling, a candidate inside the stickiness hysteresis keeps the provisional note")
    {
        SegmenterRig rig (s); // h = 0.4 at 100 %: a change needs |p - q| >= 0.9
        rig.segmenter.setOnsetTiming (rig.hop, SegmenterRig::provisionalTimeoutHops * static_cast<int64_t> (rig.hop),
                                      4 * static_cast<int64_t> (rig.hop));
        // (64.3: a fast pitch within fastNoteMarginSemitones of 64.5 would not start a note at all)
        rig.onsetFrame (voicedAt (64.3));
        rig.frame (unvoiced (-20.0f));
        rig.onsetFrame (voicedAt (64.3));
        REQUIRE (rig.segmenter.getActiveNote() == 64);
        rig.frame (unvoiced (-20.0f));
        CHECK (rig.frame (voicedAt (64.6)).empty());
        CHECK (rig.segmenter.getActiveNote() == 64);
        CHECK (rig.segmenter.isProvisional()); // the nearest note decides after the settle time (test below)
    }
}

TEST_CASE ("NoteSegmenter provisional note: a false start ends at the timeout, then the fast path waits for a gap",
           "[notesegmenter][fastonset]")
{
    VoiceSettings s;
    s.minNoteLengthMs = 250.0f; // a false start ignores minNoteLength
    SegmenterRig rig (s);

    rig.onsetFrame (voicedAt (64.0));
    rig.frame (unvoiced (-20.0f));
    rig.onsetFrame (voicedAt (64.0));
    REQUIRE (rig.noteOns().size() == 1);
    const auto start = rig.noteOns()[0].position;

    // The full window never sees the tone (loud, aperiodic frames); fast frames keep reading it
    rig.hops (voicedAt (64.0), unvoiced (-20.0f), SegmenterRig::provisionalTimeoutHops + 1);
    REQUIRE (rig.noteOffs().size() == 1);
    const auto age = rig.noteOffs()[0].position - start;
    CHECK (age >= SegmenterRig::provisionalTimeoutHops * rig.hop);
    CHECK (age < (SegmenterRig::provisionalTimeoutHops + 1) * rig.hop);

    // Blocked: neither the same tone nor non-strong but loud frames re-arm the fast path
    rig.hops (voicedAt (64.0), unvoiced (-20.0f), 5);
    rig.hops (unvoiced (-20.0f), unvoiced (-20.0f), 2);
    rig.hops (voicedAt (64.0), unvoiced (-20.0f), 5);
    CHECK (rig.noteOns().size() == 1);

    // A gap below the gate unblocks it
    rig.hops (unvoiced (-120.0f), unvoiced (-120.0f), 1);
    rig.hops (voicedAt (62.0), unvoiced (-20.0f), 2);
    CHECK (rig.noteOns().size() == 2);
    CHECK (rig.segmenter.getActiveNote() == 62);
    rig.flush();
    checkMidiInvariants (rig.events);
}

TEST_CASE ("NoteSegmenter provisional note: while settling only a clear error is corrected, then the nearest note",
           "[notesegmenter][fastonset]")
{
    // v0.2.1 review (scoops): until the full window no longer holds audio from before the start
    // (settle time), it lags behind the fast frames; the first strong frame after that decides with
    // the nearest note instead of 2 h (a mid-scoop 64.6 used to keep the 64 for T_min + minNoteLength).
    constexpr int settleHops = 4;
    const auto legato = GENERATE (false, true);
    CAPTURE (legato);
    VoiceSettings s;
    s.legato = legato;
    s.minNoteLengthMs = 250.0f; // corrections ignore minNoteLength and T_min
    REQUIRE (NoteSegmenter::provisionalSettleMarginSemitones < 0.05); // 64.55 after settling corrects

    SegmenterRig rig (s); // h = 0.2 at 50 %: a normal change needs |p - q| >= 0.7
    rig.segmenter.setOnsetTiming (rig.hop, SegmenterRig::provisionalTimeoutHops * static_cast<int64_t> (rig.hop),
                                  settleHops * static_cast<int64_t> (rig.hop));
    rig.onsetFrame (voicedAt (64.3));
    rig.frame (unvoiced (-20.0f));
    rig.onsetFrame (voicedAt (64.3));
    REQUIRE (rig.segmenter.getActiveNote() == 64);
    REQUIRE (rig.segmenter.isProvisional());
    const auto start = rig.events.back().position;

    SECTION ("a mid-scoop reading while settling keeps the note provisional; the first frame after settling corrects it")
    {
        // Frames at start + hop / 2 + k hop: settling while k hop + hop / 2 < settleHops hop
        rig.frames (voicedAt (64.6), settleHops);
        CHECK (rig.noteOns().size() == 1);
        CHECK (rig.segmenter.isProvisional());
        REQUIRE (rig.now() - start >= settleHops * rig.hop);

        const auto position = rig.now();
        const auto produced = rig.frame (voicedAt (64.55));
        REQUIRE (produced.size() == 2);
        CHECK (produced[legato ? 0 : 1].type == VsEventType::noteOn);
        CHECK (produced[legato ? 0 : 1].data1 == 65);
        CHECK (rig.events.back().position == position);
        CHECK_FALSE (rig.segmenter.isProvisional());

        // A normal note from now on: back to 64 only with 2 h, T_min and minNoteLength
        rig.framesFor (voicedAt (64.4), 400.0);
        CHECK (rig.segmenter.getActiveNote() == 65);
    }

    SECTION ("a clear error while settling is corrected at once")
    {
        rig.frame (voicedAt (52.0)); // half a hop after the onset: ignored
        CHECK (rig.noteOns().size() == 1);
        const auto produced = rig.frame (voicedAt (52.0));
        REQUIRE (produced.size() == 2);
        CHECK (rig.segmenter.getActiveNote() == 52);
        CHECK_FALSE (rig.segmenter.isProvisional());

        rig.framesFor (voicedAt (52.6), 100.0); // normal note: no nearest-note correction any more
        CHECK (rig.segmenter.getActiveNote() == 52);
    }

    SECTION ("the nearest note after settling is the same note: confirmed, nothing sent")
    {
        rig.frames (voicedAt (64.6), settleHops);
        CHECK (rig.frame (voicedAt (64.45)).empty());
        CHECK_FALSE (rig.segmenter.isProvisional());
        CHECK (rig.segmenter.getActiveNote() == 64);
    }

    SECTION ("a note seen strong while settling is not a false start at the timeout")
    {
        rig.frames (voicedAt (64.2), 2);
        rig.frames (unvoiced (-20.0f), SegmenterRig::provisionalTimeoutHops);
        CHECK_FALSE (rig.segmenter.isProvisional());
        CHECK_FALSE (rig.segmenter.isFastPathBlocked());
        CHECK (rig.noteOffs().empty()); // minNoteLength (250 ms) now applies to the release
        rig.framesFor (unvoiced (-20.0f), 300.0);
        CHECK (rig.noteOffs().size() == 1);
    }

    rig.flush();
    checkMidiInvariants (rig.events);
}

TEST_CASE ("NoteSegmenter provisional note: after a false start, a frame below the gate or a full-window note unblocks the fast path",
           "[notesegmenter][fastonset]")
{
    // v0.2.1 review: with gate - gateHysteresisDb as the gap, a noise floor just below the gate (the
    // usual setup) kept the fast path off for the whole session after one short blip.
    VoiceSettings s;
    s.minNoteLengthMs = 20.0f;
    SegmenterRig rig (s);
    const auto gate = rig.settings.gateThresholdDb;

    const auto falseStart = [&rig]
    {
        rig.onsetFrame (voicedAt (64.0));
        rig.frame (unvoiced (-20.0f));
        rig.onsetFrame (voicedAt (64.0));
        rig.hops (voicedAt (64.0), unvoiced (-20.0f), SegmenterRig::provisionalTimeoutHops + 1);
    };

    falseStart();
    REQUIRE (rig.noteOffs().size() == 1);
    CHECK (rig.segmenter.isFastPathBlocked());

    SECTION ("noise between gate - 3 dB and the gate")
    {
        rig.hops (voicedAt (64.0, 0.99f, gate + 0.5f), unvoiced (gate - 1.0f), 3); // at or above the gate: still blocked
        CHECK (rig.segmenter.isFastPathBlocked());
        rig.hops (unvoiced (gate - 1.0f), unvoiced (gate - 1.0f), 1);
        CHECK_FALSE (rig.segmenter.isFastPathBlocked());
        rig.hops (voicedAt (62.0), unvoiced (-20.0f), 2);
        CHECK (rig.noteOns().size() == 2);
        CHECK (rig.segmenter.isProvisional());
    }

    SECTION ("a note started by the full window")
    {
        rig.hops (voicedAt (62.0), voicedAt (62.0), 3); // fast frames blocked, the full window starts it
        REQUIRE (rig.noteOns().size() == 2);
        CHECK_FALSE (rig.segmenter.isProvisional());
        CHECK_FALSE (rig.segmenter.isFastPathBlocked());
        rig.hops (unvoiced (-20.0f), unvoiced (-20.0f), NoteSegmenter::releaseFrames + 15);
        REQUIRE (rig.noteOffs().size() == 2);
        rig.hops (voicedAt (60.0), unvoiced (-20.0f), 2);
        CHECK (rig.noteOns().size() == 3);
        CHECK (rig.segmenter.isProvisional());
    }

    SECTION ("while blocked, an RMS-only frame gives the same result as the full fast frame")
    {
        // VoicePipeline skips the fast search while isFastPathBlocked() (frame with rmsDb only)
        for (const auto level : { gate - 4.0f, gate - 1.0f, gate, -20.0f })
        {
            CAPTURE (level);
            SegmenterRig a (s), b (s);

            for (auto* r : { &a, &b })
            {
                r->onsetFrame (voicedAt (64.0));
                r->frame (unvoiced (-20.0f));
                r->onsetFrame (voicedAt (64.0));
                r->hops (voicedAt (64.0), unvoiced (-20.0f), SegmenterRig::provisionalTimeoutHops + 1);
                REQUIRE (r->segmenter.isFastPathBlocked());
            }

            a.onsetFrame (voicedAt (64.0, 0.99f, level));
            b.onsetFrame (unvoiced (level));
            CHECK (a.segmenter.isFastPathBlocked() == b.segmenter.isFastPathBlocked());
            a.hops (voicedAt (62.0), unvoiced (-20.0f), 2);
            b.hops (voicedAt (62.0), unvoiced (-20.0f), 2);
            CHECK (a.events == b.events);
        }
    }

    rig.flush();
    checkMidiInvariants (rig.events);
}

TEST_CASE ("NoteSegmenter provisional note: weak voiced at the timeout continues as a normal note", "[notesegmenter][fastonset]")
{
    VoiceSettings s;
    s.minNoteLengthMs = 20.0f;
    SegmenterRig rig (s);

    rig.onsetFrame (voicedAt (64.0));
    rig.frame (unvoiced (-20.0f));
    rig.onsetFrame (voicedAt (64.0));
    rig.frames (voicedAt (64.0, 0.7f), SegmenterRig::provisionalTimeoutHops + 5);
    CHECK (rig.noteOffs().empty());
    CHECK_FALSE (rig.segmenter.isProvisional());

    rig.frames (unvoiced (-20.0f), NoteSegmenter::releaseFrames);
    CHECK (rig.noteOffs().size() == 1);
    checkMidiInvariants (rig.events);
}

TEST_CASE ("NoteSegmenter provisional onset: re-armed only by a non-strong fast frame after a note", "[notesegmenter][fastonset]")
{
    VoiceSettings s;
    s.minNoteLengthMs = 20.0f;
    SegmenterRig rig (s);

    // A note started and released by the full window while the fast frames stay strong
    rig.hops (voicedAt (60.0, 0.99f), voicedAt (60.0), 10);
    REQUIRE (rig.noteOns().size() == 1);
    CHECK_FALSE (rig.segmenter.isProvisional());
    rig.hops (voicedAt (60.0, 0.99f), unvoiced (-20.0f), NoteSegmenter::releaseFrames + 5);
    REQUIRE (rig.noteOffs().size() == 1);
    CHECK (rig.noteOns().size() == 1); // not armed since the note start

    rig.hops (unvoiced (-20.0f), unvoiced (-20.0f), 1);
    rig.hops (voicedAt (60.0, 0.99f), unvoiced (-20.0f), 2);
    CHECK (rig.noteOns().size() == 2);
    CHECK (rig.segmenter.isProvisional());
    rig.flush();
    checkMidiInvariants (rig.events);
}

TEST_CASE ("NoteSegmenter provisional onset: octave shift and unplayable notes", "[notesegmenter][fastonset]")
{
    VoiceSettings s;
    s.octaveShift = 2;
    SegmenterRig rig (s);
    rig.onsetFrame (voicedAt (60.0));
    rig.onsetFrame (voicedAt (60.0));
    REQUIRE (rig.noteOns().size() == 1);
    CHECK (rig.noteOns()[0].event.data1 == 84);
    CHECK (rig.segmenter.getQuantisedNote() == 60);

    s.octaveShift = 3;
    SegmenterRig high (s);
    high.onsetFrame (voicedAt (120.0));
    high.onsetFrame (voicedAt (120.0));
    CHECK (high.events.empty()); // 120 + 36 > 127
}

TEST_CASE ("NoteSegmenter provisional state is cleared by flush and reset", "[notesegmenter][fastonset]")
{
    SegmenterRig rig;
    rig.onsetFrame (voicedAt (60.0));
    rig.onsetFrame (voicedAt (60.0));
    REQUIRE (rig.segmenter.isProvisional());
    rig.flush();
    CHECK_FALSE (rig.segmenter.isProvisional());
    CHECK (rig.segmenter.getActiveNote() == -1);

    // After a flush (e.g. a range change while singing) the fast path may restart the note at once
    rig.frame (unvoiced (-20.0f));
    rig.onsetFrame (voicedAt (60.0));
    rig.onsetFrame (voicedAt (60.0));
    CHECK (rig.noteOns().size() == 2);
    rig.flush();
    checkMidiInvariants (rig.events);
}

// ---------------------------------------------------------------------------------------------------------
// M3 detection-delay side output (docs/M3-DESIGN.md 7): getLastEventTiming() of every call, never a change
// of the events.

namespace
{
int64_t fixedFastDelay (const void*, double) noexcept { return 1500; }

NoteSegmenter::DetectionDelayModel testDelayModel()
{
    NoteSegmenter::DetectionDelayModel m;
    m.frameDelaySamples = 500.0;
    m.confirmedOnsetDelaySamples = 2000;
    m.fastOnsetDelay = &fixedFastDelay;
    m.onsetSearchSamples = 3000;
    return m;
}

/** Estimated sample of a timing for an event emitted at position. */
int64_t estimateOf (int64_t position, const VsEventTiming& t) { return position - t.delaySamples; }
} // namespace

TEST_CASE ("NoteSegmenter timing: onset from the level crossing or the model, release, change, bends, flush",
           "[notesegmenter][timing]")
{
    VoiceSettings s;
    s.bendMode = BendMode::trueBend;
    s.stickinessPercent = 0.0f;
    SegmenterRig rig (s);
    rig.segmenter.setDetectionDelayModel (testDelayModel());
    const auto& t = rig.segmenter.getLastEventTiming();

    SECTION ("full-window start: level onset within the search span, else the confirmed model")
    {
        rig.frames (unvoiced(), 50);
        auto onsetAt = rig.now() - 1000;
        rig.segmenter.setLevelOnsetSample (onsetAt);
        auto position = rig.now();
        rig.frame (voicedAt (60.0));
        REQUIRE (rig.noteOns().size() == 1);
        CHECK (t.noteOn == VsEventTiming { 1000, EventTimingKind::measured, 0 });
        CHECK (t.noteOff == VsEventTiming {});

        // Unvoiced frames: the release run starts at the first one; minus the frame delay
        rig.frames (voicedAt (60.0), 100);
        const auto releaseRun = rig.now();
        rig.frames (unvoiced(), NoteSegmenter::releaseFrames - 1);
        position = rig.now();
        rig.frame (unvoiced());
        REQUIRE (rig.noteOffs().size() == 1);
        CHECK (t.noteOff.kind == EventTimingKind::measured);
        CHECK (estimateOf (position, t.noteOff) == releaseRun - 500);

        // The old onset lies before the previous note's end: no longer used -> model
        rig.frames (unvoiced(), 20);
        position = rig.now();
        rig.frame (voicedAt (62.0));
        REQUIRE (rig.noteOns().size() == 2);
        CHECK (t.noteOn == VsEventTiming { 2000, EventTimingKind::model, 0 });

        // A level release after the note start wins over the frame estimate
        rig.frames (voicedAt (62.0), 100);
        const auto releaseAt = rig.now() + 7;
        rig.segmenter.setLevelReleaseSample (releaseAt);
        rig.frames (unvoiced(), NoteSegmenter::releaseFrames - 1);
        position = rig.now();
        rig.frame (unvoiced());
        REQUIRE (rig.noteOffs().size() == 2);
        CHECK (estimateOf (position, t.noteOff) == releaseAt);

        // A level onset too long before the note-on: model
        rig.frames (unvoiced(), 50);
        onsetAt = rig.now() - 3001;
        rig.segmenter.setLevelOnsetSample (onsetAt);
        rig.frame (voicedAt (64.0));
        CHECK (t.noteOn == VsEventTiming { 2000, EventTimingKind::model, 0 });
    }

    SECTION ("note change: change-timer start minus the frame delay, flag noteChange, old note ends there")
    {
        rig.frames (unvoiced(), 10);
        rig.frame (voicedAt (60.0));
        rig.framesFor (voicedAt (60.0), 300.0);
        const auto changeStart = rig.now();
        std::vector<VsEvent> produced;
        int64_t position = 0;

        for (int i = 0; i < 100 && produced.empty(); ++i)
        {
            position = rig.now();
            for (const auto& e : rig.frame (voicedAt (62.0)))
                if (e.type == VsEventType::noteOn)
                    produced.push_back (e);
        }

        REQUIRE (produced.size() == 1);
        CHECK (t.noteOn.kind == EventTimingKind::measured);
        CHECK (t.noteOn.flags == EventTimingFlags::noteChange);
        CHECK (estimateOf (position, t.noteOn) == changeStart - 500);
        CHECK (t.noteOff == t.noteOn);
        CHECK (t.update == VsEventTiming {}); // the change's bends belong to the note start / end
    }

    SECTION ("held-note bend updates: emit minus the frame delay; flush: kind none")
    {
        rig.frames (unvoiced(), 10);
        rig.frame (voicedAt (60.0));
        auto sawUpdate = false;

        for (int i = 0; i < 50; ++i)
        {
            const auto produced = rig.frame (voicedAt (60.0 + 0.2 * std::sin (i * 0.5)));

            if (! produced.empty() && produced[0].type == VsEventType::pitchBend)
            {
                CHECK (t.update == VsEventTiming { 500, EventTimingKind::measured, 0 });
                CHECK (t.noteOn == VsEventTiming {});
                sawUpdate = true;
            }
        }

        CHECK (sawUpdate);
        rig.flush();
        CHECK (rig.noteOffs().size() == 1);
        CHECK (t.noteOff == VsEventTiming {});
        CHECK (t.noteOn == VsEventTiming {});
        CHECK (t.update == VsEventTiming {});
    }
}

TEST_CASE ("NoteSegmenter timing: provisional start (fast model), correction and false start", "[notesegmenter][timing]")
{
    VoiceSettings s;
    s.minNoteLengthMs = 250.0f;
    s.stickinessPercent = 100.0f;

    SECTION ("provisional start without a level onset: the fast model at the note's pitch")
    {
        SegmenterRig rig (s);
        rig.segmenter.setDetectionDelayModel (testDelayModel());
        const auto& t = rig.segmenter.getLastEventTiming();
        rig.onsetFrame (voicedAt (64.0));
        rig.frame (unvoiced (-20.0f));
        rig.onsetFrame (voicedAt (64.0));
        REQUIRE (rig.segmenter.isProvisional());
        CHECK (t.noteOn == VsEventTiming { 1500, EventTimingKind::model, 0 });

        // The full window corrects it: both events of the correction carry the provisional delay + flag
        rig.frame (voicedAt (52.0));
        const auto produced = rig.frame (voicedAt (52.0));
        REQUIRE (produced.size() == 2);
        CHECK (t.noteOn == VsEventTiming { 1500, EventTimingKind::model, EventTimingFlags::correction });
        CHECK (t.noteOff == t.noteOn);
    }

    SECTION ("provisional start with a level onset; a false start's note-off gets the note-on's delay")
    {
        SegmenterRig rig (s);
        rig.segmenter.setDetectionDelayModel (testDelayModel());
        const auto& t = rig.segmenter.getLastEventTiming();
        rig.onsetFrame (voicedAt (64.0));
        rig.frame (unvoiced (-20.0f));
        rig.segmenter.setLevelOnsetSample (rig.now() - rig.hop / 2 - 300);
        rig.onsetFrame (voicedAt (64.0));
        REQUIRE (rig.segmenter.isProvisional());
        CHECK (t.noteOn == VsEventTiming { 300, EventTimingKind::measured, 0 });

        // Nothing confirms it: the false start ends at the timeout
        for (int i = 0; i < 2 * SegmenterRig::provisionalTimeoutHops && rig.noteOffs().empty(); ++i)
            rig.frame (unvoiced (-120.0f));

        REQUIRE (rig.noteOffs().size() == 1);
        CHECK (t.noteOff == VsEventTiming { 300, EventTimingKind::measured, 0 });
    }

    SECTION ("the timing never changes the events")
    {
        SegmenterRig a (s);
        SegmenterRig b (s);
        b.segmenter.setDetectionDelayModel (testDelayModel());

        for (auto* rig : { &a, &b })
        {
            rig->onsetFrame (voicedAt (64.0));
            rig->frame (unvoiced (-20.0f));
            rig->segmenter.setLevelOnsetSample (rig->now() - 50);
            rig->hops (voicedAt (64.0), voicedAt (52.0), 40);
            rig->segmenter.setLevelReleaseSample (rig->now());
            rig->frames (unvoiced(), 40);
            rig->flush();
        }

        CHECK (a.events == b.events);
        CHECK (! a.events.empty());
    }
}
