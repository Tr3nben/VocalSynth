#include "FactoryPresets.h"

#include "vocalsynth/synth/SynthSettings.h"

namespace vocalsynth
{

// The 40 factory presets (docs/SYNTH-DESIGN.md 14.3), data only: indices 0..23 from 0.4.0, 24..39 appended in
// 0.4.1 (Trap 24..31 tagged "trap", R&B 32..39 tagged "rnb"; presetGenres in FactoryPresets.h). Never reorder
// or change a released preset: projects store the name, the golden data the sound. Each preset lists ONLY the
// values that differ from the parameter defaults (SynthSettings {}): Init is the empty list. Mix parameters
// (synthOn, synthVolume, dryLevel) are never part of a preset.
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
//   releases of the legato run) and for a 4-note chord on the Poly presets (0.4.0: the worst is about
//   -2.3 dBFS; 0.4.1: Glass Arp -1.6, Trap Bell / Dark Pluck -1.8 dBFS).
//   Envelope decay / release times reach -80 dB at the set time (docs/SYNTH-DESIGN.md 8.1), so plucks need
//   longer decay values than in synths that count a time constant.
// - Noise colour stays mostly white: pink noise carries enough sub-audio energy to fail the DC check.
//   Checked by GoldenPresetTests ("level-matched", "peaks below -1 dBFS"); the hidden test
//   "[.][presetmeasure]" prints loudness, peaks, CPU, centroid, rise time, |mean| and the power above 200 Hz.
// - 0.4.1 (Trap / R&B, same rules): the 808s are sines (Osc A near position 0 + sub, both phase 0, through the
//   symmetric drive: no DC) one octave below the sung note, Legato with glide so a sung slide becomes an 808
//   slide; Env 3 -> Pitch gives the "punch" only on new notes. Drive + Voice Level -> Filter Drive add the
//   harmonics that small speakers play (about -11 / -9 dB of the power above 200 Hz at C3). Plucks and bells
//   keep a little sustain or reverb so their 4-note chord peak stays below -1 dBFS at -18 LUFS. The bell table
//   darkens along its position, so bell presets modulate it negatively (Env 3 / Brightness = brighter).

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

//==============================================================================
// 0.4.1: Trap (indices 24..31, tag "trap"), grouped by category like the list above

constexpr FactoryPresetValue trapFlute[] {
    { "glideTime", 70.0f },
    { "ampGain", -5.3f },
    { "oscAPosition", 18.0f }, { "oscAOctave", 1.0f },
    { "oscBOn", on }, { "oscBPosition", 100.0f }, { "oscBLevel", 20.0f },
    { "noiseLevel", 5.0f },
    { "filterMode", lp12 }, { "filterCutoff", 4500.0f }, { "filterResonance", 10.0f }, { "filterKeytrack", 60.0f },
    { "env1Attack", 45.0f }, { "env1Release", 220.0f },
    { "lfo1Rate", 5.2f }, { "lfo1Retrig", on }, { "lfo1Fade", 600.0f },
    // Breath: the sung "hh" / airy voice blows noise into the tone; louder singing overblows towards the triangle
    { "mod1Source", e (ModSource::voiceNoisiness) }, { "mod1Dest", e (ModDest::noiseLevel) }, { "mod1Amount", 35.0f },
    { "mod2Source", e (ModSource::voiceLevel) }, { "mod2Dest", e (ModDest::oscAPosition) }, { "mod2Amount", 15.0f },
    { "mod3Source", e (ModSource::lfo1) }, { "mod3Dest", e (ModDest::pitch) }, { "mod3Amount", 1.2f },
    { "mod4Source", e (ModSource::voiceBrightness) }, { "mod4Dest", e (ModDest::filterCutoff) }, { "mod4Amount", 12.0f },
    { "delayOn", on }, { "delaySync", e (TempoDivision::d1_4) }, { "delayFeedback", 30.0f }, { "delayPingPong", on },
    { "delayMix", 15.0f },
    { "reverbOn", on }, { "reverbSize", 65.0f },
};

constexpr FactoryPresetValue brassStab[] {
    { "playMode", mono },
    { "ampGain", 3.3f },
    { "oscAUnison", 3.0f }, { "oscADetune", 16.0f }, { "oscASpread", 40.0f }, { "oscAPhaseRand", 100.0f },
    { "oscBOn", on }, { "oscBPosition", 62.0f }, { "oscBOctave", -1.0f }, { "oscBLevel", 40.0f },
    { "filterCutoff", 600.0f }, { "filterResonance", 15.0f }, { "filterKeytrack", 50.0f }, { "filterEnvAmount", 45.0f },
    { "env1Attack", 3.0f }, { "env1Decay", 600.0f }, { "env1Sustain", 35.0f }, { "env1Release", 150.0f },
    { "env2Attack", 0.0f }, { "env2Decay", 350.0f }, { "env2Sustain", 20.0f },
    { "mod1Source", e (ModSource::voiceLevel) }, { "mod1Dest", e (ModDest::filterCutoff) }, { "mod1Amount", 30.0f },
    { "mod2Source", e (ModSource::velocity) }, { "mod2Dest", e (ModDest::filterCutoff) }, { "mod2Amount", 10.0f },
    { "delayOn", on }, { "delaySync", e (TempoDivision::d1_8) }, { "delayFeedback", 20.0f }, { "delayMix", 10.0f },
    { "reverbOn", on }, { "reverbSize", 40.0f }, { "reverbMix", 15.0f },
};

constexpr FactoryPresetValue glide808[] {
    { "glideTime", 110.0f },
    { "ampGain", -2.6f },
    { "oscAPosition", 4.0f }, { "oscAOctave", -1.0f }, { "oscALevel", 60.0f },
    { "subLevel", 55.0f },
    { "filterCutoff", 1800.0f }, { "filterDrive", 10.0f }, { "filterKeytrack", 40.0f },
    { "env1Attack", 1.0f }, { "env1Decay", 2500.0f }, { "env1Sustain", 55.0f }, { "env1Release", 250.0f },
    // The 808 "punch": a short pitch drop on every new note; connected (sung) slides keep it off (Legato)
    { "env3Attack", 0.0f }, { "env3Decay", 80.0f },
    { "mod1Source", e (ModSource::env3) }, { "mod1Dest", e (ModDest::pitch) }, { "mod1Amount", 15.0f },
    // Louder singing drives the sine harder: harmonics that small speakers can play
    { "mod2Source", e (ModSource::voiceLevel) }, { "mod2Dest", e (ModDest::filterDrive) }, { "mod2Amount", 25.0f },
    { "mod3Source", e (ModSource::voiceBrightness) }, { "mod3Dest", e (ModDest::filterCutoff) }, { "mod3Amount", 15.0f },
};

constexpr FactoryPresetValue distorted808[] {
    { "glideTime", 90.0f },
    { "ampGain", 0.5f },
    { "oscAPosition", 15.0f }, { "oscAOctave", -1.0f }, { "oscALevel", 65.0f },
    { "subLevel", 45.0f },
    { "filterCutoff", 2600.0f }, { "filterResonance", 10.0f }, { "filterDrive", 20.0f }, { "filterKeytrack", 30.0f },
    { "env1Attack", 1.0f }, { "env1Decay", 3000.0f }, { "env1Sustain", 60.0f },
    { "env3Attack", 0.0f }, { "env3Decay", 50.0f },
    { "mod1Source", e (ModSource::env3) }, { "mod1Dest", e (ModDest::pitch) }, { "mod1Amount", 20.0f },
    { "mod2Source", e (ModSource::voiceLevel) }, { "mod2Dest", e (ModDest::filterDrive) }, { "mod2Amount", 15.0f },
    { "mod3Source", e (ModSource::voiceBrightness) }, { "mod3Dest", e (ModDest::filterCutoff) }, { "mod3Amount", 20.0f },
};

constexpr FactoryPresetValue darkAtmos[] {
    { "playMode", poly }, { "ampVelocity", 30.0f },
    { "ampGain", -8.6f },
    { "oscAPosition", 40.0f }, { "oscALevel", 55.0f }, { "oscAUnison", 4.0f }, { "oscADetune", 22.0f },
    { "oscASpread", 85.0f }, { "oscAPhaseRand", 100.0f },
    { "oscBOn", on }, { "oscBPosition", 30.0f }, { "oscBOctave", -1.0f }, { "oscBLevel", 35.0f }, { "oscBUnison", 2.0f },
    { "oscBDetune", 12.0f }, { "oscBPhaseRand", 100.0f },
    { "filterCutoff", 900.0f }, { "filterResonance", 20.0f }, { "filterKeytrack", 20.0f },
    { "env1Attack", 1200.0f }, { "env1Decay", 2000.0f }, { "env1Sustain", 85.0f }, { "env1Release", 3000.0f },
    { "lfo1Shape", e (LfoShape::smoothRandom) }, { "lfo1Rate", 0.3f },
    { "lfo2Rate", 0.1f },
    { "mod1Source", e (ModSource::lfo1) }, { "mod1Dest", e (ModDest::oscAPosition) }, { "mod1Amount", 30.0f },
    { "mod2Source", e (ModSource::lfo2) }, { "mod2Dest", e (ModDest::filterCutoff) }, { "mod2Amount", 8.0f },
    { "mod3Source", e (ModSource::voiceBrightness) }, { "mod3Dest", e (ModDest::filterCutoff) }, { "mod3Amount", 25.0f },
    { "mod4Source", e (ModSource::voiceNoisiness) }, { "mod4Dest", e (ModDest::noiseLevel) }, { "mod4Amount", 12.0f },
    { "mod5Source", e (ModSource::voiceLevel) }, { "mod5Dest", e (ModDest::oscBLevel) }, { "mod5Amount", 20.0f },
    { "chorusOn", on }, { "chorusMix", 30.0f },
    { "delayOn", on }, { "delaySync", e (TempoDivision::d1_2) }, { "delayFeedback", 50.0f }, { "delayPingPong", on },
    { "delayMix", 20.0f },
    { "reverbOn", on }, { "reverbSize", 90.0f }, { "reverbDamping", 65.0f }, { "reverbMix", 45.0f },
};

constexpr FactoryPresetValue trapBell[] {
    { "playMode", poly }, { "ampVelocity", 40.0f },
    { "ampGain", -0.2f },
    { "oscAPosition", 35.0f }, { "oscAOctave", 1.0f }, { "oscALevel", 65.0f }, { "oscAPhaseRand", 100.0f },
    { "oscBOn", on }, { "oscBPosition", 0.0f }, { "oscBOctave", 1.0f }, { "oscBLevel", 25.0f }, { "oscBPhaseRand", 100.0f },
    { "env1Attack", 0.0f }, { "env1Decay", 1400.0f }, { "env1Sustain", 0.0f }, { "env1Release", 900.0f },
    { "env3Attack", 0.0f }, { "env3Decay", 400.0f },
    // The bell table darkens along its position: Env 3 starts every strike bright, a bright voice keeps it bright
    { "mod1Source", e (ModSource::env3) }, { "mod1Dest", e (ModDest::oscAPosition) }, { "mod1Amount", -25.0f },
    { "mod2Source", e (ModSource::voiceBrightness) }, { "mod2Dest", e (ModDest::oscAPosition) }, { "mod2Amount", -20.0f },
    { "mod3Source", e (ModSource::voiceNoisiness) }, { "mod3Dest", e (ModDest::delayMix) }, { "mod3Amount", 20.0f },
    { "delayOn", on }, { "delaySync", e (TempoDivision::d1_8D) }, { "delayPingPong", on }, { "delayMix", 20.0f },
    { "reverbOn", on }, { "reverbSize", 55.0f }, { "reverbMix", 22.0f },
};

constexpr FactoryPresetValue darkPluck[] {
    { "playMode", poly },
    { "ampGain", 0.1f },
    { "oscAPosition", 40.0f }, { "oscALevel", 65.0f }, { "oscAUnison", 2.0f }, { "oscADetune", 10.0f },
    { "oscAPhaseRand", 100.0f },
    { "oscBOn", on }, { "oscBPosition", 62.0f }, { "oscBOctave", -1.0f }, { "oscBLevel", 20.0f }, { "oscBPhaseRand", 100.0f },
    { "filterCutoff", 420.0f }, { "filterResonance", 18.0f }, { "filterKeytrack", 40.0f }, { "filterEnvAmount", 25.0f },
    { "env1Attack", 1.0f }, { "env1Decay", 2500.0f }, { "env1Sustain", 20.0f }, { "env1Release", 400.0f },
    { "env2Attack", 0.0f }, { "env2Decay", 450.0f }, { "env2Release", 300.0f },
    { "mod1Source", e (ModSource::voiceBrightness) }, { "mod1Dest", e (ModDest::filterCutoff) }, { "mod1Amount", 20.0f },
    { "mod2Source", e (ModSource::velocity) }, { "mod2Dest", e (ModDest::filterCutoff) }, { "mod2Amount", 10.0f },
    { "delayOn", on }, { "delaySync", e (TempoDivision::d1_4) }, { "delayFeedback", 25.0f }, { "delayMix", 18.0f },
    { "reverbOn", on }, { "reverbSize", 55.0f }, { "reverbDamping", 70.0f }, { "reverbMix", 30.0f },
};

constexpr FactoryPresetValue glassArp[] {
    { "playMode", poly },
    { "ampGain", -1.7f },
    { "oscAPosition", 30.0f }, { "oscALevel", 60.0f }, { "oscAUnison", 2.0f }, { "oscADetune", 8.0f },
    { "oscAPhaseRand", 100.0f },
    { "oscBOn", on }, { "oscBPosition", 60.0f }, { "oscBOctave", 1.0f }, { "oscBLevel", 25.0f }, { "oscBPhaseRand", 100.0f },
    { "filterMode", lp12 }, { "filterCutoff", 3000.0f }, { "filterResonance", 15.0f }, { "filterKeytrack", 50.0f },
    { "filterEnvAmount", 30.0f },
    { "env1Attack", 0.0f }, { "env1Decay", 1600.0f }, { "env1Sustain", 8.0f }, { "env1Release", 300.0f },
    { "env2Attack", 0.0f }, { "env2Decay", 350.0f },
    { "mod1Source", e (ModSource::voiceBrightness) }, { "mod1Dest", e (ModDest::oscBLevel) }, { "mod1Amount", 30.0f },
    { "mod2Source", e (ModSource::voiceLevel) }, { "mod2Dest", e (ModDest::filterCutoff) }, { "mod2Amount", 15.0f },
    { "delayOn", on }, { "delaySync", e (TempoDivision::d1_16) }, { "delayFeedback", 45.0f }, { "delayPingPong", on },
    { "reverbOn", on }, { "reverbSize", 60.0f }, { "reverbDamping", 30.0f }, { "reverbMix", 30.0f },
};

//==============================================================================
// 0.4.1: R&B (indices 32..39, tag "rnb")

constexpr FactoryPresetValue smoothRnbLead[] {
    { "glideTime", 60.0f },
    { "ampGain", -4.4f },
    { "oscAPosition", 32.0f }, { "oscAUnison", 2.0f }, { "oscADetune", 8.0f }, { "oscASpread", 30.0f },
    { "oscAPhaseRand", 100.0f },
    { "oscBOn", on }, { "oscBPosition", 0.0f }, { "oscBOctave", -1.0f }, { "oscBLevel", 30.0f },
    { "filterMode", lp12 }, { "filterCutoff", 2200.0f }, { "filterResonance", 8.0f }, { "filterKeytrack", 40.0f },
    { "env1Attack", 20.0f }, { "env1Release", 300.0f },
    // The singer's own vibrato (Voice Pitch) widened by about half on top of the bend
    { "mod1Source", e (ModSource::voicePitch) }, { "mod1Dest", e (ModDest::pitch) }, { "mod1Amount", 4.0f },
    { "mod2Source", e (ModSource::voiceLevel) }, { "mod2Dest", e (ModDest::filterCutoff) }, { "mod2Amount", 20.0f },
    { "mod3Source", e (ModSource::voiceBrightness) }, { "mod3Dest", e (ModDest::oscAPosition) }, { "mod3Amount", 20.0f },
    { "chorusOn", on }, { "chorusRate", 0.5f }, { "chorusDepth", 30.0f }, { "chorusMix", 25.0f },
    { "delayOn", on }, { "delaySync", e (TempoDivision::d1_4) }, { "delayFeedback", 25.0f }, { "delayPingPong", on },
    { "delayMix", 14.0f },
    { "reverbOn", on }, { "reverbSize", 55.0f }, { "reverbMix", 22.0f },
};

constexpr FactoryPresetValue slowJamLead[] {
    { "glideTime", 140.0f }, { "glideMode", always },
    { "ampGain", -6.2f },
    { "oscAPosition", 20.0f },
    { "oscBOn", on }, { "oscBPosition", 0.0f }, { "oscBOctave", 1.0f }, { "oscBLevel", 20.0f },
    { "filterMode", lp12 }, { "filterCutoff", 2600.0f }, { "filterResonance", 12.0f }, { "filterKeytrack", 50.0f },
    { "env1Attack", 15.0f }, { "env1Release", 350.0f },
    { "lfo1Rate", 4.8f }, { "lfo1Retrig", on }, { "lfo1Fade", 900.0f },
    { "mod1Source", e (ModSource::voiceBrightness) }, { "mod1Dest", e (ModDest::oscAPosition) }, { "mod1Amount", 35.0f },
    { "mod2Source", e (ModSource::voiceLevel) }, { "mod2Dest", e (ModDest::filterCutoff) }, { "mod2Amount", 15.0f },
    { "mod3Source", e (ModSource::lfo1) }, { "mod3Dest", e (ModDest::pitch) }, { "mod3Amount", 1.5f },
    { "chorusOn", on }, { "chorusRate", 0.3f }, { "chorusMix", 20.0f },
    { "delayOn", on }, { "delaySync", e (TempoDivision::d1_4D) }, { "delayFeedback", 30.0f }, { "delayMix", 15.0f },
    { "reverbOn", on }, { "reverbSize", 60.0f }, { "reverbMix", 22.0f },
};

constexpr FactoryPresetValue rnbBass90s[] {
    { "glideTime", 35.0f },
    { "ampGain", -4.7f },
    { "oscAPosition", 60.0f }, { "oscAOctave", -1.0f }, { "oscALevel", 65.0f },
    { "subLevel", 40.0f },
    { "filterCutoff", 380.0f }, { "filterResonance", 30.0f }, { "filterDrive", 4.0f }, { "filterKeytrack", 40.0f },
    { "filterEnvAmount", 30.0f },
    { "env1Attack", 2.0f }, { "env1Decay", 900.0f }, { "env1Sustain", 70.0f }, { "env1Release", 140.0f },
    { "env2Attack", 0.0f }, { "env2Decay", 350.0f }, { "env2Sustain", 25.0f },
    { "mod1Source", e (ModSource::voiceLevel) }, { "mod1Dest", e (ModDest::filterCutoff) }, { "mod1Amount", 20.0f },
    { "mod2Source", e (ModSource::voiceBrightness) }, { "mod2Dest", e (ModDest::filterCutoff) }, { "mod2Amount", 10.0f },
};

constexpr FactoryPresetValue silkPad[] {
    { "playMode", poly }, { "ampVelocity", 30.0f },
    { "ampGain", -5.4f },
    { "oscAPosition", 45.0f }, { "oscALevel", 60.0f }, { "oscAUnison", 4.0f }, { "oscASpread", 75.0f },
    { "oscAPhaseRand", 100.0f },
    { "oscBOn", on }, { "oscBPosition", 25.0f }, { "oscBOctave", 1.0f }, { "oscBLevel", 20.0f }, { "oscBPhaseRand", 100.0f },
    { "filterMode", lp12 }, { "filterCutoff", 2000.0f }, { "filterResonance", 5.0f }, { "filterKeytrack", 30.0f },
    { "env1Attack", 900.0f }, { "env1Decay", 1500.0f }, { "env1Sustain", 85.0f }, { "env1Release", 2400.0f },
    { "lfo2Rate", 0.15f },
    { "mod1Source", e (ModSource::lfo2) }, { "mod1Dest", e (ModDest::oscAPosition) }, { "mod1Amount", 10.0f },
    { "mod2Source", e (ModSource::voiceBrightness) }, { "mod2Dest", e (ModDest::filterCutoff) }, { "mod2Amount", 20.0f },
    { "mod3Source", e (ModSource::voiceLevel) }, { "mod3Dest", e (ModDest::oscBLevel) }, { "mod3Amount", 20.0f },
    { "chorusOn", on }, { "chorusRate", 0.3f }, { "chorusDepth", 60.0f }, { "chorusMix", 45.0f },
    { "reverbOn", on }, { "reverbSize", 75.0f }, { "reverbDamping", 60.0f }, { "reverbMix", 35.0f },
};

constexpr FactoryPresetValue airyChoir[] {
    { "playMode", poly },
    { "ampGain", -4.5f },
    { "oscAPosition", 45.0f }, { "oscALevel", 60.0f }, { "oscAUnison", 3.0f }, { "oscADetune", 14.0f },
    { "oscASpread", 65.0f }, { "oscAPhaseRand", 100.0f },
    { "oscBOn", on }, { "oscBOctave", 1.0f }, { "oscBLevel", 20.0f }, { "oscBPhaseRand", 100.0f },
    { "noiseLevel", 4.0f },
    { "filterMode", lp12 }, { "filterCutoff", 7500.0f },
    { "env1Attack", 600.0f }, { "env1Decay", 1000.0f }, { "env1Sustain", 85.0f }, { "env1Release", 1800.0f },
    { "mod1Source", e (ModSource::voiceBrightness) }, { "mod1Dest", e (ModDest::oscAPosition) }, { "mod1Amount", 40.0f },
    { "mod2Source", e (ModSource::voiceNoisiness) }, { "mod2Dest", e (ModDest::noiseLevel) }, { "mod2Amount", 25.0f },
    { "chorusOn", on }, { "chorusMix", 30.0f },
    { "delayOn", on }, { "delaySync", e (TempoDivision::d1_4D) }, { "delayPingPong", on }, { "delayMix", 12.0f },
    { "reverbOn", on }, { "reverbSize", 85.0f }, { "reverbDamping", 35.0f }, { "reverbMix", 40.0f },
};

constexpr FactoryPresetValue neoSoulKeys[] {
    { "playMode", poly }, { "ampVelocity", 70.0f },
    { "ampGain", -6.7f },
    { "oscAPosition", 12.0f }, { "oscALevel", 65.0f }, { "oscAPhaseRand", 100.0f },
    { "oscBOn", on }, { "oscBPosition", 0.0f }, { "oscBFine", 4.0f }, { "oscBLevel", 35.0f }, { "oscBPhaseRand", 100.0f },
    { "filterMode", lp12 }, { "filterCutoff", 6000.0f },
    { "env1Attack", 2.0f }, { "env1Decay", 3500.0f }, { "env1Sustain", 30.0f }, { "env1Release", 450.0f },
    { "env3Attack", 0.0f }, { "env3Decay", 500.0f },
    { "lfo2Rate", 3.5f },
    // Tine bark (Env 3 + velocity -> FM index), stereo tremolo (LFO 2 -> pan)
    { "mod1Source", e (ModSource::env3) }, { "mod1Dest", e (ModDest::oscAPosition) }, { "mod1Amount", 22.0f },
    { "mod2Source", e (ModSource::velocity) }, { "mod2Dest", e (ModDest::oscAPosition) }, { "mod2Amount", 12.0f },
    { "mod3Source", e (ModSource::voiceBrightness) }, { "mod3Dest", e (ModDest::oscAPosition) }, { "mod3Amount", 12.0f },
    { "mod4Source", e (ModSource::lfo2) }, { "mod4Dest", e (ModDest::pan) }, { "mod4Amount", 35.0f },
    { "chorusOn", on }, { "chorusRate", 0.7f }, { "chorusDepth", 30.0f }, { "chorusMix", 25.0f },
    { "reverbOn", on }, { "reverbSize", 40.0f }, { "reverbMix", 16.0f },
};

constexpr FactoryPresetValue bellKeys[] {
    { "playMode", poly }, { "ampVelocity", 60.0f },
    { "ampGain", -5.4f },
    { "oscAPosition", 30.0f }, { "oscALevel", 60.0f }, { "oscAPhaseRand", 100.0f },
    { "oscBOn", on }, { "oscBPosition", 25.0f }, { "oscBLevel", 30.0f }, { "oscBPhaseRand", 100.0f },
    { "filterMode", lp12 }, { "filterCutoff", 8000.0f },
    { "env1Attack", 1.0f }, { "env1Decay", 2800.0f }, { "env1Sustain", 20.0f }, { "env1Release", 700.0f },
    { "env3Attack", 0.0f }, { "env3Decay", 600.0f },
    { "mod1Source", e (ModSource::env3) }, { "mod1Dest", e (ModDest::oscBPosition) }, { "mod1Amount", 25.0f },
    { "mod2Source", e (ModSource::voiceBrightness) }, { "mod2Dest", e (ModDest::oscAPosition) }, { "mod2Amount", -25.0f },
    { "chorusOn", on }, { "chorusRate", 0.9f }, { "chorusDepth", 40.0f }, { "chorusMix", 30.0f },
    { "reverbOn", on }, { "reverbSize", 55.0f }, { "reverbMix", 22.0f },
};

constexpr FactoryPresetValue talkboxLead[] {
    { "glideTime", 45.0f },
    { "ampGain", -3.6f },
    { "oscAPosition", 0.0f },
    { "oscBOn", on }, { "oscBPosition", 75.0f }, { "oscBOctave", -1.0f }, { "oscBLevel", 25.0f },
    { "filterCutoff", 3500.0f }, { "filterResonance", 20.0f }, { "filterDrive", 3.0f }, { "filterKeytrack", 30.0f },
    { "env1Attack", 6.0f }, { "env1Release", 180.0f },
    // Voice Brightness opens the "mouth" from A (dark voice) over E to I (bright voice), Level -> cutoff
    { "mod1Source", e (ModSource::voiceBrightness) }, { "mod1Dest", e (ModDest::oscAPosition) }, { "mod1Amount", 50.0f },
    { "mod2Source", e (ModSource::voiceLevel) }, { "mod2Dest", e (ModDest::filterCutoff) }, { "mod2Amount", 20.0f },
    { "chorusOn", on }, { "chorusMix", 15.0f },
    { "delayOn", on }, { "delaySync", e (TempoDivision::d1_8) }, { "delayFeedback", 20.0f }, { "delayMix", 10.0f },
    { "reverbOn", on }, { "reverbMix", 15.0f },
};

constexpr std::array<FactoryPreset, 40> presets { {
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

    // 0.4.1: Trap
    { "Trap Flute", PresetCategory::lead, "trap,flute,breath", "basic-shapes", "formant-vowel", trapFlute },
    { "Brass Stab", PresetCategory::lead, "trap,brass,stab", "basic-shapes", "basic-shapes", brassStab },
    { "808 Glide Bass", PresetCategory::bass, "trap,808,glide,sub", "basic-shapes", "basic-shapes", glide808 },
    { "808 Distorted", PresetCategory::bass, "trap,808,distorted", "basic-shapes", "basic-shapes", distorted808 },
    { "Dark Atmos", PresetCategory::pad, "trap,dark,atmos", "spectral-random", "choir", darkAtmos },
    { "Trap Bell", PresetCategory::pluckKeys, "trap,bell,bright", "bell", "basic-shapes", trapBell },
    { "Dark Pluck", PresetCategory::pluckKeys, "trap,pluck,dark", "saw-bright-dark", "basic-shapes", darkPluck },
    { "Glass Arp", PresetCategory::pluckKeys, "trap,glass,arp", "harmonic-sweep", "bell", glassArp },

    // 0.4.1: R&B
    { "Smooth R&B Lead", PresetCategory::lead, "rnb,smooth,vibrato", "basic-shapes", "basic-shapes", smoothRnbLead },
    { "Slow Jam Lead", PresetCategory::lead, "rnb,slowjam,portamento", "pwm", "basic-shapes", slowJamLead },
    { "90s R&B Bass", PresetCategory::bass, "rnb,90s,synthbass", "basic-shapes", "basic-shapes", rnbBass90s },
    { "Silk Pad", PresetCategory::pad, "rnb,silk,warm", "saw-bright-dark", "basic-shapes", silkPad },
    { "Airy Choir", PresetCategory::pad, "rnb,choir,airy", "choir", "formant-vowel", airyChoir },
    { "Neo-Soul Keys", PresetCategory::pluckKeys, "rnb,keys,rhodes", "fm-2op", "basic-shapes", neoSoulKeys },
    { "Bell Keys", PresetCategory::pluckKeys, "rnb,bell,keys", "bell", "fm-2op", bellKeys },
    { "Talkbox Lead", PresetCategory::vocal, "rnb,talkbox,formant", "formant-vowel", "basic-shapes", talkboxLead },
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
