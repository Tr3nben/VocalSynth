#include "vocalsynth/synth/Wavetable.h"

#include <algorithm>
#include <utility>

namespace vocalsynth::synth
{

Wavetable::Wavetable (std::string tableName, int frames)
    : name (std::move (tableName)),
      numFrames (std::clamp (frames, 1, maxFrames)),
      data (static_cast<std::size_t> (numFrames) * static_cast<std::size_t> (frameStride), 0.0f)
{
}

} // namespace vocalsynth::synth
