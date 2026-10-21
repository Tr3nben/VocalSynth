#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
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
        const auto wanted = std::bit_ceil (std::max (std::size_t { 1 }, minCapacity));

        if (wanted != capacity || storage == nullptr)
        {
            storage = std::make_unique<T[]> (wanted);
            capacity = wanted;
            mask = wanted - 1;
        }

        reset();
        overflowCount.store (0, std::memory_order_relaxed);
    }

    /** Empties the ring (keeps the storage and the overflow counter). Not concurrent with push/pop. */
    void reset() noexcept
    {
        head.store (0, std::memory_order_relaxed);
        cachedTail = 0;
        tail.store (0, std::memory_order_relaxed);
        cachedHead = 0;
    }

    /** Producer: appends one item; false (and overflow + 1) if full. */
    bool push (const T& item) noexcept
    {
        const auto h = head.load (std::memory_order_relaxed);

        if (h - cachedTail >= capacity)
        {
            cachedTail = tail.load (std::memory_order_acquire);

            if (h - cachedTail >= capacity)
            {
                addOverflow (1);
                return false;
            }
        }

        storage[h & mask] = item;
        head.store (h + 1, std::memory_order_release);
        return true;
    }

    /** Producer: appends all items or none (overflow + items.size() if they do not fit). */
    bool tryPushAll (std::span<const T> items) noexcept
    {
        const auto n = items.size();

        if (n == 0)
            return true;

        const auto h = head.load (std::memory_order_relaxed);

        if (capacity - (h - cachedTail) < n)
        {
            cachedTail = tail.load (std::memory_order_acquire);

            if (capacity - (h - cachedTail) < n)
            {
                addOverflow (n);
                return false;
            }
        }

        // At most two contiguous pieces (wrap-around)
        const auto first = std::min (n, capacity - (h & mask));
        std::copy_n (items.data(), first, storage.get() + (h & mask));
        std::copy_n (items.data() + first, n - first, storage.get());
        head.store (h + n, std::memory_order_release);
        return true;
    }

    /** Producer: free slots as seen by the producer (a lower bound: the consumer may free more). */
    std::size_t freeSpace() const noexcept
    {
        const auto h = head.load (std::memory_order_relaxed);
        return capacity - (h - tail.load (std::memory_order_acquire));
    }

    /** Consumer: removes the oldest item; false if empty. */
    bool pop (T& item) noexcept
    {
        const auto t = tail.load (std::memory_order_relaxed);

        if (t == cachedHead)
        {
            cachedHead = head.load (std::memory_order_acquire);

            if (t == cachedHead)
                return false;
        }

        item = storage[t & mask];
        tail.store (t + 1, std::memory_order_release);
        return true;
    }

    /** Consumer: removes up to destination.size() items into destination, returns the number popped. */
    std::size_t popBulk (std::span<T> destination) noexcept
    {
        const auto t = tail.load (std::memory_order_relaxed);

        if (cachedHead - t < destination.size())
            cachedHead = head.load (std::memory_order_acquire);

        const auto n = std::min (destination.size(), cachedHead - t);

        if (n == 0)
            return 0;

        const auto first = std::min (n, capacity - (t & mask));
        std::copy_n (storage.get() + (t & mask), first, destination.data());
        std::copy_n (storage.get(), n - first, destination.data() + first);
        tail.store (t + n, std::memory_order_release);
        return n;
    }

    /** Consumer: items ready to pop (a lower bound: the producer may add more). */
    std::size_t size() const noexcept
    {
        return head.load (std::memory_order_acquire) - tail.load (std::memory_order_relaxed);
    }

    std::size_t getCapacity() const noexcept { return capacity; }

    /** Items dropped because the ring was full, since prepare(). Any thread. */
    uint64_t getOverflowCount() const noexcept { return overflowCount.load (std::memory_order_relaxed); }

private:
    /** Single writer (the producer): load + store instead of a locked read-modify-write. */
    void addOverflow (std::size_t n) noexcept
    {
        overflowCount.store (overflowCount.load (std::memory_order_relaxed) + static_cast<uint64_t> (n),
                             std::memory_order_relaxed);
    }

    static constexpr std::size_t cacheLineBytes = 64;

    std::unique_ptr<T[]> storage;
    std::size_t capacity = 0;
    std::size_t mask = 0;

    // Producer and consumer fields on separate cache lines (no false sharing between the two threads).
    // Explicit padding instead of alignas: an over-aligned member would make every class that holds a
    // ring over-aligned (MSVC C4324 at /W4 in each of them).
    [[maybe_unused]] std::byte padding0[cacheLineBytes] {};
    std::atomic<std::size_t> head { 0 }; ///< items pushed (written by the producer)
    std::size_t cachedTail = 0;          ///< producer's copy of tail
    [[maybe_unused]] std::byte padding1[cacheLineBytes] {};
    std::atomic<std::size_t> tail { 0 }; ///< items popped (written by the consumer)
    std::size_t cachedHead = 0;          ///< consumer's copy of head
    [[maybe_unused]] std::byte padding2[cacheLineBytes] {};
    std::atomic<uint64_t> overflowCount { 0 }; ///< written by the producer only
};

} // namespace vocalsynth
