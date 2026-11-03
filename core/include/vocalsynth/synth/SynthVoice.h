#pragma once

#include "vocalsynth/synth/Envelope.h"
#include "vocalsynth/synth/ModMatrix.h"
#include "vocalsynth/synth/NoiseSource.h"
#include "vocalsynth/synth/SubOscillator.h"
#include "vocalsynth/synth/SvfFilter.h"
#include "vocalsynth/synth/SynthSettings.h"
#include "vocalsynth/synth/VoiceAllocator.h"
#include "vocalsynth/synth/WavetableOscillator.h"

#include <array>
#include <cstdint>

namespace vocalsynth::synth
{

/** What a voice needs from the engine at a control position (docs/SYNTH-DESIGN.md 3, 10.3). */
struct VoiceContext
{
    const SynthSettings* settings = nullptr;  ///< smoothed settings of this tick (never null when used)
    const ModSourceValues* global = nullptr;  ///< global sources at the tick end (LFOs, voice expression);
                                              ///< the per-voice entries are ignored (never null when used)
    float bendSemitones = 0.0f;               ///< smoothed pitch bend at the tick end
};

/** One synth voice: Osc A + Osc B (wavetable, unison), Sub, Noise -> drive + SVF -> amp (Env 1, velocity,
    Amp Level mod) -> voice pan (docs/SYNTH-DESIGN.md 10.3). Env 2 drives the filter (filterEnvAmount) and is
    a mod source, Env 3 is a mod source. Stereo all the way (unison spread).

    Control protocol (binding, docs/SYNTH-DESIGN.md 3.3):
    - at every tick boundary the engine first applies the commands at that sample (apply with
      samplesToNextTick == controlTickSamples: no ramp change), then calls controlTick(): envelopes, glide,
      per-voice sources and modulation advance by controlTickSamples, all targets are computed for the tick
      end and every sub-module ramps to them over the tick;
    - a command inside a tick (samplesToNextTick < controlTickSamples): the voice applies it at that sample
      (envelopes restart from the CURRENT ramp value), advances what restarted by samplesToNextTick and
      re-targets every ramp over the remaining samples;
    - render() between control positions only runs the per-sample DSP.
    Pitch (semitones): note + bend + glide offset + osc octave * 12 + semi + fine / 100 + pitch mod;
    glide is linear in semitones over glideTimeMs (constant time), evaluated per tick.
    Inactive (Env 1 idle and no steal fade): render() adds nothing and costs nothing.
    prepare() allocates (sub tables); everything else is real-time safe. Audio thread only. Owner: agent B. */
class SynthVoice
{
public:
    void prepare (double sampleRate, uint32_t seed);

    /** Silent and inactive at once (no fade), all states cleared. */
    void reset() noexcept;

    /** Tables for Osc A / B (engine, once per block; nullptr = silence for that oscillator). */
    void setTables (const Wavetable* oscA, const Wavetable* oscB) noexcept;
    TablesInUse getTablesInUse (int oscIndex) const noexcept;

    void apply (const VoiceCommand& command, const VoiceContext& context, int samplesToNextTick) noexcept;

    void controlTick (const VoiceContext& context) noexcept;

    /** ADDS numSamples (<= the samples left in the current tick) to left / right. */
    void render (float* left, float* right, int numSamples) noexcept;

    bool isActive() const noexcept;
    bool isReleased() const noexcept;
    int getNote() const noexcept { return note; }

    /** Per-voice mod sources at the last control position (env2, env3, velocity, note, randomPerNote);
        0 for the others. Used for the global destinations of the newest voice. */
    float getSourceValue (ModSource source) const noexcept;

    /** NaN resets of the voice filter (SvfFilter::getNanResetCount). */
    uint32_t getNanResetCount() const noexcept;

private:
    double sampleRate = 48000.0;
    std::array<WavetableOscillator, numOscillators> osc;
    SubOscillator sub;
    NoiseSource noise;
    SvfFilter filter;
    std::array<Envelope, numEnvelopes> env;
    int note = -1;
    float velocity = 0.0f;
    float randomValue = 0.0f;
    uint32_t random = 1;
    float pitch = 0.0f;       ///< semitones incl. glide, without bend / osc offsets
    float glideStart = 0.0f;
    float glideTarget = 0.0f;
    double glideProgress = 1.0;
    float ampGain = 0.0f;
    float ampStep = 0.0f;
    int stealFadeSamples = 0;
    VoiceCommand pendingStart {};
    bool hasPendingStart = false;
};

} // namespace vocalsynth::synth
