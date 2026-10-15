#include "vocalsynth/VoicePipeline.h"

#include "TestSignals.h"

#include <catch2/catch_test_macros.hpp>

using namespace vocalsynth;

// TODO(C): end-to-end tests of docs/M2-DESIGN.md 6: per-range accuracy, vibrato -> one note (+ bend in
// True mode), glissando, octave jumps, missing fundamental, noise/silence/breath -> 0 notes, scale
// quantisation, legato vs retrigger, MIDI invariants, block sizes {1, 7, 64, 480, 512, 4096} x
// 44.1/48/96 kHz identical events, latency per range, settings-change flush, CPU benchmark.

TEST_CASE ("Algorithmic latency budget per range", "[pipeline]")
{
    // docs/M2-DESIGN.md 3.11: the model includes the conditioner's group delay at f0 min
    CHECK (VoicePipeline::latencyBudgetHops == 2);

    for (const auto sr : { 44100.0, 48000.0, 88200.0, 96000.0 })
    {
        const auto tenor = VoicePipeline::computeAlgorithmicLatencyMs (sr, VoiceRange::tenor);
        const auto bass = VoicePipeline::computeAlgorithmicLatencyMs (sr, VoiceRange::bass);
        const auto wide = VoicePipeline::computeAlgorithmicLatencyMs (sr, VoiceRange::wide);

        CHECK (tenor <= VoicePipeline::tenorLatencyBudgetMs);
        CHECK (bass <= VoicePipeline::bassLatencyBudgetMs);
        CHECK (wide == bass);

        // The conditioner term is real: about 3.4 ms (+ DC blocker) at 65 Hz
        const auto conditionerMs = 1000.0 * InputConditioner::groupDelaySeconds (sr, 65.0);
        CHECK (conditionerMs > 3.0);
        CHECK (conditionerMs < 4.0);

        // Lower ranges never have less latency than higher ones
        double previous = 1.0e9;

        for (const auto range : { VoiceRange::bass, VoiceRange::baritone, VoiceRange::tenor, VoiceRange::alto,
                                  VoiceRange::soprano, VoiceRange::whistle })
        {
            const auto ms = VoicePipeline::computeAlgorithmicLatencyMs (sr, range);
            CHECK (ms < previous);
            CHECK (ms > 0.0);
            previous = ms;
        }
    }

    CHECK (VoicePipeline::computeAlgorithmicLatencyMs (0.0, VoiceRange::tenor) == 0.0);
}

TEST_CASE ("VoicePipeline scaffold: silence gives no events", "[pipeline]")
{
    VoicePipeline pipeline;
    pipeline.prepare (48000.0, 512);

    const auto input = test::silence (48000.0, 0.5);
    VsEventBuffer<1024> events;
    pipeline.process (input.data(), static_cast<int> (input.size() / 8), VoiceSettings {}, events);
    CHECK (events.empty());
    CHECK (pipeline.getSnapshot().activeNote == -1);
}
