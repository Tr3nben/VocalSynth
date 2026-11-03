#pragma once

#include "vocalsynth/EventTiming.h"
#include "vocalsynth/ExpressionFrame.h"
#include "vocalsynth/VsEvent.h"
#include "vocalsynth/synth/Lfo.h"
#include "vocalsynth/synth/ModMatrix.h"
#include "vocalsynth/synth/SynthSettings.h"
#include "vocalsynth/synth/SynthVoice.h"
#include "vocalsynth/synth/VoiceAllocator.h"
#include "vocalsynth/synth/Wavetable.h"
#include "vocalsynth/synth/fx/Chorus.h"
#include "vocalsynth/synth/fx/FdnReverb.h"
#include "vocalsynth/synth/fx/StereoDelay.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>

namespace vocalsynth::synth
{

/** Host tempo for the synced LFOs and the delay (the processor reads the play head once per block). */
struct SynthTransport
{
    double bpm = 120.0; ///< <= 0 or non-finite: 120
};

/** UI / diagnostics (any thread through the processor's atomics; the engine itself is audio-thread only). */
struct SynthEngineStats
{
    int activeVoices = 0;
    uint32_t nanResets = 0; ///< filter + delay + reverb NaN resets since prepare()
};

/** The 0.4.0 synth (docs/SYNTH-DESIGN.md 2, 3, 10, 11): VoiceAllocator -> 16 SynthVoice (Mono / Legato use
    voice 0) -> sum -> Chorus -> StereoDelay -> FdnReverb -> synthVolume -> out; 2 global LFOs; mod matrix.

    This is THE interface the processor uses (frozen for 0.4):

        prepareToPlay:  engine.prepare (sampleRate, maxChunk);                  // allocates, calls reset()
        block start:    engine.setWavetable (0, store.acquire (0)); ... (1)     // once per host block
        each chunk:     engine.process (events, timings, expression, settings, transport, L, R, n); // n <= 4096
        block end:      store.release (engine.getTablesInUse (0), engine.getTablesInUse (1));
        flush:          engine.releaseAll();      bypass / reset: engine.reset();

    Timing (binding): the engine keeps its own absolute sample clock (0 at reset()). Events are applied at
    clock + sampleOffset; all note events of one sample position form one VoiceAllocator group (bends at
    that position are applied before the group; CCs are ignored). Control ticks run at every clock value
    that is a multiple of controlTickSamples; at a sample that has both, the events come first. Settings
    are read per tick (the latest process() call's settings), continuous values smoothed with a 20 ms
    one-pole per tick, choices / ints / bools switched at the tick. Expression frames are latched at the
    first tick at or after their position (the newest frame of a tick wins) and ramp linearly to their
    values over the distance to the previous frame (16 .. 4096 samples). Pitch bend: bend / 8192 *
    bendRangeSemitones, one-pole 5 ms per tick. Result: BIT-IDENTICAL output for any split of the same
    input into process() calls (block sizes 1, 7, 64, 480, 512, 4096 at 44.1 / 48 / 96 kHz).

    synthOn: switching off fades the output over synthOnFadeSeconds, then resets the engine and returns
    zeros at almost no cost; switching on starts from silence (a held note sounds from the next note-on).
    Output: left / right are OVERWRITTEN with the synth signal (incl. FX and synthVolume; mono layouts:
    the processor uses left + right / 2). Silence after every voice and FX tail ended: exactly 0.

    prepare() allocates everything (voices, FX lines, fallback sine table); process(), reset(),
    releaseAll(), setWavetable() never allocate, lock, throw or log. Audio thread only. Owner: agent C. */
class SynthEngine
{
public:
    static constexpr int maxVoices = VoiceAllocator::maxVoices;
    static constexpr int maxChunkSamples = 4096;
    static constexpr double settingsSmoothingSeconds = 0.020;
    static constexpr double bendSmoothingSeconds = 0.005;
    static constexpr double synthOnFadeSeconds = 0.010;

    SynthEngine();
    ~SynthEngine();

    /** sampleRate > 0; maxChunk is a hint (process() accepts any n <= maxChunkSamples). NOT real-time. */
    void prepare (double sampleRate, int maxChunk);

    /** Silence at once, clock 0, smoothers jump to the next settings, LFOs / FX / allocator cleared. */
    void reset() noexcept;

    /** Note-off for every held note at the next process() position (envelopes release, FX tails ring). */
    void releaseAll() noexcept;

    /** Table of oscillator oscIndex (0 = A, 1 = B) from the next tick on; nullptr = the built-in sine
        (WavetableBuilder::makeSineTable, made in prepare()). Changing it crossfades (WavetableOscillator). */
    void setWavetable (int oscIndex, const Wavetable* table) noexcept;

    /** Every table pointer oscillator oscIndex of any voice may still read (current / previous of the
        crossfade, the union over the voices): the WavetableStore must keep them alive. Never the built-in
        sine. */
    TablesInUse getTablesInUse (int oscIndex) const noexcept;

    /** events: this chunk's events (offsets in [0, numSamples), sorted); timings: entry i belongs to
        events[i] (may be shorter or empty: no correction flags); expression: this chunk's frames, nullptr =
        restExpression (Test Pattern). left / right: numSamples samples, overwritten. */
    void process (std::span<const VsEvent> events, std::span<const VsEventTiming> timings,
                  const ExpressionBuffer* expression, const SynthSettings& settings, const SynthTransport& transport,
                  float* left, float* right, int numSamples) noexcept;

    SynthEngineStats getStats() const noexcept;

    /** Absolute samples processed since reset(). */
    int64_t getSamplePosition() const noexcept { return samplePosition; }

    /** Newest expression values the engine used (for the UI meters via ExpressionPublisher). */
    ExpressionFrame getCurrentExpression() const noexcept { return expressionValue; }

private:
    double sampleRate = 48000.0;
    int64_t samplePosition = 0;
    std::unique_ptr<Wavetable> fallbackSine;
    std::array<const Wavetable*, numOscillators> tables {};
    VoiceAllocator allocator;
    std::array<SynthVoice, maxVoices> voices;
    std::array<Lfo, numLfos> lfos;
    Chorus chorus;
    StereoDelay delay;
    FdnReverb reverb;
    SynthSettings smoothed {};
    bool hasSettings = false;
    float bendTarget = 0.0f;
    float bendSmoothed = 0.0f;
    ExpressionFrame expressionValue {};
    bool releasePending = false;
};

} // namespace vocalsynth::synth
