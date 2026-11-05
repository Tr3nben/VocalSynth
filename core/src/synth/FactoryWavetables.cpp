#include "vocalsynth/synth/FactoryWavetables.h"

#include "vocalsynth/synth/RealFft.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <functional>
#include <numbers>

namespace vocalsynth::synth
{

namespace
{
// Ids and names are final (persisted, docs/SYNTH-DESIGN.md 5.5); frame counts may be tuned by agent A.
constexpr std::array<FactoryTableInfo, numFactoryTables> infos { {
    { "basic-shapes", "Basic Shapes", 64 },
    { "pwm", "PWM", 64 },
    { "harmonic-sweep", "Harmonic Sweep", 64 },
    { "saw-bright-dark", "Saw Bright-Dark", 32 },
    { "formant-vowel", "Formant Vowels", 64 },
    { "sync-sweep", "Sync Sweep", 64 },
    { "fm-2op", "FM 2-Op", 64 },
    { "fold", "Fold", 64 },
    { "organ", "Organ", 32 },
    { "digital", "Digital", 32 },
    { "spectral-random", "Spectral Random", 32 },
    { "bell", "Bell", 32 },
    { "square-stack", "Square Stack", 32 },
    { "choir", "Choir", 64 },
} };

constexpr int frameSize = Wavetable::frameSize;
constexpr int maxHarmonic = frameSize / 2 - 1; // 1023
constexpr double pi = std::numbers::pi;
constexpr double minPulseWidth = 0.10; // PWM table end (see generate)

/** Fourier series of one cycle: x (t) = sum_k cosAmp[k] cos (2 pi k t) + sinAmp[k] sin (2 pi k t), k = 1..1023. */
struct Series
{
    std::array<double, maxHarmonic + 1> cosAmp {};
    std::array<double, maxHarmonic + 1> sinAmp {};

    void add (const Series& other, double weight) noexcept
    {
        for (std::size_t k = 0; k < cosAmp.size(); ++k)
        {
            cosAmp[k] += weight * other.cosAmp[k];
            sinAmp[k] += weight * other.sinAmp[k];
        }
    }
};

Series lerp (const Series& a, const Series& b, double t)
{
    Series s;
    s.add (a, 1.0 - t);
    s.add (b, t);
    return s;
}

/** Shared FFT plans of one generateFrames() call. */
struct Synthesiser
{
    RealFft frameFft { 11 };     // 2048
    RealFft oversampledFft { 14 }; // 16384 = 8x for the time-domain recipes
    std::vector<std::complex<double>> bins = std::vector<std::complex<double>> (frameSize / 2 + 1);
    std::vector<std::complex<double>> bigBins = std::vector<std::complex<double>> (8193);
    std::vector<double> time = std::vector<double> (frameSize);
    std::vector<double> bigTime = std::vector<double> (16384);

    void appendSeries (const Series& s, std::vector<float>& out)
    {
        std::fill (bins.begin(), bins.end(), std::complex<double> {});

        for (int k = 1; k <= maxHarmonic; ++k)
        {
            const auto i = static_cast<std::size_t> (k);
            bins[i] = { 0.5 * frameSize * s.cosAmp[i], -0.5 * frameSize * s.sinAmp[i] };
        }

        frameFft.inverse (bins, time);

        for (const auto v : time)
            out.push_back (static_cast<float> (v));
    }

    /** One cycle of f (t), t in [0, 1), sampled 8x oversampled, band-limited to harmonic 1023, at 2048;
        harmonic k optionally tilted by k^(-tilt). */
    void appendTimeDomain (const std::function<double (double)>& f, std::vector<float>& out, double tilt = 0.0)
    {
        const auto n = bigTime.size();

        for (std::size_t i = 0; i < n; ++i)
            bigTime[i] = f (static_cast<double> (i) / static_cast<double> (n));

        oversampledFft.forward (bigTime, bigBins);
        std::fill (bins.begin(), bins.end(), std::complex<double> {});
        const double scale = static_cast<double> (frameSize) / static_cast<double> (n);

        for (int k = 1; k <= maxHarmonic; ++k)
            bins[static_cast<std::size_t> (k)] = bigBins[static_cast<std::size_t> (k)]
                                                 * (tilt > 0.0 ? scale * std::pow (static_cast<double> (k), -tilt) : scale);

        frameFft.inverse (bins, time);

        for (const auto v : time)
            out.push_back (static_cast<float> (v));
    }

    /** A frame computed directly at 2048 samples (the digital table: aliasing inside the cycle is the sound). */
    static void appendSamples (const std::function<double (int)>& f, std::vector<float>& out)
    {
        for (int i = 0; i < frameSize; ++i)
            out.push_back (static_cast<float> (f (i)));
    }
};

//------------------------------------------------------------------------------
// Basic spectra (all start rising at t = 0)

Series sine()
{
    Series s;
    s.sinAmp[1] = 1.0;
    return s;
}

Series saw()
{
    Series s;

    for (int k = 1; k <= maxHarmonic; ++k)
        s.sinAmp[static_cast<std::size_t> (k)] = (k % 2 == 1 ? 2.0 : -2.0) / (pi * k);

    return s;
}

Series square()
{
    Series s;

    for (int k = 1; k <= maxHarmonic; k += 2)
        s.sinAmp[static_cast<std::size_t> (k)] = 4.0 / (pi * k);

    return s;
}

Series triangle()
{
    Series s;

    for (int k = 1; k <= maxHarmonic; k += 2)
        s.sinAmp[static_cast<std::size_t> (k)] = 8.0 / (pi * pi * k * k) * ((k / 2) % 2 == 0 ? 1.0 : -1.0);

    return s;
}

/** +1 on [0, width), -1 elsewhere (DC dropped). width 0.5 = square. */
Series pulse (double width)
{
    Series s;

    for (int k = 1; k <= maxHarmonic; ++k)
    {
        const auto i = static_cast<std::size_t> (k);
        s.cosAmp[i] = 2.0 / (pi * k) * std::sin (2.0 * pi * k * width);
        s.sinAmp[i] = 2.0 / (pi * k) * (1.0 - std::cos (2.0 * pi * k * width));
    }

    return s;
}

/** Frame i of n as a position 0..1. */
double positionOf (int i, int n) noexcept
{
    return n > 1 ? static_cast<double> (i) / static_cast<double> (n - 1) : 0.0;
}

/** Key frames at equal spacing, linear crossfade of the spectra between neighbours. */
std::vector<float> morph (const std::vector<Series>& keys, int frames, Synthesiser& synth)
{
    std::vector<float> out;
    out.reserve (static_cast<std::size_t> (frames) * frameSize);

    for (int i = 0; i < frames; ++i)
    {
        const double x = positionOf (i, frames) * static_cast<double> (keys.size() - 1);
        const auto j = std::min (static_cast<std::size_t> (x), keys.size() - 2);
        synth.appendSeries (lerp (keys[j], keys[j + 1], x - static_cast<double> (j)), out);
    }

    return out;
}

//------------------------------------------------------------------------------
// Formants

struct Vowel
{
    std::array<double, 3> freq;  // Hz
    std::array<double, 3> gain;  // linear
};

constexpr double formantWidths[3] = { 90.0, 110.0, 170.0 }; // Gaussian sigma, Hz

// A, E, I, O, U (Peterson & Barney style averages)
constexpr std::array<Vowel, 5> vowels { {
    { { 730.0, 1090.0, 2440.0 }, { 1.0, 0.50, 0.18 } },
    { { 530.0, 1840.0, 2480.0 }, { 1.0, 0.40, 0.25 } },
    { { 270.0, 2290.0, 3010.0 }, { 1.0, 0.25, 0.20 } },
    { { 570.0, 840.0, 2410.0 }, { 1.0, 0.60, 0.10 } },
    { { 300.0, 870.0, 2240.0 }, { 1.0, 0.35, 0.05 } },
} };

Vowel mixVowels (const Vowel& a, const Vowel& b, double t) noexcept
{
    Vowel v {};

    for (std::size_t j = 0; j < 3; ++j)
    {
        v.freq[j] = a.freq[j] + (b.freq[j] - a.freq[j]) * t;
        v.gain[j] = a.gain[j] + (b.gain[j] - a.gain[j]) * t;
    }

    return v;
}

/** Spectral envelope of a vowel at f Hz: three Gaussian formants, a low-frequency (glottal) bump and a floor. */
double formantEnvelope (const Vowel& v, double f) noexcept
{
    double e = 0.02 + 0.8 * std::exp (-0.5 * (f / 220.0) * (f / 220.0));

    for (std::size_t j = 0; j < 3; ++j)
    {
        const double d = (f - v.freq[j]) / formantWidths[j];
        e += v.gain[j] * std::exp (-0.5 * d * d);
    }

    return e;
}

/** Source 1 / k^tilt times the envelopes of the given vowels (averaged), f0 reference 130 Hz. */
Series voiced (std::initializer_list<Vowel> set, double tilt)
{
    constexpr double f0 = 130.0;
    Series s;

    for (int k = 1; k <= maxHarmonic; ++k)
    {
        double e = 0.0;

        for (const auto& v : set)
            e += formantEnvelope (v, f0 * k);

        s.sinAmp[static_cast<std::size_t> (k)] = e / static_cast<double> (set.size()) / std::pow (static_cast<double> (k), tilt);
    }

    return s;
}

//------------------------------------------------------------------------------
uint32_t xorshift (uint32_t& state) noexcept
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

double uniform01 (uint32_t& state) noexcept
{
    return static_cast<double> (xorshift (state) >> 8) * (1.0 / 16777216.0);
}

std::vector<float> generate (FactoryTable table, int frames)
{
    Synthesiser synth;
    std::vector<float> out;
    out.reserve (static_cast<std::size_t> (frames) * frameSize);

    switch (table)
    {
        case FactoryTable::basicShapes:
            return morph ({ sine(), triangle(), saw(), square(), pulse (0.25) }, frames, synth);

        case FactoryTable::pwm:
            // 50 % -> 10 %: narrower pulses put more energy at the top of every mip level than the 8x Hermite
            // read keeps below the alias gate (-70 dB re the signal up to 1 kHz; a 2 % pulse reaches -64 dB).
            for (int i = 0; i < frames; ++i)
                synth.appendSeries (pulse (0.5 + (minPulseWidth - 0.5) * positionOf (i, frames)), out);
            break;

        case FactoryTable::harmonicSweep:
            for (int i = 0; i < frames; ++i)
            {
                const double t = positionOf (i, frames);
                Series s;
                s.sinAmp[1] = 1.0;

                for (int k = 2; k <= maxHarmonic; ++k)
                    s.sinAmp[static_cast<std::size_t> (k)] = (k % 2 == 1 ? 1.0 - t : t) / k;

                synth.appendSeries (s, out);
            }
            break;

        case FactoryTable::sawBrightDark:
            for (int i = 0; i < frames; ++i)
            {
                // tilt 0 -> -12 dB / octave = amplitude k^(-2 t)
                const double t = positionOf (i, frames);
                Series s = saw();

                for (int k = 1; k <= maxHarmonic; ++k)
                    s.sinAmp[static_cast<std::size_t> (k)] *= std::pow (static_cast<double> (k), -2.0 * t);

                synth.appendSeries (s, out);
            }
            break;

        case FactoryTable::formantVowel:
            for (int i = 0; i < frames; ++i)
            {
                const double x = positionOf (i, frames) * 4.0;
                const auto j = std::min (static_cast<std::size_t> (x), std::size_t { 3 });
                synth.appendSeries (voiced ({ mixVowels (vowels[j], vowels[j + 1], x - static_cast<double> (j)) }, 1.0), out);
            }
            break;

        case FactoryTable::syncSweep:
            for (int i = 0; i < frames; ++i)
            {
                // A hard-synced saw at ratio r is about r times brighter at the top of each mip level than a
                // saw; a gentle tilt k^(-beta), beta = 0.11 log2 (r) (-2 dB / octave at r = 8), keeps it below
                // the alias gate like the saw.
                const double ratio = 1.0 + 7.0 * positionOf (i, frames);
                synth.appendTimeDomain ([ratio] (double t)
                                        {
                                            const double p = ratio * t;
                                            return 2.0 * (p - std::floor (p)) - 1.0;
                                        },
                                        out, 0.11 * std::log2 (ratio));
            }
            break;

        case FactoryTable::fm2Op:
            for (int i = 0; i < frames; ++i)
            {
                const double index = 8.0 * positionOf (i, frames);
                synth.appendTimeDomain ([index] (double t) { return std::sin (2.0 * pi * t + index * std::sin (4.0 * pi * t)); },
                                        out);
            }
            break;

        case FactoryTable::fold:
            for (int i = 0; i < frames; ++i)
            {
                const double gain = 1.0 + 7.0 * positionOf (i, frames);
                // Triangle wavefolder: identity for |u| <= 1, reflected at +-1 beyond (period 4 in u)
                synth.appendTimeDomain ([gain] (double t)
                                        {
                                            const double v = gain * std::sin (2.0 * pi * t) + 1.0;
                                            const double w = v - 4.0 * std::floor (v / 4.0); // 0..4
                                            return w <= 2.0 ? w - 1.0 : 3.0 - w;
                                        },
                                        out);
            }
            break;

        case FactoryTable::organ:
        {
            // Drawbars 16', 5 1/3', 8', 4', 2 2/3', 2', 1 3/5', 1 1/3', 1' = harmonics 1, 3, 2, 4, 6, 8, 10, 12, 16;
            // registration digits 0..8, 3 dB per step.
            constexpr int harmonics[9] = { 1, 3, 2, 4, 6, 8, 10, 12, 16 };
            constexpr const char* registrations[] = { "888000000", "838000000", "808808008", "868600000",
                                                      "888888888", "800000888", "876543210", "848000080" };
            std::vector<Series> keys;

            for (const auto* reg : registrations)
            {
                Series s;

                for (int d = 0; d < 9; ++d)
                {
                    const int level = reg[d] - '0';

                    if (level > 0)
                        s.sinAmp[static_cast<std::size_t> (harmonics[d])] = std::pow (10.0, -3.0 * (8 - level) / 20.0);
                }

                keys.push_back (s);
            }

            return morph (keys, frames, synth);
        }

        case FactoryTable::digital:
            for (int i = 0; i < frames; ++i)
            {
                const double t = positionOf (i, frames);
                const int levels = std::max (2, static_cast<int> (std::lround (32.0 * std::pow (2.0 / 32.0, t))));
                const int hold = std::max (1, static_cast<int> (std::lround (std::pow (32.0, t))));
                Synthesiser::appendSamples ([levels, hold] (int n)
                                            {
                                                const int held = (n / hold) * hold;
                                                const double x = static_cast<double> (held) / frameSize; // 0..1 saw
                                                const double q = std::round (x * (levels - 1)) / (levels - 1);
                                                return 2.0 * q - 1.0;
                                            },
                                            out);
            }
            break;

        case FactoryTable::spectralRandom:
        {
            uint32_t state = 0x5eed1234u;
            std::vector<Series> keys (8);

            for (auto& s : keys)
            {
                s.sinAmp[1] = 1.0;

                for (int k = 2; k <= maxHarmonic; ++k)
                {
                    const double amp = uniform01 (state) / k;
                    const double phase = 2.0 * pi * uniform01 (state);
                    s.cosAmp[static_cast<std::size_t> (k)] = amp * std::sin (phase);
                    s.sinAmp[static_cast<std::size_t> (k)] = amp * std::cos (phase);
                }
            }

            return morph (keys, frames, synth);
        }

        case FactoryTable::bell:
            for (int i = 0; i < frames; ++i)
            {
                // Bell partials 1, 2.76, 5.40, 8.93, 13.34, 18.64, 24.81 rounded to harmonics; the upper ones
                // decay faster along the table, plus a soft hum an octave up
                constexpr int partials[] = { 1, 3, 5, 9, 13, 19, 25 };
                const double t = positionOf (i, frames);
                Series s;

                for (int j = 0; j < 7; ++j)
                    s.sinAmp[static_cast<std::size_t> (partials[j])]
                        += std::exp (-t * 0.9 * j) / (1.0 + 0.35 * j);

                s.sinAmp[2] += 0.3 * (1.0 - 0.5 * t);
                synth.appendSeries (s, out);
            }
            break;

        case FactoryTable::squareStack:
            for (int i = 0; i < frames; ++i)
            {
                const double layers = 1.0 + 3.0 * positionOf (i, frames);
                Series s;

                for (int j = 0; j < 4; ++j)
                {
                    const double weight = std::clamp (layers - j, 0.0, 1.0) * std::pow (0.8, j);
                    const int base = 1 << j;

                    for (int k = 1; k * base <= maxHarmonic; k += 2)
                        s.sinAmp[static_cast<std::size_t> (k * base)] += weight * 4.0 / (pi * k);
                }

                synth.appendSeries (s, out);
            }
            break;

        case FactoryTable::choir:
        {
            // Pairs A/O, E/I, O/U, A/E, I/U at 0 / 25 / 50 / 75 / 100 %, soft saw source (1 / k^1.4)
            constexpr std::size_t pairs[5][2] = { { 0, 3 }, { 1, 2 }, { 3, 4 }, { 0, 1 }, { 2, 4 } };

            for (int i = 0; i < frames; ++i)
            {
                const double x = positionOf (i, frames) * 4.0;
                const auto j = std::min (static_cast<std::size_t> (x), std::size_t { 3 });
                const double t = x - static_cast<double> (j);
                synth.appendSeries (voiced ({ mixVowels (vowels[pairs[j][0]], vowels[pairs[j + 1][0]], t),
                                              mixVowels (vowels[pairs[j][1]], vowels[pairs[j + 1][1]], t) },
                                            1.4),
                                    out);
            }
            break;
        }
    }

    return out;
}
} // namespace

const FactoryTableInfo& FactoryWavetables::getInfo (FactoryTable table) noexcept
{
    const auto index = static_cast<std::size_t> (table);
    return infos[index < infos.size() ? index : 0];
}

std::optional<FactoryTable> FactoryWavetables::fromId (std::string_view id) noexcept
{
    for (std::size_t i = 0; i < infos.size(); ++i)
        if (infos[i].id == id)
            return static_cast<FactoryTable> (i);

    return std::nullopt;
}

std::vector<float> FactoryWavetables::generateFrames (FactoryTable table)
{
    const auto index = static_cast<std::size_t> (table);

    if (index >= infos.size())
        table = FactoryTable::basicShapes;

    return generate (table, getInfo (table).numFrames);
}

WavetableBuildResult FactoryWavetables::build (FactoryTable table, const std::atomic<bool>* cancel)
{
    const auto frames = generateFrames (table);
    return WavetableBuilder::build (std::string (getInfo (table).displayName), frames, Wavetable::frameSize, cancel);
}

} // namespace vocalsynth::synth
