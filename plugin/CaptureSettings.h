#pragma once

#include "vocalsynth/CaptureHistory.h"
#include "vocalsynth/CaptureOptions.h"
#include "vocalsynth/SmfWriter.h"
#include "vocalsynth/TakeSegmenter.h"
#include "vocalsynth/WavWriter.h"

#include <juce_data_structures/juce_data_structures.h>

namespace vocalsynth
{

/** M3 capture settings that are NOT automatable parameters (docs/M3-DESIGN.md 13): export options, grab
    length, free-mode gap, audio look-back, takes folder. Saved with the project as the child tree
    CaptureState::treeType of the APVTS state (with a version property); takes themselves are never saved
    (decision 4). Plain value; the processor guards its copy with a mutex the audio thread never touches. */
struct CaptureState
{
    RenderOptions render;               ///< version, quantize, strength, ends, bends, CC, clean / pre-roll ms
                                        ///< (ticksPerQuarterNote and roundTripSamples are not stored)
    SmfFormat smfFormat = SmfFormat::type1;
    WavSampleFormat wavFormat = WavSampleFormat::pcm24;
    int grabBars = TakeSegmenter::defaultGrabBars;          ///< one of TakeSegmenter::grabBarChoices
    int grabSeconds = TakeSegmenter::defaultGrabSeconds;    ///< one of TakeSegmenter::grabSecondChoices
    double takeGapSeconds = TakeSegmenter::defaultTakeGapSeconds;
    double audioHistoryMinutes = CaptureHistory::defaultAudioMinutes;
    juce::String takesFolder;           ///< empty = CaptureExporter::getDefaultTakesFolder()

    /** Field by field (doubles compared exactly: the values come from the same clamping). */
    bool operator== (const CaptureState& other) const noexcept;

    static inline const juce::Identifier treeType { "CAPTURE" };
    static constexpr int currentVersion = 1;

    /** Child tree for the APVTS state (all fields, version = currentVersion). */
    juce::ValueTree toValueTree() const;

    /** Reads a tree written by toValueTree() of this or an older version; missing or invalid properties get
        their defaults, values are clamped (grab lengths snap to the nearest choice). An invalid tree (old
        project without capture state) gives the defaults. */
    static CaptureState fromValueTree (const juce::ValueTree& tree);
};

} // namespace vocalsynth
