#pragma once

#include "vocalsynth/synth/Wavetable.h"
#include "vocalsynth/synth/WavReader.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace vocalsynth::synth
{

/** Why a table could not be built. */
enum class WavetableBuildError
{
    none = 0,
    noSamples,       ///< empty input
    invalidFrameSize,///< frame size not a power of two in 32..8192 (and not single cycle)
    readError,       ///< the WavReadResult carried an error
    cancelled        ///< the cancel flag was set
};

/** Frames ready for the mip build and for embedding: numFrames * Wavetable::frameSize samples, DC removed,
    peak-normalised to 1 over the whole table, quantised to PCM16 values (pcm16ToFloat (quantise...)) so a
    table rebuilt from the embedded PCM16 data is bit-identical to the one built at import. */
struct WavetableFrames
{
    std::string name;
    int numFrames = 0;
    std::vector<float> samples;
};

struct WavetableBuildResult
{
    WavetableBuildError error = WavetableBuildError::none;
    std::unique_ptr<Wavetable> table; ///< null on error
    WavetableFrames frames;           ///< the normalised source frames (empty on error)

    bool ok() const noexcept { return error == WavetableBuildError::none && table != nullptr; }
};

/** Frames -> band-limited mip levels (docs/SYNTH-DESIGN.md 5.3). Deterministic: the same input gives a
    bit-identical table on the same build. All functions allocate: background thread only (WavetableWorker,
    FactoryWavetables, tests). Thread-safe (no shared state). Owner: agent A.

    Steps of build():
    1. frameSize != 2048: every frame is resampled to 2048 via its spectrum (FFT, harmonics above 1023
       dropped, zero-padded when shorter); frameSize 0 = the whole input is ONE cycle of any length
       (direct DFT of harmonics 1..min (1023, length / 2 - 1), then inverse FFT at 2048);
    2. more than Wavetable::maxFrames frames: thinned evenly (frame i of the result = source frame
       round (i * (n - 1) / (maxFrames - 1)));
    3. DC removed per frame, peak-normalised over the whole table (largest |sample| of all frames = 1),
       quantised to PCM16 (see WavetableFrames); frames are NOT phase-aligned or otherwise changed;
    4. per frame and mip level: FFT (2048) -> keep bins 1..min (maxHarmonic (level), 1023), zero the rest ->
       inverse FFT at mipLength (level) (1024..8192: zero-padded spectrum = band-limited upsampling) ->
       guard samples (Wavetable::guardBefore / guardAfter); content hash = fnv1a64 of the PCM16 frames. */
class WavetableBuilder
{
public:
    /** Longest input treated as ONE cycle (frameSize 0) by chooseFrameSize (single-cycle files are
        256..4096 samples; a longer file without clm chunk is cut into 2048-sample frames instead of being
        read as one huge cycle). */
    static constexpr std::size_t maxSingleCycleSamples = 8192;

    /** frameSize: samples per frame of the input (power of two 32..8192), or 0 for a single cycle.
        cancel (optional) is polled between frames. */
    static WavetableBuildResult build (std::string name, std::span<const float> samples, int frameSize,
                                       const std::atomic<bool>* cancel = nullptr);

    /** Frame size of an imported file (docs/SYNTH-DESIGN.md 5.4), first match: override > 0 (import dialog:
        256 / 512 / 1024 / 2048 / 4096) if the file holds at least one such frame; the clm chunk's size if the
        file holds at least one frame of it; 2048 if the length is a positive multiple of 2048; 0 (single
        cycle) if 2 <= length <= maxSingleCycleSamples; else 2048 (leftover samples ignored). */
    static int chooseFrameSize (std::size_t numSamples, int clmFrameSize, int overrideFrameSize) noexcept;

    /** readWav result -> table (chooseFrameSize + build). Leftover samples after the last whole frame are
        ignored. */
    static WavetableBuildResult buildFromWav (std::string name, const WavReadResult& wav, int overrideFrameSize,
                                              const std::atomic<bool>* cancel = nullptr);

    /** Rebuilds a table from embedded PCM16 frames (SynthState): numFrames * 2048 values. */
    static WavetableBuildResult buildFromPcm16 (std::string name, std::span<const int16_t> pcm, int numFrames,
                                                const std::atomic<bool>* cancel = nullptr);

    /** One frame (any size) whose every level is a pure sine: the oscillator's fallback until a table is
        ready (SynthEngine::prepare). */
    static std::unique_ptr<Wavetable> makeSineTable();

    /** round (x * 32767), clamped to -32767..32767. */
    static int16_t quantiseToPcm16 (float x) noexcept;
    static float pcm16ToFloat (int16_t v) noexcept { return static_cast<float> (v) / 32767.0f; }

    /** FNV-1a 64 over the little-endian bytes of the values (state / sharing key, docs/SYNTH-DESIGN.md 13). */
    static uint64_t fnv1a64 (std::span<const int16_t> pcm) noexcept;
};

} // namespace vocalsynth::synth
