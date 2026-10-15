#pragma once

#include "vocalsynth/PitchDetector.h"

#include <array>

namespace vocalsynth
{

/** Frame-to-frame cleanup of the detector output (docs/M2-DESIGN.md 3.6). Called once per hop.

    1. Median filter over the last medianLength frames (3 by default, 5 allowed):
       - frequencyHz: median of the frequencies, unvoiced frames counting as 0 Hz, so the median is
         also a majority vote on voicing ({0, 0, f} -> 0, {0, f, f} -> f);
       - confidence: median of the confidences (independently);
       - rmsDb: passed through from the NEWEST frame (a level has no outliers; keeps the gate fast).
       The filter output lags its input by medianLength / 2 frames (1 hop for 3). Before medianLength
       frames were seen, the missing history counts as unvoiced (0 Hz, confidence 0).
    2. Octave guard on the median output. "Voiced" for the guard means frequencyHz > 0 and
       confidence >= guardMinConfidence (YIN reports low-confidence pitches of noise; they must not set a
       reference that would then "correct" the next real onset). Frames below it pass through unchanged,
       count as unvoiced and end a suspect run.
       Reference = last accepted voiced pitch, forgotten after
       octaveMemoryMs without a voiced frame. A voiced frame whose pitch is 12 +- octaveToleranceSemitones
       semitones above (or below) the reference is "suspect": it is output octave-corrected (f / 2 or
       f * 2, confidence and rmsDb unchanged) and the reference is kept. Only after suspect frames in the
       same direction have persisted for octaveConfirmMs (consecutively) is the new octave accepted: that
       frame is output unchanged and becomes the reference. Any other voiced frame is output unchanged
       and becomes the reference. Unvoiced frames pass through.

    Real-time safe, no allocation (fixed std::array history). Audio thread only. */
class PitchPostProcessor
{
public:
    static constexpr int defaultMedianLength = 3;
    static constexpr int maxMedianLength = 5;
    static constexpr double octaveConfirmMs = 25.0;
    static constexpr double octaveMemoryMs = 60.0;
    static constexpr double octaveToleranceSemitones = 1.0;
    static constexpr float guardMinConfidence = 0.6f; ///< == NoteSegmenter::confidenceOff (weak voiced)

    /** frameRateHz = analysis frames per second (analysis rate / hop). Converts the ms constants to
        frames (rounded up, at least 1). Calls reset(). */
    void prepare (double frameRateHz) noexcept;

    /** Forgets the median history and the octave reference. */
    void reset() noexcept;

    /** 3 or 5 (other values: clamped to [1, maxMedianLength] and made odd). Calls reset(). */
    void setMedianLength (int length) noexcept;
    int getMedianLength() const noexcept { return medianLength; }

    /** Frames of delay added by the median: medianLength / 2. */
    int getDelayFrames() const noexcept { return medianLength / 2; }

    /** Processes one detector frame and returns the cleaned frame. */
    PitchFrame process (const PitchFrame& input) noexcept;

private:
    int medianLength = defaultMedianLength;
    std::array<PitchFrame, maxMedianLength> history {};
    int historyPos = 0;

    int octaveConfirmFrames = 10;
    int octaveMemoryFrames = 24;
    float referenceHz = 0.0f;      ///< last accepted voiced pitch, 0 = none
    int framesSinceVoiced = 0;
    int suspectDirection = 0;      ///< +1 = up an octave, -1 = down, 0 = none
    int suspectFrames = 0;
};

} // namespace vocalsynth
