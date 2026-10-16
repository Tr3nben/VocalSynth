#include "vocalsynth/SmfWriter.h"

namespace vocalsynth
{

// TODO(B): implement (docs/M3-DESIGN.md 10). Stubs: no bytes.

std::vector<uint8_t> SmfWriter::write (const RenderedTake&, const SmfOptions&)
{
    return {};
}

void SmfWriter::appendVariableLength (std::vector<uint8_t>&, uint32_t)
{
}

} // namespace vocalsynth
