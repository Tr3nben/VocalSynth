#include "vocalsynth/synth/WavetableOscillator.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

// The table read uses SSE2 where it exists (every x64 target: MSVC, clang, gcc) and plain C++ elsewhere.
// Both are deterministic within a build; cross-platform results are compared by fingerprints only (3.5).
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
 #define VOCALSYNTH_OSC_SSE2 1
 #include <emmintrin.h>
#else
 #define VOCALSYNTH_OSC_SSE2 0
#endif

namespace vocalsynth::synth
{

namespace
{
/** Lowest level whose harmonics stay <= 0.45 fs; down again only once they are <= 0.40 fs (6.1). */
int chooseMipLevel (int current, double maxIncrement) noexcept
{
    constexpr int top = Wavetable::numMipLevels - 1;
    int up = 0;

    while (up < top && Wavetable::maxHarmonic (up) * maxIncrement > WavetableOscillator::maxIncrementForLevel)
        ++up;

    if (up >= current)
        return up;

    int down = up;

    while (down < top && Wavetable::maxHarmonic (down) * maxIncrement > WavetableOscillator::hysteresisIncrementForLevel)
        ++down;

    return std::min (down, current);
}

/** Catmull-Rom weights of p[-1], p[0], p[1], p[2] as cubic polynomials in f (the binding formula of the
    header, y = ((c3 f + c2) f + c1) f + p[0], expanded): w = ((A f + B) f + C) f + D, and
    y = (w0 x[-1] + w1 x[0]) + (w2 x[1] + w3 x[2]). The two neighbour frames are blended linearly first (both
    steps are linear in the samples, so this equals interpolating two Hermite results with half the work). */
#if VOCALSYNTH_OSC_SSE2
using Lane = __m128;

/** Frame blend + Hermite; the result is in all four lanes. a / b: sample i - 1 of the level in the two frames. */
inline Lane readFrames (const float* a, const float* b, Lane frameFrac, float f) noexcept
{
    const Lane xa = _mm_loadu_ps (a);
    const Lane xb = _mm_loadu_ps (b);
    const Lane x = _mm_add_ps (xa, _mm_mul_ps (_mm_sub_ps (xb, xa), frameFrac));
    const Lane fv = _mm_set1_ps (f);
    Lane w = _mm_add_ps (_mm_mul_ps (_mm_setr_ps (-0.5f, 1.5f, -1.5f, 0.5f), fv), _mm_setr_ps (1.0f, -2.5f, 2.0f, -0.5f));
    w = _mm_add_ps (_mm_mul_ps (w, fv), _mm_setr_ps (-0.5f, 0.0f, 0.5f, 0.0f));
    w = _mm_add_ps (_mm_mul_ps (w, fv), _mm_setr_ps (0.0f, 1.0f, 0.0f, 0.0f));
    const Lane p = _mm_mul_ps (x, w);
    const Lane pairs = _mm_add_ps (p, _mm_shuffle_ps (p, p, _MM_SHUFFLE (2, 3, 0, 1)));
    return _mm_add_ps (pairs, _mm_shuffle_ps (pairs, pairs, _MM_SHUFFLE (1, 0, 3, 2)));
}

inline Lane broadcast (float v) noexcept { return _mm_set1_ps (v); }
inline Lane blend (Lane from, Lane to, Lane weight) noexcept { return _mm_add_ps (from, _mm_mul_ps (_mm_sub_ps (to, from), weight)); }
#else
using Lane = float;

inline float readFrames (const float* a, const float* b, float frameFrac, float f) noexcept
{
    constexpr float wa[4] = { -0.5f, 1.5f, -1.5f, 0.5f };
    constexpr float wb[4] = { 1.0f, -2.5f, 2.0f, -0.5f };
    constexpr float wc[4] = { -0.5f, 0.0f, 0.5f, 0.0f };
    constexpr float wd[4] = { 0.0f, 1.0f, 0.0f, 0.0f };
    float y[4];

    for (int j = 0; j < 4; ++j)
    {
        const float w = ((wa[j] * f + wb[j]) * f + wc[j]) * f + wd[j];
        y[j] = (a[j] + (b[j] - a[j]) * frameFrac) * w;
    }

    return (y[0] + y[1]) + (y[2] + y[3]);
}

inline float broadcast (float v) noexcept { return v; }
inline float blend (float from, float to, float weight) noexcept { return from + (to - from) * weight; }
#endif

/** Read position inside a level: x = phase * mipLength, i = floor (x), f = x - i. */
inline void splitPosition (double x, int& index, float& frac) noexcept
{
    index = static_cast<int> (x);
    frac = static_cast<float> (x - static_cast<double> (index));
}

/** Offset of each level relative to level 0 (Wavetable::levelOffset is a loop when called at run time). */
constexpr auto levelOffsets = []
{
    std::array<std::ptrdiff_t, Wavetable::numMipLevels> offsets {};

    for (int level = 0; level < Wavetable::numMipLevels; ++level)
        offsets[static_cast<std::size_t> (level)] = Wavetable::levelOffset (level) - Wavetable::levelOffset (0);

    return offsets;
}();

/** Where to read one table at one position: frame pointers of a level are base + frame * frameStride. */
struct FrameCursor
{
    const float* base = nullptr; ///< frame 0 of the table, sample 0 of the level
    const float* fromBase = nullptr; ///< same for the level faded out
    float framesMinusOne = 0.0f;
    int maxFirstFrame = 0;       ///< numFrames - 2 (0 for a single frame)
    int nextFrameOffset = 0;     ///< frameStride (0 for a single frame)

    void setup (const Wavetable* t, int level, int fromLevel) noexcept
    {
        const float* level0 = t->getLevel (0, 0);
        base = level0 + levelOffsets[static_cast<std::size_t> (level)];
        fromBase = level0 + levelOffsets[static_cast<std::size_t> (fromLevel)];
        const int frames = t->getNumFrames();
        framesMinusOne = static_cast<float> (frames - 1);
        maxFirstFrame = std::max (0, frames - 2);
        nextFrameOffset = frames > 1 ? Wavetable::frameStride : 0;
    }
};

inline void locate (const FrameCursor& c, float position, std::ptrdiff_t& offset, float& frac) noexcept
{
    const float fp = position * c.framesMinusOne;
    const int frame = std::min (static_cast<int> (fp), c.maxFirstFrame);
    frac = fp - static_cast<float> (frame);
    offset = static_cast<std::ptrdiff_t> (frame) * Wavetable::frameStride;
}
} // namespace

void WavetableOscillator::prepare (double newSampleRate) noexcept
{
    sampleRate = newSampleRate > 0.0 && std::isfinite (newSampleRate) ? newSampleRate : 48000.0;
    fadeTicks = std::max (1, static_cast<int> (std::ceil (tableCrossfadeSeconds * sampleRate
                                                          / static_cast<double> (controlTickSamples))));
    reset();
}

void WavetableOscillator::reset() noexcept
{
    table = nullptr;
    previousTable = nullptr;
    pendingTable = nullptr;
    mipLevel = 0;
    fromMipLevel = 0;
    unison = 1;
    renderVoices = 1;
    samplesLeftInTick = 0;
    fadeTick = 0;
    fading = false;
    started = false;
    jumpToTargets = true;
    phase.fill (0.0);
    increment.fill (0.0);
    incrementStep.fill (0.0);
    incrementTarget.fill (0.0);
    gainLeft.fill (0.0f);
    gainRight.fill (0.0f);
    gainLeftStep.fill (0.0f);
    gainRightStep.fill (0.0f);
    gainLeftTarget.fill (0.0f);
    gainRightTarget.fill (0.0f);
    startPhase.fill (0.0);
    position = positionStep = positionTarget = 0.0f;
    tableMix = tableMixTarget = 1.0f;
    tableMixStep = 0.0f;
    levelMix = levelMixTarget = 1.0f;
    levelMixStep = 0.0f;
    levelTarget = 0.0f;
}

void WavetableOscillator::setTable (const Wavetable* newTable) noexcept
{
    pendingTable = newTable;

    // Nothing sounds (never started since reset(), or level 0 reached and no crossfade): take it at once, so a
    // silent oscillator that is not ticked any more reports only the newest table. Everything else switches
    // at the next tick (beginTick).
    bool silent = ! started;

    if (! silent && ! fading && levelTarget == 0.0f)
    {
        silent = true;

        for (int u = 0; u < maxUnison; ++u)
            if (gainLeft[static_cast<std::size_t> (u)] != 0.0f || gainRight[static_cast<std::size_t> (u)] != 0.0f)
                silent = false;
    }

    if (silent)
    {
        table = newTable;
        previousTable = nullptr;
        fading = false;
        tableMix = tableMixTarget = 1.0f;
        tableMixStep = 0.0f;
    }
}

TablesInUse WavetableOscillator::getTablesInUse() const noexcept
{
    return { table, fading ? previousTable : nullptr };
}

void WavetableOscillator::restart (uint32_t seed, float phaseRandom) noexcept
{
    uint32_t state = seed != 0 ? seed : 0x9e3779b9u;
    const double amount = std::isfinite (phaseRandom) ? std::clamp (static_cast<double> (phaseRandom), 0.0, 1.0) : 0.0;

    for (int u = 0; u < maxUnison; ++u)
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        const double r = static_cast<double> (state >> 8) * (1.0 / 16777216.0); // [0, 1)
        startPhase[static_cast<std::size_t> (u)] = amount * r;
    }

    phase = startPhase;
    gainLeft.fill (0.0f);
    gainRight.fill (0.0f);
    gainLeftStep.fill (0.0f);
    gainRightStep.fill (0.0f);
    gainLeftTarget.fill (0.0f);
    gainRightTarget.fill (0.0f);
    levelTarget = 0.0f;

    table = pendingTable;
    previousTable = nullptr;
    fading = false;
    tableMix = tableMixTarget = 1.0f;
    tableMixStep = 0.0f;
    fromMipLevel = mipLevel;
    levelMix = levelMixTarget = 1.0f;
    levelMixStep = 0.0f;
    renderVoices = unison;
    started = true;
    jumpToTargets = true;
}

void WavetableOscillator::finishRamps() noexcept
{
    increment = incrementTarget;
    incrementStep.fill (0.0);
    gainLeft = gainLeftTarget;
    gainRight = gainRightTarget;
    gainLeftStep.fill (0.0f);
    gainRightStep.fill (0.0f);
    position = positionTarget;
    positionStep = 0.0f;
    tableMix = tableMixTarget;
    tableMixStep = 0.0f;
    levelMix = levelMixTarget;
    levelMixStep = 0.0f;
}

void WavetableOscillator::updateUnisonCache (int voices, float detuneCents, float pan, float spread) noexcept
{
    if (voices != cachedVoices || detuneCents != cachedDetune)
    {
        for (int u = 0; u < maxUnison; ++u)
        {
            double ratio = 1.0;

            if (voices > 1 && u < voices)
            {
                const double offset = static_cast<double> (detuneCents)
                                      * (static_cast<double> (u) / static_cast<double> (voices - 1) - 0.5);
                ratio = std::exp2 (offset / 1200.0);
            }

            detuneRatio[static_cast<std::size_t> (u)] = ratio;
        }
    }

    if (voices != cachedVoices || pan != cachedPan || spread != cachedSpread)
    {
        const float norm = 1.0f / std::sqrt (static_cast<float> (voices));

        for (int u = 0; u < maxUnison; ++u)
        {
            float p = pan;

            if (voices > 1)
                p += spread * (2.0f * static_cast<float> (u) / static_cast<float> (voices - 1) - 1.0f);

            p = std::clamp (p, -1.0f, 1.0f);
            const double angle = (static_cast<double> (p) + 1.0) * std::numbers::pi / 4.0;
            const auto i = static_cast<std::size_t> (u);
            panLeft[i] = norm * static_cast<float> (std::numbers::sqrt2 * std::cos (angle));
            panRight[i] = norm * static_cast<float> (std::numbers::sqrt2 * std::sin (angle));
        }
    }

    cachedVoices = voices;
    cachedDetune = detuneCents;
    cachedPan = pan;
    cachedSpread = spread;
}

void WavetableOscillator::beginTick (const OscillatorTick& target, int tickSamples) noexcept
{
    const int n = std::clamp (tickSamples, 1, controlTickSamples);
    const bool newTick = tickSamples >= controlTickSamples || samplesLeftInTick <= 0;

    // Sanitised targets
    const double inc = std::isfinite (target.increment) ? std::clamp (target.increment, 0.0, 0.5) : 0.0;
    const float pos = std::isfinite (target.position) ? std::clamp (target.position, 0.0f, 1.0f) : 0.0f;
    const float level = std::isfinite (target.level) ? std::max (target.level, 0.0f) : 0.0f;
    const float pan = std::isfinite (target.pan) ? std::clamp (target.pan, -1.0f, 1.0f) : 0.0f;
    const float detune = std::isfinite (target.detuneCents) ? std::clamp (target.detuneCents, 0.0f, 200.0f) : 0.0f;
    const float spread = std::isfinite (target.spread) ? std::clamp (target.spread, 0.0f, 1.0f) : 0.0f;

    if (newTick)
    {
        finishRamps();

        // Table crossfade: end, start, queue (6.1)
        if (fading && fadeTick >= fadeTicks)
        {
            fading = false;
            previousTable = nullptr;
            tableMix = tableMixTarget = 1.0f;
        }

        const bool silent = ! started || levelTarget == 0.0f;

        if (silent && pendingTable != table)
        {
            table = pendingTable;
            previousTable = nullptr;
            fading = false;
            tableMix = tableMixTarget = 1.0f;
        }
        else if (! fading && pendingTable != table)
        {
            previousTable = table;
            table = pendingTable;
            fading = true;
            fadeTick = 0;
            tableMix = 0.0f;
        }

        if (fading)
        {
            ++fadeTick;
            tableMixTarget = static_cast<float> (static_cast<double> (fadeTick) / static_cast<double> (fadeTicks));
        }

        // A level crossfade of the previous tick has ended
        fromMipLevel = mipLevel;
        levelMix = levelMixTarget = 1.0f;

        // Unison count: voices beyond the new count fade out over this tick, added ones start at their phase
        const int newUnison = std::clamp (target.unison, 1, maxUnison);

        for (int u = unison; u < newUnison; ++u)
        {
            const auto i = static_cast<std::size_t> (u);

            if (gainLeft[i] == 0.0f && gainRight[i] == 0.0f)
                phase[i] = startPhase[i];
        }

        int voices = newUnison;

        for (int u = newUnison; u < maxUnison; ++u)
            if (gainLeft[static_cast<std::size_t> (u)] != 0.0f || gainRight[static_cast<std::size_t> (u)] != 0.0f)
                voices = u + 1;

        const int oldUnison = unison;
        unison = newUnison;
        renderVoices = voices;
        updateUnisonCache (unison, detune, pan, spread);

        for (int u = 0; u < unison; ++u)
        {
            const auto i = static_cast<std::size_t> (u);
            incrementTarget[i] = inc * detuneRatio[i];

            if (u >= oldUnison && gainLeft[i] == 0.0f && gainRight[i] == 0.0f)
                increment[i] = incrementTarget[i];
        }
    }
    else
    {
        updateUnisonCache (unison, detune, pan, spread);

        for (int u = 0; u < unison; ++u)
            incrementTarget[static_cast<std::size_t> (u)] = inc * detuneRatio[static_cast<std::size_t> (u)];
    }

    positionTarget = pos;

    if (jumpToTargets)
    {
        for (int u = 0; u < unison; ++u)
            increment[static_cast<std::size_t> (u)] = incrementTarget[static_cast<std::size_t> (u)];

        position = pos;
    }

    // Mip level from the largest increment of the ramp over all sounding unison voices
    double maxInc = 0.0;

    for (int u = 0; u < renderVoices; ++u)
        maxInc = std::max ({ maxInc, increment[static_cast<std::size_t> (u)], incrementTarget[static_cast<std::size_t> (u)] });

    if (jumpToTargets)
    {
        mipLevel = fromMipLevel = chooseMipLevel (0, maxInc);
        levelMix = levelMixTarget = 1.0f;
    }
    else
    {
        const int wanted = chooseMipLevel (mipLevel, maxInc);

        if (wanted != mipLevel)
        {
            if (fromMipLevel == mipLevel)
            {
                levelMix = 0.0f; // crossfade from the current level over the (remaining) tick
            }
            else if (wanted == fromMipLevel)
            {
                fromMipLevel = mipLevel; // back to the level being faded out: reverse the running fade
                levelMix = 1.0f - levelMix;
            }
            // else (mid-tick re-target during a fade to a third level): the incoming level is replaced

            mipLevel = wanted;
        }

        levelMixTarget = 1.0f;
    }

    const bool audible = maxInc <= maxIncrementForLevel;
    levelTarget = audible ? level : 0.0f;

    for (int u = 0; u < maxUnison; ++u)
    {
        const auto i = static_cast<std::size_t> (u);
        const bool active = u < unison;
        gainLeftTarget[i] = active ? levelTarget * panLeft[i] : 0.0f;
        gainRightTarget[i] = active ? levelTarget * panRight[i] : 0.0f;
    }

    // Steps over the (remaining) tick
    const double invN = 1.0 / static_cast<double> (n);
    const float invNf = 1.0f / static_cast<float> (n);

    for (int u = 0; u < maxUnison; ++u)
    {
        const auto i = static_cast<std::size_t> (u);
        incrementStep[i] = (incrementTarget[i] - increment[i]) * invN;
        gainLeftStep[i] = (gainLeftTarget[i] - gainLeft[i]) * invNf;
        gainRightStep[i] = (gainRightTarget[i] - gainRight[i]) * invNf;
    }

    positionStep = (positionTarget - position) * invNf;
    tableMixStep = (tableMixTarget - tableMix) * invNf;
    levelMixStep = (levelMixTarget - levelMix) * invNf;
    samplesLeftInTick = n;
    jumpToTargets = false;
}

template <int numTables, bool levelFade>
void WavetableOscillator::renderSegment (float* left, float* right, int numSamples) noexcept
{
    // numSamples <= controlTickSamples. Per-sample values shared by the unison voices (frame cursors, mixes)
    // first, then voice by voice over the segment with the voice's state in registers, summed per sample in
    // voice order (the same operations for any split of the tick).
    // Table A = table (weight tableMix), table B = previousTable (weight 1 - tableMix). With one table the
    // weight is wOffset + wScale * tableMix: 1 without a crossfade, the fade ramp when the other side is null.
    const Wavetable* tableA = table;
    [[maybe_unused]] const Wavetable* tableB = previousTable;
    [[maybe_unused]] float wOffset = 0.0f;
    [[maybe_unused]] float wScale = 1.0f;

    if constexpr (numTables == 1)
    {
        if (tableA == nullptr) // fading out to nothing
        {
            tableA = tableB;
            wOffset = 1.0f;
            wScale = -1.0f;
        }
    }

    [[maybe_unused]] FrameCursor ca;
    [[maybe_unused]] FrameCursor cb;
    [[maybe_unused]] const double lenTo = static_cast<double> (Wavetable::mipLength (mipLevel));
    [[maybe_unused]] const double lenFrom = static_cast<double> (Wavetable::mipLength (fromMipLevel));

    if constexpr (numTables >= 1)
        ca.setup (tableA, mipLevel, fromMipLevel);

    if constexpr (numTables == 2)
        cb.setup (tableB, mipLevel, fromMipLevel);

    std::array<std::ptrdiff_t, controlTickSamples> offA {}, offB {};
    std::array<Lane, controlTickSamples> fracA {}, fracB {}, tMix {}, lMix {};
    std::array<float, controlTickSamples> tMixScalar {};

    {
        float pos = position;
        float tm = tableMix;
        float lm = levelMix;

        for (int s = 0; s < numSamples; ++s)
        {
            const auto i = static_cast<std::size_t> (s);
            float frac = 0.0f;

            if constexpr (numTables >= 1)
            {
                locate (ca, pos, offA[i], frac);
                fracA[i] = broadcast (frac);
            }

            if constexpr (numTables == 2)
            {
                locate (cb, pos, offB[i], frac);
                fracB[i] = broadcast (frac);
            }

            tMix[i] = broadcast (tm);
            tMixScalar[i] = tm;
            lMix[i] = broadcast (lm);
            pos += positionStep;
            tm += tableMixStep;
            lm += levelMixStep;
        }

        position = pos;
        tableMix = tm;
        levelMix = lm;
    }

#if VOCALSYNTH_OSC_SSE2
    std::array<Lane, controlTickSamples> sum; // lanes: left, right, -, -
    sum.fill (_mm_setzero_ps());
#else
    std::array<float, controlTickSamples> sumL {}, sumR {};
#endif

    for (int u = 0; u < renderVoices; ++u)
    {
        const auto v = static_cast<std::size_t> (u);
        double ph = phase[v];
        double inc = increment[v];
        const double incStep = incrementStep[v];
#if VOCALSYNTH_OSC_SSE2
        Lane gain = _mm_setr_ps (gainLeft[v], gainRight[v], 0.0f, 0.0f);
        const Lane gainStep = _mm_setr_ps (gainLeftStep[v], gainRightStep[v], 0.0f, 0.0f);
#else
        float gl = gainLeft[v];
        float gr = gainRight[v];
        const float glStep = gainLeftStep[v];
        const float grStep = gainRightStep[v];
#endif

        for (int s = 0; s < numSamples; ++s)
        {
            const auto i = static_cast<std::size_t> (s);
            Lane value {};
            int index = 0;
            float f = 0.0f;

            if constexpr (numTables >= 1)
            {
                splitPosition (ph * lenTo, index, f);
                const float* a0 = ca.base + offA[i] + index - 1;
                value = readFrames (a0, a0 + ca.nextFrameOffset, fracA[i], f);

                if constexpr (levelFade)
                {
                    int fromIndex = 0;
                    float fromF = 0.0f;
                    splitPosition (ph * lenFrom, fromIndex, fromF);
                    const float* f0 = ca.fromBase + offA[i] + fromIndex - 1;
                    value = blend (readFrames (f0, f0 + ca.nextFrameOffset, fracA[i], fromF), value, lMix[i]);
                }
            }

            if constexpr (numTables == 2)
            {
                const float* b0 = cb.base + offB[i] + index - 1;
                Lane vb = readFrames (b0, b0 + cb.nextFrameOffset, fracB[i], f);

                if constexpr (levelFade)
                {
                    int fromIndex = 0;
                    float fromF = 0.0f;
                    splitPosition (ph * lenFrom, fromIndex, fromF);
                    const float* f0 = cb.fromBase + offB[i] + fromIndex - 1;
                    vb = blend (readFrames (f0, f0 + cb.nextFrameOffset, fracB[i], fromF), vb, lMix[i]);
                }

                value = blend (vb, value, tMix[i]);
            }

#if VOCALSYNTH_OSC_SSE2
            sum[i] = _mm_add_ps (sum[i], _mm_mul_ps (value, gain));
            gain = _mm_add_ps (gain, gainStep);
#else
            sumL[i] += value * gl;
            sumR[i] += value * gr;
            gl += glStep;
            gr += grStep;
#endif
            ph += inc;

            if (ph >= 1.0) [[unlikely]] // a branch: a select would sit on the loop-carried phase chain
                ph -= 1.0;

            inc += incStep;
        }

        phase[v] = ph;
        increment[v] = inc;
#if VOCALSYNTH_OSC_SSE2
        alignas (16) float g[4];
        _mm_store_ps (g, gain);
        gainLeft[v] = g[0];
        gainRight[v] = g[1];
#else
        gainLeft[v] = gl;
        gainRight[v] = gr;
#endif
    }

    if constexpr (numTables >= 1)
    {
        for (int s = 0; s < numSamples; ++s)
        {
            const auto i = static_cast<std::size_t> (s);
#if VOCALSYNTH_OSC_SSE2
            alignas (16) float lr[4];
            _mm_store_ps (lr, sum[i]);
            const float sl = lr[0];
            const float sr = lr[1];
#else
            const float sl = sumL[i];
            const float sr = sumR[i];
#endif

            if constexpr (numTables == 1)
            {
                const float w = wOffset + wScale * tMixScalar[i];
                left[s] += sl * w;
                right[s] += sr * w;
            }
            else
            {
                left[s] += sl;
                right[s] += sr;
            }
        }
    }
}

void WavetableOscillator::render (float* left, float* right, int numSamples) noexcept
{
    while (numSamples > 0)
    {
        // Past the end of the ramp (render called without a new beginTick): hold the targets.
        if (samplesLeftInTick <= 0)
        {
            finishRamps();
            samplesLeftInTick = std::min (numSamples, controlTickSamples);
        }

        const int n = std::min (numSamples, samplesLeftInTick);
        const bool levelFade = fromMipLevel != mipLevel;
        const bool twoTables = fading && table != nullptr && previousTable != nullptr;
        const bool anyTable = table != nullptr || (fading && previousTable != nullptr);

        if (twoTables)
            levelFade ? renderSegment<2, true> (left, right, n) : renderSegment<2, false> (left, right, n);
        else if (anyTable)
            levelFade ? renderSegment<1, true> (left, right, n) : renderSegment<1, false> (left, right, n);
        else
            renderSegment<0, false> (left, right, n);

        samplesLeftInTick -= n;
        left += n;
        right += n;
        numSamples -= n;

        if (samplesLeftInTick == 0)
            finishRamps();
    }
}

} // namespace vocalsynth::synth
