// FactoryWavetables: 14 ids unique, fromId round trip, every table builds, frame counts, determinism, build time.
// Test plan: docs/SYNTH-DESIGN.md 21. Owner: agent A (Wavetable).

#include "vocalsynth/synth/FactoryWavetables.h"

#include "SynthTestUtils.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>
#include <set>
#include <string>

using namespace vocalsynth::synth;
using namespace vocalsynth::synth::test;

TEST_CASE ("FactoryWavetables: ids and names are unique and permanent, fromId round trip", "[synth][factory]")
{
    // The ids are saved in projects and presets ("factory:<id>"): this list must never change, only grow.
    const char* expected[numFactoryTables] = { "basic-shapes", "pwm", "harmonic-sweep", "saw-bright-dark",
                                               "formant-vowel", "sync-sweep", "fm-2op", "fold", "organ", "digital",
                                               "spectral-random", "bell", "square-stack", "choir" };
    std::set<std::string> ids, names;

    for (int i = 0; i < numFactoryTables; ++i)
    {
        const auto table = static_cast<FactoryTable> (i);
        const auto& info = FactoryWavetables::getInfo (table);
        CHECK (info.id == expected[i]);
        CHECK (! info.displayName.empty());
        CHECK (info.numFrames >= 1);
        CHECK (info.numFrames <= Wavetable::maxFrames);
        ids.insert (std::string (info.id));
        names.insert (std::string (info.displayName));
        REQUIRE (FactoryWavetables::fromId (info.id).has_value());
        CHECK (*FactoryWavetables::fromId (info.id) == table);
    }

    CHECK (ids.size() == static_cast<std::size_t> (numFactoryTables));
    CHECK (names.size() == static_cast<std::size_t> (numFactoryTables));
    CHECK (! FactoryWavetables::fromId ("").has_value());
    CHECK (! FactoryWavetables::fromId ("Basic Shapes").has_value());
    CHECK (! FactoryWavetables::fromId ("basic-shapes ").has_value());
    CHECK (FactoryWavetables::getInfo (static_cast<FactoryTable> (99)).id == "basic-shapes");
    CHECK (defaultFactoryTable == FactoryTable::basicShapes);
    CHECK (FactoryWavetables::getInfo (defaultFactoryTable).displayName == "Basic Shapes");
}

TEST_CASE ("FactoryWavetables: every table builds, deterministic, normalised, not silent", "[synth][factory]")
{
    for (int i = 0; i < numFactoryTables; ++i)
    {
        const auto id = static_cast<FactoryTable> (i);
        const auto& info = FactoryWavetables::getInfo (id);
        CAPTURE (info.id);

        const auto frames = FactoryWavetables::generateFrames (id);
        CHECK (frames.size() == static_cast<std::size_t> (info.numFrames) * Wavetable::frameSize);
        CHECK (frames == FactoryWavetables::generateFrames (id)); // deterministic (seeded generators)

        for (const auto v : frames)
            REQUIRE (std::isfinite (v));

        const auto result = FactoryWavetables::build (id);
        REQUIRE (result.ok());
        CHECK (result.table->getNumFrames() == info.numFrames);
        CHECK (result.table->getName() == info.displayName);
        CHECK (peakAbs (std::span<const float> (result.frames.samples)) == 1.0);
        CHECK (result.table->getContentHash() == FactoryWavetables::build (id).table->getContentHash());

        // Every frame sounds
        for (int f = 0; f < info.numFrames; ++f)
            REQUIRE (rms (std::span<const float> (result.table->getLevel (f, 0), 8192)) > 0.01);
    }
}

TEST_CASE ("FactoryWavetables: recipes", "[synth][factory]")
{
    const auto harmonics = [] (const Wavetable& t, int frame)
    {
        const auto p = powerSpectrum (std::span<const float> (t.getLevel (frame, 0), 8192));
        std::vector<double> db (p.size());

        for (std::size_t k = 0; k < p.size(); ++k)
            db[k] = toDb (p[k] / p[1]);

        return db;
    };

    // Basic Shapes: sine at 0 %, triangle (odd 1 / k^2) at 25 %, saw at 50 %, square at 75 %
    const auto basic = FactoryWavetables::build (FactoryTable::basicShapes);
    CHECK (harmonics (*basic.table, 0)[2] < -100.0);
    CHECK (harmonics (*basic.table, 0)[3] < -100.0);
    const int last = basic.table->getNumFrames() - 1;
    const auto quarter = [&] (int q) { return static_cast<int> (std::lround (q * last / 4.0)); };
    // Frames are evenly spaced, so 25 / 50 / 75 % are only exact keyframes for 4 * n + 1 frames: check the
    // nearest frames loosely (the crossfade is linear in the spectra)
    CHECK (std::abs (harmonics (*basic.table, quarter (2))[2] - toDb (0.25)) < 1.0);    // saw: 1 / k
    CHECK (harmonics (*basic.table, quarter (3))[2] < harmonics (*basic.table, quarter (2))[2] - 10.0); // square: odd

    // Saw Bright-Dark: the last frame falls 12 dB per octave faster than the first
    const auto tilt = FactoryWavetables::build (FactoryTable::sawBrightDark);
    const auto bright = harmonics (*tilt.table, 0);
    const auto dark = harmonics (*tilt.table, tilt.table->getNumFrames() - 1);
    CHECK (std::abs (bright[8] - toDb (1.0 / 64.0)) < 0.5);
    CHECK (std::abs ((dark[8] - bright[8]) - (-36.0)) < 0.5); // 3 octaves x -12 dB

    // Organ: drawbar harmonics only (1, 2, 3, 4, 6, 8, 10, 12, 16)
    const auto organ = FactoryWavetables::build (FactoryTable::organ);
    const auto reg = harmonics (*organ.table, 0); // 888000000: 16', 5 1/3', 8' equal
    CHECK (std::abs (reg[2]) < 0.1);
    CHECK (std::abs (reg[3]) < 0.1);
    CHECK (reg[5] < -100.0);
    CHECK (reg[4] < -100.0);

    // FM 2-Op: index 0 at the first frame is a pure sine
    const auto fm = FactoryWavetables::build (FactoryTable::fm2Op);
    CHECK (harmonics (*fm.table, 0)[3] < -100.0);
    CHECK (harmonics (*fm.table, fm.table->getNumFrames() - 1)[5] > -40.0);
}

TEST_CASE ("FactoryWavetables: benchmark, every table builds in < 150 ms", "[synth][factory][!benchmark]")
{
    for (int i = 0; i < numFactoryTables; ++i)
    {
        const auto id = static_cast<FactoryTable> (i);
        double best = 1e9;

        for (int run = 0; run < 3; ++run)
        {
            const auto start = std::chrono::steady_clock::now();
            const auto result = FactoryWavetables::build (id);
            best = std::min (best, std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - start).count());
            REQUIRE (result.ok());
        }

        std::printf ("FactoryWavetables %-16s %3d frames %6.1f ms %5.1f MB\n",
                     std::string (FactoryWavetables::getInfo (id).id).c_str(), FactoryWavetables::getInfo (id).numFrames,
                     best, FactoryWavetables::getInfo (id).numFrames * Wavetable::frameStride * 4.0 / 1e6);
        CHECK (best < 150.0);
    }
}
