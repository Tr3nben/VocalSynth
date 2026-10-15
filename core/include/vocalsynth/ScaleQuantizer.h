#pragma once

#include "vocalsynth/VoiceSettings.h"

#include <cstdint>

namespace vocalsynth
{

/** Key + scale -> nearest allowed MIDI note (docs/M2-DESIGN.md 3.7).

    Scales (semitones above the key):
      chromatic        0 1 2 3 4 5 6 7 8 9 10 11
      major            0 2 4 5 7 9 11
      naturalMinor     0 2 3 5 7 8 10
      harmonicMinor    0 2 3 5 7 8 11
      majorPentatonic  0 2 4 7 9
      minorPentatonic  0 3 5 7 10
      blues            0 3 5 6 7 10
      dorian           0 2 3 5 7 9 10
      mixolydian       0 2 4 5 7 9 10
    key 0..11 = C..B transposes the pattern (pitch class = (note - key) mod 12).

    Tie rule (binding): if the pitch is exactly (|da - db| <= tieEpsilonSemitones) between two allowed
    notes, the CURRENT note wins if it is one of them, otherwise the LOWER one. Results are limited to
    allowed notes inside [0, 127]. Stateless apart from the scale; real-time safe. */
class ScaleQuantizer
{
public:
    static constexpr double tieEpsilonSemitones = 1e-9;

    /** Bit p set = pitch class p (relative to the key) is in the scale. Bit 0 is always set. */
    static constexpr uint16_t scaleMask (Scale scale) noexcept
    {
        switch (scale)
        {
            case Scale::chromatic:       return 0x0fff;
            case Scale::major:           return maskOf ({ 0, 2, 4, 5, 7, 9, 11 });
            case Scale::naturalMinor:    return maskOf ({ 0, 2, 3, 5, 7, 8, 10 });
            case Scale::harmonicMinor:   return maskOf ({ 0, 2, 3, 5, 7, 8, 11 });
            case Scale::majorPentatonic: return maskOf ({ 0, 2, 4, 7, 9 });
            case Scale::minorPentatonic: return maskOf ({ 0, 3, 5, 7, 10 });
            case Scale::blues:           return maskOf ({ 0, 3, 5, 6, 7, 10 });
            case Scale::dorian:          return maskOf ({ 0, 2, 3, 5, 7, 9, 10 });
            case Scale::mixolydian:      return maskOf ({ 0, 2, 4, 5, 7, 9, 10 });
        }

        return 0x0fff;
    }

    /** key is clamped to 0..11, scale to the valid enum range. */
    void setScale (int key, Scale scale) noexcept;

    int getKey() const noexcept { return key; }
    Scale getScale() const noexcept { return scale; }

    /** True if the MIDI note (0..127) belongs to the current key + scale. */
    bool isAllowed (int midiNote) const noexcept;

    /** Nearest allowed note to a fractional MIDI pitch (69.0 = A4), with the tie rule above.
        currentNote = the note now sounding (before octave shift), or -1 for none. */
    int quantize (double midiPitch, int currentNote) const noexcept;

private:
    template <std::size_t N>
    static constexpr uint16_t maskOf (const int (&intervals)[N]) noexcept
    {
        uint16_t m = 0;

        for (auto i : intervals)
            m = static_cast<uint16_t> (m | (1u << i));

        return m;
    }

    int key = 0;
    Scale scale = Scale::chromatic;
    uint16_t mask = 0x0fff;
};

} // namespace vocalsynth
