#include "vocalsynth/ScaleQuantizer.h"

#include <algorithm>
#include <cmath>

namespace vocalsynth
{

namespace
{
// Every scale has gaps of at most 3 semitones, so the nearest allowed note is always within one octave
// of lround (p); +-12 also covers the edges 0 and 127 (one full octave lies inside the MIDI range).
constexpr int searchRadius = 12;
} // namespace

void ScaleQuantizer::setScale (int newKey, Scale newScale) noexcept
{
    key = std::clamp (newKey, 0, 11);
    scale = static_cast<Scale> (std::clamp (static_cast<int> (newScale), 0, numScales - 1));
    mask = scaleMask (scale);
}

bool ScaleQuantizer::isAllowed (int midiNote) const noexcept
{
    if (midiNote < 0 || midiNote > 127)
        return false;

    const auto pitchClass = ((midiNote - key) % 12 + 12) % 12;
    return (mask & (1u << pitchClass)) != 0;
}

int ScaleQuantizer::quantize (double midiPitch, int currentNote) const noexcept
{
    // A non-finite pitch has no nearest note: keep the current one if it is valid, else use middle C.
    if (! std::isfinite (midiPitch))
        midiPitch = isAllowed (currentNote) ? static_cast<double> (currentNote) : 60.0;

    // Outside the MIDI range the nearest allowed note is the nearest one to the range edge.
    const auto p = std::clamp (midiPitch, 0.0, 127.0);
    const auto centre = static_cast<int> (std::lround (p));
    const auto lo = std::max (0, centre - searchRadius);
    const auto hi = std::min (127, centre + searchRadius);

    int best = -1;
    double bestDistance = 0.0;

    // Ascending scan: on a tie the lower note is found first and only the current note can replace it.
    for (int n = lo; n <= hi; ++n)
    {
        if (! isAllowed (n))
            continue;

        const auto distance = std::abs (p - static_cast<double> (n));

        if (best < 0 || distance < bestDistance - tieEpsilonSemitones)
        {
            best = n;
            bestDistance = distance;
        }
        else if (std::abs (distance - bestDistance) <= tieEpsilonSemitones && n == currentNote)
        {
            best = n;
        }
    }

    return best >= 0 ? best : centre; // unreachable: bit 0 of every mask is set
}

} // namespace vocalsynth
