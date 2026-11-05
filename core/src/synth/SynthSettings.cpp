#include "vocalsynth/synth/SynthSettings.h"

#include <cmath>

namespace vocalsynth::synth
{

namespace
{

/** Clamped to the range; NaN / Inf -> fallback (the parameter default). */
float clampValue (float value, Range range, float fallback) noexcept
{
    if (! std::isfinite (value))
        return fallback;

    return value < range.min ? range.min : (value > range.max ? range.max : value);
}

/** Integers are rounded on the way in (an int field is always integral already) and clamped. */
int clampInt (int value, Range range) noexcept
{
    const auto lo = static_cast<int> (std::lround (range.min));
    const auto hi = static_cast<int> (std::lround (range.max));
    return value < lo ? lo : (value > hi ? hi : value);
}

/** Enum forced into [0, count): an out-of-range value becomes the fallback (the default). */
template <typename Enum>
Enum clampEnum (Enum value, int count, Enum fallback) noexcept
{
    const auto index = static_cast<int> (value);
    return index >= 0 && index < count ? value : fallback;
}

OscSettings sanitiseOsc (const OscSettings& in, const OscSettings& def) noexcept
{
    OscSettings out = in;
    out.positionPercent = clampValue (in.positionPercent, limits::percent, def.positionPercent);
    out.octave = clampInt (in.octave, limits::oscOctave);
    out.semitones = clampInt (in.semitones, limits::oscSemitones);
    out.fineCents = clampValue (in.fineCents, limits::oscFineCents, def.fineCents);
    out.levelPercent = clampValue (in.levelPercent, limits::percent, def.levelPercent);
    out.panPercent = clampValue (in.panPercent, limits::bipolarPercent, def.panPercent);
    out.unison = clampInt (in.unison, limits::oscUnison);
    out.detuneCents = clampValue (in.detuneCents, limits::oscDetuneCents, def.detuneCents);
    out.spreadPercent = clampValue (in.spreadPercent, limits::percent, def.spreadPercent);
    out.phaseRandPercent = clampValue (in.phaseRandPercent, limits::percent, def.phaseRandPercent);
    return out;
}

EnvelopeSettings sanitiseEnvelope (const EnvelopeSettings& in, const EnvelopeSettings& def) noexcept
{
    EnvelopeSettings out;
    out.attackMs = clampValue (in.attackMs, limits::envelopeTimeMs, def.attackMs);
    out.decayMs = clampValue (in.decayMs, limits::envelopeTimeMs, def.decayMs);
    out.sustainPercent = clampValue (in.sustainPercent, limits::percent, def.sustainPercent);
    out.releaseMs = clampValue (in.releaseMs, limits::envelopeTimeMs, def.releaseMs);
    return out;
}

LfoSettings sanitiseLfo (const LfoSettings& in, const LfoSettings& def) noexcept
{
    LfoSettings out = in;
    out.shape = clampEnum (in.shape, numLfoShapes, def.shape);
    out.rateHz = clampValue (in.rateHz, limits::lfoRateHz, def.rateHz);
    out.sync = clampEnum (in.sync, numTempoDivisions, def.sync);
    out.fadeMs = clampValue (in.fadeMs, limits::lfoFadeMs, def.fadeMs);
    return out;
}

} // namespace

SynthSettings SynthSettings::sanitised() const noexcept
{
    const SynthSettings def {};
    SynthSettings s = *this;

    s.synthVolumeDb = clampValue (synthVolumeDb, limits::synthVolumeDb, def.synthVolumeDb);
    s.dryLevelDb = clampValue (dryLevelDb, limits::dryLevelDb, def.dryLevelDb);

    s.playMode = clampEnum (playMode, numPlayModes, def.playMode);
    s.glideTimeMs = clampValue (glideTimeMs, limits::glideTimeMs, def.glideTimeMs);
    s.glideMode = clampEnum (glideMode, numGlideModes, def.glideMode);
    s.ampVelocityPercent = clampValue (ampVelocityPercent, limits::percent, def.ampVelocityPercent);
    s.ampGainDb = clampValue (ampGainDb, limits::ampGainDb, def.ampGainDb);

    for (std::size_t i = 0; i < osc.size(); ++i)
        s.osc[i] = sanitiseOsc (osc[i], def.osc[i]);

    s.subLevelPercent = clampValue (subLevelPercent, limits::percent, def.subLevelPercent);
    s.subOctave = clampEnum (subOctave, numSubOctaves, def.subOctave);
    s.subShape = clampEnum (subShape, numSubShapes, def.subShape);
    s.noiseLevelPercent = clampValue (noiseLevelPercent, limits::percent, def.noiseLevelPercent);
    s.noiseColorPercent = clampValue (noiseColorPercent, limits::percent, def.noiseColorPercent);

    s.filterMode = clampEnum (filterMode, numFilterModes, def.filterMode);
    s.filterCutoffHz = clampValue (filterCutoffHz, limits::filterCutoffHz, def.filterCutoffHz);
    s.filterResonancePercent = clampValue (filterResonancePercent, limits::percent, def.filterResonancePercent);
    s.filterDriveDb = clampValue (filterDriveDb, limits::filterDriveDb, def.filterDriveDb);
    s.filterKeytrackPercent = clampValue (filterKeytrackPercent, limits::percent, def.filterKeytrackPercent);
    s.filterEnvAmountPercent = clampValue (filterEnvAmountPercent, limits::bipolarPercent, def.filterEnvAmountPercent);

    for (std::size_t i = 0; i < env.size(); ++i)
        s.env[i] = sanitiseEnvelope (env[i], def.env[i]);

    for (std::size_t i = 0; i < lfo.size(); ++i)
        s.lfo[i] = sanitiseLfo (lfo[i], def.lfo[i]);

    for (auto& slot : s.mod)
    {
        slot.source = clampEnum (slot.source, numModSources, ModSource::off);
        slot.dest = clampEnum (slot.dest, numModDests, ModDest::off);
        slot.amountPercent = clampValue (slot.amountPercent, limits::bipolarPercent, 0.0f);
    }

    s.chorus.rateHz = clampValue (chorus.rateHz, limits::chorusRateHz, def.chorus.rateHz);
    s.chorus.depthPercent = clampValue (chorus.depthPercent, limits::percent, def.chorus.depthPercent);
    s.chorus.mixPercent = clampValue (chorus.mixPercent, limits::percent, def.chorus.mixPercent);

    s.delay.timeMs = clampValue (delay.timeMs, limits::delayTimeMs, def.delay.timeMs);
    s.delay.sync = clampEnum (delay.sync, numTempoDivisions, def.delay.sync);
    s.delay.feedbackPercent = clampValue (delay.feedbackPercent, limits::delayFeedbackPercent, def.delay.feedbackPercent);
    s.delay.mixPercent = clampValue (delay.mixPercent, limits::percent, def.delay.mixPercent);

    s.reverb.sizePercent = clampValue (reverb.sizePercent, limits::percent, def.reverb.sizePercent);
    s.reverb.dampingPercent = clampValue (reverb.dampingPercent, limits::percent, def.reverb.dampingPercent);
    s.reverb.mixPercent = clampValue (reverb.mixPercent, limits::percent, def.reverb.mixPercent);

    s.bendRangeSemitones = bendRangeSemitones < 0 ? 0 : (bendRangeSemitones > 48 ? 48 : bendRangeSemitones);
    return s;
}

} // namespace vocalsynth::synth
