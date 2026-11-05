// SynthSettings: defaults == the design's parameter defaults, sanitised() clamps every field, NaN -> default,
// tempoDivisionQuarterNotes incl. the bar length. Test plan: docs/SYNTH-DESIGN.md 21. Owner: agent B (Voice).

#include "vocalsynth/synth/SynthSettings.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

using namespace vocalsynth::synth;

namespace
{

constexpr float qnan = std::numeric_limits<float>::quiet_NaN();
constexpr float inf = std::numeric_limits<float>::infinity();

template <typename Enum>
Enum badEnum (int value)
{
    return static_cast<Enum> (value);
}

} // namespace

TEST_CASE ("SynthSettings: defaults are the parameter defaults of the design (15)", "[synth][settings]")
{
    const SynthSettings s;
    CHECK_FALSE (s.synthOn);
    CHECK (s.synthVolumeDb == -6.0f);
    CHECK (s.dryLevelDb == 0.0f);
    CHECK (s.playMode == PlayMode::legato);
    CHECK (s.glideTimeMs == 0.0f);
    CHECK (s.glideMode == GlideMode::legatoOnly);
    CHECK (s.ampVelocityPercent == 50.0f);
    CHECK (s.ampGainDb == 0.0f);

    for (std::size_t i = 0; i < 2; ++i)
    {
        const auto& o = s.osc[i];
        CHECK (o.on == (i == 0));
        CHECK (o.positionPercent == 50.0f);
        CHECK (o.octave == 0);
        CHECK (o.semitones == 0);
        CHECK (o.fineCents == 0.0f);
        CHECK (o.levelPercent == 70.0f);
        CHECK (o.panPercent == 0.0f);
        CHECK (o.unison == 1);
        CHECK (o.detuneCents == 20.0f);
        CHECK (o.spreadPercent == 50.0f);
        CHECK (o.phaseRandPercent == 0.0f);
    }

    CHECK (s.subLevelPercent == 0.0f);
    CHECK (s.subOctave == SubOctave::down1);
    CHECK (s.subShape == SubShape::sine);
    CHECK (s.noiseLevelPercent == 0.0f);
    CHECK (s.noiseColorPercent == 0.0f);
    CHECK (s.filterOn);
    CHECK (s.filterMode == FilterMode::lp24);
    CHECK (s.filterCutoffHz == 20000.0f);
    CHECK (s.filterResonancePercent == 0.0f);
    CHECK (s.filterDriveDb == 0.0f);
    CHECK (s.filterKeytrackPercent == 0.0f);
    CHECK (s.filterEnvAmountPercent == 0.0f);

    for (std::size_t i = 0; i < 3; ++i)
    {
        CHECK (s.env[i].attackMs == 5.0f);
        CHECK (s.env[i].decayMs == 300.0f);
        CHECK (s.env[i].sustainPercent == (i == 0 ? 100.0f : 0.0f));
        CHECK (s.env[i].releaseMs == 200.0f);
    }

    for (std::size_t i = 0; i < 2; ++i)
    {
        CHECK (s.lfo[i].shape == LfoShape::sine);
        CHECK (s.lfo[i].rateHz == (i == 0 ? 5.0f : 0.5f));
        CHECK (s.lfo[i].sync == TempoDivision::off);
        CHECK_FALSE (s.lfo[i].retrigger);
        CHECK (s.lfo[i].fadeMs == 0.0f);
    }

    for (const auto& m : s.mod)
        CHECK (m == ModSlotSettings { ModSource::off, ModDest::off, 0.0f });

    CHECK (s.chorus == ChorusSettings { false, 0.8f, 50.0f, 50.0f });
    CHECK (s.delay == DelaySettings { false, 375.0f, TempoDivision::off, 35.0f, false, 25.0f });
    CHECK (s.reverb == ReverbSettings { false, 50.0f, 50.0f, 25.0f });
    CHECK (s.bendRangeSemitones == 2);

    // Defaults lie inside their ranges and survive sanitising unchanged
    CHECK (s.sanitised() == s);
}

TEST_CASE ("SynthSettings: sanitised clamps every field to its range", "[synth][settings]")
{
    SynthSettings low, high;
    low.synthVolumeDb = -100.0f;      high.synthVolumeDb = 50.0f;
    low.dryLevelDb = -100.0f;         high.dryLevelDb = 10.0f;
    low.glideTimeMs = -5.0f;          high.glideTimeMs = 1.0e6f;
    low.ampVelocityPercent = -1.0f;   high.ampVelocityPercent = 101.0f;
    low.ampGainDb = -30.0f;           high.ampGainDb = 30.0f;

    for (auto* s : { &low, &high })
    {
        const bool lo = s == &low;

        for (auto& o : s->osc)
        {
            o.positionPercent = lo ? -1.0f : 200.0f;
            o.octave = lo ? -9 : 9;
            o.semitones = lo ? -30 : 30;
            o.fineCents = lo ? -500.0f : 500.0f;
            o.levelPercent = lo ? -5.0f : 105.0f;
            o.panPercent = lo ? -200.0f : 200.0f;
            o.unison = lo ? 0 : 99;
            o.detuneCents = lo ? -1.0f : 300.0f;
            o.spreadPercent = lo ? -1.0f : 101.0f;
            o.phaseRandPercent = lo ? -1.0f : 101.0f;
        }

        s->subLevelPercent = lo ? -1.0f : 101.0f;
        s->noiseLevelPercent = lo ? -1.0f : 101.0f;
        s->noiseColorPercent = lo ? -1.0f : 101.0f;
        s->filterCutoffHz = lo ? 1.0f : 1.0e6f;
        s->filterResonancePercent = lo ? -1.0f : 101.0f;
        s->filterDriveDb = lo ? -1.0f : 40.0f;
        s->filterKeytrackPercent = lo ? -1.0f : 101.0f;
        s->filterEnvAmountPercent = lo ? -200.0f : 200.0f;

        for (auto& e : s->env)
            e = lo ? EnvelopeSettings { -1.0f, -1.0f, -1.0f, -1.0f } : EnvelopeSettings { 1.0e5f, 1.0e5f, 101.0f, 1.0e5f };

        for (auto& l : s->lfo)
        {
            l.rateHz = lo ? 0.0f : 100.0f;
            l.fadeMs = lo ? -1.0f : 1.0e5f;
        }

        for (auto& m : s->mod)
            m.amountPercent = lo ? -150.0f : 150.0f;

        s->chorus.rateHz = lo ? 0.0f : 10.0f;
        s->chorus.depthPercent = lo ? -1.0f : 101.0f;
        s->chorus.mixPercent = lo ? -1.0f : 101.0f;
        s->delay.timeMs = lo ? 0.0f : 5000.0f;
        s->delay.feedbackPercent = lo ? -1.0f : 100.0f;
        s->delay.mixPercent = lo ? -1.0f : 101.0f;
        s->reverb.sizePercent = lo ? -1.0f : 101.0f;
        s->reverb.dampingPercent = lo ? -1.0f : 101.0f;
        s->reverb.mixPercent = lo ? -1.0f : 101.0f;
        s->bendRangeSemitones = lo ? -3 : 96;
    }

    const auto a = low.sanitised();
    const auto b = high.sanitised();
    CHECK (a.synthVolumeDb == -60.0f);   CHECK (b.synthVolumeDb == 6.0f);
    CHECK (a.dryLevelDb == -60.0f);      CHECK (b.dryLevelDb == 0.0f);
    CHECK (a.glideTimeMs == 0.0f);       CHECK (b.glideTimeMs == 2000.0f);
    CHECK (a.ampVelocityPercent == 0.0f); CHECK (b.ampVelocityPercent == 100.0f);
    CHECK (a.ampGainDb == -24.0f);       CHECK (b.ampGainDb == 12.0f);

    for (std::size_t i = 0; i < 2; ++i)
    {
        CHECK (a.osc[i].positionPercent == 0.0f);   CHECK (b.osc[i].positionPercent == 100.0f);
        CHECK (a.osc[i].octave == -3);              CHECK (b.osc[i].octave == 3);
        CHECK (a.osc[i].semitones == -12);          CHECK (b.osc[i].semitones == 12);
        CHECK (a.osc[i].fineCents == -100.0f);      CHECK (b.osc[i].fineCents == 100.0f);
        CHECK (a.osc[i].levelPercent == 0.0f);      CHECK (b.osc[i].levelPercent == 100.0f);
        CHECK (a.osc[i].panPercent == -100.0f);     CHECK (b.osc[i].panPercent == 100.0f);
        CHECK (a.osc[i].unison == 1);               CHECK (b.osc[i].unison == 8);
        CHECK (a.osc[i].detuneCents == 0.0f);       CHECK (b.osc[i].detuneCents == 100.0f);
        CHECK (a.osc[i].spreadPercent == 0.0f);     CHECK (b.osc[i].spreadPercent == 100.0f);
        CHECK (a.osc[i].phaseRandPercent == 0.0f);  CHECK (b.osc[i].phaseRandPercent == 100.0f);
    }

    CHECK (a.subLevelPercent == 0.0f);          CHECK (b.subLevelPercent == 100.0f);
    CHECK (a.noiseLevelPercent == 0.0f);        CHECK (b.noiseLevelPercent == 100.0f);
    CHECK (a.noiseColorPercent == 0.0f);        CHECK (b.noiseColorPercent == 100.0f);
    CHECK (a.filterCutoffHz == 20.0f);          CHECK (b.filterCutoffHz == 20000.0f);
    CHECK (a.filterResonancePercent == 0.0f);   CHECK (b.filterResonancePercent == 100.0f);
    CHECK (a.filterDriveDb == 0.0f);            CHECK (b.filterDriveDb == 24.0f);
    CHECK (a.filterKeytrackPercent == 0.0f);    CHECK (b.filterKeytrackPercent == 100.0f);
    CHECK (a.filterEnvAmountPercent == -100.0f); CHECK (b.filterEnvAmountPercent == 100.0f);

    for (std::size_t i = 0; i < 3; ++i)
    {
        CHECK (a.env[i] == EnvelopeSettings { 0.0f, 0.0f, 0.0f, 0.0f });
        CHECK (b.env[i] == EnvelopeSettings { 10000.0f, 10000.0f, 100.0f, 10000.0f });
    }

    for (std::size_t i = 0; i < 2; ++i)
    {
        CHECK (a.lfo[i].rateHz == 0.01f);  CHECK (b.lfo[i].rateHz == 30.0f);
        CHECK (a.lfo[i].fadeMs == 0.0f);   CHECK (b.lfo[i].fadeMs == 10000.0f);
    }

    for (std::size_t i = 0; i < numModSlots; ++i)
    {
        CHECK (a.mod[i].amountPercent == -100.0f);
        CHECK (b.mod[i].amountPercent == 100.0f);
    }

    CHECK (a.chorus.rateHz == 0.05f);           CHECK (b.chorus.rateHz == 5.0f);
    CHECK (a.chorus.depthPercent == 0.0f);      CHECK (b.chorus.depthPercent == 100.0f);
    CHECK (a.chorus.mixPercent == 0.0f);        CHECK (b.chorus.mixPercent == 100.0f);
    CHECK (a.delay.timeMs == 1.0f);             CHECK (b.delay.timeMs == 2000.0f);
    CHECK (a.delay.feedbackPercent == 0.0f);    CHECK (b.delay.feedbackPercent == 95.0f);
    CHECK (a.delay.mixPercent == 0.0f);         CHECK (b.delay.mixPercent == 100.0f);
    CHECK (a.reverb.sizePercent == 0.0f);       CHECK (b.reverb.sizePercent == 100.0f);
    CHECK (a.reverb.dampingPercent == 0.0f);    CHECK (b.reverb.dampingPercent == 100.0f);
    CHECK (a.reverb.mixPercent == 0.0f);        CHECK (b.reverb.mixPercent == 100.0f);
    CHECK (a.bendRangeSemitones == 0);          CHECK (b.bendRangeSemitones == 48);

    // Sanitising is idempotent
    CHECK (a.sanitised() == a);
    CHECK (b.sanitised() == b);
}

TEST_CASE ("SynthSettings: NaN / Inf become the defaults, invalid enums the default choice", "[synth][settings]")
{
    SynthSettings s;
    s.synthVolumeDb = qnan;
    s.ampGainDb = inf;
    s.glideTimeMs = -inf;
    s.osc[0].positionPercent = qnan;
    s.osc[1].levelPercent = qnan;
    s.osc[1].detuneCents = inf;
    s.filterCutoffHz = qnan;
    s.filterEnvAmountPercent = qnan;
    s.env[1].sustainPercent = qnan;
    s.env[2].releaseMs = inf;
    s.lfo[1].rateHz = qnan;
    s.mod[2].amountPercent = qnan;
    s.delay.timeMs = qnan;
    s.chorus.rateHz = qnan;
    s.reverb.mixPercent = qnan;
    s.playMode = badEnum<PlayMode> (7);
    s.glideMode = badEnum<GlideMode> (-1);
    s.subOctave = badEnum<SubOctave> (2);
    s.subShape = badEnum<SubShape> (3);
    s.filterMode = badEnum<FilterMode> (5);
    s.lfo[0].shape = badEnum<LfoShape> (7);
    s.lfo[0].sync = badEnum<TempoDivision> (15);
    s.delay.sync = badEnum<TempoDivision> (-3);
    s.mod[0].source = badEnum<ModSource> (numModSources);
    s.mod[0].dest = badEnum<ModDest> (numModDests);
    s.mod[1].dest = ModDest::pitch; // the last valid destination stays

    const auto c = s.sanitised();
    const SynthSettings d;
    CHECK (c.synthVolumeDb == d.synthVolumeDb);
    CHECK (c.ampGainDb == d.ampGainDb);
    CHECK (c.glideTimeMs == d.glideTimeMs);
    CHECK (c.osc[0].positionPercent == 50.0f);
    CHECK (c.osc[1].levelPercent == 70.0f);
    CHECK (c.osc[1].detuneCents == 20.0f);
    CHECK (c.filterCutoffHz == 20000.0f);
    CHECK (c.filterEnvAmountPercent == 0.0f);
    CHECK (c.env[1].sustainPercent == 0.0f);
    CHECK (c.env[2].releaseMs == 200.0f);
    CHECK (c.lfo[1].rateHz == 0.5f);
    CHECK (c.mod[2].amountPercent == 0.0f);
    CHECK (c.delay.timeMs == 375.0f);
    CHECK (c.chorus.rateHz == 0.8f);
    CHECK (c.reverb.mixPercent == 25.0f);
    CHECK (c.playMode == PlayMode::legato);
    CHECK (c.glideMode == GlideMode::legatoOnly);
    CHECK (c.subOctave == SubOctave::down1);
    CHECK (c.subShape == SubShape::sine);
    CHECK (c.filterMode == FilterMode::lp24);
    CHECK (c.lfo[0].shape == LfoShape::sine);
    CHECK (c.lfo[0].sync == TempoDivision::off);
    CHECK (c.delay.sync == TempoDivision::off);
    CHECK (c.mod[0].source == ModSource::off);
    CHECK (c.mod[0].dest == ModDest::off);
    CHECK (c.mod[1].dest == ModDest::pitch);

    // The valid choices of every enum survive
    for (int i = 0; i < numTempoDivisions; ++i)
    {
        SynthSettings t;
        t.lfo[1].sync = static_cast<TempoDivision> (i);
        CHECK (t.sanitised().lfo[1].sync == static_cast<TempoDivision> (i));
    }
}

TEST_CASE ("SynthSettings: tempoDivisionQuarterNotes", "[synth][settings]")
{
    CHECK (tempoDivisionQuarterNotes (TempoDivision::off) == 0.0);
    CHECK (tempoDivisionQuarterNotes (TempoDivision::d1_32) == 0.125);
    CHECK (tempoDivisionQuarterNotes (TempoDivision::d1_16T) == 1.0 / 6.0);
    CHECK (tempoDivisionQuarterNotes (TempoDivision::d1_16) == 0.25);
    CHECK (tempoDivisionQuarterNotes (TempoDivision::d1_16D) == 0.375);
    CHECK (tempoDivisionQuarterNotes (TempoDivision::d1_8T) == 1.0 / 3.0);
    CHECK (tempoDivisionQuarterNotes (TempoDivision::d1_8) == 0.5);
    CHECK (tempoDivisionQuarterNotes (TempoDivision::d1_8D) == 0.75);
    CHECK (tempoDivisionQuarterNotes (TempoDivision::d1_4T) == 2.0 / 3.0);
    CHECK (tempoDivisionQuarterNotes (TempoDivision::d1_4) == 1.0);
    CHECK (tempoDivisionQuarterNotes (TempoDivision::d1_4D) == 1.5);
    CHECK (tempoDivisionQuarterNotes (TempoDivision::d1_2) == 2.0);
    CHECK (tempoDivisionQuarterNotes (TempoDivision::bar1) == 4.0);
    CHECK (tempoDivisionQuarterNotes (TempoDivision::bars2) == 8.0);
    CHECK (tempoDivisionQuarterNotes (TempoDivision::bars4) == 16.0);

    // Bars follow the time signature (6/8 and 3/4 -> 3, 7/8 -> 3.5); the note divisions do not
    CHECK (tempoDivisionQuarterNotes (TempoDivision::bar1, 3.0) == 3.0);
    CHECK (tempoDivisionQuarterNotes (TempoDivision::bars2, 3.5) == 7.0);
    CHECK (tempoDivisionQuarterNotes (TempoDivision::bars4, 3.0) == 12.0);
    CHECK (tempoDivisionQuarterNotes (TempoDivision::d1_4, 3.0) == 1.0);
    CHECK (tempoDivisionQuarterNotes (TempoDivision::d1_8T, 7.0) == 1.0 / 3.0);

    // No / broken time signature -> 4
    CHECK (tempoDivisionQuarterNotes (TempoDivision::bar1, 0.0) == 4.0);
    CHECK (tempoDivisionQuarterNotes (TempoDivision::bar1, -2.0) == 4.0);
    CHECK (tempoDivisionQuarterNotes (TempoDivision::bar1, std::numeric_limits<double>::quiet_NaN()) == 4.0);
    CHECK (tempoDivisionQuarterNotes (TempoDivision::bar1, std::numeric_limits<double>::infinity()) == 4.0);

    static_assert (tempoDivisionQuarterNotes (TempoDivision::bars2, 3.0) == 6.0);
    CHECK (SynthTransport {} == SynthTransport { 120.0, 4.0 });
}
