#include "vocalsynth/synth/VoiceAllocator.h"

namespace vocalsynth::synth
{

// Scaffold (docs/SYNTH-DESIGN.md 10): emits no commands. Agent B implements.

void VoiceAllocator::prepare (double newSampleRate) noexcept
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    reset();
}

void VoiceAllocator::reset() noexcept
{
    voiceNote.fill (-1);
    voiceStart.fill (0);
    voiceRelease.fill (-1);
    stack.fill (0);
    stackSize = 0;
    lastNote = -1;
    newestVoice = -1;
}

void VoiceAllocator::processGroup (std::span<const NoteInput>, int64_t, const AllocatorSettings&,
                                   const std::array<bool, maxVoices>&, VoiceCommandList&) noexcept
{
}

void VoiceAllocator::releaseAll (VoiceCommandList&) noexcept
{
    stackSize = 0;
}

int VoiceAllocator::getNewestVoice (const std::array<bool, maxVoices>&) const noexcept
{
    return -1;
}

int VoiceAllocator::getNumHeldNotes() const noexcept
{
    return stackSize;
}

} // namespace vocalsynth::synth
