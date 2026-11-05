// WavetableBuilder: band limits < -120 dB per level, DC 0, peak 1, determinism, thinning, single cycle, PCM16 round trip, mip build benchmark.
// Test plan: docs/SYNTH-DESIGN.md 21. Owner: agent A (Wavetable).

#include "vocalsynth/synth/WavetableBuilder.h"

#include "SynthTestUtils.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstring>
#include <cstdio>
#include <limits>

using namespace vocalsynth::synth;
using namespace vocalsynth::synth::test;

namespace
{
/** Pseudo-random bright frames (every harmonic present, random phases): the hardest case for band limits. */
std::vector<float> randomFrames (int frames, int frameSize, uint32_t seed)
{
    std::vector<float> x (static_cast<std::size_t> (frames) * static_cast<std::size_t> (frameSize));

    for (auto& v : x)
    {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        v = static_cast<float> (static_cast<double> (static_cast<int32_t> (seed)) / 2147483648.0) + 0.3f; // + DC
    }

    return x;
}

bool tablesIdentical (const Wavetable& a, const Wavetable& b)
{
    if (a.getNumFrames() != b.getNumFrames())
        return false;

    for (int f = 0; f < a.getNumFrames(); ++f)
        for (int level = 0; level < Wavetable::numMipLevels; ++level)
        {
            const float* pa = a.getLevel (f, level);
            const float* pb = b.getLevel (f, level);

            for (int i = -1; i < Wavetable::mipLength (level) + 2; ++i)
                if (std::memcmp (&pa[i], &pb[i], sizeof (float)) != 0)
                    return false;
        }

    return true;
}

/** Complex harmonic amplitudes of a stored level (c_k: x (t) = Re sum c_k e^(2 pi i k t)). */
std::vector<std::complex<double>> levelHarmonics (const Wavetable& t, int frame, int level)
{
    const int n = Wavetable::mipLength (level);
    const auto s = spectrum (std::span<const float> (t.getLevel (frame, level), static_cast<std::size_t> (n)));
    std::vector<std::complex<double>> c (s.size());

    for (std::size_t k = 0; k < s.size(); ++k)
        c[k] = (k == 0 ? 1.0 : 2.0) * s[k] / static_cast<double> (n);

    return c;
}
} // namespace

TEST_CASE ("WavetableBuilder: every level is band-limited, DC free, guards copy their sources", "[synth][builder]")
{
    const auto input = randomFrames (3, 2048, 99u);
    const auto result = WavetableBuilder::build ("random", input, 2048);
    REQUIRE (result.ok());
    REQUIRE (result.table->getNumFrames() == 3);
    const Wavetable& t = *result.table;

    for (int f = 0; f < 3; ++f)
        for (int level = 0; level < Wavetable::numMipLevels; ++level)
        {
            const int n = Wavetable::mipLength (level);
            const float* p = t.getLevel (f, level);
            const auto power = powerSpectrum (std::span<const float> (p, static_cast<std::size_t> (n)));
            const int maxHarmonic = std::min (Wavetable::maxHarmonic (level), 1023);
            double strongest = 0.0;
            double above = 0.0;

            for (std::size_t k = 1; k < power.size(); ++k)
            {
                if (static_cast<int> (k) <= maxHarmonic)
                    strongest = std::max (strongest, power[k]);
                else
                    above = std::max (above, power[k]);
            }

            CAPTURE (f, level);
            CHECK (toDb (above / strongest) < -120.0);
            CHECK (toDb (power[0] / strongest) < -120.0); // DC

            double mean = 0.0;

            for (int i = 0; i < n; ++i)
                mean += p[i];

            CHECK (std::abs (mean / n) < 1e-6);

            // Guards: p[-1] = last, p[N] = first, p[N + 1] = second
            CHECK (p[-1] == p[n - 1]);
            CHECK (p[n] == p[0]);
            CHECK (p[n + 1] == p[1]);

            // Every kept harmonic equals the source frame's (the levels are phase-coherent)
            if (level > 0)
            {
                const auto c0 = levelHarmonics (t, f, 0);
                const auto cl = levelHarmonics (t, f, level);
                double maxDiff = 0.0;

                for (int k = 1; k <= maxHarmonic; ++k)
                    maxDiff = std::max (maxDiff, std::abs (c0[static_cast<std::size_t> (k)] - cl[static_cast<std::size_t> (k)]));

                CHECK (maxDiff < 1e-6);
            }
        }

    // The PCM16 frames: DC removed, peak 1, bin 1024 dropped only in the levels
    REQUIRE (result.frames.numFrames == 3);
    REQUIRE (result.frames.samples.size() == 3u * 2048u);
    CHECK (peakAbs (std::span<const float> (result.frames.samples)) == 1.0);

    for (const auto v : result.frames.samples)
        REQUIRE (WavetableBuilder::pcm16ToFloat (WavetableBuilder::quantiseToPcm16 (v)) == v);
}

TEST_CASE ("WavetableBuilder: peak-normalised over the whole table, level-0 peak 1", "[synth][builder]")
{
    // Frame 0: sine at amplitude 0.5, frame 1: sine at 0.25 -> 1.0 and 0.5 after normalisation
    std::vector<float> input;

    for (const double amp : { 0.5, 0.25 })
        for (int i = 0; i < 2048; ++i)
            input.push_back (static_cast<float> (amp * std::sin (2.0 * std::numbers::pi * i / 2048.0)));

    const auto result = WavetableBuilder::build ("sines", input, 2048);
    REQUIRE (result.ok());
    const float* level0 = result.table->getLevel (0, 0);
    CHECK (std::abs (peakAbs (std::span<const float> (level0, 8192)) - 1.0) < 1e-4);
    CHECK (std::abs (peakAbs (std::span<const float> (result.table->getLevel (1, 0), 8192)) - 0.5) < 1e-4);

    for (int level = 1; level < Wavetable::numMipLevels; ++level)
        CHECK (std::abs (peakAbs (std::span<const float> (result.table->getLevel (0, level), static_cast<std::size_t> (Wavetable::mipLength (level)))) - 1.0) < 1e-4);

    CHECK (result.table->getName() == "sines");
    CHECK (result.frames.name == "sines");
}

TEST_CASE ("WavetableBuilder: deterministic, hash of the PCM16 frames", "[synth][builder]")
{
    const auto input = randomFrames (5, 2048, 7u);
    const auto a = WavetableBuilder::build ("a", input, 2048);
    const auto b = WavetableBuilder::build ("b", input, 2048);
    REQUIRE (a.ok());
    REQUIRE (b.ok());
    CHECK (tablesIdentical (*a.table, *b.table));
    CHECK (a.frames.samples == b.frames.samples);

    std::vector<int16_t> pcm (a.frames.samples.size());

    for (std::size_t i = 0; i < pcm.size(); ++i)
        pcm[i] = WavetableBuilder::quantiseToPcm16 (a.frames.samples[i]);

    CHECK (a.table->getContentHash() == WavetableBuilder::fnv1a64 (pcm));
    CHECK (a.table->getContentHash() != 0);

    const auto other = WavetableBuilder::build ("c", randomFrames (5, 2048, 8u), 2048);
    CHECK (other.table->getContentHash() != a.table->getContentHash());
}

TEST_CASE ("WavetableBuilder: a table rebuilt from its PCM16 frames is bit-identical", "[synth][builder]")
{
    for (const int frameSize : { 2048, 512, 0 })
    {
        const auto input = frameSize == 0 ? randomFrames (1, 1500, 3u) : randomFrames (4, frameSize, 3u);
        const auto imported = WavetableBuilder::build ("import.wav", input, frameSize);
        REQUIRE (imported.ok());

        std::vector<int16_t> pcm (imported.frames.samples.size());

        for (std::size_t i = 0; i < pcm.size(); ++i)
            pcm[i] = WavetableBuilder::quantiseToPcm16 (imported.frames.samples[i]);

        const auto rebuilt = WavetableBuilder::buildFromPcm16 ("import.wav", pcm, imported.frames.numFrames);
        REQUIRE (rebuilt.ok());
        CAPTURE (frameSize);
        CHECK (tablesIdentical (*imported.table, *rebuilt.table));
        CHECK (rebuilt.table->getContentHash() == imported.table->getContentHash());
        CHECK (rebuilt.frames.samples == imported.frames.samples);
    }

    // Wrong sizes
    const std::vector<int16_t> pcm (2048 * 2, 100);
    CHECK (WavetableBuilder::buildFromPcm16 ("x", pcm, 3).error == WavetableBuildError::noSamples);
    CHECK (WavetableBuilder::buildFromPcm16 ("x", pcm, 0).error == WavetableBuildError::noSamples);
    CHECK (WavetableBuilder::buildFromPcm16 ("x", {}, 0).error == WavetableBuildError::noSamples);
    CHECK (WavetableBuilder::buildFromPcm16 ("x", pcm, 2).ok());
}

TEST_CASE ("WavetableBuilder: more than 256 frames are thinned evenly", "[synth][builder]")
{
    // Frame j: sine at amplitude 0.2 + 0.8 j / 299 -> after thinning, frame i holds source round (i * 299 / 255)
    constexpr int sourceFrames = 300;
    std::vector<float> input;

    for (int j = 0; j < sourceFrames; ++j)
        for (int i = 0; i < 2048; ++i)
            input.push_back (static_cast<float> ((0.2 + 0.8 * j / 299.0) * std::sin (2.0 * std::numbers::pi * i / 2048.0)));

    const auto result = WavetableBuilder::build ("long", input, 2048);
    REQUIRE (result.ok());
    REQUIRE (result.table->getNumFrames() == 256);

    for (int i = 0; i < 256; ++i)
    {
        const int source = static_cast<int> (std::lround (i * 299.0 / 255.0));
        const double expected = (0.2 + 0.8 * source / 299.0); // the last frame (amplitude 1) sets the peak
        const double amp = std::abs (levelHarmonics (*result.table, i, 10)[1]);
        CAPTURE (i, source);
        CHECK (std::abs (amp - expected) < 1e-4);
    }
}

TEST_CASE ("WavetableBuilder: single cycles of 1000 / 1001 samples and resampled 512 / 4096 frames", "[synth][builder]")
{
    // A saw of any length: harmonics 1..min (1023, len / 2 - 1) with amplitude 1 / k
    for (const int length : { 1000, 1001, 600, 8192 })
    {
        std::vector<float> cycle (static_cast<std::size_t> (length));
        const int top = std::min (1023, length / 2 - 1);

        for (int i = 0; i < length; ++i)
        {
            double v = 0.0;

            for (int k = 1; k <= std::min (top, 200); ++k)
                v += std::sin (2.0 * std::numbers::pi * k * i / length) / k;

            cycle[static_cast<std::size_t> (i)] = static_cast<float> (0.5 * v);
        }

        const auto result = WavetableBuilder::build ("cycle", cycle, 0);
        REQUIRE (result.ok());
        REQUIRE (result.table->getNumFrames() == 1);
        const auto c = levelHarmonics (*result.table, 0, 0);
        const double a1 = std::abs (c[1]);
        CAPTURE (length);

        for (const int k : { 2, 3, 10, 57, 200 })
        {
            CHECK (std::abs (std::abs (c[static_cast<std::size_t> (k)]) * k / a1 - 1.0) < 2e-3);
            CHECK (std::abs (std::arg (c[static_cast<std::size_t> (k)]) - std::arg (c[1])) < 1e-2); // sine phase kept
        }

        for (const int k : { 201, 400, 1000 })
            CHECK (std::abs (c[static_cast<std::size_t> (k)]) / a1 < 1e-3);
    }

    // 512- and 4096-sample frames: the harmonics survive (above 1023 dropped)
    for (const int frameSize : { 512, 4096, 32, 8192 })
    {
        std::vector<float> frames;
        const int high = std::min (frameSize / 2 - 1, 1500);

        for (int f = 0; f < 2; ++f)
            for (int i = 0; i < frameSize; ++i)
            {
                const double t = static_cast<double> (i) / frameSize;
                frames.push_back (static_cast<float> (0.4 * std::sin (2.0 * std::numbers::pi * t)
                                                      + (f + 1) * 0.1 * std::cos (2.0 * std::numbers::pi * 3.0 * t)
                                                      + 0.05 * std::sin (2.0 * std::numbers::pi * high * t)));
            }

        const auto result = WavetableBuilder::build ("resampled", frames, frameSize);
        REQUIRE (result.ok());
        REQUIRE (result.table->getNumFrames() == 2);
        const auto c = levelHarmonics (*result.table, 1, 0);
        CAPTURE (frameSize);
        CHECK (std::abs (std::abs (c[3]) / std::abs (c[1]) - 0.5) < 1e-3);

        if (high <= 1023)
            CHECK (std::abs (std::abs (c[static_cast<std::size_t> (high)]) / std::abs (c[1]) - 0.125) < 1e-3);
        else
            for (std::size_t k = 4; k < c.size(); ++k)
                REQUIRE (std::abs (c[k]) / std::abs (c[1]) < 1e-3);
    }
}

TEST_CASE ("WavetableBuilder: chooseFrameSize", "[synth][builder]")
{
    using B = WavetableBuilder;
    // Override first (if the file holds one such frame), then clm, then multiples of 2048, then single cycle
    CHECK (B::chooseFrameSize (4096, 2048, 1024) == 1024);
    CHECK (B::chooseFrameSize (4096, 0, 256) == 256);
    CHECK (B::chooseFrameSize (500, 0, 1024) == 0);      // override larger than the file: single cycle
    CHECK (B::chooseFrameSize (20000, 512, 0) == 512);   // clm
    CHECK (B::chooseFrameSize (100, 512, 0) == 0);       // clm frame larger than the file
    CHECK (B::chooseFrameSize (2048 * 64, 0, 0) == 2048);
    CHECK (B::chooseFrameSize (2048, 0, 0) == 2048);
    CHECK (B::chooseFrameSize (1000, 0, 0) == 0);
    CHECK (B::chooseFrameSize (8192, 0, 0) == 2048);     // a multiple of 2048 wins over the single cycle
    CHECK (B::chooseFrameSize (8000, 0, 0) == 0);
    CHECK (B::chooseFrameSize (B::maxSingleCycleSamples, 0, 0) == 2048);
    CHECK (B::chooseFrameSize (B::maxSingleCycleSamples + 1, 0, 0) == 2048); // long recording: 2048 frames
    CHECK (B::chooseFrameSize (1000000, 0, 0) == 2048);
    CHECK (B::chooseFrameSize (2, 0, 0) == 0);
    CHECK (B::chooseFrameSize (1, 0, 0) == 2048); // too short for anything: build() reports noSamples
    CHECK (B::chooseFrameSize (0, 0, 0) == 2048);
}

TEST_CASE ("WavetableBuilder: buildFromWav, errors and cancel", "[synth][builder]")
{
    CHECK (WavetableBuilder::build ("x", {}, 2048).error == WavetableBuildError::noSamples);
    const std::vector<float> some (3000, 0.1f);
    CHECK (WavetableBuilder::build ("x", some, 100).error == WavetableBuildError::invalidFrameSize);
    CHECK (WavetableBuilder::build ("x", some, 16).error == WavetableBuildError::invalidFrameSize);
    CHECK (WavetableBuilder::build ("x", some, 16384).error == WavetableBuildError::invalidFrameSize);
    CHECK (WavetableBuilder::build ("x", some, 4096).error == WavetableBuildError::noSamples); // no whole frame
    CHECK (WavetableBuilder::build ("x", std::vector<float> (9000, 0.1f), 0).error == WavetableBuildError::invalidFrameSize);
    CHECK (! WavetableBuilder::build ("x", some, 2048).frames.samples.empty());

    // A silent or DC-only input builds a silent table; NaN / Inf samples count as 0
    auto silent = WavetableBuilder::build ("dc", some, 2048);
    REQUIRE (silent.ok());
    CHECK (peakAbs (std::span<const float> (silent.table->getLevel (0, 0), 8192)) == 0.0);

    std::vector<float> broken (2048, 0.0f);
    broken[3] = std::numeric_limits<float>::quiet_NaN();
    broken[100] = std::numeric_limits<float>::infinity();
    broken[700] = 0.5f;

    for (const int size : { 2048, 1024, 0 })
    {
        const auto result = WavetableBuilder::build ("nan", std::span<const float> (broken).first (size == 1024 ? 1024u : 2048u), size);
        REQUIRE (result.ok());

        for (int level = 0; level < Wavetable::numMipLevels; ++level)
            for (int i = 0; i < Wavetable::mipLength (level); ++i)
                REQUIRE (std::isfinite (result.table->getLevel (0, level)[i]));
    }

    std::atomic<bool> cancel { true };
    CHECK (WavetableBuilder::build ("x", some, 2048, &cancel).error == WavetableBuildError::cancelled);
    CHECK (WavetableBuilder::build ("x", some, 2048, &cancel).table == nullptr);

    WavReadResult wav;
    wav.error = WavReadError::truncated;
    CHECK (WavetableBuilder::buildFromWav ("x", wav, 0).error == WavetableBuildError::readError);

    // A long file without clm: 5 frames of 2048, leftovers ignored; clm 512; override; single cycle
    wav = {};
    wav.samples = randomFrames (1, 5 * 2048 + 100, 5u);
    CHECK (WavetableBuilder::buildFromWav ("x", wav, 0).table->getNumFrames() == 5);
    wav.clmFrameSize = 512;
    CHECK (WavetableBuilder::buildFromWav ("x", wav, 0).table->getNumFrames() == 20);
    CHECK (WavetableBuilder::buildFromWav ("x", wav, 4096).table->getNumFrames() == 2);
    wav.samples.resize (3 * 2048 + 100); // <= 8192 samples without clm: one cycle
    wav.clmFrameSize = 0;
    CHECK (WavetableBuilder::buildFromWav ("x", wav, 0).table->getNumFrames() == 1);
    wav.clmFrameSize = 0;
    wav.samples.resize (777);
    CHECK (WavetableBuilder::buildFromWav ("x", wav, 0).table->getNumFrames() == 1);
    wav.samples.resize (1);
    CHECK (WavetableBuilder::buildFromWav ("x", wav, 0).error == WavetableBuildError::noSamples);
}

TEST_CASE ("WavetableBuilder: makeSineTable, quantise, hash", "[synth][builder]")
{
    const auto sine = WavetableBuilder::makeSineTable();
    REQUIRE (sine != nullptr);
    CHECK (sine->getNumFrames() == 1);

    for (int level = 0; level < Wavetable::numMipLevels; ++level)
    {
        const int n = Wavetable::mipLength (level);
        const float* p = sine->getLevel (0, level);
        const auto power = powerSpectrum (std::span<const float> (p, static_cast<std::size_t> (n)));

        for (std::size_t k = 0; k < power.size(); ++k)
            if (k != 1)
                REQUIRE (toDb (power[k] / power[1]) < -120.0);

        CHECK (p[-1] == p[n - 1]);
        CHECK (p[n] == p[0]);
        CHECK (p[n + 1] == p[1]);
        CHECK (std::abs (p[n / 4] - 1.0f) < 1e-6f);
    }

    CHECK (WavetableBuilder::quantiseToPcm16 (1.0f) == 32767);
    CHECK (WavetableBuilder::quantiseToPcm16 (-1.0f) == -32767);
    CHECK (WavetableBuilder::quantiseToPcm16 (2.0f) == 32767);
    CHECK (WavetableBuilder::quantiseToPcm16 (std::numeric_limits<float>::quiet_NaN()) == 0);
    CHECK (WavetableBuilder::quantiseToPcm16 (0.5f) == 16384);
    CHECK (WavetableBuilder::fnv1a64 ({}) == 14695981039346656037ull);
    const int16_t one[] = { 1 };
    CHECK (WavetableBuilder::fnv1a64 (one) != WavetableBuilder::fnv1a64 ({}));
}

TEST_CASE ("WavetableBuilder: benchmark, 256-frame mip build", "[synth][builder][!benchmark]")
{
    const auto input = randomFrames (256, 2048, 11u);
    double best = 1e9;

    for (int run = 0; run < 3; ++run)
    {
        const auto start = std::chrono::steady_clock::now();
        const auto result = WavetableBuilder::build ("big", input, 2048);
        best = std::min (best, std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - start).count());
        REQUIRE (result.ok());
    }

    std::printf ("WavetableBuilder: 256 frames in %.1f ms (%.1f MB)\n", best, 256.0 * Wavetable::frameStride * 4.0 / 1e6);
    CHECK (best < 150.0);
}
