// VoiceAllocator: invariants under random sequences, steal order, last-note priority, deferred note-off, correction
// rule, Poly pairing, glide flags, realistic VoicePipeline event streams.
// Test plan: docs/SYNTH-DESIGN.md 21. Owner: agent B (Voice).

#include "vocalsynth/synth/VoiceAllocator.h"

#include "../TestSignals.h"
#include "vocalsynth/VoicePipeline.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <map>
#include <vector>

using namespace vocalsynth;
using namespace vocalsynth::synth;

namespace
{

using Type = VoiceCommandType;

NoteInput on (int note, int velocity = 100, bool correction = false)
{
    return { true, static_cast<uint8_t> (note), static_cast<uint8_t> (velocity), correction };
}

NoteInput off (int note, bool correction = false)
{
    return { false, static_cast<uint8_t> (note), 0, correction };
}

AllocatorSettings mode (PlayMode m, bool glide = false, GlideMode g = GlideMode::legatoOnly)
{
    return { m, g, glide };
}

/** A minimal voice model for the tests: a voice is active from a note command until `releaseSamples` after
    its release (the engine passes this activity to every group). */
struct Harness
{
    VoiceAllocator allocator;
    AllocatorSettings settings;
    std::array<bool, VoiceAllocator::maxVoices> active {};
    std::array<bool, VoiceAllocator::maxVoices> held {};
    std::array<int, VoiceAllocator::maxVoices> note {};
    std::array<int64_t, VoiceAllocator::maxVoices> releasedAt {};
    int64_t releaseSamples = 4800;
    std::vector<VoiceCommand> all;

    explicit Harness (AllocatorSettings s, double rate = 48000.0) : settings (s)
    {
        allocator.prepare (rate);
        note.fill (-1);
        releasedAt.fill (-1);
    }

    void age (int64_t t)
    {
        for (std::size_t v = 0; v < active.size(); ++v)
            if (active[v] && ! held[v] && releasedAt[v] >= 0 && t - releasedAt[v] >= releaseSamples)
                active[v] = false;
    }

    void apply (const VoiceCommand& c, int64_t t)
    {
        REQUIRE (c.voice < VoiceAllocator::maxVoices);
        const auto v = c.voice;

        if (c.type == Type::release)
        {
            CHECK (held[v]); // a release only for a voice that holds a note
            held[v] = false;
            releasedAt[v] = t;
            return;
        }

        if (c.type == Type::legato || c.type == Type::correction)
            CHECK (active[v]); // only on a sounding voice

        active[v] = true;
        held[v] = true;
        note[v] = c.note;
        releasedAt[v] = -1;
    }

    std::vector<VoiceCommand> group (std::initializer_list<NoteInput> events, int64_t t)
    {
        return group (std::vector<NoteInput> (events), t);
    }

    std::vector<VoiceCommand> group (const std::vector<NoteInput>& events, int64_t t)
    {
        age (t);
        VoiceCommandList out;
        allocator.processGroup (events, t, settings, active, out);
        REQUIRE (out.size() <= VoiceCommandList::capacity);
        std::vector<VoiceCommand> cmds (out.view().begin(), out.view().end());

        for (const auto& c : cmds)
        {
            apply (c, t);
            all.push_back (c);
        }

        return cmds;
    }

    std::vector<VoiceCommand> releaseAll()
    {
        VoiceCommandList out;
        allocator.releaseAll (out);
        std::vector<VoiceCommand> cmds (out.view().begin(), out.view().end());

        for (const auto& c : cmds)
            apply (c, 0);

        return cmds;
    }

    int heldVoices() const { return static_cast<int> (std::count (held.begin(), held.end(), true)); }
};

} // namespace

TEST_CASE ("VoiceAllocator: Mono / Legato basics on voice 0", "[synth][allocator]")
{
    const auto m = GENERATE (PlayMode::mono, PlayMode::legato);
    Harness h (mode (m));

    auto c = h.group ({ on (60, 90) }, 0);
    REQUIRE (c.size() == 1);
    CHECK (c[0].type == Type::start);
    CHECK (c[0].voice == 0);
    CHECK (c[0].note == 60);
    CHECK (c[0].velocity == 90);
    CHECK_FALSE (c[0].glide);
    CHECK (h.allocator.getNumHeldNotes() == 1);

    c = h.group ({ off (60) }, 10000);
    REQUIRE (c.size() == 1);
    CHECK (c[0].type == Type::release);
    CHECK (h.allocator.getNumHeldNotes() == 0);

    // Note-on while releasing: a start (not connected)
    c = h.group ({ on (62) }, 11000);
    REQUIRE (c.size() == 1);
    CHECK (c[0].type == Type::start);

    // Unknown note-off: ignored
    c = h.group ({ off (70) }, 20000);
    CHECK (c.empty());
}

TEST_CASE ("VoiceAllocator: both VoicePipeline change orders are connected changes", "[synth][allocator]")
{
    const auto m = GENERATE (PlayMode::mono, PlayMode::legato);
    const auto legatoOrder = GENERATE (false, true);
    CAPTURE (static_cast<int> (m), legatoOrder);
    Harness h (mode (m, true));

    h.group ({ on (60) }, 0);
    const auto t = int64_t { 48000 };
    const auto c = legatoOrder ? h.group ({ on (64), off (60) }, t) : h.group ({ off (60), on (64) }, t);

    // One command, no release: Legato -> legato, Mono -> start (retrigger), both with glide (connected)
    REQUIRE (c.size() == 1);
    CHECK (c[0].type == (m == PlayMode::legato ? Type::legato : Type::start));
    CHECK (c[0].voice == 0);
    CHECK (c[0].note == 64);
    CHECK (c[0].glide);
    CHECK (c[0].glideFromNote == -1);
    CHECK (h.allocator.getNumHeldNotes() == 1);

    const auto r = h.group ({ off (64) }, 2 * t);
    REQUIRE (r.size() == 1);
    CHECK (r[0].type == Type::release);
}

TEST_CASE ("VoiceAllocator: Mono / Legato last-note priority", "[synth][allocator]")
{
    Harness h (mode (PlayMode::legato));
    h.group ({ on (60) }, 0);
    h.group ({ on (64) }, 10000);
    auto c = h.group ({ on (67) }, 20000);
    REQUIRE (c.size() == 1);
    CHECK (c[0].type == Type::legato);
    CHECK (c[0].note == 67);
    CHECK (h.allocator.getNumHeldNotes() == 3);

    // Releasing a non-sounding held note changes nothing
    c = h.group ({ off (60) }, 30000);
    CHECK (c.empty());

    // Releasing the sounding note goes back to the newest held note (connected, never a correction)
    c = h.group ({ off (67) }, 30500);
    REQUIRE (c.size() == 1);
    CHECK (c[0].type == Type::legato);
    CHECK (c[0].note == 64);

    c = h.group ({ off (64) }, 40000);
    REQUIRE (c.size() == 1);
    CHECK (c[0].type == Type::release);
    CHECK (h.allocator.getNumHeldNotes() == 0);

    // Mono: the return is a retrigger (start) with the held note's own velocity
    Harness mono (mode (PlayMode::mono));
    mono.group ({ on (50, 40) }, 0);
    mono.group ({ on (55, 120) }, 10000);
    c = mono.group ({ off (55) }, 20000);
    REQUIRE (c.size() == 1);
    CHECK (c[0].type == Type::start);
    CHECK (c[0].note == 50);
    CHECK (c[0].velocity == 40);
}

TEST_CASE ("VoiceAllocator: correction rule at 39 / 41 ms and with the flag", "[synth][allocator]")
{
    const auto m = GENERATE (PlayMode::mono, PlayMode::legato, PlayMode::poly);
    const auto rate = GENERATE (44100.0, 48000.0, 96000.0);
    const auto legatoOrder = GENERATE (false, true);
    CAPTURE (static_cast<int> (m), rate, legatoOrder);

    const auto change = [&] (Harness& h, int64_t t, int from, int to, bool flag)
    {
        return legatoOrder ? h.group ({ on (to, 100, flag), off (from, flag) }, t)
                           : h.group ({ off (from, flag), on (to, 100, flag) }, t);
    };

    const auto ms = [rate] (double v) { return static_cast<int64_t> (v * 0.001 * rate); };
    const int64_t start = 1000;

    {
        Harness h (mode (m, true, GlideMode::always), rate);
        h.group ({ on (60, 80) }, start);
        const auto c = change (h, start + ms (39.0), 60, 61, false);
        REQUIRE (c.size() == 1);
        CHECK (c[0].type == Type::correction);
        CHECK (c[0].voice == 0);
        CHECK (c[0].note == 61);
        CHECK_FALSE (c[0].glide);
        CHECK (h.heldVoices() == 1);

        // The corrected note keeps the original start: a change 45 ms after it is a normal change
        const auto d = change (h, start + ms (45.0), 61, 63, false);
        CHECK (std::none_of (d.begin(), d.end(), [] (const VoiceCommand& x) { return x.type == Type::correction; }));
    }
    {
        Harness h (mode (m, true, GlideMode::always), rate);
        h.group ({ on (60) }, start);
        const auto c = change (h, start + ms (41.0), 60, 61, false);
        CHECK (std::none_of (c.begin(), c.end(), [] (const VoiceCommand& x) { return x.type == Type::correction; }));

        if (m == PlayMode::poly)
        {
            // A new voice for the new note, the old one releases (overlapping tails)
            REQUIRE (c.size() == 2);
            const auto& s = c[legatoOrder ? 0 : 1];
            const auto& r = c[legatoOrder ? 1 : 0];
            CHECK (s.type == Type::start);
            CHECK (s.voice == 1);
            CHECK (s.glide);
            CHECK (s.glideFromNote == 60);
            CHECK (r.type == Type::release);
            CHECK (r.voice == 0);
        }
        else
        {
            REQUIRE (c.size() == 1);
            CHECK (c[0].glide);
        }
    }
    {
        // The flag extends the window
        Harness h (mode (m, true), rate);
        h.group ({ on (60) }, start);
        const auto c = change (h, start + ms (300.0), 60, 59, true);
        REQUIRE (c.size() == 1);
        CHECK (c[0].type == Type::correction);
        CHECK (c[0].note == 59);
        CHECK (h.heldVoices() == 1);
        const auto r = h.group ({ off (59) }, start + ms (500.0));
        REQUIRE (r.size() == 1);
        CHECK (r[0].type == Type::release);
        CHECK (h.heldVoices() == 0);
    }
}

TEST_CASE ("VoiceAllocator: glide flags", "[synth][allocator]")
{
    SECTION ("Legato Only: connected changes only")
    {
        Harness h (mode (PlayMode::legato, true, GlideMode::legatoOnly));
        CHECK_FALSE (h.group ({ on (60) }, 0)[0].glide);
        CHECK (h.group ({ on (62) }, 10000)[0].glide);
        h.group ({ off (62), off (60) }, 20000);
        CHECK_FALSE (h.group ({ on (64) }, 30000)[0].glide); // from release: not connected
    }

    SECTION ("Always: every start once a previous note exists")
    {
        Harness h (mode (PlayMode::mono, true, GlideMode::always));
        CHECK_FALSE (h.group ({ on (60) }, 0)[0].glide); // first note since reset
        h.group ({ off (60) }, 10000);
        CHECK (h.group ({ on (64) }, 200000)[0].glide); // also from silence
        h.allocator.reset();
        CHECK_FALSE (h.group ({ on (66) }, 300000)[0].glide);
    }

    SECTION ("Glide off: never")
    {
        Harness h (mode (PlayMode::legato, false, GlideMode::always));
        h.group ({ on (60) }, 0);
        CHECK_FALSE (h.group ({ on (62) }, 10000)[0].glide);
    }

    SECTION ("Poly: from the most recent note-on's note")
    {
        Harness h (mode (PlayMode::poly, true, GlideMode::always));
        const auto first = h.group ({ on (60) }, 0);
        CHECK_FALSE (first[0].glide);
        CHECK (first[0].glideFromNote == -1);
        h.group ({ off (60) }, 5000);
        const auto second = h.group ({ on (67) }, 100000);
        CHECK (second[0].glide);
        CHECK (second[0].glideFromNote == 60);

        Harness l (mode (PlayMode::poly, true, GlideMode::legatoOnly));
        l.group ({ on (60) }, 0);
        CHECK (l.group ({ on (64) }, 10000)[0].glide); // 60 still held: connected
        l.group ({ off (60), off (64) }, 20000);
        CHECK_FALSE (l.group ({ on (65) }, 100000)[0].glide);
    }
}

TEST_CASE ("VoiceAllocator: Poly chord, retrigger of the same note, lowest free voice", "[synth][allocator]")
{
    Harness h (mode (PlayMode::poly));
    const auto c = h.group ({ on (60), on (64), on (67) }, 0); // a chord without pairs: three starts
    REQUIRE (c.size() == 3);

    for (std::size_t i = 0; i < 3; ++i)
    {
        CHECK (c[i].type == Type::start);
        CHECK (c[i].voice == i);
    }

    // The same note again (held or releasing) restarts its own voice
    auto r = h.group ({ on (64) }, 100000);
    REQUIRE (r.size() == 1);
    CHECK (r[0].voice == 1);
    h.group ({ off (64) }, 110000);
    r = h.group ({ on (64) }, 111000);
    REQUIRE (r.size() == 1);
    CHECK (r[0].type == Type::start);
    CHECK (r[0].voice == 1);

    // A voice whose release ended is free again: the lowest one is taken
    h.group ({ off (60) }, 120000);
    h.age (120000 + h.releaseSamples);
    r = h.group ({ on (70) }, 120000 + h.releaseSamples);
    REQUIRE (r.size() == 1);
    CHECK (r[0].voice == 0);
}

TEST_CASE ("VoiceAllocator: Poly steal order and a second steal during the fade", "[synth][allocator]")
{
    Harness h (mode (PlayMode::poly));
    h.releaseSamples = 1 << 30; // releases never end in this test

    for (int i = 0; i < 16; ++i)
        h.group ({ on (40 + i) }, i * 10);

    // Releases started at different times: the one that started first is stolen first
    h.group ({ off (45) }, 1000);
    h.group ({ off (43) }, 1100);
    auto c = h.group ({ on (80) }, 2000);
    REQUIRE (c.size() == 1);
    CHECK (c[0].type == Type::steal);
    CHECK (c[0].voice == 5);
    c = h.group ({ on (81) }, 2010);
    CHECK (c[0].voice == 3);

    // No voice in release: the oldest note-on (voice 0, then 1, ...)
    c = h.group ({ on (82) }, 2020);
    CHECK (c[0].type == Type::steal);
    CHECK (c[0].voice == 0);

    // Steal every other voice once, then voice 0 again within the 2 ms fade: note 82 loses its voice
    for (int i = 0; i < 15; ++i)
        h.group ({ on (90 + i) }, 2030 + i);

    c = h.group ({ on (110) }, 2050);
    REQUIRE (c.size() == 1);
    CHECK (c[0].type == Type::steal);
    CHECK (c[0].voice == 0);

    // The replaced note's note-off is ignored; the new one's releases its voice
    CHECK (h.group ({ off (82) }, 2100).empty());
    c = h.group ({ off (110) }, 2200);
    REQUIRE (c.size() == 1);
    CHECK (c[0].type == Type::release);
    CHECK (c[0].voice == 0);
}

TEST_CASE ("VoiceAllocator: Poly pairing", "[synth][allocator]")
{
    SECTION ("an unpaired note-on in a correction group is a normal start")
    {
        Harness h (mode (PlayMode::poly));
        h.group ({ on (60) }, 0);
        h.group ({ on (64) }, 10);
        // 2 note-ons, 1 note-off within the window: the first note-on pairs with the note-off
        const auto c = h.group ({ on (61), off (60), on (67) }, 500);
        REQUIRE (c.size() == 2);
        CHECK (c[0].type == Type::correction);
        CHECK (c[0].voice == 0);
        CHECK (c[0].note == 61);
        CHECK (c[1].type == Type::start);
        CHECK (c[1].voice == 2);
        CHECK (h.heldVoices() == 3);
    }

    SECTION ("pairs follow the event order, the earliest note-off first")
    {
        Harness h (mode (PlayMode::poly));
        h.group ({ on (60), on (64) }, 0);
        const auto c = h.group ({ off (64), off (60), on (65), on (61) }, 100);
        REQUIRE (c.size() == 2);
        CHECK (c[0].type == Type::correction);
        CHECK (c[0].voice == 1); // 65 takes 64's voice (the first note-off)
        CHECK (c[1].type == Type::correction);
        CHECK (c[1].voice == 0);
        CHECK (c[1].note == 61);
    }

    SECTION ("a note-off of a note without a voice does not pair")
    {
        Harness h (mode (PlayMode::poly));
        h.group ({ on (60) }, 0);
        const auto c = h.group ({ off (59), on (62) }, 100);
        REQUIRE (c.size() == 1);
        CHECK (c[0].type == Type::start);
        CHECK (c[0].voice == 1);
    }
}

TEST_CASE ("VoiceAllocator: releaseAll and getNewestVoice", "[synth][allocator]")
{
    Harness h (mode (PlayMode::poly));
    h.group ({ on (60), on (64), on (67) }, 0);
    h.group ({ off (64) }, 100);
    CHECK (h.allocator.getNewestVoice (h.active) == 2);

    const auto r = h.releaseAll();
    CHECK (r.size() == 2); // voices 0 and 2 still held; 1 is already releasing
    CHECK (h.heldVoices() == 0);
    CHECK (h.allocator.getNumHeldNotes() == 0);

    // Note-offs after releaseAll are unknown
    CHECK (h.group ({ off (60), off (67) }, 200).empty());

    // Newest voice that is still active
    std::array<bool, VoiceAllocator::maxVoices> act {};
    act[0] = true;
    CHECK (h.allocator.getNewestVoice (act) == 0);
    act.fill (false);
    CHECK (h.allocator.getNewestVoice (act) == -1);

    Harness mono (mode (PlayMode::legato));
    CHECK (mono.allocator.getNewestVoice (mono.active) == -1);
    mono.group ({ on (60) }, 0);
    CHECK (mono.allocator.getNewestVoice (mono.active) == 0);
    CHECK (mono.releaseAll().size() == 1);
    CHECK (mono.releaseAll().empty());
}

TEST_CASE ("VoiceAllocator: random event sequences keep every invariant", "[synth][allocator]")
{
    const auto m = GENERATE (PlayMode::mono, PlayMode::legato, PlayMode::poly);
    CAPTURE (static_cast<int> (m));
    uint32_t seed = 0x1234567u + static_cast<uint32_t> (m);
    const auto rnd = [&seed] (uint32_t n)
    {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        return seed % n;
    };

    Harness h (mode (m, rnd (2) == 0, GlideMode::always));
    std::array<bool, 128> pressed {};
    int64_t t = 0;

    for (int g = 0; g < 10000; ++g)
    {
        t += rnd (5) == 0 ? 0 : 1 + rnd (3000); // sometimes several groups very close together
        h.releaseSamples = 100 + rnd (5000);
        std::vector<NoteInput> events;
        const auto count = 1 + rnd (rnd (10) == 0 ? 40 : 4);

        for (uint32_t e = 0; e < count; ++e)
        {
            const auto n = static_cast<int> (36 + rnd (m == PlayMode::poly ? 30u : 16u)); // mono: within the 16-note stack
            const bool flag = rnd (8) == 0;
            events.push_back (rnd (2) == 0 ? on (n, 1 + static_cast<int> (rnd (127)), flag) : off (n, flag));
        }

        if (rnd (500) == 0)
        {
            h.releaseAll();
            pressed.fill (false);
        }

        const auto cmds = h.group (events, t);

        for (const auto& e : events)
            pressed[e.note] = e.isNoteOn;

        // Commands: Mono / Legato only voice 0, no steal; at most capacity
        for (const auto& c : cmds)
        {
            if (m != PlayMode::poly)
            {
                CHECK (c.voice == 0);
                CHECK (c.type != Type::steal);
            }
        }

        // No hanging voice: a voice holds a note only while that note is pressed
        int heldCount = 0;

        for (std::size_t v = 0; v < h.held.size(); ++v)
        {
            if (h.held[v])
            {
                ++heldCount;
                CAPTURE (g, v, h.note[v]);
                REQUIRE (pressed[static_cast<std::size_t> (h.note[v])]);
            }
        }

        CHECK (heldCount <= VoiceAllocator::maxVoices);

        // Mono / Legato: a pressed note means the voice sounds (some held note)
        if (m != PlayMode::poly && std::any_of (pressed.begin(), pressed.end(), [] (bool b) { return b; }))
            CHECK (heldCount == 1);
    }

    // Release everything that is still pressed: nothing hangs
    std::vector<NoteInput> offs;

    for (int n = 0; n < 128; ++n)
        if (pressed[static_cast<std::size_t> (n)])
            offs.push_back (off (n));

    for (std::size_t i = 0; i < offs.size(); i += 8)
        h.group (std::vector<NoteInput> (offs.begin() + static_cast<std::ptrdiff_t> (i),
                                         offs.begin() + static_cast<std::ptrdiff_t> (std::min (offs.size(), i + 8))),
                 t + 100000);

    CHECK (h.heldVoices() == 0);
    CHECK (h.allocator.getNumHeldNotes() == 0);
}

TEST_CASE ("VoiceAllocator: a huge group never exceeds the command capacity and leaves nothing hanging", "[synth][allocator]")
{
    Harness h (mode (PlayMode::poly));
    std::vector<NoteInput> events;

    for (int i = 0; i < 100; ++i)
        events.push_back (on (i));

    for (int i = 0; i < 50; ++i)
        events.push_back (off (i));

    const auto c = h.group (events, 0);
    CHECK (c.size() <= VoiceCommandList::capacity);

    std::vector<NoteInput> offs;

    for (int i = 0; i < 128; ++i)
        offs.push_back (off (i));

    for (std::size_t i = 0; i < offs.size(); i += 16)
        h.group (std::vector<NoteInput> (offs.begin() + static_cast<std::ptrdiff_t> (i),
                                         offs.begin() + static_cast<std::ptrdiff_t> (i + 16)),
                 1000);

    CHECK (h.heldVoices() == 0);
}

namespace
{

/** Runs a VoicePipeline over `input` and feeds its note events, grouped per absolute sample, with their
    timing flags into a harness. Returns the commands per type. */
std::map<Type, int> runPipeline (Harness& h, const test::Signal& input, const VoiceSettings& vs, int& corrections)
{
    VoicePipeline pipeline;
    pipeline.prepare (48000.0, 512);
    VsEventBuffer<1024> events;
    VsEventTimingBuffer<1024> timings;
    std::map<Type, int> counts;
    int64_t clock = 0;
    corrections = 0;

    for (std::size_t pos = 0; pos < input.size(); pos += 512)
    {
        const auto n = static_cast<int> (std::min<std::size_t> (512, input.size() - pos));
        events.clear();
        timings.clear();
        pipeline.process (input.data() + pos, n, vs, events, timings);
        std::size_t i = 0;

        while (i < events.size())
        {
            const auto offset = events[i].sampleOffset;
            std::vector<NoteInput> group;

            for (; i < events.size() && events[i].sampleOffset == offset; ++i)
            {
                const auto& e = events[i];

                if (e.type != VsEventType::noteOn && e.type != VsEventType::noteOff)
                    continue;

                const bool flag = i < timings.size() && (timings[i].flags & EventTimingFlags::correction) != 0;
                corrections += flag && e.type == VsEventType::noteOn ? 1 : 0;
                group.push_back (e.type == VsEventType::noteOn ? on (e.data1, e.data2, flag) : off (e.data1, flag));
            }

            if (! group.empty())
                for (const auto& c : h.group (group, clock + offset))
                    ++counts[c.type];
        }

        clock += n;
    }

    return counts;
}

} // namespace

TEST_CASE ("VoiceAllocator: realistic VoicePipeline event streams", "[synth][allocator]")
{
    const auto legato = GENERATE (false, true);
    const auto m = GENERATE (PlayMode::mono, PlayMode::legato, PlayMode::poly);
    CAPTURE (legato, static_cast<int> (m));

    using test::midiToHz;
    // Sung line: notes with legato pitch steps and rests (sawtooth, clearly voiced)
    const auto input = test::toneSequence (48000.0,
                                           { { midiToHz (57), 0.4 }, { midiToHz (60), 0.4 }, { midiToHz (64), 0.4 },
                                             { 0.0, 0.3 }, { midiToHz (62), 0.5 }, { midiToHz (59), 0.4 }, { 0.0, 0.5 },
                                             { midiToHz (55), 0.6 }, { midiToHz (57), 0.3 }, { 0.0, 0.5 } },
                                           0.4f, test::sawShape);
    VoiceSettings vs;
    vs.range = VoiceRange::tenor;
    vs.legato = legato;

    Harness h (mode (m, true));
    int corrections = 0;
    auto counts = runPipeline (h, input, vs, corrections);

    // Everything released at the end, no hanging voice
    CHECK (h.heldVoices() == 0);
    CHECK (h.allocator.getNumHeldNotes() == 0);
    CHECK (counts[Type::start] + counts[Type::steal] >= 3);
    CHECK (counts[Type::release] >= 3);

    if (m == PlayMode::legato)
    {
        // The pitch steps inside a phrase are connected changes (both pipeline orders), never a start
        CHECK (counts[Type::legato] >= 3);
        CHECK (counts[Type::start] <= 3 + corrections);
        CHECK (counts[Type::release] == 3);
    }

    if (m == PlayMode::mono)
    {
        CHECK (counts[Type::legato] == 0);
        CHECK (counts[Type::release] == 3);
    }

    if (m == PlayMode::poly)
    {
        // Every non-corrected change overlaps the old tail on another voice
        CHECK (counts[Type::start] >= 6);
        CHECK (counts[Type::steal] == 0);
    }
}
