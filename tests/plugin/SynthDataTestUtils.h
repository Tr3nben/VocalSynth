#pragma once

// Shared helpers of agent D's plugin-data tests (WavetableWorker, SynthState, PresetManager). Header only.

#include "WavetableStore.h"
#include "vocalsynth/synth/WavetableBuilder.h"

#include <juce_core/juce_core.h>

#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

namespace vocalsynth::testdata
{

/** A fresh folder under the temp directory with spaces and umlauts in its name; deleted with its content. */
struct ScopedTempFolder
{
    juce::File folder;

    explicit ScopedTempFolder (const juce::String& prefix = juce::String::fromUTF8 ("VocalSynth Tests \xc3\xa4\xc3\xb6\xc3\xbc "))
        : folder (juce::File::getSpecialLocation (juce::File::tempDirectory)
                      .getChildFile (prefix + juce::String::toHexString (juce::Random::getSystemRandom().nextInt64())))
    {
        folder.createDirectory();
    }

    ~ScopedTempFolder() { folder.deleteRecursively(); }

    ScopedTempFolder (const ScopedTempFolder&) = delete;
    ScopedTempFolder& operator= (const ScopedTempFolder&) = delete;
};

/** numFrames frames of 2048 samples: a sine that morphs into a saw; seed adds a seed-dependent harmonic so
    different seeds give different tables (and hashes). Peak below 1. */
inline std::vector<float> morphFrames (int numFrames, int seed)
{
    constexpr int n = synth::Wavetable::frameSize;
    std::vector<float> samples (static_cast<std::size_t> (numFrames) * n);
    const double twoPi = 2.0 * 3.14159265358979323846;

    for (int f = 0; f < numFrames; ++f)
    {
        const double morph = numFrames > 1 ? static_cast<double> (f) / (numFrames - 1) : 0.0;

        for (int i = 0; i < n; ++i)
        {
            const double ph = twoPi * i / n;
            double v = (1.0 - morph) * std::sin (ph);

            for (int k = 1; k <= 20; ++k)
                v += morph * 0.5 * std::sin (k * ph) / k;

            v += 0.1 * std::sin ((3 + seed % 17) * ph + seed);
            samples[static_cast<std::size_t> (f * n + i)] = static_cast<float> (0.5 * v);
        }
    }

    return samples;
}

/** 16-bit PCM WAV bytes (1 or 2 channels; stereo writes the same samples to both). */
inline juce::MemoryBlock makeWav16 (const std::vector<float>& mono, int channels = 1, double sampleRate = 44100.0)
{
    juce::MemoryOutputStream out;
    const auto dataBytes = static_cast<uint32_t> (mono.size() * static_cast<std::size_t> (channels) * 2);

    out.write ("RIFF", 4);
    out.writeInt (static_cast<int> (36 + dataBytes));
    out.write ("WAVE", 4);
    out.write ("fmt ", 4);
    out.writeInt (16);
    out.writeShort (1);
    out.writeShort (static_cast<short> (channels));
    out.writeInt (static_cast<int> (sampleRate));
    out.writeInt (static_cast<int> (sampleRate) * channels * 2);
    out.writeShort (static_cast<short> (channels * 2));
    out.writeShort (16);
    out.write ("data", 4);
    out.writeInt (static_cast<int> (dataBytes));

    for (const auto v : mono)
        for (int c = 0; c < channels; ++c)
            out.writeShort (static_cast<short> (std::lround (juce::jlimit (-1.0f, 1.0f, v) * 32767.0f)));

    return out.getMemoryBlock();
}

inline juce::String hashOf (const std::vector<int16_t>& pcm)
{
    return juce::String::toHexString (static_cast<juce::int64> (synth::WavetableBuilder::fnv1a64 (pcm)))
        .paddedLeft ('0', 16)
        .toLowerCase();
}

/** User table data as an import would produce it (PCM16 of normalised frames, correct hash). */
inline std::shared_ptr<UserTableData> makeUserData (int numFrames, int seed, const juce::String& name = "Test Table.wav")
{
    const auto frames = morphFrames (numFrames, seed);
    auto data = std::make_shared<UserTableData>();
    data->name = name;
    data->numFrames = numFrames;
    data->pcm.resize (frames.size());

    for (std::size_t i = 0; i < frames.size(); ++i)
        data->pcm[i] = synth::WavetableBuilder::quantiseToPcm16 (frames[i]);

    data->hash = hashOf (data->pcm);
    return data;
}

} // namespace vocalsynth::testdata
