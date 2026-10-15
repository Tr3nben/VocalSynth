#pragma once

#include <span>
#include <vector>

namespace vocalsynth
{

/** Result of analysing one frame (docs/M2-DESIGN.md 3.5). */
struct PitchFrame
{
    static constexpr float silenceDb = -120.0f;

    float frequencyHz = 0.0f; ///< 0 = unvoiced / no candidate in the search range
    float confidence = 0.0f;  ///< 0..1 (YIN: 1 - d'(tau), MPM: NSDF peak value)
    float rmsDb = silenceDb;  ///< RMS of the newest rmsSamples() of the frame in dBFS, floor silenceDb

    bool operator== (const PitchFrame&) const noexcept = default;
};

/** RMS level in dBFS of the given samples, 20 log10 (rms), floored at PitchFrame::silenceDb
    (also for an empty span). A full-scale sine gives about -3 dB. */
float computeRmsDb (std::span<const float> samples) noexcept;

/** Number of newest frame samples the detectors use for rmsDb: one period of f0 min,
    ceil (analysisSampleRate / f0MinHz), at most the frame length. */
int rmsSamplesFor (double analysisSampleRate, double f0MinHz) noexcept;

/** Common interface of the pitch detectors. The pipeline prepares BOTH implementations and switches
    between them without allocating (algorithm change = flush, see VoiceSettings requiresFlush()).

    Contract for analyse() (docs/M2-DESIGN.md 3.5):
    - window: one frame from the Framer, oldest sample first, at the analysis rate;
      window.size() <= maxWindowSamples and normally == Framer::windowSamplesFor (fa, f0MinHz).
    - Candidate lags: tauMin = max (2, floor (fa / f0MaxHz)) .. tauMax = ceil (fa / f0MinHz * 2^(r / 12))
      with r = rangeToleranceSemitones (at most window.size() / 2 * 17 / 16). The lag function (YIN d',
      MPM NSDF) is evaluated for tau = 0 (MPM) or 1 (YIN) .. maxLag = tauMax + 1 (the guard lag, so
      tauMax has both neighbours). maxLag is about 3 % beyond window.size() / 2
      (== Framer::maxLagFor (fa, f0MinHz)): a tone at f0 min that reads a few cents flat must not hit
      the edge rule (with tauMax = ceil (fa / f0MinHz) the lower edge was one lag, ~6 cents at 82 Hz,
      below f0 min). The window, and with it the latency, is unchanged.
    - Edge rule (binding): the chosen extremum must be a local extremum including its outer neighbour:
      a YIN minimum at tauMax that is still falling (d'(tauMax + 1) < d'(tauMax)) or at tauMin with
      d'(tauMin - 1) < d'(tauMin), or an MPM peak at either end that is still rising outwards, means the
      period lies outside the range: the frame is unvoiced. So a tone outside [f0Min, f0Max] (by more
      than about one lag) is never reported as the nearest range limit.
      To see a period that lies more than one lag above the range (f0 > f0Max), the selection scans
      from scanMin = max (2, tauMin / 2) instead of tauMin: every period shorter than tauMin has a
      multiple in [tauMin / 2, tauMin), so such a tone yields a chosen extremum below tauMin (= unvoiced)
      instead of its octave below. The chosen extremum must lie in [tauMin, tauMax].
    - Parabolic interpolation around the chosen lag (always possible: both neighbours exist). An
      interpolated frequency more than rangeToleranceSemitones outside [f0MinHz, f0MaxHz] is unvoiced
      (matters only for very short periods, e.g. Whistle at 2.5 kHz = 6.4 lags, where one lag is more
      than 2 semitones); otherwise it is reported as measured (NOT clamped to [f0MinHz, f0MaxHz]: a
      note at the edge of the range sung a few cents off must show that offset in the tuner and in
      True bend). So frequencyHz lies in [f0MinHz / t, f0MaxHz * t], t = 2^(rangeToleranceSemitones / 12).
    - frequencyHz = 0 and confidence = 0 if rmsDb < silenceFloorDb or no candidate exists.
    - Level step: frequencyHz = 0 and confidence = 0 if rmsDb (newest period) and the RMS of the whole
      window differ by more than maxLevelStepDb. Such a window straddles an onset (newest part loud,
      oldest still silent) or a release (newest part already silent), and both lag functions are
      biased there: at a release YIN drifts sharp with confidence 0.7-0.96 (226 -> 234 Hz within
      6 ms), which sent a ~1 semitone pitch-bend spike right before the note-off. A steady tone stays
      within ~1.5 dB (the newest period holds at least one period of every pitch in the range); the
      detectors are not confident in windows filled by less than ~75 % anyway, so onsets are not
      delayed.
    - rmsDb = computeRmsDb (newest rmsSamplesFor (fa, f0MinHz) samples).
    - Deterministic, stateless between frames (the same window gives the same result).
    Real-time safe: analyse() never allocates or locks; it is bounded by O(window * maxLag). */
class PitchDetector
{
public:
    static constexpr float silenceFloorDb = -90.0f;
    static constexpr float maxLevelStepDb = 6.0f;
    static constexpr double rangeToleranceSemitones = 0.5;

    virtual ~PitchDetector() = default;

    /** Allocates the work buffers for frames up to maxWindowSamples (NOT real-time; may throw). */
    virtual void prepare (double analysisSampleRate, int maxWindowSamples) = 0;

    virtual PitchFrame analyse (std::span<const float> window, double f0MinHz, double f0MaxHz) noexcept = 0;
};

/** YIN (de Cheveigne & Kawahara 2002), direct difference function.

    Integration length Wi = window.size() - maxLag (~0.97 window.size() / 2). d(tau) = sum_{j<Wi} (x[j] - x[j+tau])^2
    for tau = 1..maxLag, cumulative-mean normalised d'(tau) (d'(0) = 1). Picks the first tau in
    [scanMin, tauMax] with d'(tau) < absoluteThreshold and walks down to its local minimum (up to
    maxLag); if there is none, the global minimum of d' in [scanMin, maxLag]. Edge rule (a minimum outside
    [tauMin, tauMax], i.e. a dip still falling at tauMax or one that is lower at tauMin - 1 or below,
    makes the frame unvoiced) and parabolic interpolation as in PitchDetector, on the raw d(tau) of
    the three lags around the minimum (d' would bias short periods by > 10 cents).
    Before the edge rule, a sub-multiple check (chosen / 2, chosen / 3, repeated): a local minimum there
    with d' <= subMultipleRatio * d'(chosen) + subMultipleSlack replaces the chosen lag (onset frames,
    see PitchDetector.cpp).
    confidence = clamp (1 - d'(tau), 0, 1), where d'(tau) is the interpolated (vertex) value if that
    parabola is plausible (vertex >= 0), else the sampled one. A frame picked by the global-minimum
    fallback (no dip under absoluteThreshold) gets at most fallbackMaxConfidence: such frames occur at
    onsets, while the oldest part of the window (YIN's integration part) is still partly silent, and
    there the global minimum is often at 2 T with d' just above the threshold (confidence 0.80-0.85),
    which used to start notes an octave low. Capped, they can hold a note (weak voiced) but never
    start one (NoteSegmenter::confidenceOn). */
class YinPitchDetector final : public PitchDetector
{
public:
    static constexpr float absoluteThreshold = 0.15f;
    static constexpr float fallbackMaxConfidence = 0.75f; ///< in [confidenceOff, confidenceOn) of NoteSegmenter
    static constexpr float subMultipleRatio = 1.5f;       ///< sub-multiple check, see analyse()
    static constexpr float subMultipleSlack = 0.02f;

    void prepare (double analysisSampleRate, int maxWindowSamples) override;
    PitchFrame analyse (std::span<const float> window, double f0MinHz, double f0MaxHz) noexcept override;

private:
    double sampleRate = 16000.0;
    int maxWindow = 0;
    std::vector<float> difference; ///< d' for tau = 0..maxLag (<= maxWindowSamples * 17 / 32 + 1, + spare)
};

/** McLeod Pitch Method (McLeod & Wyvill 2005), direct NSDF.

    n(tau) = 2 r(tau) / m(tau) over the whole frame (r = sum_{j<N-tau} x[j] x[j+tau], m = sum of squares of
    both overlapping parts, N = window.size()) for tau = 0..maxLag. Lobes: the initial lobe around
    tau = 0 (up to the first negative-going zero crossing) is skipped; every later lobe runs from a
    positive-going zero crossing to the next negative-going one (a lobe still open at maxLag ends
    there). Key maximum = highest n inside a lobe. Among the key maxima with tau in [scanMin, maxLag],
    the first one with n >= keyThreshold * (highest of them) wins; edge rule as in PitchDetector (the
    winner must lie in [tauMin, tauMax], else unvoiced: a key maximum below tauMin is a period above
    f0 max, one at maxLag a lobe still rising at the end of the range). No key maximum (e.g. no zero
    crossing for strongly low-passed noise) = unvoiced. Parabolic interpolation.
    confidence = clamp (n(peak), 0, 1). */
class MpmPitchDetector final : public PitchDetector
{
public:
    static constexpr float keyThreshold = 0.9f;

    void prepare (double analysisSampleRate, int maxWindowSamples) override;
    PitchFrame analyse (std::span<const float> window, double f0MinHz, double f0MaxHz) noexcept override;

private:
    double sampleRate = 16000.0;
    int maxWindow = 0;
    std::vector<float> nsdf; ///< n(tau) for tau = 0..maxLag (<= maxWindowSamples * 17 / 32 + 1, + spare)
};

} // namespace vocalsynth
