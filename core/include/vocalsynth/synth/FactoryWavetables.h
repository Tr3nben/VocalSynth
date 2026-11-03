#pragma once

#include "vocalsynth/synth/WavetableBuilder.h"

#include <atomic>
#include <optional>
#include <string_view>

namespace vocalsynth::synth
{

/** The factory wavetables, generated in code (no assets, docs/SYNTH-DESIGN.md 5.5). The id strings are
    PERMANENT (saved as "factory:<id>" in projects and presets); only append new tables. */
enum class FactoryTable
{
    basicShapes = 0, ///< sine -> triangle -> saw -> square -> pulse (keyframes at 0, 25, 50, 75, 100 %)
    pwm,             ///< pulse width 50 % -> 2 %
    harmonicSweep,   ///< odd -> even harmonics
    sawBrightDark,   ///< saw with a falling spectral tilt
    formantVowel,    ///< additive saw with formant envelopes A - E - I - O - U
    syncSweep,       ///< hard-sync ratio 1 -> 8 (band-limited via the FFT)
    fm2Op,           ///< 2-operator FM, index 0 -> 8
    fold,            ///< sine through a wavefolder, gain 1 -> 8
    organ,           ///< drawbar combinations
    digital,         ///< sample-and-bit quantisation sweep
    spectralRandom,  ///< seeded random spectra, interpolated
    bell,            ///< inharmonic-ish bell spectra (rounded to harmonics)
    squareStack,     ///< stacked detuned-octave squares
    choir            ///< mixtures of vowel formants
};
inline constexpr int numFactoryTables = 14;

struct FactoryTableInfo
{
    std::string_view id;          ///< permanent, e.g. "basic-shapes"
    std::string_view displayName; ///< e.g. "Basic Shapes"
    int numFrames = 0;            ///< frames generated (<= Wavetable::maxFrames)
};

/** Default table of both oscillators (new instance, old project, missing reference, Init preset). */
inline constexpr FactoryTable defaultFactoryTable = FactoryTable::basicShapes;

/** Factory table data (pure, any thread). Owner: agent A. */
class FactoryWavetables
{
public:
    static const FactoryTableInfo& getInfo (FactoryTable table) noexcept;
    static std::optional<FactoryTable> fromId (std::string_view id) noexcept;

    /** The raw frames (numFrames * Wavetable::frameSize), deterministic (seeded generators). Allocates. */
    static std::vector<float> generateFrames (FactoryTable table);

    /** generateFrames + WavetableBuilder::build. Background thread; < 150 ms for any factory table
        (Release). */
    static WavetableBuildResult build (FactoryTable table, const std::atomic<bool>* cancel = nullptr);
};

} // namespace vocalsynth::synth
