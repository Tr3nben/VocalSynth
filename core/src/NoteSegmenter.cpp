#include "vocalsynth/NoteSegmenter.h"

#include "vocalsynth/Pitch.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace vocalsynth
{

StickinessParams NoteSegmenter::stickinessFromPercent (double percent) noexcept
{
    const auto u = (std::isnan (percent) ? 50.0 : std::clamp (percent, 0.0, 100.0)) / 100.0; // NaN -> default
    return { 0.4 * u, 10.0 + 50.0 * u };
}

uint8_t NoteSegmenter::velocityFor (double onsetRmsDb, double gateThresholdDb, double sensitivityPercent) noexcept
{
    const auto s = (std::isnan (sensitivityPercent) ? 70.0 : std::clamp (sensitivityPercent, 0.0, 100.0))
                   / 100.0;
    const auto rel = (onsetRmsDb - gateThresholdDb) / velocityRangeDb;
    const auto x = std::isnan (rel) ? 0.0 : std::clamp (rel, 0.0, 1.0);
    const auto full = static_cast<double> (minFullVelocity) + static_cast<double> (127 - minFullVelocity) * x;
    const auto velocity = std::lround ((1.0 - s) * static_cast<double> (fixedVelocity) + s * full);
    return static_cast<uint8_t> (std::clamp (velocity, 1L, 127L));
}

void NoteSegmenter::prepare (double newSampleRate) noexcept
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    reset();
}

void NoteSegmenter::reset() noexcept
{
    onsetFrames = 0;
    onsetMaxRmsDb = PitchFrame::silenceDb;
    unvoicedFrames = 0;
    releaseCountFrames = 0;
    activeQuantisedNote = -1;
    activeOutputNote = -1;
    activeChannel = 1;
    noteStartSample = 0;
    changeTimerRunning = false;
    changeTimerStart = 0;
    lastFrameVoiced = false;
    noteLevelDb = PitchFrame::silenceDb;
    noteLevelSample = 0;
    historyCount = 0;
    historyNext = 0;
    noteFromRest = false;
    restStartSample = 0;
    dropoutHeld = false;
    reentryPending = false;
    reentryDeadline = 0;
    levelHistoryCount = 0;
    levelHistoryNext = 0;
    provisional = false;
    provisionalStartSample = 0;
    provisionalSeenStrong = false;
    fastArmed = true;
    fastBlocked = false;
    fastFrames = 0;
    fastLastPitch = 0.0;
    fastMaxRmsDb = PitchFrame::silenceDb;

    lastTiming = {};
    hasLevelOnset = false;
    levelOnsetSample = 0;
    hasLevelRelease = false;
    levelReleaseSample = 0;
    noteOnTiming = {};
    noteEstimatedStart = 0;
    hasPreviousNoteEnd = false;
    previousNoteEnd = 0;
    releaseRunStart = 0;
}

void NoteSegmenter::setDetectionDelayModel (const DetectionDelayModel& model) noexcept
{
    delayModel = model;
    delayModel.onsetSearchSamples = std::max<int64_t> (0, model.onsetSearchSamples);
    delayModel.confirmedOnsetDelaySamples = std::max<int64_t> (0, model.confirmedOnsetDelaySamples);
    delayModel.frameDelaySamples = std::isfinite (model.frameDelaySamples) ? std::max (0.0, model.frameDelaySamples) : 0.0;
}

void NoteSegmenter::setLevelOnsetSample (int64_t absoluteSample) noexcept
{
    hasLevelOnset = true;
    levelOnsetSample = absoluteSample;
}

void NoteSegmenter::setLevelReleaseSample (int64_t absoluteSample) noexcept
{
    hasLevelRelease = true;
    levelReleaseSample = absoluteSample;
}

VsEventTiming NoteSegmenter::timingFor (int64_t emitSample, int64_t estimate, EventTimingKind kind, uint8_t flags) noexcept
{
    const auto delay = std::clamp<int64_t> (emitSample - estimate, 0, std::numeric_limits<int32_t>::max());
    return { static_cast<int32_t> (delay), kind, flags };
}

int64_t NoteSegmenter::frameDelay() const noexcept
{
    return static_cast<int64_t> (std::llround (delayModel.frameDelaySamples));
}

VsEventTiming NoteSegmenter::onsetTiming (int64_t absoluteSample, bool provisionalPath, double p) const noexcept
{
    // The newest upward level crossing, if it belongs to this note: not too long before the emit sample
    // and after the previous note ended (a level that never fell between two notes is no onset)
    if (hasLevelOnset && levelOnsetSample <= absoluteSample
        && absoluteSample - levelOnsetSample <= delayModel.onsetSearchSamples
        && (! hasPreviousNoteEnd || levelOnsetSample >= previousNoteEnd))
        return timingFor (absoluteSample, levelOnsetSample, EventTimingKind::measured, 0);

    auto delay = delayModel.confirmedOnsetDelaySamples;

    if (provisionalPath && delayModel.fastOnsetDelay != nullptr)
        delay = delayModel.fastOnsetDelay (delayModel.fastOnsetDelayContext, midiNoteToFrequency (p));

    return timingFor (absoluteSample, absoluteSample - delay, EventTimingKind::model, 0);
}

VsEventTiming NoteSegmenter::changeTiming (int64_t absoluteSample) const noexcept
{
    const auto estimate = std::max (changeTimerStart - frameDelay(), noteEstimatedStart + 1);
    return timingFor (absoluteSample, std::min (estimate, absoluteSample), EventTimingKind::measured,
                      EventTimingFlags::noteChange);
}

VsEventTiming NoteSegmenter::releaseTiming (int64_t absoluteSample) const noexcept
{
    auto estimate = releaseRunStart - frameDelay();

    if (hasLevelRelease && levelReleaseSample > noteEstimatedStart && levelReleaseSample <= absoluteSample)
        estimate = levelReleaseSample;

    estimate = std::clamp (estimate, noteEstimatedStart + 1, absoluteSample);
    return timingFor (absoluteSample, estimate, EventTimingKind::measured, 0);
}

void NoteSegmenter::setOnsetTiming (int64_t ignoreSamples, int64_t timeoutSamples, int64_t settleSamples) noexcept
{
    provisionalIgnoreSamples = std::max<int64_t> (0, ignoreSamples);
    provisionalTimeoutSamples = std::max<int64_t> (0, timeoutSamples);
    provisionalSettleSamples = std::max<int64_t> (0, settleSamples);
}

void NoteSegmenter::processOnsetFrame (const PitchFrame& frame, int64_t absoluteSample, int sampleOffset,
                                       BendGenerator& bend, VsEventSink out) noexcept
{
    lastTiming = {};

    if (activeOutputNote >= 0)
        return;

    // After a false start the fast path waits for a gap (a level below the gate). Such a frame is
    // never fast-strong, so isFastPathBlocked() callers may pass an RMS-only frame (same result).
    if (frame.rmsDb < gateThresholdDb)
        fastBlocked = false;

    if (fastBlocked)
        return;

    const auto hasPitch = frame.frequencyHz > 0.0f && std::isfinite (frame.frequencyHz);
    const auto strong = hasPitch && frame.confidence >= fastConfidenceOn && frame.rmsDb >= gateThresholdDb;

    if (! strong)
    {
        fastArmed = true;
        fastFrames = 0;
        return;
    }

    if (! fastArmed)
        return;

    const auto p = frequencyToMidiNote (static_cast<double> (frame.frequencyHz));

    if (fastFrames > 0 && std::abs (p - fastLastPitch) * 100.0 > fastMaxSpreadCents)
        fastFrames = 0;

    fastMaxRmsDb = fastFrames == 0 ? frame.rmsDb : std::max (fastMaxRmsDb, frame.rmsDb);
    fastLastPitch = p;

    fastFrames = std::min (fastFrames + 1, fastOnsetConfirmFrames); // bounded: a run can go on (below)

    if (fastFrames < fastOnsetConfirmFrames)
        return;

    // The note identity must be clear: a pitch within fastNoteMarginSemitones of the boundary between two
    // allowed notes (a vibrato trough at the onset, a scoop, the sharp first frames of an onset) is not
    // started from; the run goes on, so the next frame starts the note once the pitch has moved inside
    // a note (still within fastMaxSpreadCents), else the full window decides.
    const auto q = quantizer.quantize (p, -1);

    if (quantizer.quantize (p - fastNoteMarginSemitones, -1) != q || quantizer.quantize (p + fastNoteMarginSemitones, -1) != q)
        return;

    const auto outputNote = q + 12 * octaveShift;

    if (! isPlayable (outputNote))
    {
        fastFrames = 0;
        return;
    }

    const auto velocity = velocityFor (static_cast<double> (fastMaxRmsDb), static_cast<double> (gateThresholdDb),
                                       velocitySensitivityPercent);
    startNote (q, outputNote, velocity, fastMaxRmsDb, p, absoluteSample, sampleOffset, bend, out,
               onsetTiming (absoluteSample, true, p));
    noteFromRest = true;
    restStartSample = absoluteSample;
    provisional = true;
    provisionalStartSample = absoluteSample;
    provisionalSeenStrong = false;
    lastFrameVoiced = true;
}

void NoteSegmenter::setSettings (const VoiceSettings& settings) noexcept
{
    const auto toSamples = [this] (double ms) noexcept
    {
        return static_cast<int64_t> (std::llround (ms * 0.001 * sampleRate));
    };

    const auto stickiness = stickinessFromPercent (static_cast<double> (settings.stickinessPercent));

    quantizer.setScale (settings.key, settings.scale);
    gateThresholdDb = settings.gateThresholdDb;
    hysteresisSemitones = stickiness.hysteresisSemitones;
    minChangeSamples = toSamples (stickiness.minChangeMs);
    arrivalSamples = toSamples (arrivalFraction * stickiness.minChangeMs);
    glideWindowSamples = toSamples (glideWindowMs);
    onsetGlideHoldSamples = toSamples (onsetGlideHoldMs);
    onsetGlideWindowSamples = toSamples (onsetGlideWindowMs);
    minNoteSamples = toSamples (static_cast<double> (settings.minNoteLengthMs));
    dropoutHoldSamples = toSamples (dropoutHoldMs);
    dropoutLevelWindowSamples = toSamples (dropoutLevelWindowMs);
    octaveShift = settings.octaveShift;
    velocitySensitivityPercent = static_cast<double> (settings.velocitySensitivityPercent);
    legato = settings.legato;
    channel = static_cast<uint8_t> (std::clamp (settings.channel, 1, 16));
}

void NoteSegmenter::processFrame (const PitchFrame& frame, int64_t absoluteSample, int sampleOffset,
                                  BendGenerator& bend, VsEventSink out) noexcept
{
    // NaN in any field makes the comparisons false, i.e. the frame unvoiced.
    const auto hasPitch = frame.frequencyHz > 0.0f && std::isfinite (frame.frequencyHz);
    const auto strong = hasPitch && frame.confidence >= confidenceOn && frame.rmsDb >= gateThresholdDb;
    const auto weak = hasPitch && frame.confidence >= confidenceOff
                      && frame.rmsDb >= gateThresholdDb - gateHysteresisDb;
    const auto p = hasPitch ? frequencyToMidiNote (static_cast<double> (frame.frequencyHz)) : 0.0;
    lastTiming = {};
    pushLevelHistory (frame.rmsDb, absoluteSample);

    if (activeOutputNote < 0)
        processIdleFrame (frame, strong, p, absoluteSample, sampleOffset, bend, out);
    else if (! provisional || ! processProvisionalFrame (frame, strong, weak, p, absoluteSample, sampleOffset, bend, out))
        processNoteFrame (frame, weak, p, absoluteSample, sampleOffset, bend, out);
}

bool NoteSegmenter::processProvisionalFrame (const PitchFrame& frame, bool strong, bool weak, double p,
                                             int64_t absoluteSample, int sampleOffset, BendGenerator& bend,
                                             VsEventSink out) noexcept
{
    const auto age = absoluteSample - provisionalStartSample;

    // The octave shift moved the note out of 0..127: the normal path ends it at once
    if (! isPlayable (activeQuantisedNote + 12 * octaveShift))
    {
        provisional = false;
        return false;
    }

    if (age < provisionalIgnoreSamples)
        return true;

    if (strong)
    {
        // The full window is trusted for a fine decision only once it no longer holds audio from before
        // the start (the onset, the start of a scoop into the note), i.e. after provisionalSettleSamples:
        // before that it lags behind the fast frames, and a small margin corrected towards the stale
        // part (a rising scoop read as the semitone below). So while settling only a clear error of the
        // fast frames (another note or octave: the candidate wins by 2 h) is corrected at once, and the
        // note stays provisional; the first strong frame after that moves it to its nearest note
        // (margin provisionalSettleMarginSemitones, 2 cents: a pitch midway between two allowed notes
        // reads 54.002 or 53.998 by detector noise, which must not flip the note) and ends the provisional
        // state. With 2 h there, a
        // mid-scoop reading kept a wrong-semitone start for T_min + minNoteLength (v0.2.1 review,
        // vocadito_1: note F1 0.264 -> 0.225; now 0.278).
        const auto settling = age < provisionalSettleSamples;
        const auto candidate = quantizer.quantize (p, activeQuantisedNote);
        const auto q = static_cast<double> (activeQuantisedNote);
        const auto advantage = std::abs (p - q) - std::abs (p - static_cast<double> (candidate));
        const auto needed = settling ? 2.0 * hysteresisSemitones
                                     : std::min (provisionalSettleMarginSemitones, 2.0 * hysteresisSemitones);
        provisionalSeenStrong = true;
        provisional = settling;

        if (candidate != activeQuantisedNote && advantage >= needed - stickinessEpsilonSemitones
            && isPlayable (candidate + 12 * octaveShift))
        {
            unvoicedFrames = 0;
            auto correction = noteOnTiming;
            correction.flags = static_cast<uint8_t> (correction.flags | EventTimingFlags::correction);
            const auto provisionalStart = noteEstimatedStart;
            changeNote (candidate, candidate + 12 * octaveShift, frame, p, absoluteSample, sampleOffset, bend, out,
                        correction);
            noteEstimatedStart = provisionalStart; // the corrected note is the provisional one
            noteFromRest = true;                   // ... and still the start from rest (onset glide hold)
            return true; // a normal note from now on (startNote() cleared provisional)
        }

        return false; // processed like a normal note's frame (still provisional while settling)
    }

    if (age < provisionalTimeoutSamples)
        return true; // hold: the full window is still filling

    provisional = false;

    // A note the full window has already seen (strong) is a normal one now, also when it ends here
    if (weak || provisionalSeenStrong)
        return false;

    endNote (absoluteSample, sampleOffset, bend, out, noteOnTiming); // false start
    fastBlocked = true;
    return true;
}

void NoteSegmenter::processIdleFrame (const PitchFrame& frame, bool strong, double p, int64_t absoluteSample,
                                      int sampleOffset, BendGenerator& bend, VsEventSink out) noexcept
{
    const auto q = strong ? quantizer.quantize (p, -1) : -1;
    const auto outputNote = q + 12 * octaveShift;

    // A note that the octave shift moves outside 0..127 is not played: the frame counts as unvoiced.
    if (! strong || ! isPlayable (outputNote))
    {
        onsetFrames = 0;
        onsetMaxRmsDb = PitchFrame::silenceDb;
        lastFrameVoiced = false;
        return;
    }

    lastFrameVoiced = true;
    onsetMaxRmsDb = onsetFrames == 0 ? frame.rmsDb : std::max (onsetMaxRmsDb, frame.rmsDb);

    if (++onsetFrames < onsetConfirmFrames)
        return;

    const auto velocity = velocityFor (static_cast<double> (onsetMaxRmsDb), static_cast<double> (gateThresholdDb),
                                       velocitySensitivityPercent);
    startNote (q, outputNote, velocity, onsetMaxRmsDb, p, absoluteSample, sampleOffset, bend, out,
               onsetTiming (absoluteSample, false, p));
    noteFromRest = true;
    restStartSample = absoluteSample;
    fastBlocked = false; // the full window agrees with a voice again: the false start was a one-off
}

bool NoteSegmenter::isPitchReliable (const PitchFrame& frame, int64_t absoluteSample) noexcept
{
    const auto elapsedSeconds = static_cast<double> (absoluteSample - noteLevelSample) / sampleRate;
    const auto fallen = static_cast<double> (noteLevelDb) - levelFallDbPerSecond * std::max (0.0, elapsedSeconds);
    noteLevelDb = std::max (frame.rmsDb, static_cast<float> (std::max (fallen, static_cast<double> (PitchFrame::silenceDb))));
    noteLevelSample = absoluteSample;
    return frame.confidence >= confidenceOn && frame.rmsDb >= noteLevelDb - bendHoldDropDb;
}

float NoteSegmenter::noteLevelAt (int64_t absoluteSample) const noexcept
{
    const auto elapsedSeconds = static_cast<double> (absoluteSample - noteLevelSample) / sampleRate;
    const auto fallen = static_cast<double> (noteLevelDb) - levelFallDbPerSecond * std::max (0.0, elapsedSeconds);
    return static_cast<float> (std::max (fallen, static_cast<double> (PitchFrame::silenceDb)));
}

bool NoteSegmenter::isDropout (const PitchFrame& frame, double p, int64_t absoluteSample) const noexcept
{
    // The voice is still there but the detector lost it at a level step (see class comment): no
    // weak-voiced pitch, a level near the note's, a level step in the recent frames. A low-confidence pitch
    // must still read the held note (change hysteresis); below dropoutPitchConfidence it is noise (YIN at
    // the rise after a dip: 2-3 semitones sharp at confidence 0-0.2). NaN fails every comparison.
    const auto hasPitch = frame.frequencyHz > 0.0f && std::isfinite (frame.frequencyHz);

    if (hasPitch && frame.confidence >= confidenceOff)
        return false;

    if (hasPitch && frame.confidence >= dropoutPitchConfidence)
    {
        const auto candidate = quantizer.quantize (p, activeQuantisedNote);
        const auto q = static_cast<double> (activeQuantisedNote);
        const auto advantage = std::abs (p - q) - std::abs (p - static_cast<double> (candidate));

        if (candidate != activeQuantisedNote && advantage >= 2.0 * hysteresisSemitones - stickinessEpsilonSemitones)
            return false;
    }

    return frame.rmsDb >= gateThresholdDb && frame.rmsDb >= noteLevelAt (absoluteSample) - dropoutMaxDropDb
           && levelRange (absoluteSample, dropoutLevelWindowSamples) >= dropoutMinLevelStepDb;
}

void NoteSegmenter::pushLevelHistory (float rmsDb, int64_t absoluteSample) noexcept
{
    const auto index = static_cast<std::size_t> (levelHistoryNext);
    levelHistoryDb[index] = std::isfinite (rmsDb) ? std::max (rmsDb, PitchFrame::silenceDb) : PitchFrame::silenceDb;
    levelHistorySample[index] = absoluteSample;
    levelHistoryNext = levelHistoryNext + 1 < levelHistoryFrames ? levelHistoryNext + 1 : 0;
    levelHistoryCount = std::min (levelHistoryCount + 1, levelHistoryFrames);
}

float NoteSegmenter::levelRange (int64_t absoluteSample, int64_t windowSamples) const noexcept
{
    auto lo = std::numeric_limits<float>::max();
    auto hi = std::numeric_limits<float>::lowest();

    for (int i = 0; i < levelHistoryCount; ++i)
    {
        const auto index = static_cast<std::size_t> ((levelHistoryNext - 1 - i + levelHistoryFrames) % levelHistoryFrames);

        if (absoluteSample - levelHistorySample[index] > windowSamples)
            break;

        lo = std::min (lo, levelHistoryDb[index]);
        hi = std::max (hi, levelHistoryDb[index]);
    }

    return hi >= lo ? hi - lo : 0.0f;
}

void NoteSegmenter::pushPitchHistory (double p, int64_t absoluteSample) noexcept
{
    const auto index = static_cast<std::size_t> (historyNext);
    historyPitch[index] = p;
    historySample[index] = absoluteSample;
    historyNext = historyNext + 1 < pitchHistoryFrames ? historyNext + 1 : 0;
    historyCount = std::min (historyCount + 1, pitchHistoryFrames);
}

double NoteSegmenter::pitchSlope (int64_t absoluteSample, int64_t windowSamples) const noexcept
{
    // Least squares over (t, p), t in seconds relative to absoluteSample (newest first)
    double sumT = 0.0, sumP = 0.0, sumTT = 0.0, sumTP = 0.0;
    int n = 0;

    for (int i = 0; i < historyCount; ++i)
    {
        const auto index = static_cast<std::size_t> ((historyNext - 1 - i + pitchHistoryFrames) % pitchHistoryFrames);
        const auto age = absoluteSample - historySample[index];

        if (age > windowSamples)
            break;

        const auto t = -static_cast<double> (age) / sampleRate;
        const auto v = historyPitch[index];
        sumT += t;
        sumP += v;
        sumTT += t * t;
        sumTP += t * v;
        ++n;
    }

    if (n < 3)
        return 0.0;

    const auto count = static_cast<double> (n);
    const auto denominator = count * sumTT - sumT * sumT;
    return denominator > 0.0 ? (count * sumTP - sumT * sumP) / denominator : 0.0;
}

bool NoteSegmenter::hasArrived (int64_t absoluteSample, int64_t windowSamples, int candidate) const noexcept
{
    auto lo = std::numeric_limits<double>::max();
    auto hi = std::numeric_limits<double>::lowest();

    for (int i = 0; i < historyCount; ++i)
    {
        const auto index = static_cast<std::size_t> ((historyNext - 1 - i + pitchHistoryFrames) % pitchHistoryFrames);

        if (absoluteSample - historySample[index] > windowSamples)
            return hi - lo <= arrivalMaxSpreadSemitones + stickinessEpsilonSemitones; // the span is covered

        const auto v = historyPitch[index];

        if (std::abs (v - static_cast<double> (candidate)) > arrivalMaxDistanceSemitones + stickinessEpsilonSemitones)
            return false;

        lo = std::min (lo, v);
        hi = std::max (hi, v);
    }

    return false; // the history does not reach back over the whole span
}

bool NoteSegmenter::isGliding (int64_t absoluteSample) const noexcept
{
    // Only the frames since the change timer started count: those beyond the change threshold. A step to
    // another note is flat there (its jump lies before), a glide through or into the note is not.
    const auto sinceTimer = absoluteSample - changeTimerStart;

    if (noteFromRest && absoluteSample - restStartSample < onsetGlideHoldSamples
        && std::abs (pitchSlope (absoluteSample, std::min (onsetGlideWindowSamples, sinceTimer))) > onsetGlideHoldSemitonesPerSecond)
        return true;

    return std::abs (pitchSlope (absoluteSample, std::min (glideWindowSamples, sinceTimer))) > glideHoldSemitonesPerSecond;
}

void NoteSegmenter::processNoteFrame (const PitchFrame& frame, bool weak, double p, int64_t absoluteSample,
                                      int sampleOffset, BendGenerator& bend, VsEventSink out) noexcept
{
    const auto shiftedNote = activeQuantisedNote + 12 * octaveShift;

    // Octave shift changed so that the held note is no longer playable: end it at once, on any frame.
    if (! isPlayable (shiftedNote))
    {
        endNote (absoluteSample, sampleOffset, bend, out, {});
        return;
    }

    // A pitch whose candidate note would be unplayable is treated like an unvoiced frame (release path).
    auto candidate = -1;
    auto changeWanted = false;

    // Only a frame with confidence >= confidenceOn may ask for a note change (start or keep the T_min
    // timer); weaker frames only hold the note. Detector octave errors mostly come with confidence in
    // [confidenceOff, confidenceOn) (YIN fallback and inconsistent frames are capped there, a tone
    // below the range reads as 2 f0 at 0.56-0.79), and the octave guard does not check such frames
    // (PitchPostProcessor::guardMinConfidence): a held A2 followed by a G2 below the Tenor range
    // became a G3. A note can only start on strong frames, so a change needs the same.
    if (weak)
    {
        candidate = quantizer.quantize (p, activeQuantisedNote);
        const auto q = static_cast<double> (activeQuantisedNote);
        const auto advantage = std::abs (p - q) - std::abs (p - static_cast<double> (candidate));
        changeWanted = candidate != activeQuantisedNote && frame.confidence >= confidenceOn
                       && advantage >= 2.0 * hysteresisSemitones - stickinessEpsilonSemitones;

        if (changeWanted && ! isPlayable (candidate + 12 * octaveShift))
            weak = false;
    }

    if (! weak)
    {
        historyCount = 0;
        lastFrameVoiced = false;
        changeTimerRunning = false;

        if (unvoicedFrames == 0)
        {
            releaseRunStart = absoluteSample;
            releaseCountFrames = 0;
        }

        unvoicedFrames = std::min (unvoicedFrames + 1, releaseFrames); // > 0 = in a run (bounded)

        // Dropout hold (v0.3.1): a loud frame without a weak-voiced pitch early in the run does not count
        if (absoluteSample - releaseRunStart < dropoutHoldSamples && isDropout (frame, p, absoluteSample))
        {
            dropoutHeld = true;
            return;
        }

        // The note is held (no bend updates) until releaseFrames counted frames AND minNoteLength.
        releaseCountFrames = std::min (releaseCountFrames + 1, releaseFrames);

        if (releaseCountFrames >= releaseFrames && absoluteSample - noteStartSample >= minNoteSamples)
            endNote (absoluteSample, sampleOffset, bend, out, releaseTiming (absoluteSample));

        return;
    }

    lastFrameVoiced = true;

    if (unvoicedFrames > 0 && dropoutHeld)
    {
        // The voice is back after a held dropout: the first strong frame decides about a re-entry
        reentryPending = true;
        reentryDeadline = absoluteSample + dropoutHoldSamples;
    }

    unvoicedFrames = 0;
    dropoutHeld = false;

    // Pitch history for the v0.3.1 change rules: consecutive frames that may request a change
    if (frame.confidence >= confidenceOn)
        pushPitchHistory (p, absoluteSample);
    else
        historyCount = 0;

    const auto residualCents = 100.0 * (p - static_cast<double> (activeQuantisedNote));
    const auto reliable = isPitchReliable (frame, absoluteSample);

    // Octave shift changed while the note is held: immediate change to the same q (no T_min, no
    // minNoteLength), with the usual legato/retrigger order.
    if (shiftedNote != activeOutputNote)
    {
        changeNote (activeQuantisedNote, shiftedNote, frame, p, absoluteSample, sampleOffset, bend, out, {});
        return;
    }

    lastTiming.update = timingFor (absoluteSample, absoluteSample - frameDelay(), EventTimingKind::measured, 0);

    // Re-entry after a held dropout (v0.3.1): the voice came back at another note. Without the hold this
    // was a release and a new onset; the change comes at once, as that onset did (a big legato leap
    // whose transition windows give no pitch), instead of after T_min. The first strong frame (or the
    // deadline) decides; the same note just continues.
    if (reentryPending && (frame.confidence >= confidenceOn || absoluteSample >= reentryDeadline))
    {
        reentryPending = false;

        if (changeWanted && frame.rmsDb >= gateThresholdDb && absoluteSample - noteStartSample >= minNoteSamples)
        {
            lastTiming.update = {};
            changeTimerStart = absoluteSample; // the estimate: where the voice came back
            changeNote (candidate, candidate + 12 * octaveShift, frame, p, absoluteSample, sampleOffset, bend, out,
                        changeTiming (absoluteSample));
            return;
        }
    }

    if (! changeWanted)
    {
        changeTimerRunning = false;

        if (reliable)
            bend.update (residualCents, absoluteSample, sampleOffset, activeChannel, out);

        return;
    }

    if (! changeTimerRunning)
    {
        changeTimerRunning = true;
        changeTimerStart = absoluteSample;
    }

    // T_min, or the shorter arrival span once the voice has landed on the candidate; neither while the
    // pitch is still gliding (the timer keeps running: the change comes where the glide ends)
    const auto elapsed = absoluteSample - changeTimerStart;
    const auto timeUp = elapsed >= minChangeSamples
                        || (elapsed >= arrivalSamples && hasArrived (absoluteSample, arrivalSamples, candidate));

    if (timeUp && absoluteSample - noteStartSample >= minNoteSamples && ! isGliding (absoluteSample))
    {
        lastTiming.update = {};
        changeNote (candidate, candidate + 12 * octaveShift, frame, p, absoluteSample, sampleOffset, bend, out,
                    changeTiming (absoluteSample));
        return;
    }

    if (reliable)
        bend.update (residualCents, absoluteSample, sampleOffset, activeChannel, out);
}

void NoteSegmenter::startNote (int q, int outputNote, uint8_t velocity, float levelDb, double p,
                               int64_t absoluteSample, int sampleOffset, BendGenerator& bend, VsEventSink out,
                               const VsEventTiming& timing) noexcept
{
    lastTiming.noteOn = timing;
    noteOnTiming = timing;
    noteEstimatedStart = absoluteSample - timing.delaySamples;

    noteLevelDb = levelDb;
    noteLevelSample = absoluteSample;
    activeQuantisedNote = q;
    activeOutputNote = outputNote;
    activeChannel = channel;
    noteStartSample = absoluteSample;
    unvoicedFrames = 0;
    changeTimerRunning = false;
    historyCount = 0;
    noteFromRest = false; // the callers that start from rest set it again
    dropoutHeld = false;
    reentryPending = false;
    onsetFrames = 0;
    onsetMaxRmsDb = PitchFrame::silenceDb;
    provisional = false;
    fastArmed = false;
    fastFrames = 0;

    bend.noteStarted (100.0 * (p - static_cast<double> (q)), absoluteSample, sampleOffset, activeChannel, out);
    out.push (VsEvent::makeNoteOn (sampleOffset, activeChannel, static_cast<uint8_t> (outputNote), velocity));
}

void NoteSegmenter::changeNote (int q, int outputNote, const PitchFrame& frame, double p, int64_t absoluteSample,
                                int sampleOffset, BendGenerator& bend, VsEventSink out, const VsEventTiming& timing) noexcept
{
    // The old note ends where the new one starts (same estimate)
    lastTiming.noteOff = timing;
    hasPreviousNoteEnd = true;
    previousNoteEnd = absoluteSample - timing.delaySamples;

    const auto oldNote = static_cast<uint8_t> (activeOutputNote);
    const auto oldChannel = activeChannel;
    const auto velocity = velocityFor (static_cast<double> (frame.rmsDb), static_cast<double> (gateThresholdDb),
                                       velocitySensitivityPercent);

    if (! legato)
        out.push (VsEvent::makeNoteOff (sampleOffset, oldChannel, oldNote));

    startNote (q, outputNote, velocity, frame.rmsDb, p, absoluteSample, sampleOffset, bend, out, timing);

    if (legato)
        out.push (VsEvent::makeNoteOff (sampleOffset, oldChannel, oldNote));

    lastFrameVoiced = true;
}

void NoteSegmenter::endNote (int64_t absoluteSample, int sampleOffset, BendGenerator& bend, VsEventSink out,
                             const VsEventTiming& timing) noexcept
{
    lastTiming.noteOff = timing;
    hasPreviousNoteEnd = true;
    previousNoteEnd = absoluteSample - timing.delaySamples;

    out.push (VsEvent::makeNoteOff (sampleOffset, activeChannel, static_cast<uint8_t> (activeOutputNote)));
    bend.noteEnded (absoluteSample, sampleOffset, activeChannel, out);

    activeQuantisedNote = -1;
    activeOutputNote = -1;
    unvoicedFrames = 0;
    changeTimerRunning = false;
    onsetFrames = 0;
    onsetMaxRmsDb = PitchFrame::silenceDb;
    lastFrameVoiced = false;
    provisional = false;
}

void NoteSegmenter::flush (int64_t absoluteSample, int sampleOffset, BendGenerator& bend, VsEventSink out) noexcept
{
    if (activeOutputNote >= 0)
        endNote (absoluteSample, sampleOffset, bend, out, {});

    reset(); // also lastTiming = {}: a flush happens when it is sent (kind none)
}

} // namespace vocalsynth
