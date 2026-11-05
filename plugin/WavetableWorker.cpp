#include "WavetableWorker.h"

#include "vocalsynth/synth/FactoryWavetables.h"
#include "vocalsynth/synth/WavReader.h"
#include "vocalsynth/synth/WavetableBuilder.h"

#include <map>
#include <new>
#include <string>

namespace vocalsynth
{

namespace
{

using synth::FactoryTable;
using synth::FactoryWavetables;
using synth::Wavetable;
using synth::WavetableBuilder;

/** Largest WAV file the import reads into memory: the longest table WavReader accepts (256 x 4096 sample
    frames) in 8 channels of 64-bit float, plus room for other chunks. Anything bigger is not a wavetable. */
constexpr juce::int64 maxImportFileBytes = 256LL * 4096LL * 8LL * 8LL + 1024LL * 1024LL;

constexpr int garbageIntervalMs = 100;

juce::String toHash (uint64_t hash)
{
    return juce::String::toHexString (static_cast<juce::int64> (hash)).paddedLeft ('0', 16).toLowerCase();
}

juce::String factoryKey (FactoryTable table)
{
    const auto id = FactoryWavetables::getInfo (table).id;
    return "factory:" + juce::String (id.data(), id.size());
}

WavetableReference factoryReference (FactoryTable table)
{
    const auto& info = FactoryWavetables::getInfo (table);
    WavetableReference r;
    r.kind = WavetableReference::Kind::factory;
    r.id = juce::String (info.id.data(), info.id.size());
    r.name = juce::String (info.displayName.data(), info.displayName.size());
    return r;
}

WavetableReference userReference (const UserTableData& data)
{
    WavetableReference r;
    r.kind = WavetableReference::Kind::user;
    r.id = data.hash;
    r.name = data.name;
    return r;
}

juce::String defaultFactoryId()
{
    const auto id = FactoryWavetables::getInfo (synth::defaultFactoryTable).id;
    return juce::String (id.data(), id.size());
}

std::optional<FactoryTable> factoryFromId (const juce::String& id)
{
    return FactoryWavetables::fromId (id.toStdString());
}

/** Process-wide user table data by hash (weak): a project TABLE without data (the data was stored with an
    earlier TABLE, or another instance holds the table) can still be restored WITH its data, so the next save
    embeds it again. Never the audio thread. */
struct SharedUserData
{
    std::mutex mutex;
    std::map<std::string, std::weak_ptr<const UserTableData>> data;

    static SharedUserData& get()
    {
        static SharedUserData instance;
        return instance;
    }

    std::shared_ptr<const UserTableData> find (const juce::String& hash)
    {
        const std::scoped_lock l (mutex);
        const auto it = data.find (hash.toStdString());
        return it != data.end() ? it->second.lock() : nullptr;
    }

    void add (const std::shared_ptr<const UserTableData>& d)
    {
        const std::scoped_lock l (mutex);

        for (auto it = data.begin(); it != data.end();)
            it = it->second.expired() ? data.erase (it) : std::next (it);

        data[d->hash.toStdString()] = d;
    }
};

/** The shared factory table, built if no instance holds it. Null if cancelled / failed. */
std::shared_ptr<const Wavetable> getFactoryTable (FactoryTable table, const std::atomic<bool>* cancel)
{
    const auto key = factoryKey (table);

    if (auto shared = WavetableStore::findShared (key))
        return shared;

    auto result = FactoryWavetables::build (table, cancel);

    if (! result.ok())
        return nullptr;

    // Another instance may have built it meanwhile: use one copy process-wide
    if (auto shared = WavetableStore::findShared (key))
        return shared;

    std::shared_ptr<const Wavetable> built = std::move (result.table);
    WavetableStore::addShared (key, built);
    return built;
}

/** Validates embedded / project data (docs/SYNTH-DESIGN.md 12.2, 13.1): data without PCM (a TABLE without
    data) is looked up by its hash among the tables of this process; the hash is recomputed from the PCM
    (a mismatching reference is ignored); frames 1..256 and exactly frames * 2048 values, else broken. */
std::shared_ptr<const UserTableData> prepareEmbedded (std::shared_ptr<const UserTableData> data, juce::String& error)
{
    if (data == nullptr)
    {
        error = "No table data";
        return nullptr;
    }

    if (data->pcm.empty())
    {
        auto shared = data->hash.isNotEmpty() ? SharedUserData::get().find (data->hash.toLowerCase()) : nullptr;

        if (shared == nullptr)
        {
            error = "Table missing: " + (data->name.isNotEmpty() ? data->name : data->hash);
            return nullptr;
        }

        data = std::move (shared);
    }

    if (data->numFrames < 1 || data->numFrames > Wavetable::maxFrames
        || data->pcm.size() != static_cast<std::size_t> (data->numFrames) * static_cast<std::size_t> (Wavetable::frameSize))
    {
        error = "Broken table data: " + data->name;
        return nullptr;
    }

    const auto hash = toHash (WavetableBuilder::fnv1a64 (data->pcm));

    if (hash != data->hash)
    {
        auto fixed = std::make_shared<UserTableData> (*data);
        fixed->hash = hash;
        data = std::move (fixed);
    }

    return data;
}

/** The table of validated user data: shared by hash, else built from the PCM16 frames. */
std::shared_ptr<const Wavetable> getUserTable (const UserTableData& data, const std::atomic<bool>* cancel)
{
    const auto key = "user:" + data.hash;

    if (auto shared = WavetableStore::findShared (key))
        return shared;

    auto result = WavetableBuilder::buildFromPcm16 (data.name.toStdString(), data.pcm, data.numFrames, cancel);

    if (! result.ok())
        return nullptr;

    if (auto shared = WavetableStore::findShared (key))
        return shared;

    std::shared_ptr<const Wavetable> built = std::move (result.table);
    WavetableStore::addShared (key, built);
    return built;
}

juce::String describeBuildError (synth::WavetableBuildError e)
{
    switch (e)
    {
        case synth::WavetableBuildError::none:             return {};
        case synth::WavetableBuildError::noSamples:        return "The file holds no samples";
        case synth::WavetableBuildError::invalidFrameSize: return "Unsupported frame size";
        case synth::WavetableBuildError::readError:        return "Could not read the WAV file";
        case synth::WavetableBuildError::cancelled:        return "Cancelled";
    }

    return "Could not build the table";
}

struct ImportResult
{
    std::shared_ptr<const Wavetable> table;
    std::shared_ptr<const UserTableData> data;
    juce::String error;
    bool cancelled = false;
};

/** File -> WavReader -> builder -> PCM16 data + hash (docs/SYNTH-DESIGN.md 5.4, 12.2). Worker thread. */
ImportResult importFile (const juce::File& file, int frameSizeOverride, const std::atomic<bool>* cancel)
{
    ImportResult r;

    if (! file.existsAsFile())
    {
        r.error = "File not found: " + file.getFileName();
        return r;
    }

    if (file.getSize() > maxImportFileBytes)
    {
        r.error = "File too large for a wavetable";
        return r;
    }

    juce::MemoryBlock bytes;

    if (! file.loadFileAsData (bytes))
    {
        r.error = "Could not read " + file.getFileName();
        return r;
    }

    const auto wav = synth::WavReader::read (
        std::span<const std::byte> (static_cast<const std::byte*> (bytes.getData()), bytes.getSize()));

    if (! wav.ok())
    {
        const auto text = synth::describeWavReadError (wav.error);
        r.error = juce::String (text.data(), text.size());
        return r;
    }

    bytes.reset();
    const auto name = file.getFileName();
    auto built = WavetableBuilder::buildFromWav (name.toStdString(), wav, frameSizeOverride, cancel);

    if (built.error == synth::WavetableBuildError::cancelled)
    {
        r.cancelled = true;
        return r;
    }

    if (! built.ok())
    {
        r.error = describeBuildError (built.error);
        return r;
    }

    auto data = std::make_shared<UserTableData>();
    data->name = name;
    data->numFrames = built.frames.numFrames;
    data->pcm.resize (built.frames.samples.size());

    for (std::size_t i = 0; i < data->pcm.size(); ++i)
        data->pcm[i] = WavetableBuilder::quantiseToPcm16 (built.frames.samples[i]);

    data->hash = toHash (WavetableBuilder::fnv1a64 (data->pcm));

    const auto key = "user:" + data->hash;
    r.table = WavetableStore::findShared (key);

    if (r.table == nullptr)
    {
        r.table = std::shared_ptr<const Wavetable> (std::move (built.table));
        WavetableStore::addShared (key, r.table);
    }

    // Same file imported twice (or by another instance): share the data too
    if (auto shared = SharedUserData::get().find (data->hash); shared != nullptr && shared->pcm == data->pcm)
        r.data = std::move (shared);
    else
        r.data = std::move (data);

    return r;
}

} // namespace

//==============================================================================
WavetableWorker::WavetableWorker (WavetableStore& s)
    : juce::Thread ("VocalSynth wavetables"),
      store (s)
{
    for (auto& r : requested)
        r.reference = factoryReference (synth::defaultFactoryTable);

    startThread (juce::Thread::Priority::low);
}

WavetableWorker::~WavetableWorker()
{
    stop();
}

void WavetableWorker::stop()
{
    {
        const std::scoped_lock l (lock);
        stopped = true;

        for (auto& p : pending)
            p.reset();

        cancel.store (true, std::memory_order_relaxed);
    }

    signalThreadShouldExit();
    notify();

    // Bounded: the build polls the cancel flag between frames; a file read is at most ~70 MB
    stopThread (30000);
}

//==============================================================================
bool WavetableWorker::isNewestLocked (int slot, uint64_t sequence) const
{
    return latestSequence[static_cast<std::size_t> (slot)] == sequence;
}

uint64_t WavetableWorker::beginRequestLocked (int slot)
{
    const auto s = static_cast<std::size_t> (slot);
    pending[s].reset();

    if (runningSlot == slot)
        cancel.store (true, std::memory_order_relaxed);

    return ++latestSequence[s];
}

void WavetableWorker::enqueue (Job job)
{
    {
        const std::scoped_lock l (lock);

        if (stopped)
            return;

        job.sequence = beginRequestLocked (job.slot);
        job.order = nextOrder++;

        if (job.kind == Job::Kind::factory || job.kind == Job::Kind::embedded)
        {
            // Factory / embedded: the reference counts from the request on (12.2)
            auto& r = requested[static_cast<std::size_t> (job.slot)];

            if (job.kind == Job::Kind::factory)
                r = { factoryReference (*factoryFromId (job.factoryId)), nullptr, nullptr };
            else
                r = { userReference (*job.data), nullptr, job.data };
        }

        const auto slot = static_cast<std::size_t> (job.slot);
        pending[slot] = std::move (job);
    }

    notify();
}

void WavetableWorker::requestFactory (int slot, const juce::String& factoryId)
{
    if (slot < 0 || slot >= WavetableStore::numSlots)
        return;

    if (! factoryFromId (factoryId).has_value())
    {
        failRequest (slot, "Unknown factory table: " + factoryId);
        return;
    }

    Job job;
    job.kind = Job::Kind::factory;
    job.slot = slot;
    job.factoryId = factoryId;
    enqueue (std::move (job));
}

void WavetableWorker::requestImport (int slot, const juce::File& file, int frameSizeOverride)
{
    if (slot < 0 || slot >= WavetableStore::numSlots)
        return;

    Job job;
    job.kind = Job::Kind::import;
    job.slot = slot;
    job.file = file;
    job.frameSizeOverride = frameSizeOverride;
    enqueue (std::move (job));
}

void WavetableWorker::requestEmbedded (int slot, std::shared_ptr<const UserTableData> data)
{
    if (slot < 0 || slot >= WavetableStore::numSlots)
        return;

    juce::String error;
    auto prepared = prepareEmbedded (std::move (data), error);

    if (prepared == nullptr)
    {
        failRequest (slot, error);
        return;
    }

    Job job;
    job.kind = Job::Kind::embedded;
    job.slot = slot;
    job.data = std::move (prepared);
    enqueue (std::move (job));
}

//==============================================================================
bool WavetableWorker::loadFactoryNow (int slot, const juce::String& factoryId, juce::String& errorMessage)
{
    if (slot < 0 || slot >= WavetableStore::numSlots)
    {
        errorMessage = "Invalid oscillator";
        return false;
    }

    auto table = factoryFromId (factoryId);
    const bool ok = table.has_value();

    if (! ok)
    {
        errorMessage = "Unknown factory table: " + factoryId;
        table = synth::defaultFactoryTable;
    }

    const auto reference = factoryReference (*table);
    uint64_t sequence = 0;

    {
        const std::scoped_lock l (lock);
        sequence = beginRequestLocked (slot);
        requested[static_cast<std::size_t> (slot)] = { reference, nullptr, nullptr };
        ++syncLoadsRunning;
    }

    std::shared_ptr<const Wavetable> built;

    try
    {
        built = getFactoryTable (*table, nullptr);
    }
    catch (const std::bad_alloc&)
    {
        built = nullptr;
    }

    if (built == nullptr)
    {
        // Out of memory: nothing to publish, the slot keeps what it has
        errorMessage = "Out of memory";
        fail (slot, sequence, errorMessage);
        finishSync();
        return false;
    }

    finish (slot, sequence, std::move (built), reference, nullptr);

    if (! ok)
    {
        const std::scoped_lock l (lock);
        status.errorSlot = slot;
        status.lastError = errorMessage;
    }

    finishSync();
    return ok;
}

bool WavetableWorker::loadEmbeddedNow (int slot, std::shared_ptr<const UserTableData> data, juce::String& errorMessage)
{
    if (slot < 0 || slot >= WavetableStore::numSlots)
    {
        errorMessage = "Invalid oscillator";
        return false;
    }

    juce::String error;
    std::shared_ptr<const UserTableData> prepared;

    try
    {
        prepared = prepareEmbedded (std::move (data), error);
    }
    catch (const std::bad_alloc&)
    {
        error = "Out of memory";
    }

    if (prepared == nullptr)
    {
        // Missing / broken -> the default table (12.2, 13.1); the error stays visible in the status
        juce::String ignored;
        loadFactoryNow (slot, defaultFactoryId(), ignored);
        errorMessage = error;

        const std::scoped_lock l (lock);
        status.errorSlot = slot;
        status.lastError = error;
        return false;
    }

    const auto reference = userReference (*prepared);
    uint64_t sequence = 0;

    {
        const std::scoped_lock l (lock);
        sequence = beginRequestLocked (slot);
        requested[static_cast<std::size_t> (slot)] = { reference, nullptr, prepared };
        ++syncLoadsRunning;
    }

    std::shared_ptr<const Wavetable> built;

    try
    {
        built = getUserTable (*prepared, nullptr);
    }
    catch (const std::bad_alloc&)
    {
        built = nullptr;
    }

    if (built == nullptr)
    {
        finishSync();
        juce::String ignored;
        loadFactoryNow (slot, defaultFactoryId(), ignored);
        errorMessage = "Could not build " + prepared->name;

        const std::scoped_lock l (lock);
        status.errorSlot = slot;
        status.lastError = errorMessage;
        return false;
    }

    SharedUserData::get().add (prepared);
    finish (slot, sequence, std::move (built), reference, prepared);
    finishSync();
    return true;
}

void WavetableWorker::finishSync()
{
    const std::scoped_lock l (lock);
    --syncLoadsRunning;
    ++status.completed;
}

//==============================================================================
void WavetableWorker::finish (int slot, uint64_t sequence, std::shared_ptr<const synth::Wavetable> table,
                              const WavetableReference& reference, std::shared_ptr<const UserTableData> userData)
{
    const std::scoped_lock l (lock);

    if (! isNewestLocked (slot, sequence))
        return; // a newer request (sync or async) owns the slot: never overwrite it

    requested[static_cast<std::size_t> (slot)] = { reference, table, userData };
    store.publish (slot, std::move (table), reference, std::move (userData));

    if (status.errorSlot == slot)
    {
        status.errorSlot = -1;
        status.lastError.clear();
    }
}

void WavetableWorker::fail (int slot, uint64_t sequence, const juce::String& message)
{
    const std::scoped_lock l (lock);

    if (! isNewestLocked (slot, sequence))
        return; // superseded: the newer request decides what the slot holds and shows

    status.errorSlot = slot;
    status.lastError = message;

    // The slot keeps its table: save what plays, not the failed request
    auto current = store.getSlot (slot);

    if (current.table != nullptr)
        requested[static_cast<std::size_t> (slot)] = std::move (current);
}

void WavetableWorker::failRequest (int slot, const juce::String& message)
{
    uint64_t sequence = 0;

    {
        const std::scoped_lock l (lock);
        sequence = beginRequestLocked (slot);
        ++status.completed;
    }

    fail (slot, sequence, message);
}

WavetableSlotInfo WavetableWorker::getRequestedReference (int slot) const
{
    if (slot < 0 || slot >= WavetableStore::numSlots)
        return {};

    const std::scoped_lock l (lock);
    return requested[static_cast<std::size_t> (slot)];
}

WavetableWorker::Status WavetableWorker::getStatus() const
{
    const std::scoped_lock l (lock);
    auto s = status;
    s.busy = runningSlot >= 0 || syncLoadsRunning > 0 || pending[0].has_value() || pending[1].has_value();
    return s;
}

bool WavetableWorker::waitUntilIdle (int timeoutMs)
{
    const auto end = juce::Time::getMillisecondCounterHiRes() + static_cast<double> (timeoutMs);

    for (;;)
    {
        if (! getStatus().busy)
            return true;

        if (juce::Time::getMillisecondCounterHiRes() >= end)
            return false;

        juce::Thread::sleep (2);
    }
}

//==============================================================================
void WavetableWorker::processJob (const Job& job)
{
    switch (job.kind)
    {
        case Job::Kind::factory:
        {
            const auto table = factoryFromId (job.factoryId);
            auto built = table.has_value() ? getFactoryTable (*table, &cancel) : nullptr;

            if (built != nullptr)
                finish (job.slot, job.sequence, std::move (built), factoryReference (*table), nullptr);
            else if (! cancel.load (std::memory_order_relaxed))
                fail (job.slot, job.sequence, "Could not build " + job.factoryId);

            break;
        }

        case Job::Kind::embedded:
        {
            auto built = getUserTable (*job.data, &cancel);

            if (built != nullptr)
            {
                SharedUserData::get().add (job.data);
                finish (job.slot, job.sequence, std::move (built), userReference (*job.data), job.data);
            }
            else if (! cancel.load (std::memory_order_relaxed))
            {
                fail (job.slot, job.sequence, "Could not build " + job.data->name);
            }

            break;
        }

        case Job::Kind::import:
        {
            auto result = importFile (job.file, job.frameSizeOverride, &cancel);

            if (result.table != nullptr)
            {
                SharedUserData::get().add (result.data);
                finish (job.slot, job.sequence, std::move (result.table), userReference (*result.data), result.data);
            }
            else if (! result.cancelled)
            {
                fail (job.slot, job.sequence, result.error);
            }

            break;
        }
    }
}

void WavetableWorker::run()
{
    auto nextGarbage = juce::Time::getMillisecondCounter() + static_cast<juce::uint32> (garbageIntervalMs);

    while (! threadShouldExit())
    {
        std::optional<Job> job;

        {
            const std::scoped_lock l (lock);

            if (! stopped)
            {
                // Oldest queued request first
                std::optional<std::size_t> pick;

                for (std::size_t s = 0; s < pending.size(); ++s)
                    if (pending[s].has_value() && (! pick.has_value() || pending[s]->order < pending[*pick]->order))
                        pick = s;

                if (pick.has_value())
                {
                    job = std::move (pending[*pick]);
                    pending[*pick].reset();
                    runningSlot = job->slot;
                    cancel.store (false, std::memory_order_relaxed);
                }
            }
        }

        if (job.has_value())
        {
            try
            {
                processJob (*job);
            }
            catch (const std::bad_alloc&)
            {
                fail (job->slot, job->sequence, "Out of memory");
            }

            const std::scoped_lock l (lock);
            runningSlot = -1;
            ++status.completed;
        }

        const auto now = juce::Time::getMillisecondCounter();

        if (static_cast<juce::int32> (now - nextGarbage) >= 0)
        {
            store.collectGarbage();
            nextGarbage = now + static_cast<juce::uint32> (garbageIntervalMs);
        }

        if (! job.has_value())
            wait (garbageIntervalMs);
    }
}

} // namespace vocalsynth
