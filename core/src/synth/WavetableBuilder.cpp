#include "vocalsynth/synth/WavetableBuilder.h"

#include "vocalsynth/synth/RealFft.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <numbers>
#include <utility>

namespace vocalsynth::synth
{

namespace
{
constexpr int frameSize = Wavetable::frameSize;
constexpr int maxKeptHarmonic = frameSize / 2 - 1; // 1023: bin 1024 (source Nyquist) is always dropped

bool isCancelled (const std::atomic<bool>* cancel) noexcept
{
    return cancel != nullptr && cancel->load (std::memory_order_relaxed);
}

int log2Of (int powerOfTwo) noexcept
{
    int order = 0;

    while ((1 << order) < powerOfTwo)
        ++order;

    return order;
}

double finiteOrZero (float x) noexcept
{
    return std::isfinite (x) ? static_cast<double> (x) : 0.0;
}

bool isPowerOfTwo (int v) noexcept
{
    return v > 0 && (v & (v - 1)) == 0;
}

/** Writes the guard copies of one level (p[-1] = last, p[N], p[N + 1] = first two samples). */
void writeGuards (float* p, int length) noexcept
{
    p[-1] = p[length - 1];
    p[length] = p[0];
    p[length + 1] = p[1];
}

/** The FFT plans of one build (allocated once per build, background thread). */
struct Plans
{
    RealFft source { 11 };                     // 2048
    std::array<RealFft, 4> levels { RealFft { 10 }, RealFft { 11 }, RealFft { 12 }, RealFft { 13 } }; // 1024..8192
    std::vector<double> time = std::vector<double> (8192);
    std::vector<std::complex<double>> spectrum = std::vector<std::complex<double>> (1025);
    std::vector<std::complex<double>> padded = std::vector<std::complex<double>> (4097);

    RealFft& forLength (int length) noexcept { return levels[static_cast<std::size_t> (log2Of (length) - 10)]; }
};

/** Spectrum (bins 0..1024 of a 2048 frame) -> one frame of the given length (>= 2048 or the source size
    itself), keeping bins 1..maxBin, scaled so the amplitudes stay the same. */
void spectrumToFrame (Plans& plans, std::span<const std::complex<double>> spectrum, int sourceLength, int maxBin,
                      int length, float* out)
{
    RealFft& fft = plans.forLength (length);
    const auto bins = static_cast<std::size_t> (length / 2 + 1);
    auto padded = std::span (plans.padded).first (bins);
    std::fill (padded.begin(), padded.end(), std::complex<double> {});
    const double scale = static_cast<double> (length) / static_cast<double> (sourceLength);
    const auto top = static_cast<std::size_t> (std::min ({ maxBin, length / 2 - 1, static_cast<int> (spectrum.size()) - 1 }));

    for (std::size_t k = 1; k <= top; ++k)
        padded[k] = spectrum[k] * scale;

    auto time = std::span (plans.time).first (static_cast<std::size_t> (length));
    fft.inverse (padded, time);

    for (std::size_t i = 0; i < time.size(); ++i)
        out[i] = static_cast<float> (time[i]);
}

/** Resamples one frame of any power-of-two size 32..8192 to 2048 via its spectrum (harmonics above 1023 and
    the source Nyquist dropped). */
void resampleFrame (std::span<const float> in, float* out)
{
    const int n = static_cast<int> (in.size());
    RealFft fft (log2Of (n));
    std::vector<double> time (in.size());
    std::transform (in.begin(), in.end(), time.begin(), finiteOrZero);
    std::vector<std::complex<double>> spectrum (static_cast<std::size_t> (n / 2 + 1));
    fft.forward (time, spectrum);

    const int maxBin = std::min (maxKeptHarmonic, n / 2 - 1);
    std::vector<std::complex<double>> bins (static_cast<std::size_t> (frameSize / 2 + 1));

    for (int k = 1; k <= maxBin; ++k)
        bins[static_cast<std::size_t> (k)] = spectrum[static_cast<std::size_t> (k)] * (static_cast<double> (frameSize) / n);

    RealFft inverse (11);
    std::vector<double> result (frameSize);
    inverse.inverse (bins, result);

    for (int i = 0; i < frameSize; ++i)
        out[i] = static_cast<float> (result[static_cast<std::size_t> (i)]);
}

/** One cycle of any length 2..maxSingleCycleSamples -> 2048 samples: direct DFT of harmonics
    1..min (1023, length / 2 - 1), inverse FFT at 2048. */
void resampleSingleCycle (std::span<const float> in, float* out)
{
    const auto n = in.size();
    const int maxBin = std::min (maxKeptHarmonic, static_cast<int> (n / 2) - 1);
    std::vector<double> cosTable (n), sinTable (n);

    for (std::size_t i = 0; i < n; ++i)
    {
        const double angle = 2.0 * std::numbers::pi * static_cast<double> (i) / static_cast<double> (n);
        cosTable[i] = std::cos (angle);
        sinTable[i] = std::sin (angle);
    }

    std::vector<std::complex<double>> bins (static_cast<std::size_t> (frameSize / 2 + 1));
    const double scale = static_cast<double> (frameSize) / static_cast<double> (n);

    for (int k = 1; k <= maxBin; ++k)
    {
        double re = 0.0;
        double im = 0.0;
        std::size_t index = 0;

        for (std::size_t i = 0; i < n; ++i)
        {
            const double x = finiteOrZero (in[i]);
            re += x * cosTable[index];
            im -= x * sinTable[index];
            index += static_cast<std::size_t> (k);

            if (index >= n)
                index -= n;
        }

        bins[static_cast<std::size_t> (k)] = { re * scale, im * scale };
    }

    RealFft inverse (11);
    std::vector<double> result (frameSize);
    inverse.inverse (bins, result);

    for (int i = 0; i < frameSize; ++i)
        out[i] = static_cast<float> (result[static_cast<std::size_t> (i)]);
}

/** Step 4 of build(): frames (already normalised PCM16 values) -> mip levels, hash. */
WavetableBuildResult buildLevels (WavetableFrames frames, std::span<const int16_t> pcm, const std::atomic<bool>* cancel)
{
    WavetableBuildResult result;
    auto table = std::make_unique<Wavetable> (frames.name, frames.numFrames);
    Plans plans;
    std::vector<double> source (frameSize);
    auto spectrum = std::span (plans.spectrum);

    for (int f = 0; f < frames.numFrames; ++f)
    {
        if (isCancelled (cancel))
        {
            result.error = WavetableBuildError::cancelled;
            return result;
        }

        const auto* in = frames.samples.data() + static_cast<std::size_t> (f) * frameSize;

        for (int i = 0; i < frameSize; ++i)
            source[static_cast<std::size_t> (i)] = static_cast<double> (in[i]);

        plans.source.forward (source, spectrum);

        for (int level = 0; level < Wavetable::numMipLevels; ++level)
        {
            const int length = Wavetable::mipLength (level);
            float* p = table->getLevelForWriting (f, level);
            spectrumToFrame (plans, spectrum, frameSize, std::min (Wavetable::maxHarmonic (level), maxKeptHarmonic),
                             length, p);
            writeGuards (p, length);
        }
    }

    table->setContentHash (WavetableBuilder::fnv1a64 (pcm));
    result.table = std::move (table);
    result.frames = std::move (frames);
    return result;
}
} // namespace

WavetableBuildResult WavetableBuilder::build (std::string name, std::span<const float> samples, int sourceFrameSize,
                                              const std::atomic<bool>* cancel)
{
    WavetableBuildResult result;

    if (samples.empty())
    {
        result.error = WavetableBuildError::noSamples;
        return result;
    }

    const bool singleCycle = sourceFrameSize == 0;

    if (singleCycle ? (samples.size() < 2 || samples.size() > maxSingleCycleSamples)
                    : (! isPowerOfTwo (sourceFrameSize) || sourceFrameSize < 32 || sourceFrameSize > 8192))
    {
        result.error = WavetableBuildError::invalidFrameSize;
        return result;
    }

    const std::size_t sourceSize = singleCycle ? samples.size() : static_cast<std::size_t> (sourceFrameSize);
    const std::size_t sourceFrames = samples.size() / sourceSize;

    if (sourceFrames == 0)
    {
        result.error = WavetableBuildError::noSamples;
        return result;
    }

    // 1. + 2. thin to maxFrames (frame i = source frame round (i (n - 1) / (maxFrames - 1))), resample to 2048
    const int numFrames = static_cast<int> (std::min (sourceFrames, static_cast<std::size_t> (Wavetable::maxFrames)));
    WavetableFrames frames;
    frames.name = name;
    frames.numFrames = numFrames;
    frames.samples.assign (static_cast<std::size_t> (numFrames) * frameSize, 0.0f);

    for (int i = 0; i < numFrames; ++i)
    {
        if (isCancelled (cancel))
        {
            result.error = WavetableBuildError::cancelled;
            return result;
        }

        std::size_t src = static_cast<std::size_t> (i);

        if (sourceFrames > static_cast<std::size_t> (Wavetable::maxFrames))
            src = static_cast<std::size_t> (std::llround (static_cast<double> (i) * static_cast<double> (sourceFrames - 1)
                                                          / static_cast<double> (Wavetable::maxFrames - 1)));

        const auto in = samples.subspan (src * sourceSize, sourceSize);
        float* out = frames.samples.data() + static_cast<std::size_t> (i) * frameSize;

        if (singleCycle)
            resampleSingleCycle (in, out);
        else if (sourceSize == static_cast<std::size_t> (frameSize))
            std::copy (in.begin(), in.end(), out);
        else
            resampleFrame (in, out);

        // 3. DC removed per frame (non-finite input samples count as 0)
        double sum = 0.0;

        for (int k = 0; k < frameSize; ++k)
        {
            if (! std::isfinite (out[k]))
                out[k] = 0.0f;

            sum += static_cast<double> (out[k]);
        }

        const double mean = sum / frameSize;

        for (int k = 0; k < frameSize; ++k)
            out[k] = static_cast<float> (static_cast<double> (out[k]) - mean);
    }

    // 3. peak-normalised over the whole table, quantised to PCM16 values
    float peak = 0.0f;

    for (const auto v : frames.samples)
        peak = std::max (peak, std::abs (v));

    const double gain = peak > 0.0f ? 1.0 / static_cast<double> (peak) : 0.0;
    std::vector<int16_t> pcm (frames.samples.size());

    for (std::size_t i = 0; i < pcm.size(); ++i)
    {
        pcm[i] = quantiseToPcm16 (static_cast<float> (static_cast<double> (frames.samples[i]) * gain));
        frames.samples[i] = pcm16ToFloat (pcm[i]);
    }

    // 4. mip levels
    return buildLevels (std::move (frames), pcm, cancel);
}

int WavetableBuilder::chooseFrameSize (std::size_t numSamples, int clmFrameSize, int overrideFrameSize) noexcept
{
    const auto holds = [numSamples] (int size) { return size > 0 && numSamples >= static_cast<std::size_t> (size); };

    if (overrideFrameSize > 0 && holds (overrideFrameSize))
        return overrideFrameSize;

    if (clmFrameSize > 0 && holds (clmFrameSize))
        return clmFrameSize;

    if (numSamples > 0 && numSamples % static_cast<std::size_t> (frameSize) == 0)
        return frameSize;

    if (numSamples >= 2 && numSamples <= maxSingleCycleSamples)
        return 0;

    return frameSize;
}

WavetableBuildResult WavetableBuilder::buildFromWav (std::string name, const WavReadResult& wav, int overrideFrameSize,
                                                     const std::atomic<bool>* cancel)
{
    if (! wav.ok())
    {
        WavetableBuildResult result;
        result.error = WavetableBuildError::readError;
        return result;
    }

    const int size = chooseFrameSize (wav.samples.size(), wav.clmFrameSize, overrideFrameSize);
    std::span<const float> samples (wav.samples);

    if (size > 0)
        samples = samples.first (samples.size() - samples.size() % static_cast<std::size_t> (size));

    return build (std::move (name), samples, size, cancel);
}

WavetableBuildResult WavetableBuilder::buildFromPcm16 (std::string name, std::span<const int16_t> pcm, int numFrames,
                                                       const std::atomic<bool>* cancel)
{
    if (numFrames <= 0 || numFrames > Wavetable::maxFrames
        || pcm.size() != static_cast<std::size_t> (numFrames) * frameSize)
    {
        WavetableBuildResult result;
        result.error = WavetableBuildError::noSamples;
        return result;
    }

    // The embedded frames ARE the normalised PCM16 frames of the import (WavetableFrames): only step 4, so the
    // rebuilt table is bit-identical to the imported one.
    WavetableFrames frames;
    frames.name = std::move (name);
    frames.numFrames = numFrames;
    frames.samples.resize (pcm.size());
    std::transform (pcm.begin(), pcm.end(), frames.samples.begin(), pcm16ToFloat);
    return buildLevels (std::move (frames), pcm, cancel);
}

std::unique_ptr<Wavetable> WavetableBuilder::makeSineTable()
{
    auto table = std::make_unique<Wavetable> ("Sine", 1);

    for (int level = 0; level < Wavetable::numMipLevels; ++level)
    {
        const int length = Wavetable::mipLength (level);
        float* p = table->getLevelForWriting (0, level);

        for (int i = 0; i < length; ++i)
            p[i] = static_cast<float> (std::sin (2.0 * std::numbers::pi * static_cast<double> (i) / static_cast<double> (length)));

        writeGuards (p, length);
    }

    return table;
}

int16_t WavetableBuilder::quantiseToPcm16 (float x) noexcept
{
    if (! std::isfinite (x))
        return 0;

    const auto scaled = static_cast<int> (std::lround (static_cast<double> (std::clamp (x, -1.0f, 1.0f)) * 32767.0));
    return static_cast<int16_t> (std::clamp (scaled, -32767, 32767));
}

uint64_t WavetableBuilder::fnv1a64 (std::span<const int16_t> pcm) noexcept
{
    uint64_t hash = 14695981039346656037ull;

    for (const auto v : pcm)
    {
        const auto u = static_cast<uint16_t> (v);

        for (const auto byte : { static_cast<uint8_t> (u & 0xffu), static_cast<uint8_t> (u >> 8) })
        {
            hash ^= byte;
            hash *= 1099511628211ull;
        }
    }

    return hash;
}

} // namespace vocalsynth::synth
