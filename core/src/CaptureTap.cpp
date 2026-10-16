#include "vocalsynth/CaptureTap.h"

namespace vocalsynth
{

// TODO(A): implement (docs/M3-DESIGN.md 4). The scaffold only keeps the clock, so nothing downstream
// sees records yet.

void CaptureTap::prepare (double newSampleRate, int newMaxBlockSize)
{
    resetClearsHistory = ! (newSampleRate > 0.0 && newSampleRate == sampleRate) || clock == 0;

    if (resetClearsHistory)
        clock = 0;

    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    maxBlockSize = newMaxBlockSize;
    resetPending = true;
    openNotes = {};
    bendNotCentred.reset();
    publishedClock.store (clock, std::memory_order_release);
}

void CaptureTap::beginBlock (const TransportInfo&, const CaptureBlockSettings&) noexcept
{
}

void CaptureTap::flushNotes() noexcept
{
}

void CaptureTap::processChunk (const float*, int numSamples, std::span<const VsEvent>,
                               std::span<const VsEventTiming>) noexcept
{
    if (numSamples <= 0)
        return;

    clock += numSamples;
    publishedClock.store (clock, std::memory_order_release);
}

void CaptureTap::processBypassed (int numSamples) noexcept
{
    processChunk (nullptr, numSamples, {}, {});
}

void CaptureTap::requestNewTake() noexcept
{
    newTakeRequests.fetch_add (1, std::memory_order_release);
}

void CaptureTap::requestGrab (int bars, double seconds) noexcept
{
    grabBars.store (bars, std::memory_order_relaxed);
    grabSeconds.store (seconds, std::memory_order_relaxed);
    grabRequests.fetch_add (1, std::memory_order_release);
}

void CaptureTap::requestClear() noexcept
{
    clearRequests.fetch_add (1, std::memory_order_release);
}

CaptureTap::OverflowCounts CaptureTap::getOverflowCounts() const noexcept
{
    return { records.getOverflowCount(), audio.getOverflowCount() };
}

bool CaptureTap::pushRecord (const CaptureRecord&) noexcept
{
    return false;
}

void CaptureTap::pushPendingGap() noexcept
{
}

void CaptureTap::pushAudioFrame() noexcept
{
}

void CaptureTap::trackEvent (const VsEvent&) noexcept
{
}

} // namespace vocalsynth
