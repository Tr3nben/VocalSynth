#pragma once

#include "vocalsynth/VsEvent.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace vocalsynth
{

/** How the true time of an event was obtained (M3 capture, docs/M3-DESIGN.md 7). */
enum class EventTimingKind : uint8_t
{
    none,     ///< no estimate: the event happened when it was emitted (test pattern, flushes, CC)
    model,    ///< emit sample minus the latency model (computeFastOnsetLatencyMs / computeConfirmedLatencyMs)
    measured  ///< from the analysis (level envelope, change timer, frame centre)
};

/** Bits of VsEventTiming::flags. */
namespace EventTimingFlags
{
/** The note-on (and the note-off of the old note sent with it) corrects a provisional note that the full
    window read differently (v0.2.1 fast onset). Clean exports the corrected note from the provisional
    start and drops the provisional one; Raw keeps both as sent. */
inline constexpr uint8_t correction = 1u << 0;
/** The note-on is a legato or retrigger note change (no silence before it). */
inline constexpr uint8_t noteChange = 1u << 1;
} // namespace EventTimingFlags

/** Capture side output of the VoicePipeline for one emitted VsEvent (docs/M3-DESIGN.md 7). Plain data.
    delaySamples = emit sample - estimated true sample (host rate, normally >= 0). Relative to the event's
    own emit sample, so it does not depend on the pipeline's clock (which restarts on every reset()). */
struct VsEventTiming
{
    int32_t delaySamples = 0;
    EventTimingKind kind = EventTimingKind::none;
    uint8_t flags = 0;

    constexpr bool operator== (const VsEventTiming&) const noexcept = default;
};

/** Fixed-capacity list of VsEventTiming, used next to a VsEventBuffer of the same capacity: entry i belongs
    to event i. Never allocates: push() returns false when full. Single-threaded use. */
template <std::size_t Capacity>
class VsEventTimingBuffer
{
public:
    static constexpr std::size_t capacity = Capacity;

    bool push (const VsEventTiming& t) noexcept
    {
        if (count >= Capacity)
            return false;

        timings[count++] = t;
        return true;
    }

    void clear() noexcept { count = 0; }

    std::size_t size() const noexcept { return count; }
    bool empty() const noexcept { return count == 0; }

    const VsEventTiming& operator[] (std::size_t i) const noexcept { return timings[i]; }

    std::span<const VsEventTiming> view() const noexcept { return { timings.data(), count }; }

private:
    std::array<VsEventTiming, Capacity> timings {};
    std::size_t count = 0;
};

/** Non-owning, type-erased handle to any VsEventTimingBuffer<N> (like VsEventSink). Must not outlive the
    buffer. A default-constructed sink discards everything (no capture). */
class VsEventTimingSink
{
public:
    VsEventTimingSink() noexcept = default;

    template <std::size_t N>
    VsEventTimingSink (VsEventTimingBuffer<N>& buffer) noexcept // NOLINT: implicit on purpose
        : target (&buffer),
          pushFn ([] (void* t, const VsEventTiming& e) noexcept
                  { return static_cast<VsEventTimingBuffer<N>*> (t)->push (e); })
    {
    }

    /** Returns false if there is no buffer or it is full. */
    bool push (const VsEventTiming& t) const noexcept { return pushFn != nullptr && pushFn (target, t); }

    bool isConnected() const noexcept { return pushFn != nullptr; }

private:
    void* target = nullptr;
    bool (*pushFn) (void*, const VsEventTiming&) noexcept = nullptr;
};

} // namespace vocalsynth
