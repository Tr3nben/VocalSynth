#include "vocalsynth/synth/WavReader.h"

namespace vocalsynth::synth
{

// Scaffold (docs/SYNTH-DESIGN.md 5.4): every input is rejected. Agent A implements.

std::string_view describeWavReadError (WavReadError error) noexcept
{
    switch (error)
    {
        case WavReadError::none:              return "OK";
        case WavReadError::empty:             return "The file is empty";
        case WavReadError::notRiffWave:       return "Not a WAV file";
        case WavReadError::missingFormat:     return "The WAV file has no format chunk";
        case WavReadError::unsupportedFormat: return "Unsupported WAV format";
        case WavReadError::missingData:       return "The WAV file has no audio data";
        case WavReadError::truncated:         return "The WAV file is damaged (cut off)";
        case WavReadError::tooLong:           return "The WAV file is too long for a wavetable";
    }

    return "Unknown error";
}

WavReadResult WavReader::read (std::span<const std::byte> bytes)
{
    WavReadResult result;
    result.error = bytes.empty() ? WavReadError::empty : WavReadError::notRiffWave;
    return result;
}

int WavReader::parseClmFrameSize (std::string_view) noexcept
{
    return 0;
}

} // namespace vocalsynth::synth
