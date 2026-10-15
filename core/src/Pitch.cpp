#include "vocalsynth/Pitch.h"

#include <algorithm>
#include <cmath>

namespace vocalsynth
{

double frequencyToMidiNote (double frequencyHz) noexcept
{
    if (! (frequencyHz > 0.0))
        return 0.0;

    return referenceNote + 12.0 * std::log2 (frequencyHz / referenceFrequencyHz);
}

double midiNoteToFrequency (double midiNote) noexcept
{
    return referenceFrequencyHz * std::exp2 ((midiNote - referenceNote) / 12.0);
}

double centsFromNote (double frequencyHz, int midiNote) noexcept
{
    return 100.0 * (frequencyToMidiNote (frequencyHz) - static_cast<double> (midiNote));
}

int nearestMidiNote (double frequencyHz) noexcept
{
    const auto note = std::lround (frequencyToMidiNote (frequencyHz));
    return static_cast<int> (std::clamp (note, 0L, 127L));
}

} // namespace vocalsynth
