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
    - publish() (worker / message thread): slots[s].exchange (new) (acq_rel); the old table goes to the
      retire list with retireBlock = blockCounter.load (acquire).
    - audio thread, block start: acquire (s) = slots[s].load (acquire) -> engine.setWavetable();
      block end: release (inUseA, inUseB) stores the four pointers the engine still reads (current /
      previous of each oscillator) into inUse[] (release) and then blockCounter.fetch_add (1, release).
    - collectGarbage() (worker every 100 ms, message thread): a retired table is freed (its shared_ptr
      dropped) when blockCounter >= retireBlock + 2 (a whole block started after the exchange) and no inUse[]
      entry equals it. releaseAllRetired(): audio not running (prepareToPlay, releaseResources, destructor):
      clears inUse[] and frees every retired table.
    - The audio thread never allocates, frees, locks or touches a shared_ptr. Tables are immutable.
    - Process-wide sharing: findShared / addShared keep weak references by key ("factory:<id>" or
      "user:<hash>"), so instances share identical tables and a factory table is generated once while used.
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
