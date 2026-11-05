#pragma once

#include "vocalsynth/synth/SynthSettings.h"

#include <array>
#include <cstdint>
#include <vector>

namespace vocalsynth::synth
{

/** Stereo / ping-pong delay, global after the chorus (docs/SYNTH-DESIGN.md 11.2).
    - Time: timeMs, or tempo-synced (bpm and bar length from the host, SynthTransport; 120 BPM / 4 quarter
      notes if unknown) clamped to limits::delayTimeMs;
      time changes glide (the read position moves at most 1 sample per sample: tape-like, no click).
    - Feedback 0..95 %, in the feedback path a one-pole high-pass at 120 Hz and a one-pole low-pass at
      6 kHz (fixed), soft-limited (u / sqrt (1 + u^2)) so the loop stays bounded.
    - Ping-pong: the mono sum enters the left line, the lines cross-feed (L -> R -> L); else two parallel
      lines (L -> L, R -> R).
    - Mix: dual-linear law (Chorus), on/off with a 10 ms crossfade, cleared when off.
    - Silence: when the input is 0 and the lines' peak stays below 1e-6 (-120 dBFS) for one delay time
      (per-sample counter, decided in beginTick()), the lines are cleared and output is exactly the dry input
      (tail ended).
    prepare() allocates maxDelaySeconds per channel at the given rate; the rest is real-time safe.
    NaN guard: a non-finite line sample clears the lines at the next beginTick() (getNanResetCount).
    Owner: agent C. */
class StereoDelay
{
public:
    static constexpr double maxDelaySeconds = 2.0;
    static constexpr double switchFadeSeconds = 0.010;

    void prepare (double sampleRate);
    void reset() noexcept;

    void beginTick (const DelaySettings& settings, const SynthTransport& transport, float mixMod,
                    int tickSamples) noexcept;

    void process (float* left, float* right, int numSamples) noexcept;

    bool isBypassed() const noexcept;
    uint32_t getNanResetCount() const noexcept { return nanResets; }

private:
    struct FeedbackFilter
    {
        float highPassIn = 0.0f, highPassOut = 0.0f, lowPass = 0.0f;
    };

    float targetDelaySamples (const DelaySettings& settings, const SynthTransport& transport) const noexcept;
    void finishRamp() noexcept;
    void clearState() noexcept;
    template <bool pingPongMode, bool gliding>
    void run (float* left, float* right, int numSamples) noexcept;

    double sampleRate = 48000.0;
    std::array<std::vector<float>, 2> lines;
    int length = 1;          ///< samples per line
    int writeIndex = 0;
    int written = 0;         ///< samples written since the last (logical) clear, saturating at length
    float maxDelaySamples = 1.0f;
    float delay = 1.0f, delayTarget = 1.0f; ///< read distance in samples (slews <= 1 sample per sample)
    float feedback = 0.0f, feedbackTarget = 0.0f, feedbackStep = 0.0f;
    float onGain = 0.0f, onTarget = 0.0f, onStep = 0.0f;
    float dryGain = 1.0f, dryTarget = 1.0f, dryStep = 0.0f;
    float wetGain = 0.0f, wetTarget = 0.0f, wetStep = 0.0f;
    float fadeStepPerSample = 0.0f;
    float highPassCoefficient = 1.0f, lowPassCoefficient = 1.0f;
    std::array<FeedbackFilter, 2> filter {};
    int rampLeft = 0;
    int silentRun = 0;       ///< consecutive samples with input 0 and both written samples below 1e-6
    float nanCheck = 0.0f;   ///< sum of the written samples of this tick (non-finite -> clear)
    uint32_t nanResets = 0;
    bool pingPong = false;
    bool bypassed = true;
    bool idle = false;       ///< tail ended
    bool firstTick = true;
};

} // namespace vocalsynth::synth
