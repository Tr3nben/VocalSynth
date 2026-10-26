// vs-eval: runs the VocalSynth voice pipeline offline on WAV files and prints JSON metrics
// (docs/M2-DESIGN.md 5, real recordings: docs/CORPUS.md). JUCE-free.
//
// Usage:
//   vs-eval [options] <file.wav | dir> [...]   a directory adds every *.wav in it (sorted)
//   vs-eval --self-test [--corpus-dir <dir>] [--keep]
//                                     writes the synthetic corpus (WAV + CSV) to a temporary directory (or
//                                     <dir>), evaluates it from disk with both algorithms, prints the JSON and
//                                     exits with 1 if a gate fails (ctest vs-eval.self-test)
//   vs-eval --write-corpus <dir>      only writes the synthetic corpus as WAV + CSV labels
//   vs-eval --unit-test               checks the metric code on hand-made data (ctest vs-eval.unit-test)
// Options:
//   --range <auto|bass|baritone|tenor|alto|soprano|whistle|wide>
//                            default auto: the file name prefix ("bass_..." -> bass), else tenor
//   --algorithm <yin|mpm|both>                                default both
//   --labels <file.csv>      labels for the (single) input file; default: <file>.csv next to the WAV if present
//   --f0-ref <file.csv>      reference f0 for the (single) input file; default: <file>.f0.csv if present
//   --block <n>              host block size for the offline run, default 512
//   --key <0..11> --scale <0..8>                              pipeline settings
//   --stickiness <%[,%...]> --gate <dB[,dB...]> --min-note <ms[,ms...]>
//                            pipeline settings; a comma list sweeps the grid of all combinations
//   --jobs <n>               worker threads (files x algorithms x grid points in parallel), default 1
//   --json <file>            write the JSON there instead of stdout
//   --markdown <file|->      also write a Markdown summary (tables per algorithm, range, dataset, sweep)
//   --summary-only           leave out the per-file results
//   --notes                  also list the detected notes per file
//   --frames <prefix>        diagnostics for a single input file without a sweep: writes every analysis frame
//                            to <prefix><algorithm>.csv (time, raw and processed f0 / confidence / RMS, active
//                            note, voiced) and the notes to <prefix><algorithm>.notes.csv
//
// File names: "<range>_<dataset>_<rest>.wav" (tools/corpus writes them so). The range prefix selects the
// voice range (--range auto); the second token groups the results ("byDataset"); "byRange" groups by range.
//
// Metrics (per file, per algorithm, and pooled over all files, per range and per dataset):
//   pitch            detector frames (time = window centre) against the label notes: raw pitch accuracy
//                    (labelled frames within +-50 cents of the label note), octave error rate (12 or 24 +- 0.5
//                    semitones off), voicing false alarm (unlabelled frames the pipeline called voiced). Frames
//                    within half an analysis window (+ 1 hop) of a label boundary are not scored ("collarMs").
//   processedPitch   the same after the median + octave guard
//   f0, processedF0  only with a reference f0 (<file>.f0.csv): the same frames against the reference f0
//                    (raw pitch / raw chroma accuracy +-50 cents, octave error rate, voicing recall / false
//                    alarm, median |cents| of the correct frames); unknown reference frames (< 0) and frames
//                    within the collar of a reference voicing change are not scored
//   notes            note precision / recall / F1 (same MIDI note, onset within +-50 ms, one-to-one) and the
//                    onset latency of the matched notes (mean / median / p95), split into notes that start
//                    after a rest ("fromRest") and legato note changes ("change", which carry the stickiness
//                    delay T_min by design; "changeDelay" = how late they come); "restLatency" = first note-on
//                    (any pitch) -50..+200 ms around each reference note after a rest. Files labelled
//                    "# vs-eval: pitch-only" (glissandi) are not scored here; files with an empty label list
//                    count their notes as false positives ("notesOnUnvoicedFiles").
// Grid (sweep): the per-file results, byRange and byDataset are for the base point (each swept setting at
// its default value if the list contains it, else the first value); "sweep" has the pooled summary of
// every grid point.
// Self-test gates (both algorithms): raw pitch accuracy >= 0.95, octave errors < 2 %, note F1 of the notes
// from rest >= 0.85 (the overall F1 includes the legato changes and is reported, not gated), no notes on
// the files without voice.
// Exit codes: 0 ok, 1 a self-test / unit-test gate failed, 2 usage or I/O error.

#include "Corpus.h"
#include "Metrics.h"
#include "WavFile.h"

#include "vocalsynth/VoicePipeline.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace
{

using namespace vseval;
using vocalsynth::PitchAlgorithm;
using vocalsynth::VoiceRange;

// --- Gates of the self-test (docs/M2-DESIGN.md 5), for BOTH algorithms -----------------------------------
constexpr double minRawPitchAccuracy = 0.95;
constexpr double maxOctaveErrorRate = 0.02;
constexpr double minNoteF1 = 0.85;

struct Options
{
    std::string range = "auto";
    std::string algorithm = "both";
    std::string labels;
    std::string f0Ref;
    int block = 512;
    int jobs = 1;
    vocalsynth::VoiceSettings settings {};
    std::vector<float> gates, stickiness, minNotes; ///< sweep lists (empty = the single value in settings)
    bool listNotes = false;
    std::string framesPrefix;
    bool selfTest = false;
    bool unitTest = false;
    bool keepCorpus = false;
    bool summaryOnly = false;
    std::string corpusDir;
    std::string writeCorpusDir;
    std::string jsonPath;
    std::string markdownPath;
    std::vector<std::string> files;
};

/** Everything one offline run produces. */
struct RunResult
{
    std::vector<FramePitch> raw;
    std::vector<FramePitch> processed;
    std::vector<DetectedNote> notes;
    double collarSeconds = 0.0;
    double latencyModelMs = 0.0;
    std::string framesCsv; ///< only with --frames
};

struct FrameRecord
{
    int64_t hopSample;
    vocalsynth::PitchFrame raw;
    vocalsynth::PitchFrame processed;
    int activeNote;
    bool voiced;
};

void recordFrame (void* context, const vocalsynth::AnalysisFrameInfo& info) noexcept
{
    static_cast<std::vector<FrameRecord>*> (context)->push_back (
        { info.hopSample, info.raw, info.processed, info.activeNote, info.voiced });
}

RunResult runPipeline (const std::vector<float>& mono, double sampleRate, const vocalsynth::VoiceSettings& settings, int block,
                       bool withFramesCsv = false)
{
    vocalsynth::VoicePipeline pipeline;
    pipeline.prepare (sampleRate, block);

    std::vector<FrameRecord> frames;
    frames.reserve (mono.size() / static_cast<std::size_t> (std::max (1, pipeline.getHopInputSamples())) + 16);
    pipeline.setFrameObserver (recordFrame, &frames);

    RunResult result;
    vocalsynth::VsEventBuffer<1024> events;
    std::vector<int> openNote (128, -1); // note -> index in result.notes

    const auto handle = [&] (int64_t blockStart)
    {
        for (const auto& e : events)
        {
            const auto t = static_cast<double> (blockStart + e.sampleOffset) / sampleRate;

            if (e.type == vocalsynth::VsEventType::noteOn)
            {
                openNote[e.data1] = static_cast<int> (result.notes.size());
                result.notes.push_back ({ t, t, e.data1, e.data2 });
            }
            else if (e.type == vocalsynth::VsEventType::noteOff && openNote[e.data1] >= 0)
            {
                result.notes[static_cast<std::size_t> (openNote[e.data1])].offsetSeconds = t;
                openNote[e.data1] = -1;
            }
        }
    };

    for (std::size_t pos = 0; pos < mono.size(); pos += static_cast<std::size_t> (block))
    {
        const auto n = static_cast<int> (std::min (mono.size() - pos, static_cast<std::size_t> (block)));
        const auto start = pipeline.getSamplePosition();
        events.clear();
        pipeline.process (mono.data() + pos, n, settings, events);
        handle (start);
    }

    events.clear();
    pipeline.flush (0, events);
    handle (pipeline.getSamplePosition());

    // Frame time stamps: the window centre (and for the processed frames also the median delay)
    const auto centre = pipeline.getFrameCentreDelayInputSamples();
    const auto medianDelay = static_cast<double> (vocalsynth::PitchPostProcessor::defaultMedianLength / 2)
                             * static_cast<double> (pipeline.getHopInputSamples());

    for (const auto& f : frames)
    {
        const auto t = (static_cast<double> (f.hopSample) - centre) / sampleRate;
        result.raw.push_back ({ t, static_cast<double> (f.raw.frequencyHz), f.voiced });
        result.processed.push_back ({ t - medianDelay / sampleRate, static_cast<double> (f.processed.frequencyHz), f.voiced });
    }

    if (withFramesCsv)
    {
        // Emit time of the frame (input sample that completed the hop) and the window centre of the raw frame
        result.framesCsv = "emit_s,centre_s,raw_hz,raw_conf,raw_db,proc_hz,proc_conf,proc_db,active_note,voiced\n";
        char line[256];

        for (const auto& f : frames)
        {
            std::snprintf (line, sizeof (line), "%.6f,%.6f,%.3f,%.4f,%.2f,%.3f,%.4f,%.2f,%d,%d\n",
                           static_cast<double> (f.hopSample) / sampleRate,
                           (static_cast<double> (f.hopSample) - centre) / sampleRate,
                           static_cast<double> (f.raw.frequencyHz), static_cast<double> (f.raw.confidence),
                           static_cast<double> (f.raw.rmsDb), static_cast<double> (f.processed.frequencyHz),
                           static_cast<double> (f.processed.confidence), static_cast<double> (f.processed.rmsDb),
                           f.activeNote, f.voiced ? 1 : 0);
            result.framesCsv += line;
        }
    }

    const auto factor = static_cast<double> (pipeline.getDecimationFactor());
    result.collarSeconds = (factor * static_cast<double> (pipeline.getWindowSamples()) / 2.0
                            + static_cast<double> (pipeline.getHopInputSamples()))
                           / sampleRate;
    result.latencyModelMs = pipeline.getAlgorithmicLatencyMs();
    return result;
}

// --- JSON -----------------------------------------------------------------------------------------------

std::string jsonString (const std::string& s)
{
    std::string out = "\"";

    for (const auto c : s)
    {
        if (c == '"' || c == '\\')
        {
            out += '\\';
            out += c;
        }
        else if (static_cast<unsigned char> (c) < 0x20)
        {
            char b[8];
            std::snprintf (b, sizeof (b), "\\u%04x", static_cast<unsigned> (static_cast<unsigned char> (c)));
            out += b;
        }
        else
        {
            out += c;
        }
    }

    return out + "\"";
}

std::string number (double v, int decimals = 4)
{
    if (! std::isfinite (v))
        return "null";

    char b[64];
    std::snprintf (b, sizeof (b), "%.*f", decimals, v);
    return b;
}

std::string pitchJson (const PitchMetrics& m)
{
    return "{\"voicedFrames\":" + std::to_string (m.voicedReferenceFrames) + ",\"unvoicedFrames\":"
           + std::to_string (m.unvoicedReferenceFrames) + ",\"rawPitchAccuracy\":" + number (m.rawPitchAccuracy)
           + ",\"octaveErrorRate\":" + number (m.octaveErrorRate) + ",\"voicingFalseAlarm\":"
           + number (m.voicingFalseAlarm) + "}";
}

std::string f0Json (const F0Metrics& m)
{
    return "{\"voicedFrames\":" + std::to_string (m.referenceVoicedFrames) + ",\"unvoicedFrames\":"
           + std::to_string (m.referenceUnvoicedFrames) + ",\"excludedFrames\":" + std::to_string (m.excludedFrames)
           + ",\"rawPitchAccuracy\":" + number (m.rawPitchAccuracy) + ",\"rawChromaAccuracy\":"
           + number (m.rawChromaAccuracy) + ",\"octaveErrorRate\":" + number (m.octaveErrorRate) + ",\"voicingRecall\":"
           + number (m.voicingRecall) + ",\"voicingFalseAlarm\":" + number (m.voicingFalseAlarm)
           + ",\"medianAbsCents\":" + number (m.medianAbsCents, 1) + "}";
}

std::string categoryJson (const NoteCategory& c)
{
    return "{\"reference\":" + std::to_string (c.referenceNotes) + ",\"detected\":" + std::to_string (c.detectedNotes)
           + ",\"matched\":" + std::to_string (c.matched) + ",\"matchedDetections\":" + std::to_string (c.matchedDetections)
           + ",\"precision\":" + number (c.precision)
           + ",\"recall\":" + number (c.recall) + ",\"f1\":" + number (c.f1) + "}";
}

std::string ms (double seconds) { return number (1000.0 * seconds, 2); }

std::string notesJson (const NoteMetrics& m)
{
    return "{\"reference\":" + std::to_string (m.referenceNotes) + ",\"detected\":" + std::to_string (m.detectedNotes)
           + ",\"matched\":" + std::to_string (m.matched) + ",\"precision\":" + number (m.precision)
           + ",\"recall\":" + number (m.recall) + ",\"f1\":" + number (m.f1) + ",\"meanOnsetLatencyMs\":"
           + ms (m.meanOnsetLatencySeconds) + ",\"medianOnsetLatencyMs\":" + ms (m.medianOnsetLatencySeconds)
           + ",\"p95OnsetLatencyMs\":" + ms (m.p95OnsetLatencySeconds) + ",\"maxOnsetLatencyMs\":"
           + ms (m.maxOnsetLatencySeconds) + ",\"fromRest\":" + categoryJson (m.fromRest)
           + ",\"change\":" + categoryJson (m.change) + ",\"changeDelay\":{\"count\":" + std::to_string (m.changeDelayCount)
           + ",\"meanMs\":" + ms (m.meanChangeDelaySeconds) + ",\"maxMs\":" + ms (m.maxChangeDelaySeconds)
           + "},\"restLatency\":{\"count\":" + std::to_string (m.restLatencyCount) + ",\"missing\":"
           + std::to_string (m.restLatencyMissing) + ",\"meanMs\":" + ms (m.meanRestLatencySeconds) + ",\"medianMs\":"
           + ms (m.medianRestLatencySeconds) + ",\"p95Ms\":" + ms (m.p95RestLatencySeconds) + "}}";
}

// --- Evaluation -----------------------------------------------------------------------------------------

struct Totals
{
    int files = 0;
    double seconds = 0.0;
    PitchMetrics pitch, processed;
    int f0Files = 0;
    F0Metrics f0, processedF0;
    NoteMetrics notes;
    int unvoicedFiles = 0;
    int notesOnUnvoicedFiles = 0;
    int unlabelledDetectedNotes = 0; ///< files without a labels file: only counted
};

void finalise (Totals& t)
{
    finalise (t.pitch);
    finalise (t.processed);
    finalise (t.f0);
    finalise (t.processedF0);
    finalise (t.notes);
}

struct InputFile
{
    std::string path;
    std::string labelsPath; ///< empty = no labels
    std::string f0Path;     ///< empty = no reference f0
};

const char* algorithmName (PitchAlgorithm a) { return a == PitchAlgorithm::mpm ? "mpm" : "yin"; }

bool parseRange (const std::string& name, VoiceRange& out)
{
    for (int i = 0; i < vocalsynth::numVoiceRanges; ++i)
        if (name == vocalsynth::voiceRangeTable[static_cast<std::size_t> (i)].name)
        {
            out = static_cast<VoiceRange> (i);
            return true;
        }

    return false;
}

VoiceRange rangeForFile (const Options& options, const std::string& path)
{
    VoiceRange range = VoiceRange::tenor;

    if (options.range != "auto")
    {
        parseRange (options.range, range);
        return range;
    }

    const auto stem = std::filesystem::path (path).stem().string();
    const auto prefix = stem.substr (0, stem.find_first_of ("_-"));
    parseRange (prefix, range);
    return range;
}

/** Second '_' token of the file name ("tenor_dcs-solo_x.wav" -> "dcs-solo"), "other" if there is none. */
std::string datasetForFile (const std::string& path)
{
    const auto stem = std::filesystem::path (path).stem().string();
    const auto first = stem.find ('_');

    if (first == std::string::npos)
        return "other";

    const auto second = stem.find ('_', first + 1);

    if (second == std::string::npos || second == first + 1)
        return "other";

    return stem.substr (first + 1, second - first - 1);
}

/** One task: one file x one algorithm x one grid point. */
struct TaskResult
{
    bool ok = true;
    std::string error;
    std::string headerJson; ///< file-level fields (identical for every algorithm)
    std::string json;       ///< the ,"yin":{...} fragment
    VoiceRange range = VoiceRange::tenor;
    double seconds = 0.0;
    bool hasLabels = false, hasF0 = false, pitchOnly = false, unvoicedFile = false;
    PitchMetrics pitch, processed;
    F0Metrics f0, processedF0;
    NoteMetrics notes;
    int detectedNotes = 0;
};

bool writeText (const std::string& path, const std::string& text);

TaskResult runTask (const Options& options, const InputFile& file, const vocalsynth::VoiceSettings& gridSettings,
                    PitchAlgorithm algorithm)
{
    TaskResult r;
    WavData wav;

    if (! readWav (file.path, wav, r.error))
    {
        r.ok = false;
        return r;
    }

    LabelFile labels;
    r.hasLabels = ! file.labelsPath.empty();

    if (r.hasLabels && ! readLabelsCsv (file.labelsPath, labels, r.error))
    {
        r.ok = false;
        return r;
    }

    ReferenceF0 reference;
    r.hasF0 = ! file.f0Path.empty();

    if (r.hasF0 && ! readReferenceF0Csv (file.f0Path, reference, r.error))
    {
        r.ok = false;
        return r;
    }

    auto settings = gridSettings;
    settings.range = rangeForFile (options, file.path);
    settings.algorithm = algorithm;
    r.range = settings.range;
    r.seconds = static_cast<double> (wav.mono.size()) / wav.sampleRate;
    r.pitchOnly = labels.pitchOnly;
    r.headerJson = "{\"file\":" + jsonString (file.path) + ",\"labels\":" + (r.hasLabels ? jsonString (file.labelsPath) : "null")
                   + ",\"f0Reference\":" + (r.hasF0 ? jsonString (file.f0Path) : "null") + ",\"range\":"
                   + jsonString (vocalsynth::voiceRangeInfo (settings.range).name) + ",\"dataset\":"
                   + jsonString (datasetForFile (file.path)) + ",\"sampleRate\":" + number (wav.sampleRate, 0)
                   + ",\"channels\":" + std::to_string (wav.numChannels) + ",\"seconds\":" + number (r.seconds, 3)
                   + ",\"pitchOnly\":" + (labels.pitchOnly ? "true" : "false");

    const auto run = runPipeline (wav.mono, wav.sampleRate, settings, options.block, ! options.framesPrefix.empty());

    if (! options.framesPrefix.empty())
    {
        std::string notesCsv = "onset_s,offset_s,midi_note,velocity\n";

        for (const auto& n : run.notes)
            notesCsv += number (n.onsetSeconds, 6) + "," + number (n.offsetSeconds, 6) + "," + std::to_string (n.midiNote)
                        + "," + std::to_string (n.velocity) + "\n";

        const auto prefix = options.framesPrefix + algorithmName (algorithm);

        if (! writeText (prefix + ".csv", run.framesCsv) || ! writeText (prefix + ".notes.csv", notesCsv))
        {
            r.ok = false;
            r.error = "cannot write " + prefix + ".csv";
            return r;
        }
    }
    r.detectedNotes = static_cast<int> (run.notes.size());
    r.json = std::string (",\"") + algorithmName (algorithm) + "\":{\"detectedNotes\":" + std::to_string (run.notes.size())
             + ",\"latencyModelMs\":" + number (run.latencyModelMs, 2) + ",\"collarMs\":" + ms (run.collarSeconds);

    if (r.hasLabels)
    {
        r.pitch = computePitchMetrics (run.raw, labels.notes, run.collarSeconds);
        r.processed = computePitchMetrics (run.processed, labels.notes, run.collarSeconds);
        r.json += ",\"pitch\":" + pitchJson (r.pitch) + ",\"processedPitch\":" + pitchJson (r.processed);
        r.unvoicedFile = labels.notes.empty();

        if (! labels.pitchOnly)
        {
            r.notes = computeNoteMetrics (run.notes, labels.notes);
            r.json += ",\"notes\":" + notesJson (r.notes);
        }
    }

    if (r.hasF0)
    {
        r.f0 = computeF0Metrics (run.raw, reference, run.collarSeconds);
        r.processedF0 = computeF0Metrics (run.processed, reference, run.collarSeconds);
        r.json += ",\"f0\":" + f0Json (r.f0) + ",\"processedF0\":" + f0Json (r.processedF0);
    }

    if (options.listNotes)
    {
        r.json += ",\"noteList\":[";

        for (std::size_t n = 0; n < run.notes.size(); ++n)
            r.json += (n > 0 ? "," : "") + std::string ("[") + number (run.notes[n].onsetSeconds) + ","
                      + number (run.notes[n].offsetSeconds) + "," + std::to_string (run.notes[n].midiNote) + ","
                      + std::to_string (run.notes[n].velocity) + "]";

        r.json += "]";
    }

    r.json += "}";
    return r;
}

void add (Totals& total, const TaskResult& r)
{
    ++total.files;
    total.seconds += r.seconds;

    if (r.hasLabels)
    {
        accumulate (total.pitch, r.pitch);
        accumulate (total.processed, r.processed);

        if (r.unvoicedFile)
        {
            ++total.unvoicedFiles;
            total.notesOnUnvoicedFiles += r.detectedNotes;
        }

        if (! r.pitchOnly)
            accumulate (total.notes, r.notes);
    }
    else
    {
        total.unlabelledDetectedNotes += r.detectedNotes;
    }

    if (r.hasF0)
    {
        ++total.f0Files;
        accumulate (total.f0, r.f0);
        accumulate (total.processedF0, r.processedF0);
    }
}

/** One grid point of the sweep. */
struct GridPoint
{
    vocalsynth::VoiceSettings settings;
    std::vector<Totals> totals; ///< per algorithm
    std::map<std::string, std::vector<Totals>> byRange, byDataset;
};

/** Runs every (grid point, file, algorithm) task on options.jobs threads; results in a fixed order, so the
    output does not depend on the thread count. Returns false on the first I/O error. */
bool evaluate (const Options& options, const std::vector<InputFile>& files, const std::vector<PitchAlgorithm>& algorithms,
               std::vector<GridPoint>& grid, std::size_t baseIndex, std::string& filesJson, std::string& error)
{
    const auto perPoint = files.size() * algorithms.size();
    std::vector<TaskResult> results (grid.size() * perPoint);
    std::atomic<std::size_t> next { 0 };

    const auto worker = [&]
    {
        for (auto i = next++; i < results.size(); i = next++)
        {
            const auto point = i / perPoint;
            const auto file = (i % perPoint) / algorithms.size();
            const auto algorithm = i % algorithms.size();
            results[i] = runTask (options, files[file], grid[point].settings, algorithms[algorithm]);
        }
    };

    const auto threads = static_cast<std::size_t> (std::clamp (options.jobs, 1, 64));
    std::vector<std::thread> pool;

    for (std::size_t t = 1; t < std::min (threads, results.size()); ++t)
        pool.emplace_back (worker);

    worker();

    for (auto& t : pool)
        t.join();

    for (const auto& r : results)
        if (! r.ok)
        {
            error = r.error;
            return false;
        }

    for (std::size_t p = 0; p < grid.size(); ++p)
    {
        auto& point = grid[p];
        point.totals.assign (algorithms.size(), {});

        for (std::size_t f = 0; f < files.size(); ++f)
            for (std::size_t a = 0; a < algorithms.size(); ++a)
            {
                const auto& r = results[p * perPoint + f * algorithms.size() + a];
                add (point.totals[a], r);

                auto& range = point.byRange[vocalsynth::voiceRangeInfo (r.range).name];
                auto& dataset = point.byDataset[datasetForFile (files[f].path)];
                range.resize (algorithms.size());
                dataset.resize (algorithms.size());
                add (range[a], r);
                add (dataset[a], r);
            }

        for (auto& t : point.totals)
            finalise (t);

        for (auto* groups : { &point.byRange, &point.byDataset })
            for (auto& [name, totals] : *groups)
                for (auto& t : totals)
                    finalise (t);
    }

    filesJson = "\"files\":[";

    if (! options.summaryOnly)
        for (std::size_t f = 0; f < files.size(); ++f)
        {
            const auto first = baseIndex * perPoint + f * algorithms.size();
            filesJson += (f > 0 ? "," : "") + results[first].headerJson;

            for (std::size_t a = 0; a < algorithms.size(); ++a)
                filesJson += results[first + a].json;

            filesJson += "}";
        }

    filesJson += "]";
    return true;
}

struct GateResult
{
    bool passed = true;
    std::string json;
};

GateResult checkGates (const Totals& t)
{
    GateResult g;
    const auto rpa = t.pitch.rawPitchAccuracy >= minRawPitchAccuracy;
    const auto octave = t.pitch.octaveErrorRate < maxOctaveErrorRate;
    const auto f1 = t.notes.fromRest.f1 >= minNoteF1;
    const auto noise = t.notesOnUnvoicedFiles == 0;
    g.passed = rpa && octave && f1 && noise;
    g.json = std::string ("{\"passed\":") + (g.passed ? "true" : "false") + ",\"rawPitchAccuracy\":" + (rpa ? "true" : "false")
             + ",\"octaveErrorRate\":" + (octave ? "true" : "false") + ",\"noteF1FromRest\":" + (f1 ? "true" : "false")
             + ",\"noNotesOnNoise\":" + (noise ? "true" : "false") + "}";
    return g;
}

std::string totalsJson (const Totals& t)
{
    auto json = "{\"files\":" + std::to_string (t.files) + ",\"minutes\":" + number (t.seconds / 60.0, 2) + ",\"pitch\":"
                + pitchJson (t.pitch) + ",\"processedPitch\":" + pitchJson (t.processed) + ",\"notes\":" + notesJson (t.notes)
                + ",\"unvoicedFiles\":" + std::to_string (t.unvoicedFiles) + ",\"notesOnUnvoicedFiles\":"
                + std::to_string (t.notesOnUnvoicedFiles) + ",\"detectedNotesWithoutLabels\":"
                + std::to_string (t.unlabelledDetectedNotes);

    if (t.f0Files > 0)
        json += ",\"f0Files\":" + std::to_string (t.f0Files) + ",\"f0\":" + f0Json (t.f0) + ",\"processedF0\":"
                + f0Json (t.processedF0);

    return json;
}

std::string summaryJson (const std::vector<PitchAlgorithm>& algorithms, const std::vector<Totals>& totals, bool withGates,
                         bool& allPassed)
{
    std::string json = "\"summary\":{";
    allPassed = true;
    std::vector<GateResult> gates;

    for (std::size_t a = 0; a < algorithms.size(); ++a)
    {
        const auto& t = totals[a];
        json += (a > 0 ? "," : "") + std::string ("\"") + algorithmName (algorithms[a]) + "\":" + totalsJson (t);

        if (withGates)
        {
            gates.push_back (checkGates (t));
            allPassed = allPassed && gates.back().passed;
            json += ",\"gates\":" + gates.back().json;
        }

        json += "}";
    }

    json += "}";

    if (algorithms.size() == 2)
    {
        const auto& y = totals[0];
        const auto& m = totals[1];
        const auto better = [] (double a, double b, bool higherIsBetter)
        { return a == b ? "tie" : ((a > b) == higherIsBetter ? "yin" : "mpm"); };

        // Recommendation: passes the gates (if evaluated), then note F1, then raw pitch accuracy, then fewer
        // octave errors, then fewer false alarms
        const auto key = [] (const Totals& t)
        {
            return std::vector<double> { t.notesOnUnvoicedFiles == 0 ? 1.0 : 0.0, std::round (t.notes.f1 * 1000.0),
                                         std::round (t.notes.fromRest.f1 * 1000.0),
                                         std::round (t.pitch.rawPitchAccuracy * 1000.0),
                                         -std::round (t.pitch.octaveErrorRate * 1000.0),
                                         -std::round (t.pitch.voicingFalseAlarm * 1000.0) };
        };

        auto ky = key (y), km = key (m);

        if (withGates)
        {
            ky.insert (ky.begin(), gates[0].passed ? 1.0 : 0.0);
            km.insert (km.begin(), gates[1].passed ? 1.0 : 0.0);
        }

        json += ",\"comparison\":{\"rawPitchAccuracy\":\"" + std::string (better (y.pitch.rawPitchAccuracy, m.pitch.rawPitchAccuracy, true))
                + "\",\"octaveErrorRate\":\"" + better (y.pitch.octaveErrorRate, m.pitch.octaveErrorRate, false)
                + "\",\"voicingFalseAlarm\":\"" + better (y.pitch.voicingFalseAlarm, m.pitch.voicingFalseAlarm, false)
                + "\",\"noteF1\":\"" + better (y.notes.f1, m.notes.f1, true) + "\",\"noteF1FromRest\":\""
                + better (y.notes.fromRest.f1, m.notes.fromRest.f1, true) + "\",\"changeDelay\":\""
                + better (y.notes.meanChangeDelaySeconds, m.notes.meanChangeDelaySeconds, false) + "\",\"meanOnsetLatency\":\""
                + better (y.notes.meanOnsetLatencySeconds, m.notes.meanOnsetLatencySeconds, false);

        if (y.f0Files > 0)
            json += "\",\"f0RawPitchAccuracy\":\"" + std::string (better (y.f0.rawPitchAccuracy, m.f0.rawPitchAccuracy, true))
                    + "\",\"f0VoicingRecall\":\"" + better (y.f0.voicingRecall, m.f0.voicingRecall, true);

        json += "\",\"recommendedDefault\":\"" + std::string (km > ky ? "mpm" : "yin") + "\"}";
    }

    return json;
}

std::string groupsJson (const char* name, const std::map<std::string, std::vector<Totals>>& groups,
                        const std::vector<PitchAlgorithm>& algorithms)
{
    std::string json = std::string ("\"") + name + "\":{";
    bool firstGroup = true;

    for (const auto& [group, totals] : groups)
    {
        json += (firstGroup ? "" : ",") + jsonString (group) + ":{";
        firstGroup = false;

        for (std::size_t a = 0; a < algorithms.size(); ++a)
            json += (a > 0 ? "," : "") + std::string ("\"") + algorithmName (algorithms[a]) + "\":" + totalsJson (totals[a]) + "}";

        json += "}";
    }

    return json + "}";
}

std::string settingsJson (const vocalsynth::VoiceSettings& s)
{
    return "{\"gateDb\":" + number (s.gateThresholdDb, 1) + ",\"stickinessPercent\":" + number (s.stickinessPercent, 1)
           + ",\"minNoteMs\":" + number (s.minNoteLengthMs, 1) + ",\"key\":" + std::to_string (s.key) + ",\"scale\":"
           + std::to_string (static_cast<int> (s.scale)) + "}";
}

// --- Markdown summary -----------------------------------------------------------------------------------

std::string pct (double v) { return number (100.0 * v, 1); }

std::string markdownHeader()
{
    return "| Group | Alg | Files | Min | RPA % | RCA % | Oct % | VRec % | VFA % | Note P/R/F1 | F1 rest | Lat med/p95 ms | Rest-lat med/p95 ms | Change delay ms |\n"
           "|---|---|---:|---:|---:|---:|---:|---:|---:|---|---:|---|---|---:|\n";
}

/** One table row; the pitch columns come from the reference f0 if the group has one, else from the labels. */
std::string markdownRow (const std::string& group, const char* algorithm, const Totals& t)
{
    const auto hasF0 = t.f0Files > 0;
    const auto& n = t.notes;
    char buffer[512];
    std::snprintf (buffer, sizeof (buffer), "| %s | %s | %d | %.1f | %s | %s | %s | %s | %s | %.2f / %.2f / %.2f | %.2f | %.0f / %.0f | %.0f / %.0f | %.0f |\n",
                   group.c_str(), algorithm, t.files, t.seconds / 60.0,
                   pct (hasF0 ? t.f0.rawPitchAccuracy : t.pitch.rawPitchAccuracy).c_str(),
                   hasF0 ? pct (t.f0.rawChromaAccuracy).c_str() : "-",
                   pct (hasF0 ? t.f0.octaveErrorRate : t.pitch.octaveErrorRate).c_str(),
                   hasF0 ? pct (t.f0.voicingRecall).c_str() : "-",
                   pct (hasF0 ? t.f0.voicingFalseAlarm : t.pitch.voicingFalseAlarm).c_str(), n.precision, n.recall, n.f1,
                   n.fromRest.f1, 1000.0 * n.medianOnsetLatencySeconds, 1000.0 * n.p95OnsetLatencySeconds,
                   1000.0 * n.medianRestLatencySeconds, 1000.0 * n.p95RestLatencySeconds, 1000.0 * n.meanChangeDelaySeconds);
    return buffer;
}

std::string settingsText (const vocalsynth::VoiceSettings& s)
{
    char buffer[128];
    std::snprintf (buffer, sizeof (buffer), "gate %.0f dB, stickiness %.0f %%, min note %.0f ms", static_cast<double> (s.gateThresholdDb),
                   static_cast<double> (s.stickinessPercent), static_cast<double> (s.minNoteLengthMs));
    return buffer;
}

std::string markdownReport (const std::vector<PitchAlgorithm>& algorithms, const std::vector<GridPoint>& grid,
                            std::size_t baseIndex, int blockSize)
{
    const auto& base = grid[baseIndex];
    std::string md = "## VocalSynth vs-eval\n\n";
    md += "Base settings: " + settingsText (base.settings) + ", block " + std::to_string (blockSize)
          + ". RPA/RCA = raw pitch/chroma accuracy (+-50 cents), Oct = octave errors, VRec/VFA = voicing recall / "
            "false alarm (against the reference f0 where there is one, else against the label notes), notes matched "
            "+-50 ms; Lat = onset latency of the matched notes, Rest-lat = first note-on after each reference note "
            "that starts after a rest.\n\n";

    md += "### Overall\n\n" + markdownHeader();

    for (std::size_t a = 0; a < algorithms.size(); ++a)
        md += markdownRow ("all", algorithmName (algorithms[a]), base.totals[a]);

    for (const auto& [title, groups] : { std::pair { "By voice range", &base.byRange }, std::pair { "By dataset", &base.byDataset } })
    {
        md += std::string ("\n### ") + title + "\n\n" + markdownHeader();

        for (const auto& [group, totals] : *groups)
            for (std::size_t a = 0; a < algorithms.size(); ++a)
                md += markdownRow (group, algorithmName (algorithms[a]), totals[a]);
    }

    if (grid.size() > 1)
    {
        md += "\n### Sweep (all files)\n\nSorted by note F1, then by RPA. * = base settings.\n\n"
              "| Settings | Alg | RPA % | VRec % | VFA % | Note F1 | F1 rest | Lat med ms | Rest-lat med/p95 ms | Change delay ms |\n"
              "|---|---|---:|---:|---:|---:|---:|---:|---|---:|\n";

        struct Row
        {
            double f1, rpa;
            std::string text;
        };

        std::vector<Row> rows;

        for (std::size_t p = 0; p < grid.size(); ++p)
            for (std::size_t a = 0; a < algorithms.size(); ++a)
            {
                const auto& t = grid[p].totals[a];
                const auto hasF0 = t.f0Files > 0;
                const auto rpa = hasF0 ? t.f0.rawPitchAccuracy : t.pitch.rawPitchAccuracy;
                char buffer[384];
                std::snprintf (buffer, sizeof (buffer), "| %s%s | %s | %s | %s | %s | %.3f | %.3f | %.0f | %.0f / %.0f | %.0f |\n",
                               settingsText (grid[p].settings).c_str(), p == baseIndex ? " *" : "", algorithmName (algorithms[a]),
                               pct (rpa).c_str(), hasF0 ? pct (t.f0.voicingRecall).c_str() : "-",
                               pct (hasF0 ? t.f0.voicingFalseAlarm : t.pitch.voicingFalseAlarm).c_str(), t.notes.f1,
                               t.notes.fromRest.f1, 1000.0 * t.notes.medianOnsetLatencySeconds,
                               1000.0 * t.notes.medianRestLatencySeconds, 1000.0 * t.notes.p95RestLatencySeconds,
                               1000.0 * t.notes.meanChangeDelaySeconds);
                rows.push_back ({ t.notes.f1, rpa, buffer });
            }

        std::stable_sort (rows.begin(), rows.end(), [] (const Row& x, const Row& y)
                          { return x.f1 > y.f1 || (x.f1 == y.f1 && x.rpa > y.rpa); });

        for (const auto& r : rows)
            md += r.text;
    }

    return md;
}

// --- Corpus on disk -------------------------------------------------------------------------------------

bool writeCorpus (const std::string& dir, std::vector<InputFile>& written, std::string& error)
{
    std::error_code ec;
    std::filesystem::create_directories (dir, ec);

    if (ec)
    {
        error = "cannot create " + dir + ": " + ec.message();
        return false;
    }

    for (const auto& item : buildSyntheticCorpus())
    {
        const auto base = (std::filesystem::path (dir) / item.name).string();

        if (! writeWavMono (base + ".wav", item.samples, item.sampleRate) || ! writeLabelsCsv (base + ".csv", item.labels))
        {
            error = "cannot write " + base + ".wav/.csv";
            return false;
        }

        // Round trip: the evaluation reads exactly what was generated (float32 is lossless here)
        WavData back;

        if (! readWav (base + ".wav", back, error) || back.mono != item.samples || back.sampleRate != item.sampleRate)
        {
            error = "WAV round trip failed for " + base + ".wav " + error;
            return false;
        }

        written.push_back ({ base + ".wav", base + ".csv", {} });
    }

    return true;
}

std::string uniqueTempDir()
{
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    return (std::filesystem::temp_directory_path() / ("vs-eval-selftest-" + std::to_string (ticks))).string();
}

// --- Unit test of the metric code (ctest vs-eval.unit-test) ---------------------------------------------

int runUnitTest()
{
    int failures = 0;
    const auto check = [&] (bool ok, const char* what)
    {
        if (! ok)
        {
            ++failures;
            std::fprintf (stderr, "vs-eval unit test FAILED: %s\n", what);
        }
    };
    const auto near = [] (double a, double b, double tolerance) { return std::abs (a - b) <= tolerance; };

    // Reference CSV: header, comment, unknown value, voicing transitions
    const auto dir = uniqueTempDir();
    std::filesystem::create_directories (dir);
    const auto refPath = (std::filesystem::path (dir) / "tenor_test_ref.f0.csv").string();
    {
        std::ofstream out (refPath);
        out << "# reference\ntime_s,f0_hz\n";

        for (int i = 0; i < 100; ++i) // 10 ms grid: 0..0.29 unvoiced, 0.30..0.69 voiced 220 Hz, 0.70..0.99 unvoiced
        {
            const auto t = 0.01 * static_cast<double> (i);
            const auto f = (i >= 30 && i < 70) ? (i == 50 ? -1.0 : 220.0) : 0.0;
            out << t << "," << f << "\n";
        }
    }

    ReferenceF0 ref;
    std::string error;
    check (readReferenceF0Csv (refPath, ref, error), "readReferenceF0Csv");
    check (ref.times.size() == 100 && near (ref.hopSeconds, 0.01, 1.0e-9), "reference size / hop");
    check (ref.transitions.size() == 2 && near (ref.transitions[0], 0.295, 1.0e-9) && near (ref.transitions[1], 0.695, 1.0e-9),
           "reference transitions");
    check (near (referenceAt (ref, 0.405), 220.0, 1.0e-9), "referenceAt interpolates between voiced samples");
    check (referenceAt (ref, 0.5) < 0.0, "referenceAt unknown sample");
    check (referenceAt (ref, 0.1) == 0.0, "referenceAt unvoiced");
    check (referenceAt (ref, 1.2) < 0.0, "referenceAt beyond the track is unknown");

    // Log-frequency interpolation between two voiced samples an octave apart
    ReferenceF0 glide;
    glide.times = { 0.0, 0.01 };
    glide.f0 = { 100.0, 200.0 };
    finaliseReference (glide);
    check (near (referenceAt (glide, 0.005), 100.0 * std::sqrt (2.0), 1.0e-6), "referenceAt log interpolation");

    // Frame metrics: 10 frames in the voiced part, away from the collar
    std::vector<FramePitch> frames;
    const double values[] = { 220.0, 221.0, 226.0, 440.0, 110.0, 0.0, 233.0, 220.0, 220.0, 219.0 };

    for (int i = 0; i < 10; ++i)
        frames.push_back ({ 0.40 + 0.02 * static_cast<double> (i) - (i >= 5 ? -0.01 : 0.0), values[i], i != 5 });

    frames.push_back ({ 0.10, 0.0, true });   // unvoiced reference, pipeline voiced: false alarm
    frames.push_back ({ 0.15, 0.0, false });  // unvoiced, correct
    frames.push_back ({ 0.30, 220.0, true }); // inside the collar (transition 0.295)
    frames.push_back ({ 0.50, 220.0, true }); // unknown reference

    const auto m = computeF0Metrics (frames, ref, 0.02);
    check (m.referenceVoicedFrames == 10 && m.referenceUnvoicedFrames == 2 && m.excludedFrames == 2, "f0 frame counts");
    // within 50 cents of 220: 220, 221 (7.9 c), 226 (46.6 c), 220, 220, 219 (-7.9 c) -> 6; 233 Hz = 99 c off
    check (m.pitchCorrectFrames == 6 && m.chromaCorrectFrames == 8, "f0 pitch / chroma counts");
    check (near (m.rawPitchAccuracy, 0.6, 1.0e-9) && near (m.octaveErrorRate, 0.2, 1.0e-9), "f0 accuracy / octave rate");
    check (near (m.voicingRecall, 0.9, 1.0e-9) && near (m.voicingFalseAlarm, 0.5, 1.0e-9), "f0 voicing recall / false alarm");
    check (near (m.medianAbsCents, 0.5, 1.0e-9), "f0 median cents");

    // Pooling recomputes the rates from the counts
    auto pooled = m;
    accumulate (pooled, m);
    finalise (pooled);
    check (pooled.referenceVoicedFrames == 20 && near (pooled.rawPitchAccuracy, 0.6, 1.0e-9), "f0 pooling");

    // Percentiles (nearest rank)
    check (near (percentile ({ 5.0, 1.0, 3.0, 2.0, 4.0 }, 50.0), 3.0, 0.0) && near (percentile ({ 1.0, 2.0 }, 95.0), 2.0, 0.0)
               && percentile ({}, 50.0) == 0.0,
           "percentile");

    // Note latency statistics
    const std::vector<NoteLabel> labels { { 1.0, 2.0, 60 }, { 3.0, 4.0, 62 }, { 4.0, 5.0, 64 }, { 6.0, 7.0, 65 } };
    const std::vector<DetectedNote> detected { { 1.020, 2.0, 60, 100 }, { 3.030, 4.05, 61, 100 }, { 4.050, 5.0, 64, 100 },
                                               { 6.150, 7.0, 65, 100 } };
    const auto notes = computeNoteMetrics (detected, labels);
    check (notes.matched == 2 && near (notes.medianOnsetLatencySeconds, 0.020, 1.0e-9) && near (notes.p95OnsetLatencySeconds, 0.050, 1.0e-9),
           "note matching / latency percentiles");
    // From rest: notes 1, 2 and 4 (note 3 is a legato change); first note-on 20, 30 (wrong pitch) and 150 ms late
    check (notes.restLatencyCount == 3 && notes.restLatencyMissing == 0 && near (notes.medianRestLatencySeconds, 0.030, 1.0e-9)
               && near (notes.p95RestLatencySeconds, 0.150, 1.0e-9),
           "rest latency");

    // Categories: a reference note after a rest matched by a detected note change counts for the
    // reference's recall and the detection's precision (precision can never exceed 1)
    const auto cross = computeNoteMetrics ({ { 0.5, 1.01, 59, 100 }, { 1.01, 2.0, 60, 100 } }, { { 1.0, 2.0, 60 } });
    check (cross.fromRest.matched == 1 && cross.fromRest.matchedDetections == 0 && cross.fromRest.precision == 0.0
               && cross.fromRest.recall == 1.0 && cross.change.precision == 1.0,
           "note categories");
    check (notes.fromRest.detectedNotes == 3 && notes.fromRest.matchedDetections == 1 && notes.change.matchedDetections == 1,
           "note category counts");

    check (datasetForFile ("/x/tenor_dcs-solo_FullChoir-Solo03.wav") == "dcs-solo" && datasetForFile ("tenor_melody.wav") == "other"
               && datasetForFile ("a__b.wav") == "other",
           "datasetForFile");

    std::error_code ec;
    std::filesystem::remove_all (dir, ec);
    std::printf ("{\"tool\":\"vs-eval\",\"unitTest\":true,\"failures\":%d}\n", failures);
    return failures == 0 ? 0 : 1;
}

// --- Command line ---------------------------------------------------------------------------------------

int usage (const char* message)
{
    std::fprintf (stderr, "vs-eval: %s\nUsage: vs-eval [--range r] [--algorithm yin|mpm|both] [--labels f.csv] [--f0-ref f.f0.csv]\n"
                          "               [--block n] [--key k] [--scale s] [--stickiness %%[,..]] [--gate dB[,..]]\n"
                          "               [--min-note ms[,..]] [--jobs n] [--json out.json] [--markdown out.md|-]\n"
                          "               [--summary-only] [--notes] [--frames prefix] file.wav|dir...\n"
                          "       vs-eval --self-test [--corpus-dir dir] [--keep]\n"
                          "       vs-eval --write-corpus dir\n"
                          "       vs-eval --unit-test\n",
                  message);
    return 2;
}

bool parseArgs (int argc, char** argv, Options& o, std::string& error)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        const auto value = [&] () -> const char*
        {
            if (i + 1 >= argc)
            {
                error = "missing value for " + a;
                return nullptr;
            }

            return argv[++i];
        };
        const auto parseNumber = [&] (const std::string& text, double& out) -> bool
        {
            char* end = nullptr;
            out = std::strtod (text.c_str(), &end);

            if (text.empty() || end == text.c_str() || *end != '\0' || ! std::isfinite (out))
            {
                error = "not a number for " + a + ": " + text;
                return false;
            }

            return true;
        };
        const auto number = [&] (double& out) -> bool
        {
            const auto* v = value();
            return v != nullptr && parseNumber (v, out);
        };
        /** "a" or "a,b,c": the first value goes to single, all values to list (if more than one). */
        const auto numberList = [&] (float& single, std::vector<float>& list) -> bool
        {
            const auto* v = value();

            if (v == nullptr)
                return false;

            std::vector<float> values;
            std::stringstream in (v);
            std::string item;

            while (std::getline (in, item, ','))
            {
                double x = 0.0;

                if (! parseNumber (item, x))
                    return false;

                values.push_back (static_cast<float> (x));
            }

            if (values.empty())
            {
                error = "empty list for " + a;
                return false;
            }

            single = values.front();
            list = values.size() > 1 ? values : std::vector<float> {};
            return true;
        };

        double x = 0.0;

        if (a == "--self-test")
            o.selfTest = true;
        else if (a == "--unit-test")
            o.unitTest = true;
        else if (a == "--keep")
            o.keepCorpus = true;
        else if (a == "--notes")
            o.listNotes = true;
        else if (a == "--summary-only")
            o.summaryOnly = true;
        else if (a == "--corpus-dir" || a == "--write-corpus" || a == "--range" || a == "--algorithm" || a == "--labels"
                 || a == "--f0-ref" || a == "--json" || a == "--markdown" || a == "--frames")
        {
            const auto* v = value();

            if (v == nullptr)
                return false;

            auto& target = a == "--corpus-dir"     ? o.corpusDir
                           : a == "--write-corpus" ? o.writeCorpusDir
                           : a == "--range"        ? o.range
                           : a == "--algorithm"    ? o.algorithm
                           : a == "--labels"       ? o.labels
                           : a == "--f0-ref"       ? o.f0Ref
                           : a == "--json"         ? o.jsonPath
                           : a == "--frames"       ? o.framesPrefix
                                                   : o.markdownPath;
            target = v;
        }
        else if (a == "--block" && number (x))
            o.block = static_cast<int> (x);
        else if (a == "--jobs" && number (x))
            o.jobs = static_cast<int> (x);
        else if (a == "--key" && number (x))
            o.settings.key = static_cast<int> (x);
        else if (a == "--scale" && number (x))
            o.settings.scale = static_cast<vocalsynth::Scale> (static_cast<int> (x));
        else if (a == "--stickiness" && numberList (o.settings.stickinessPercent, o.stickiness))
            continue;
        else if (a == "--gate" && numberList (o.settings.gateThresholdDb, o.gates))
            continue;
        else if (a == "--min-note" && numberList (o.settings.minNoteLengthMs, o.minNotes))
            continue;
        else if (! error.empty())
            return false;
        else if (a.rfind ("--", 0) == 0)
        {
            error = "unknown option " + a;
            return false;
        }
        else
            o.files.push_back (a);
    }

    if (! error.empty())
        return false;

    VoiceRange r;

    if (o.range != "auto" && ! parseRange (o.range, r))
    {
        error = "unknown range " + o.range;
        return false;
    }

    if (o.algorithm != "yin" && o.algorithm != "mpm" && o.algorithm != "both")
    {
        error = "unknown algorithm " + o.algorithm;
        return false;
    }

    if (o.block < 1 || o.block > 1 << 16)
    {
        error = "block size must be 1..65536";
        return false;
    }

    if (o.jobs < 1 || o.jobs > 64)
    {
        error = "jobs must be 1..64";
        return false;
    }

    o.settings = vocalsynth::sanitise (o.settings);
    return true;
}

/** Grid of all combinations of the swept settings; baseIndex = every swept setting at its default value
    (if the list contains it), else at the first value of its list. */
std::vector<GridPoint> buildGrid (const Options& o, std::size_t& baseIndex)
{
    const vocalsynth::VoiceSettings defaults {};
    const auto listOr = [] (const std::vector<float>& list, float single) { return list.empty() ? std::vector<float> { single } : list; };
    const auto gates = listOr (o.gates, o.settings.gateThresholdDb);
    const auto sticks = listOr (o.stickiness, o.settings.stickinessPercent);
    const auto minNotes = listOr (o.minNotes, o.settings.minNoteLengthMs);
    const auto baseOf = [] (const std::vector<float>& list, float preferred)
    {
        const auto it = std::find (list.begin(), list.end(), preferred);
        return it == list.end() ? list.front() : *it;
    };
    const auto baseGate = baseOf (gates, defaults.gateThresholdDb);
    const auto baseStick = baseOf (sticks, defaults.stickinessPercent);
    const auto baseMinNote = baseOf (minNotes, defaults.minNoteLengthMs);

    std::vector<GridPoint> grid;
    baseIndex = 0;

    for (const auto g : gates)
        for (const auto s : sticks)
            for (const auto n : minNotes)
            {
                auto settings = o.settings;
                settings.gateThresholdDb = g;
                settings.stickinessPercent = s;
                settings.minNoteLengthMs = n;

                if (g == baseGate && s == baseStick && n == baseMinNote)
                    baseIndex = grid.size();

                grid.push_back ({ vocalsynth::sanitise (settings), {}, {}, {} });
            }

    return grid;
}

/** Expands directories into their *.wav files (sorted) and finds the labels / reference f0 next to each. */
bool collectInputs (const Options& options, std::vector<InputFile>& files, std::string& error)
{
    std::vector<std::string> paths;

    for (const auto& f : options.files)
    {
        std::error_code ec;

        if (std::filesystem::is_directory (f, ec))
        {
            std::vector<std::string> inDir;

            for (const auto& entry : std::filesystem::directory_iterator (f, ec))
                if (entry.is_regular_file() && entry.path().extension() == ".wav")
                    inDir.push_back (entry.path().string());

            std::sort (inDir.begin(), inDir.end());
            paths.insert (paths.end(), inDir.begin(), inDir.end());
        }
        else
        {
            paths.push_back (f);
        }
    }

    if (paths.empty())
    {
        error = "no input files";
        return false;
    }

    if ((! options.labels.empty() || ! options.f0Ref.empty()) && paths.size() != 1)
    {
        error = "--labels / --f0-ref need exactly one input file";
        return false;
    }

    for (const auto& f : paths)
    {
        auto labels = options.labels;
        auto f0 = options.f0Ref;
        const std::filesystem::path p (f);

        if (labels.empty())
        {
            const auto candidate = std::filesystem::path (p).replace_extension (".csv");

            if (std::filesystem::exists (candidate))
                labels = candidate.string();
        }

        if (f0.empty())
        {
            const auto candidate = std::filesystem::path (p).replace_extension (".f0.csv");

            if (std::filesystem::exists (candidate))
                f0 = candidate.string();
        }

        files.push_back ({ f, labels, f0 });
    }

    return true;
}

bool writeText (const std::string& path, const std::string& text)
{
    if (path == "-")
    {
        std::fputs (text.c_str(), stdout);
        return true;
    }

    std::ofstream out (path, std::ios::binary);
    out << text;
    return static_cast<bool> (out);
}

} // namespace

int main (int argc, char** argv)
{
    Options options;
    std::string error;

    if (! parseArgs (argc, argv, options, error))
        return usage (error.c_str());

    if (options.unitTest)
        return runUnitTest();

    if (! options.writeCorpusDir.empty())
    {
        std::vector<InputFile> written;

        if (! writeCorpus (options.writeCorpusDir, written, error))
            return usage (error.c_str());

        std::printf ("{\"tool\":\"vs-eval\",\"corpus\":%s,\"files\":%zu}\n", jsonString (options.writeCorpusDir).c_str(),
                     written.size());
        return 0;
    }

    std::vector<PitchAlgorithm> algorithms;

    if (options.algorithm != "mpm")
        algorithms.push_back (PitchAlgorithm::yin);

    if (options.algorithm != "yin")
        algorithms.push_back (PitchAlgorithm::mpm);

    std::vector<InputFile> files;
    std::string corpusDir;

    if (options.selfTest)
    {
        corpusDir = options.corpusDir.empty() ? uniqueTempDir() : options.corpusDir;

        if (! writeCorpus (corpusDir, files, error))
            return usage (error.c_str());
    }
    else if (! collectInputs (options, files, error))
    {
        return usage (error.c_str());
    }

    std::size_t baseIndex = 0;
    auto grid = buildGrid (options, baseIndex);

    if (! options.framesPrefix.empty() && (files.size() != 1 || grid.size() != 1))
        return usage ("--frames needs a single input file and no sweep");

    std::string json = "{\"tool\":\"vs-eval\",\"selfTest\":" + std::string (options.selfTest ? "true" : "false")
                       + ",\"blockSize\":" + std::to_string (options.block) + ",\"settings\":"
                       + settingsJson (grid[baseIndex].settings) + ",";

    if (options.selfTest)
        json += "\"corpusDir\":" + jsonString (corpusDir) + ",\"gates\":{\"minRawPitchAccuracy\":" + number (minRawPitchAccuracy, 2)
                + ",\"maxOctaveErrorRate\":" + number (maxOctaveErrorRate, 2) + ",\"minNoteF1FromRest\":" + number (minNoteF1, 2)
                + ",\"notesOnNoise\":0},";

    std::string filesJson;
    const auto ok = evaluate (options, files, algorithms, grid, baseIndex, filesJson, error);

    if (options.selfTest && options.corpusDir.empty() && ! options.keepCorpus)
    {
        std::error_code ec;
        std::filesystem::remove_all (corpusDir, ec);
    }

    if (! ok)
        return usage (error.c_str());

    const auto& base = grid[baseIndex];
    bool allPassed = true;
    json += filesJson + "," + summaryJson (algorithms, base.totals, options.selfTest, allPassed) + ","
            + groupsJson ("byRange", base.byRange, algorithms) + "," + groupsJson ("byDataset", base.byDataset, algorithms);

    if (grid.size() > 1)
    {
        json += ",\"sweep\":[";

        for (std::size_t p = 0; p < grid.size(); ++p)
        {
            bool unused = true;
            json += (p > 0 ? "," : "") + std::string ("{\"settings\":") + settingsJson (grid[p].settings)
                    + ",\"base\":" + (p == baseIndex ? "true" : "false") + ","
                    + summaryJson (algorithms, grid[p].totals, false, unused) + "}";
        }

        json += "]";
    }

    json += "}\n";

    if (options.jsonPath.empty())
        std::fputs (json.c_str(), stdout);
    else if (! writeText (options.jsonPath, json))
        return usage (("cannot write " + options.jsonPath).c_str());

    if (! options.markdownPath.empty()
        && ! writeText (options.markdownPath, markdownReport (algorithms, grid, baseIndex, options.block)))
        return usage (("cannot write " + options.markdownPath).c_str());

    return options.selfTest && ! allPassed ? 1 : 0;
}
