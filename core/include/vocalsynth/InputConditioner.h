#pragma once

namespace vocalsynth
{

/** First stage of the voice pipeline, at the HOST sample rate (docs/M2-DESIGN.md 3.2).

    Input is the mono downmix made by the plugin. Applies, in this order:
    1. DC blocker: one-pole/one-zero high-pass, y[n] = x[n] - x[n-1] + R * y[n-1],
       R = exp (-2 pi dcBlockerHz / sampleRate).
    2. 2nd-order Butterworth high-pass at highPassHz (RBJ biquad, Q = 1/sqrt 2), transposed direct form II.

    State and coefficients are double precision; output is float. Removes rumble, handling noise and DC
    that would otherwise dominate the RMS gate and the low lags of the difference function.

    Group delay matters for the latency budget: near the cutoff the high-pass delays the envelope of the
    fundamental (3.62 ms at 65 Hz = Bass f0 min, 1.40 ms at 110 Hz = Tenor f0 min, DC blocker included), so
    VoicePipeline::computeAlgorithmicLatencyMs() adds groupDelaySeconds (sr, f0 min of the range).

    Filter memories smaller than denormalFloor are flushed to 0 (long silences in tools/tests that run
    without FTZ/DAZ stay fast); this is per sample, so block-size invariance is unaffected.

    Strictly per-sample and stateful only through its filter memories, so it is block-size invariant.
    Real-time safe: no allocation, no locks. Not thread-safe; audio thread only. */
class InputConditioner
{
public:
    static constexpr double dcBlockerHz = 5.0;
    static constexpr double highPassHz = 60.0;
    static constexpr double highPassQ = 0.70710678118654752;
    static constexpr double denormalFloor = 1.0e-30;

    /** Group delay in seconds of the whole conditioner (DC blocker + high-pass) at frequencyHz for a
        host rate, from the exact digital transfer function. Pure function, any thread. */
    static double groupDelaySeconds (double sampleRate, double frequencyHz) noexcept;

    /** Computes the coefficients for the host rate and calls reset(). Does not allocate. */
    void prepare (double sampleRate) noexcept;

    /** Clears the filter memories (output starts from silence). */
    void reset() noexcept;

    /** Filters one sample. Never returns NaN/Inf for finite input; non-finite input is treated as 0. */
    float processSample (float input) noexcept;

    /** Filters numSamples samples; in and out may be the same buffer. */
    void process (const float* in, float* out, int numSamples) noexcept;

private:
    // DC blocker
    double dcR = 0.0;
    double dcX1 = 0.0;
    double dcY1 = 0.0;

    // Biquad high-pass (normalised, a0 == 1), TDF-II state
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double s1 = 0.0, s2 = 0.0;
};

} // namespace vocalsynth
