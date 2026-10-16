#include "vocalsynth/TakeRenderer.h"

namespace vocalsynth
{

// TODO(B): implement (docs/M3-DESIGN.md 9). Stubs: an empty one-bar 4/4 clip at 120 BPM.

RenderedTake TakeRenderer::render (const Take& take, const RenderOptions& options)
{
    const auto o = sanitise (options);

    RenderedTake r;
    r.ticksPerQuarterNote = o.ticksPerQuarterNote;
    r.tempos.push_back ({ 0, TakeTimeline::fallbackBpm });
    r.timeSignatures.push_back ({ 0, 4, 4 });
    r.endTick = 4 * static_cast<int64_t> (o.ticksPerQuarterNote);
    r.audio = computeAudioSpan (take, o);
    return r;
}

AudioSpan TakeRenderer::computeAudioSpan (const Take& take, const RenderOptions&)
{
    return { take.startSample, take.endSample - take.startSample };
}

void TakeRenderer::refineNoteOnsets (Take&, std::span<const float>, int64_t)
{
}

} // namespace vocalsynth
