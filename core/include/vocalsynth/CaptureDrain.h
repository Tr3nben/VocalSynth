#pragma once

#include "vocalsynth/CaptureHistory.h"
#include "vocalsynth/CaptureRecord.h"
#include "vocalsynth/CaptureTap.h"
#include "vocalsynth/Take.h"
#include "vocalsynth/TakeSegmenter.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace vocalsynth
{

/** The consumer side of the capture rings in one place (docs/M3-DESIGN.md 5.3), shared by the plugin's
    CaptureWorker::step() and the core end-to-end tests, so both feed the history and the segmenter in
    exactly the same way (the block-size invariance and byte-identical .mid gates rely on it).

    drain(): clock = tap.getClock() (acquire, BEFORE popping); then every record in the ring, in stream
    order: audio record -> its frame from the audio ring -> history.addAudio (a frame that is not there,
    which the protocol rules out, is left to the history's zero fill); any other record ->
    history.addRecord, then segmenter.process. Finally segmenter.advanceTo (clock). Records the producer
    pushed after the clock was loaded may be handled as well (advanceTo never moves backwards); at most
    about one ring capacity per call. Returns the number of records handled. Capture worker thread (or the test's thread) only; not real-time (the history and the
    segmenter allocate); the scratch buffers are members (no allocation per call). */
class CaptureDrain
{
public:
    static constexpr std::size_t recordsPerPop = 1024; ///< 32 KiB scratch

    std::size_t drain (CaptureTap& tap, CaptureHistory& history, TakeSegmenter& segmenter,
                       std::vector<Take>& closedTakes);

    /** Clock loaded by the last drain() (every record before it was handled). */
    int64_t getLastClock() const noexcept { return lastClock; }

private:
    std::array<CaptureRecord, recordsPerPop> records {};
    std::array<float, CaptureTap::audioFrameSamples> frame {};
    int64_t lastClock = 0;
};

} // namespace vocalsynth
