#include "Metrics.h"

namespace vseval
{

// TODO(C): implement (docs/M2-DESIGN.md 5).

bool readLabelsCsv (const std::string& path, std::vector<NoteLabel>& out, std::string& error)
{
    out.clear();
    error = "CSV reader not implemented yet: " + path;
    return false;
}

PitchMetrics computePitchMetrics (const std::vector<FramePitch>& /*frames*/, const std::vector<NoteLabel>& /*labels*/)
{
    return {};
}

NoteMetrics computeNoteMetrics (const std::vector<DetectedNote>& /*detected*/, const std::vector<NoteLabel>& /*labels*/,
                                double /*onsetToleranceSeconds*/)
{
    return {};
}

} // namespace vseval
