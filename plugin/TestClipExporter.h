#pragma once

#include "vocalsynth/TestPatternGenerator.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

namespace vocalsynth
{

/** Writes the drag & drop test clips for the M1 FL Studio checks (MIDI into a pattern, WAV into the
    playlist). Message thread only: allocates and does file I/O. Files go to <temp>/VocalSynth/. */
namespace TestClipExporter
{
inline constexpr double renderSampleRate = 48000.0;
inline constexpr int ticksPerQuarterNote = 960;
inline constexpr int numBars = 4; // 4/4
inline constexpr int endTick = numBars * 4 * ticksPerQuarterNote; ///< end-of-track, so FL sees a 4-bar clip
inline constexpr double wavFrequencyHz = 440.0;
inline constexpr double wavLengthSeconds = 2.0;
inline constexpr int wavBitDepth = 24;

/** <temp>/VocalSynth/ (created if missing). */
juce::File getExportDirectory();

/** Runs TestPatternGenerator offline at renderSampleRate for numBars at settings.bpm and returns the
    events in ticks (ticksPerQuarterNote), with a tempo meta event and a track name at tick 0, note-offs
    paired (updateMatchedPairs) and a final flush so nothing hangs (its events at endTick at most),
    and an end-of-track meta event at endTick. If settings.pattern is off, the Notes pattern is
    rendered instead, so the drag always yields something audible. */
juce::MidiMessageSequence renderTestPattern (const TestPatternSettings& settings);

/** Writes renderTestPattern() as a type-1 .mid file. FL may still hold a previously dragged file open,
    so write to a temporary file and move it into place; if that fails, use a numbered sibling
    (File::getNonexistentSibling). Returns the file, or juce::File() on failure. */
juce::File exportMidi (const TestPatternSettings& settings);

/** Writes a wavLengthSeconds, wavFrequencyHz sine (mono, -6 dBFS, 48 kHz, 24 bit) as .wav.
    Same file-locking rule as exportMidi(). Returns the file, or juce::File() on failure. */
juce::File exportWav();
} // namespace TestClipExporter

} // namespace vocalsynth
