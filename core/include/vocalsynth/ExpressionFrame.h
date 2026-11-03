#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <span>

namespace vocalsynth
{

/** Voice expression of one analysis hop (docs/SYNTH-DESIGN.md 16), produced by the ExpressionTracker inside
    the VoicePipeline and consumed by the SynthEngine as mod sources. Plain data. 0.5 appends vowelX and
    vowelY at the END (never reorder: tests and the engine use the names, nothing is persisted).

    sampleOffset follows the event rule of the VoicePipeline: the offset (within the process() call) of the
    host input sample that completed the hop, so absolute positions never depend on the block split. */
struct ExpressionFrame
{
    int sampleOffset = 0;
    float level = 0.0f;      ///< 0..1: smoothed RMS between the gate threshold (0) and -6 dBFS (1)
    float brightness = 0.0f; ///< 0..1: spectral centroid, log-normalised 200 Hz (0) .. 5 kHz (1)
    float noisiness = 0.0f;  ///< 0..1: spectral flatness combined with 1 - pitch confidence
    float pitchCents = 0.0f; ///< -100..100: sung pitch minus the active note's centre, 0 without a note

    constexpr bool operator== (const ExpressionFrame&) const noexcept = default;
};

/** Values without a voice: what the engine uses when no ExpressionBuffer is given (Test Pattern) -
    level 1 while a note is held, everything else 0 (docs/SYNTH-DESIGN.md 16.4). The engine smooths the
    rest level per tick like the tracker (attack 5 ms, release 50 ms), so Voice Level -> Amp does not click. */
constexpr ExpressionFrame restExpression (bool noteSounding) noexcept
{
    return { 0, noteSounding ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f };
}

/** Fixed-capacity list of ExpressionFrames for one chunk (<= 4096 samples): at most one frame per hop
    (2 ms) plus one, i.e. 257 at 8 kHz. Never allocates: push() returns false when full. Frames are pushed
    in time order. Single-threaded use. */
class ExpressionBuffer
{
public:
    static constexpr std::size_t capacity = 512;

    bool push (const ExpressionFrame& f) noexcept
    {
        if (count >= capacity)
            return false;

        frames[count++] = f;
        return true;
    }

    void clear() noexcept { count = 0; }
    std::size_t size() const noexcept { return count; }
    bool empty() const noexcept { return count == 0; }
    const ExpressionFrame& operator[] (std::size_t i) const noexcept { return frames[i]; }
    std::span<const ExpressionFrame> view() const noexcept { return { frames.data(), count }; }

private:
    std::array<ExpressionFrame, capacity> frames {};
    std::size_t count = 0;
};

/** Newest expression values for the UI meters (30 Hz): relaxed atomics, single writer (audio thread),
    any reader. The four values may come from two neighbouring hops (harmless for meters). */
class ExpressionPublisher
{
public:
    void publish (const ExpressionFrame& f) noexcept
    {
        level.store (f.level, std::memory_order_relaxed);
        brightness.store (f.brightness, std::memory_order_relaxed);
        noisiness.store (f.noisiness, std::memory_order_relaxed);
        pitchCents.store (f.pitchCents, std::memory_order_relaxed);
    }

    ExpressionFrame read() const noexcept
    {
        return { 0, level.load (std::memory_order_relaxed), brightness.load (std::memory_order_relaxed),
                 noisiness.load (std::memory_order_relaxed), pitchCents.load (std::memory_order_relaxed) };
    }

private:
    std::atomic<float> level { 0.0f };
    std::atomic<float> brightness { 0.0f };
    std::atomic<float> noisiness { 0.0f };
    std::atomic<float> pitchCents { 0.0f };
    static_assert (std::atomic<float>::is_always_lock_free);
};

} // namespace vocalsynth
