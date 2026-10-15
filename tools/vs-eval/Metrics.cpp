#include "Metrics.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace vseval
{

namespace
{

std::string trim (const std::string& s)
{
    const auto first = s.find_first_not_of (" \t\r\n");

    if (first == std::string::npos)
        return {};

    const auto last = s.find_last_not_of (" \t\r\n");
    return s.substr (first, last - first + 1);
}

/** Parses "a,b,c" (',' ';' or whitespace separated) into three numbers; false if it is not such a line. */
bool parseLabelLine (std::string line, NoteLabel& label)
{
    std::replace (line.begin(), line.end(), ',', ' ');
    std::replace (line.begin(), line.end(), ';', ' ');
    std::istringstream in (line);
    double onset = 0.0, offset = 0.0, note = 0.0;

    if (! (in >> onset >> offset >> note))
        return false;

    label = { onset, offset, static_cast<int> (std::lround (note)) };
    return true;
}

double semitonesBetween (double frequencyHz, int midiNote)
{
    return 12.0 * std::log2 (frequencyHz / 440.0) + 69.0 - static_cast<double> (midiNote);
}

} // namespace

bool readLabelsCsv (const std::string& path, LabelFile& out, std::string& error)
{
    out = {};
    std::ifstream file (path);

    if (! file)
    {
        error = "cannot open labels file: " + path;
        return false;
    }

    std::string line;
    int lineNumber = 0;
    bool sawData = false;

    while (std::getline (file, line))
    {
        ++lineNumber;
        line = trim (line);

        if (line.empty())
            continue;

        if (line[0] == '#')
        {
            if (line.find ("vs-eval: pitch-only") != std::string::npos)
                out.pitchOnly = true;

            continue;
        }

        NoteLabel label;

        if (! parseLabelLine (line, label))
        {
            if (! sawData) // header line
                continue;

            error = path + ":" + std::to_string (lineNumber) + ": expected onset_s,offset_s,midi_note";
            return false;
        }

        if (! (label.offsetSeconds > label.onsetSeconds) || label.midiNote < 0 || label.midiNote > 127)
        {
            error = path + ":" + std::to_string (lineNumber) + ": invalid label (offset <= onset or note outside 0..127)";
            return false;
        }

        sawData = true;
        out.notes.push_back (label);
    }

    std::sort (out.notes.begin(), out.notes.end(),
               [] (const NoteLabel& a, const NoteLabel& b) { return a.onsetSeconds < b.onsetSeconds; });
    return true;
}

bool writeLabelsCsv (const std::string& path, const LabelFile& labels)
{
    std::ofstream file (path);

    if (! file)
        return false;

    if (labels.pitchOnly)
        file << "# vs-eval: pitch-only (notes are not scored)\n";

    file << "onset_s,offset_s,midi_note\n";
    char buffer[96];

    for (const auto& l : labels.notes)
    {
        std::snprintf (buffer, sizeof (buffer), "%.6f,%.6f,%d\n", l.onsetSeconds, l.offsetSeconds, l.midiNote);
        file << buffer;
    }

    return static_cast<bool> (file);
}

PitchMetrics computePitchMetrics (const std::vector<FramePitch>& frames, const std::vector<NoteLabel>& labels,
                                  double collarSeconds)
{
    PitchMetrics m;

    for (const auto& frame : frames)
    {
        const NoteLabel* inside = nullptr;
        bool nearBoundary = false;

        for (const auto& label : labels)
        {
            if (std::abs (frame.timeSeconds - label.onsetSeconds) < collarSeconds
                || std::abs (frame.timeSeconds - label.offsetSeconds) < collarSeconds)
                nearBoundary = true;

            if (frame.timeSeconds >= label.onsetSeconds && frame.timeSeconds < label.offsetSeconds)
                inside = &label;
        }

        if (nearBoundary)
            continue;

        if (inside == nullptr)
        {
            ++m.unvoicedReferenceFrames;

            if (frame.voiced)
                ++m.falseAlarmFrames;

            continue;
        }

        ++m.voicedReferenceFrames;

        if (! (frame.frequencyHz > 0.0))
            continue;

        const auto error = semitonesBetween (frame.frequencyHz, inside->midiNote);

        if (std::abs (error) <= 0.5)
            ++m.correctFrames;
        else if (std::abs (std::abs (error) - 12.0) <= 0.5 || std::abs (std::abs (error) - 24.0) <= 0.5)
            ++m.octaveErrorFrames;
    }

    finalise (m);
    return m;
}

void accumulate (PitchMetrics& total, const PitchMetrics& add)
{
    total.voicedReferenceFrames += add.voicedReferenceFrames;
    total.unvoicedReferenceFrames += add.unvoicedReferenceFrames;
    total.correctFrames += add.correctFrames;
    total.octaveErrorFrames += add.octaveErrorFrames;
    total.falseAlarmFrames += add.falseAlarmFrames;
}

void finalise (PitchMetrics& m)
{
    const auto voiced = static_cast<double> (std::max (1, m.voicedReferenceFrames));
    const auto unvoiced = static_cast<double> (std::max (1, m.unvoicedReferenceFrames));
    m.rawPitchAccuracy = m.voicedReferenceFrames > 0 ? static_cast<double> (m.correctFrames) / voiced : 0.0;
    m.octaveErrorRate = static_cast<double> (m.octaveErrorFrames) / voiced;
    m.voicingFalseAlarm = static_cast<double> (m.falseAlarmFrames) / unvoiced;
}

NoteMetrics computeNoteMetrics (const std::vector<DetectedNote>& detected, const std::vector<NoteLabel>& labels,
                                double onsetToleranceSeconds)
{
    NoteMetrics m;
    m.referenceNotes = static_cast<int> (labels.size());
    m.detectedNotes = static_cast<int> (detected.size());

    // All candidate pairs within the tolerance, closest onset first (greedy one-to-one)
    struct Pair
    {
        double distance;
        std::size_t reference, detection;
    };

    std::vector<Pair> pairs;

    for (std::size_t r = 0; r < labels.size(); ++r)
        for (std::size_t d = 0; d < detected.size(); ++d)
        {
            const auto distance = std::abs (detected[d].onsetSeconds - labels[r].onsetSeconds);

            if (detected[d].midiNote == labels[r].midiNote && distance <= onsetToleranceSeconds)
                pairs.push_back ({ distance, r, d });
        }

    std::sort (pairs.begin(), pairs.end(), [] (const Pair& a, const Pair& b)
    {
        return a.distance < b.distance || (a.distance == b.distance && a.reference < b.reference);
    });

    std::vector<bool> referenceUsed (labels.size(), false), detectionUsed (detected.size(), false);

    for (const auto& p : pairs)
    {
        if (referenceUsed[p.reference] || detectionUsed[p.detection])
            continue;

        referenceUsed[p.reference] = detectionUsed[p.detection] = true;
        ++m.matched;
        const auto latency = detected[p.detection].onsetSeconds - labels[p.reference].onsetSeconds;
        m.sumOnsetLatencySeconds += latency;
        m.maxOnsetLatencySeconds = std::max (m.maxOnsetLatencySeconds, latency);
    }

    // Categories: a note is a "change" if it starts where the previous one ends (1 ms slack)
    const auto isReferenceChange = [&] (std::size_t r)
    { return r > 0 && std::abs (labels[r].onsetSeconds - labels[r - 1].offsetSeconds) < 0.001; };
    const auto isDetectedChange = [&] (std::size_t d)
    {
        for (std::size_t k = 0; k < detected.size(); ++k)
            if (k != d && std::abs (detected[k].offsetSeconds - detected[d].onsetSeconds) < 1.0e-6)
                return true;

        return false;
    };

    for (std::size_t r = 0; r < labels.size(); ++r)
    {
        auto& category = isReferenceChange (r) ? m.change : m.fromRest;
        ++category.referenceNotes;

        if (referenceUsed[r])
            ++category.matched;
    }

    for (std::size_t d = 0; d < detected.size(); ++d)
        ++(isDetectedChange (d) ? m.change : m.fromRest).detectedNotes;

    // How late are the changes? Nearest same-note detection 0..150 ms after the reference change
    for (std::size_t r = 0; r < labels.size(); ++r)
    {
        if (! isReferenceChange (r))
            continue;

        auto best = 1.0e9;

        for (const auto& d : detected)
        {
            const auto delay = d.onsetSeconds - labels[r].onsetSeconds;

            if (d.midiNote == labels[r].midiNote && delay >= 0.0 && delay <= 0.150)
                best = std::min (best, delay);
        }

        if (best < 1.0e9)
        {
            ++m.changeDelayCount;
            m.sumChangeDelaySeconds += best;
            m.maxChangeDelaySeconds = std::max (m.maxChangeDelaySeconds, best);
        }
    }

    finalise (m);
    return m;
}

namespace
{
void accumulate (NoteCategory& total, const NoteCategory& add)
{
    total.referenceNotes += add.referenceNotes;
    total.detectedNotes += add.detectedNotes;
    total.matched += add.matched;
}

void finalise (NoteCategory& c)
{
    c.precision = c.detectedNotes > 0 ? static_cast<double> (c.matched) / static_cast<double> (c.detectedNotes) : 1.0;
    c.recall = c.referenceNotes > 0 ? static_cast<double> (c.matched) / static_cast<double> (c.referenceNotes) : 1.0;
    c.f1 = c.precision + c.recall > 0.0 ? 2.0 * c.precision * c.recall / (c.precision + c.recall) : 0.0;
}
} // namespace

void accumulate (NoteMetrics& total, const NoteMetrics& add)
{
    accumulate (total.fromRest, add.fromRest);
    accumulate (total.change, add.change);
    total.changeDelayCount += add.changeDelayCount;
    total.sumChangeDelaySeconds += add.sumChangeDelaySeconds;
    total.maxChangeDelaySeconds = std::max (total.maxChangeDelaySeconds, add.maxChangeDelaySeconds);
    total.referenceNotes += add.referenceNotes;
    total.detectedNotes += add.detectedNotes;
    total.matched += add.matched;
    total.sumOnsetLatencySeconds += add.sumOnsetLatencySeconds;
    total.maxOnsetLatencySeconds = std::max (total.maxOnsetLatencySeconds, add.maxOnsetLatencySeconds);
}

void finalise (NoteMetrics& m)
{
    // Conventions for empty sets: nothing to find and nothing found = perfect
    m.precision = m.detectedNotes > 0 ? static_cast<double> (m.matched) / static_cast<double> (m.detectedNotes) : 1.0;
    m.recall = m.referenceNotes > 0 ? static_cast<double> (m.matched) / static_cast<double> (m.referenceNotes) : 1.0;
    m.f1 = m.precision + m.recall > 0.0 ? 2.0 * m.precision * m.recall / (m.precision + m.recall) : 0.0;
    m.meanOnsetLatencySeconds = m.matched > 0 ? m.sumOnsetLatencySeconds / static_cast<double> (m.matched) : 0.0;
    m.meanChangeDelaySeconds = m.changeDelayCount > 0 ? m.sumChangeDelaySeconds / static_cast<double> (m.changeDelayCount) : 0.0;
    finalise (m.fromRest);
    finalise (m.change);
}

} // namespace vseval
