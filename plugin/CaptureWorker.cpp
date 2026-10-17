#include "CaptureWorker.h"

namespace vocalsynth
{

// TODO(C): implement (docs/M3-DESIGN.md 5, 12). Stubs: no thread, nothing captured.

TransportInfo makeTransportInfo (juce::AudioPlayHead*) noexcept
{
    return {};
}

CaptureOffsetSettings CaptureWorkerSources::readOffset() const noexcept
{
    CaptureOffsetSettings o;

    if (offsetMode != nullptr)
        o.mode = static_cast<CaptureOffsetMode> (
            juce::jlimit (0, numCaptureOffsetModes - 1, juce::roundToInt (offsetMode->load (std::memory_order_relaxed))));

    if (offsetMs != nullptr)
        o.offsetMs = static_cast<double> (offsetMs->load (std::memory_order_relaxed));

    if (fineOffsetMs != nullptr)
        o.fineOffsetMs = static_cast<double> (fineOffsetMs->load (std::memory_order_relaxed));

    return o;
}

class CaptureWorker::Thread
{
};

CaptureWorker::CaptureWorker (CaptureTap& t, juce::File folder, CaptureWorkerSources s)
    : tap (t),
      instanceFolder (std::move (folder)),
      sources (std::move (s))
{
}

CaptureWorker::~CaptureWorker()
{
    stop();
}

void CaptureWorker::start()
{
}

void CaptureWorker::suspend()
{
}

void CaptureWorker::resume()
{
}

void CaptureWorker::stop()
{
    thread.reset();
}

bool CaptureWorker::isRunning() const noexcept
{
    return thread != nullptr;
}

void CaptureWorker::setManualSteppingForTests (bool manual) noexcept
{
    manualStepping = manual;
}

void CaptureWorker::setOptions (const CaptureWorkerOptions& options)
{
    const std::scoped_lock lock (optionsMutex);
    pendingOptions = options;
}

std::vector<std::shared_ptr<const Take>> CaptureWorker::getTakes() const
{
    const std::scoped_lock lock (listMutex);
    return takes;
}

std::shared_ptr<const Take> CaptureWorker::getOpenTake() const
{
    const std::scoped_lock lock (listMutex);
    return openTakeSnapshot;
}

TakeWavState CaptureWorker::getWavState (int) const
{
    return {};
}

void CaptureWorker::markWavPersisted (int, const juce::File&)
{
}

CaptureStatus CaptureWorker::getStatus() const noexcept
{
    CaptureStatus s;
    s.overflow = tap.getOverflowCounts();
    return s;
}

void CaptureWorker::runStepForTests()
{
    step();
}

void CaptureWorker::step()
{
    (void) historyPrepared;
    (void) manualStepping;
    (void) drainer;
}

} // namespace vocalsynth
