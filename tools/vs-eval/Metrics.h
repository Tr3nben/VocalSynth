#pragma once

// Evaluation metrics for vs-eval (docs/M2-DESIGN.md 5). JUCE-free.

#include <cstdint>
#include <string>
#include <vector>

namespace vseval
{

/** One labelled note (CSV line "onset_s,offset_s,midi_note"). */
struct NoteLabel
{
    double onsetSeconds = 0.0;
    double offsetSeconds = 0.0;
    int midiNote = 0;
};

/** One detected note, reconstructed from the pipeline's note-on/off events. */
struct DetectedNote
{
    double onsetSeconds = 0.0;
    double offsetSeconds = 0.0;
    int midiNote = 0;
    int velocity = 0;
};

/** One analysis frame for the pitch metrics. timeSeconds is the frame centre (hop time minus the
    analysis delay, see docs/M2-DESIGN.md 5). */
struct FramePitch
{
    double timeSeconds = 0.0;
    double frequencyHz = 0.0; ///< 0 = unvoiced
};

struct PitchMetrics
{
    int voicedReferenceFrames = 0;  ///< frames inside a labelled note
    double rawPitchAccuracy = 0.0;  ///< share of those frames within +-50 cents of the label
    double octaveErrorRate = 0.0;   ///< share of those frames off by 12 +- 0.5 semitones (or 24)
    double voicingFalseAlarm = 0.0; ///< share of unlabelled frames reported voiced
};

struct NoteMetrics
{
    int referenceNotes = 0;
    int detectedNotes = 0;
    int matched = 0;          ///< same MIDI note, onset within +-onsetToleranceSeconds, one-to-one
    double precision = 0.0;
    double recall = 0.0;
    double f1 = 0.0;
};

/** Reads "onset_s,offset_s,midi_note" lines (header line and '#' comments allowed). */
bool readLabelsCsv (const std::string& path, std::vector<NoteLabel>& out, std::string& error);

PitchMetrics computePitchMetrics (const std::vector<FramePitch>& frames, const std::vector<NoteLabel>& labels);

NoteMetrics computeNoteMetrics (const std::vector<DetectedNote>& detected, const std::vector<NoteLabel>& labels,
                                double onsetToleranceSeconds = 0.050);

} // namespace vseval
