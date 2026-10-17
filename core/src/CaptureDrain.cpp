#include "vocalsynth/CaptureDrain.h"

#include <algorithm>
#include <span>

namespace vocalsynth
{

std::size_t CaptureDrain::drain (CaptureTap& tap, CaptureHistory& history, TakeSegmenter& segmenter,
                                 std::vector<Take>& closedTakes)
{
    // Load the clock first: every record with sample < clock is in the ring now (CaptureTap::getClock)
    const auto clock = tap.getClock();
    auto& recordRing = tap.getRecordRing();
    auto& audioRing = tap.getAudioRing();
    std::size_t handled = 0;

    // Bounded: at most one ring capacity per call (records pushed meanwhile wait for the next call)
    for (std::size_t round = 0; round <= recordRing.getCapacity() / recordsPerPop; ++round)
    {
        const auto n = recordRing.popBulk (std::span<CaptureRecord> { records });

        for (std::size_t i = 0; i < n; ++i)
        {
            const auto& record = records[i];

            if (record.type == CaptureRecordType::audio)
            {
                const auto wanted = static_cast<std::size_t> (
                    std::clamp (record.asAudioNumSamples(), 0, CaptureTap::audioFrameSamples));
                const auto got = audioRing.popBulk (std::span<float> { frame.data(), wanted });
                history.addAudio (record.sample, std::span<const float> { frame.data(), got });
            }
            else
            {
                history.addRecord (record);
                segmenter.process (record, history, closedTakes);
            }
        }

        handled += n;

        if (n < records.size())
            break;
    }

    segmenter.advanceTo (clock, history, closedTakes);
    lastClock = clock;
    return handled;
}

} // namespace vocalsynth
