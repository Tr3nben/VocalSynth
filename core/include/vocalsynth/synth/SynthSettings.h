#pragma once

#include <array>
#include <cstdint>

namespace vocalsynth::synth
{

// Synth settings of release 0.4.0 (docs/SYNTH-DESIGN.md 4 and 15). Plain values in the SAME units as the
// parameters of plugin/SynthParameters.h ("Klarwerte": percent as 0..100, ms, dB, Hz, cents), so the
// SynthSettingsBuilder is a straight copy and tests / golden renders can write the numbers of a preset.
// Every enum value equals the index of the matching choice parameter. Only ever APPEND enum entries.
// The member defaults ARE the parameter defaults (checked by static_asserts in SynthParameters.h).

enum class PlayMode
{
    mono = 0, ///< one voice, every note-on restarts the envelopes (from the current level)
    legato,   ///< one voice, connected note changes keep the envelopes running (default)
    poly      ///< up to VoiceAllocator::maxVoices voices
};
inline constexpr int numPlayModes = 3;

enum class GlideMode
{
    always = 0,
    legatoOnly ///< glide only on connected note changes (default)
};
inline constexpr int numGlideModes = 2;

enum class SubOctave
{
    down1 = 0,
    down2
};
inline constexpr int numSubOctaves = 2;

enum class SubShape
{
    sine = 0,
    triangle,
    square
};
inline constexpr int numSubShapes = 3;

enum class FilterMode
{
    lp12 = 0,
    lp24,
    bp,
    hp,
    notch
};
inline constexpr int numFilterModes = 5;

enum class LfoShape
{
    sine = 0,
    triangle,
    sawUp,
    sawDown,
    square,
    sampleHold,
    smoothRandom
};
inline constexpr int numLfoShapes = 7;

/** Tempo-synced period of an LFO cycle or a delay time. off = free (Hz / ms). */
enum class TempoDivision
{
    off = 0,
    d1_32,
    d1_16T,
    d1_16,
    d1_16D,
    d1_8T,
    d1_8,
    d1_8D,
    d1_4T,
    d1_4,
    d1_4D,
    d1_2,
    bar1,
    bars2,
    bars4
};
inline constexpr int numTempoDivisions = 15;

/** Length of a division in quarter notes (a bar is always 4 quarter notes in 0.4), 0 for off. */
constexpr double tempoDivisionQuarterNotes (TempoDivision d) noexcept
{
    switch (d)
    {
        case TempoDivision::off:    return 0.0;
        case TempoDivision::d1_32:  return 0.125;
        case TempoDivision::d1_16T: return 1.0 / 6.0;
        case TempoDivision::d1_16:  return 0.25;
        case TempoDivision::d1_16D: return 0.375;
        case TempoDivision::d1_8T:  return 1.0 / 3.0;
        case TempoDivision::d1_8:   return 0.5;
        case TempoDivision::d1_8D:  return 0.75;
        case TempoDivision::d1_4T:  return 2.0 / 3.0;
        case TempoDivision::d1_4:   return 1.0;
        case TempoDivision::d1_4D:  return 1.5;
        case TempoDivision::d1_2:   return 2.0;
        case TempoDivision::bar1:   return 4.0;
        case TempoDivision::bars2:  return 8.0;
        case TempoDivision::bars4:  return 16.0;
    }

    return 0.0;
}

/** Mod-matrix sources (docs/SYNTH-DESIGN.md 9). Value ranges: env 0..1, LFO -1..1, velocity 0..1,
    note (key - 60) / 60 (-1..1 over the MIDI range, clamped), random per note -1..1, voice level /
    brightness / noisiness 0..1, voice pitch -1..1 (= -100..+100 cents). 0.5 appends vowelX, vowelY. */
enum class ModSource
{
    off = 0,
    env2,
    env3,
    lfo1,
    lfo2,
    velocity,
    note,
    randomPerNote,
    voiceLevel,
    voiceBrightness,
    voiceNoisiness,
    voicePitch
};
inline constexpr int numModSources = 12;

/** Mod-matrix destinations (docs/SYNTH-DESIGN.md 9, scaling per destination there and in modDestScale()). */
enum class ModDest
{
    off = 0,
    oscAPosition,
    oscAPitch,
    oscALevel,
    oscADetune,
    oscBPosition,
    oscBPitch,
    oscBLevel,
    oscBDetune,
    subLevel,
    noiseLevel,
    filterCutoff,
    filterResonance,
    filterDrive,
    ampLevel,
    pan,
    lfo1Rate,
    lfo2Rate,
    chorusMix,
    delayMix,
    reverbMix
};
inline constexpr int numModDests = 21;

/** The control tick (docs/SYNTH-DESIGN.md 3): modulation, envelopes, LFOs, filter coefficients and mip
    levels are evaluated every controlTickSamples samples at ABSOLUTE positions (multiples of it since the
    engine's reset()), with per-sample linear ramps in between. Never depends on the host block size. */
inline constexpr int controlTickSamples = 16;

inline constexpr int numOscillators = 2;
inline constexpr int numEnvelopes = 3;
inline constexpr int numLfos = 2;
inline constexpr int numModSlots = 8;
inline constexpr int maxUnisonVoices = 8;

/** Closed range of a continuous or integer setting. */
struct Range
{
    float min = 0.0f;
    float max = 1.0f;
};

/** Ranges of every synth setting (plain units). The parameter ranges in plugin/SynthParameters.h are
    built from these, sanitised() clamps to them. Never change a range after 0.4.0 (saved projects). */
namespace limits
{
inline constexpr Range synthVolumeDb { -60.0f, 6.0f };   ///< <= min: silent
inline constexpr Range dryLevelDb { -60.0f, 0.0f };      ///< <= min: silent
inline constexpr Range glideTimeMs { 0.0f, 2000.0f };
inline constexpr Range percent { 0.0f, 100.0f };
inline constexpr Range bipolarPercent { -100.0f, 100.0f };
inline constexpr Range oscOctave { -3.0f, 3.0f };
inline constexpr Range oscSemitones { -12.0f, 12.0f };
inline constexpr Range oscFineCents { -100.0f, 100.0f };
inline constexpr Range oscUnison { 1.0f, 8.0f };
inline constexpr Range oscDetuneCents { 0.0f, 100.0f };
inline constexpr Range filterCutoffHz { 20.0f, 20000.0f };
inline constexpr Range filterDriveDb { 0.0f, 24.0f };
inline constexpr Range envelopeTimeMs { 0.0f, 10000.0f };
inline constexpr Range lfoRateHz { 0.01f, 30.0f };
inline constexpr Range lfoFadeMs { 0.0f, 10000.0f };
inline constexpr Range chorusRateHz { 0.05f, 5.0f };
inline constexpr Range delayTimeMs { 1.0f, 2000.0f };
inline constexpr Range delayFeedbackPercent { 0.0f, 95.0f };
} // namespace limits

struct OscSettings
{
    bool on = true;
    float positionPercent = 50.0f; ///< 0..100 % of the table (frame position, interpolated)
    int octave = 0;                ///< -3..3
    int semitones = 0;             ///< -12..12
    float fineCents = 0.0f;        ///< -100..100
    float levelPercent = 70.0f;    ///< linear amplitude 0..100 %
    float panPercent = 0.0f;       ///< -100 (left) .. 100 (right)
    int unison = 1;                ///< 1..8
    float detuneCents = 20.0f;     ///< total spread of the unison voices, symmetric around the centre
    float spreadPercent = 50.0f;   ///< stereo width of the unison voices
    float phaseRandPercent = 0.0f; ///< random start phase per unison voice (0 = all start at phase 0)

    constexpr bool operator== (const OscSettings&) const noexcept = default;
};

struct EnvelopeSettings
{
    float attackMs = 5.0f;
    float decayMs = 300.0f;
    float sustainPercent = 100.0f;
    float releaseMs = 200.0f;

    constexpr bool operator== (const EnvelopeSettings&) const noexcept = default;
};

struct LfoSettings
{
    LfoShape shape = LfoShape::sine;
    float rateHz = 5.0f;                    ///< used while sync == off
    TempoDivision sync = TempoDivision::off;
    bool retrigger = false;                 ///< phase 0 on every envelope-starting note-on
    float fadeMs = 0.0f;                    ///< linear fade-in after every envelope-starting note-on

    constexpr bool operator== (const LfoSettings&) const noexcept = default;
};

struct ModSlotSettings
{
    ModSource source = ModSource::off;
    ModDest dest = ModDest::off;
    float amountPercent = 0.0f; ///< -100..100

    constexpr bool operator== (const ModSlotSettings&) const noexcept = default;
};

struct ChorusSettings
{
    bool on = false;
    float rateHz = 0.8f;
    float depthPercent = 50.0f;
    float mixPercent = 50.0f;

    constexpr bool operator== (const ChorusSettings&) const noexcept = default;
};

struct DelaySettings
{
    bool on = false;
    float timeMs = 375.0f;                   ///< used while sync == off
    TempoDivision sync = TempoDivision::off; ///< synced time is clamped to limits::delayTimeMs
    float feedbackPercent = 35.0f;
    bool pingPong = false;
    float mixPercent = 25.0f;

    constexpr bool operator== (const DelaySettings&) const noexcept = default;
};

struct ReverbSettings
{
    bool on = false;
    float sizePercent = 50.0f;
    float dampingPercent = 50.0f;
    float mixPercent = 25.0f;

    constexpr bool operator== (const ReverbSettings&) const noexcept = default;
};

/** Every synth parameter as one plain value, built per block from the APVTS raw atomics
    (SynthSettingsBuilder) and passed by const reference to SynthEngine::process(). */
struct SynthSettings
{
    // Global / mix (synthOn's parameter default is off, see docs/SYNTH-DESIGN.md 13.3)
    bool synthOn = false;
    float synthVolumeDb = -6.0f;
    float dryLevelDb = 0.0f; ///< used by the processor (FX), not by the engine

    // Play mode
    PlayMode playMode = PlayMode::legato;
    float glideTimeMs = 0.0f;
    GlideMode glideMode = GlideMode::legatoOnly;
    float ampVelocityPercent = 50.0f;

    std::array<OscSettings, numOscillators> osc { OscSettings {}, OscSettings { .on = false } };

    // Sub / noise
    float subLevelPercent = 0.0f;
    SubOctave subOctave = SubOctave::down1;
    SubShape subShape = SubShape::sine;
    float noiseLevelPercent = 0.0f;
    float noiseColorPercent = 0.0f; ///< 0 = white, 100 = pink

    // Filter
    bool filterOn = true;
    FilterMode filterMode = FilterMode::lp24;
    float filterCutoffHz = 20000.0f;
    float filterResonancePercent = 0.0f;
    float filterDriveDb = 0.0f;
    float filterKeytrackPercent = 0.0f;   ///< 100 % = cutoff follows the note 1:1 (centre note 60)
    float filterEnvAmountPercent = 0.0f;  ///< Env 2 -> cutoff, 100 % = +5 octaves at env 1.0

    /** env[0] = Env 1 (amp), env[1] = Env 2 (filter, also a mod source), env[2] = Env 3 (mod). */
    std::array<EnvelopeSettings, numEnvelopes> env { EnvelopeSettings {},
                                                     EnvelopeSettings { .sustainPercent = 0.0f },
                                                     EnvelopeSettings { .sustainPercent = 0.0f } };

    std::array<LfoSettings, numLfos> lfo { LfoSettings {}, LfoSettings { .rateHz = 0.5f } };

    std::array<ModSlotSettings, numModSlots> mod {};

    ChorusSettings chorus;
    DelaySettings delay;
    ReverbSettings reverb;

    // Not synth parameters: filled in by the processor
    int bendRangeSemitones = 2; ///< from the voice setting bendRange (2, 12, 24, 48)

    /** Every value clamped to its range (limits), NaN / Inf replaced by the default, enums forced into
        their valid range, bendRangeSemitones clamped to 0..48. Real-time safe. */
    SynthSettings sanitised() const noexcept;

    constexpr bool operator== (const SynthSettings&) const noexcept = default;
};

} // namespace vocalsynth::synth
