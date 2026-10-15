#include "vocalsynth/PitchDetector.h"

#include <algorithm>
#include <cmath>

namespace vocalsynth
{

float computeRmsDb (std::span<const float> samples) noexcept
{
    if (samples.empty())
        return PitchFrame::silenceDb;

    double sum = 0.0;

    for (auto s : samples)
        sum += static_cast<double> (s) * static_cast<double> (s);

    const auto rms = std::sqrt (sum / static_cast<double> (samples.size()));

    if (! (rms > 0.0))
        return PitchFrame::silenceDb;

    return std::max (PitchFrame::silenceDb, static_cast<float> (20.0 * std::log10 (rms)));
}

int rmsSamplesFor (double analysisSampleRate, double f0MinHz) noexcept
{
    if (! (analysisSampleRate > 0.0) || ! (f0MinHz > 0.0))
        return 1;

    return std::max (1, static_cast<int> (std::ceil (analysisSampleRate / f0MinHz)));
}

//==============================================================================
namespace
{
/** Lag bounds of one analysis (docs/M2-DESIGN.md 3.5). */
struct LagRange
{
    int maxLag = 0;  ///< lag function evaluated for tau <= maxLag (= tauMax + 1, the guard lag)
    int tauMin = 0;  ///< shortest period in the range: max (2, floor (fa / f0Max))
    int tauMax = 0;  ///< longest period accepted: ceil (fa / f0Min) extended by rangeToleranceSemitones
    int scanMin = 0; ///< selection starts here (edge rule above the range): max (2, tauMin / 2)
    bool valid = false;
};

LagRange lagRangeFor (double fa, std::size_t windowSize, double f0MinHz, double f0MaxHz) noexcept
{
    LagRange r;

    if (! (fa > 0.0) || ! (f0MinHz > 0.0) || ! (f0MaxHz > f0MinHz))
        return r;

    // The window holds 2 * (ceil (fa / f0Min) + 1) samples (Framer). The candidates reach
    // rangeToleranceSemitones below f0 min, so that a tone at f0 min that reads a few cents flat (a
    // singer on the lowest note of the range, or the high-pass transient right after an onset) is
    // still inside: with only the one guard lag of the window the lower edge used to be one lag
    // (~6 cents at 82 Hz) below f0 min, and such frames were unvoiced (up to 7 ms extra onset latency at
    // f0 min in the pipeline tests). The extension (about 3 %) comes out of YIN's integration length,
    // never out of the window, so the latency model is unchanged.
    const auto nominalMaxLag = static_cast<int> (windowSize / 2);
    const auto tolerance = std::exp2 (PitchDetector::rangeToleranceSemitones / 12.0);
    r.tauMin = std::max (2, static_cast<int> (std::floor (fa / f0MaxHz)));
    r.tauMax = std::min (static_cast<int> (std::ceil (fa * tolerance / f0MinHz)), nominalMaxLag + nominalMaxLag / 16);
    r.maxLag = r.tauMax + 1;
    r.scanMin = std::max (2, r.tauMin / 2);
    r.valid = r.tauMin <= r.tauMax && r.maxLag < static_cast<int> (windowSize);
    return r;
}

/** Lag-function buffer length for windows up to maxWindow samples (see lagRangeFor: maxLag <=
    maxWindow / 2 + maxWindow / 32 + 1). */
std::size_t lagBufferSizeFor (int maxWindow) noexcept
{
    return static_cast<std::size_t> (maxWindow / 2 + maxWindow / 32 + 4);
}

/** Window to analyse: at most the prepared size (the newest samples if a caller passes more). */
std::span<const float> limitWindow (std::span<const float> window, int maxWindow) noexcept
{
    const auto limit = static_cast<std::size_t> (std::max (0, maxWindow));
    return window.size() > limit ? window.last (limit) : window;
}

/** rmsDb of the newest rmsSamplesFor (fa, f0Min) samples. */
float frameRmsDb (std::span<const float> window, double fa, double f0MinHz) noexcept
{
    const auto n = std::min (window.size(), static_cast<std::size_t> (rmsSamplesFor (fa, f0MinHz)));
    return computeRmsDb (window.last (n));
}

/** True if the newest period (frameRmsDb) and the whole window differ by more than maxLevelStepDb:
    the window straddles an onset or a release, where both lag functions are biased (see PitchDetector). */
bool isLevelStep (std::span<const float> window, float frameRmsDb) noexcept
{
    const auto windowRmsDb = computeRmsDb (window);
    return std::abs (frameRmsDb - windowRmsDb) > PitchDetector::maxLevelStepDb;
}

/** Plain float loops with four independent accumulators (breaks the add dependency chain; the
    summation order is fixed, so results are deterministic). */
float dotProduct (const float* a, const float* b, int n) noexcept
{
    float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, s3 = 0.0f;
    int i = 0;

    for (; i + 4 <= n; i += 4)
    {
        s0 += a[i] * b[i];
        s1 += a[i + 1] * b[i + 1];
        s2 += a[i + 2] * b[i + 2];
        s3 += a[i + 3] * b[i + 3];
    }

    for (; i < n; ++i)
        s0 += a[i] * b[i];

    return (s0 + s1) + (s2 + s3);
}

float squaredDistance (const float* a, const float* b, int n) noexcept
{
    float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, s3 = 0.0f;
    int i = 0;

    for (; i + 4 <= n; i += 4)
    {
        const auto d0 = a[i] - b[i];
        const auto d1 = a[i + 1] - b[i + 1];
        const auto d2 = a[i + 2] - b[i + 2];
        const auto d3 = a[i + 3] - b[i + 3];
        s0 += d0 * d0;
        s1 += d1 * d1;
        s2 += d2 * d2;
        s3 += d3 * d3;
    }

    for (; i < n; ++i)
    {
        const auto d = a[i] - b[i];
        s0 += d * d;
    }

    return (s0 + s1) + (s2 + s3);
}

/** Vertex offset (-1..1) of the parabola through (tau-1, a), (tau, b), (tau+1, c); 0 if degenerate. */
double parabolicOffset (float a, float b, float c) noexcept
{
    const auto denominator = static_cast<double> (a) - 2.0 * static_cast<double> (b) + static_cast<double> (c);

    if (denominator == 0.0)
        return 0.0;

    const auto offset = 0.5 * (static_cast<double> (a) - static_cast<double> (c)) / denominator;
    return std::clamp (offset, -1.0, 1.0);
}

/** Value at the vertex of that parabola (the interpolated extremum value). */
float parabolicVertexValue (float a, float b, float c) noexcept
{
    const auto offset = parabolicOffset (a, b, c);
    return static_cast<float> (static_cast<double> (b) - 0.25 * (static_cast<double> (a) - static_cast<double> (c)) * offset);
}

/** Applies the range tolerance to an interpolated period; 0 = outside the range. A frequency inside the
    tolerance is returned as measured, never clamped to the limit: some table limits sit a few cents
    inside a note (Tenor f0 max 523 Hz < C5 = 523.25 Hz, Alto f0 min 147 Hz > D3 = 146.83 Hz), and a
    clamp made the edge notes always read in tune (tuner, True bend). */
float frequencyInRange (double fa, double period, double f0MinHz, double f0MaxHz) noexcept
{
    if (! (period > 0.0))
        return 0.0f;

    const auto f = fa / period;
    const auto tolerance = std::exp2 (PitchDetector::rangeToleranceSemitones / 12.0);

    if (f > f0MaxHz * tolerance || f < f0MinHz / tolerance)
        return 0.0f;

    return static_cast<float> (f);
}
} // namespace

//==============================================================================
void YinPitchDetector::prepare (double analysisSampleRate, int maxWindowSamples)
{
    sampleRate = analysisSampleRate > 0.0 ? analysisSampleRate : 16000.0;
    maxWindow = std::max (1, maxWindowSamples);
    difference.assign (lagBufferSizeFor (maxWindow), 0.0f);
}

PitchFrame YinPitchDetector::analyse (std::span<const float> input, double f0MinHz, double f0MaxHz) noexcept
{
    PitchFrame frame;
    const auto window = limitWindow (input, maxWindow);
    frame.rmsDb = frameRmsDb (window, sampleRate, f0MinHz);

    const auto lags = lagRangeFor (sampleRate, window.size(), f0MinHz, f0MaxHz);

    if (frame.rmsDb < silenceFloorDb || ! lags.valid || difference.size() < static_cast<std::size_t> (lags.maxLag + 2)
        || isLevelStep (window, frame.rmsDb))
        return frame;

    // Difference function d(tau) = sum_{j<Wi} (x[j] - x[j + tau])^2, Wi = size - maxLag, turned into the
    // cumulative-mean-normalised d'(tau) in place. d'(0) = 1.
    const auto* x = window.data();
    const auto integration = static_cast<int> (window.size()) - lags.maxLag;
    auto* dn = difference.data();
    dn[0] = 1.0f;
    double runningSum = 0.0;

    for (int tau = 1; tau <= lags.maxLag; ++tau)
    {
        const auto d = squaredDistance (x, x + tau, integration);
        runningSum += static_cast<double> (d);
        dn[tau] = runningSum > 0.0 ? static_cast<float> (static_cast<double> (d) * static_cast<double> (tau) / runningSum)
                                   : 1.0f;
    }

    // First dip below the absolute threshold, walked down to its local minimum (may run past tauMax:
    // then the dip belongs to a period below f0 min). A local minimum also counts when only its
    // interpolated (vertex) value is below the threshold: with periods of a few lags (Whistle) the
    // integer lags can miss the bottom of the dip by up to half a lag, which would otherwise skip the
    // true period and pick its double (octave down) ...
    // The vertex is only trusted when the parabola is plausible: d' >= 0 by definition, so a vertex below
    // 0 means the three points are no parabolic dip (e.g. at an onset, where the still silent integration
    // part leaves the placeholder d' = 1 up to some lag and then d' jumps to ~tau: the parabola through
    // (1, 1, 165) has its vertex at -19.5, which used to read as a confident pitch). Then the sampled
    // value is used, for the dip test and for the confidence.
    // At maxLag (the global-minimum fallback can end there) there is no right neighbour: d'(maxLag + 1)
    // is not computed for this frame (a stale value from an earlier, longer frame would make the
    // result depend on the history), so the sampled value is used, as in MPM.
    const auto vertexAt = [dn, &lags] (int tau) noexcept
    {
        if (tau >= lags.maxLag)
            return dn[tau];

        const auto vertex = parabolicVertexValue (dn[tau - 1], dn[tau], dn[tau + 1]);
        return vertex >= 0.0f ? vertex : dn[tau];
    };

    int chosen = -1;
    bool fromDip = true;

    for (int tau = lags.scanMin; tau <= lags.tauMax; ++tau)
    {
        if (dn[tau] < absoluteThreshold)
        {
            chosen = tau;

            while (chosen < lags.maxLag && dn[chosen + 1] < dn[chosen])
                ++chosen;

            break;
        }

        if (dn[tau] <= dn[tau - 1] && dn[tau] < dn[tau + 1] && vertexAt (tau) < absoluteThreshold)
        {
            chosen = tau;
            break;
        }
    }

    // ... else the global minimum (first occurrence) over the whole scanned range
    if (chosen < 0)
    {
        fromDip = false;
        chosen = lags.scanMin;

        for (int tau = lags.scanMin + 1; tau <= lags.maxLag; ++tau)
            if (dn[tau] < dn[chosen])
                chosen = tau;
    }

    // Sub-multiple check: a local minimum near chosen / 2 or chosen / 3 that is almost as deep wins
    // (repeated, so 4 T -> 2 T -> T). While the oldest part of the window (the integration part) is
    // still partly silent, i.e. in the frames right after an onset, the silent samples add the same
    // energy to d(T) and d(2 T), and the cumulative-mean normalisation then makes d'(2 T) the deeper
    // one: d'(T) = 0.16, d'(2 T) = 0.12 for a saw at 165 Hz with 10 % of the window still silent,
    // which started notes an octave low. For a genuine period T the sub-multiples are far worse
    // (even with a 3x 2nd harmonic d'(T / 2) ~ 0.2 against d'(T) ~ 0; 0.5-0.8 with more harmonics).
    for (bool refined = true; refined;)
    {
        refined = false;

        for (int k = 3; k >= 2 && ! refined; --k)
        {
            const auto centre = static_cast<double> (chosen) / static_cast<double> (k);
            const auto lo = std::max (lags.scanMin, static_cast<int> (std::floor (centre)) - 1);
            const auto hi = std::min (lags.maxLag - 1, static_cast<int> (std::ceil (centre)) + 1);
            int best = -1;

            for (int tau = lo; tau <= hi; ++tau)
                if (dn[tau] <= dn[tau - 1] && dn[tau] <= dn[tau + 1] && (best < 0 || dn[tau] < dn[best]))
                    best = tau;

            if (best > 0 && best < chosen && vertexAt (best) <= subMultipleRatio * vertexAt (chosen) + subMultipleSlack)
            {
                chosen = best;
                refined = true;
            }
        }
    }

    // Edge rule: the minimum must lie inside the range (an extremum at tauMin - 1 or below, or at
    // tauMax + 1, means the period is outside the range)
    if (chosen < lags.tauMin || chosen > lags.tauMax)
        return frame;

    // The period is interpolated on the raw d(tau) (recomputed for the three lags), not on d': the
    // cumulative-mean normalisation tilts the parabola by tau / sum, which with short periods (~15 lags at
    // 1 kHz) moved the estimate by more than 10 cents (de Cheveigne & Kawahara, step 5).
    const auto rawAt = [x, integration] (int tau) noexcept { return squaredDistance (x, x + tau, integration); };
    const auto offset = parabolicOffset (rawAt (chosen - 1), rawAt (chosen), rawAt (chosen + 1));
    const auto frequency = frequencyInRange (sampleRate, static_cast<double> (chosen) + offset, f0MinHz, f0MaxHz);

    if (frequency > 0.0f)
    {
        frame.frequencyHz = frequency;
        frame.confidence = std::clamp (1.0f - vertexAt (chosen), 0.0f, 1.0f);

        // No dip under the threshold: not periodic enough to start a note (see fallbackMaxConfidence)
        if (! fromDip)
            frame.confidence = std::min (frame.confidence, fallbackMaxConfidence);
    }

    return frame;
}

//==============================================================================
void MpmPitchDetector::prepare (double analysisSampleRate, int maxWindowSamples)
{
    sampleRate = analysisSampleRate > 0.0 ? analysisSampleRate : 16000.0;
    maxWindow = std::max (1, maxWindowSamples);
    nsdf.assign (lagBufferSizeFor (maxWindow), 0.0f);
}

PitchFrame MpmPitchDetector::analyse (std::span<const float> input, double f0MinHz, double f0MaxHz) noexcept
{
    PitchFrame frame;
    const auto window = limitWindow (input, maxWindow);
    frame.rmsDb = frameRmsDb (window, sampleRate, f0MinHz);

    const auto lags = lagRangeFor (sampleRate, window.size(), f0MinHz, f0MaxHz);

    if (frame.rmsDb < silenceFloorDb || ! lags.valid || nsdf.size() < static_cast<std::size_t> (lags.maxLag + 2)
        || isLevelStep (window, frame.rmsDb))
        return frame;

    // NSDF n(tau) = 2 r(tau) / m(tau) over the whole window; m(tau) (sum of squares of both overlapping
    // parts) is updated incrementally in double.
    const auto* x = window.data();
    const auto size = static_cast<int> (window.size());
    auto* n = nsdf.data();
    double m = 2.0 * static_cast<double> (dotProduct (x, x, size));

    for (int tau = 0; tau <= lags.maxLag; ++tau)
    {
        if (tau > 0)
        {
            const auto head = static_cast<double> (x[tau - 1]);
            const auto tail = static_cast<double> (x[size - tau]);
            m = std::max (0.0, m - head * head - tail * tail);
        }

        const auto r = static_cast<double> (dotProduct (x, x + tau, size - tau));
        n[tau] = m > 0.0 ? static_cast<float> (2.0 * r / m) : 0.0f;
    }

    // Key maxima: skip the lobe around tau = 0 (up to the first value <= 0), then every positive lobe
    // (a lobe still open at maxLag ends there) contributes its highest value. Only key maxima at
    // tau >= scanMin take part (scanMin < tauMin, so a period above f0 max is seen and rejected below).
    // Key maxima are compared by their interpolated (vertex) values, as in McLeod & Wyvill: with
    // periods of a few lags the integer peak can be far below the true one and lose against 2 T.
    // (A lobe still open at maxLag has no right neighbour: its value is n(maxLag), never interpolated.)
    const auto vertexAt = [n, &lags] (int tau) noexcept
    { return tau < lags.maxLag ? parabolicVertexValue (n[tau - 1], n[tau], n[tau + 1]) : n[tau]; };

    const auto forEachKeyMaximum = [&] (auto&& visit) noexcept
    {
        int tau = 1;

        while (tau <= lags.maxLag && n[tau] > 0.0f)
            ++tau;

        while (tau <= lags.maxLag)
        {
            while (tau <= lags.maxLag && n[tau] <= 0.0f)
                ++tau;

            if (tau > lags.maxLag)
                break;

            int best = tau;

            for (; tau <= lags.maxLag && n[tau] > 0.0f; ++tau)
                if (n[tau] > n[best])
                    best = tau;

            if (best >= lags.scanMin && visit (best))
                return;
        }
    };

    float highest = 0.0f;
    forEachKeyMaximum ([&] (int tau) noexcept
    {
        highest = std::max (highest, vertexAt (tau));
        return false;
    });

    if (! (highest > 0.0f))
        return frame;

    int chosen = -1;
    const auto threshold = keyThreshold * highest;
    forEachKeyMaximum ([&] (int tau) noexcept
    {
        if (vertexAt (tau) < threshold)
            return false;

        chosen = tau;
        return true;
    });

    // Edge rule: the peak must lie inside the range (a key maximum below tauMin is a period above
    // f0 max; one at tauMax + 1 = maxLag is a lobe still rising at the end, i.e. a period below f0 min)
    if (chosen < lags.tauMin || chosen > lags.tauMax)
        return frame;

    const auto offset = parabolicOffset (n[chosen - 1], n[chosen], n[chosen + 1]);
    const auto frequency = frequencyInRange (sampleRate, static_cast<double> (chosen) + offset, f0MinHz, f0MaxHz);

    if (frequency > 0.0f)
    {
        frame.frequencyHz = frequency;
        frame.confidence = std::clamp (vertexAt (chosen), 0.0f, 1.0f);
    }

    return frame;
}

} // namespace vocalsynth
