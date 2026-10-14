#pragma once

#include "vocalsynth/VsEvent.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>
#include <bitset>
#include <cstdint>
#include <span>

namespace vocalsynth
{

class VirtualMidiPort;

/** Converts core VsEvents into a juce::MidiBuffer, sample-accurately, and guards against stuck notes.

    - Sample offsets are clamped to [0, numSamples - 1] (numSamples >= 1).
    - Tracks active notes per channel (16 x 128 bitset, no allocation). A note-on for a note that is
      already active is preceded by a note-off for it (no duplicate note-ons downstream). A note-off
      for an inactive note is dropped. A note-on with velocity 0 is handled as a note-off.
      Channel and data values are clamped to MIDI ranges (1..16, 0..127, bend -8192..8191).
    - Also remembers per channel whether the last pitch bend written was not centred.
    - flushAllNotes() sends a note-off for every active note and a pitch-bend centre on every channel
      whose bend is not centred (so a processor-level flush is complete even after the generator was reset).
    - Every message written is also pushed to the VirtualMidiPort, if one is set and enabled.
    - Counts the messages written per type (relaxed atomics) so the UI can show that the plugin DID
      send pitch bend / CC even when nothing arrives at the receiver in FL (T1-T5 diagnosis).
    - Use std::bitset::operator[] (never test()/set(pos)/reset(pos): those throw std::out_of_range).

    Audio thread only (the virtual port pointer is set once on the message thread before playback).
    Note: juce::MidiBuffer::addEvent can grow its storage. The plugin wrappers reuse their MidiBuffer,
    so it only grows during the first busy blocks (accepted M1 risk, docs/M1-DESIGN.md 5). */
class MidiOutputStage
{
public:
    /** Messages written since construction (not reset by reset()). Read by the UI thread. */
    struct SentCounts
    {
        uint32_t noteOns = 0;
        uint32_t noteOffs = 0;
        uint32_t pitchBends = 0;
        uint32_t controlChanges = 0;
    };

    /** Optional mirror for T7. May be nullptr. Not owned. Set before playback starts. */
    void setVirtualPort (VirtualMidiPort* port) noexcept { virtualPort = port; }

    /** Forgets all active notes and bend state WITHOUT emitting anything. */
    void reset() noexcept;

    /** Appends events to out. The events belong to a chunk of numSamples samples that starts at
        startOffset within the host block (the processor splits long blocks, docs/M1-DESIGN.md 4.4):
        each offset is clamped to [0, numSamples - 1], then startOffset is added. */
    template <std::size_t N>
    void write (const VsEventBuffer<N>& events, int numSamples, juce::MidiBuffer& out, int startOffset = 0) noexcept
    {
        writeEvents (events.view(), numSamples, out, startOffset);
    }

    /** Non-template version of write(), implemented in the .cpp. */
    void writeEvents (std::span<const VsEvent> events, int numSamples, juce::MidiBuffer& out,
                      int startOffset = 0) noexcept;

    /** Note-off for every active note and bend centre where needed, at sampleOffset (clamped to >= 0).
        Afterwards nothing is active. Emits nothing if nothing is active. */
    void flushAllNotes (int sampleOffset, juce::MidiBuffer& out) noexcept;

    bool isNoteActive (int channel, int note) const noexcept;
    int getNumActiveNotes() const noexcept;
    bool isBendCentred (int channel) const noexcept;

    /** Any thread. */
    SentCounts getSentCounts() const noexcept
    {
        return { sentNoteOns.load (std::memory_order_relaxed), sentNoteOffs.load (std::memory_order_relaxed),
                 sentPitchBends.load (std::memory_order_relaxed),
                 sentControlChanges.load (std::memory_order_relaxed) };
    }

private:
    /** Adds one message to out and mirrors it to the virtual port. */
    void emit (const juce::MidiMessage& message, int sampleOffset, juce::MidiBuffer& out) noexcept;

    std::array<std::bitset<128>, 16> activeNotes {}; ///< [channel - 1][note]
    std::bitset<16> bendNotCentred {};                ///< [channel - 1]
    VirtualMidiPort* virtualPort = nullptr;

    // Audio thread writes (single writer: load + store, or fetch_add), UI reads
    std::atomic<uint32_t> sentNoteOns { 0 };
    std::atomic<uint32_t> sentNoteOffs { 0 };
    std::atomic<uint32_t> sentPitchBends { 0 };
    std::atomic<uint32_t> sentControlChanges { 0 };
    static_assert (std::atomic<uint32_t>::is_always_lock_free);
};

} // namespace vocalsynth
