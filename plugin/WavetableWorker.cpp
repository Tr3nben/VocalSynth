#include "WavetableWorker.h"

namespace vocalsynth
{

// Scaffold (docs/SYNTH-DESIGN.md 12.2): the thread is not started, requests are ignored. Agent D implements.

WavetableWorker::WavetableWorker (WavetableStore& s)
    : juce::Thread ("VocalSynth wavetables"),
      store (s)
{
}

WavetableWorker::~WavetableWorker()
{
    stop();
}

void WavetableWorker::requestFactory (int, const juce::String&) {}

void WavetableWorker::requestImport (int, const juce::File&, int) {}

void WavetableWorker::requestEmbedded (int, std::shared_ptr<const UserTableData>) {}

WavetableWorker::Status WavetableWorker::getStatus() const
{
    return {};
}

bool WavetableWorker::waitUntilIdle (int)
{
    return true;
}

void WavetableWorker::stop()
{
    cancel.store (true, std::memory_order_relaxed);
    stopThread (2000);
}

void WavetableWorker::run()
{
    while (! threadShouldExit())
    {
        store.collectGarbage();
        wait (100);
    }
}

} // namespace vocalsynth
