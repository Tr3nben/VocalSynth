#pragma once

#include <juce_core/juce_core.h>

#include <functional>

namespace vocalsynth::ExportFiles
{

/** File helpers shared by the M1 TestClipExporter and the M3 CaptureExporter (docs/M3-DESIGN.md 12).
    Message thread or capture worker thread (file I/O, allocates). */

/** <temp>/VocalSynth/ (created if missing). */
juce::File getTempRoot();

/** Writes via a temporary file next to target and moves it into place. FL may still hold the file of an
    earlier drag open; then a numbered sibling (File::getNonexistentSibling) is used instead.
    writeContent must create, write and close the file it is given and return true on success.
    Returns the file written (target or the sibling), or juce::File() on failure. */
juce::File writeAtomically (const juce::File& target, const std::function<bool (const juce::File&)>& writeContent);

/** Writes bytes with writeAtomically(). */
juce::File writeBytesAtomically (const juce::File& target, const void* data, size_t numBytes);

/** Moves source to target (rename; copy + delete as a fallback, e.g. across drives); if target exists, a
    numbered sibling is used. Returns the resulting file, or juce::File() on failure (source unchanged). */
juce::File moveOrCopy (const juce::File& source, const juce::File& target);

} // namespace vocalsynth::ExportFiles
