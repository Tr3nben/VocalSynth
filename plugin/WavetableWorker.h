#pragma once

#include "WavetableStore.h"

#include <juce_core/juce_core.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

namespace vocalsynth
{

/** Builds wavetables and publishes them to the WavetableStore (docs/SYNTH-DESIGN.md 12.2).

    Two ways in (critique 0.4):
    - SYNCHRONOUS on the calling (non-audio) thread: loadFactoryNow / loadEmbeddedNow. Used by the
      processor constructor (default table), setStateInformation (project load: the tables are published
      when it returns, so playback, offline render and an immediate getStateInformation see them) and
      PresetManager::load. A factory table found shared (WavetableStore::findShared) costs nothing; a build
      takes <= 150 ms (A's gate).
    - ASYNCHRONOUS on the background thread: requestFactory / requestImport / requestEmbedded, used by the
      UI (table menu, Import WAV...) so the message thread never waits for a file or a build.
    Factory tables: FactoryWavetables::build, shared by key; imports: file bytes -> WavReader ->
    WavetableBuilder::buildFromWav, frames quantised to PCM16, hash, shared by hash; embedded project /
    preset tables: buildFromPcm16 (the hash is recomputed from the data; a mismatching ref is ignored).
    Per slot only the NEWEST request counts: every request (sync or async) takes a per-slot sequence number;
    a queued older one is dropped, one in progress is cancelled via the builder's cancel flag, and a result
    is published only if its number is still the newest for the slot (checked under the worker's lock right
    before publish), so a late async build never overwrites a later synchronous load.
    getRequestedReference() = the reference of the newest request per slot (factory / embedded: at request
    time; import: once the file has been read and hashed): this is what SynthState saves.
    Until a slot gets its first table the engine plays its built-in sine. Also calls store.collectGarbage()
    every 100 ms. Never the audio thread. The thread starts in the constructor and joins in stop() / the
    destructor (bounded: one build step). Owner: agent D. */
class WavetableWorker : private juce::Thread
{
public:
    explicit WavetableWorker (WavetableStore& store);
    ~WavetableWorker() override;

    void requestFactory (int slot, const juce::String& factoryId);
    void requestImport (int slot, const juce::File& file, int frameSizeOverride = 0);
    void requestEmbedded (int slot, std::shared_ptr<const UserTableData> data);

    /** Synchronous variants (calling thread, never the audio thread). False + the default factory table
        published for the slot if the id is unknown / the data is broken (errorMessage set). */
    bool loadFactoryNow (int slot, const juce::String& factoryId, juce::String& errorMessage);
    bool loadEmbeddedNow (int slot, std::shared_ptr<const UserTableData> data, juce::String& errorMessage);

    /** Reference (+ user data) of the newest request for the slot; the default factory table before any.
        Any non-audio thread (internal lock). */
    WavetableSlotInfo getRequestedReference (int slot) const;

    struct Status
    {
        bool busy = false;
        int errorSlot = -1;      ///< slot of the last failed request, -1 if the last request per slot worked
        juce::String lastError;  ///< English, for the UI ("Not a WAV file", ...)
        uint32_t completed = 0;  ///< requests finished (published or failed) since construction
    };

    Status getStatus() const;

    /** Tests: true once no request is pending or running (polls, message thread). */
    bool waitUntilIdle (int timeoutMs);

    void stop();

private:
    /** One asynchronous request (at most one queued per slot: a newer one replaces it). */
    struct Job
    {
        enum class Kind
        {
            factory,
            import,
            embedded
        };

        Kind kind = Kind::factory;
        int slot = 0;
        uint64_t sequence = 0;
        uint64_t order = 0;                          ///< global arrival order (oldest queued job runs first)
        juce::String factoryId;
        juce::File file;
        int frameSizeOverride = 0;
        std::shared_ptr<const UserTableData> data;  ///< embedded: validated, hash recomputed
    };

    void run() override;

    /** Under the lock: a new request for the slot (drops its queued job, cancels its running build). */
    uint64_t beginRequestLocked (int slot);
    void enqueue (Job job);
    void processJob (const Job& job);

    /** Publishes if sequence is still the newest for the slot (worker lock held across check and publish). */
    void finish (int slot, uint64_t sequence, std::shared_ptr<const synth::Wavetable> table,
                 const WavetableReference& reference, std::shared_ptr<const UserTableData> userData);

    /** A failed request: status error; if it is still the newest, the requested reference falls back to
        what the slot holds (asynchronous) - the synchronous loads publish the default table instead. */
    void fail (int slot, uint64_t sequence, const juce::String& message);
    void failRequest (int slot, const juce::String& message); ///< an asynchronous request rejected at once
    void finishSync();

    bool isNewestLocked (int slot, uint64_t sequence) const;

    WavetableStore& store;
    std::atomic<bool> cancel { false }; ///< cancel flag of the running asynchronous build

    mutable std::mutex lock;            ///< everything below; never the audio thread
    std::array<uint64_t, WavetableStore::numSlots> latestSequence {};
    std::array<std::optional<Job>, WavetableStore::numSlots> pending;
    std::array<WavetableSlotInfo, WavetableStore::numSlots> requested;
    uint64_t nextOrder = 0;
    int runningSlot = -1;
    int syncLoadsRunning = 0;
    bool stopped = false;
    Status status;
};

} // namespace vocalsynth
