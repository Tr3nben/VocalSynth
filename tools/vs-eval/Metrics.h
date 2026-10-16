#pragma once

// Evaluation metrics for vs-eval (docs/M2-DESIGN.md 5). JUCE-free.

#include <array>
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
    int matched = 0;           ///< matched reference notes of this category (recall)
    int matchedDetections = 0; ///< matched detected notes of this category (precision); the two differ when
                               ///< a reference note after a rest is matched by a detected note change
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
    double medianOnsetLatencySeconds = 0.0;
    double p95OnsetLatencySeconds = 0.0;
    std::vector<double> onsetLatencies;   ///< of the matched notes (kept for pooled percentiles)

    /** Onset latency independent of the +-50 ms matching: for every reference note that starts after a
        rest, the first detected note-on (any pitch) within [-restLatencyEarlySeconds, +restLatencyLateSeconds]
        of its onset. Counts how late a note-on comes when the singer starts a note, even when the pitch or
        the +-50 ms window does not match. */
    int restLatencyCount = 0;             ///< reference notes from rest with such a note-on
    int restLatencyMissing = 0;           ///< reference notes from rest without one
    double meanRestLatencySeconds = 0.0;
    double medianRestLatencySeconds = 0.0;
    double p95RestLatencySeconds = 0.0;
    std::vector<double> restLatencies;
};

/** Search window of NoteMetrics::restLatencies. */
inline constexpr double restLatencyEarlySeconds = 0.050;
inline constexpr double restLatencyLateSeconds = 0.200;

/** Nearest-rank percentile (0..100) of unsorted values; 0 for an empty list. */
double percentile (std::vector<double> values, double percent);

// --- Frame-level f0 reference (real recordings, tools/corpus) -------------------------------------------

/** A reference f0 track, CSV "time_s,f0_hz" (header, '#' comments and empty lines allowed; separators ','
    ';' or whitespace; extra columns ignored). f0 > 0: voiced at that pitch; 0: unvoiced; < 0: unknown (the
    reference sources disagree, see docs/CORPUS.md) - such frames are not scored at all. Times ascending. */
struct ReferenceF0
{
    std::vector<double> times;
    std::vector<double> f0;
    double hopSeconds = 0.0;          ///< median spacing of the samples
    std::vector<double> transitions;  ///< times where the reference switches voiced <-> unvoiced (midpoints)
};

bool readReferenceF0Csv (const std::string& path, ReferenceF0& out, std::string& error);

/** Fills hopSeconds and transitions from times/f0 (readReferenceF0Csv calls it). */
void finaliseReference (ReferenceF0& ref);

/** Reference value at time t: between two voiced samples linear in log-frequency, otherwise the nearest
    sample. Returns the f0 (> 0), 0 (unvoiced) or -1 (unknown, also when t is more than one hop outside the
    track or the nearest sample is more than 0.75 hop away). */
double referenceAt (const ReferenceF0& ref, double timeSeconds);

/** Frame metrics against a reference f0 (mir_eval-style definitions, +-50 cents):
    rawPitchAccuracy  share of the reference-voiced frames whose estimate is within +-50 cents
    rawChromaAccuracy the same with octave errors forgiven (any number of octaves)
    octaveErrorRate   rawChromaAccuracy - rawPitchAccuracy (estimate whole octaves off)
    voicingRecall     share of the reference-voiced frames the pipeline called voiced
    voicingFalseAlarm share of the reference-unvoiced frames the pipeline called voiced
    medianAbsCents    median |error| of the frames within +-50 cents (1-cent histogram)
    Frames at unknown reference values and within collarSeconds of a voicing transition are excluded. */
struct F0Metrics
{
    int referenceVoicedFrames = 0;
    int referenceUnvoicedFrames = 0;
    int excludedFrames = 0;           ///< unknown reference or inside the collar
    int pitchCorrectFrames = 0;
    int chromaCorrectFrames = 0;
    int voicedRecallFrames = 0;       ///< reference voiced and pipeline voiced
    int falseAlarmFrames = 0;
    std::array<int, 51> absCentsHistogram {}; ///< correct frames by |error| in whole cents (0..50)

    double rawPitchAccuracy = 0.0;
    double rawChromaAccuracy = 0.0;
    double octaveErrorRate = 0.0;
    double voicingRecall = 0.0;
    double voicingFalseAlarm = 0.0;
    double medianAbsCents = 0.0;
};

F0Metrics computeF0Metrics (const std::vector<FramePitch>& frames, const ReferenceF0& ref, double collarSeconds);
void accumulate (F0Metrics& total, const F0Metrics& add);
void finalise (F0Metrics& m);

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
