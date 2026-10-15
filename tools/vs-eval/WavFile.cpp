#include "WavFile.h"

namespace vseval
{

// TODO(C): implement (docs/M2-DESIGN.md 5).

bool readWav (const std::string& path, WavData& out, std::string& error)
{
    out = {};
    error = "WAV reader not implemented yet: " + path;
    return false;
}

bool writeWavMono (const std::string& /*path*/, const std::vector<float>& /*samples*/, double /*sampleRate*/)
{
    return false;
}

} // namespace vseval
