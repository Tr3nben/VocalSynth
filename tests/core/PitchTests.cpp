#include "vocalsynth/Pitch.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using Catch::Matchers::WithinAbs;
using namespace vocalsynth;

TEST_CASE ("A4 is MIDI note 69 at 440 Hz", "[pitch]")
{
    CHECK_THAT (frequencyToMidiNote (440.0), WithinAbs (69.0, 1e-12));
    CHECK_THAT (midiNoteToFrequency (69.0), WithinAbs (440.0, 1e-12));
    CHECK (nearestMidiNote (440.0) == 69);
}

TEST_CASE ("Octaves are 12 semitones apart", "[pitch]")
{
    CHECK_THAT (frequencyToMidiNote (220.0), WithinAbs (57.0, 1e-12));
    CHECK_THAT (frequencyToMidiNote (880.0), WithinAbs (81.0, 1e-12));
    CHECK_THAT (midiNoteToFrequency (60.0), WithinAbs (261.6255653005986, 1e-9)); // middle C
}

TEST_CASE ("Note/frequency conversion round-trips over the vocal range", "[pitch]")
{
    const auto note = GENERATE (28.0, 40.0, 52.5, 60.0, 69.0, 84.25, 96.0);
    CHECK_THAT (frequencyToMidiNote (midiNoteToFrequency (note)), WithinAbs (note, 1e-9));
}

TEST_CASE ("Cents offset from a note", "[pitch]")
{
    CHECK_THAT (centsFromNote (440.0, 69), WithinAbs (0.0, 1e-9));
    CHECK_THAT (centsFromNote (midiNoteToFrequency (69.25), 69), WithinAbs (25.0, 1e-9));
    CHECK_THAT (centsFromNote (midiNoteToFrequency (68.6), 69), WithinAbs (-40.0, 1e-9));
}

TEST_CASE ("Nearest note rounds to the closest semitone and clamps to MIDI range", "[pitch]")
{
    CHECK (nearestMidiNote (midiNoteToFrequency (69.49)) == 69);
    CHECK (nearestMidiNote (midiNoteToFrequency (69.51)) == 70);
    CHECK (nearestMidiNote (1.0) == 0);
    CHECK (nearestMidiNote (100000.0) == 127);
}

TEST_CASE ("Unvoiced input (0 or negative Hz) maps to note 0", "[pitch]")
{
    CHECK (frequencyToMidiNote (0.0) == 0.0);
    CHECK (frequencyToMidiNote (-10.0) == 0.0);
    CHECK (nearestMidiNote (0.0) == 0);
}
