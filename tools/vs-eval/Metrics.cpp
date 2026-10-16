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
        m.onsetLatencies.push_back (latency);
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

    // Onset latency after a rest, independent of the matching: first note-on (any pitch) near the onset
    for (std::size_t r = 0; r < labels.size(); ++r)
    {
        if (isReferenceChange (r))
            continue;

        auto first = 1.0e9;

        for (const auto& d : detected)
        {
            const auto delay = d.onsetSeconds - labels[r].onsetSeconds;

            if (delay >= -restLatencyEarlySeconds && delay <= restLatencyLateSeconds)
                first = std::min (first, delay);
        }

        if (first < 1.0e9)
        {
            ++m.restLatencyCount;
            m.restLatencies.push_back (first);
        }
        else
        {
            ++m.restLatencyMissing;
        }
    }

    for (std::size_t d = 0; d < detected.size(); ++d)
    {
        auto& category = isDetectedChange (d) ? m.change : m.fromRest;
        ++category.detectedNotes;

        if (detectionUsed[d])
            ++category.matchedDetections;
    }

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
    total.matchedDetections += add.matchedDetections;
}

void finalise (NoteCategory& c)
{
    c.precision = c.detectedNotes > 0 ? static_cast<double> (c.matchedDetections) / static_cast<double> (c.detectedNotes) : 1.0;
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
    total.onsetLatencies.insert (total.onsetLatencies.end(), add.onsetLatencies.begin(), add.onsetLatencies.end());
    total.restLatencyCount += add.restLatencyCount;
    total.restLatencyMissing += add.restLatencyMissing;
    total.restLatencies.insert (total.restLatencies.end(), add.restLatencies.begin(), add.restLatencies.end());
}

void finalise (NoteMetrics& m)
{
    // Conventions for empty sets: nothing to find and nothing found = perfect
    m.precision = m.detectedNotes > 0 ? static_cast<double> (m.matched) / static_cast<double> (m.detectedNotes) : 1.0;
    m.recall = m.referenceNotes > 0 ? static_cast<double> (m.matched) / static_cast<double> (m.referenceNotes) : 1.0;
    m.f1 = m.precision + m.recall > 0.0 ? 2.0 * m.precision * m.recall / (m.precision + m.recall) : 0.0;
    m.meanOnsetLatencySeconds = m.matched > 0 ? m.sumOnsetLatencySeconds / static_cast<double> (m.matched) : 0.0;
    m.meanChangeDelaySeconds = m.changeDelayCount > 0 ? m.sumChangeDelaySeconds / static_cast<double> (m.changeDelayCount) : 0.0;
    m.medianOnsetLatencySeconds = percentile (m.onsetLatencies, 50.0);
    m.p95OnsetLatencySeconds = percentile (m.onsetLatencies, 95.0);

    auto restSum = 0.0;

    for (const auto v : m.restLatencies)
        restSum += v;

    m.meanRestLatencySeconds = m.restLatencies.empty() ? 0.0 : restSum / static_cast<double> (m.restLatencies.size());
    m.medianRestLatencySeconds = percentile (m.restLatencies, 50.0);
    m.p95RestLatencySeconds = percentile (m.restLatencies, 95.0);
    finalise (m.fromRest);
    finalise (m.change);
}

double percentile (std::vector<double> values, double percent)
{
    if (values.empty())
        return 0.0;

    std::sort (values.begin(), values.end());
    // Nearest rank: the smallest value with at least percent % of the values <= it
    const auto rank = static_cast<std::size_t> (std::ceil (std::clamp (percent, 0.0, 100.0) / 100.0
                                                           * static_cast<double> (values.size())));
    return values[std::clamp<std::size_t> (rank, 1, values.size()) - 1];
}

// --- Reference f0 ---------------------------------------------------------------------------------------

namespace
{
/** Voicing state of a reference value: 1 voiced, 0 unvoiced, -1 unknown. */
int voicingState (double f0) { return f0 > 0.0 ? 1 : (f0 < 0.0 ? -1 : 0); }

double centsBetween (double estimateHz, double referenceHz) { return 1200.0 * std::log2 (estimateHz / referenceHz); }
} // namespace

void finaliseReference (ReferenceF0& ref)
{
    std::vector<double> gaps;

    for (std::size_t i = 1; i < ref.times.size(); ++i)
        gaps.push_back (ref.times[i] - ref.times[i - 1]);

    ref.hopSeconds = gaps.empty() ? 0.01 : percentile (gaps, 50.0);
    ref.transitions.clear();

    // Unknown samples do not count as a state: a transition is a change between the last known state and
    // the next known one (at the midpoint of the two samples)
    int lastState = -1;
    double lastTime = 0.0;

    for (std::size_t i = 0; i < ref.times.size(); ++i)
    {
        const auto state = voicingState (ref.f0[i]);

        if (state < 0)
            continue;

        if (lastState >= 0 && state != lastState)
            ref.transitions.push_back (0.5 * (lastTime + ref.times[i]));

        lastState = state;
        lastTime = ref.times[i];
    }
}

bool readReferenceF0Csv (const std::string& path, ReferenceF0& out, std::string& error)
{
    out = {};
    std::ifstream file (path);

    if (! file)
    {
        error = "cannot open f0 reference: " + path;
        return false;
    }

    std::string line;
    int lineNumber = 0;
    bool sawData = false;

    while (std::getline (file, line))
    {
        ++lineNumber;
        line = trim (line);

        if (line.empty() || line[0] == '#')
            continue;

        std::replace (line.begin(), line.end(), ',', ' ');
        std::replace (line.begin(), line.end(), ';', ' ');
        std::istringstream in (line);
        double t = 0.0, f = 0.0;

        if (! (in >> t >> f) || ! std::isfinite (t) || ! std::isfinite (f))
        {
            if (! sawData) // header line
                continue;

            error = path + ":" + std::to_string (lineNumber) + ": expected time_s,f0_hz";
            return false;
        }

        if (! out.times.empty() && ! (t > out.times.back()))
        {
            error = path + ":" + std::to_string (lineNumber) + ": times must be strictly ascending";
            return false;
        }

        sawData = true;
        out.times.push_back (t);
        out.f0.push_back (f);
    }

    if (out.times.empty())
    {
        error = path + ": no f0 samples";
        return false;
    }

    finaliseReference (out);
    return true;
}

double referenceAt (const ReferenceF0& ref, double timeSeconds)
{
    if (ref.times.empty())
        return -1.0;

    const auto it = std::lower_bound (ref.times.begin(), ref.times.end(), timeSeconds);
    const auto upper = static_cast<std::size_t> (it - ref.times.begin()); // first sample >= t

    if (upper < ref.times.size() && upper > 0)
    {
        const auto a = upper - 1, b = upper;

        if (ref.f0[a] > 0.0 && ref.f0[b] > 0.0 && ref.times[b] - ref.times[a] <= 1.5 * ref.hopSeconds)
        {
            const auto w = (timeSeconds - ref.times[a]) / (ref.times[b] - ref.times[a]);
            return std::exp2 ((1.0 - w) * std::log2 (ref.f0[a]) + w * std::log2 (ref.f0[b]));
        }
    }

    // Nearest sample
    std::size_t nearest = upper;

    if (upper == ref.times.size()
        || (upper > 0 && timeSeconds - ref.times[upper - 1] <= ref.times[upper] - timeSeconds))
        nearest = upper - 1;

    if (std::abs (ref.times[nearest] - timeSeconds) > 0.75 * ref.hopSeconds)
        return -1.0;

    return ref.f0[nearest] < 0.0 ? -1.0 : ref.f0[nearest];
}

F0Metrics computeF0Metrics (const std::vector<FramePitch>& frames, const ReferenceF0& ref, double collarSeconds)
{
    F0Metrics m;

    for (const auto& frame : frames)
    {
        const auto reference = referenceAt (ref, frame.timeSeconds);

        // Collar: nearest voicing transition of the reference
        bool nearTransition = false;
        const auto it = std::lower_bound (ref.transitions.begin(), ref.transitions.end(), frame.timeSeconds);

        if (it != ref.transitions.end() && *it - frame.timeSeconds < collarSeconds)
            nearTransition = true;

        if (it != ref.transitions.begin() && frame.timeSeconds - *(it - 1) < collarSeconds)
            nearTransition = true;

        if (reference < 0.0 || nearTransition)
        {
            ++m.excludedFrames;
            continue;
        }

        if (reference == 0.0)
        {
            ++m.referenceUnvoicedFrames;

            if (frame.voiced)
                ++m.falseAlarmFrames;

            continue;
        }

        ++m.referenceVoicedFrames;

        if (frame.voiced)
            ++m.voicedRecallFrames;

        if (! (frame.frequencyHz > 0.0))
            continue;

        const auto cents = centsBetween (frame.frequencyHz, reference);
        const auto folded = cents - 1200.0 * std::round (cents / 1200.0);

        if (std::abs (cents) <= 50.0)
        {
            ++m.pitchCorrectFrames;
            ++m.absCentsHistogram[static_cast<std::size_t> (std::min (50.0, std::floor (std::abs (cents))))];
        }

        if (std::abs (folded) <= 50.0)
            ++m.chromaCorrectFrames;
    }

    finalise (m);
    return m;
}

void accumulate (F0Metrics& total, const F0Metrics& add)
{
    total.referenceVoicedFrames += add.referenceVoicedFrames;
    total.referenceUnvoicedFrames += add.referenceUnvoicedFrames;
    total.excludedFrames += add.excludedFrames;
    total.pitchCorrectFrames += add.pitchCorrectFrames;
    total.chromaCorrectFrames += add.chromaCorrectFrames;
    total.voicedRecallFrames += add.voicedRecallFrames;
    total.falseAlarmFrames += add.falseAlarmFrames;

    for (std::size_t i = 0; i < total.absCentsHistogram.size(); ++i)
        total.absCentsHistogram[i] += add.absCentsHistogram[i];
}

void finalise (F0Metrics& m)
{
    const auto voiced = static_cast<double> (std::max (1, m.referenceVoicedFrames));
    const auto unvoiced = static_cast<double> (std::max (1, m.referenceUnvoicedFrames));
    m.rawPitchAccuracy = static_cast<double> (m.pitchCorrectFrames) / voiced;
    m.rawChromaAccuracy = static_cast<double> (m.chromaCorrectFrames) / voiced;
    m.octaveErrorRate = static_cast<double> (m.chromaCorrectFrames - m.pitchCorrectFrames) / voiced;
    m.voicingRecall = static_cast<double> (m.voicedRecallFrames) / voiced;
    m.voicingFalseAlarm = static_cast<double> (m.falseAlarmFrames) / unvoiced;

    // Median of the 1-cent histogram (bin centre)
    m.medianAbsCents = 0.0;
    const auto half = (m.pitchCorrectFrames + 1) / 2;
    int seen = 0;

    for (std::size_t i = 0; i < m.absCentsHistogram.size() && m.pitchCorrectFrames > 0; ++i)
    {
        seen += m.absCentsHistogram[i];

        if (seen >= half)
        {
            m.medianAbsCents = static_cast<double> (i) + 0.5;
            break;
        }
    }
}

} // namespace vseval
