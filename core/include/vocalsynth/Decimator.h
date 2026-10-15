#pragma once

#include <cstdint>
#include <vector>

namespace vocalsynth
{

/** Integer-factor decimator with an anti-alias FIR (docs/M2-DESIGN.md 3.3).

    Factor D = max (1, floor (sampleRate / targetAnalysisRate)): 44.1 kHz -> 2 (22.05 kHz),
    48 kHz -> 3 (16 kHz), 96 kHz -> 6 (16 kHz). The analysis runs at sampleRate / D.

    Filter: linear-phase Kaiser-windowed sinc, cutoff 0.49 fa (fa = output rate; designed for 62 dB so
    that the fixed tap count really reaches 60 dB, see Decimator.cpp), passband edge
    passbandFraction * fa, stopband edge stopbandFraction * fa with >= stopbandAttenuationDb attenuation,
    DC gain exactly 1 (taps normalised to sum 1). Aliases from the transition band only fold into
    (0.35 fa, 0.5 fa), above everything the pitch detectors rely on. numTapsFor() gives the (odd) tap count.
    With D == 1 the decimator is a pass-through (1 tap, delay 0).

    Timing (binding, it makes the pipeline block-size invariant): counting input samples n = 0, 1, 2, ...
    since reset(), output y[k] is produced by the call that pushes input sample n = k * D, and
    y[k] = sum_{j=0}^{N-1} h[j] * x[k*D - j] (x[< 0] = 0). The FIR is evaluated only on those samples.

    Real-time safe after prepare(): pushSample() never allocates or locks. Audio thread only. */
class Decimator
{
public:
    static constexpr double targetAnalysisRate = 16000.0;
    static constexpr double passbandFraction = 0.35;      ///< of the output rate
    static constexpr double stopbandFraction = 0.65;      ///< of the output rate
    static constexpr double stopbandAttenuationDb = 60.0;

    /** D for a host sample rate: max (1, floor (sampleRate / targetAnalysisRate)). */
    static int factorFor (double sampleRate) noexcept;

    /** Odd FIR length for a factor (Kaiser estimate N = ceil ((A - 7.95) / (2.285 * dw)) + 1 with
        dw = 2 pi (stopbandFraction - passbandFraction) / D, rounded up to odd). 1 for D == 1.
        D = 2 -> 27, 3 -> 39, 5 -> 63, 6 -> 75. */
    static int numTapsFor (int factor) noexcept;

    /** Computes D and the taps and allocates the history (NOT real-time; may throw std::bad_alloc).
        Calls reset(). */
    void prepare (double inputSampleRate);

    /** Clears the history and the phase counter (next pushed sample is n = 0). */
    void reset() noexcept;

    /** Pushes one input sample. Returns true and writes out when this sample produces an output
        (n % D == 0), false otherwise. */
    bool pushSample (float input, float& out) noexcept;

    int getFactor() const noexcept { return factor; }
    double getInputSampleRate() const noexcept { return inputRate; }
    double getOutputSampleRate() const noexcept { return inputRate / static_cast<double> (factor); }
    int getNumTaps() const noexcept { return static_cast<int> (taps.size()); }

    /** Group delay of the linear-phase FIR in INPUT samples: (numTaps - 1) / 2. */
    int getGroupDelayInputSamples() const noexcept { return (getNumTaps() - 1) / 2; }

private:
    double inputRate = 48000.0;
    int factor = 1;
    int phase = 0;                ///< n % D of the next input sample
    std::vector<float> taps;      ///< h[0..N)
    std::vector<float> history;   ///< 2 * N, every sample written twice so the dot product is contiguous
    int writePos = 0;
};

} // namespace vocalsynth
