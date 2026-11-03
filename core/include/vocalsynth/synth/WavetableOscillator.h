#pragma once

#include "vocalsynth/synth/SynthSettings.h"
#include "vocalsynth/synth/Wavetable.h"

#include <array>
#include <cstdint>

namespace vocalsynth::synth
{

/** Targets of one oscillator for the END of the current control tick; the oscillator ramps linearly from
    its current values to these over the tick (docs/SYNTH-DESIGN.md 3, 6.1). */
struct OscillatorTick
{
    double increment = 0.0;   ///< cycles per sample of the CENTRE unison voice (pitch incl. octave, semi,
                              ///< fine, glide, bend and modulation), >= 0
    float position = 0.0f;    ///< 0..1 (frame position, interpolated between neighbour frames)
    float level = 0.0f;       ///< linear gain (osc level x modulation), >= 0; the oscillator itself
                              ///< scales the unison sum by 1 / sqrt (unison)
    float pan = 0.0f;         ///< -1..1
    float detuneCents = 0.0f; ///< total unison spread 0..200 (voice i of n: detune * (i / (n - 1) - 0.5))
    float spread = 0.0f;      ///< stereo width 0..1 of the unison voices (voice i: pan + spread * (2 i / (n - 1) - 1))
    int unison = 1;           ///< 1..maxUnisonVoices, switched at the tick (phases of new voices: see restart)
};

/** Which tables an oscillator reads right now (current, and the previous one during a table crossfade). */
struct TablesInUse
{
    const Wavetable* current = nullptr;
    const Wavetable* previous = nullptr;
};

/** One wavetable oscillator with up to 8 unison voices (docs/SYNTH-DESIGN.md 6.1). Phases, increments and
    gains are Structure-of-Arrays over the unison voices (the compiler vectorises over voices, never over
    time). Linear interpolation inside a mip level, linear between the two neighbour frames.

    Mip level: chosen per tick from the largest increment of the tick (all unison voices, start and end of
    the ramp): the lowest level L with maxHarmonic (L) * increment <= 0.45; going back DOWN to a level with
    more harmonics only once maxHarmonic * increment <= 0.40 (hysteresis). A level change crossfades
    linearly over that one tick (reads both levels). increment > 0.45 (fundamental above 0.45 fs): silent.

    Table change (setTable with another pointer): the old table keeps sounding and crossfades linearly to
    the new one over tableCrossfadeSeconds, rounded up to whole ticks, starting at the next tick. A change
    during a crossfade restarts it from the table that is current at that moment.

    No allocation, no locks: audio thread only. setTable() never dereferences a table until the next tick;
    nullptr means silence. Owner: agent A. */
class WavetableOscillator
{
public:
    static constexpr int maxUnison = maxUnisonVoices;
    static constexpr double tableCrossfadeSeconds = 0.005;
    static constexpr double maxIncrementForLevel = 0.45;
    static constexpr double hysteresisIncrementForLevel = 0.40;

    /** Not real-time critical, no allocation. Calls reset(). */
    void prepare (double sampleRate) noexcept;

    /** Silent, phases 0, current values 0, crossfades finished (keeps the table pointer). */
    void reset() noexcept;

    /** Table to read from the next tick on (see class comment). */
    void setTable (const Wavetable* table) noexcept;
    TablesInUse getTablesInUse() const noexcept;

    /** Note start (not legato): unison phase i = phaseRandom * r_i with r_i in [0, 1) from a xorshift32
        seeded with seed (phaseRandom 0: all phases 0). The gain ramp of the next tick starts from 0. */
    void restart (uint32_t seed, float phaseRandom) noexcept;

    /** Sets the ramps from the current values to target over tickSamples (1..16) samples. */
    void beginTick (const OscillatorTick& target, int tickSamples) noexcept;

    /** ADDS numSamples (<= the samples left in the current tick) to left / right. */
    void render (float* left, float* right, int numSamples) noexcept;

    int getMipLevel() const noexcept { return mipLevel; }

private:
    double sampleRate = 48000.0;
    const Wavetable* table = nullptr;
    const Wavetable* previousTable = nullptr;
    const Wavetable* pendingTable = nullptr;
    int mipLevel = 0;
    int unison = 1;
    std::array<double, maxUnison> phase {};
    std::array<double, maxUnison> increment {};
    std::array<float, maxUnison> gainLeft {};
    std::array<float, maxUnison> gainRight {};
};

} // namespace vocalsynth::synth
