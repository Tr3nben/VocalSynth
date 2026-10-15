#include "MidiOutputStage.h"

#include "VirtualMidiPort.h"

#include <algorithm>

namespace vocalsynth
{

namespace
{
    /** Single writer (the audio thread): load + store instead of a locked read-modify-write. */
    void increment (std::atomic<uint32_t>& counter) noexcept
    {
        counter.store (counter.load (std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    }

    int clampChannel (int channel) noexcept { return std::clamp (channel, 1, 16); }
    int clampData (int value) noexcept { return std::clamp (value, 0, 127); }
} // namespace

void MidiOutputStage::reset() noexcept
{
    for (auto& channel : activeNotes)
        channel.reset();

    bendNotCentred.reset();
}

void MidiOutputStage::writeEvents (std::span<const VsEvent> events, int numSamples, juce::MidiBuffer& out,
                                   int startOffset) noexcept
{
    if (numSamples < 1)
        return;

    const auto chunkStart = std::max (0, startOffset);

    for (const auto& e : events)
    {
        const auto offset = std::clamp (e.sampleOffset, 0, numSamples - 1) + chunkStart;
        const auto channel = clampChannel ((int) e.channel);
        auto& notes = activeNotes[(size_t) (channel - 1)];

        switch (e.type)
        {
            case VsEventType::noteOn:
            {
                const auto note = clampData ((int) e.data1);
                const auto velocity = clampData ((int) e.data2);

                if (velocity == 0)
                {
                    // A note-on with velocity 0 means note-off in MIDI: handle it as one, so the
                    // tracking matches what the receiver does.
                    if (notes[(size_t) note])
                    {
                        notes[(size_t) note] = false;
                        emit (juce::MidiMessage::noteOff (channel, note), offset, out);
                        increment (sentNoteOffs);
                    }
                    break;
                }

                if (notes[(size_t) note])
                {
                    // No duplicate note-ons downstream: close the old one first.
                    emit (juce::MidiMessage::noteOff (channel, note), offset, out);
                    increment (sentNoteOffs);
                }

                notes[(size_t) note] = true;
                emit (juce::MidiMessage::noteOn (channel, note, (juce::uint8) velocity), offset, out);
                increment (sentNoteOns);
                break;
            }

            case VsEventType::noteOff:
            {
                const auto note = clampData ((int) e.data1);

                if (! notes[(size_t) note])
                    break; // never sent (or already off): drop it

                notes[(size_t) note] = false;
                emit (juce::MidiMessage::noteOff (channel, note, (juce::uint8) clampData ((int) e.data2)), offset, out);
                increment (sentNoteOffs);
                break;
            }

            case VsEventType::pitchBend:
            {
                const auto bend = std::clamp ((int) e.bend, -8192, 8191);
                bendNotCentred[(size_t) (channel - 1)] = (bend != 0);
                emit (juce::MidiMessage::pitchWheel (channel, bend + 8192), offset, out);
                increment (sentPitchBends);
                break;
            }

            case VsEventType::controlChange:
            {
                emit (juce::MidiMessage::controllerEvent (channel, clampData ((int) e.data1), clampData ((int) e.data2)),
                      offset, out);
                increment (sentControlChanges);
                break;
            }
        }
    }
}

void MidiOutputStage::flushAllNotes (int sampleOffset, juce::MidiBuffer& out) noexcept
{
    const auto offset = std::max (0, sampleOffset);

    for (size_t ch = 0; ch < activeNotes.size(); ++ch)
    {
        auto& notes = activeNotes[ch];

        if (notes.any())
        {
            for (size_t note = 0; note < 128; ++note)
            {
                if (notes[note])
                {
                    emit (juce::MidiMessage::noteOff ((int) ch + 1, (int) note), offset, out);
                    increment (sentNoteOffs);
                }
            }
        }

        if (bendNotCentred[ch])
        {
            emit (juce::MidiMessage::pitchWheel ((int) ch + 1, 8192), offset, out);
            increment (sentPitchBends);
        }
    }

    reset();
}

bool MidiOutputStage::isNoteActive (int channel, int note) const noexcept
{
    if (channel < 1 || channel > 16 || note < 0 || note > 127)
        return false;

    return activeNotes[(size_t) (channel - 1)][(size_t) note];
}

bool MidiOutputStage::isBendCentred (int channel) const noexcept
{
    if (channel < 1 || channel > 16)
        return true;

    return ! bendNotCentred[(size_t) (channel - 1)];
}

int MidiOutputStage::getNumActiveNotes() const noexcept
{
    int total = 0;
    for (const auto& channel : activeNotes)
        total += (int) channel.count();
    return total;
}

void MidiOutputStage::emit (const juce::MidiMessage& message, int sampleOffset, juce::MidiBuffer& out) noexcept
{
    out.addEvent (message, sampleOffset);

    if (virtualPort != nullptr)
        virtualPort->push (message);
}

} // namespace vocalsynth
