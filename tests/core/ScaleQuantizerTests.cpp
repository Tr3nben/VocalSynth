#include "vocalsynth/ScaleQuantizer.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <vector>

using namespace vocalsynth;

namespace
{

/** Independent copy of the interval table (docs/M2-DESIGN.md 3.7), so a typo in the header mask fails. */
std::vector<int> intervalsOf (Scale scale)
{
    switch (scale)
    {
        case Scale::chromatic:       return { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
        case Scale::major:           return { 0, 2, 4, 5, 7, 9, 11 };
        case Scale::naturalMinor:    return { 0, 2, 3, 5, 7, 8, 10 };
        case Scale::harmonicMinor:   return { 0, 2, 3, 5, 7, 8, 11 };
        case Scale::majorPentatonic: return { 0, 2, 4, 7, 9 };
        case Scale::minorPentatonic: return { 0, 3, 5, 7, 10 };
        case Scale::blues:           return { 0, 3, 5, 6, 7, 10 };
        case Scale::dorian:          return { 0, 2, 3, 5, 7, 9, 10 };
        case Scale::mixolydian:      return { 0, 2, 4, 5, 7, 9, 10 };
    }

    return {};
}

bool inTable (int note, int key, Scale scale)
{
    const auto pitchClass = ((note - key) % 12 + 12) % 12;

    for (auto i : intervalsOf (scale))
        if (i == pitchClass)
            return true;

    return false;
}

using NoteTable = std::array<bool, 128>;

NoteTable noteTable (int key, Scale scale)
{
    NoteTable table {};

    for (int n = 0; n <= 127; ++n)
        table[static_cast<std::size_t> (n)] = inTable (n, key, scale);

    return table;
}

/** Brute-force reference over all 128 notes with the tie rule (current wins, else lower). */
int referenceQuantize (double p, int currentNote, const NoteTable& allowed)
{
    int best = -1;
    double bestDistance = std::numeric_limits<double>::infinity();

    for (int n = 0; n <= 127; ++n)
    {
        if (! allowed[static_cast<std::size_t> (n)])
            continue;

        const auto d = std::abs (p - static_cast<double> (n));

        if (d < bestDistance - 1e-9)
        {
            best = n;
            bestDistance = d;
        }
        else if (std::abs (d - bestDistance) <= 1e-9 && n == currentNote)
        {
            best = n;
        }
    }

    return best;
}

const std::array<Scale, numScales> allScales { Scale::chromatic,       Scale::major,           Scale::naturalMinor,
                                               Scale::harmonicMinor,   Scale::majorPentatonic, Scale::minorPentatonic,
                                               Scale::blues,           Scale::dorian,          Scale::mixolydian };

} // namespace

TEST_CASE ("ScaleQuantizer: masks match the interval table", "[scalequantizer]")
{
    for (auto scale : allScales)
    {
        uint16_t expected = 0;

        for (auto i : intervalsOf (scale))
            expected = static_cast<uint16_t> (expected | (1u << i));

        CAPTURE (static_cast<int> (scale));
        CHECK (ScaleQuantizer::scaleMask (scale) == expected);
        CHECK ((ScaleQuantizer::scaleMask (scale) & 1u) == 1u); // the key itself is always in the scale
    }

    CHECK (ScaleQuantizer::scaleMask (Scale::chromatic) == 0x0fff);
}

TEST_CASE ("ScaleQuantizer: membership for every scale and key", "[scalequantizer]")
{
    ScaleQuantizer q;

    for (auto scale : allScales)
    {
        for (int key = 0; key < 12; ++key)
        {
            q.setScale (key, scale);
            REQUIRE (q.getKey() == key);
            REQUIRE (q.getScale() == scale);

            for (int note = 0; note <= 127; ++note)
            {
                CAPTURE (static_cast<int> (scale), key, note);
                CHECK (q.isAllowed (note) == inTable (note, key, scale));
            }

            CHECK_FALSE (q.isAllowed (-1));
            CHECK_FALSE (q.isAllowed (128));
        }
    }

    // Spot checks in musical terms
    q.setScale (0, Scale::major); // C major
    CHECK (q.isAllowed (60));
    CHECK_FALSE (q.isAllowed (61));
    CHECK (q.isAllowed (71));
    q.setScale (9, Scale::naturalMinor); // A minor: same notes as C major
    for (int note = 0; note <= 127; ++note)
        CHECK (q.isAllowed (note) == inTable (note, 0, Scale::major));
    q.setScale (2, Scale::blues); // D blues: D F G G# A C
    CHECK (q.isAllowed (62));
    CHECK (q.isAllowed (65));
    CHECK (q.isAllowed (68));
    CHECK_FALSE (q.isAllowed (64));
}

TEST_CASE ("ScaleQuantizer: nearest allowed note for every scale and key", "[scalequantizer]")
{
    ScaleQuantizer q;
    const auto currentNote = GENERATE (-1, 60, 61, 0, 127);
    int mismatches = 0;

    for (auto scale : allScales)
    {
        for (int key = 0; key < 12; ++key)
        {
            q.setScale (key, scale);
            const auto allowed = noteTable (key, scale);

            // Grid of 0.05 semitones over -2..130 hits exact integers, exact midpoints (ties) and both
            // MIDI range edges.
            for (int i = -40; i <= 2600; ++i)
            {
                const auto p = static_cast<double> (i) * 0.05;
                const auto result = q.quantize (p, currentNote);
                const auto expected = referenceQuantize (p, currentNote, allowed);

                // Checked without a Catch assertion per point (1.4 M points); details only on failure.
                if (result != expected || ! q.isAllowed (result))
                {
                    CAPTURE (static_cast<int> (scale), key, p, currentNote, result, expected);
                    FAIL_CHECK ("quantize differs from the brute-force reference");
                    ++mismatches;
                }
            }

            // An allowed note quantises to itself, whatever is sounding
            for (int note = 0; note <= 127; ++note)
                if (q.isAllowed (note) && q.quantize (static_cast<double> (note), currentNote) != note)
                    ++mismatches;
        }
    }

    CHECK (mismatches == 0);
}

TEST_CASE ("ScaleQuantizer: tie goes to the current note, else the lower one", "[scalequantizer]")
{
    ScaleQuantizer q;

    SECTION ("chromatic: exactly half way between two semitones")
    {
        q.setScale (0, Scale::chromatic);
        CHECK (q.quantize (60.5, -1) == 60); // no current note -> lower
        CHECK (q.quantize (60.5, 60) == 60);
        CHECK (q.quantize (60.5, 61) == 61); // current note wins
        CHECK (q.quantize (60.5, 62) == 60); // current note is not one of the two -> lower
        CHECK (q.quantize (60.5 + 1e-6, 60) == 61); // not a tie any more
        CHECK (q.quantize (60.5 - 1e-6, 61) == 60);
        CHECK (q.quantize (60.49, 61) == 60);
        CHECK (q.quantize (60.51, 60) == 61);
    }

    SECTION ("C major: a sung C# lies between C and D")
    {
        q.setScale (0, Scale::major);
        CHECK (q.quantize (61.0, -1) == 60);
        CHECK (q.quantize (61.0, 60) == 60);
        CHECK (q.quantize (61.0, 62) == 62);
        CHECK (q.quantize (61.2, -1) == 62);
        CHECK (q.quantize (60.8, 62) == 60);
        // E-F is a semitone step in C major: the midpoint is 64.5
        CHECK (q.quantize (64.5, -1) == 64);
        CHECK (q.quantize (64.5, 65) == 65);
    }

    SECTION ("pentatonic: E-G gap of 3 semitones, midpoint 65.5")
    {
        q.setScale (0, Scale::majorPentatonic);
        CHECK (q.quantize (65.5, -1) == 64);
        CHECK (q.quantize (65.5, 67) == 67);
        CHECK (q.quantize (65.4, 67) == 64);
        CHECK (q.quantize (65.6, 64) == 67);
        CHECK (q.quantize (66.0, -1) == 67); // F# is nearer to G
        CHECK (q.quantize (65.0, -1) == 64); // F is nearer to E
    }

    SECTION ("transposed key: tie rule independent of the key")
    {
        q.setScale (7, Scale::major); // G major: F# instead of F
        CHECK (q.quantize (65.0, -1) == 64); // F between E and F#
        CHECK (q.quantize (65.0, 66) == 66);
    }
}

TEST_CASE ("ScaleQuantizer: limits 0..127 and invalid input", "[scalequantizer]")
{
    ScaleQuantizer q;

    SECTION ("chromatic")
    {
        q.setScale (0, Scale::chromatic);
        CHECK (q.quantize (-5.0, -1) == 0);
        CHECK (q.quantize (-0.4, -1) == 0);
        CHECK (q.quantize (127.4, -1) == 127);
        CHECK (q.quantize (300.0, -1) == 127);
        CHECK (q.quantize (-1.0e9, -1) == 0);
        CHECK (q.quantize (1.0e9, -1) == 127);
    }

    SECTION ("B major: neither 0 (C) nor 127 (G) is in the scale")
    {
        q.setScale (11, Scale::major); // B C# D# E F# G# A#
        REQUIRE_FALSE (q.isAllowed (0));
        REQUIRE_FALSE (q.isAllowed (127));
        CHECK (q.quantize (0.0, -1) == 1);
        CHECK (q.quantize (-3.0, -1) == 1);
        CHECK (q.quantize (127.0, -1) == 126);
        CHECK (q.quantize (140.0, -1) == 126);
    }

    SECTION ("non-finite pitch returns an allowed note")
    {
        q.setScale (0, Scale::major);
        const auto nan = std::numeric_limits<double>::quiet_NaN();
        const auto inf = std::numeric_limits<double>::infinity();
        CHECK (q.quantize (nan, 64) == 64); // keeps the current note
        CHECK (q.isAllowed (q.quantize (nan, -1)));
        CHECK (q.isAllowed (q.quantize (inf, 61)));
        CHECK (q.isAllowed (q.quantize (-inf, -1)));
    }

    SECTION ("setScale clamps key and scale")
    {
        q.setScale (-3, Scale::major);
        CHECK (q.getKey() == 0);
        q.setScale (15, Scale::major);
        CHECK (q.getKey() == 11);
        q.setScale (0, static_cast<Scale> (42));
        CHECK (q.getScale() == Scale::mixolydian);
        q.setScale (0, static_cast<Scale> (-1));
        CHECK (q.getScale() == Scale::chromatic);
    }

    SECTION ("default is C chromatic")
    {
        const ScaleQuantizer fresh;
        CHECK (fresh.getKey() == 0);
        CHECK (fresh.getScale() == Scale::chromatic);
        for (int note = 0; note <= 127; ++note)
            CHECK (fresh.isAllowed (note));
    }
}
