#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <type_traits>

namespace vocalsynth
{

/** Bounded single-producer / single-consumer ring of trivially copyable items (docs/M3-DESIGN.md 3).

    Producer = audio thread (CaptureTap), consumer = capture worker thread. The capacity is a power of two
    fixed by prepare(); push and pop never block, lock or allocate. Indices are monotonically increasing
    std::atomic<std::size_t> counters (head = items pushed, tail = items popped), published with release
    and read with acquire; each side caches the other side's index and re-reads it only when the cached
    value says "full" / "empty". Full = head - tail == capacity (no wasted slot).

    Overflow: a push that does not fit stores nothing, returns false / 0 and adds the number of dropped
    items to the overflow counter (relaxed atomic, read by any thread). The caller decides what a drop
    means (the CaptureTap turns it into a gap record).

    Threading: prepare() and reset() are NOT concurrent with anything (the plugin suspends the worker and
    the audio thread is not running in prepareToPlay). push / tryPushAll / freeSpace = producer only;
    pop / popBulk / size = consumer only; getCapacity / getOverflowCount = any thread. */
template <typename T>
class SpscRing
{
    static_assert (std::is_trivially_copyable_v<T>);
    static_assert (std::atomic<std::size_t>::is_always_lock_free);
    static_assert (std::atomic<uint64_t>::is_always_lock_free);

public:
    SpscRing() = default;
    SpscRing (const SpscRing&) = delete;
    SpscRing& operator= (const SpscRing&) = delete;

    /** Allocates max (1, minCapacity) rounded up to a power of two and clears the ring and the overflow
        counter. NOT real-time; may throw std::bad_alloc. Keeps the storage if the capacity is unchanged. */
    void prepare (std::size_t minCapacity)
    {
        // TODO(A): implement (docs/M3-DESIGN.md 3)
        (void) minCapacity;
    }

    /** Empties the ring (keeps the storage and the overflow counter). Not concurrent with push/pop. */
    void reset() noexcept
    {
        // TODO(A)
    }

    /** Producer: appends one item; false (and overflow + 1) if full. */
    bool push (const T& item) noexcept
    {
        // TODO(A)
        (void) item;
        return false;
    }

    /** Producer: appends all items or none (overflow + items.size() if they do not fit). */
    bool tryPushAll (std::span<const T> items) noexcept
    {
        // TODO(A)
        (void) items;
        return false;
    }

    /** Producer: free slots as seen by the producer (a lower bound: the consumer may free more). */
    std::size_t freeSpace() const noexcept
    {
        // TODO(A)
        return 0;
    }

    /** Consumer: removes the oldest item; false if empty. */
    bool pop (T& item) noexcept
    {
        // TODO(A)
        (void) item;
        return false;
    }

    /** Consumer: removes up to destination.size() items into destination, returns the number popped. */
    std::size_t popBulk (std::span<T> destination) noexcept
    {
        // TODO(A)
        (void) destination;
        return 0;
    }

    /** Consumer: items ready to pop (a lower bound: the producer may add more). */
    std::size_t size() const noexcept
    {
        // TODO(A)
        return 0;
    }

    std::size_t getCapacity() const noexcept { return capacity; }

    /** Items dropped because the ring was full, since prepare(). Any thread. */
    uint64_t getOverflowCount() const noexcept { return overflowCount.load (std::memory_order_relaxed); }

private:
    std::unique_ptr<T[]> storage;
    std::size_t capacity = 0;
    std::size_t mask = 0;

    // Producer side (own cache line in the implementation: TODO(A) alignas (64) as needed)
    std::atomic<std::size_t> head { 0 };
    std::size_t cachedTail = 0;

    // Consumer side
    std::atomic<std::size_t> tail { 0 };
    std::size_t cachedHead = 0;

    std::atomic<uint64_t> overflowCount { 0 };
};

} // namespace vocalsynth
