// WavetableOscillator: frequency +-0.1 ct, alias test, unison detune / spread, morph and mip switch without jumps, table crossfade, benchmark.
// Test plan: docs/SYNTH-DESIGN.md 21. Owner: agent A (Wavetable).

#include "vocalsynth/synth/WavetableOscillator.h"

#include "vocalsynth/synth/FactoryWavetables.h"
#include "SynthTestUtils.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <type_traits>

using namespace vocalsynth::synth;
using namespace vocalsynth::synth::test;

namespace
{
constexpr std::size_t aliasFftSize = 65536;
constexpr double rates[] = { 44100.0, 48000.0, 96000.0 };

bool verbose()
{
    return std::getenv ("VOCALSYNTH_TEST_VERBOSE") != nullptr;
}

OscillatorTick toneTick (double increment, float position = 0.0f)
{
    OscillatorTick t;
    t.increment = increment;
    t.position = position;
    t.level = 1.0f;
    return t;
}

/** A started oscillator playing a constant tick target for numSamples (ticks every 16 samples). */
Stereo renderTone (const Wavetable* table, double sampleRate, const OscillatorTick& tick, int numSamples,
                   uint32_t seed = 1, float phaseRandom = 0.0f)
{
    WavetableOscillator osc;
    osc.prepare (sampleRate);
    osc.setTable (table);
    osc.restart (seed, phaseRandom);
    Stereo out (static_cast<std::size_t> (numSamples));
    runTicks (
        numSamples, {}, [&] (int) { osc.beginTick (tick, controlTickSamples); },
        [&] (int offset, int n) { osc.render (out.left.data() + offset, out.right.data() + offset, n); });
    return out;
}

/** Alias gate measure (21): f0 on an exact bin (m odd), rectangular window, steady state after the first tick. */
double aliasDb (const Wavetable& table, double sampleRate, double f0, float position = 0.0f, bool relativeToTotal = false)
{
    const auto m = static_cast<std::size_t> (std::llround (f0 * static_cast<double> (aliasFftSize) / sampleRate)) | 1u;
    const double increment = static_cast<double> (m) / static_cast<double> (aliasFftSize);
    const auto out = renderTone (&table, sampleRate, toneTick (increment, position),
                                 static_cast<int> (aliasFftSize) + controlTickSamples);
    const auto power = powerSpectrum (std::span<const float> (out.left).subspan (controlTickSamples, aliasFftSize));
    double harmonic = 0.0;

    for (std::size_t k = m; k < power.size(); k += m)
        harmonic += power[k];

    // A frame whose harmonics all lie above the level's band (sync ratio 8 at a level with 4 harmonics) is
    // exactly silent there: nothing to alias.
    if (relativeToTotal && harmonic == 0.0)
        return peakAbs (std::span<const float> (out.left)) == 0.0 ? -400.0 : 400.0;

    return nonHarmonicDb (power, m, relativeToTotal ? harmonic : -1.0);
}

std::vector<double> logSpaced (double from, double to, int count)
{
    std::vector<double> v;

    for (int i = 0; i < count; ++i)
        v.push_back (from * std::pow (to / from, static_cast<double> (i) / static_cast<double> (count - 1)));

    return v;
}

/** Click measure (21): worst over all 1024-sample Hann windows (hop 256) of the energy of render - reference in
    the bins below 0.2 fs, relative to the reference's energy in that window (dB). */
double worstClickDb (std::span<const float> render, std::span<const double> reference, std::size_t skipStart = 0)
{
    constexpr std::size_t n = 1024;
    const auto w = hannWindow (n);
    const std::size_t maxBin = static_cast<std::size_t> (0.2 * static_cast<double> (n));
    double worst = -400.0;

    for (std::size_t start = skipStart; start + n <= render.size(); start += 256)
    {
        std::vector<double> diff (n), ref (n);

        for (std::size_t i = 0; i < n; ++i)
        {
            diff[i] = static_cast<double> (render[start + i]) - reference[start + i];
            ref[i] = reference[start + i];
        }

        const auto pd = powerSpectrum (std::span<const double> (diff), std::span<const double> (w));
        const auto pr = powerSpectrum (std::span<const double> (ref), std::span<const double> (w));
        double err = 0.0;
        double total = 0.0;

        for (std::size_t k = 0; k < pd.size(); ++k)
        {
            if (k < maxBin)
                err += pd[k];

            total += pr[k];
        }

        worst = std::max (worst, toDb (err / total));

        if (verbose() && toDb (err / total) > -60.0)
            std::printf ("  click window at %zu: %.1f dB\n", start, toDb (err / total));
    }

    return worst;
}

/** Weight of a reference harmonic at k * increment cycles per sample: all harmonics below 0.45 fs, faded
    out smoothly (raised cosine) from 0.30 to 0.45 fs. A hard cut would switch a harmonic off within one
    sample when a rising glide carries it across 0.45 fs; that step splatters into the measured band below
    0.2 fs and would be blamed on the oscillator. Above 0.2 fs the reference is not compared anyway. */
double referenceWeight (double cyclesPerSample)
{
    if (cyclesPerSample <= 0.30)
        return 1.0;

    if (cyclesPerSample >= 0.45)
        return 0.0;

    return 0.5 + 0.5 * std::cos (std::numbers::pi * (cyclesPerSample - 0.30) / 0.15);
}

/** Additive reference sum_k w_k Re (c_k e^(2 pi i k phase)) over the harmonics below 0.45 fs, c_k interpolated
    between two frames (frac) like the oscillator's position. */
double additive (const std::vector<std::complex<double>>& a, const std::vector<std::complex<double>>& b, double frac,
                 double phase, double increment)
{
    const std::complex<double> rotor (std::cos (2.0 * std::numbers::pi * phase), std::sin (2.0 * std::numbers::pi * phase));
    std::complex<double> e = rotor;
    double sum = 0.0;

    for (std::size_t k = 1; k < a.size() && static_cast<double> (k) * increment < 0.45; ++k)
    {
        const auto c = (a[k] + (b[k] - a[k]) * frac) * referenceWeight (static_cast<double> (k) * increment);
        sum += c.real() * e.real() - c.imag() * e.imag();
        e = { e.real() * rotor.real() - e.imag() * rotor.imag(), e.real() * rotor.imag() + e.imag() * rotor.real() };
    }

    return sum;
}

double additive (const std::vector<std::complex<double>>& c, double phase, double increment)
{
    return additive (c, c, 0.0, phase, increment);
}

/** The oscillator's centre gain for unison 1, pan 0, level 1 (same float operations as the cache). */
float centreGain()
{
    return 1.0f * static_cast<float> (std::numbers::sqrt2 * std::cos (std::numbers::pi / 4.0));
}

const Wavetable& factoryTable (FactoryTable id)
{
    static std::array<std::shared_ptr<const Wavetable>, numFactoryTables> cache;
    auto& slot = cache[static_cast<std::size_t> (id)];

    if (slot == nullptr)
        slot = std::shared_ptr<const Wavetable> (FactoryWavetables::build (id).table);

    return *slot;
}
} // namespace

//==============================================================================
TEST_CASE ("WavetableOscillator: real-time structure (no heap state, noexcept audio API)", "[synth][oscillator]")
{
    // No owned heap memory at all: nothing in render / beginTick / setTable can allocate or free.
    STATIC_REQUIRE (std::is_trivially_destructible_v<WavetableOscillator>);
    STATIC_REQUIRE (std::is_trivially_copyable_v<WavetableOscillator>);

    WavetableOscillator osc;
    float l = 0.0f, r = 0.0f;
    STATIC_REQUIRE (noexcept (osc.render (&l, &r, 1)));
    STATIC_REQUIRE (noexcept (osc.beginTick (OscillatorTick {}, 16)));
    STATIC_REQUIRE (noexcept (osc.setTable (nullptr)));
    STATIC_REQUIRE (noexcept (osc.restart (1u, 0.5f)));
    STATIC_REQUIRE (noexcept (osc.getTablesInUse()));
    STATIC_REQUIRE (noexcept (osc.reset()));
}

TEST_CASE ("WavetableOscillator: silence without a table, before a note and after reset", "[synth][oscillator]")
{
    WavetableOscillator osc;
    osc.prepare (48000.0);
    Stereo out (256);

    // No table: exact silence
    osc.restart (1, 0.0f);
    runTicks (256, {}, [&] (int) { osc.beginTick (toneTick (0.01), 16); },
              [&] (int o, int n) { osc.render (out.left.data() + o, out.right.data() + o, n); });
    CHECK (peakAbs (std::span<const float> (out.left)) == 0.0);
    CHECK (peakAbs (std::span<const float> (out.right)) == 0.0);

    // A table, sounding, then reset(): every pointer forgotten
    osc.setTable (&sawTable());
    runTicks (256, {}, [&] (int) { osc.beginTick (toneTick (0.01), 16); },
              [&] (int o, int n) { osc.render (out.left.data() + o, out.right.data() + o, n); });
    CHECK (peakAbs (std::span<const float> (out.left)) > 0.1);
    osc.reset();
    CHECK (osc.getTablesInUse().current == nullptr);
    CHECK (osc.getTablesInUse().previous == nullptr);

    std::fill (out.left.begin(), out.left.end(), 0.0f);
    runTicks (256, {}, [&] (int) { osc.beginTick (toneTick (0.01), 16); },
              [&] (int o, int n) { osc.render (out.left.data() + o, out.right.data() + o, n); });
    CHECK (peakAbs (std::span<const float> (out.left)) == 0.0);

    // Increment above 0.45 (fundamental above 0.45 fs): silent after the ramp-out tick
    const auto high = renderTone (&sawTable(), 48000.0, toneTick (0.46), 512);
    CHECK (peakAbs (std::span<const float> (high.left).subspan (16)) == 0.0);
}

TEST_CASE ("WavetableOscillator: frequency within 0.1 cent from 20 Hz to 8 kHz", "[synth][oscillator]")
{
    for (const double fs : { 44100.0, 96000.0 })
        for (const double f0 : logSpaced (20.0, 8000.0, 13))
        {
            for (const Wavetable* table : { &sineTable(), &sawTable() })
            {
                const auto out = renderTone (table, fs, toneTick (f0 / fs), 65536 + 1024 + 16);
                const double measured = measureFrequency (std::span<const float> (out.left).subspan (16), fs);
                CAPTURE (fs, f0, measured, table->getName());
                CHECK (std::abs (cents (measured, f0)) < 0.1);
            }
        }
}

TEST_CASE ("WavetableOscillator: Hermite read and frame interpolation follow the binding formula", "[synth][oscillator]")
{
    // Position 0.3 of the 2-frame saw -> square table at a fixed level: compare with the formula evaluated
    // directly on the stored samples.
    const Wavetable& table = sawToSquareTable();
    const double increment = 1.0 / 3000.3; // level 0 (1024 * inc <= 0.45)
    const auto out = renderTone (&table, 48000.0, toneTick (increment, 0.3f), 4096);
    const float gain = centreGain();
    const float* a = table.getLevel (0, 0);
    const float* b = table.getLevel (1, 0);
    double phase = 0.0;
    double worst = 0.0;

    for (int s = 0; s < 4096; ++s)
    {
        if (s >= 16)
        {
            const double x = phase * 8192.0;
            const int i = static_cast<int> (x);
            const double f = x - i;
            const auto h = [&] (const float* p)
            {
                const double xm1 = p[i - 1], x0 = p[i], x1 = p[i + 1], x2 = p[i + 2];
                const double c1 = 0.5 * (x1 - xm1);
                const double c2 = xm1 - 2.5 * x0 + 2.0 * x1 - 0.5 * x2;
                const double c3 = 0.5 * (x2 - xm1) + 1.5 * (x0 - x1);
                return ((c3 * f + c2) * f + c1) * f + x0;
            };
            const double expected = (h (a) + (h (b) - h (a)) * 0.3) * gain;
            worst = std::max (worst, std::abs (expected - static_cast<double> (out.left[static_cast<std::size_t> (s)])));
        }

        phase += increment;

        if (phase >= 1.0)
            phase -= 1.0;
    }

    CHECK (worst < 2e-6);
    CHECK (out.left == out.right); // pan 0: both channels equal
}

//==============================================================================
TEST_CASE ("WavetableOscillator: alias gate, saw sweep at 44.1 / 48 / 96 kHz", "[synth][oscillator][alias]")
{
    // >= 60 log-spaced f0 per rate (20 Hz .. 5 kHz), f0 on exact bins, rectangular window: all non-harmonic
    // energy <= -60 dB re the fundamental up to 5 kHz and <= -70 dB up to 1 kHz (docs/SYNTH-DESIGN.md 21).
    for (const double fs : rates)
    {
        double worstTo1k = -400.0;
        double worstTo5k = -400.0;

        for (const double f0 : logSpaced (20.0, 5000.0, 64))
        {
            const double db = aliasDb (sawTable(), fs, f0);
            CAPTURE (fs, f0, db);
            CHECK (db <= (f0 <= 1000.0 ? -70.0 : -60.0));
            (f0 <= 1000.0 ? worstTo1k : worstTo5k) = std::max (f0 <= 1000.0 ? worstTo1k : worstTo5k, db);
        }

        if (verbose())
            std::printf ("alias saw %.0f Hz: worst <= 1 kHz %.1f dB, 1..5 kHz %.1f dB\n", fs, worstTo1k, worstTo5k);
    }
}

TEST_CASE ("WavetableOscillator: alias gate, every factory table", "[synth][oscillator][alias]")
{
    // Every factory table at 44.1 / 48 / 96 kHz. The non-harmonic energy of a level does not depend on f0 inside
    // the level's range (it is the Hermite images of the level's harmonics), so every level whose range lies in
    // 20 Hz .. 5 kHz is measured once in the middle of its range (0.35 fs / maxHarmonic), at positions 0, 0.5
    // and 1 (at 48 kHz also 0.25 and 0.75). Same limits as the saw, but relative to the total harmonic power:
    // several frames have no fundamental at all (sync ratio 2..8 repeats a saw, an organ registration without
    // 16'), so "below the fundamental" is only defined for the saw sweep above (both differ by 2.2 dB there).
    for (int t = 0; t < numFactoryTables; ++t)
    {
        const auto id = static_cast<FactoryTable> (t);
        const Wavetable& table = factoryTable (id);
        double worstTo1k = -400.0;
        double worstTo5k = -400.0;

        for (const double fs : rates)
            for (int level = 0; level < Wavetable::numMipLevels; ++level)
            {
                const double f0 = 0.35 * fs / Wavetable::maxHarmonic (level);

                if (f0 < 20.0 || f0 > 5000.0)
                    continue;

                for (const float position : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
                {
                    if (fs != 48000.0 && (position == 0.25f || position == 0.75f))
                        continue;

                    const double db = aliasDb (table, fs, f0, position, true);
                    CAPTURE (FactoryWavetables::getInfo (id).id, fs, f0, level, position, db);
                    CHECK (db <= (f0 <= 1000.0 ? -70.0 : -60.0));
                    (f0 <= 1000.0 ? worstTo1k : worstTo5k) = std::max (f0 <= 1000.0 ? worstTo1k : worstTo5k, db);
                }
            }

        if (verbose())
            std::printf ("alias %-16s worst <= 1 kHz %.1f dB, 1..5 kHz %.1f dB\n",
                         std::string (FactoryWavetables::getInfo (id).id).c_str(), worstTo1k, worstTo5k);
    }
}

//==============================================================================
TEST_CASE ("WavetableOscillator: unison detune distribution, level and stereo spread", "[synth][oscillator]")
{
    constexpr double fs = 48000.0;
    constexpr double f0 = 1000.0;
    constexpr std::size_t n = 65536;

    for (const int voices : { 2, 3, 8 })
        for (const float spread : { 0.0f, 1.0f, 0.4f })
        {
            OscillatorTick tick = toneTick (f0 / fs);
            tick.unison = voices;
            tick.detuneCents = 100.0f;
            tick.spread = spread;
            tick.pan = spread == 0.4f ? 0.3f : 0.0f;
            const auto out = renderTone (&sineTable(), fs, tick, static_cast<int> (n) + 32);
            const auto w = hannWindow (n);
            double windowSum = 0.0;

            for (const auto v : w)
                windowSum += v;

            for (int i = 0; i < voices; ++i)
            {
                // Expected: detune * (i / (n - 1) - 0.5) cents, pan p_i, equal-power gains / sqrt (n)
                const double offset = 100.0 * (static_cast<double> (i) / (voices - 1) - 0.5);
                const double f = f0 * std::exp2 (offset / 1200.0);
                const double p = std::clamp (static_cast<double> (tick.pan) + spread * (2.0 * i / (voices - 1) - 1.0), -1.0, 1.0);
                const double gl = std::numbers::sqrt2 * std::cos ((p + 1.0) * std::numbers::pi / 4.0) / std::sqrt (voices);
                const double gr = std::numbers::sqrt2 * std::sin ((p + 1.0) * std::numbers::pi / 4.0) / std::sqrt (voices);

                // Amplitude at exactly f (windowed DTFT), both channels
                const auto amplitude = [&] (const std::vector<float>& x)
                {
                    std::complex<double> acc;
                    const double omega = 2.0 * std::numbers::pi * f / fs;

                    for (std::size_t s = 0; s < n; ++s)
                        acc += static_cast<double> (x[s + 32]) * w[s]
                               * std::complex<double> (std::cos (omega * static_cast<double> (s)), -std::sin (omega * static_cast<double> (s)));

                    return 2.0 * std::abs (acc) / windowSum;
                };

                CAPTURE (voices, spread, i, f, gl, gr);
                CHECK (std::abs (amplitude (out.left) - gl) < 2e-3);
                CHECK (std::abs (amplitude (out.right) - gr) < 2e-3);
            }

            // The strongest component of a 2-voice unison lies at +-50 cents
            if (voices == 2 && spread == 1.0f)
            {
                const double fl = measureFrequency (std::span<const float> (out.left).subspan (32, n - 2048), fs, 32768);
                const double fr = measureFrequency (std::span<const float> (out.right).subspan (32, n - 2048), fs, 32768);
                CHECK (std::abs (cents (fl, f0) + 50.0) < 0.1);
                CHECK (std::abs (cents (fr, f0) - 50.0) < 0.1);
            }
        }

    // Unison 1: one voice at the centre, no detune, pan law 1 / 1 at the centre
    OscillatorTick one = toneTick (f0 / fs);
    one.detuneCents = 100.0f;
    one.spread = 1.0f;
    const auto mono = renderTone (&sineTable(), fs, one, 65536 + 1100);
    CHECK (std::abs (cents (measureFrequency (std::span<const float> (mono.left).subspan (32), fs), f0)) < 0.01);
    CHECK (std::abs (peakAbs (std::span<const float> (mono.left).subspan (32)) - 1.0) < 1e-3);
    CHECK (mono.left == mono.right);
}

TEST_CASE ("WavetableOscillator: start phases follow phaseRandom and the seed", "[synth][oscillator]")
{
    OscillatorTick tick = toneTick (0.001);
    tick.unison = 4;
    tick.detuneCents = 0.0f;

    const auto a = renderTone (&sineTable(), 48000.0, tick, 256, 7u, 0.0f);
    const auto b = renderTone (&sineTable(), 48000.0, tick, 256, 7u, 1.0f);
    const auto c = renderTone (&sineTable(), 48000.0, tick, 256, 7u, 1.0f);
    const auto d = renderTone (&sineTable(), 48000.0, tick, 256, 8u, 1.0f);

    // phaseRandom 0: all voices in phase -> the sum is sqrt (4) times one voice (sine from phase 0)
    const double expected = 2.0 * std::sin (2.0 * std::numbers::pi * 0.001 * 100.0) * centreGain();
    CHECK (std::abs (a.left[100] - expected) < 1e-4);
    CHECK (b.left == c.left);  // deterministic
    CHECK (b.left != d.left);  // seed matters
    CHECK (b.left != a.left);  // phaseRandom matters
}

//==============================================================================
TEST_CASE ("WavetableOscillator: mip level choice with hysteresis", "[synth][oscillator]")
{
    WavetableOscillator osc;
    osc.prepare (48000.0);
    osc.setTable (&sawTable());
    osc.restart (1, 0.0f);
    std::array<float, 16> l {}, r {};
    const auto tickAt = [&] (double h0) // 1024 * increment
    {
        osc.beginTick (toneTick (h0 / 1024.0), 16);
        osc.render (l.data(), r.data(), 16);
        return osc.getMipLevel();
    };

    CHECK (tickAt (0.40) == 0);
    CHECK (tickAt (0.44) == 0);
    CHECK (tickAt (0.46) == 1);  // above 0.45: up
    CHECK (tickAt (0.42) == 1);  // between 0.40 and 0.45: stays (the ramp started at 0.46 anyway)
    CHECK (tickAt (0.42) == 1);  // still above 0.40: stays
    CHECK (tickAt (0.39) == 1);  // the ramp starts at 0.42 -> max 0.42 > 0.40
    CHECK (tickAt (0.39) == 0);  // constant 0.39 <= 0.40: down
    CHECK (tickAt (3.0) == 3);   // 128 * 3 / 1024 = 0.375
    CHECK (tickAt (100.0) == 8); // 4 * 100 / 1024 = 0.39
    CHECK (tickAt (400.0) == 10);
    CHECK (tickAt (0.2) == 10);  // the ramp starts at 400 / 1024
    CHECK (tickAt (0.2) == 0);   // a big step down goes straight to the right level
}

//==============================================================================
TEST_CASE ("WavetableOscillator: no click during a glide 40 Hz -> 10 kHz with a position morph", "[synth][oscillator][click]")
{
    // (and the same glide downwards, which switches the levels the other way, at the 0.40 hysteresis point)
    // The test drives beginTick itself and builds an additive reference with the same phase accumulation
    // (all harmonics below 0.45 fs, the frames' spectra interpolated like the position): energy of render -
    // reference below 0.2 fs <= -60 dB of the reference in every 1024-sample Hann window (docs/SYNTH-DESIGN.md 21).
    const Wavetable& table = sawToSquareTable();
    const auto cSaw = frameHarmonics (table, 0);
    const auto cSquare = frameHarmonics (table, 1);

    for (const bool falling : { false, true })
        for (const double fs : { 44100.0, 48000.0 })
        {
            const int numSamples = static_cast<int> (2.0 * fs);
            const auto targetAt = [&] (int tick)
            {
                const double t = std::min (2.0, static_cast<double> ((tick + 1) * controlTickSamples) / fs);
                const double x = falling ? 1.0 - t / 2.0 : t / 2.0;
                return toneTick (40.0 * std::pow (250.0, x) / fs, static_cast<float> (x));
            };

            WavetableOscillator osc;
            osc.prepare (fs);
            osc.setTable (&table);
            osc.restart (1, 0.0f);
            Stereo out (static_cast<std::size_t> (numSamples));
            int levelChanges = 0;
            int lastLevel = -1;
            runTicks (
                numSamples, {}, [&] (int tick) { osc.beginTick (targetAt (tick), controlTickSamples); },
                [&] (int o, int n)
                {
                    osc.render (out.left.data() + o, out.right.data() + o, n);
                    levelChanges += lastLevel >= 0 && osc.getMipLevel() != lastLevel ? 1 : 0;
                    lastLevel = osc.getMipLevel();
                });

            CHECK (levelChanges >= 7); // level 1 at 40 Hz .. level 9 at 10 kHz

            // Reference: the oscillator's own ramps (double increment, float position and gain), additive synthesis
            std::vector<double> reference (static_cast<std::size_t> (numSamples));
            double phase = 0.0;
            double inc = 0.0;
            float pos = 0.0f;
            float gain = 0.0f;
            const float gainTarget = centreGain();

            for (int tick = 0; tick * controlTickSamples < numSamples; ++tick)
            {
                const auto target = targetAt (tick);
                double incStep = 0.0;
                float posStep = 0.0f;
                float gainStep = 0.0f;

                if (tick == 0)
                {
                    inc = target.increment;
                    pos = target.position;
                    gainStep = (gainTarget - gain) * (1.0f / 16.0f);
                }
                else
                {
                    incStep = (target.increment - inc) * (1.0 / 16.0);
                    posStep = (target.position - pos) * (1.0f / 16.0f);
                }

                for (int s = 0; s < controlTickSamples; ++s)
                {
                    const auto index = static_cast<std::size_t> (tick * controlTickSamples + s);

                    if (index >= reference.size())
                        break;

                    reference[index] = static_cast<double> (gain) * additive (cSaw, cSquare, static_cast<double> (pos), phase, inc);
                    phase += inc;

                    if (phase >= 1.0)
                        phase -= 1.0;

                    inc += incStep;
                    pos += posStep;
                    gain += gainStep;
                }

                inc = target.increment;
                pos = target.position;
                gain = gainTarget;
            }

            const double worst = worstClickDb (out.left, reference);
            CAPTURE (fs, falling, worst);
            CHECK (worst <= -60.0);

            if (verbose())
                std::printf ("click glide %s %.0f Hz: worst window %.1f dB (%d level changes)\n", falling ? "down" : "up", fs,
                             worst, levelChanges);
        }
}

TEST_CASE ("WavetableOscillator: table change saw -> square crossfades linearly over 5 ms without a click", "[synth][oscillator][click]")
{
    const auto cSaw = frameHarmonics (sawTable(), 0);
    const auto cSquare = frameHarmonics (squareTable(), 0);

    for (const double fs : { 44100.0, 48000.0, 96000.0 })
        for (const double f0 : { 110.0, 1000.0, 3000.0 })
        {
            const int numSamples = 8192;
            const int changeAt = 3001; // a block start inside a tick
            const int fadeStart = (changeAt / 16 + 1) * 16;
            const int fadeTicks = static_cast<int> (std::ceil (0.005 * fs / 16.0));
            const double inc = f0 / fs;

            WavetableOscillator osc;
            osc.prepare (fs);
            osc.setTable (&sawTable());
            osc.restart (1, 0.0f);
            Stereo out (static_cast<std::size_t> (numSamples));
            TablesInUse during {};
            const int blocks[] = { changeAt, numSamples };
            int position = 0;
            runTicks (
                numSamples, blocks, [&] (int) { osc.beginTick (toneTick (inc), controlTickSamples); },
                [&] (int o, int n)
                {
                    if (o == changeAt)
                        osc.setTable (&squareTable());

                    if (o == fadeStart + 32)
                        during = osc.getTablesInUse();

                    osc.render (out.left.data() + o, out.right.data() + o, n);
                    position = o + n;
                });

            CHECK (position == numSamples);
            CHECK (during.current == &squareTable());
            CHECK (during.previous == &sawTable());
            CHECK (osc.getTablesInUse().current == &squareTable());
            CHECK (osc.getTablesInUse().previous == nullptr);

            // Ideal 5 ms linear crossfade starting at the first tick after the change
            std::vector<double> reference (static_cast<std::size_t> (numSamples));
            double phase = 0.0;

            for (int s = 0; s < numSamples; ++s)
            {
                const double gain = s < 16 ? static_cast<double> (s) / 16.0 * centreGain() : centreGain();
                const double w = std::clamp (static_cast<double> (s - fadeStart) / (16.0 * fadeTicks), 0.0, 1.0);
                reference[static_cast<std::size_t> (s)]
                    = gain * ((1.0 - w) * additive (cSaw, phase, inc) + w * additive (cSquare, phase, inc));
                phase += inc;

                if (phase >= 1.0)
                    phase -= 1.0;
            }

            const double worst = worstClickDb (out.left, reference);
            CAPTURE (fs, f0, worst);
            CHECK (worst <= -60.0);

            if (verbose())
                std::printf ("click table change %.0f Hz f0 %.0f: worst window %.1f dB\n", fs, f0, worst);
        }
}

//==============================================================================
TEST_CASE ("WavetableOscillator: table change protocol (queue, silent switch, restart, reset)", "[synth][oscillator]")
{
    const Wavetable& t1 = sawTable();
    const Wavetable& t2 = squareTable();
    const Wavetable& t3 = sineTable();
    WavetableOscillator osc;
    osc.prepare (48000.0); // 15 fade ticks
    std::array<float, 16> l {}, r {};
    const auto tick = [&] (float level = 1.0f)
    {
        auto t = toneTick (0.01);
        t.level = level;
        osc.beginTick (t, 16);
        osc.render (l.data(), r.data(), 16);
    };

    // Not started: setTable takes effect at once
    osc.setTable (&t1);
    CHECK (osc.getTablesInUse().current == &t1);
    osc.setTable (&t2);
    CHECK (osc.getTablesInUse().current == &t2);
    CHECK (osc.getTablesInUse().previous == nullptr);

    osc.restart (1, 0.0f);
    tick(); // gain ramps up from 0: the oscillator is sounding from here on
    tick();

    // A change while sounding: applied at the next tick, crossfade over 15 ticks
    osc.setTable (&t1);
    CHECK (osc.getTablesInUse().current == &t2);
    tick();
    CHECK (osc.getTablesInUse().current == &t1);
    CHECK (osc.getTablesInUse().previous == &t2);

    // A change during the crossfade is queued (not reported) and starts after it
    for (int i = 0; i < 5; ++i)
        tick();

    osc.setTable (&t3);
    osc.setTable (&t2); // newest wins
    tick();
    CHECK (osc.getTablesInUse().current == &t1);
    CHECK (osc.getTablesInUse().previous == &t2);

    for (int i = 0; i < 8; ++i)
        tick(); // 15 fade ticks done

    CHECK (osc.getTablesInUse().previous == &t2);
    tick(); // the first fade has ended, the queued one (t1 -> t2) starts
    CHECK (osc.getTablesInUse().current == &t2);
    CHECK (osc.getTablesInUse().previous == &t1);

    // restart() ends a crossfade; a queued table becomes current at once
    osc.setTable (&t3);
    osc.restart (2, 0.0f);
    CHECK (osc.getTablesInUse().current == &t3);
    CHECK (osc.getTablesInUse().previous == nullptr);
    tick();
    tick();

    // Silent oscillator (level 0): a new table at the next tick, no crossfade
    tick (0.0f);
    osc.setTable (&t1);
    tick (0.0f);
    CHECK (osc.getTablesInUse().current == &t1);
    CHECK (osc.getTablesInUse().previous == nullptr);

    // nullptr: crossfade to silence, then exact zero
    tick (1.0f);
    tick (1.0f);
    osc.setTable (nullptr);

    for (int i = 0; i < 16; ++i)
        tick();

    CHECK (osc.getTablesInUse().current == nullptr);
    CHECK (osc.getTablesInUse().previous == nullptr);
    l.fill (0.0f);
    osc.render (l.data(), r.data(), 16);
    CHECK (l == std::array<float, 16> {});

    // reset() forgets everything, including a queued table
    osc.setTable (&t2);
    osc.reset();
    CHECK (osc.getTablesInUse().current == nullptr);
    osc.restart (1, 0.0f);
    tick();
    CHECK (osc.getTablesInUse().current == nullptr);
}

//==============================================================================
TEST_CASE ("WavetableOscillator: bit-identical for every block split", "[synth][oscillator]")
{
    // Glide over several mip levels, position sweep, unison count changes, detune / pan / spread ramps, a table
    // change and a mid-tick restart with re-target at fixed absolute samples, setTable at every block start
    // (as the engine does): the output must be bit-identical for every host block size (docs/SYNTH-DESIGN.md 3.5).
    const auto render = [] (double fs, int block)
    {
        const int numSamples = static_cast<int> (0.2 * fs);
        const int restartAt = numSamples / 2 + 5;
        const int changeAt = numSamples / 3 + 3;
        WavetableOscillator osc;
        osc.prepare (fs);
        osc.setTable (&sawToSquareTable());
        osc.restart (11u, 0.7f);
        Stereo out (static_cast<std::size_t> (numSamples));
        int position = 0;

        while (position < numSamples)
        {
            int end = std::min (numSamples, position + block);

            for (const int cut : { restartAt, changeAt })
                if (position < cut && cut < end)
                    end = cut;

            osc.setTable (position >= changeAt ? &squareTable() : &sawToSquareTable());

            while (position < end)
            {
                const double t = static_cast<double> (position) / fs;

                if (position % controlTickSamples == 0)
                {
                    OscillatorTick tick;
                    tick.increment = 30.0 * std::pow (400.0, t / 0.2) / fs;
                    tick.position = static_cast<float> (0.5 + 0.5 * std::sin (40.0 * t));
                    tick.level = 0.8f;
                    tick.pan = static_cast<float> (0.3 * std::sin (13.0 * t));
                    tick.detuneCents = static_cast<float> (30.0 + 20.0 * std::sin (7.0 * t));
                    tick.spread = 0.6f;
                    tick.unison = 1 + (position / 2000) % 8;
                    osc.beginTick (tick, controlTickSamples);
                }

                if (position == restartAt)
                {
                    OscillatorTick tick;
                    tick.increment = 0.01;
                    tick.level = 0.5f;
                    tick.unison = 3;
                    osc.restart (12u, 0.3f);
                    osc.beginTick (tick, controlTickSamples - position % controlTickSamples);
                }

                int pieceEnd = std::min (end, (position / controlTickSamples + 1) * controlTickSamples);
                if (position < restartAt && restartAt < pieceEnd)
                    pieceEnd = restartAt;

                osc.render (out.left.data() + position, out.right.data() + position, pieceEnd - position);
                position = pieceEnd;
            }
        }

        return out;
    };

    for (const double fs : rates)
    {
        const auto reference = render (fs, 1);
        CHECK (peakAbs (std::span<const float> (reference.left)) > 0.05);

        for (const int block : { 7, 64, 480, 512, 4096 })
        {
            const auto out = render (fs, block);
            CAPTURE (fs, block);
            CHECK (out.left == reference.left);
            CHECK (out.right == reference.right);
        }
    }
}

//==============================================================================
TEST_CASE ("WavetableOscillator: benchmark", "[synth][oscillator][!benchmark]")
{
    // ns per unison voice per sample (budget 6 ns, docs/SYNTH-DESIGN.md 18): 8 voices, 2 frames (position
    // between frames), 48 kHz, incl. the tick. Minimum of 9 runs (shared machines are noisy).
    const Wavetable& table = factoryTable (FactoryTable::basicShapes);
    std::array<float, 128> l {}, r {};

    for (const bool fading : { false, true })
    {
        double best = 1e9;

        for (int run = 0; run < 9; ++run)
        {
            WavetableOscillator osc;
            osc.prepare (48000.0);
            osc.setTable (&table);
            osc.restart (3u, 1.0f);
            OscillatorTick tick = toneTick (220.0 / 48000.0, 0.37f);
            tick.unison = 8;
            tick.detuneCents = 30.0f;
            tick.spread = 0.8f;
            osc.beginTick (tick, 16);
            osc.render (l.data(), r.data(), 16);
            const int blocks = 2000;
            const auto start = std::chrono::steady_clock::now();

            for (int b = 0; b < blocks; ++b)
            {
                if (fading && b % 2 == 0)
                    osc.setTable (b % 4 == 0 ? &sawTable() : &table); // a table crossfade all the time

                for (int s = 0; s < 128; s += 16)
                {
                    tick.position = 0.37f + 0.0001f * static_cast<float> (b % 100);
                    osc.beginTick (tick, 16);
                    osc.render (l.data() + s, r.data() + s, 16);
                }
            }

            const double seconds = std::chrono::duration<double> (std::chrono::steady_clock::now() - start).count();
            best = std::min (best, seconds * 1e9 / (static_cast<double> (blocks) * 128.0 * 8.0));
        }

        std::printf ("WavetableOscillator%s: %.2f ns per unison voice per sample (8 voices, incl. tick), checksum %g\n",
                     fading ? " (table crossfade)" : "", best, static_cast<double> (l[5] + r[77]));
        CHECK (best > 0.0);
    }
}
