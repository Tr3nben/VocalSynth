#pragma once

#include "vocalsynth/VsEvent.h"

#include <span>

namespace vocalsynth
{

/** Tiny mono sine voice so the user can hear the test pattern without any MIDI routing (M1, T6).

    Reacts to noteOn / noteOff / pitchBend (+-2 semitones) at their sampleOffsets, ignores CCs.
    Linear attack 5 ms, linear release 60 ms, peak gain about -12 dBFS. The same signal is ADDED to
    every channel. Output is exactly 0 when no note sounds; never produces NaN or denormals.

    The processor calls process() for EVERY block, also while the "Monitor Tone" switch is off, and
    uses setOutputEnabled() for the switch: the voice keeps tracking notes and bends while muted, so
    switching the tone on during a held note (Pitch Bend / CC Sweep patterns) is audible at once.
    The output gain ramps linearly over attackSeconds on both transitions (no click); while disabled
    and fully ramped down, nothing is written to the channels.
    Real-time safe: no allocation, no locks. Audio thread only. */
class MonitorSynth
{
public:
    static constexpr double attackSeconds = 0.005;
    static constexpr double releaseSeconds = 0.060;
    static constexpr float peakGain = 0.25f; // about -12 dBFS
    static constexpr double bendRangeSemitones = 2.0;

    /** May be called repeatedly with different rates. Calls reset(). */
    void prepare (double sampleRate) noexcept;

    /** Silences the voice immediately (no release). Keeps the setOutputEnabled() state; the output
        gain jumps to its target (nothing sounds after a reset anyway). */
    void reset() noexcept;

    /** Gate-off for the sounding note and pitch-bend centre, like the note-off + bend-centre pair
        of a MIDI flush: the voice fades out over releaseSeconds instead of being cut (no click when
        the processor restarts the pattern while the tone is audible). If nothing sounds, the output
        gain jumps to its target as in reset(). */
    void releaseAll() noexcept;

    /** Monitor Tone switch. Audio thread (called from processBlock before process()). */
    void setOutputEnabled (bool shouldOutput) noexcept { outputEnabled = shouldOutput; }
    bool isOutputEnabled() const noexcept { return outputEnabled; }

    /** Adds numSamples of the voice into channels[0..numChannels), applying the events of this
        block at their sampleOffsets (events must be sorted by offset; offsets outside
        [0, numSamples) are clamped). */
    template <std::size_t N>
    void process (const VsEventBuffer<N>& events, float* const* channels, int numChannels, int numSamples) noexcept
    {
        processEvents (events.view(), channels, numChannels, numSamples);
    }

    /** Non-template version of process(), implemented in the .cpp. */
    void processEvents (std::span<const VsEvent> events, float* const* channels, int numChannels,
                        int numSamples) noexcept;

    /** True while the voice is sounding (including the release tail). */
    bool isActive() const noexcept { return envelope > 0.0 || gateOn; }

private:
    double sampleRate = 48000.0;
    double phase = 0.0;      ///< 0..1
    double envelope = 0.0;   ///< 0..1
    bool gateOn = false;
    int currentNote = -1;
    double bendSemitones = 0.0;
    bool outputEnabled = true;  ///< default true so the synth can be tested standalone
    double outputGain = 1.0;    ///< ramps towards outputEnabled ? 1 : 0

    // Per-sample increments, computed in prepare() / when the pitch changes.
    double phaseIncrement = 0.0; ///< cycles per sample for currentNote + bendSemitones
    double attackStep = 0.0;     ///< envelope increment per sample (0 -> 1 in attackSeconds)
    double releaseStep = 0.0;    ///< envelope decrement per sample (1 -> 0 in releaseSeconds)
    double gainStep = 0.0;       ///< output gain ramp per sample (attackSeconds for a full ramp)

    void applyEvent (const VsEvent& e) noexcept;
    void updatePhaseIncrement() noexcept;
    void renderSegment (float* const* channels, int numChannels, int start, int end) noexcept;
};

} // namespace vocalsynth
