#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace vocalsynth
{

/** Ring buffer + frame clock at the ANALYSIS rate (docs/M2-DESIGN.md 3.4).

    Hop: hopSamplesFor (fa) = max (1, lround (fa * hopSeconds)), 2 ms (16 kHz -> 32, 22.05 kHz -> 44,
    17.64 kHz -> 35). The hop does not depend on the voice range, so a range change never moves the
    frame clock.

    Window: windowSamplesFor (fa, f0Min) = 2 * maxLagFor (fa, f0Min) with
    maxLagFor = ceil (fa / f0Min) + lagGuardSamples, i.e. (at least) 2 periods of the lowest f0 of the
    range. The detectors compute their lag function for tau = 0..maxLag and integrate over the first
    maxLag samples; the guard lag makes a period of exactly 1 / f0Min an INTERIOR extremum (its
    neighbour tau + 1 exists), so the edge rule of PitchDetector can reject pitches below the range
    without losing f0Min itself. Tenor at 16 kHz: 294 samples = 18.4 ms; Bass/Wide: 496 = 31.0 ms.

    Frame clock (binding): a hop completes on every hopSamples-th pushed sample (pushes number 1..n since
    reset(): when n % hopSamples == 0). The ring is zero-initialised, so the first frames contain leading
    zeros; no frame is skipped. Independent of how the host splits its blocks.

    Real-time safe after prepare(). Audio thread only. */
class Framer
{
public:
    static constexpr double hopSeconds = 0.002;
    static constexpr int lagGuardSamples = 1;

    static int hopSamplesFor (double analysisSampleRate) noexcept;

    /** Largest lag the detectors evaluate: ceil (fa / f0Min) + lagGuardSamples (>= 2). */
    static int maxLagFor (double analysisSampleRate, double f0MinHz) noexcept;

    /** 2 * maxLagFor (fa, f0Min). */
    static int windowSamplesFor (double analysisSampleRate, double f0MinHz) noexcept;

    /** Allocates the ring for windows up to maxWindowSamples (NOT real-time; may throw std::bad_alloc).
        The window length is set to maxWindowSamples. Calls reset(). */
    void prepare (double analysisSampleRate, int maxWindowSamples);

    /** Zeroes the ring and restarts the frame clock (next push is number 1). */
    void reset() noexcept;

    /** Changes the window length (clamped to [1, maxWindowSamples]). Keeps the ring contents and the
        hop phase, so the next frame already uses the new length. Real-time safe. */
    void setWindowSamples (int windowSamples) noexcept;

    /** Pushes one analysis-rate sample. Returns true if this sample completed a hop; getWindow() then
        holds the frame to analyse. */
    bool pushSample (float sample) noexcept;

    /** The newest getWindowSamples() samples, oldest first, contiguous. Valid until the next push. */
    std::span<const float> getWindow() const noexcept;

    int getWindowSamples() const noexcept { return windowSamples; }
    int getMaxWindowSamples() const noexcept { return maxWindow; }
    int getHopSamples() const noexcept { return hopSamples; }
    int64_t getNumSamplesPushed() const noexcept { return pushed; }

private:
    int hopSamples = 32;
    int maxWindow = 0;
    int windowSamples = 0;
    std::vector<float> ring; ///< 2 * maxWindow, every sample written twice (contiguous windows)
    int writePos = 0;
    int64_t pushed = 0;
};

} // namespace vocalsynth
