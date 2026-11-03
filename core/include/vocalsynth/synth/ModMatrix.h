#pragma once

#include "vocalsynth/synth/SynthSettings.h"

#include <array>

namespace vocalsynth::synth
{

/** Current value of every mod source, indexed by static_cast<int> (ModSource); [off] is always 0. */
using ModSourceValues = std::array<float, numModSources>;

/** Summed modulation per destination, indexed by static_cast<int> (ModDest), in DESTINATION units
    (modDestScale): semitones, octaves, dB, linear gain or fraction. */
using ModDestValues = std::array<float, numModDests>;

/** Destination change at amount +100 % and source value 1 (docs/SYNTH-DESIGN.md 9.2):
    position +1 (whole table), pitch +12 semitones, osc / sub / noise level +1 (linear, added to the
    knob's 0..1 and clamped 0..1), detune +100 cents (clamped 0..200), cutoff +60 semitones (5 octaves),
    resonance +1 (clamped 0..1), drive +24 dB (clamped 0..24), amp level +1 (gain *= clamp (1 + m, 0, 2)),
    pan +1 (clamped -1..1), LFO rate +4 octaves, FX mix +1 (added to the mix fraction, clamped 0..1), pitch
    +12 semitones (added to the voice's note pitch like the bend: Osc A, Osc B, sub and filter keytrack). */
constexpr float modDestScale (ModDest dest) noexcept
{
    switch (dest)
    {
        case ModDest::off:             return 0.0f;
        case ModDest::oscAPosition:
        case ModDest::oscBPosition:    return 1.0f;
        case ModDest::oscAPitch:
        case ModDest::oscBPitch:       return 12.0f;
        case ModDest::oscALevel:
        case ModDest::oscBLevel:
        case ModDest::subLevel:
        case ModDest::noiseLevel:      return 1.0f;
        case ModDest::oscADetune:
        case ModDest::oscBDetune:      return 100.0f;
        case ModDest::filterCutoff:    return 60.0f;
        case ModDest::filterResonance: return 1.0f;
        case ModDest::filterDrive:     return 24.0f;
        case ModDest::ampLevel:        return 1.0f;
        case ModDest::pan:             return 1.0f;
        case ModDest::lfo1Rate:
        case ModDest::lfo2Rate:        return 4.0f;
        case ModDest::chorusMix:
        case ModDest::delayMix:
        case ModDest::reverbMix:       return 1.0f;
        case ModDest::pitch:           return 12.0f;
    }

    return 0.0f;
}

/** Sources that differ per voice (Env 2/3, velocity, note, random); the rest are global (LFOs, voice
    expression). */
constexpr bool isPerVoiceSource (ModSource source) noexcept
{
    return source == ModSource::env2 || source == ModSource::env3 || source == ModSource::velocity
           || source == ModSource::note || source == ModSource::randomPerNote;
}

/** Destinations that exist once per engine (LFO rates, FX mixes); evaluated at the tick with the global
    sources and the per-voice sources of the NEWEST active voice (0 if none). */
constexpr bool isGlobalDest (ModDest dest) noexcept
{
    return dest == ModDest::lfo1Rate || dest == ModDest::lfo2Rate || dest == ModDest::chorusMix
           || dest == ModDest::delayMix || dest == ModDest::reverbMix;
}

/** The 8-slot modulation matrix (docs/SYNTH-DESIGN.md 9). Stateless: out[d] = sum over the slots with
    dest d of (amountPercent / 100) * sources[source] * modDestScale (d). Slots with source or dest off,
    and non-finite inputs, contribute 0. Modulation is never written back into parameters.
    Real-time safe. Owner: agent B. */
class ModMatrix
{
public:
    static void compute (const std::array<ModSlotSettings, numModSlots>& slots, const ModSourceValues& sources,
                         ModDestValues& out) noexcept;

    /** compute() restricted to global (isGlobalDest) or to per-voice destinations. */
    static void computeGlobal (const std::array<ModSlotSettings, numModSlots>& slots,
                               const ModSourceValues& sources, ModDestValues& out) noexcept;
    static void computePerVoice (const std::array<ModSlotSettings, numModSlots>& slots,
                                 const ModSourceValues& sources, ModDestValues& out) noexcept;
};

} // namespace vocalsynth::synth
