// WavetableWorker: factory / import / embedded requests, newest request wins, errors, Unicode paths.
// Test plan: docs/SYNTH-DESIGN.md 21. Owner: agent D (Plugin data).

#include "SynthDataTestUtils.h"
#include "WavetableWorker.h"
#include "vocalsynth/synth/FactoryWavetables.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <random>
#include <thread>

using vocalsynth::UserTableData;
using vocalsynth::WavetableReference;
using vocalsynth::WavetableStore;
using vocalsynth::WavetableWorker;
namespace testdata = vocalsynth::testdata;
namespace synth = vocalsynth::synth;

namespace
{
constexpr int idleTimeoutMs = 20000;

juce::String factoryId (synth::FactoryTable t)
{
    const auto id = synth::FactoryWavetables::getInfo (t).id;
    return juce::String (id.data(), id.size());
}

juce::File writeWav (const juce::File& folder, const juce::String& name, const juce::MemoryBlock& bytes)
{
    const auto file = folder.getChildFile (name);
    REQUIRE (file.replaceWithData (bytes.getData(), bytes.getSize()));
    return file;
}
} // namespace

TEST_CASE ("WavetableWorker: synchronous factory loads are published when they return", "[plugin][synth][worker]")
{
    WavetableStore store;
    WavetableWorker worker (store);

    // Before any request: the default reference (the engine plays its sine until a table is published)
    CHECK (worker.getRequestedReference (0).reference.toRefString() == "factory:basic-shapes");
    CHECK (worker.getRequestedReference (0).reference.name == "Basic Shapes");
    CHECK (store.acquire (0) == nullptr);

    juce::String error;
    REQUIRE (worker.loadFactoryNow (0, "pwm", error));
    CHECK (error.isEmpty());

    const auto slot = store.getSlot (0);
    CHECK (slot.reference.toRefString() == "factory:pwm");
    CHECK (slot.reference.name == "PWM");
    REQUIRE (slot.table != nullptr);
    CHECK (store.acquire (0) == slot.table.get());
    CHECK (slot.table->getNumFrames() == synth::FactoryWavetables::getInfo (synth::FactoryTable::pwm).numFrames);
    CHECK (worker.getRequestedReference (0).reference == slot.reference);
    CHECK (worker.getRequestedReference (0).table == slot.table);
    CHECK (store.acquire (1) == nullptr);

    // Shared: a second slot / instance gets the same table without a build
    WavetableStore otherStore;
    WavetableWorker otherWorker (otherStore);
    REQUIRE (otherWorker.loadFactoryNow (1, "pwm", error));
    CHECK (otherStore.acquire (1) == store.acquire (0));

    const auto status = worker.getStatus();
    CHECK_FALSE (status.busy);
    CHECK (status.errorSlot == -1);
    CHECK (status.completed == 1);
}

TEST_CASE ("WavetableWorker: an unknown factory id gives the default table (sync) or keeps the table (async)", "[plugin][synth][worker]")
{
    WavetableStore store;
    WavetableWorker worker (store);
    juce::String error;

    CHECK_FALSE (worker.loadFactoryNow (1, "no-such-table", error));
    CHECK (error.contains ("no-such-table"));
    CHECK (store.getSlot (1).reference.toRefString() == "factory:basic-shapes");
    CHECK (store.acquire (1) != nullptr);
    CHECK (worker.getRequestedReference (1).reference.toRefString() == "factory:basic-shapes");
    CHECK (worker.getStatus().errorSlot == 1);

    REQUIRE (worker.loadFactoryNow (1, "fold", error));
    CHECK (worker.getStatus().errorSlot == -1);
    CHECK (worker.getStatus().lastError.isEmpty());

    worker.requestFactory (1, "nope");
    REQUIRE (worker.waitUntilIdle (idleTimeoutMs));
    CHECK (store.getSlot (1).reference.id == "fold");
    CHECK (worker.getRequestedReference (1).reference.id == "fold");
    CHECK (worker.getStatus().errorSlot == 1);

    CHECK_FALSE (worker.loadFactoryNow (2, "pwm", error));
    CHECK_FALSE (worker.loadFactoryNow (-1, "pwm", error));
}

TEST_CASE ("WavetableWorker: asynchronous factory requests", "[plugin][synth][worker]")
{
    WavetableStore store;
    WavetableWorker worker (store);

    worker.requestFactory (0, "organ");
    worker.requestFactory (1, "digital");

    // The reference counts from the request on (what a save right now must store)
    CHECK (worker.getRequestedReference (0).reference.id == "organ");
    CHECK (worker.getRequestedReference (1).reference.id == "digital");

    REQUIRE (worker.waitUntilIdle (idleTimeoutMs));
    CHECK (store.getSlot (0).reference.id == "organ");
    CHECK (store.getSlot (1).reference.id == "digital");
    CHECK (store.acquire (0) != nullptr);
    CHECK (worker.getStatus().completed >= 2);
    CHECK_FALSE (worker.getStatus().busy);

    // Only the newest request of a slot counts
    worker.requestFactory (0, "bell");
    worker.requestFactory (0, "choir");
    worker.requestFactory (0, "square-stack");
    REQUIRE (worker.waitUntilIdle (idleTimeoutMs));
    CHECK (store.getSlot (0).reference.id == "square-stack");
    CHECK (worker.getRequestedReference (0).reference.id == "square-stack");
}

TEST_CASE ("WavetableWorker: a late asynchronous build never overwrites a later synchronous load", "[plugin][synth][worker]")
{
    WavetableStore store;
    WavetableWorker worker (store);
    std::mt19937 rng (7);
    const std::array<juce::String, 6> ids { "harmonic-sweep", "saw-bright-dark", "formant-vowel", "sync-sweep",
                                            "fm-2op", "spectral-random" };

    for (int n = 0; n < 12; ++n)
    {
        const auto asyncId = ids[rng() % ids.size()];
        const auto syncId = ids[rng() % ids.size()];
        CAPTURE (n, asyncId, syncId);

        // Not shared yet on the first use: the async build really runs while the sync load happens
        worker.requestFactory (0, asyncId);

        if (n % 3 == 1)
            std::this_thread::sleep_for (std::chrono::milliseconds (static_cast<int> (rng() % 20)));

        juce::String error;
        REQUIRE (worker.loadFactoryNow (0, syncId, error));
        CHECK (store.getSlot (0).reference.id == syncId);

        REQUIRE (worker.waitUntilIdle (idleTimeoutMs));
        CHECK (store.getSlot (0).reference.id == syncId);
        CHECK (worker.getRequestedReference (0).reference.id == syncId);
    }
}

TEST_CASE ("WavetableWorker: WAV import from a path with spaces and umlauts", "[plugin][synth][worker]")
{
    testdata::ScopedTempFolder temp;
    WavetableStore store;
    WavetableWorker worker (store);
    juce::String error;
    REQUIRE (worker.loadFactoryNow (1, "basic-shapes", error));

    const auto name = juce::String::fromUTF8 ("M\xc3\xa4in T\xc3\xa4" "ble \xc3\x9f 1.wav");
    const auto file = writeWav (temp.folder, name, testdata::makeWav16 (testdata::morphFrames (4, 3), 2));

    worker.requestImport (1, file);
    REQUIRE (worker.waitUntilIdle (idleTimeoutMs));

    const auto slot = store.getSlot (1);
    CHECK (worker.getStatus().errorSlot == -1);
    CHECK (slot.reference.kind == WavetableReference::Kind::user);
    CHECK (slot.reference.name == name);
    CHECK (slot.reference.id.length() == 16);
    CHECK (slot.reference.id.containsOnly ("0123456789abcdef"));
    REQUIRE (slot.table != nullptr);
    CHECK (slot.table->getNumFrames() == 4);
    REQUIRE (slot.userData != nullptr);
    CHECK (slot.userData->numFrames == 4);
    CHECK (slot.userData->pcm.size() == 4u * 2048u);
    CHECK (slot.userData->hash == slot.reference.id);
    CHECK (testdata::hashOf (slot.userData->pcm) == slot.reference.id);
    CHECK (slot.userData->name == name);
    CHECK (slot.table->getContentHash() == synth::WavetableBuilder::fnv1a64 (slot.userData->pcm));

    const auto requested = worker.getRequestedReference (1);
    CHECK (requested.reference == slot.reference);
    CHECK (requested.userData == slot.userData);

    // The embedded data rebuilds the same table: shared by hash while the import is alive ...
    WavetableStore store2;
    WavetableWorker worker2 (store2);
    auto copy = std::make_shared<UserTableData> (*slot.userData);
    REQUIRE (worker2.loadEmbeddedNow (0, copy, error));
    CHECK (store2.acquire (0) == store.acquire (1));

    // ... and bit-identical from the PCM alone
    const auto rebuilt = synth::WavetableBuilder::buildFromPcm16 ("x", slot.userData->pcm, 4);
    REQUIRE (rebuilt.ok());

    for (int level = 0; level < synth::Wavetable::numMipLevels; level += 3)
        for (int i = 0; i < synth::Wavetable::mipLength (level); i += 97)
            REQUIRE (juce::exactlyEqual (rebuilt.table->getLevel (2, level)[i], slot.table->getLevel (2, level)[i]));

    // A frame-size override cuts the same file differently
    worker.requestImport (0, file, 1024);
    REQUIRE (worker.waitUntilIdle (idleTimeoutMs));
    REQUIRE (store.getSlot (0).table != nullptr);
    CHECK (store.getSlot (0).table->getNumFrames() == 8);
    CHECK (store.getSlot (0).reference.id != slot.reference.id);
}

TEST_CASE ("WavetableWorker: import errors keep the slot's table", "[plugin][synth][worker]")
{
    testdata::ScopedTempFolder temp;
    WavetableStore store;
    WavetableWorker worker (store);
    juce::String error;
    REQUIRE (worker.loadFactoryNow (0, "fm-2op", error));
    const auto before = store.acquire (0);

    worker.requestImport (0, temp.folder.getChildFile ("missing.wav"));
    REQUIRE (worker.waitUntilIdle (idleTimeoutMs));
    CHECK (worker.getStatus().errorSlot == 0);
    CHECK (worker.getStatus().lastError.contains ("not found"));
    CHECK (store.acquire (0) == before);
    CHECK (worker.getRequestedReference (0).reference.id == "fm-2op");

    juce::MemoryBlock junk;
    junk.append ("this is not a wav file at all", 29);
    worker.requestImport (0, writeWav (temp.folder, "junk.wav", junk));
    REQUIRE (worker.waitUntilIdle (idleTimeoutMs));
    CHECK (worker.getStatus().errorSlot == 0);
    CHECK (worker.getStatus().lastError.isNotEmpty());
    CHECK (store.acquire (0) == before);

    // A valid import afterwards clears the error
    worker.requestImport (0, writeWav (temp.folder, "ok.wav", testdata::makeWav16 (testdata::morphFrames (1, 1))));
    REQUIRE (worker.waitUntilIdle (idleTimeoutMs));
    CHECK (worker.getStatus().errorSlot == -1);
    CHECK (store.getSlot (0).reference.kind == WavetableReference::Kind::user);
    CHECK (store.acquire (0) != before);
}

TEST_CASE ("WavetableWorker: embedded data - hash recomputed, broken data, data by hash", "[plugin][synth][worker]")
{
    WavetableStore store;
    WavetableWorker worker (store);
    juce::String error;

    auto data = testdata::makeUserData (3, 11, "Embedded.wav");
    const auto correctHash = data->hash;

    // A mismatching reference is ignored: the hash comes from the data
    auto wrongHash = std::make_shared<UserTableData> (*data);
    wrongHash->hash = "0000000000000000";
    REQUIRE (worker.loadEmbeddedNow (0, wrongHash, error));
    CHECK (store.getSlot (0).reference.toRefString() == "user:" + correctHash);
    CHECK (store.getSlot (0).reference.name == "Embedded.wav");
    REQUIRE (store.getSlot (0).userData != nullptr);
    CHECK (store.getSlot (0).userData->hash == correctHash);
    CHECK (worker.getRequestedReference (0).userData->hash == correctHash);

    // A TABLE without data: found by hash while this process holds the table
    auto refOnly = std::make_shared<UserTableData>();
    refOnly->name = "Embedded.wav";
    refOnly->hash = correctHash;
    REQUIRE (worker.loadEmbeddedNow (1, refOnly, error));
    CHECK (store.acquire (1) == store.acquire (0));
    REQUIRE (store.getSlot (1).userData != nullptr);
    CHECK (store.getSlot (1).userData->pcm == data->pcm);

    // Broken: wrong value count, frame count out of range, no data -> the default table, error
    const auto expectDefault = [&] (std::shared_ptr<UserTableData> bad, int slot)
    {
        juce::String e;
        CHECK_FALSE (worker.loadEmbeddedNow (slot, std::move (bad), e));
        CHECK (e.isNotEmpty());
        CHECK (store.getSlot (slot).reference.toRefString() == "factory:basic-shapes");
        CHECK (worker.getRequestedReference (slot).reference.toRefString() == "factory:basic-shapes");
        CHECK (worker.getStatus().errorSlot == slot);
    };

    auto shortData = std::make_shared<UserTableData> (*data);
    shortData->pcm.resize (shortData->pcm.size() - 1);
    expectDefault (shortData, 0);

    auto tooMany = std::make_shared<UserTableData> (*data);
    tooMany->numFrames = 300;
    expectDefault (tooMany, 1);

    auto zero = std::make_shared<UserTableData> (*data);
    zero->numFrames = 0;
    zero->pcm.clear();
    zero->hash = "0123456789abcdef"; // unknown hash, no data
    expectDefault (zero, 0);

    expectDefault (nullptr, 1);

    // Async embedded request
    worker.requestEmbedded (1, data);
    CHECK (worker.getRequestedReference (1).reference.id == correctHash);
    REQUIRE (worker.waitUntilIdle (idleTimeoutMs));
    CHECK (store.getSlot (1).reference.id == correctHash);
    CHECK (worker.getStatus().errorSlot == -1);

    // Async broken: rejected at once, the slot keeps its table
    worker.requestEmbedded (1, shortData);
    REQUIRE (worker.waitUntilIdle (idleTimeoutMs));
    CHECK (store.getSlot (1).reference.id == correctHash);
    CHECK (worker.getRequestedReference (1).reference.id == correctHash);
    CHECK (worker.getStatus().errorSlot == 1);
}

TEST_CASE ("WavetableWorker: a shared user table is reused only for the same PCM, not for the same hash alone "
           "(review 0.4 R2-8)",
           "[plugin][synth][worker]")
{
    WavetableStore store;
    WavetableWorker worker (store);
    juce::String error;
    const auto a = testdata::makeUserData (2, 801, "A.wav");
    const auto b = testdata::makeUserData (2, 802, "B.wav");
    REQUIRE (a->hash != b->hash);
    REQUIRE (worker.loadEmbeddedNow (0, a, error));
    const auto tableA = store.getSlot (0).table;
    REQUIRE (tableA != nullptr);

    // An FNV-1a collision (B's hash names A's table in the process-wide map), simulated by registering it
    WavetableStore::addShared ("user:" + b->hash, tableA);
    REQUIRE (worker.loadEmbeddedNow (1, b, error));
    const auto tableB = store.getSlot (1).table;
    REQUIRE (tableB != nullptr);
    CHECK (tableB != tableA);
    CHECK (tableB->getNumFrames() == 2);

    // The same PCM again (another instance, a project) still shares the table
    REQUIRE (worker.loadEmbeddedNow (0, std::make_shared<UserTableData> (*b), error));
    CHECK (store.getSlot (0).table == tableB);
    worker.stop();
}

TEST_CASE ("WavetableWorker: the default table stays built process-wide", "[plugin][synth][worker]")
{
    {
        WavetableStore store;
        WavetableWorker worker (store);
        juce::String error;
        REQUIRE (worker.loadFactoryNow (0, factoryId (synth::defaultFactoryTable), error));
    }

    CHECK (WavetableStore::findShared ("factory:basic-shapes") != nullptr);

    // Constructing more instances costs no build: published at once from the shared table
    const auto start = juce::Time::getMillisecondCounterHiRes();

    for (int i = 0; i < 20; ++i)
    {
        WavetableStore store;
        WavetableWorker worker (store);
        juce::String error;
        REQUIRE (worker.loadFactoryNow (0, "basic-shapes", error));
        REQUIRE (worker.loadFactoryNow (1, "basic-shapes", error));
        CHECK (store.acquire (0) == WavetableStore::findShared ("factory:basic-shapes").get());
    }

    CHECK (juce::Time::getMillisecondCounterHiRes() - start < 5000.0);
}

TEST_CASE ("WavetableWorker: rapid create / destroy with requests in flight", "[plugin][synth][worker]")
{
    testdata::ScopedTempFolder temp;
    const auto wav = writeWav (temp.folder, "big one.wav", testdata::makeWav16 (testdata::morphFrames (64, 5)));

    for (int i = 0; i < 40; ++i)
    {
        WavetableStore store;
        WavetableWorker worker (store);

        switch (i % 4)
        {
            case 0: worker.requestImport (0, wav); break;
            case 1: worker.requestFactory (1, "spectral-random"); worker.requestImport (0, wav); break;
            case 2: worker.requestEmbedded (0, testdata::makeUserData (2, i)); break;
            default: break;
        }

        if (i % 5 == 0)
            std::this_thread::sleep_for (std::chrono::milliseconds (3));

        if (i % 2 == 0)
            worker.stop(); // explicit stop, then the destructor again

        // Requests after stop are ignored
        worker.requestFactory (0, "pwm");
    }

    SUCCEED();
}

TEST_CASE ("WavetableWorker: concurrent sync loads, async requests, readers and an audio thread", "[plugin][synth][worker]")
{
    WavetableStore store;
    WavetableWorker worker (store);
    std::atomic<bool> running { true };
    std::atomic<int64_t> blocks { 0 };
    juce::String error;
    REQUIRE (worker.loadFactoryNow (0, "basic-shapes", error));
    REQUIRE (worker.loadFactoryNow (1, "basic-shapes", error));

    std::thread audio ([&]
    {
        std::array<const synth::Wavetable*, 2> current {};
        std::array<const synth::Wavetable*, 2> previous {};

        while (running.load (std::memory_order_relaxed))
        {
            for (std::size_t s = 0; s < 2; ++s)
            {
                const auto* t = store.acquire (static_cast<int> (s));

                if (t != current[s])
                {
                    previous[s] = current[s];
                    current[s] = t;
                }
                else
                {
                    previous[s] = nullptr;
                }

                volatile float sink = 0.0f;

                for (const auto* table : { current[s], previous[s] })
                    if (table != nullptr)
                        sink = sink + table->getLevel (table->getNumFrames() - 1, 0)[1000];
            }

            store.release ({ current[0], previous[0] }, { current[1], previous[1] });
            blocks.fetch_add (1, std::memory_order_relaxed);
            std::this_thread::yield();
        }
    });

    std::thread reader ([&]
    {
        while (running.load (std::memory_order_relaxed))
        {
            const auto r = worker.getRequestedReference (1);
            const auto status = worker.getStatus();
            juce::ignoreUnused (r, status);
            std::this_thread::yield();
        }
    });

    std::thread syncLoader ([&]
    {
        juce::String e;

        for (int i = 0; i < 30; ++i)
            worker.loadFactoryNow (1, i % 2 == 0 ? "pwm" : "fold", e);
    });

    for (int i = 0; i < 30; ++i)
    {
        worker.requestFactory (1, i % 3 == 0 ? "organ" : "digital");
        worker.requestFactory (0, i % 2 == 0 ? "pwm" : "basic-shapes");
        worker.requestEmbedded (0, testdata::makeUserData (1, i % 4));
        std::this_thread::yield();
    }

    syncLoader.join();
    REQUIRE (worker.waitUntilIdle (idleTimeoutMs));

    // The last request of slot 1 was either the sync loader's or the message thread's: both views agree
    CHECK (worker.getRequestedReference (1).reference == store.getSlot (1).reference);
    CHECK (worker.getRequestedReference (0).reference == store.getSlot (0).reference);
    CHECK (store.getSlot (0).reference.kind == WavetableReference::Kind::user);

    running.store (false);
    audio.join();
    reader.join();
    CHECK (blocks.load() > 0);
}
