#pragma once

// Minimal RIFF/WAVE reader + writer for vs-eval (no JUCE). docs/M2-DESIGN.md 5.

#include <string>
#include <vector>

namespace vseval
{

struct WavData
{
    double sampleRate = 0.0;
    int numChannels = 0;
    std::vector<float> mono; ///< channels averaged, -1..1
};

/** Reads PCM 16/24/32-bit integer and 32-bit float WAV files (WAVE_FORMAT_PCM, _IEEE_FLOAT and
    _EXTENSIBLE with those subformats), any channel count (downmixed to mono by averaging). Skips unknown
    chunks. Returns false and sets error on failure. */
bool readWav (const std::string& path, WavData& out, std::string& error);

/** Writes a mono 32-bit float WAV (used to dump the synthetic corpus). Returns false on failure. */
bool writeWavMono (const std::string& path, const std::vector<float>& samples, double sampleRate);

} // namespace vseval
