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
        block start:    engine.setWavetable (0, store.acquire (0)); ... (1)     // EVERY block, also bypassed
        each chunk:     engine.process (events, timings, expression, settings, transport, L, R, n); // n <= 4096
        block end:      store.release (engine.getTablesInUse (0), engine.getTablesInUse (1));  // EVERY block
        flush:          engine.releaseAll();      bypass: releaseAll + synthOn off for the 10 ms fade, then reset();
        releaseResources: engine.reset(); store.releaseAllRetired();           // audio stopped

    Table pointers (binding, docs/SYNTH-DESIGN.md 12.1): reset() and prepare() forget every table pointer
    (engine, voices, oscillators). A table set with setWavetable() is applied at the next tick; it is only
    read after that if the processor set it in the SAME block (every block starts with setWavetable), so
    getTablesInUse() needs to report only what the voices read (current / previous of a crossfade).
    The engine hands a new table to the voices only between crossfades: a table set while a crossfade runs
    waits for its end (<= 5 ms; newest wins), so all voices share one table timeline and at most two
    pointers per oscillator are ever read (also when notes start mid-crossfade).

    Timing (binding): the engine keeps its own absolute sample clock (0 at reset()). A tick at clock t is run
    by the process() call whose range [c, c + n) contains t, with that call's settings. Events are applied at
    clock + sampleOffset; all note events of one sample position form one VoiceAllocator group (bends at
    that position are applied before the group; CCs are ignored). Control ticks run at every clock value
    that is a multiple of controlTickSamples; at a sample that has both, the events come first. Settings
    are read per tick (the latest process() call's settings), continuous values smoothed with a 20 ms
    one-pole per tick, choices / ints / bools switched at the tick. Expression frames are latched at the
    first tick at or after their position (the newest frame of a tick wins) and ramp linearly to their
    values over the distance to the previous frame (16 .. 4096 samples). Pitch bend: bend / 8192 *
    bendRangeSemitones, one-pole 5 ms per tick. Result: BIT-IDENTICAL output for any split of the same
    input into process() calls (block sizes 1, 7, 64, 480, 512, 4096 at 44.1 / 48 / 96 kHz).

    synthOn: switching off fades the output over synthOnFadeSeconds, then (at a tick) clears voices,
    allocator, LFOs and FX like reset() but KEEPS the clock and the tables, and returns zeros at almost no
    cost; switching on starts from silence (a held note sounds from the next note-on). While fading out,
    note-ons are ignored and note-offs still release (synthOn may come back before the fade ends). Every state decision
    (fade end, voice inactive, FX tail end / bypass, NaN reset, denormal flush) is taken at a tick or an
    event position, never at a process() or render-segment boundary (docs/SYNTH-DESIGN.md 3.5).
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
    /** Unison voices per oscillator in Poly mode (docs/SYNTH-DESIGN.md 18 decision rule; the parameter range
        stays 1..8, Mono / Legato always get the full count). Decision at the end of wave 2: no cap - the
        worst case (Poly 16 x 2 osc x 8 unison, sub, noise, LP24 + drive, all FX, 48 kHz / 128) measured
        8.2 % (< 12 %). Must not change once the golden fingerprints exist. */
    static constexpr int polyMaxUnison = maxUnisonVoices;

    SynthEngine();
    ~SynthEngine();

    /** sampleRate > 0; maxChunk is a hint (process() accepts any n <= maxChunkSamples). NOT real-time. */
    void prepare (double sampleRate, int maxChunk);

    /** Silence at once, clock 0, smoothers jump to the next settings, LFOs / FX / allocator cleared, every
        table pointer forgotten (the built-in sine plays until the next setWavetable()). */
    void reset() noexcept;

    /** Note-off for every held note at the next process() position (envelopes release, FX tails ring); the pitch
        bend goes back to the centre at once (smoothed 5 ms), like the MIDI port's flush. */
    void releaseAll() noexcept;

    /** Table of oscillator oscIndex (0 = A, 1 = B) from the next tick on, for every voice (silent voices
        switch at once, sounding ones crossfade, WavetableOscillator); nullptr = the built-in sine
        (WavetableBuilder::makeSineTable, made in prepare()). The first table after reset() is taken without
        a crossfade. */
    void setWavetable (int oscIndex, const Wavetable* table) noexcept;

    /** Every table pointer oscillator oscIndex of any voice may still read (current / previous of the
        crossfade, the union over ALL voices, also silent ones; at most two distinct pointers because every
        voice gets the same setTable() at the same tick): the WavetableStore must keep them alive. Never the
        built-in sine. */
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
    enum class RunState : uint8_t
    {
        off,      ///< synthOn off and faded: everything cleared, nothing rendered (the clock runs)
        running,
        fadingOut ///< synthOn switched off: output fades over synthOnFadeSeconds, then off
    };

    struct PendingFrame
    {
        ExpressionFrame frame {};
        int64_t position = 0;
        int distance = controlTickSamples;
        bool valid = false;
    };

    void controlTick (int64_t tickPosition) noexcept;
    void smoothSettings() noexcept;
    void updateDerivedSettings() noexcept;
    void activate() noexcept;
    void clearSounding() noexcept;
    void handleGroup (int64_t position) noexcept;
    void applyCommands (int64_t position) noexcept;
    void forwardTables() noexcept;
    void updateExpression (int64_t tickPosition) noexcept;
    void consumeFrame (int index, PendingFrame& out) noexcept;
    void renderSegment (float* left, float* right, int numSamples) noexcept;
    float bendSemitones (int raw) const noexcept;

    double sampleRate = 48000.0;
    int64_t samplePosition = 0;
    std::unique_ptr<Wavetable> fallbackSine;
    std::array<const Wavetable*, numOscillators> tables {};  ///< newest setWavetable() (nullptr = the sine)
    std::array<const Wavetable*, numOscillators> applied {}; ///< what the voices were given (never nullptr after prepare)
    VoiceAllocator allocator;
    std::array<SynthVoice, maxVoices> voices;
    std::array<Lfo, numLfos> lfos;
    Chorus chorus;
    StereoDelay delay;
    FdnReverb reverb;

    // Settings: target = sanitised settings of the current process() call; smoothed = per tick (20 ms);
    // voiceSettings = smoothed with the engine's own limits (Poly unison cap) - what the voices read.
    SynthSettings target {};
    SynthSettings smoothed {};
    SynthSettings voiceSettings {};
    SynthTransport transport {};
    AllocatorSettings allocatorSettings {};
    bool hasSettings = false;
    float smoothingCoefficient = 1.0f;
    RunState state = RunState::off;

    // Output: synthVolume and the synthOn fade, ramped per sample over the tick
    float cachedVolumeDb = 1000.0f, volumeLinear = 0.0f;
    float volumeGain = 0.0f, volumeTarget = 0.0f, volumeStep = 0.0f;
    float onGain = 0.0f, onTarget = 0.0f, onStep = 0.0f;
    float onFadePerTick = 1.0f;
    int outputRampLeft = 0;

    // Pitch bend (raw value of the newest bend event, smoothed in semitones per tick)
    int bendRaw = 0;
    float bendSmoothed = 0.0f;
    float bendCoefficient = 1.0f;

    // Expression: frames of the current call (ticks latch them in order), the newest frame carried over
    const ExpressionBuffer* frames = nullptr;
    int frameIndex = 0;
    int64_t callStart = 0;
    int callSamples = 0;
    PendingFrame carried {};
    int64_t lastFramePosition = 0;
    bool hasLastFrame = false;
    ExpressionFrame expressionValue {};
    ExpressionFrame expressionTarget {};
    ExpressionFrame expressionStep {};
    int expressionRemaining = 0;
    float restAttackCoefficient = 1.0f, restReleaseCoefficient = 1.0f;

    // Modulation
    ModSourceValues globalSources {};
    std::array<float, numLfos> lfoRateOctaves {}; ///< LFO n Rate modulation of the previous tick
    float chorusMixMod = 0.0f, delayMixMod = 0.0f, reverbMixMod = 0.0f;

    // Events
    std::array<NoteInput, VoiceCommandList::capacity> group {};
    int groupSize = 0;
    VoiceCommandList commands;
    bool releasePending = false;
};

} // namespace vocalsynth::synth
