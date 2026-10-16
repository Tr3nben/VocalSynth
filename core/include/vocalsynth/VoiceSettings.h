#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace vocalsynth
{

// M2 pitch -> MIDI settings (docs/M2-SPEC.md, docs/M2-DESIGN.md section 3.1).
// Every enum value equals the index of the matching choice parameter in plugin/Parameters.h.
// Only ever append new entries at the end (saved projects store the index).

/** Voice range ("Input Type"): limits the f0 search and sets the analysis window length. */
enum class VoiceRange
{
    bass = 0,
    baritone,
    tenor,
    alto,
    soprano,
    whistle,
    wide
};

inline constexpr int numVoiceRanges = 7;

enum class PitchAlgorithm
{
    yin = 0,
    mpm
};

inline constexpr int numPitchAlgorithms = 2;

enum class Scale
{
    chromatic = 0,
    major,
    naturalMinor,
    harmonicMinor,
    majorPentatonic,
    minorPentatonic,
    blues,
    dorian,
    mixolydian
};

inline constexpr int numScales = 9;

enum class BendMode
{
    off = 0,
    anchored, ///< dead zone around the note centre + smoothing
    trueBend  ///< 1:1 residual ("True" in the UI; "true" is a keyword)
};

inline constexpr int numBendModes = 3;

/** Pitch-bend range of the receiving synth. Must match the synth's setting. */
enum class BendRange
{
    semitones2 = 0,
    semitones12,
    semitones24,
    semitones48
};

inline constexpr int numBendRanges = 4;

/** +-semitones covered by the full 14-bit bend for a BendRange. */
constexpr int bendRangeSemitones (BendRange range) noexcept
{
    switch (range)
    {
        case BendRange::semitones2:  return 2;
        case BendRange::semitones12: return 12;
        case BendRange::semitones24: return 24;
        case BendRange::semitones48: return 48;
    }

    return 2;
}

/** f0 search limits of one voice range (initial values from the spec, tune with tests). */
struct VoiceRangeInfo
{
    const char* name; ///< lower-case identifier, e.g. for vs-eval's --range option
    double f0MinHz;
    double f0MaxHz;
};

inline constexpr std::array<VoiceRangeInfo, numVoiceRanges> voiceRangeTable { {
    { "bass", 65.0, 330.0 },      // C2 .. E4
    { "baritone", 82.0, 392.0 },  // E2 .. G4
    { "tenor", 110.0, 523.0 },    // A2 .. C5 (default)
    { "alto", 147.0, 698.0 },     // D3 .. F5
    { "soprano", 196.0, 1047.0 }, // G3 .. C6
    { "whistle", 500.0, 2500.0 },
    { "wide", 65.0, 1047.0 },     // all voices: longest window, most octave risk
} };

/** Lowest f0 min of all ranges: every buffer is preallocated for it, so a range change never allocates. */
inline constexpr double lowestF0MinHz = 65.0;

constexpr const VoiceRangeInfo& voiceRangeInfo (VoiceRange range) noexcept
{
    const auto index = std::clamp (static_cast<int> (range), 0, numVoiceRanges - 1);
    return voiceRangeTable[static_cast<std::size_t> (index)];
}

/** All user settings the VoicePipeline needs, passed by value (const&) with EVERY process() call.
    Built by the processor from the APVTS raw atomics at the start of each block, so settings changes
    need no locks: the pipeline compares with the previous block's settings (docs/M2-DESIGN.md 3.10).
    Plain value type, trivially copyable. */
struct VoiceSettings
{
    VoiceRange range = VoiceRange::tenor;
    PitchAlgorithm algorithm = PitchAlgorithm::mpm; ///< default chosen by the vs-eval comparison (docs/M2-DESIGN.md 10)
    int key = 0;                             ///< 0..11 = C, C#, ..., B
    Scale scale = Scale::chromatic;
    float stickinessPercent = 50.0f;         ///< 0..100
    float gateThresholdDb = -40.0f;          ///< -70..-10 dBFS (RMS of the conditioned signal)
    float minNoteLengthMs = 60.0f;           ///< 20..250
    int octaveShift = 0;                     ///< -3..+3 octaves, applied to the output note
    BendMode bendMode = BendMode::off;
    BendRange bendRange = BendRange::semitones2;
    float velocitySensitivityPercent = 70.0f; ///< 0 = fixed velocity 100, 100 = full onset-RMS mapping
    bool legato = false;                     ///< false = retrigger (off before on), true = on before off
    int channel = 1;                         ///< MIDI channel 1..16

    bool operator== (const VoiceSettings&) const noexcept = default;
};

/** Settings ranges (same as the plugin parameters). */
inline constexpr float minStickinessPercent = 0.0f;
inline constexpr float maxStickinessPercent = 100.0f;
inline constexpr float minGateThresholdDb = -70.0f;
inline constexpr float maxGateThresholdDb = -10.0f;
inline constexpr float minMinNoteLengthMs = 20.0f;
inline constexpr float maxMinNoteLengthMs = 250.0f;
inline constexpr int minOctaveShift = -3;
inline constexpr int maxOctaveShift = 3;
inline constexpr float minVelocitySensitivityPercent = 0.0f;
inline constexpr float maxVelocitySensitivityPercent = 100.0f;

/** Returns a copy with every field clamped into its valid range (enums included, NaN -> default).
    The pipeline sanitises the settings itself; callers need not. */
constexpr VoiceSettings sanitise (VoiceSettings s) noexcept
{
    const auto clampEnum = [] (auto value, int count) noexcept
    {
        using Enum = decltype (value);
        return static_cast<Enum> (std::clamp (static_cast<int> (value), 0, count - 1));
    };

    const auto clampFloat = [] (float value, float lo, float hi, float fallback) noexcept
    {
        return value == value ? std::clamp (value, lo, hi) : fallback; // NaN != NaN
    };

    s.range = clampEnum (s.range, numVoiceRanges);
    s.algorithm = clampEnum (s.algorithm, numPitchAlgorithms);
    s.key = std::clamp (s.key, 0, 11);
    s.scale = clampEnum (s.scale, numScales);
    s.stickinessPercent = clampFloat (s.stickinessPercent, minStickinessPercent, maxStickinessPercent, 50.0f);
    s.gateThresholdDb = clampFloat (s.gateThresholdDb, minGateThresholdDb, maxGateThresholdDb, -40.0f);
    s.minNoteLengthMs = clampFloat (s.minNoteLengthMs, minMinNoteLengthMs, maxMinNoteLengthMs, 60.0f);
    s.octaveShift = std::clamp (s.octaveShift, minOctaveShift, maxOctaveShift);
    s.bendMode = clampEnum (s.bendMode, numBendModes);
    s.bendRange = clampEnum (s.bendRange, numBendRanges);
    s.velocitySensitivityPercent = clampFloat (s.velocitySensitivityPercent, minVelocitySensitivityPercent,
                                               maxVelocitySensitivityPercent, 70.0f);
    s.channel = std::clamp (s.channel, 1, 16);
    return s;
}

/** True if switching from a to b must flush the active note (note-off + bend centre at offset 0 of the
    block, then a new onset is required): range, algorithm, key, scale or MIDI channel changed.
    All other fields take effect at the next analysis frame without a flush (docs/M2-DESIGN.md 3.10). */
constexpr bool requiresFlush (const VoiceSettings& a, const VoiceSettings& b) noexcept
{
    return a.range != b.range || a.algorithm != b.algorithm || a.key != b.key || a.scale != b.scale
           || a.channel != b.channel;
}

} // namespace vocalsynth
