#pragma once

#include "vocalsynth/synth/SynthSettings.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <cmath>
#include <cstddef>

namespace vocalsynth::params
{

// 0.4.0 synth parameters (docs/SYNTH-DESIGN.md 15). IDs are PERMANENT, appended after the M3.1 parameters by
// addSynthParameters() (called at the end of createLayout() in Parameters.h). Version hint 4 for all of them
// (AUv2 index order, see Parameters.h). Ranges come from vocalsynth::synth::limits, defaults from
// vocalsynth::synth::SynthSettings {} - one source of truth with the core. Choice lists are FROZEN in length
// from 0.4.0 (host automation stores index / (n - 1)); later additions need a new parameter (docs/SYNTH-DESIGN.md
// 15, 24.4). Entries planned for later releases are reserved in the lists already (mod sources Vowel X / Y, Vowel
// Weight 1..3).
// Value texts carry no unit (the label does), like the M2 parameters.
// Written by the design agent; frozen for 0.4 (changes only through the design document).
inline constexpr int synthParameterVersionHint = 4;

/** The value a NEW instance gets for synthOn (decisions E4 / E5: on in FX and Instrument) = the parameter's
    default (integration 0.4: a default off with a different initial value fails clap-validator's
    param-default-values; docs/SYNTH-DESIGN.md 13.3, 24). The core SynthSettings {} keeps synthOn = false (engine
    default). A state without synthOn (0.3.x and older) loads with the synth OFF through the processor's
    migrateState() rule, not through the default. */
inline constexpr bool synthOnForNewInstances = true;

namespace synthdetail
{
inline juce::ParameterID id (const char* name)
{
    return { name, synthParameterVersionHint };
}
} // namespace synthdetail

// --- Global / mix ---
inline const juce::ParameterID synthOn = synthdetail::id ("synthOn");
inline const juce::ParameterID synthVolume = synthdetail::id ("synthVolume");
inline const juce::ParameterID dryLevel = synthdetail::id ("dryLevel");

// --- Play mode ---
inline const juce::ParameterID playMode = synthdetail::id ("playMode");
inline const juce::ParameterID glideTime = synthdetail::id ("glideTime");
inline const juce::ParameterID glideMode = synthdetail::id ("glideMode");
inline const juce::ParameterID ampVelocity = synthdetail::id ("ampVelocity");
inline const juce::ParameterID ampGain = synthdetail::id ("ampGain");

// --- Oscillators A / B ---
struct OscParameterIds
{
    juce::ParameterID on, position, octave, semi, fine, level, pan, unison, detune, spread, phaseRand;
};

inline const std::array<OscParameterIds, synth::numOscillators> oscParameters {
    OscParameterIds { synthdetail::id ("oscAOn"), synthdetail::id ("oscAPosition"), synthdetail::id ("oscAOctave"),
                      synthdetail::id ("oscASemi"), synthdetail::id ("oscAFine"), synthdetail::id ("oscALevel"),
                      synthdetail::id ("oscAPan"), synthdetail::id ("oscAUnison"), synthdetail::id ("oscADetune"),
                      synthdetail::id ("oscASpread"), synthdetail::id ("oscAPhaseRand") },
    OscParameterIds { synthdetail::id ("oscBOn"), synthdetail::id ("oscBPosition"), synthdetail::id ("oscBOctave"),
                      synthdetail::id ("oscBSemi"), synthdetail::id ("oscBFine"), synthdetail::id ("oscBLevel"),
                      synthdetail::id ("oscBPan"), synthdetail::id ("oscBUnison"), synthdetail::id ("oscBDetune"),
                      synthdetail::id ("oscBSpread"), synthdetail::id ("oscBPhaseRand") }
};

// --- Sub / noise ---
inline const juce::ParameterID subLevel = synthdetail::id ("subLevel");
inline const juce::ParameterID subOctave = synthdetail::id ("subOctave");
inline const juce::ParameterID subShape = synthdetail::id ("subShape");
inline const juce::ParameterID noiseLevel = synthdetail::id ("noiseLevel");
inline const juce::ParameterID noiseColor = synthdetail::id ("noiseColor");

// --- Filter ---
inline const juce::ParameterID filterOn = synthdetail::id ("filterOn");
inline const juce::ParameterID filterMode = synthdetail::id ("filterMode");
inline const juce::ParameterID filterCutoff = synthdetail::id ("filterCutoff");
inline const juce::ParameterID filterResonance = synthdetail::id ("filterResonance");
inline const juce::ParameterID filterDrive = synthdetail::id ("filterDrive");
inline const juce::ParameterID filterKeytrack = synthdetail::id ("filterKeytrack");
inline const juce::ParameterID filterEnvAmount = synthdetail::id ("filterEnvAmount");

// --- Envelopes 1..3 (Env 1 = amp, Env 2 = filter, Env 3 = mod) ---
struct EnvelopeParameterIds
{
    juce::ParameterID attack, decay, sustain, release;
};

inline const std::array<EnvelopeParameterIds, synth::numEnvelopes> envelopeParameters {
    EnvelopeParameterIds { synthdetail::id ("env1Attack"), synthdetail::id ("env1Decay"),
                           synthdetail::id ("env1Sustain"), synthdetail::id ("env1Release") },
    EnvelopeParameterIds { synthdetail::id ("env2Attack"), synthdetail::id ("env2Decay"),
                           synthdetail::id ("env2Sustain"), synthdetail::id ("env2Release") },
    EnvelopeParameterIds { synthdetail::id ("env3Attack"), synthdetail::id ("env3Decay"),
                           synthdetail::id ("env3Sustain"), synthdetail::id ("env3Release") }
};

// --- LFOs 1..2 ---
struct LfoParameterIds
{
    juce::ParameterID shape, rate, sync, retrig, fade;
};

inline const std::array<LfoParameterIds, synth::numLfos> lfoParameters {
    LfoParameterIds { synthdetail::id ("lfo1Shape"), synthdetail::id ("lfo1Rate"), synthdetail::id ("lfo1Sync"),
                      synthdetail::id ("lfo1Retrig"), synthdetail::id ("lfo1Fade") },
    LfoParameterIds { synthdetail::id ("lfo2Shape"), synthdetail::id ("lfo2Rate"), synthdetail::id ("lfo2Sync"),
                      synthdetail::id ("lfo2Retrig"), synthdetail::id ("lfo2Fade") }
};

// --- Mod matrix slots 1..8 ---
struct ModSlotParameterIds
{
    juce::ParameterID source, dest, amount;
};

inline const std::array<ModSlotParameterIds, synth::numModSlots> modParameters {
    ModSlotParameterIds { synthdetail::id ("mod1Source"), synthdetail::id ("mod1Dest"), synthdetail::id ("mod1Amount") },
    ModSlotParameterIds { synthdetail::id ("mod2Source"), synthdetail::id ("mod2Dest"), synthdetail::id ("mod2Amount") },
    ModSlotParameterIds { synthdetail::id ("mod3Source"), synthdetail::id ("mod3Dest"), synthdetail::id ("mod3Amount") },
    ModSlotParameterIds { synthdetail::id ("mod4Source"), synthdetail::id ("mod4Dest"), synthdetail::id ("mod4Amount") },
    ModSlotParameterIds { synthdetail::id ("mod5Source"), synthdetail::id ("mod5Dest"), synthdetail::id ("mod5Amount") },
    ModSlotParameterIds { synthdetail::id ("mod6Source"), synthdetail::id ("mod6Dest"), synthdetail::id ("mod6Amount") },
    ModSlotParameterIds { synthdetail::id ("mod7Source"), synthdetail::id ("mod7Dest"), synthdetail::id ("mod7Amount") },
    ModSlotParameterIds { synthdetail::id ("mod8Source"), synthdetail::id ("mod8Dest"), synthdetail::id ("mod8Amount") }
};

// --- FX ---
inline const juce::ParameterID chorusOn = synthdetail::id ("chorusOn");
inline const juce::ParameterID chorusRate = synthdetail::id ("chorusRate");
inline const juce::ParameterID chorusDepth = synthdetail::id ("chorusDepth");
inline const juce::ParameterID chorusMix = synthdetail::id ("chorusMix");
inline const juce::ParameterID delayOn = synthdetail::id ("delayOn");
inline const juce::ParameterID delayTime = synthdetail::id ("delayTime");
inline const juce::ParameterID delaySync = synthdetail::id ("delaySync");
inline const juce::ParameterID delayFeedback = synthdetail::id ("delayFeedback");
inline const juce::ParameterID delayPingPong = synthdetail::id ("delayPingPong");
inline const juce::ParameterID delayMix = synthdetail::id ("delayMix");
inline const juce::ParameterID reverbOn = synthdetail::id ("reverbOn");
inline const juce::ParameterID reverbSize = synthdetail::id ("reverbSize");
inline const juce::ParameterID reverbDamping = synthdetail::id ("reverbDamping");
inline const juce::ParameterID reverbMix = synthdetail::id ("reverbMix");

/** Number of synth parameters (docs/SYNTH-DESIGN.md 15): 3 mix + 5 play / amp + 2 x 11 osc + 5 sub/noise
    + 7 filter + 3 x 4 env + 2 x 5 LFO + 8 x 3 mod + 14 FX. */
inline constexpr int numSynthParameters = 3 + 5 + 2 * 11 + 5 + 7 + 3 * 4 + 2 * 5 + 8 * 3 + 14;
static_assert (numSynthParameters == 102);

// Choice names. Array sizes are the core enum counts: index == static_cast<int> (core enum). Frozen lengths.
inline constexpr std::array<const char*, synth::numPlayModes> playModeNames { "Mono", "Legato", "Poly" };
inline constexpr std::array<const char*, synth::numGlideModes> glideModeNames { "Always", "Legato Only" };
inline constexpr std::array<const char*, synth::numSubOctaves> subOctaveNames { "-1 Oct", "-2 Oct" };
inline constexpr std::array<const char*, synth::numSubShapes> subShapeNames { "Sine", "Triangle", "Square" };
inline constexpr std::array<const char*, synth::numFilterModes> filterModeNames { "LP12", "LP24", "BP", "HP",
                                                                                  "Notch" };
inline constexpr std::array<const char*, synth::numLfoShapes> lfoShapeNames { "Sine",   "Triangle", "Saw Up",
                                                                              "Saw Down", "Square", "S&H",
                                                                              "Smooth Random" };
inline constexpr std::array<const char*, synth::numTempoDivisions> tempoDivisionNames {
    "Off", "1/32", "1/16T", "1/16", "1/16D", "1/8T", "1/8", "1/8D", "1/4T", "1/4", "1/4D", "1/2", "1 Bar", "2 Bars",
    "4 Bars"
};
inline constexpr std::array<const char*, synth::numModSources> modSourceNames {
    "Off",    "Env 2",  "Env 3",      "LFO 1",           "LFO 2",           "Velocity",
    "Note",   "Random", "Voice Level", "Voice Brightness", "Voice Noisiness", "Voice Pitch",
    "Vowel X", "Vowel Y", "Vowel Weight 1", "Vowel Weight 2", "Vowel Weight 3"
};

/** The release that brings a reserved mod source (synth::isReservedModSource), for the UI ("Vowel X (0.5)");
    empty for the sources of this release. */
inline const char* modSourceReleaseHint (int index) noexcept
{
    switch (index)
    {
        case static_cast<int> (synth::ModSource::vowelX):
        case static_cast<int> (synth::ModSource::vowelY):       return "0.5";
        case static_cast<int> (synth::ModSource::vowelWeight1):
        case static_cast<int> (synth::ModSource::vowelWeight2):
        case static_cast<int> (synth::ModSource::vowelWeight3): return "0.6";
        default:                                                return "";
    }
}
inline constexpr std::array<const char*, synth::numModDests> modDestNames {
    "Off",          "Osc A Position", "Osc A Pitch",      "Osc A Level", "Osc A Detune", "Osc B Position",
    "Osc B Pitch",  "Osc B Level",    "Osc B Detune",     "Sub Level",   "Noise Level",  "Filter Cutoff",
    "Filter Resonance", "Filter Drive", "Amp Level",      "Pan",         "LFO 1 Rate",   "LFO 2 Rate",
    "Chorus Mix",   "Delay Mix",      "Reverb Mix",       "Pitch"
};

namespace synthdetail
{
template <std::size_t N>
constexpr bool allNamed (const std::array<const char*, N>& names) noexcept
{
    for (const auto* name : names)
        if (name == nullptr)
            return false;

    return true;
}

template <std::size_t N>
juce::StringArray toStringArray (const std::array<const char*, N>& names)
{
    juce::StringArray result;

    for (const auto* name : names)
        result.add (juce::String (juce::CharPointer_UTF8 (name)));

    return result;
}
} // namespace synthdetail

static_assert (synthdetail::allNamed (playModeNames) && synthdetail::allNamed (glideModeNames)
               && synthdetail::allNamed (subOctaveNames) && synthdetail::allNamed (subShapeNames)
               && synthdetail::allNamed (filterModeNames) && synthdetail::allNamed (lfoShapeNames)
               && synthdetail::allNamed (tempoDivisionNames) && synthdetail::allNamed (modSourceNames)
               && synthdetail::allNamed (modDestNames));

inline const juce::StringArray playModeChoices = synthdetail::toStringArray (playModeNames);
inline const juce::StringArray glideModeChoices = synthdetail::toStringArray (glideModeNames);
inline const juce::StringArray subOctaveChoices = synthdetail::toStringArray (subOctaveNames);
inline const juce::StringArray subShapeChoices = synthdetail::toStringArray (subShapeNames);
inline const juce::StringArray filterModeChoices = synthdetail::toStringArray (filterModeNames);
inline const juce::StringArray lfoShapeChoices = synthdetail::toStringArray (lfoShapeNames);
inline const juce::StringArray tempoDivisionChoices = synthdetail::toStringArray (tempoDivisionNames);
inline const juce::StringArray modSourceChoices = synthdetail::toStringArray (modSourceNames);
inline const juce::StringArray modDestChoices = synthdetail::toStringArray (modDestNames);

/** The parameter defaults: the core's SynthSettings defaults (docs/SYNTH-DESIGN.md 15). */
inline constexpr synth::SynthSettings synthDefaults {};

// Skew centres of the non-linear knobs (value at the knob's middle). Cutoff, LFO / chorus rate and delay
// time are exactly logarithmic (logRange).
inline constexpr float glideTimeCentreMs = 300.0f;
inline constexpr float envelopeTimeCentreMs = 500.0f;
inline constexpr float lfoFadeCentreMs = 1000.0f;

namespace synthdetail
{
/** x in 0..1 <-> min * (max / min)^x (min > 0). */
inline juce::NormalisableRange<float> logRange (synth::Range r)
{
    return juce::NormalisableRange<float> (
        r.min, r.max, [] (float start, float end, float x) { return start * std::pow (end / start, x); },
        [] (float start, float end, float v) { return std::log (v / start) / std::log (end / start); },
        [] (float start, float end, float v) { return juce::jlimit (start, end, v); });
}

inline juce::NormalisableRange<float> linearRange (synth::Range r)
{
    return juce::NormalisableRange<float> (r.min, r.max);
}

inline juce::NormalisableRange<float> skewedRange (synth::Range r, float centre)
{
    juce::NormalisableRange<float> range (r.min, r.max);
    range.setSkewForCentre (centre);
    return range;
}

/** Text without unit: integers for %, ms >= 100, Hz >= 100, cents; one decimal below (and for dB);
    two decimals for rates below 10 Hz. */
inline juce::String integerText (float v, int)
{
    return juce::String (juce::roundToInt (v));
}

inline juce::String oneDecimalText (float v, int)
{
    return juce::String (v, 1);
}

inline juce::String timeText (float v, int)
{
    return std::abs (v) < 100.0f ? juce::String (v, 1) : juce::String (juce::roundToInt (v));
}

inline juce::String frequencyText (float v, int)
{
    if (std::abs (v) < 10.0f)
        return juce::String (v, 2);

    return std::abs (v) < 100.0f ? juce::String (v, 1) : juce::String (juce::roundToInt (v));
}

inline void addFloat (juce::AudioProcessorValueTreeState::ParameterLayout& layout, const juce::ParameterID& id,
                      const juce::String& name, juce::NormalisableRange<float> range, float defaultValue,
                      const juce::String& label, juce::String (*toText) (float, int))
{
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        id, name, range, defaultValue,
        juce::AudioParameterFloatAttributes().withLabel (label).withStringFromValueFunction (toText)));
}

inline void addInt (juce::AudioProcessorValueTreeState::ParameterLayout& layout, const juce::ParameterID& id,
                    const juce::String& name, synth::Range range, int defaultValue, const juce::String& label = {})
{
    layout.add (std::make_unique<juce::AudioParameterInt> (id, name, juce::roundToInt (range.min),
                                                           juce::roundToInt (range.max), defaultValue,
                                                           juce::AudioParameterIntAttributes().withLabel (label)));
}

inline void addBool (juce::AudioProcessorValueTreeState::ParameterLayout& layout, const juce::ParameterID& id,
                     const juce::String& name, bool defaultValue)
{
    layout.add (std::make_unique<juce::AudioParameterBool> (id, name, defaultValue));
}

inline void addChoice (juce::AudioProcessorValueTreeState::ParameterLayout& layout, const juce::ParameterID& id,
                       const juce::String& name, const juce::StringArray& choices, int defaultIndex)
{
    layout.add (std::make_unique<juce::AudioParameterChoice> (id, name, choices, defaultIndex));
}

template <typename Enum>
constexpr int index (Enum e) noexcept
{
    return static_cast<int> (e);
}
} // namespace synthdetail

/** Appends all numSynthParameters synth parameters in the order of docs/SYNTH-DESIGN.md 15 (= the order of
    this file). Called once, at the END of createLayout(). */
inline void addSynthParameters (juce::AudioProcessorValueTreeState::ParameterLayout& layout)
{
    using namespace synthdetail;
    namespace lim = synth::limits;
    const auto& d = synthDefaults;

    // Global / mix
    addBool (layout, synthOn, "Synth On", synthOnForNewInstances); // not d.synthOn (see synthOnForNewInstances)
    addFloat (layout, synthVolume, "Synth Volume", linearRange (lim::synthVolumeDb), d.synthVolumeDb, "dB",
              oneDecimalText);
    addFloat (layout, dryLevel, "Dry Level", linearRange (lim::dryLevelDb), d.dryLevelDb, "dB", oneDecimalText);

    // Play mode
    addChoice (layout, playMode, "Play Mode", playModeChoices, index (d.playMode));
    addFloat (layout, glideTime, "Glide Time", skewedRange (lim::glideTimeMs, glideTimeCentreMs), d.glideTimeMs, "ms",
              timeText);
    addChoice (layout, glideMode, "Glide Mode", glideModeChoices, index (d.glideMode));
    addFloat (layout, ampVelocity, "Amp Velocity", linearRange (lim::percent), d.ampVelocityPercent, "%", integerText);
    addFloat (layout, ampGain, "Amp Gain", linearRange (lim::ampGainDb), d.ampGainDb, "dB", oneDecimalText);

    // Oscillators
    for (std::size_t i = 0; i < oscParameters.size(); ++i)
    {
        const auto& ids = oscParameters[i];
        const auto& o = d.osc[i];
        const juce::String prefix = i == 0 ? "Osc A " : "Osc B ";

        addBool (layout, ids.on, prefix + "On", o.on);
        addFloat (layout, ids.position, prefix + "Position", linearRange (lim::percent), o.positionPercent, "%",
                  integerText);
        addInt (layout, ids.octave, prefix + "Octave", lim::oscOctave, o.octave, "oct");
        addInt (layout, ids.semi, prefix + "Semi", lim::oscSemitones, o.semitones, "st");
        addFloat (layout, ids.fine, prefix + "Fine", linearRange (lim::oscFineCents), o.fineCents, "ct", integerText);
        addFloat (layout, ids.level, prefix + "Level", linearRange (lim::percent), o.levelPercent, "%", integerText);
        addFloat (layout, ids.pan, prefix + "Pan", linearRange (lim::bipolarPercent), o.panPercent, "%", integerText);
        addInt (layout, ids.unison, prefix + "Unison", lim::oscUnison, o.unison);
        addFloat (layout, ids.detune, prefix + "Detune", linearRange (lim::oscDetuneCents), o.detuneCents, "ct",
                  integerText);
        addFloat (layout, ids.spread, prefix + "Spread", linearRange (lim::percent), o.spreadPercent, "%",
                  integerText);
        addFloat (layout, ids.phaseRand, prefix + "Phase Rand", linearRange (lim::percent), o.phaseRandPercent, "%",
                  integerText);
    }

    // Sub / noise
    addFloat (layout, subLevel, "Sub Level", linearRange (lim::percent), d.subLevelPercent, "%", integerText);
    addChoice (layout, subOctave, "Sub Octave", subOctaveChoices, index (d.subOctave));
    addChoice (layout, subShape, "Sub Shape", subShapeChoices, index (d.subShape));
    addFloat (layout, noiseLevel, "Noise Level", linearRange (lim::percent), d.noiseLevelPercent, "%", integerText);
    addFloat (layout, noiseColor, "Noise Color", linearRange (lim::percent), d.noiseColorPercent, "%", integerText);

    // Filter
    addBool (layout, filterOn, "Filter On", d.filterOn);
    addChoice (layout, filterMode, "Filter Mode", filterModeChoices, index (d.filterMode));
    addFloat (layout, filterCutoff, "Filter Cutoff", logRange (lim::filterCutoffHz), d.filterCutoffHz, "Hz",
              frequencyText);
    addFloat (layout, filterResonance, "Filter Resonance", linearRange (lim::percent), d.filterResonancePercent, "%",
              integerText);
    addFloat (layout, filterDrive, "Filter Drive", linearRange (lim::filterDriveDb), d.filterDriveDb, "dB",
              oneDecimalText);
    addFloat (layout, filterKeytrack, "Filter Keytrack", linearRange (lim::percent), d.filterKeytrackPercent, "%",
              integerText);
    addFloat (layout, filterEnvAmount, "Filter Env Amount", linearRange (lim::bipolarPercent),
              d.filterEnvAmountPercent, "%", integerText);

    // Envelopes
    for (std::size_t i = 0; i < envelopeParameters.size(); ++i)
    {
        const auto& ids = envelopeParameters[i];
        const auto& e = d.env[i];
        const auto prefix = "Env " + juce::String (static_cast<int> (i) + 1) + " ";
        const auto timeRange = skewedRange (lim::envelopeTimeMs, envelopeTimeCentreMs);

        addFloat (layout, ids.attack, prefix + "Attack", timeRange, e.attackMs, "ms", timeText);
        addFloat (layout, ids.decay, prefix + "Decay", timeRange, e.decayMs, "ms", timeText);
        addFloat (layout, ids.sustain, prefix + "Sustain", linearRange (lim::percent), e.sustainPercent, "%",
                  integerText);
        addFloat (layout, ids.release, prefix + "Release", timeRange, e.releaseMs, "ms", timeText);
    }

    // LFOs
    for (std::size_t i = 0; i < lfoParameters.size(); ++i)
    {
        const auto& ids = lfoParameters[i];
        const auto& l = d.lfo[i];
        const auto prefix = "LFO " + juce::String (static_cast<int> (i) + 1) + " ";

        addChoice (layout, ids.shape, prefix + "Shape", lfoShapeChoices, index (l.shape));
        addFloat (layout, ids.rate, prefix + "Rate", logRange (lim::lfoRateHz), l.rateHz, "Hz", frequencyText);
        addChoice (layout, ids.sync, prefix + "Sync", tempoDivisionChoices, index (l.sync));
        addBool (layout, ids.retrig, prefix + "Retrig", l.retrigger);
        addFloat (layout, ids.fade, prefix + "Fade", skewedRange (lim::lfoFadeMs, lfoFadeCentreMs), l.fadeMs, "ms",
                  timeText);
    }

    // Mod matrix
    for (std::size_t i = 0; i < modParameters.size(); ++i)
    {
        const auto& ids = modParameters[i];
        const auto& m = d.mod[i];
        const auto prefix = "Mod " + juce::String (static_cast<int> (i) + 1) + " ";

        addChoice (layout, ids.source, prefix + "Source", modSourceChoices, index (m.source));
        addChoice (layout, ids.dest, prefix + "Dest", modDestChoices, index (m.dest));
        addFloat (layout, ids.amount, prefix + "Amount", linearRange (lim::bipolarPercent), m.amountPercent, "%",
                  integerText);
    }

    // FX
    addBool (layout, chorusOn, "Chorus On", d.chorus.on);
    addFloat (layout, chorusRate, "Chorus Rate", logRange (lim::chorusRateHz), d.chorus.rateHz, "Hz", frequencyText);
    addFloat (layout, chorusDepth, "Chorus Depth", linearRange (lim::percent), d.chorus.depthPercent, "%", integerText);
    addFloat (layout, chorusMix, "Chorus Mix", linearRange (lim::percent), d.chorus.mixPercent, "%", integerText);

    addBool (layout, delayOn, "Delay On", d.delay.on);
    addFloat (layout, delayTime, "Delay Time", logRange (lim::delayTimeMs), d.delay.timeMs, "ms", timeText);
    addChoice (layout, delaySync, "Delay Sync", tempoDivisionChoices, index (d.delay.sync));
    addFloat (layout, delayFeedback, "Delay Feedback", linearRange (lim::delayFeedbackPercent), d.delay.feedbackPercent,
              "%", integerText);
    addBool (layout, delayPingPong, "Delay Ping Pong", d.delay.pingPong);
    addFloat (layout, delayMix, "Delay Mix", linearRange (lim::percent), d.delay.mixPercent, "%", integerText);

    addBool (layout, reverbOn, "Reverb On", d.reverb.on);
    addFloat (layout, reverbSize, "Reverb Size", linearRange (lim::percent), d.reverb.sizePercent, "%", integerText);
    addFloat (layout, reverbDamping, "Reverb Damping", linearRange (lim::percent), d.reverb.dampingPercent, "%",
              integerText);
    addFloat (layout, reverbMix, "Reverb Mix", linearRange (lim::percent), d.reverb.mixPercent, "%", integerText);
}

/** IDs of all synth parameters in layout order (tests, presets, "every synth parameter has a control"). */
inline juce::StringArray getSynthParameterIds()
{
    juce::StringArray ids { synthOn.getParamID(), synthVolume.getParamID(), dryLevel.getParamID(),
                            playMode.getParamID(), glideTime.getParamID(), glideMode.getParamID(),
                            ampVelocity.getParamID(), ampGain.getParamID() };

    for (const auto& o : oscParameters)
        for (const auto* id : { &o.on, &o.position, &o.octave, &o.semi, &o.fine, &o.level, &o.pan, &o.unison,
                                &o.detune, &o.spread, &o.phaseRand })
            ids.add (id->getParamID());

    for (const auto* id : { &subLevel, &subOctave, &subShape, &noiseLevel, &noiseColor, &filterOn, &filterMode,
                            &filterCutoff, &filterResonance, &filterDrive, &filterKeytrack, &filterEnvAmount })
        ids.add (id->getParamID());

    for (const auto& e : envelopeParameters)
        for (const auto* id : { &e.attack, &e.decay, &e.sustain, &e.release })
            ids.add (id->getParamID());

    for (const auto& l : lfoParameters)
        for (const auto* id : { &l.shape, &l.rate, &l.sync, &l.retrig, &l.fade })
            ids.add (id->getParamID());

    for (const auto& m : modParameters)
        for (const auto* id : { &m.source, &m.dest, &m.amount })
            ids.add (id->getParamID());

    for (const auto* id : { &chorusOn, &chorusRate, &chorusDepth, &chorusMix, &delayOn, &delayTime, &delaySync,
                            &delayFeedback, &delayPingPong, &delayMix, &reverbOn, &reverbSize, &reverbDamping,
                            &reverbMix })
        ids.add (id->getParamID());

    return ids;
}

/** Synth parameters a preset sets: all except the mix (synthOn, synthVolume, dryLevel stay as the user set
    them). Voice, test, capture parameters are never touched by presets (docs/SYNTH-DESIGN.md 14.4). */
inline bool isPresetParameter (const juce::String& paramId)
{
    if (paramId == synthOn.getParamID() || paramId == synthVolume.getParamID() || paramId == dryLevel.getParamID())
        return false;

    return getSynthParameterIds().contains (paramId);
}

} // namespace vocalsynth::params
