#pragma once

#include "WavetableStore.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <memory>

namespace vocalsynth
{

/** Background thread that builds wavetables and publishes them to the WavetableStore
    (docs/SYNTH-DESIGN.md 12.2): factory tables (FactoryWavetables::build, shared by key), imports (file bytes
    -> WavReader -> WavetableBuilder::buildFromWav, frames quantised to PCM16, hash, shared by hash) and
    embedded project / preset tables (buildFromPcm16). Per slot only the NEWEST request counts (an older one
    still queued is dropped, one in progress is cancelled via the builder's cancel flag). Until a slot gets
    its first table the engine plays its built-in sine. Also calls store.collectGarbage() every 100 ms.
    Requests and getStatus(): message thread (lock-free / short lock); never the audio thread.
    The thread starts in the constructor and joins in stop() / the destructor (bounded: one build step).
    Owner: agent D. */
class WavetableWorker : private juce::Thread
{
public:
    explicit WavetableWorker (WavetableStore& store);
    ~WavetableWorker() override;

    void requestFactory (int slot, const juce::String& factoryId);
    void requestImport (int slot, const juce::File& file, int frameSizeOverride = 0);
    void requestEmbedded (int slot, std::shared_ptr<const UserTableData> data);

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
    void run() override;

    WavetableStore& store;
    std::atomic<bool> cancel { false };
};

} // namespace vocalsynth
