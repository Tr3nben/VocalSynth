// vs-eval: runs the VocalSynth voice pipeline offline on WAV files and prints JSON metrics
// (docs/M2-DESIGN.md 5). JUCE-free.
//
// Usage:
//   vs-eval [options] <file.wav> [<file.wav> ...]
//   vs-eval --self-test [--corpus-dir <dir>] [--keep]
//                                     writes the synthetic corpus (WAV + CSV) to a temporary directory (or
//                                     <dir>), evaluates it from disk with both algorithms, prints the JSON and
//                                     exits with 1 if a gate fails (ctest vs-eval.self-test)
//   vs-eval --write-corpus <dir>      only writes the synthetic corpus as WAV + CSV labels
// Options:
//   --range <auto|bass|baritone|tenor|alto|soprano|whistle|wide>
//                            default auto: the file name prefix ("bass_..." -> bass), else tenor
//   --algorithm <yin|mpm|both>                                default both
//   --labels <file.csv>      labels for the (single) input file; default: <file>.csv next to the WAV if present
//   --block <n>              host block size for the offline run, default 512
//   --key <0..11> --scale <0..8> --stickiness <%> --gate <dB> --min-note <ms>   pipeline settings
//   --notes                  also list the detected notes per file
//
// Metrics (per file, per algorithm, and pooled over all files):
//   pitch            detector frames (time = window centre): raw pitch accuracy (labelled frames within
//                    +-50 cents of the label note), octave error rate (12 or 24 +- 0.5 semitones off),
//                    voicing false alarm (unlabelled frames the pipeline called voiced). Frames within half
//                    an analysis window (+ 1 hop) of a label boundary are not scored ("collarMs").
//   processedPitch   the same after the median + octave guard
//   notes            note precision / recall / F1 (same MIDI note, onset within +-50 ms, one-to-one) and the
//                    onset latency of the matched notes, split into notes that start after a rest
//                    ("fromRest") and legato note changes ("change", which carry the stickiness delay T_min
//                    by design; "changeDelay" = how late they come). Files labelled "# vs-eval: pitch-only"
//                    (glissandi) are not scored here; files with an empty label list count their notes as
//                    false positives ("notesOnUnvoicedFiles").
// Self-test gates (both algorithms): raw pitch accuracy >= 0.95, octave errors < 2 %, note F1 of the notes
// from rest >= 0.85 (the overall F1 includes the legato changes and is reported, not gated), no notes on
// the files without voice.
// Exit codes: 0 ok, 1 a self-test gate failed, 2 usage or I/O error.

#include "Corpus.h"
#include "Metrics.h"
#include "WavFile.h"

#include "vocalsynth/VoicePipeline.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>
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
    int block = 512;
    vocalsynth::VoiceSettings settings {};
    bool listNotes = false;
    bool selfTest = false;
    bool keepCorpus = false;
    std::string corpusDir;
    std::string writeCorpusDir;
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
};

struct FrameRecord
{
    int64_t hopSample;
    float raw;
    float processed;
    bool voiced;
};

void recordFrame (void* context, const vocalsynth::AnalysisFrameInfo& info) noexcept
{
    static_cast<std::vector<FrameRecord>*> (context)->push_back (
        { info.hopSample, info.raw.frequencyHz, info.processed.frequencyHz, info.voiced });
}

RunResult runPipeline (const std::vector<float>& mono, double sampleRate, const vocalsynth::VoiceSettings& settings, int block)
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
        result.raw.push_back ({ t, static_cast<double> (f.raw), f.voiced });
        result.processed.push_back ({ t - medianDelay / sampleRate, static_cast<double> (f.processed), f.voiced });
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

std::string categoryJson (const NoteCategory& c)
{
    return "{\"reference\":" + std::to_string (c.referenceNotes) + ",\"detected\":" + std::to_string (c.detectedNotes)
           + ",\"matched\":" + std::to_string (c.matched) + ",\"precision\":" + number (c.precision)
           + ",\"recall\":" + number (c.recall) + ",\"f1\":" + number (c.f1) + "}";
}

std::string notesJson (const NoteMetrics& m)
{
    return "{\"reference\":" + std::to_string (m.referenceNotes) + ",\"detected\":" + std::to_string (m.detectedNotes)
           + ",\"matched\":" + std::to_string (m.matched) + ",\"precision\":" + number (m.precision)
           + ",\"recall\":" + number (m.recall) + ",\"f1\":" + number (m.f1) + ",\"meanOnsetLatencyMs\":"
           + number (1000.0 * m.meanOnsetLatencySeconds, 2) + ",\"maxOnsetLatencyMs\":"
           + number (1000.0 * m.maxOnsetLatencySeconds, 2) + ",\"fromRest\":" + categoryJson (m.fromRest)
           + ",\"change\":" + categoryJson (m.change) + ",\"changeDelay\":{\"count\":" + std::to_string (m.changeDelayCount)
           + ",\"meanMs\":" + number (1000.0 * m.meanChangeDelaySeconds, 2) + ",\"maxMs\":"
           + number (1000.0 * m.maxChangeDelaySeconds, 2) + "}}";
}

// --- Evaluation -----------------------------------------------------------------------------------------

struct Totals
{
    int files = 0;
    PitchMetrics pitch, processed;
    NoteMetrics notes;
    int unvoicedFiles = 0;
    int notesOnUnvoicedFiles = 0;
    int unlabelledDetectedNotes = 0; ///< files without a labels file: only counted
};

struct InputFile
{
    std::string path;
    std::string labelsPath; ///< empty = no labels
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

/** Evaluates the files; appends the per-file JSON to `json`. Returns false on an I/O error. */
bool evaluateFiles (const Options& options, const std::vector<InputFile>& files,
                    const std::vector<PitchAlgorithm>& algorithms, std::vector<Totals>& totals, std::string& json,
                    std::string& error)
{
    totals.assign (algorithms.size(), {});
    json += "\"files\":[";

    for (std::size_t fi = 0; fi < files.size(); ++fi)
    {
        const auto& file = files[fi];
        WavData wav;

        if (! readWav (file.path, wav, error))
            return false;

        LabelFile labels;
        const auto hasLabels = ! file.labelsPath.empty();

        if (hasLabels && ! readLabelsCsv (file.labelsPath, labels, error))
            return false;

        auto settings = options.settings;
        settings.range = rangeForFile (options, file.path);

        json += fi > 0 ? "," : "";
        json += "{\"file\":" + jsonString (file.path) + ",\"labels\":" + (hasLabels ? jsonString (file.labelsPath) : "null")
                + ",\"range\":" + jsonString (vocalsynth::voiceRangeInfo (settings.range).name)
                + ",\"sampleRate\":" + number (wav.sampleRate, 0) + ",\"channels\":" + std::to_string (wav.numChannels)
                + ",\"seconds\":" + number (static_cast<double> (wav.mono.size()) / wav.sampleRate, 3)
                + ",\"pitchOnly\":" + (labels.pitchOnly ? "true" : "false");

        for (std::size_t a = 0; a < algorithms.size(); ++a)
        {
            settings.algorithm = algorithms[a];
            const auto run = runPipeline (wav.mono, wav.sampleRate, settings, options.block);
            auto& total = totals[a];
            ++total.files;

            json += std::string (",\"") + algorithmName (algorithms[a]) + "\":{\"detectedNotes\":"
                    + std::to_string (run.notes.size()) + ",\"latencyModelMs\":" + number (run.latencyModelMs, 2);

            if (hasLabels)
            {
                const auto pitch = computePitchMetrics (run.raw, labels.notes, run.collarSeconds);
                const auto processed = computePitchMetrics (run.processed, labels.notes, run.collarSeconds);
                accumulate (total.pitch, pitch);
                accumulate (total.processed, processed);
                json += ",\"collarMs\":" + number (1000.0 * run.collarSeconds, 2) + ",\"pitch\":" + pitchJson (pitch)
                        + ",\"processedPitch\":" + pitchJson (processed);

                if (labels.notes.empty())
                {
                    ++total.unvoicedFiles;
                    total.notesOnUnvoicedFiles += static_cast<int> (run.notes.size());
                }

                if (! labels.pitchOnly)
                {
                    const auto notes = computeNoteMetrics (run.notes, labels.notes);
                    accumulate (total.notes, notes);
                    json += ",\"notes\":" + notesJson (notes);
                }
            }
            else
            {
                total.unlabelledDetectedNotes += static_cast<int> (run.notes.size());
            }

            if (options.listNotes)
            {
                json += ",\"noteList\":[";

                for (std::size_t n = 0; n < run.notes.size(); ++n)
                    json += (n > 0 ? "," : "") + std::string ("[") + number (run.notes[n].onsetSeconds) + ","
                            + number (run.notes[n].offsetSeconds) + "," + std::to_string (run.notes[n].midiNote) + ","
                            + std::to_string (run.notes[n].velocity) + "]";

                json += "]";
            }

            json += "}";
        }

        json += "}";
    }

    json += "]";

    for (auto& t : totals)
    {
        finalise (t.pitch);
        finalise (t.processed);
        finalise (t.notes);
    }

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

std::string summaryJson (const std::vector<PitchAlgorithm>& algorithms, const std::vector<Totals>& totals, bool withGates,
                         bool& allPassed)
{
    std::string json = "\"summary\":{";
    allPassed = true;
    std::vector<GateResult> gates;

    for (std::size_t a = 0; a < algorithms.size(); ++a)
    {
        const auto& t = totals[a];
        json += (a > 0 ? "," : "") + std::string ("\"") + algorithmName (algorithms[a]) + "\":{\"files\":"
                + std::to_string (t.files) + ",\"pitch\":" + pitchJson (t.pitch) + ",\"processedPitch\":"
                + pitchJson (t.processed) + ",\"notes\":" + notesJson (t.notes) + ",\"unvoicedFiles\":"
                + std::to_string (t.unvoicedFiles) + ",\"notesOnUnvoicedFiles\":" + std::to_string (t.notesOnUnvoicedFiles)
                + ",\"detectedNotesWithoutLabels\":" + std::to_string (t.unlabelledDetectedNotes);

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
                + better (y.notes.meanOnsetLatencySeconds, m.notes.meanOnsetLatencySeconds, false)
                + "\",\"recommendedDefault\":\"" + (km > ky ? "mpm" : "yin") + "\"}";
    }

    return json;
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

        written.push_back ({ base + ".wav", base + ".csv" });
    }

    return true;
}

std::string uniqueTempDir()
{
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    return (std::filesystem::temp_directory_path() / ("vs-eval-selftest-" + std::to_string (ticks))).string();
}

int usage (const char* message)
{
    std::fprintf (stderr, "vs-eval: %s\nUsage: vs-eval [--range r] [--algorithm yin|mpm|both] [--labels f.csv] [--block n]\n"
                          "               [--key k] [--scale s] [--stickiness %%] [--gate dB] [--min-note ms] [--notes] file.wav...\n"
                          "       vs-eval --self-test [--corpus-dir dir] [--keep]\n"
                          "       vs-eval --write-corpus dir\n",
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
        const auto number = [&] (double& out) -> bool
        {
            const auto* v = value();

            if (v == nullptr)
                return false;

            char* end = nullptr;
            out = std::strtod (v, &end);

            if (end == v || *end != '\0')
            {
                error = "not a number for " + a + ": " + v;
                return false;
            }

            return true;
        };

        double x = 0.0;

        if (a == "--self-test")
            o.selfTest = true;
        else if (a == "--keep")
            o.keepCorpus = true;
        else if (a == "--notes")
            o.listNotes = true;
        else if (a == "--corpus-dir" || a == "--write-corpus" || a == "--range" || a == "--algorithm" || a == "--labels")
        {
            const auto* v = value();

            if (v == nullptr)
                return false;

            (a == "--corpus-dir" ? o.corpusDir : a == "--write-corpus" ? o.writeCorpusDir : a == "--range" ? o.range
                                                                    : a == "--algorithm"                ? o.algorithm
                                                                                                        : o.labels) = v;
        }
        else if (a == "--block" && number (x))
            o.block = static_cast<int> (x);
        else if (a == "--key" && number (x))
            o.settings.key = static_cast<int> (x);
        else if (a == "--scale" && number (x))
            o.settings.scale = static_cast<vocalsynth::Scale> (static_cast<int> (x));
        else if (a == "--stickiness" && number (x))
            o.settings.stickinessPercent = static_cast<float> (x);
        else if (a == "--gate" && number (x))
            o.settings.gateThresholdDb = static_cast<float> (x);
        else if (a == "--min-note" && number (x))
            o.settings.minNoteLengthMs = static_cast<float> (x);
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

    o.settings = vocalsynth::sanitise (o.settings);
    return true;
}

} // namespace

int main (int argc, char** argv)
{
    Options options;
    std::string error;

    if (! parseArgs (argc, argv, options, error))
        return usage (error.c_str());

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
    else
    {
        if (options.files.empty())
            return usage ("no input files");

        if (! options.labels.empty() && options.files.size() != 1)
            return usage ("--labels needs exactly one input file");

        for (const auto& f : options.files)
        {
            auto labels = options.labels;

            if (labels.empty())
            {
                const auto candidate = std::filesystem::path (f).replace_extension (".csv");

                if (std::filesystem::exists (candidate))
                    labels = candidate.string();
            }

            files.push_back ({ f, labels });
        }
    }

    std::string json = "{\"tool\":\"vs-eval\",\"selfTest\":" + std::string (options.selfTest ? "true" : "false")
                       + ",\"blockSize\":" + std::to_string (options.block) + ",";

    if (options.selfTest)
        json += "\"corpusDir\":" + jsonString (corpusDir) + ",\"gates\":{\"minRawPitchAccuracy\":" + number (minRawPitchAccuracy, 2)
                + ",\"maxOctaveErrorRate\":" + number (maxOctaveErrorRate, 2) + ",\"minNoteF1FromRest\":" + number (minNoteF1, 2)
                + ",\"notesOnNoise\":0},";

    std::vector<Totals> totals;
    const auto ok = evaluateFiles (options, files, algorithms, totals, json, error);

    if (options.selfTest && options.corpusDir.empty() && ! options.keepCorpus)
    {
        std::error_code ec;
        std::filesystem::remove_all (corpusDir, ec);
    }

    if (! ok)
        return usage (error.c_str());

    bool allPassed = true;
    json += "," + summaryJson (algorithms, totals, options.selfTest, allPassed) + "}";
    std::printf ("%s\n", json.c_str());
    return options.selfTest && ! allPassed ? 1 : 0;
}
