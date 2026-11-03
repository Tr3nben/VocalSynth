#pragma once

#include "vocalsynth/ExpressionFrame.h"
#include "vocalsynth/synth/RealFft.h"

#include <span>
#include <vector>

namespace vocalsynth
{

/** What the VoicePipeline hands the tracker once per analysis hop (docs/SYNTH-DESIGN.md 16.2). */
struct ExpressionTrackerInput
{
    std::span<const float> window; ///< newest analysis-rate samples of the Framer window, oldest first
                                   ///< (the tracker uses the newest fftSize of them, zero-padded if fewer)
    float rmsDb = -120.0f;         ///< PitchFrame::rmsDb of the processed frame
    float confidence = 0.0f;       ///< processed PitchFrame confidence 0..1
    float frequencyHz = 0.0f;      ///< processed f0, 0 = unvoiced
    int sungNote = -1;             ///< active note WITHOUT the octave shift (quantised), -1 if none
    float gateThresholdDb = -35.0f;
};

/** Voice -> Level / Brightness / Noisiness / Pitch, once per analysis hop (docs/SYNTH-DESIGN.md 16).
    - Level: x = clamp ((rmsDb - gate) / (-6 - gate), 0, 1), one-pole at the hop rate, attack 5 ms,
      release 50 ms.
    - Brightness: every 2nd hop, Hann-windowed fftSize-point power spectrum (RealFft) of the newest window
      samples; centroid c over the bins 80 Hz .. min (8 kHz, 0.45 fa); b = clamp (log2 (c / 200) / log2 (25),
      0, 1); held between updates; 0 while level == 0.
    - Noisiness: spectral flatness F (geometric / arithmetic mean of the power, same bins, every 2nd hop),
      n = clamp (0.5 F' + 0.5 (1 - confidence), 0, 1) with F' = clamp (F / 0.5, 0, 1); 0 while level == 0.
    - Pitch: 1200 log2 (f0 / f (sungNote)), clamped -100..100; 0 when unvoiced or sungNote < 0.
    Brightness, noisiness and pitch are one-pole smoothed with 10 ms. prepare() allocates (FFT); processHop()
    and reset() are real-time safe. Audio thread only. CPU <= 0.5 % at 48 kHz (docs/SYNTH-DESIGN.md 18).
    Owner: agent C. */
class ExpressionTracker
{
public:
    static constexpr int fftOrder = 9; ///< 512 points
    static constexpr int fftSize = 1 << fftOrder;
    static constexpr int spectrumEveryHops = 2;
    static constexpr double levelAttackSeconds = 0.005;
    static constexpr double levelReleaseSeconds = 0.050;
    static constexpr float levelTopDb = -6.0f;
    static constexpr double smoothingSeconds = 0.010;
    static constexpr float brightnessLowHz = 200.0f;
    static constexpr float brightnessHighHz = 5000.0f;

    /** analysisSampleRate: VoicePipeline::getAnalysisSampleRate(), hopSamples at that rate. NOT real-time. */
    void prepare (double analysisSampleRate, int hopSamples);

    /** Smoothers to 0, hop counter 0. */
    void reset() noexcept;

    /** Returns the frame of this hop (sampleOffset 0: the pipeline sets it). */
    ExpressionFrame processHop (const ExpressionTrackerInput& input) noexcept;

private:
    double analysisRate = 16000.0;
    int hopSamples = 32;
    int hopCounter = 0;
    synth::RealFft fft;
    std::vector<float> windowed;
    std::vector<float> power;
    std::vector<float> hann;
    ExpressionFrame current {};
};

} // namespace vocalsynth
