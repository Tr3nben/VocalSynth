#include "CaptureSettings.h"

#include "CaptureWorker.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>

namespace vocalsynth
{

namespace
{
const juce::Identifier versionId ("version");
const juce::Identifier exportVersionId ("exportVersion");
const juce::Identifier quantizeId ("quantize");
const juce::Identifier quantizeStrengthId ("quantizeStrength");
const juce::Identifier quantizeEndsId ("quantizeEnds");
const juce::Identifier bendsId ("bends");
const juce::Identifier controlChangesId ("controlChanges");
const juce::Identifier cleanMinNoteMsId ("cleanMinNoteMs");
const juce::Identifier freePreRollMsId ("freePreRollMs");
const juce::Identifier smfFormatId ("smfFormat");
const juce::Identifier wavFormatId ("wavFormat");
const juce::Identifier grabBarsId ("grabBars");
const juce::Identifier grabSecondsId ("grabSeconds");
const juce::Identifier takeGapSecondsId ("takeGapSeconds");
const juce::Identifier audioHistoryMinutesId ("audioHistoryMinutes");
const juce::Identifier takesFolderId ("takesFolder");

/** Index property clamped to [0, count - 1]; missing or not a number: fallback. */
int readIndex (const juce::ValueTree& tree, const juce::Identifier& id, int count, int fallback)
{
    const auto& v = tree.getProperty (id);

    if (v.isVoid() || ! (v.isInt() || v.isInt64() || v.isDouble() || v.isBool() || v.isString()))
        return fallback;

    const auto d = static_cast<double> (v);

    if (! std::isfinite (d))
        return fallback;

    return std::clamp (static_cast<int> (std::lround (d)), 0, count - 1);
}

double readDouble (const juce::ValueTree& tree, const juce::Identifier& id, double minValue, double maxValue,
                   double fallback)
{
    const auto& v = tree.getProperty (id);

    if (v.isVoid())
        return fallback;

    const auto d = static_cast<double> (v);
    return std::isfinite (d) ? std::clamp (d, minValue, maxValue) : fallback;
}

bool readBool (const juce::ValueTree& tree, const juce::Identifier& id, bool fallback)
{
    const auto& v = tree.getProperty (id);
    return v.isVoid() ? fallback : static_cast<bool> (v);
}

/** The choice nearest to value (ties: the smaller one). */
template <std::size_t N>
int snapToChoice (double value, const std::array<int, N>& choices, int fallback)
{
    if (! std::isfinite (value))
        return fallback;

    auto best = choices[0];

    for (const auto c : choices)
        if (std::abs (static_cast<double> (c) - value) < std::abs (static_cast<double> (best) - value))
            best = c;

    return best;
}
} // namespace

bool CaptureState::operator== (const CaptureState& o) const noexcept
{
    return sameRenderOptions (render, o.render) && smfFormat == o.smfFormat && wavFormat == o.wavFormat
           && grabBars == o.grabBars && grabSeconds == o.grabSeconds
           && std::equal_to<double>() (takeGapSeconds, o.takeGapSeconds)
           && std::equal_to<double>() (audioHistoryMinutes, o.audioHistoryMinutes) && takesFolder == o.takesFolder;
}

juce::ValueTree CaptureState::toValueTree() const
{
    juce::ValueTree tree (treeType);
    tree.setProperty (versionId, currentVersion, nullptr);
    tree.setProperty (exportVersionId, static_cast<int> (render.version), nullptr);
    tree.setProperty (quantizeId, static_cast<int> (render.quantize), nullptr);
    tree.setProperty (quantizeStrengthId, render.quantizeStrengthPercent, nullptr);
    tree.setProperty (quantizeEndsId, render.quantizeEnds, nullptr);
    tree.setProperty (bendsId, static_cast<int> (render.bends), nullptr);
    tree.setProperty (controlChangesId, render.controlChanges, nullptr);
    tree.setProperty (cleanMinNoteMsId, render.cleanMinNoteMs, nullptr);
    tree.setProperty (freePreRollMsId, render.freePreRollMs, nullptr);
    tree.setProperty (smfFormatId, static_cast<int> (smfFormat), nullptr);
    tree.setProperty (wavFormatId, static_cast<int> (wavFormat), nullptr);
    tree.setProperty (grabBarsId, grabBars, nullptr);
    tree.setProperty (grabSecondsId, grabSeconds, nullptr);
    tree.setProperty (takeGapSecondsId, takeGapSeconds, nullptr);
    tree.setProperty (audioHistoryMinutesId, audioHistoryMinutes, nullptr);
    tree.setProperty (takesFolderId, takesFolder, nullptr);
    return tree;
}

CaptureState CaptureState::fromValueTree (const juce::ValueTree& tree)
{
    CaptureState s;

    if (! tree.isValid() || ! tree.hasType (treeType))
        return s;

    // Version 1 is the first one; later versions only add properties (unknown ones are ignored)
    const CaptureState d;
    auto& r = s.render;
    r.version = static_cast<ExportVersion> (readIndex (tree, exportVersionId, numExportVersions, static_cast<int> (d.render.version)));
    r.quantize = static_cast<QuantizeGrid> (readIndex (tree, quantizeId, numQuantizeGrids, static_cast<int> (d.render.quantize)));
    r.quantizeStrengthPercent = readDouble (tree, quantizeStrengthId, 0.0, 100.0, d.render.quantizeStrengthPercent);
    r.quantizeEnds = readBool (tree, quantizeEndsId, d.render.quantizeEnds);
    r.bends = static_cast<BendExport> (readIndex (tree, bendsId, numBendExports, static_cast<int> (d.render.bends)));
    r.controlChanges = readBool (tree, controlChangesId, d.render.controlChanges);
    r.cleanMinNoteMs = readDouble (tree, cleanMinNoteMsId, 0.0, maxCleanMinNoteMs, d.render.cleanMinNoteMs);
    r.freePreRollMs = readDouble (tree, freePreRollMsId, 0.0, maxFreePreRollMs, d.render.freePreRollMs);
    r = sanitise (r);

    s.smfFormat = static_cast<SmfFormat> (readIndex (tree, smfFormatId, 2, static_cast<int> (d.smfFormat)));
    s.wavFormat = static_cast<WavSampleFormat> (readIndex (tree, wavFormatId, numWavSampleFormats, static_cast<int> (d.wavFormat)));
    s.grabBars = snapToChoice (readDouble (tree, grabBarsId, -1.0e9, 1.0e9, d.grabBars), TakeSegmenter::grabBarChoices,
                               d.grabBars);
    s.grabSeconds = snapToChoice (readDouble (tree, grabSecondsId, -1.0e9, 1.0e9, d.grabSeconds),
                                  TakeSegmenter::grabSecondChoices, d.grabSeconds);
    s.takeGapSeconds = readDouble (tree, takeGapSecondsId, TakeSegmenter::minTakeGapSeconds,
                                   TakeSegmenter::maxTakeGapSeconds, d.takeGapSeconds);
    s.audioHistoryMinutes = readDouble (tree, audioHistoryMinutesId, CaptureHistory::minAudioMinutes,
                                        CaptureHistory::maxAudioMinutes, d.audioHistoryMinutes);
    s.takesFolder = tree.getProperty (takesFolderId, d.takesFolder).toString();
    return s;
}

} // namespace vocalsynth
