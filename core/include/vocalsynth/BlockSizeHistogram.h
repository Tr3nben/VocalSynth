#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace vocalsynth
{

/** Lock-free statistics of the host's block sizes, for the M1 FL Studio check
    ("Use fixed size buffers" on/off). record() is called by the audio thread only; snapshot() and
    reset() by the UI thread. All atomics use relaxed ordering: the numbers are informative, a
    reset() racing with record() may lose one block, which is acceptable.

    Bins: [1], [2-16], [17-32], [33-64], [65-128], [129-256], [257-512], [513-1024], [>1024].
    Blocks with numSamples <= 0 are counted in emptyBlocks only (not in the bins, min, max, total). */
class BlockSizeHistogram
{
public:
    static constexpr std::size_t numBins = 9;

    struct Snapshot
    {
        std::array<uint64_t, numBins> counts {};
        uint64_t totalBlocks = 0; ///< blocks with numSamples >= 1
        uint64_t emptyBlocks = 0; ///< blocks with numSamples <= 0
        int minBlockSize = 0;     ///< 0 if no block recorded yet
        int maxBlockSize = 0;
        int lastBlockSize = 0;
    };

    /** Bin index for a block size >= 1. */
    static constexpr std::size_t binIndex (int numSamples) noexcept
    {
        constexpr int upperBounds[numBins - 1] = { 1, 16, 32, 64, 128, 256, 512, 1024 };

        for (std::size_t i = 0; i < numBins - 1; ++i)
            if (numSamples <= upperBounds[i])
                return i;

        return numBins - 1;
    }

    /** Short label for the UI, e.g. "1", "2-16", ">1024". */
    static constexpr const char* binLabel (std::size_t bin) noexcept
    {
        constexpr const char* labels[numBins] = { "1",       "2-16",     "17-32",     "33-64", "65-128",
                                                  "129-256", "257-512",  "513-1024",  ">1024" };
        return bin < numBins ? labels[bin] : "";
    }

    /** Audio thread. Wait-free. */
    void record (int numSamples) noexcept
    {
        if (numSamples <= 0)
        {
            emptyBlocks.fetch_add (1, std::memory_order_relaxed);
            return;
        }

        counts[binIndex (numSamples)].fetch_add (1, std::memory_order_relaxed);
        totalBlocks.fetch_add (1, std::memory_order_relaxed);
        lastBlockSize.store (numSamples, std::memory_order_relaxed);

        // Single writer: a plain load/compare/store is enough (no CAS loop). 0 means "nothing yet".
        const auto currentMin = minBlockSize.load (std::memory_order_relaxed);

        if (currentMin == 0 || numSamples < currentMin)
            minBlockSize.store (numSamples, std::memory_order_relaxed);

        if (numSamples > maxBlockSize.load (std::memory_order_relaxed))
            maxBlockSize.store (numSamples, std::memory_order_relaxed);
    }

    /** UI thread. */
    Snapshot snapshot() const noexcept
    {
        Snapshot s;

        for (std::size_t i = 0; i < numBins; ++i)
            s.counts[i] = counts[i].load (std::memory_order_relaxed);

        s.totalBlocks = totalBlocks.load (std::memory_order_relaxed);
        s.emptyBlocks = emptyBlocks.load (std::memory_order_relaxed);
        s.minBlockSize = minBlockSize.load (std::memory_order_relaxed);
        s.maxBlockSize = maxBlockSize.load (std::memory_order_relaxed);
        s.lastBlockSize = lastBlockSize.load (std::memory_order_relaxed);
        return s;
    }

    /** UI thread (also safe before playback starts). */
    void reset() noexcept
    {
        for (auto& c : counts)
            c.store (0, std::memory_order_relaxed);

        totalBlocks.store (0, std::memory_order_relaxed);
        emptyBlocks.store (0, std::memory_order_relaxed);
        minBlockSize.store (0, std::memory_order_relaxed);
        maxBlockSize.store (0, std::memory_order_relaxed);
        lastBlockSize.store (0, std::memory_order_relaxed);
    }

private:
    std::array<std::atomic<uint64_t>, numBins> counts {};
    std::atomic<uint64_t> totalBlocks { 0 };
    std::atomic<uint64_t> emptyBlocks { 0 };
    std::atomic<int> minBlockSize { 0 };
    std::atomic<int> maxBlockSize { 0 };
    std::atomic<int> lastBlockSize { 0 };

    static_assert (std::atomic<uint64_t>::is_always_lock_free);
    static_assert (std::atomic<int>::is_always_lock_free);
};

} // namespace vocalsynth
