#include "vocalsynth/synth/ModMatrix.h"

#include <cmath>

namespace vocalsynth::synth
{

namespace
{

enum class DestFilter
{
    all,
    globalOnly,
    perVoiceOnly
};

void computeFiltered (const std::array<ModSlotSettings, numModSlots>& slots, const ModSourceValues& sources,
                      ModDestValues& out, DestFilter filter) noexcept
{
    out.fill (0.0f);

    for (const auto& slot : slots)
    {
        const auto s = static_cast<int> (slot.source);
        const auto d = static_cast<int> (slot.dest);

        if (s <= 0 || s >= numModSources || d <= 0 || d >= numModDests)
            continue;

        if (filter != DestFilter::all && isGlobalDest (slot.dest) != (filter == DestFilter::globalOnly))
            continue;

        const auto value = sources[static_cast<std::size_t> (s)];
        const auto amount = slot.amountPercent;

        if (! std::isfinite (value) || ! std::isfinite (amount))
            continue;

        out[static_cast<std::size_t> (d)] += amount * 0.01f * value * modDestScale (slot.dest);
    }
}

} // namespace

void ModMatrix::compute (const std::array<ModSlotSettings, numModSlots>& slots, const ModSourceValues& sources,
                         ModDestValues& out) noexcept
{
    computeFiltered (slots, sources, out, DestFilter::all);
}

void ModMatrix::computeGlobal (const std::array<ModSlotSettings, numModSlots>& slots, const ModSourceValues& sources,
                               ModDestValues& out) noexcept
{
    computeFiltered (slots, sources, out, DestFilter::globalOnly);
}

void ModMatrix::computePerVoice (const std::array<ModSlotSettings, numModSlots>& slots,
                                 const ModSourceValues& sources, ModDestValues& out) noexcept
{
    computeFiltered (slots, sources, out, DestFilter::perVoiceOnly);
}

} // namespace vocalsynth::synth
