#include "WavetableStore.h"

#include <algorithm>
#include <map>
#include <string>

namespace vocalsynth
{

juce::String WavetableReference::toRefString() const
{
    return (kind == Kind::factory ? "factory:" : "user:") + id;
}

WavetableReference WavetableReference::fromRefString (const juce::String& ref, const juce::String& name)
{
    WavetableReference r;

    if (ref.startsWith ("factory:") && ref.length() > 8)
    {
        r.id = ref.substring (8);
    }
    else if (ref.startsWith ("user:") && ref.length() > 5)
    {
        r.kind = Kind::user;
        r.id = ref.substring (5);
    }

    r.name = name;
    return r;
}

WavetableReference WavetableReference::defaultFactory()
{
    return {};
}

//==============================================================================
namespace
{

/** Process-wide table sharing (docs/SYNTH-DESIGN.md 12.3): key -> weak reference, plus one strong reference
    to the default table so that constructing instances (scans, pluginval, the plugin tests) builds it once.
    std::string keys: no JUCE object in a static that outlives the JUCE runtime. Never the audio thread. */
struct SharedTables
{
    std::mutex mutex;
    std::map<std::string, std::weak_ptr<const synth::Wavetable>> tables;
    std::shared_ptr<const synth::Wavetable> defaultTable;

    static SharedTables& get()
    {
        static SharedTables instance;
        return instance;
    }
};

const std::string defaultSharedKey = "factory:basic-shapes";

} // namespace

std::shared_ptr<const synth::Wavetable> WavetableStore::findShared (const juce::String& key)
{
    auto& shared = SharedTables::get();
    const std::scoped_lock lock (shared.mutex);
    const auto it = shared.tables.find (key.toStdString());
    return it != shared.tables.end() ? it->second.lock() : nullptr;
}

void WavetableStore::addShared (const juce::String& key, const std::shared_ptr<const synth::Wavetable>& table)
{
    if (table == nullptr || key.isEmpty())
        return;

    auto& shared = SharedTables::get();
    const auto k = key.toStdString();
    std::shared_ptr<const synth::Wavetable> dropped; // released after the lock

    const std::scoped_lock lock (shared.mutex);

    // Forget entries whose tables are gone, so the map stays as small as the set of live tables
    for (auto it = shared.tables.begin(); it != shared.tables.end();)
        it = it->second.expired() ? shared.tables.erase (it) : std::next (it);

    shared.tables[k] = table;

    if (k == defaultSharedKey)
    {
        dropped = std::move (shared.defaultTable);
        shared.defaultTable = table;
    }
}

//==============================================================================
WavetableStore::WavetableStore() = default;

WavetableStore::~WavetableStore()
{
    // The owner destroys the engine (or has reset it) before the store, and the worker before both
    // (docs/SYNTH-DESIGN.md 17): nothing reads the tables any more.
    releaseAllRetired();
}

void WavetableStore::publish (int slot, std::shared_ptr<const synth::Wavetable> table,
                              const WavetableReference& reference, std::shared_ptr<const UserTableData> userData)
{
    if (slot < 0 || slot >= numSlots)
        return;

    std::shared_ptr<const synth::Wavetable> old;

    {
        const std::scoped_lock lock (mutex);
        auto& s = info[static_cast<std::size_t> (slot)];
        old = std::move (s.table);
        s = { reference, std::move (table), std::move (userData) };

        // 12.1: exchange first, THEN read the counter (seq_cst both): every block numbered above retireBlock
        // starts after the exchange in the single total order and loads the new pointer.
        const auto* previous = slots[static_cast<std::size_t> (slot)].exchange (s.table.get(), std::memory_order_seq_cst);
        const auto retireBlock = blockCounter.load (std::memory_order_seq_cst);

        // Re-publishing the same table (ABA by sharing) retires nothing: the slot keeps it alive.
        if (old != nullptr && old.get() != s.table.get())
        {
            jassert (previous == old.get());
            retired.push_back ({ std::move (old), retireBlock });
        }

        juce::ignoreUnused (previous);
        generation.fetch_add (1, std::memory_order_acq_rel);
    }

    // old is either retired (moved) or the same object as the new table: dropping it here frees nothing
}

WavetableSlotInfo WavetableStore::getSlot (int slot) const
{
    if (slot < 0 || slot >= numSlots)
        return {};

    const std::scoped_lock lock (mutex);
    return info[static_cast<std::size_t> (slot)];
}

void WavetableStore::collectGarbage()
{
    std::vector<std::shared_ptr<const synth::Wavetable>> freed; // destroyed after the lock (may be 23 MB)

    {
        const std::scoped_lock lock (mutex);

        if (retired.empty())
            return;

        // Counter first (seq_cst, synchronises with the audio thread's fetch_add), then inUse (acquire):
        // the inUse values seen are at least those of the block that brought the counter to this value.
        const auto blocks = blockCounter.load (std::memory_order_seq_cst);
        std::array<const synth::Wavetable*, 2 * numSlots> used {};

        for (std::size_t i = 0; i < used.size(); ++i)
            used[i] = inUse[i].load (std::memory_order_acquire);

        const auto isUsed = [&used] (const synth::Wavetable* t)
        { return std::find (used.begin(), used.end(), t) != used.end(); };

        for (auto it = retired.begin(); it != retired.end();)
        {
            if (blocks >= it->retireBlock + 2 && ! isUsed (it->table.get()))
            {
                freed.push_back (std::move (it->table));
                it = retired.erase (it);
            }
            else
            {
                ++it;
            }
        }
    }
}

void WavetableStore::releaseAllRetired()
{
    std::vector<Retired> freed;

    {
        const std::scoped_lock lock (mutex);

        for (auto& p : inUse)
            p.store (nullptr, std::memory_order_seq_cst);

        freed.swap (retired);
    }
}

int WavetableStore::getNumRetired() const
{
    const std::scoped_lock lock (mutex);
    return static_cast<int> (retired.size());
}

const synth::Wavetable* WavetableStore::acquire (int slot) const noexcept
{
    if (slot < 0 || slot >= numSlots)
        return nullptr;

    return slots[static_cast<std::size_t> (slot)].load (std::memory_order_seq_cst);
}

void WavetableStore::release (const synth::TablesInUse& oscA, const synth::TablesInUse& oscB) noexcept
{
    inUse[0].store (oscA.current, std::memory_order_release);
    inUse[1].store (oscA.previous, std::memory_order_release);
    inUse[2].store (oscB.current, std::memory_order_release);
    inUse[3].store (oscB.previous, std::memory_order_release);
    blockCounter.fetch_add (1, std::memory_order_seq_cst);
}

} // namespace vocalsynth
