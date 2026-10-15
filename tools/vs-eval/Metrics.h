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
    double frequencyHz = 0.0; ///< 0 = no pitch estimate
    bool voiced = false;      ///< the pipeline's voicing decision for this frame (NoteSegmenter)
};

struct PitchMetrics
{
    int voicedReferenceFrames = 0;   ///< frames inside a labelled note (outside the collar)
    int unvoicedReferenceFrames = 0; ///< frames outside every labelled note (outside the collar)
    double rawPitchAccuracy = 0.0;   ///< share of the voiced reference frames within +-50 cents of the label
    double octaveErrorRate = 0.0;    ///< share of the voiced reference frames off by 12 +- 0.5 or 24 +- 0.5 semitones
    double voicingFalseAlarm = 0.0;  ///< share of the unvoiced reference frames the pipeline called voiced
    int correctFrames = 0;           ///< counts behind the rates (for pooling over files)
    int octaveErrorFrames = 0;
    int falseAlarmFrames = 0;
};

/** Counts of one note category (see NoteMetrics). */
struct NoteCategory
{
    int referenceNotes = 0;
    int detectedNotes = 0;
    int matched = 0;
    double precision = 0.0;
    double recall = 0.0;
    double f1 = 0.0;
};

struct NoteMetrics
{
    /** The matching is global; the categories split it by how a note starts. "fromRest": the reference note
        starts after a rest (or the detected note after silence); "change": it starts where the previous
        note ends (legato line; for detections: a note change at the same sample, legato or retrigger).
        Note changes carry the stickiness delay T_min by design (35 ms at the default 50 %), so with the
        +-50 ms tolerance they are matched only partly; changeDelay shows by how much they are late. */
    NoteCategory fromRest, change;
    int changeDelayCount = 0;              ///< reference changes with a same-note detection 0..150 ms after
    double meanChangeDelaySeconds = 0.0;
    double maxChangeDelaySeconds = 0.0;
    double sumChangeDelaySeconds = 0.0;

    int referenceNotes = 0;
    int detectedNotes = 0;
    int matched = 0;          ///< same MIDI note, onset within +-onsetToleranceSeconds, one-to-one
    double precision = 0.0;
    double recall = 0.0;
    double f1 = 0.0;
    double meanOnsetLatencySeconds = 0.0; ///< detected - reference onset over the matched notes
    double maxOnsetLatencySeconds = 0.0;
    double sumOnsetLatencySeconds = 0.0;  ///< for pooling over files
};

/** Contents of a labels file. A comment line "# vs-eval: pitch-only" marks the labels as valid for the
    frame (pitch) metrics only: the notes are not scored (e.g. a glissando, whose note boundaries depend
    on the stickiness, not on the signal). */
struct LabelFile
{
    std::vector<NoteLabel> notes;
    bool pitchOnly = false;
};

/** Reads "onset_s,offset_s,midi_note" lines (a header line, empty lines and '#' comments allowed;
    separators ',', ';' or whitespace). Labels are sorted by onset. */
bool readLabelsCsv (const std::string& path, LabelFile& out, std::string& error);

/** Writes labels in the format readLabelsCsv() reads (with header, and the pitch-only directive). */
bool writeLabelsCsv (const std::string& path, const LabelFile& labels);

/** Frame metrics. Frames whose time lies within collarSeconds of a label boundary (onset or offset) are
    not scored at all: there the analysis window holds both sides of the boundary, and no detector with
    a window of 2 * collar can be right. vs-eval passes half the analysis window. */
PitchMetrics computePitchMetrics (const std::vector<FramePitch>& frames, const std::vector<NoteLabel>& labels,
                                  double collarSeconds = 0.0);

/** Pools frame counts of several files (rates recomputed from the counts). */
void accumulate (PitchMetrics& total, const PitchMetrics& add);
void finalise (PitchMetrics& m);

/** Note metrics: greedy one-to-one matching, closest onset first, same MIDI note, onset within
    +-onsetToleranceSeconds (the offset is not scored). */
NoteMetrics computeNoteMetrics (const std::vector<DetectedNote>& detected, const std::vector<NoteLabel>& labels,
                                double onsetToleranceSeconds = 0.050);

void accumulate (NoteMetrics& total, const NoteMetrics& add);
void finalise (NoteMetrics& m);

} // namespace vseval
