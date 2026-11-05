// WavetableStore: retire protocol, stress test (1000 swaps while an audio thread renders), sharing by key, references.
// Test plan: docs/SYNTH-DESIGN.md 21. Owner: agent D (Plugin data).

#include "WavetableStore.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>
#include <vector>

using vocalsynth::UserTableData;
using vocalsynth::WavetableReference;
using vocalsynth::WavetableStore;
using vocalsynth::synth::TablesInUse;
using vocalsynth::synth::Wavetable;

namespace
{
/** A one-frame table whose level-0 samples all hold `marker` (the fake engine checks it on every read). */
std::shared_ptr<const Wavetable> makeTable (float marker)
{
    auto table = std::make_shared<Wavetable> ("t" + std::to_string (marker), 1);
    auto* p = table->getLevelForWriting (0, 0);

    for (int i = -Wavetable::guardBefore; i < Wavetable::mipLength (0) + Wavetable::guardAfter; ++i)
        p[i] = marker;

    return table;
}

float markerOf (const Wavetable* t)
{
    return t->getLevel (0, 0)[0];
}

/** What the engine does with the tables of one oscillator: a table change crossfades over `fadeBlocks` blocks,
    a change during a fade is queued (newest wins), like WavetableOscillator (docs/SYNTH-DESIGN.md 6.1). */
struct FakeOscillator
{
    const Wavetable* current = nullptr;
    const Wavetable* previous = nullptr;
    const Wavetable* queued = nullptr;
    int fadeLeft = 0;
    int fadeBlocks = 3;

    void setTable (const Wavetable* t)
    {
        if (t == current && queued == nullptr)
            return;

        if (fadeLeft > 0)
        {
            queued = t == current ? nullptr : t;
            return;
        }

        previous = current;
        current = t;
        fadeLeft = previous != nullptr ? fadeBlocks : 0;
    }

    /** Reads every table it uses; returns false on a wrong marker (a freed / reused table). */
    bool render (const std::vector<std::pair<const Wavetable*, float>>& known)
    {
        bool ok = true;

        for (const auto* t : { current, previous })
        {
            if (t == nullptr)
                continue;

            float sum = 0.0f;
            const auto* p = t->getLevel (0, 0);

            for (int i = 0; i < 64; ++i)
                sum += p[i * 37];

            const auto marker = markerOf (t);
            ok = ok && juce::exactlyEqual (sum, 64.0f * marker);

            for (const auto& [table, m] : known)
                if (table == t)
                    ok = ok && juce::exactlyEqual (m, marker);
        }

        if (fadeLeft > 0 && --fadeLeft == 0)
        {
            previous = nullptr;

            if (queued != nullptr)
            {
                previous = current;
                current = queued;
                queued = nullptr;
                fadeLeft = fadeBlocks;
            }
        }

        return ok;
    }

    TablesInUse inUse() const { return { current, previous }; }
};
} // namespace

TEST_CASE ("WavetableReference: text form", "[plugin][synth][store]")
{
    const auto f = WavetableReference::fromRefString ("factory:pwm", "PWM");
    CHECK (f.kind == WavetableReference::Kind::factory);
    CHECK (f.id == "pwm");
    CHECK (f.name == "PWM");
    CHECK (f.toRefString() == "factory:pwm");

    const auto u = WavetableReference::fromRefString ("user:0123456789abcdef", "My Table.wav");
    CHECK (u.kind == WavetableReference::Kind::user);
    CHECK (u.id == "0123456789abcdef");
    CHECK (u.toRefString() == "user:0123456789abcdef");

    for (const auto* bad : { "", "factory:", "user:", "pwm", "Factory:pwm", "file:abc" })
    {
        CAPTURE (bad);
        const auto r = WavetableReference::fromRefString (bad);
        CHECK (r.kind == WavetableReference::Kind::factory);
        CHECK (r.id == "basic-shapes");
    }

    CHECK (WavetableReference::defaultFactory().toRefString() == "factory:basic-shapes");
}

TEST_CASE ("WavetableStore: a retired table is freed only after two counted blocks and while not in use", "[plugin][synth][store]")
{
    WavetableStore store;
    std::weak_ptr<const Wavetable> weak1, weak2;
    const Wavetable* raw1 = nullptr;
    const Wavetable* raw2 = nullptr;

    CHECK (store.acquire (0) == nullptr);
    CHECK (store.getSlot (0).table == nullptr);

    {
        auto t1 = makeTable (1.0f);
        weak1 = t1;
        raw1 = t1.get();
        store.publish (0, std::move (t1), WavetableReference::fromRefString ("factory:pwm", "PWM"));
    }

    CHECK (store.acquire (0) == raw1);
    CHECK (store.acquire (1) == nullptr);
    CHECK (store.getSlot (0).reference.id == "pwm");
    CHECK (store.getNumRetired() == 0);

    // Block 0 runs with table 1
    store.release ({ raw1, nullptr }, {});

    // Block 1 starts with table 1 (acquired before the publish) ...
    CHECK (store.acquire (0) == raw1);

    {
        auto t2 = makeTable (2.0f);
        weak2 = t2;
        raw2 = t2.get();
        store.publish (0, std::move (t2), WavetableReference::fromRefString ("factory:fold", "Fold"));
    }

    CHECK (store.getNumRetired() == 1); // table 1, retireBlock = 1
    store.collectGarbage();
    CHECK_FALSE (weak1.expired());

    // ... and ends still reading it
    store.release ({ raw1, nullptr }, {});
    store.collectGarbage();
    CHECK_FALSE (weak1.expired()); // only one counted block since the publish

    // Block 2 acquires table 2 and crossfades from table 1
    CHECK (store.acquire (0) == raw2);
    store.release ({ raw2, raw1 }, {});
    store.collectGarbage();
    CHECK_FALSE (weak1.expired()); // two blocks, but in use (crossfade)

    // Still fading in block 3, reported by oscillator B this time (tables are shared between slots)
    store.release ({ raw2, nullptr }, { nullptr, raw1 });
    store.collectGarbage();
    CHECK_FALSE (weak1.expired());

    // Block 4: the fade is over
    store.release ({ raw2, nullptr }, {});
    CHECK (store.getNumRetired() == 1);
    store.collectGarbage();
    CHECK (weak1.expired());
    CHECK (store.getNumRetired() == 0);
    CHECK_FALSE (weak2.expired());
    CHECK (store.getSlot (0).table.get() == raw2);
}

TEST_CASE ("WavetableStore: releaseAllRetired frees everything retired and forgets the in-use report", "[plugin][synth][store]")
{
    WavetableStore store;
    std::vector<std::weak_ptr<const Wavetable>> weak;

    for (int i = 0; i < 5; ++i)
    {
        auto t = makeTable (static_cast<float> (i));
        weak.push_back (t);
        store.publish (i % 2, std::move (t), {});
    }

    store.release ({ store.acquire (0), weak[0].lock().get() }, { store.acquire (1), nullptr });
    CHECK (store.getNumRetired() == 3);
    store.collectGarbage();
    CHECK (store.getNumRetired() == 3);

    store.releaseAllRetired();
    CHECK (store.getNumRetired() == 0);

    for (int i = 0; i < 3; ++i)
        CHECK (weak[static_cast<std::size_t> (i)].expired());

    CHECK_FALSE (weak[3].expired()); // current of slot 1
    CHECK_FALSE (weak[4].expired()); // current of slot 0
}

TEST_CASE ("WavetableStore: re-publishing a table that is still current (ABA by sharing)", "[plugin][synth][store]")
{
    WavetableStore store;
    auto a = makeTable (1.0f);
    auto b = makeTable (2.0f);
    std::weak_ptr<const Wavetable> weakB = b;
    const auto* rawA = a.get();

    store.publish (0, a, WavetableReference::fromRefString ("factory:basic-shapes"));
    store.publish (0, a, WavetableReference::fromRefString ("factory:basic-shapes")); // same table: nothing retired
    CHECK (store.getNumRetired() == 0);

    store.publish (0, std::move (b), WavetableReference::fromRefString ("factory:pwm"));
    store.publish (0, a, WavetableReference::fromRefString ("factory:basic-shapes")); // A -> B -> A
    CHECK (store.getNumRetired() == 2);
    a.reset();

    for (int block = 0; block < 3; ++block)
    {
        CHECK (store.acquire (0) == rawA);
        store.release ({ rawA, nullptr }, {});
    }

    store.collectGarbage();
    CHECK (store.getNumRetired() == 1); // A stays retired while it is in use (one reference only)
    CHECK (weakB.expired());

    // A is still the current table: the retired entry was just one more reference
    CHECK (store.getSlot (0).table.get() == rawA);
    CHECK (markerOf (store.acquire (0)) == 1.0f);

    store.release ({}, {});
    store.collectGarbage();
    CHECK (store.getNumRetired() == 0);
    CHECK (store.getSlot (0).table.get() == rawA);
    CHECK (markerOf (store.acquire (0)) == 1.0f);
}

TEST_CASE ("WavetableStore: slots, references, user data and generation", "[plugin][synth][store]")
{
    WavetableStore store;
    const auto g0 = store.getGeneration();

    auto data = std::make_shared<UserTableData>();
    data->name = "My Table.wav";
    data->hash = "0123456789abcdef";
    data->numFrames = 1;
    data->pcm.assign (2048, 7);

    store.publish (1, makeTable (3.0f), WavetableReference::fromRefString ("user:0123456789abcdef", "My Table.wav"), data);
    CHECK (store.getGeneration() == g0 + 1);

    const auto info = store.getSlot (1);
    CHECK (info.reference.kind == WavetableReference::Kind::user);
    CHECK (info.userData == data);
    CHECK (info.table.get() == store.acquire (1));
    CHECK (store.getSlot (0).table == nullptr);

    // Invalid slots are ignored
    store.publish (2, makeTable (4.0f), {});
    store.publish (-1, makeTable (4.0f), {});
    CHECK (store.getGeneration() == g0 + 1);
    CHECK (store.acquire (2) == nullptr);
    CHECK (store.acquire (-1) == nullptr);
    CHECK (store.getSlot (5).table == nullptr);
}

TEST_CASE ("WavetableStore: process-wide sharing by key keeps weak references", "[plugin][synth][store]")
{
    const juce::String key = "user:feedfacecafebeef";
    CHECK (WavetableStore::findShared (key) == nullptr);

    {
        auto t = makeTable (5.0f);
        WavetableStore::addShared (key, t);
        CHECK (WavetableStore::findShared (key) == t);
        CHECK (WavetableStore::findShared ("user:feedfacecafebee0") == nullptr);

        // A second instance gets the same table
        WavetableStore a, b;
        a.publish (0, WavetableStore::findShared (key), {});
        b.publish (1, WavetableStore::findShared (key), {});
        CHECK (a.acquire (0) == b.acquire (1));
    }

    // Nobody holds it any more: gone
    CHECK (WavetableStore::findShared (key) == nullptr);

    WavetableStore::addShared ("", makeTable (1.0f));
    WavetableStore::addShared ("user:0000000000000000", nullptr);
    CHECK (WavetableStore::findShared ("user:0000000000000000") == nullptr);
}

TEST_CASE ("WavetableStore: stress - 1000 publishes while an audio thread renders", "[plugin][synth][store]")
{
    WavetableStore store;
    std::atomic<bool> running { true };
    std::atomic<int> errors { 0 };
    std::atomic<int64_t> blocks { 0 };

    // Shared tables that are re-published again and again (ABA through sharing)
    const auto sharedA = makeTable (1000.0f);
    const auto sharedB = makeTable (2000.0f);
    store.publish (0, sharedA, {});
    store.publish (1, sharedB, {});

    std::thread audio ([&]
    {
        std::array<FakeOscillator, 2> osc;
        osc[0].fadeBlocks = 2;
        osc[1].fadeBlocks = 5;

        while (running.load (std::memory_order_relaxed))
        {
            for (int s = 0; s < 2; ++s)
                osc[static_cast<std::size_t> (s)].setTable (store.acquire (s));

            for (auto& o : osc)
                if (! o.render ({}))
                    errors.fetch_add (1, std::memory_order_relaxed);

            store.release (osc[0].inUse(), osc[1].inUse());
            blocks.fetch_add (1, std::memory_order_relaxed);
        }
    });

    for (int i = 0; i < 1000; ++i)
    {
        const auto slot = i % 2;

        if (i % 7 == 0)
            store.publish (slot, slot == 0 ? sharedA : sharedB, {});
        else if (i % 11 == 0)
            store.publish (slot, sharedA, {}); // the same table in both slots
        else
            store.publish (slot, makeTable (static_cast<float> (i)), {});

        if (i % 3 == 0)
            store.collectGarbage();

        // Let the audio thread run a few blocks now and then, so tables really get freed while it renders
        if (i % 50 == 0)
        {
            const auto target = blocks.load() + 8;

            while (blocks.load() < target)
                std::this_thread::yield();
        }

        (void) store.getSlot (slot);
    }

    const auto target = blocks.load() + 20;

    while (blocks.load() < target)
    {
        store.collectGarbage();
        std::this_thread::yield();
    }

    running.store (false);
    audio.join();

    CHECK (errors.load() == 0);
    CHECK (blocks.load() > 100);

    // Blocks that use no table any more: every retired table goes (only references are dropped for the
    // tables that are still current)
    store.release ({}, {});
    store.release ({}, {});
    store.collectGarbage();
    CHECK (store.getNumRetired() == 0);
    CHECK (store.acquire (0) == store.getSlot (0).table.get());
    CHECK (store.acquire (1) == store.getSlot (1).table.get());

    store.releaseAllRetired();
    CHECK (store.getNumRetired() == 0);
}
