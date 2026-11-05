#include "vocalsynth/synth/SvfFilter.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace vocalsynth::synth
{

// TPT state-variable filter (docs/SYNTH-DESIGN.md 7, equations in the header). Per tick: g = tan (pi fc / fs)
// and k = 1 / Q for the tick end, per-sample linear ramps of g, k and the drive stage; the a1..a3 of the
// TPT structure are recomputed per sample only while a ramp runs. The ramps count the samples to the tick
// end in a member and land exactly on the targets there, so any split of a tick into process() calls
// performs the same operations (block-size invariance, 3.5).
//
// Drive: d > 0 dB runs x' = s (G x) / sqrt (G); d == 0 skips the stage. A switch between the two
// crossfades the dry and the saturated signal over that one tick (mix 0 <-> 1), so drive modulation through
// 0 dB does not step the level; with the mix 0 at both ends the stage is skipped (exactly linear).

namespace
{

constexpr float butterworthQ = 0.70710678f;
constexpr float lp24Stage1Q = 0.54119610f;
constexpr float lp24Stage2Q = 1.30656296f;
constexpr float denormalLimit = 1.0e-20f;

/** The TPT step of the header, rearranged so the state recursion has a short dependency chain
    (algebraically identical: ic1' = 2 v1 - ic1 = (2 a1 - 1) ic1 + 2 a2 v3, ic2' = 2 v2 - ic2 =
    ic2 + 2 a2 ic1 + 2 a3 v3, v1 = (ic1 + ic1') / 2, v2 = (ic2 + ic2') / 2). */
struct Coefficients
{
    float m11, b1, b2; ///< 2 a1 - 1, 2 a2, 2 a3
};

inline Coefficients makeCoefficients (float g, float k) noexcept
{
    const auto a1 = 1.0f / (1.0f + g * (g + k));
    const auto a2 = g * a1;
    return { 2.0f * a1 - 1.0f, 2.0f * a2, 2.0f * g * a2 };
}

/** One TPT SVF step. Returns v1 (band, without k) and v2 (low pass) through the references. */
inline void svfStep (float x, const Coefficients& c, float& ic1, float& ic2, float& v1, float& v2) noexcept
{
    const auto v3 = x - ic2;
    const auto n1 = c.m11 * ic1 + c.b1 * v3;
    const auto n2 = ic2 + c.b1 * ic1 + c.b2 * v3;
    v1 = 0.5f * (ic1 + n1);
    v2 = 0.5f * (ic2 + n2);
    ic1 = n1;
    ic2 = n2;
}

inline float saturate (float x, float gain, float norm, float mix) noexcept
{
    const auto u = gain * x;
    const auto wet = u / std::sqrt (1.0f + u * u) * norm;
    return x + mix * (wet - x);
}

template <FilterMode M>
inline float filterSample (float x, const Coefficients& c, const Coefficients& c1, float k,
                           std::array<float, 4>& s) noexcept
{
    float v1 = 0.0f, v2 = 0.0f;

    if constexpr (M == FilterMode::lp24)
    {
        svfStep (x, c1, s[0], s[1], v1, v2);
        const auto stage1 = v2;
        svfStep (stage1, c, s[2], s[3], v1, v2);
        return v2;
    }
    else
    {
        svfStep (x, c, s[0], s[1], v1, v2);

        if constexpr (M == FilterMode::lp12)
            return v2;
        else if constexpr (M == FilterMode::bp)
            return k * v1;
        else if constexpr (M == FilterMode::hp)
            return x - k * v1 - v2;
        else
            return x - k * v1;
    }
}

} // namespace

void SvfFilter::prepare (double newSampleRate) noexcept
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    k1 = 1.0f / lp24Stage1Q;
    gTarget = static_cast<float> (std::tan (std::numbers::pi * std::min (20000.0, 0.49 * sampleRate) / sampleRate));
    kTarget = 1.0f / butterworthQ;
    cachedCutoff = -1.0f;
    cachedResonance = -1.0f;
    cachedDrive = -1.0f;
    nanResets = 0;
    reset();
}

void SvfFilter::reset() noexcept
{
    for (auto& channel : state)
        channel.fill (0.0f);

    g = gTarget;
    k = kTarget;
    driveGain = driveGainTarget;
    driveNorm = driveNormTarget;
    driveMix = driveMixTarget;
    gStep = kStep = driveGainStep = driveNormStep = driveMixStep = 0.0f;
    rampLeft = 0;
    jumpToTarget = true;
}

void SvfFilter::beginTick (const FilterTick& target, int tickSamples) noexcept
{
    // A re-target inside a running ramp continues from the current values
    if (rampLeft > 0)
    {
        const auto p = static_cast<float> (rampTotal - rampLeft);
        g += gStep * p;
        k += kStep * p;
        driveGain += driveGainStep * p;
        driveNorm += driveNormStep * p;
        driveMix += driveMixStep * p;
        gStep = kStep = driveGainStep = driveNormStep = driveMixStep = 0.0f;
        rampLeft = 0;
    }

    // Guards at a control position (never at a segment end, docs/SYNTH-DESIGN.md 3.5)
    bool finite = true;

    for (const auto& channel : state)
        for (const auto v : channel)
            finite = finite && std::isfinite (v);

    if (! finite)
    {
        for (auto& channel : state)
            channel.fill (0.0f);

        ++nanResets;
    }
    else
    {
        for (auto& channel : state)
            for (auto& v : channel)
                if (std::abs (v) < denormalLimit)
                    v = 0.0f;
    }

    // Mode: switched here; LP24's second stage is not run by the other modes, so it starts from 0
    const auto newMode = static_cast<int> (target.mode) >= 0 && static_cast<int> (target.mode) < numFilterModes
                             ? target.mode
                             : FilterMode::lp24;

    if (newMode == FilterMode::lp24 && mode != FilterMode::lp24)
    {
        for (auto& channel : state)
            channel[2] = channel[3] = 0.0f;
    }

    mode = newMode;

    // Targets for the tick end
    // Targets for the tick end (tan / pow only when their inputs changed: budget, docs/SYNTH-DESIGN.md 18)
    const auto maxCutoff = static_cast<float> (0.49 * sampleRate);
    auto cutoff = std::isfinite (target.cutoffHz) ? target.cutoffHz : maxCutoff;
    cutoff = std::clamp (cutoff, 20.0f, maxCutoff);

    if (cutoff != cachedCutoff)
    {
        cachedCutoff = cutoff;
        gTarget = static_cast<float> (std::tan (std::numbers::pi * static_cast<double> (cutoff) / sampleRate));
    }

    const auto r = std::isfinite (target.resonance) ? std::clamp (target.resonance, 0.0f, 1.0f) : 0.0f;
    const bool fourPole = mode == FilterMode::lp24;

    if (r != cachedResonance || fourPole != cachedFourPole)
    {
        cachedResonance = r;
        cachedFourPole = fourPole;
        const auto q0 = fourPole ? lp24Stage2Q : butterworthQ;
        kTarget = 1.0f / (q0 * std::pow (qMax / q0, r));
    }

    const auto drive = std::isfinite (target.driveDb) ? std::clamp (target.driveDb, 0.0f, 24.0f) : 0.0f;

    if (drive > 0.0f)
    {
        if (drive != cachedDrive)
        {
            cachedDrive = drive;
            cachedDriveGain = std::pow (10.0f, drive / 20.0f);
            cachedDriveNorm = 1.0f / std::sqrt (cachedDriveGain);
        }

        driveGainTarget = cachedDriveGain;
        driveNormTarget = cachedDriveNorm;
        driveMixTarget = 1.0f;

        if (driveMix == 0.0f && driveMixStep == 0.0f)
        {
            // From the bypassed state: the drive amount itself jumps, the mix fades it in
            driveGain = driveGainTarget;
            driveNorm = driveNormTarget;
        }
    }
    else
    {
        driveMixTarget = 0.0f; // gain / norm stay where they are while the mix fades out
        driveGainTarget = driveGain;
        driveNormTarget = driveNorm;
    }

    const auto n = std::max (1, tickSamples);

    if (jumpToTarget)
    {
        jumpToTarget = false;
        g = gTarget;
        k = kTarget;
        driveGain = driveGainTarget;
        driveNorm = driveNormTarget;
        driveMix = driveMixTarget;
        gStep = kStep = driveGainStep = driveNormStep = driveMixStep = 0.0f;
        rampLeft = 0;
        return;
    }

    const auto inv = 1.0f / static_cast<float> (n);
    gStep = (gTarget - g) * inv;
    kStep = (kTarget - k) * inv;
    driveGainStep = (driveGainTarget - driveGain) * inv;
    driveNormStep = (driveNormTarget - driveNorm) * inv;
    driveMixStep = (driveMixTarget - driveMix) * inv;
    const bool ramping = gStep != 0.0f || kStep != 0.0f || driveGainStep != 0.0f || driveNormStep != 0.0f
                         || driveMixStep != 0.0f;
    rampLeft = ramping ? n : 0;
    rampTotal = n;
}

template <FilterMode M>
void SvfFilter::run (float* left, float* right, int numSamples, bool ramping, int rampPosition) noexcept
{
    // Everything in locals: the output pointers could alias the members, which would force a reload and a
    // store of every state per sample
    auto s0 = state[0];
    auto s1 = state[1];
    const auto gl = g, kl = k, dg = driveGain, dn = driveNorm, dm = driveMix;
    const auto kStage1 = k1;

    if (ramping)
    {
        // Ramp values from the position in the ramp (start + step * p), not accumulated: the same numbers for
        // any split, and the coefficient pass below vectorises (the divisions leave the recursion's chain)
        const auto gs = gStep, ks = kStep, dgs = driveGainStep, dns = driveNormStep, dms = driveMixStep;
        const bool drive = dm != 0.0f || dms != 0.0f;
        alignas (32) float m11[controlTickSamples], b1[controlTickSamples], b2[controlTickSamples];
        alignas (32) float m11s[controlTickSamples], b1s[controlTickSamples], b2s[controlTickSamples];
        alignas (32) float kk[controlTickSamples], gainA[controlTickSamples], normA[controlTickSamples],
            mixA[controlTickSamples];

        for (int done = 0; done < numSamples;)
        {
            const int m = std::min (numSamples - done, controlTickSamples);
            const auto base = static_cast<float> (rampPosition + done + 1);

            // Coefficient pass (structure of arrays, vectorised): a1 = 1 / (1 + g (g + k)), m11 = 2 a1 - 1,
            // b1 = 2 g a1, b2 = 2 g^2 a1; LP24 shares one division between both stages
            for (int i = 0; i < m; ++i)
            {
                const auto p = base + static_cast<float> (i);
                const auto gi = gl + gs * p;
                const auto ki = kl + ks * p;
                kk[i] = ki;
                const auto d = 1.0f + gi * (gi + ki);

                if constexpr (M == FilterMode::lp24)
                {
                    const auto d1 = 1.0f + gi * (gi + kStage1);
                    const auto rcp = 1.0f / (d * d1);
                    const auto a = d1 * rcp;
                    const auto a1 = d * rcp;
                    m11[i] = 2.0f * a - 1.0f;
                    b1[i] = 2.0f * gi * a;
                    b2[i] = b1[i] * gi;
                    m11s[i] = 2.0f * a1 - 1.0f;
                    b1s[i] = 2.0f * gi * a1;
                    b2s[i] = b1s[i] * gi;
                }
                else
                {
                    const auto a = 1.0f / d;
                    m11[i] = 2.0f * a - 1.0f;
                    b1[i] = 2.0f * gi * a;
                    b2[i] = b1[i] * gi;
                }

                gainA[i] = dg + dgs * p;
                normA[i] = dn + dns * p;
                mixA[i] = dm + dms * p;
            }

            for (int i = 0; i < m; ++i)
            {
                auto xl = left[done + i];
                auto xr = right[done + i];

                if (drive)
                {
                    xl = saturate (xl, gainA[i], normA[i], mixA[i]);
                    xr = saturate (xr, gainA[i], normA[i], mixA[i]);
                }

                const Coefficients c { m11[i], b1[i], b2[i] };
                const Coefficients c1 = M == FilterMode::lp24 ? Coefficients { m11s[i], b1s[i], b2s[i] } : c;
                left[done + i] = filterSample<M> (xl, c, c1, kk[i], s0);
                right[done + i] = filterSample<M> (xr, c, c1, kk[i], s1);
            }

            done += m;
        }
    }
    else
    {
        const auto c = makeCoefficients (gl, kl);
        const auto c1 = M == FilterMode::lp24 ? makeCoefficients (gl, kStage1) : c;

        if (dm != 0.0f)
        {
            for (int i = 0; i < numSamples; ++i)
            {
                left[i] = filterSample<M> (saturate (left[i], dg, dn, dm), c, c1, kl, s0);
                right[i] = filterSample<M> (saturate (right[i], dg, dn, dm), c, c1, kl, s1);
            }
        }
        else
        {
            for (int i = 0; i < numSamples; ++i)
            {
                left[i] = filterSample<M> (left[i], c, c1, kl, s0);
                right[i] = filterSample<M> (right[i], c, c1, kl, s1);
            }
        }
    }

    state[0] = s0;
    state[1] = s1;
}

void SvfFilter::process (float* left, float* right, int numSamples) noexcept
{
    while (numSamples > 0)
    {
        const bool ramping = rampLeft > 0;
        const auto n = ramping ? std::min (numSamples, rampLeft) : numSamples;
        const auto position = rampTotal - rampLeft;

        switch (mode)
        {
            case FilterMode::lp12:  run<FilterMode::lp12> (left, right, n, ramping, position); break;
            case FilterMode::lp24:  run<FilterMode::lp24> (left, right, n, ramping, position); break;
            case FilterMode::bp:    run<FilterMode::bp> (left, right, n, ramping, position); break;
            case FilterMode::hp:    run<FilterMode::hp> (left, right, n, ramping, position); break;
            case FilterMode::notch: run<FilterMode::notch> (left, right, n, ramping, position); break;
        }

        if (ramping)
        {
            rampLeft -= n;

            if (rampLeft == 0)
            {
                g = gTarget;
                k = kTarget;
                driveGain = driveGainTarget;
                driveNorm = driveNormTarget;
                driveMix = driveMixTarget;
                gStep = kStep = driveGainStep = driveNormStep = driveMixStep = 0.0f;
            }
        }

        left += n;
        right += n;
        numSamples -= n;
    }
}

} // namespace vocalsynth::synth
