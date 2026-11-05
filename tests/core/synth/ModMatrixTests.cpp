// ModMatrix: scaling of every destination, summing, off slots, global / per-voice split.
// Test plan: docs/SYNTH-DESIGN.md 21. Owner: agent B (Voice).

#include "vocalsynth/synth/ModMatrix.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <limits>

using namespace vocalsynth::synth;
using Catch::Matchers::WithinAbs;

namespace
{

ModSourceValues allSources (float value)
{
    ModSourceValues v {};
    v.fill (value);
    v[0] = 0.0f;
    return v;
}

std::array<ModSlotSettings, numModSlots> oneSlot (ModSource s, ModDest d, float amount)
{
    std::array<ModSlotSettings, numModSlots> slots {};
    slots[3] = { s, d, amount };
    return slots;
}

} // namespace

TEST_CASE ("ModMatrix: every destination scales as in the design table", "[synth][mod]")
{
    // Design 9.2: value at +100 % and source 1
    const std::array<float, numModDests> expected {
        0.0f,                     // off
        1.0f, 12.0f, 1.0f, 100.0f, // Osc A position, pitch, level, detune
        1.0f, 12.0f, 1.0f, 100.0f, // Osc B
        1.0f, 1.0f,               // sub, noise level
        60.0f, 1.0f, 24.0f,       // cutoff (semitones), resonance, drive (dB)
        1.0f, 1.0f,               // amp level, pan
        4.0f, 4.0f,               // LFO rates (octaves)
        1.0f, 1.0f, 1.0f,         // chorus / delay / reverb mix
        12.0f                     // pitch (appended)
    };

    for (int d = 0; d < numModDests; ++d)
    {
        CAPTURE (d);
        CHECK (modDestScale (static_cast<ModDest> (d)) == expected[static_cast<std::size_t> (d)]);

        ModDestValues out {};
        ModMatrix::compute (oneSlot (ModSource::lfo1, static_cast<ModDest> (d), 100.0f), allSources (1.0f), out);

        for (int o = 0; o < numModDests; ++o)
            CHECK (out[static_cast<std::size_t> (o)] == (o == d ? expected[static_cast<std::size_t> (d)] : 0.0f));

        // Amount and source value scale linearly, bipolar
        ModMatrix::compute (oneSlot (ModSource::lfo1, static_cast<ModDest> (d), -50.0f), allSources (0.5f), out);
        CHECK_THAT (out[static_cast<std::size_t> (d)], WithinAbs (-0.25 * expected[static_cast<std::size_t> (d)], 1e-5));
    }
}

TEST_CASE ("ModMatrix: each source feeds its own value", "[synth][mod]")
{
    ModSourceValues sources {};

    for (int s = 1; s < numModSources; ++s)
        sources[static_cast<std::size_t> (s)] = 0.01f * static_cast<float> (s);

    for (int s = 0; s < numModSources; ++s)
    {
        ModDestValues out {};
        ModMatrix::compute (oneSlot (static_cast<ModSource> (s), ModDest::filterResonance, 100.0f), sources, out);
        CHECK (out[static_cast<std::size_t> (ModDest::filterResonance)] == (s == 0 ? 0.0f : 0.01f * static_cast<float> (s)));
    }
}

TEST_CASE ("ModMatrix: slots sum per destination, off slots and NaN contribute nothing", "[synth][mod]")
{
    std::array<ModSlotSettings, numModSlots> slots {};
    slots[0] = { ModSource::env2, ModDest::filterCutoff, 50.0f };
    slots[1] = { ModSource::lfo1, ModDest::filterCutoff, 25.0f };
    slots[2] = { ModSource::off, ModDest::filterCutoff, 100.0f };   // source off
    slots[3] = { ModSource::lfo2, ModDest::off, 100.0f };           // dest off
    slots[4] = { ModSource::velocity, ModDest::pan, 0.0f };         // amount 0
    slots[5] = { ModSource::voicePitch, ModDest::pitch, 100.0f / 12.0f };
    slots[6] = { ModSource::note, ModDest::oscAPitch, std::numeric_limits<float>::quiet_NaN() };
    slots[7] = { ModSource::randomPerNote, ModDest::filterCutoff, 100.0f };

    ModSourceValues sources {};
    sources[static_cast<std::size_t> (ModSource::env2)] = 1.0f;
    sources[static_cast<std::size_t> (ModSource::lfo1)] = -1.0f;
    sources[static_cast<std::size_t> (ModSource::lfo2)] = 1.0f;
    sources[static_cast<std::size_t> (ModSource::velocity)] = 1.0f;
    sources[static_cast<std::size_t> (ModSource::voicePitch)] = 0.5f; // +50 cents
    sources[static_cast<std::size_t> (ModSource::note)] = 1.0f;
    sources[static_cast<std::size_t> (ModSource::randomPerNote)] = std::numeric_limits<float>::infinity();

    ModDestValues out {};
    out.fill (99.0f); // overwritten
    ModMatrix::compute (slots, sources, out);
    CHECK_THAT (out[static_cast<std::size_t> (ModDest::filterCutoff)], WithinAbs (30.0 - 15.0, 1e-5));
    CHECK (out[static_cast<std::size_t> (ModDest::off)] == 0.0f);
    CHECK (out[static_cast<std::size_t> (ModDest::pan)] == 0.0f);
    CHECK (out[static_cast<std::size_t> (ModDest::oscAPitch)] == 0.0f);
    // Voice Pitch -> Pitch at 8.33 % = 1:1 cents
    CHECK_THAT (out[static_cast<std::size_t> (ModDest::pitch)], WithinAbs (0.5, 1e-6));

    for (int d = 0; d < numModDests; ++d)
        CHECK (std::isfinite (out[static_cast<std::size_t> (d)]));
}

TEST_CASE ("ModMatrix: global and per-voice destinations are split", "[synth][mod]")
{
    std::array<ModSlotSettings, numModSlots> slots {};
    slots[0] = { ModSource::lfo1, ModDest::lfo2Rate, 100.0f };
    slots[1] = { ModSource::env2, ModDest::reverbMix, 50.0f };
    slots[2] = { ModSource::lfo2, ModDest::oscAPosition, 100.0f };
    slots[3] = { ModSource::velocity, ModDest::chorusMix, 100.0f };
    slots[4] = { ModSource::voiceLevel, ModDest::ampLevel, 100.0f };

    const auto sources = allSources (1.0f);
    ModDestValues all {}, global {}, voice {};
    ModMatrix::compute (slots, sources, all);
    ModMatrix::computeGlobal (slots, sources, global);
    ModMatrix::computePerVoice (slots, sources, voice);

    for (int d = 0; d < numModDests; ++d)
    {
        const auto dest = static_cast<ModDest> (d);
        CAPTURE (d);
        CHECK (global[static_cast<std::size_t> (d)] == (isGlobalDest (dest) ? all[static_cast<std::size_t> (d)] : 0.0f));
        CHECK (voice[static_cast<std::size_t> (d)] == (isGlobalDest (dest) ? 0.0f : all[static_cast<std::size_t> (d)]));
    }

    CHECK (global[static_cast<std::size_t> (ModDest::lfo2Rate)] == 4.0f);
    CHECK (global[static_cast<std::size_t> (ModDest::reverbMix)] == 0.5f);
    CHECK (voice[static_cast<std::size_t> (ModDest::oscAPosition)] == 1.0f);
    CHECK (voice[static_cast<std::size_t> (ModDest::ampLevel)] == 1.0f);

    // The source / destination classification of the design (9.1, 9.3)
    CHECK (isPerVoiceSource (ModSource::env2));
    CHECK (isPerVoiceSource (ModSource::env3));
    CHECK (isPerVoiceSource (ModSource::velocity));
    CHECK (isPerVoiceSource (ModSource::note));
    CHECK (isPerVoiceSource (ModSource::randomPerNote));
    CHECK_FALSE (isPerVoiceSource (ModSource::lfo1));
    CHECK_FALSE (isPerVoiceSource (ModSource::voiceLevel));
    CHECK_FALSE (isPerVoiceSource (ModSource::voicePitch));
    CHECK_FALSE (isGlobalDest (ModDest::pitch));
    CHECK_FALSE (isGlobalDest (ModDest::filterCutoff));
    CHECK (isGlobalDest (ModDest::delayMix));
}
