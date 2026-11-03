#pragma once

#include "vocalsynth/synth/Wavetable.h"
#include "vocalsynth/synth/WavetableOscillator.h"

#include <juce_core/juce_core.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace vocalsynth
{

/** Where an oscillator's table comes from (state, presets, UI). Text form: "factory:<id>" or "user:<hash>"
    (16 lower-case hex digits of WavetableBuilder::fnv1a64 of the PCM16 frames). docs/SYNTH-DESIGN.md 12, 13. */
struct WavetableReference
{
    enum class Kind
    {
        factory,
        user
    };

    Kind kind = Kind::factory;
    juce::String id = "basic-shapes"; ///< factory id, or the user table's hash
    juce::String name;                ///< display name (factory display name / imported file name)

    juce::String toRefString() const;

    /** Parses "factory:<id>" / "user:<hash>"; anything else -> the default factory table. */
    static WavetableReference fromRefString (const juce::String& ref, const juce::String& name = {});

    static WavetableReference defaultFactory();

    bool operator== (const WavetableReference& other) const noexcept
    {
        return kind == other.kind && id == other.id && name == other.name;
    }
};

/** The normalised PCM16 frames of a user table (import or project), kept so the state can embed them.
    Immutable once shared. */
struct UserTableData
{
    juce::String name;
    juce::String hash;           ///< == WavetableReference::id
    int numFrames = 0;
    std::vector<int16_t> pcm;    ///< numFrames * synth::Wavetable::frameSize values
};

/** What a slot holds (message thread view). */
struct WavetableSlotInfo
{
    WavetableReference reference;
    std::shared_ptr<const synth::Wavetable> table;     ///< null until the first table is published
    std::shared_ptr<const UserTableData> userData;     ///< user tables only
};

/** Owns the tables of the two oscillator slots and hands them to the audio thread lock-free
    (docs/SYNTH-DESIGN.md 12). Protocol (binding):
    - publish() (worker / message thread): slots[s].exchange (new) (seq_cst); the old table goes to the
      retire list with retireBlock = blockCounter.load (seq_cst), read AFTER the exchange.
    - audio thread, EVERY block (processBlock and processBlockBypassed): block start acquire (s) =
      slots[s].load (seq_cst) -> engine.setWavetable(); block end release (inUseA, inUseB) stores the four
      pointers the engine still reads (current / previous of each oscillator) into inUse[] (release) and
      then blockCounter.fetch_add (1, seq_cst).
    - collectGarbage() (worker every 100 ms, message thread): a retired table is freed (its shared_ptr
      dropped) when blockCounter.load (seq_cst) >= retireBlock + 2 (a whole block started after the
      exchange and ended) and no inUse[] entry (acquire) equals it. seq_cst on the slot exchange / load and
      the counter is what makes "the block after retireBlock saw the new pointer" true in the C++ memory
      model (with acquire / release only, a load reading N does not order the worker's exchange before the
      audio thread's next slot load); on x86 the loads stay plain moves.
    - releaseAllRetired(): audio not running (prepareToPlay, releaseResources, destructor) AND the engine
      has been reset (SynthEngine::reset() forgets every pointer, so no pointer of an earlier block can be
      read again): clears inUse[] and frees every retired table.
    - Retired entries hold shared_ptrs: re-publishing a table that is still current (ABA by sharing)
      only drops one reference. While the host does not call processBlock (suspended without
      releaseResources) retired tables wait; they are bounded by the user's table changes.
    - The audio thread never allocates, frees, locks or touches a shared_ptr. Tables are immutable.
    - Process-wide sharing: findShared / addShared keep weak references by key ("factory:<id>" or
      "user:<hash>"), so instances share identical tables and a factory table is generated once while used;
      the default table "factory:basic-shapes" is also kept by a process-wide strong reference.
    - getSlot() returns the slot as last PUBLISHED; the state saves the last REQUESTED reference
      (WavetableWorker::getRequestedReference), so a save right after a request never loses it.
    Owner: agent D. */
class WavetableStore
{
public:
    static constexpr int numSlots = 2;

    WavetableStore();
    ~WavetableStore();

    // --- message / worker threads (internal mutex, never taken by the audio thread) ---
    void publish (int slot, std::shared_ptr<const synth::Wavetable> table, const WavetableReference& reference,
                  std::shared_ptr<const UserTableData> userData = {});
    WavetableSlotInfo getSlot (int slot) const;

    /** Increments with every publish (the UI redraws the wavetable view when it changes). Any thread. */
    uint32_t getGeneration() const noexcept { return generation.load (std::memory_order_acquire); }

    void collectGarbage();
    void releaseAllRetired();
    int getNumRetired() const;

    // --- audio thread (wait-free) ---
    const synth::Wavetable* acquire (int slot) const noexcept;
    void release (const synth::TablesInUse& oscA, const synth::TablesInUse& oscB) noexcept;

    // --- process-wide sharing (any non-audio thread) ---
    static std::shared_ptr<const synth::Wavetable> findShared (const juce::String& key);
    static void addShared (const juce::String& key, const std::shared_ptr<const synth::Wavetable>& table);

private:
    struct Retired
    {
        std::shared_ptr<const synth::Wavetable> table;
        uint64_t retireBlock = 0;
    };

    std::array<std::atomic<const synth::Wavetable*>, numSlots> slots {};
    std::array<std::atomic<const synth::Wavetable*>, 2 * numSlots> inUse {};
    std::atomic<uint64_t> blockCounter { 0 };
    std::atomic<uint32_t> generation { 0 };

    mutable std::mutex mutex;
    std::array<WavetableSlotInfo, numSlots> info;
    std::vector<Retired> retired;
};

} // namespace vocalsynth
