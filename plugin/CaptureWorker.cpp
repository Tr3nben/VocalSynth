#include "CaptureWorker.h"

namespace vocalsynth
{

// TODO(C): implement (docs/M3-DESIGN.md 5, 12). Stubs: no thread, nothing captured.

TransportInfo makeTransportInfo (juce::AudioPlayHead*) noexcept
{
    return {};
}

class CaptureWorker::Thread
{
};

CaptureWorker::CaptureWorker (CaptureTap& t, juce::File folder)
    : tap (t),
      instanceFolder (std::move (folder))
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

void CaptureWorker::setOptions (double, double)
{
}

void CaptureWorker::setWavOptions (const RenderOptions&, WavSampleFormat)
{
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
}

} // namespace vocalsynth
