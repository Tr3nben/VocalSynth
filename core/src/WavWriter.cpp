#include "vocalsynth/WavWriter.h"

namespace vocalsynth
{

// TODO(B): implement (docs/M3-DESIGN.md 11). Stubs: no bytes.

uint64_t WavWriter::maxFrames (WavSampleFormat) noexcept
{
    return 0;
}

std::vector<uint8_t> WavWriter::makeHeader (WavSampleFormat, double, uint64_t)
{
    return {};
}

int WavWriter::dataPaddingBytes (WavSampleFormat, uint64_t) noexcept
{
    return 0;
}

std::size_t WavWriter::encode (std::span<const float>, WavSampleFormat, std::span<uint8_t>) noexcept
{
    return 0;
}

} // namespace vocalsynth
