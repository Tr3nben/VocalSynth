#include "FactoryPresets.h"

#include "vocalsynth/synth/SynthSettings.h"

namespace vocalsynth
{

// The 24 factory presets of 0.4.0 (docs/SYNTH-DESIGN.md 14.3), data only. Each preset lists ONLY the values
// that differ from the parameter defaults (SynthSettings {}): Init is the empty list. Mix parameters (synthOn,
// synthVolume, dryLevel) are never part of a preset.
//
// Sound design rules (agent F):
// - Mono voices (leads, basses, vocal) stay in the default Legato (connected note changes keep the envelopes,
//   glide only on connected changes); Mono where every note should re-pluck (Acid, Pluck Bass); Poly for pads,
//   keys and the atmosphere preset (the overlapping releases of the sung line become a chord-like wash).
// - Every preset except Init uses at least one voice source (Voice Level / Brightness / Noisiness / Pitch).
//   Amounts are moderate: a singer rarely reaches 1.0 on any of them. Voice Level -> Amp only in Choir Pad and
//   only at a small amount (Voice Level falls with 50 ms at the end of a note and would shorten the release).
// - Bass presets play one octave below the sung note (Osc octave -1; the sub at -1 Oct, which follows the note,
//   doubles that fundamental), so a voice around C3..C4 lands on a bass register. Sub Bass keeps Osc A at the
//   sung pitch as a soft overtone over the sub.
// - Poly presets start every oscillator with random phases, so the notes of a chord do not add up in phase.
// - Unison above 2 voices always with phase random >= 50 % (docs/SYNTH-DESIGN.md 6.1: in-phase unison starts
//   with a +9 dB peak at 8 voices).
// - Level matching (ampGain): every preset except Init is set to -18 LUFS +-1 LU (K-weighted, BS.1770
//   weighting, maximum momentary loudness over 400 ms windows, L + R) for one held C4, velocity 100, with a
//   constant "typical singing" expression (level 0.7, brightness 0.5, noisiness 0.2, pitch 0), synthVolume
//   -6 dB, 48 kHz. Init stays at the defaults (ampGain 0 dB, about -16.5 LUFS). -18 leaves the headroom for the
//   peak rule: <= -1 dBFS for every golden sequence (single notes on the mono presets, the overlapping
//   releases of the legato run) and for a 4-note chord on the Poly presets (the worst is about -2.3 dBFS).
//   Envelope decay / release times reach -80 dB at the set time (docs/SYNTH-DESIGN.md 8.1), so plucks need
//   longer decay values than in synths that count a time constant.
// - Noise colour stays mostly white: pink noise carries enough sub-audio energy to fail the DC check.
//   Checked by GoldenPresetTests ("level-matched", "peaks below -1 dBFS"); the hidden test
//   "[.][presetmeasure]" prints loudness, peaks, CPU, centroid and rise time per preset.

namespace
{
using namespace synth;

template <typename Enum>
constexpr float e (Enum value) noexcept
{
    return static_cast<float> (static_cast<int> (value));
}

constexpr float on = 1.0f;

// Choice values used below
constexpr float poly = e (PlayMode::poly);
constexpr float mono = e (PlayMode::mono);
constexpr float always = e (GlideMode::always);
constexpr float lp12 = e (FilterMode::lp12);

//==============================================================================
// Lead

constexpr FactoryPresetValue voiceLead[] {
    { "glideTime", 60.0f },
    { "ampGain", -5.5f },
    { "oscAUnison", 2.0f }, { "oscADetune", 12.0f }, { "oscASpread", 40.0f }, { "oscAPhaseRand", 100.0f },
    { "oscBOn", on }, { "oscBPosition", 30.0f }, { "oscBOctave", -1.0f }, { "oscBLevel", 35.0f },
    { "filterCutoff", 1400.0f }, { "filterResonance", 18.0f }, { "filterKeytrack", 50.0f }, { "filterEnvAmount", 15.0f },
    { "env1Attack", 8.0f }, { "env1Release", 250.0f },
    { "env2Decay", 500.0f }, { "env2Sustain", 30.0f }, { "env2Release", 300.0f },
    { "mod1Source", e (ModSource::voiceLevel) }, { "mod1Dest", e (ModDest::filterCutoff) }, { "mod1Amount", 35.0f },
    { "mod2Source", e (ModSource::voiceBrightness) }, { "mod2Dest", e (ModDest::filterCutoff) }, { "mod2Amount", 15.0f },
    { "chorusOn", on }, { "chorusRate", 0.6f }, { "chorusDepth", 35.0f }, { "chorusMix", 30.0f },
    { "delayOn", on }, { "delaySync", e (TempoDivision::d1_8D) }, { "delayFeedback", 30.0f }, { "delayPingPong", on },
    { "delayMix", 18.0f },
    { "reverbOn", on }, { "reverbSize", 45.0f }, { "reverbMix", 18.0f },
};

constexpr FactoryPresetValue supersawLead[] {
    { "glideTime", 40.0f },
    { "ampGain", -4.7f },
    { "oscAUnison", 7.0f }, { "oscADetune", 38.0f }, { "oscASpread", 85.0f }, { "oscAPhaseRand", 100.0f },
    { "oscBOn", on }, { "oscBOctave", 1.0f }, { "oscBLevel", 30.0f }, { "oscBUnison", 3.0f },
    { "oscBPhaseRand", 100.0f },
    { "filterCutoff", 5200.0f }, { "filterResonance", 10.0f }, { "filterKeytrack", 30.0f },
    { "env1Attack", 10.0f }, { "env1Release", 350.0f },
    { "mod1Source", e (ModSource::voiceLevel) }, { "mod1Dest", e (ModDest::filterCutoff) }, { "mod1Amount", 18.0f },
    { "mod2Source", e (ModSource::voiceBrightness) }, { "mod2Dest", e (ModDest::oscBLevel) }, { "mod2Amount", 25.0f },
    { "delayOn", on }, { "delaySync", e (TempoDivision::d1_8D) }, { "delayPingPong", on }, { "delayMix", 20.0f },
    { "reverbOn", on }, { "reverbSize", 55.0f }, { "reverbMix", 22.0f },
};

constexpr FactoryPresetValue pwmLead[] {
    { "glideTime", 80.0f },
    { "ampGain", -7.1f },
    { "oscAPosition", 25.0f },
    { "oscBOn", on }, { "oscBPosition", 60.0f }, { "oscBFine", 8.0f }, { "oscBLevel", 45.0f },
    { "filterMode", lp12 }, { "filterCutoff", 3500.0f }, { "filterResonance", 10.0f }, { "filterKeytrack", 40.0f },
    { "lfo1Rate", 5.5f }, { "lfo1Retrig", on }, { "lfo1Fade", 700.0f },
    { "lfo2Shape", e (LfoShape::triangle) }, { "lfo2Rate", 0.35f },
    { "mod1Source", e (ModSource::lfo1) }, { "mod1Dest", e (ModDest::pitch) }, { "mod1Amount", 2.0f },
    { "mod2Source", e (ModSource::lfo2) }, { "mod2Dest", e (ModDest::oscAPosition) }, { "mod2Amount", 30.0f },
    { "mod3Source", e (ModSource::lfo2) }, { "mod3Dest", e (ModDest::oscBPosition) }, { "mod3Amount", -25.0f },
    { "mod4Source", e (ModSource::voiceLevel) }, { "mod4Dest", e (ModDest::filterCutoff) }, { "mod4Amount", 25.0f },
    { "delayOn", on }, { "delaySync", e (TempoDivision::d1_4) }, { "delayFeedback", 25.0f }, { "delayMix", 15.0f },
    { "reverbOn", on }, { "reverbMix", 15.0f },
};

constexpr FactoryPresetValue syncLead[] {
    { "glideTime", 50.0f },
    { "ampGain", -4.6f },
    { "oscAPosition", 15.0f }, { "oscAUnison", 2.0f }, { "oscADetune", 10.0f }, { "oscAPhaseRand", 100.0f },
    { "oscBOn", on }, { "oscBOctave", -1.0f }, { "oscBLevel", 30.0f },
    { "filterMode", lp12 }, { "filterCutoff", 6000.0f }, { "filterResonance", 15.0f }, { "filterDrive", 6.0f },
    { "env3Attack", 0.0f }, { "env3Decay", 450.0f }, { "env3Sustain", 25.0f }, { "env3Release", 300.0f },
    { "mod1Source", e (ModSource::env3) }, { "mod1Dest", e (ModDest::oscAPosition) }, { "mod1Amount", 45.0f },
    { "mod2Source", e (ModSource::voiceBrightness) }, { "mod2Dest", e (ModDest::oscAPosition) }, { "mod2Amount", 35.0f },
    { "mod3Source", e (ModSource::voiceLevel) }, { "mod3Dest", e (ModDest::filterCutoff) }, { "mod3Amount", 15.0f },
    { "delayOn", on }, { "delaySync", e (TempoDivision::d1_8) }, { "delayFeedback", 30.0f }, { "delayMix", 15.0f },
    { "reverbOn", on }, { "reverbMix", 15.0f },
};

constexpr FactoryPresetValue brassLead[] {
    { "glideTime", 30.0f },
    { "ampGain", -5.5f },
    { "oscAUnison", 3.0f }, { "oscADetune", 14.0f }, { "oscAPhaseRand", 100.0f },
    { "oscBOn", on }, { "oscBPosition", 62.0f }, { "oscBFine", -6.0f }, { "oscBLevel", 50.0f },
    { "filterCutoff", 700.0f }, { "filterResonance", 12.0f }, { "filterKeytrack", 60.0f }, { "filterEnvAmount", 40.0f },
    { "env1Attack", 40.0f }, { "env1Release", 220.0f },
    { "env2Attack", 80.0f }, { "env2Decay", 600.0f }, { "env2Sustain", 45.0f }, { "env2Release", 300.0f },
    { "mod1Source", e (ModSource::voiceLevel) }, { "mod1Dest", e (ModDest::filterCutoff) }, { "mod1Amount", 35.0f },
    { "mod2Source", e (ModSource::velocity) }, { "mod2Dest", e (ModDest::filterCutoff) }, { "mod2Amount", 10.0f },
    { "reverbOn", on }, { "reverbMix", 18.0f },
};

constexpr FactoryPresetValue whistle[] {
    { "glideTime", 90.0f }, { "glideMode", always },
    { "ampGain", -2.8f },
    { "oscAPosition", 3.0f }, { "oscAOctave", 1.0f },
    { "noiseLevel", 4.0f }, { "noiseColor", 15.0f },
    { "filterMode", lp12 }, { "filterCutoff", 3000.0f }, { "filterResonance", 20.0f }, { "filterKeytrack", 100.0f },
    { "env1Attack", 30.0f }, { "env1Release", 180.0f },
    { "lfo1Retrig", on }, { "lfo1Fade", 900.0f },
    { "mod1Source", e (ModSource::voiceNoisiness) }, { "mod1Dest", e (ModDest::noiseLevel) }, { "mod1Amount", 40.0f },
    { "mod2Source", e (ModSource::lfo1) }, { "mod2Dest", e (ModDest::pitch) }, { "mod2Amount", 1.5f },
    { "mod3Source", e (ModSource::voiceLevel) }, { "mod3Dest", e (ModDest::filterCutoff) }, { "mod3Amount", 10.0f },
    { "delayOn", on }, { "delaySync", e (TempoDivision::d1_4D) }, { "delayFeedback", 25.0f }, { "delayMix", 12.0f },
    { "reverbOn", on }, { "reverbSize", 60.0f },
};

//==============================================================================
// Bass

constexpr FactoryPresetValue subBass[] {
    { "glideTime", 45.0f },
    { "ampGain", -6.2f },
    { "oscAPosition", 22.0f }, { "oscALevel", 40.0f },
    { "subLevel", 75.0f },
    { "filterCutoff", 900.0f }, { "filterDrive", 3.0f },
    { "env1Attack", 4.0f }, { "env1Release", 120.0f },
    { "mod1Source", e (ModSource::voiceLevel) }, { "mod1Dest", e (ModDest::filterCutoff) }, { "mod1Amount", 20.0f },
};

constexpr FactoryPresetValue reeseBass[] {
    { "glideTime", 40.0f },
    { "ampGain", -5.5f },
    { "oscAOctave", -1.0f }, { "oscALevel", 60.0f }, { "oscAUnison", 2.0f }, { "oscADetune", 28.0f },
    { "oscASpread", 30.0f }, { "oscAPhaseRand", 100.0f },
    { "oscBOn", on }, { "oscBPosition", 20.0f }, { "oscBOctave", -1.0f }, { "oscBFine", 9.0f }, { "oscBLevel", 55.0f }, { "oscBPhaseRand", 100.0f },
    { "subLevel", 25.0f },
    { "filterCutoff", 1000.0f }, { "filterResonance", 18.0f }, { "filterDrive", 6.0f }, { "filterKeytrack", 30.0f },
    { "env1Release", 150.0f },
    { "lfo2Rate", 0.25f },
    { "mod1Source", e (ModSource::lfo2) }, { "mod1Dest", e (ModDest::filterCutoff) }, { "mod1Amount", 6.0f },
    { "mod2Source", e (ModSource::lfo2) }, { "mod2Dest", e (ModDest::oscADetune) }, { "mod2Amount", 15.0f },
    { "mod3Source", e (ModSource::voiceLevel) }, { "mod3Dest", e (ModDest::filterCutoff) }, { "mod3Amount", 25.0f },
};

constexpr FactoryPresetValue acidBass[] {
    { "playMode", mono }, { "glideTime", 70.0f },
    { "ampGain", -3.8f },
    { "oscAOctave", -1.0f },
    { "filterCutoff", 280.0f }, { "filterResonance", 72.0f }, { "filterDrive", 9.0f }, { "filterKeytrack", 40.0f },
    { "filterEnvAmount", 50.0f },
    { "env1Attack", 2.0f }, { "env1Decay", 400.0f }, { "env1Sustain", 85.0f }, { "env1Release", 120.0f },
    { "env2Attack", 0.0f }, { "env2Decay", 280.0f }, { "env2Sustain", 15.0f },
    { "mod1Source", e (ModSource::voiceLevel) }, { "mod1Dest", e (ModDest::filterCutoff) }, { "mod1Amount", 22.0f },
    { "mod2Source", e (ModSource::voiceBrightness) }, { "mod2Dest", e (ModDest::filterCutoff) }, { "mod2Amount", 15.0f },
    { "delayOn", on }, { "delaySync", e (TempoDivision::d1_8D) }, { "delayFeedback", 30.0f }, { "delayMix", 10.0f },
};

constexpr FactoryPresetValue pluckBass[] {
    { "playMode", mono },
    { "ampGain", -1.5f },
    { "oscAPosition", 62.0f }, { "oscAOctave", -1.0f },
    { "subLevel", 35.0f },
    { "filterCutoff", 220.0f }, { "filterResonance", 25.0f }, { "filterKeytrack", 50.0f }, { "filterEnvAmount", 55.0f },
    { "env1Attack", 1.0f }, { "env1Decay", 700.0f }, { "env1Sustain", 35.0f }, { "env1Release", 120.0f },
    { "env2Attack", 0.0f }, { "env2Decay", 220.0f }, { "env2Release", 150.0f },
    { "mod1Source", e (ModSource::voiceLevel) }, { "mod1Dest", e (ModDest::filterCutoff) }, { "mod1Amount", 15.0f },
    { "mod2Source", e (ModSource::velocity) }, { "mod2Dest", e (ModDest::filterCutoff) }, { "mod2Amount", 15.0f },
};

constexpr FactoryPresetValue fmBass[] {
    { "glideTime", 35.0f },
    { "ampGain", -7.9f },
    { "oscAPosition", 20.0f }, { "oscAOctave", -1.0f },
    { "subLevel", 35.0f },
    { "filterCutoff", 2200.0f }, { "filterResonance", 10.0f }, { "filterKeytrack", 30.0f },
    { "env3Attack", 0.0f }, { "env3Decay", 250.0f },
    { "mod1Source", e (ModSource::voiceBrightness) }, { "mod1Dest", e (ModDest::oscAPosition) }, { "mod1Amount", 45.0f },
    { "mod2Source", e (ModSource::voiceLevel) }, { "mod2Dest", e (ModDest::oscAPosition) }, { "mod2Amount", 15.0f },
    { "mod3Source", e (ModSource::env3) }, { "mod3Dest", e (ModDest::oscAPosition) }, { "mod3Amount", 20.0f },
};

//==============================================================================
// Pad (Poly)

constexpr FactoryPresetValue warmPad[] {
    { "playMode", poly }, { "ampVelocity", 30.0f },
    { "ampGain", -5.8f },
    { "oscAPosition", 25.0f }, { "oscAUnison", 5.0f }, { "oscADetune", 28.0f },
    { "oscASpread", 80.0f }, { "oscAPhaseRand", 100.0f },
    { "oscBOn", on }, { "oscBPosition", 30.0f }, { "oscBOctave", -1.0f }, { "oscBLevel", 30.0f }, { "oscBUnison", 3.0f },
    { "oscBDetune", 18.0f }, { "oscBPhaseRand", 100.0f },
    { "filterCutoff", 3000.0f }, { "filterResonance", 8.0f }, { "filterKeytrack", 30.0f },
    { "env1Attack", 700.0f }, { "env1Decay", 1000.0f }, { "env1Sustain", 90.0f }, { "env1Release", 1800.0f },
    { "lfo2Rate", 0.2f },
    { "mod1Source", e (ModSource::lfo2) }, { "mod1Dest", e (ModDest::oscAPosition) }, { "mod1Amount", 12.0f },
    { "mod2Source", e (ModSource::voiceLevel) }, { "mod2Dest", e (ModDest::filterCutoff) }, { "mod2Amount", 20.0f },
    { "mod3Source", e (ModSource::voiceBrightness) }, { "mod3Dest", e (ModDest::oscAPosition) }, { "mod3Amount", -20.0f },
    { "chorusOn", on }, { "chorusRate", 0.4f }, { "chorusMix", 40.0f },
    { "reverbOn", on }, { "reverbSize", 70.0f }, { "reverbDamping", 55.0f }, { "reverbMix", 30.0f },
};

constexpr FactoryPresetValue choirPad[] {
    { "playMode", poly },
    { "ampGain", -7.2f },
    { "oscAPosition", 20.0f }, { "oscALevel", 65.0f }, { "oscAUnison", 4.0f }, { "oscADetune", 18.0f },
    { "oscASpread", 70.0f }, { "oscAPhaseRand", 100.0f },
    { "oscBOn", on }, { "oscBPosition", 75.0f }, { "oscBFine", 6.0f }, { "oscBLevel", 30.0f }, { "oscBPhaseRand", 100.0f },
    { "noiseColor", 30.0f },
    { "filterMode", lp12 }, { "filterCutoff", 5000.0f },
    { "env1Attack", 450.0f }, { "env1Decay", 800.0f }, { "env1Sustain", 85.0f }, { "env1Release", 1400.0f },
    { "mod1Source", e (ModSource::voiceBrightness) }, { "mod1Dest", e (ModDest::oscAPosition) }, { "mod1Amount", 50.0f },
    { "mod2Source", e (ModSource::voiceLevel) }, { "mod2Dest", e (ModDest::ampLevel) }, { "mod2Amount", 20.0f },
    { "mod3Source", e (ModSource::voiceNoisiness) }, { "mod3Dest", e (ModDest::noiseLevel) }, { "mod3Amount", 15.0f },
    { "chorusOn", on }, { "chorusMix", 35.0f },
    { "reverbOn", on }, { "reverbSize", 75.0f }, { "reverbMix", 32.0f },
};

constexpr FactoryPresetValue glassPad[] {
    { "playMode", poly },
    { "ampGain", -7.5f },
    { "oscAPosition", 25.0f }, { "oscALevel", 60.0f }, { "oscAUnison", 3.0f }, { "oscADetune", 10.0f },
    { "oscASpread", 60.0f }, { "oscAPhaseRand", 100.0f },
    { "oscBOn", on }, { "oscBPosition", 85.0f }, { "oscBOctave", 1.0f }, { "oscBLevel", 30.0f }, { "oscBUnison", 2.0f },
    { "oscBDetune", 8.0f }, { "oscBPhaseRand", 100.0f },
    { "filterMode", lp12 }, { "filterCutoff", 7000.0f },
    { "env1Attack", 250.0f }, { "env1Decay", 1500.0f }, { "env1Sustain", 70.0f }, { "env1Release", 2500.0f },
    { "lfo1Shape", e (LfoShape::triangle) }, { "lfo1Rate", 0.15f },
    { "lfo2Rate", 0.23f },
    { "mod1Source", e (ModSource::lfo1) }, { "mod1Dest", e (ModDest::oscAPosition) }, { "mod1Amount", 25.0f },
    { "mod2Source", e (ModSource::lfo2) }, { "mod2Dest", e (ModDest::oscBPosition) }, { "mod2Amount", -20.0f },
    { "mod3Source", e (ModSource::voiceBrightness) }, { "mod3Dest", e (ModDest::oscBLevel) }, { "mod3Amount", 30.0f },
    { "chorusOn", on }, { "chorusMix", 30.0f },
    { "delayOn", on }, { "delaySync", e (TempoDivision::d1_4D) }, { "delayPingPong", on }, { "delayMix", 15.0f },
    { "reverbOn", on }, { "reverbSize", 80.0f }, { "reverbDamping", 30.0f }, { "reverbMix", 35.0f },
};

//==============================================================================
// Pluck / Keys (Poly)

constexpr FactoryPresetValue polyPluck[] {
    { "playMode", poly },
    { "ampGain", 0.9f },
    { "oscALevel", 65.0f }, { "oscAUnison", 2.0f }, { "oscADetune", 10.0f }, { "oscAPhaseRand", 100.0f },
    { "oscBOn", on }, { "oscBPosition", 75.0f }, { "oscBOctave", 1.0f }, { "oscBLevel", 30.0f }, { "oscBPhaseRand", 100.0f },
    { "filterCutoff", 700.0f }, { "filterResonance", 20.0f }, { "filterKeytrack", 50.0f }, { "filterEnvAmount", 40.0f },
    { "env1Attack", 1.0f }, { "env1Decay", 3000.0f }, { "env1Sustain", 15.0f }, { "env1Release", 500.0f },
    { "env2Attack", 0.0f }, { "env2Decay", 320.0f }, { "env2Release", 250.0f },
    { "mod1Source", e (ModSource::velocity) }, { "mod1Dest", e (ModDest::filterCutoff) }, { "mod1Amount", 15.0f },
    { "mod2Source", e (ModSource::voiceLevel) }, { "mod2Dest", e (ModDest::filterCutoff) }, { "mod2Amount", 10.0f },
    { "delayOn", on }, { "delaySync", e (TempoDivision::d1_8D) }, { "delayPingPong", on }, { "delayMix", 22.0f },
    { "reverbOn", on }, { "reverbMix", 20.0f },
};

constexpr FactoryPresetValue fmEPiano[] {
    { "playMode", poly }, { "ampVelocity", 70.0f },
    { "ampGain", -6.5f },
    { "oscAPosition", 18.0f }, { "oscALevel", 65.0f }, { "oscAPhaseRand", 100.0f },
    { "oscBOn", on }, { "oscBPosition", 0.0f }, { "oscBLevel", 35.0f }, { "oscBPhaseRand", 100.0f },
    { "filterMode", lp12 }, { "filterCutoff", 9000.0f },
    { "env1Attack", 2.0f }, { "env1Decay", 2500.0f }, { "env1Sustain", 25.0f }, { "env1Release", 500.0f },
    { "env3Attack", 0.0f }, { "env3Decay", 700.0f },
    { "mod1Source", e (ModSource::env3) }, { "mod1Dest", e (ModDest::oscAPosition) }, { "mod1Amount", 28.0f },
    { "mod2Source", e (ModSource::velocity) }, { "mod2Dest", e (ModDest::oscAPosition) }, { "mod2Amount", 15.0f },
    { "mod3Source", e (ModSource::voiceBrightness) }, { "mod3Dest", e (ModDest::oscAPosition) }, { "mod3Amount", 10.0f },
    { "chorusOn", on }, { "chorusRate", 1.2f }, { "chorusDepth", 35.0f }, { "chorusMix", 35.0f },
    { "reverbOn", on }, { "reverbSize", 40.0f }, { "reverbMix", 15.0f },
};

constexpr FactoryPresetValue organ[] {
    { "playMode", poly }, { "ampVelocity", 0.0f },
    { "ampGain", -5.2f },
    { "oscAPosition", 35.0f }, { "oscALevel", 60.0f }, { "oscAPhaseRand", 100.0f },
    { "filterMode", lp12 }, { "filterCutoff", 9000.0f },
    { "env1Attack", 4.0f }, { "env1Release", 90.0f },
    { "mod1Source", e (ModSource::voiceBrightness) }, { "mod1Dest", e (ModDest::oscAPosition) }, { "mod1Amount", 40.0f },
    { "chorusOn", on }, { "chorusRate", 4.5f }, { "chorusDepth", 25.0f }, { "chorusMix", 40.0f },
    { "reverbOn", on }, { "reverbSize", 45.0f }, { "reverbMix", 18.0f },
};

//==============================================================================
// Vocal (voice-driven timbre)

constexpr FactoryPresetValue vowelMorph[] {
    { "glideTime", 50.0f },
    { "ampGain", -3.5f },
    { "oscAPosition", 100.0f }, { "oscAUnison", 2.0f }, { "oscADetune", 8.0f }, { "oscAPhaseRand", 100.0f },
    { "oscBOn", on }, { "oscBPosition", 60.0f }, { "oscBOctave", -1.0f }, { "oscBLevel", 20.0f },
    { "filterMode", lp12 }, { "filterCutoff", 7000.0f },
    // Voice Brightness walks the formant table from U (dark) over O to I (bright): sing "uu" vs "ii"
    { "mod1Source", e (ModSource::voiceBrightness) }, { "mod1Dest", e (ModDest::oscAPosition) }, { "mod1Amount", -50.0f },
    { "mod2Source", e (ModSource::voiceLevel) }, { "mod2Dest", e (ModDest::filterCutoff) }, { "mod2Amount", 15.0f },
    { "chorusOn", on }, { "chorusMix", 20.0f },
    { "reverbOn", on }, { "reverbMix", 20.0f },
};

constexpr FactoryPresetValue talkingSaw[] {
    { "glideTime", 40.0f },
    { "ampGain", -4.7f },
    { "oscAUnison", 3.0f }, { "oscADetune", 12.0f }, { "oscASpread", 40.0f }, { "oscAPhaseRand", 100.0f },
    { "oscBOn", on }, { "oscBPosition", 75.0f }, { "oscBOctave", -1.0f }, { "oscBLevel", 30.0f },
    { "filterCutoff", 380.0f }, { "filterResonance", 50.0f }, { "filterDrive", 4.0f }, { "filterKeytrack", 50.0f },
    { "mod1Source", e (ModSource::voiceBrightness) }, { "mod1Dest", e (ModDest::filterCutoff) }, { "mod1Amount", 45.0f },
    { "mod2Source", e (ModSource::voiceLevel) }, { "mod2Dest", e (ModDest::filterCutoff) }, { "mod2Amount", 25.0f },
    { "delayOn", on }, { "delaySync", e (TempoDivision::d1_8) }, { "delayFeedback", 25.0f }, { "delayMix", 12.0f },
};

constexpr FactoryPresetValue breathyVoice[] {
    { "glideTime", 60.0f },
    { "ampGain", -5.2f },
    { "oscAPosition", 12.0f }, { "oscALevel", 55.0f },
    { "oscBOn", on }, { "oscBPosition", 15.0f }, { "oscBLevel", 45.0f },
    { "noiseLevel", 3.0f }, { "noiseColor", 10.0f },
    { "filterMode", lp12 }, { "filterCutoff", 2800.0f }, { "filterResonance", 5.0f }, { "filterKeytrack", 40.0f },
    { "env1Attack", 25.0f }, { "env1Release", 300.0f },
    { "mod1Source", e (ModSource::voiceNoisiness) }, { "mod1Dest", e (ModDest::noiseLevel) }, { "mod1Amount", 50.0f },
    { "mod2Source", e (ModSource::voiceNoisiness) }, { "mod2Dest", e (ModDest::filterCutoff) }, { "mod2Amount", 10.0f },
    { "mod3Source", e (ModSource::voiceBrightness) }, { "mod3Dest", e (ModDest::oscAPosition) }, { "mod3Amount", 25.0f },
    { "chorusOn", on }, { "chorusMix", 20.0f },
    { "reverbOn", on }, { "reverbSize", 60.0f }, { "reverbMix", 28.0f },
};

constexpr FactoryPresetValue vibratoWah[] {
    { "glideTime", 50.0f },
    { "ampGain", -6.9f },
    { "oscAPosition", 55.0f }, { "oscAUnison", 2.0f }, { "oscADetune", 10.0f }, { "oscAPhaseRand", 100.0f },
    { "oscBOn", on }, { "oscBPosition", 40.0f }, { "oscBOctave", -1.0f }, { "oscBLevel", 35.0f },
    { "filterCutoff", 800.0f }, { "filterResonance", 55.0f }, { "filterKeytrack", 40.0f },
    { "lfo1Retrig", on }, { "lfo1Fade", 1200.0f },
    // The singer's own vibrato (Voice Pitch, +-100 cents = +-1) opens and closes the filter
    { "mod1Source", e (ModSource::voicePitch) }, { "mod1Dest", e (ModDest::filterCutoff) }, { "mod1Amount", 35.0f },
    { "mod2Source", e (ModSource::voiceLevel) }, { "mod2Dest", e (ModDest::filterCutoff) }, { "mod2Amount", 20.0f },
    { "mod3Source", e (ModSource::lfo1) }, { "mod3Dest", e (ModDest::filterCutoff) }, { "mod3Amount", 5.0f },
    { "chorusOn", on }, { "chorusMix", 20.0f },
    { "delayOn", on }, { "delaySync", e (TempoDivision::d1_8D) }, { "delayFeedback", 25.0f }, { "delayMix", 12.0f },
};

//==============================================================================
// FX

constexpr FactoryPresetValue spaceDrift[] {
    { "playMode", poly },
    { "ampGain", -7.7f },
    { "oscAPosition", 30.0f }, { "oscALevel", 55.0f }, { "oscAUnison", 4.0f }, { "oscADetune", 25.0f },
    { "oscASpread", 90.0f }, { "oscAPhaseRand", 100.0f },
    { "oscBOn", on }, { "oscBPosition", 20.0f }, { "oscBOctave", 1.0f }, { "oscBLevel", 25.0f }, { "oscBUnison", 2.0f },
    { "oscBDetune", 15.0f }, { "oscBPhaseRand", 100.0f },
    { "noiseColor", 35.0f },
    { "filterCutoff", 2500.0f }, { "filterResonance", 25.0f },
    { "env1Attack", 900.0f }, { "env1Decay", 2000.0f }, { "env1Sustain", 80.0f }, { "env1Release", 3500.0f },
    { "lfo1Shape", e (LfoShape::smoothRandom) }, { "lfo1Rate", 0.4f },
    { "lfo2Rate", 0.07f },
    { "mod1Source", e (ModSource::lfo1) }, { "mod1Dest", e (ModDest::oscAPosition) }, { "mod1Amount", 40.0f },
    { "mod2Source", e (ModSource::lfo2) }, { "mod2Dest", e (ModDest::filterCutoff) }, { "mod2Amount", 15.0f },
    { "mod3Source", e (ModSource::voiceNoisiness) }, { "mod3Dest", e (ModDest::noiseLevel) }, { "mod3Amount", 25.0f },
    { "mod4Source", e (ModSource::voiceBrightness) }, { "mod4Dest", e (ModDest::oscBPosition) }, { "mod4Amount", 40.0f },
    { "delayOn", on }, { "delaySync", e (TempoDivision::d1_4D) }, { "delayFeedback", 55.0f }, { "delayPingPong", on },
    { "reverbOn", on }, { "reverbSize", 90.0f }, { "reverbDamping", 40.0f }, { "reverbMix", 45.0f },
};

constexpr FactoryPresetValue glitch[] {
    { "ampGain", -7.3f },
    { "oscAPosition", 30.0f }, { "oscALevel", 65.0f },
    { "oscBOn", on }, { "oscBOctave", -1.0f }, { "oscBLevel", 35.0f },
    { "filterMode", lp12 }, { "filterCutoff", 4000.0f }, { "filterResonance", 30.0f },
    { "lfo1Shape", e (LfoShape::sampleHold) }, { "lfo1Sync", e (TempoDivision::d1_16) },
    { "lfo2Shape", e (LfoShape::square) }, { "lfo2Sync", e (TempoDivision::d1_8) },
    { "mod1Source", e (ModSource::lfo1) }, { "mod1Dest", e (ModDest::oscAPosition) }, { "mod1Amount", 45.0f },
    { "mod2Source", e (ModSource::lfo2) }, { "mod2Dest", e (ModDest::filterCutoff) }, { "mod2Amount", 25.0f },
    { "mod3Source", e (ModSource::voiceBrightness) }, { "mod3Dest", e (ModDest::oscAPosition) }, { "mod3Amount", 20.0f },
    { "mod4Source", e (ModSource::voiceNoisiness) }, { "mod4Dest", e (ModDest::delayMix) }, { "mod4Amount", 30.0f },
    { "delayOn", on }, { "delaySync", e (TempoDivision::d1_16D) }, { "delayFeedback", 45.0f }, { "delayPingPong", on },
    { "reverbOn", on }, { "reverbMix", 15.0f },
};

constexpr std::array<FactoryPreset, 24> presets { {
    { "Init", PresetCategory::init, "", "basic-shapes", "basic-shapes", {} },

    { "Voice Lead", PresetCategory::lead, "voice,saw,warm", "basic-shapes", "saw-bright-dark", voiceLead },
    { "Supersaw Lead", PresetCategory::lead, "supersaw,bright,wide", "basic-shapes", "basic-shapes", supersawLead },
    { "PWM Lead", PresetCategory::lead, "square,pwm,vibrato", "pwm", "pwm", pwmLead },
    { "Sync Lead", PresetCategory::lead, "sync,bright,aggressive", "sync-sweep", "basic-shapes", syncLead },
    { "Brass Lead", PresetCategory::lead, "brass,swell,warm", "basic-shapes", "basic-shapes", brassLead },
    { "Whistle", PresetCategory::lead, "whistle,sine,breath", "basic-shapes", "basic-shapes", whistle },

    { "Sub Bass", PresetCategory::bass, "sub,deep,clean", "basic-shapes", "basic-shapes", subBass },
    { "Reese Bass", PresetCategory::bass, "reese,detuned,dark", "basic-shapes", "saw-bright-dark", reeseBass },
    { "Acid Bass", PresetCategory::bass, "acid,resonant,squelch", "basic-shapes", "basic-shapes", acidBass },
    { "Pluck Bass", PresetCategory::bass, "pluck,punchy", "basic-shapes", "basic-shapes", pluckBass },
    { "FM Bass", PresetCategory::bass, "fm,growl,voice", "fm-2op", "basic-shapes", fmBass },

    { "Warm Pad", PresetCategory::pad, "warm,wide,slow", "saw-bright-dark", "basic-shapes", warmPad },
    { "Choir Pad", PresetCategory::pad, "choir,vowel,airy", "choir", "formant-vowel", choirPad },
    { "Glass Pad", PresetCategory::pad, "glass,bell,shimmer", "bell", "harmonic-sweep", glassPad },

    { "Poly Pluck", PresetCategory::pluckKeys, "pluck,bright,short", "basic-shapes", "basic-shapes", polyPluck },
    { "FM E-Piano", PresetCategory::pluckKeys, "fm,keys,soft", "fm-2op", "basic-shapes", fmEPiano },
    { "Drawbar Organ", PresetCategory::pluckKeys, "organ,drawbar,keys", "organ", "basic-shapes", organ },

    { "Vowel Morph", PresetCategory::vocal, "vowel,formant,voice", "formant-vowel", "saw-bright-dark", vowelMorph },
    { "Talking Saw", PresetCategory::vocal, "talk,wah,filter", "basic-shapes", "basic-shapes", talkingSaw },
    { "Breathy Voice", PresetCategory::vocal, "breath,air,soft", "formant-vowel", "basic-shapes", breathyVoice },
    { "Vibrato Wah", PresetCategory::vocal, "wah,vibrato,funky", "basic-shapes", "pwm", vibratoWah },

    { "Space Drift", PresetCategory::fx, "atmos,evolving,space", "spectral-random", "fold", spaceDrift },
    { "Glitch", PresetCategory::fx, "glitch,digital,rhythmic", "digital", "square-stack", glitch },
} };
} // namespace

int FactoryPresets::getNumPresets() noexcept
{
    return static_cast<int> (presets.size());
}

const FactoryPreset& FactoryPresets::getPreset (int index) noexcept
{
    const auto i = index < 0 ? 0 : (index >= getNumPresets() ? getNumPresets() - 1 : index);
    return presets[static_cast<std::size_t> (i)];
}

} // namespace vocalsynth
