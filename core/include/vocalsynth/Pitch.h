#pragma once

namespace vocalsynth
{

/** Reference tuning: MIDI note 69 (A4) = 440 Hz. */
inline constexpr double referenceNote = 69.0;
inline constexpr double referenceFrequencyHz = 440.0;

/** Converts a frequency in Hz to a fractional MIDI note number (69.0 = A4 = 440 Hz).
    Returns 0 for frequencies <= 0 (treat as "unvoiced"). Real-time safe. */
double frequencyToMidiNote (double frequencyHz) noexcept;

/** Converts a (fractional) MIDI note number to a frequency in Hz. Real-time safe. */
double midiNoteToFrequency (double midiNote) noexcept;

/** Signed distance in cents from the given integer MIDI note to the frequency.
    Positive means the frequency is sharp of the note. Real-time safe. */
double centsFromNote (double frequencyHz, int midiNote) noexcept;

/** Nearest integer MIDI note for a frequency, clamped to the valid MIDI range [0, 127]. */
int nearestMidiNote (double frequencyHz) noexcept;

} // namespace vocalsynth
