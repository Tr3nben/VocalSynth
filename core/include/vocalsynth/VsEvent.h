#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace vocalsynth
{

/** Kind of a VocalSynth event. Converted to MIDI only in plugin/ (MidiOutputStage). */
enum class VsEventType : uint8_t
{
    noteOn,
    noteOff,
    pitchBend,
    controlChange
};

/** One timestamped event produced by the core. Plain data, trivially copyable.
    sampleOffset is relative to the start of the current block (0 <= offset < numSamples). */
struct VsEvent
{
    VsEventType type = VsEventType::noteOn;
    int sampleOffset = 0;
    uint8_t channel = 1; ///< MIDI channel 1..16
    uint8_t data1 = 0;   ///< note number (noteOn/noteOff) or CC number (controlChange)
    uint8_t data2 = 0;   ///< velocity (noteOn/noteOff) or CC value (controlChange)
    int16_t bend = 0;    ///< -8192..8191, pitchBend only (0 = centre)

    static constexpr VsEvent makeNoteOn (int offset, uint8_t ch, uint8_t note, uint8_t velocity) noexcept
    {
        return { VsEventType::noteOn, offset, ch, note, velocity, 0 };
    }

    static constexpr VsEvent makeNoteOff (int offset, uint8_t ch, uint8_t note) noexcept
    {
        return { VsEventType::noteOff, offset, ch, note, 0, 0 };
    }

    static constexpr VsEvent makePitchBend (int offset, uint8_t ch, int16_t value) noexcept
    {
        return { VsEventType::pitchBend, offset, ch, 0, 0, value };
    }

    static constexpr VsEvent makeControlChange (int offset, uint8_t ch, uint8_t cc, uint8_t value) noexcept
    {
        return { VsEventType::controlChange, offset, ch, cc, value, 0 };
    }

    constexpr bool operator== (const VsEvent&) const noexcept = default;
};

/** Fixed-capacity event list for one audio block. Never allocates: push() returns false when full.
    Producers append events in time order (non-decreasing sampleOffset). Single-threaded use. */
template <std::size_t Capacity>
class VsEventBuffer
{
public:
    static constexpr std::size_t capacity = Capacity;

    bool push (const VsEvent& e) noexcept
    {
        if (count >= Capacity)
            return false;

        events[count++] = e;
        return true;
    }

    void clear() noexcept { count = 0; }

    std::size_t size() const noexcept { return count; }
    bool empty() const noexcept { return count == 0; }
    bool full() const noexcept { return count >= Capacity; }

    const VsEvent& operator[] (std::size_t i) const noexcept { return events[i]; }
    VsEvent& operator[] (std::size_t i) noexcept { return events[i]; }

    const VsEvent* begin() const noexcept { return events.data(); }
    const VsEvent* end() const noexcept { return events.data() + count; }
    VsEvent* begin() noexcept { return events.data(); }
    VsEvent* end() noexcept { return events.data() + count; }

    std::span<const VsEvent> view() const noexcept { return { events.data(), count }; }

private:
    std::array<VsEvent, Capacity> events {};
    std::size_t count = 0;
};

/** Non-owning, type-erased handle to any VsEventBuffer<N>, so code that writes events does not
    have to be a template (the logic can live in a .cpp). Cheap to copy; must not outlive the buffer. */
class VsEventSink
{
public:
    template <std::size_t N>
    VsEventSink (VsEventBuffer<N>& buffer) noexcept // NOLINT: implicit on purpose
        : target (&buffer),
          pushFn ([] (void* t, const VsEvent& e) noexcept { return static_cast<VsEventBuffer<N>*> (t)->push (e); })
    {
    }

    /** Returns false if the underlying buffer is full (the event is dropped). */
    bool push (const VsEvent& e) const noexcept { return pushFn (target, e); }

private:
    void* target;
    bool (*pushFn) (void*, const VsEvent&) noexcept;
};

} // namespace vocalsynth
