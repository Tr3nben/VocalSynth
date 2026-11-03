#include "WavetableStore.h"

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

// Scaffold (docs/SYNTH-DESIGN.md 12): publish keeps the table (no retire protocol yet). Agent D implements.

WavetableStore::WavetableStore() = default;

WavetableStore::~WavetableStore()
{
    releaseAllRetired();
}

void WavetableStore::publish (int slot, std::shared_ptr<const synth::Wavetable> table,
                              const WavetableReference& reference, std::shared_ptr<const UserTableData> userData)
{
    if (slot < 0 || slot >= numSlots)
        return;

    const std::scoped_lock lock (mutex);
    auto& s = info[static_cast<std::size_t> (slot)];
    auto old = std::move (s.table);
    s = { reference, std::move (table), std::move (userData) };
    slots[static_cast<std::size_t> (slot)].exchange (s.table.get(), std::memory_order_seq_cst);
    retired.push_back ({ std::move (old), blockCounter.load (std::memory_order_seq_cst) });
    generation.fetch_add (1, std::memory_order_acq_rel);
}

WavetableSlotInfo WavetableStore::getSlot (int slot) const
{
    if (slot < 0 || slot >= numSlots)
        return {};

    const std::scoped_lock lock (mutex);
    return info[static_cast<std::size_t> (slot)];
}

void WavetableStore::collectGarbage() {}

void WavetableStore::releaseAllRetired()
{
    const std::scoped_lock lock (mutex);

    for (auto& p : inUse)
        p.store (nullptr, std::memory_order_relaxed);

    retired.clear();
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

std::shared_ptr<const synth::Wavetable> WavetableStore::findShared (const juce::String&)
{
    return {};
}

void WavetableStore::addShared (const juce::String&, const std::shared_ptr<const synth::Wavetable>&) {}

} // namespace vocalsynth
