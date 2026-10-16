#include "vocalsynth/NoteSegmenter.h"

#include "vocalsynth/Pitch.h"

#include <algorithm>
#include <cmath>

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
    activeQuantisedNote = -1;
    activeOutputNote = -1;
    activeChannel = 1;
    noteStartSample = 0;
    changeTimerRunning = false;
    changeTimerStart = 0;
    lastFrameVoiced = false;
    noteLevelDb = PitchFrame::silenceDb;
    noteLevelSample = 0;
    provisional = false;
    fastArmed = true;
    fastBlocked = false;
    fastFrames = 0;
    fastLastPitch = 0.0;
    fastMaxRmsDb = PitchFrame::silenceDb;
}

void NoteSegmenter::setOnsetTiming (int64_t ignoreSamples, int64_t timeoutSamples) noexcept
{
    provisionalIgnoreSamples = std::max<int64_t> (0, ignoreSamples);
    provisionalTimeoutSamples = std::max<int64_t> (0, timeoutSamples);
}

void NoteSegmenter::processOnsetFrame (const PitchFrame& frame, int64_t absoluteSample, int sampleOffset,
                                       BendGenerator& bend, VsEventSink out) noexcept
{
    if (activeOutputNote >= 0)
        return;

    // After a false start the fast path waits for a real gap (level below the gate's lower edge)
    if (frame.rmsDb < gateThresholdDb - gateHysteresisDb)
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

    if (++fastFrames < fastOnsetConfirmFrames)
        return;

    const auto q = quantizer.quantize (p, -1);
    const auto outputNote = q + 12 * octaveShift;

    if (! isPlayable (outputNote))
    {
        fastFrames = 0;
        return;
    }

    const auto velocity = velocityFor (static_cast<double> (fastMaxRmsDb), static_cast<double> (gateThresholdDb),
                                       velocitySensitivityPercent);
    startNote (q, outputNote, velocity, fastMaxRmsDb, p, absoluteSample, sampleOffset, bend, out);
    provisional = true;
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
    minNoteSamples = toSamples (static_cast<double> (settings.minNoteLengthMs));
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

    if (activeOutputNote < 0)
        processIdleFrame (frame, strong, p, absoluteSample, sampleOffset, bend, out);
    else if (! provisional || ! processProvisionalFrame (frame, strong, weak, p, absoluteSample, sampleOffset, bend, out))
        processNoteFrame (frame, weak, p, absoluteSample, sampleOffset, bend, out);
}

bool NoteSegmenter::processProvisionalFrame (const PitchFrame& frame, bool strong, bool weak, double p,
                                             int64_t absoluteSample, int sampleOffset, BendGenerator& bend,
                                             VsEventSink out) noexcept
{
    const auto age = absoluteSample - noteStartSample;

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
        // Confirmation by the full window. A different note (the fast frames read another note or
        // octave) is corrected at once; otherwise the note simply continues with this frame.
        provisional = false;
        const auto candidate = quantizer.quantize (p, activeQuantisedNote);
        const auto q = static_cast<double> (activeQuantisedNote);
        const auto advantage = std::abs (p - q) - std::abs (p - static_cast<double> (candidate));

        if (candidate != activeQuantisedNote && advantage >= 2.0 * hysteresisSemitones - stickinessEpsilonSemitones
            && isPlayable (candidate + 12 * octaveShift))
        {
            unvoicedFrames = 0;
            changeNote (candidate, candidate + 12 * octaveShift, frame, p, absoluteSample, sampleOffset, bend, out);
            return true;
        }

        return false;
    }

    if (age < provisionalTimeoutSamples)
        return true; // hold: the full window is still filling

    provisional = false;

    if (weak)
        return false;

    endNote (absoluteSample, sampleOffset, bend, out); // false start
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
    startNote (q, outputNote, velocity, onsetMaxRmsDb, p, absoluteSample, sampleOffset, bend, out);
}

bool NoteSegmenter::isPitchReliable (const PitchFrame& frame, int64_t absoluteSample) noexcept
{
    const auto elapsedSeconds = static_cast<double> (absoluteSample - noteLevelSample) / sampleRate;
    const auto fallen = static_cast<double> (noteLevelDb) - levelFallDbPerSecond * std::max (0.0, elapsedSeconds);
    noteLevelDb = std::max (frame.rmsDb, static_cast<float> (std::max (fallen, static_cast<double> (PitchFrame::silenceDb))));
    noteLevelSample = absoluteSample;
    return frame.confidence >= confidenceOn && frame.rmsDb >= noteLevelDb - bendHoldDropDb;
}

void NoteSegmenter::processNoteFrame (const PitchFrame& frame, bool weak, double p, int64_t absoluteSample,
                                      int sampleOffset, BendGenerator& bend, VsEventSink out) noexcept
{
    const auto shiftedNote = activeQuantisedNote + 12 * octaveShift;

    // Octave shift changed so that the held note is no longer playable: end it at once, on any frame.
    if (! isPlayable (shiftedNote))
    {
        endNote (absoluteSample, sampleOffset, bend, out);
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
        lastFrameVoiced = false;
        changeTimerRunning = false;

        // The note is held (no bend updates) until releaseFrames unvoiced frames AND minNoteLength.
        if (++unvoicedFrames >= releaseFrames && absoluteSample - noteStartSample >= minNoteSamples)
            endNote (absoluteSample, sampleOffset, bend, out);

        return;
    }

    lastFrameVoiced = true;
    unvoicedFrames = 0;
    const auto residualCents = 100.0 * (p - static_cast<double> (activeQuantisedNote));
    const auto reliable = isPitchReliable (frame, absoluteSample);

    // Octave shift changed while the note is held: immediate change to the same q (no T_min, no
    // minNoteLength), with the usual legato/retrigger order.
    if (shiftedNote != activeOutputNote)
    {
        changeNote (activeQuantisedNote, shiftedNote, frame, p, absoluteSample, sampleOffset, bend, out);
        return;
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

    if (absoluteSample - changeTimerStart >= minChangeSamples && absoluteSample - noteStartSample >= minNoteSamples)
    {
        changeNote (candidate, candidate + 12 * octaveShift, frame, p, absoluteSample, sampleOffset, bend, out);
        return;
    }

    if (reliable)
        bend.update (residualCents, absoluteSample, sampleOffset, activeChannel, out);
}

void NoteSegmenter::startNote (int q, int outputNote, uint8_t velocity, float levelDb, double p,
                               int64_t absoluteSample, int sampleOffset, BendGenerator& bend, VsEventSink out) noexcept
{
    noteLevelDb = levelDb;
    noteLevelSample = absoluteSample;
    activeQuantisedNote = q;
    activeOutputNote = outputNote;
    activeChannel = channel;
    noteStartSample = absoluteSample;
    unvoicedFrames = 0;
    changeTimerRunning = false;
    onsetFrames = 0;
    onsetMaxRmsDb = PitchFrame::silenceDb;
    provisional = false;
    fastArmed = false;
    fastFrames = 0;

    bend.noteStarted (100.0 * (p - static_cast<double> (q)), absoluteSample, sampleOffset, activeChannel, out);
    out.push (VsEvent::makeNoteOn (sampleOffset, activeChannel, static_cast<uint8_t> (outputNote), velocity));
}

void NoteSegmenter::changeNote (int q, int outputNote, const PitchFrame& frame, double p, int64_t absoluteSample,
                                int sampleOffset, BendGenerator& bend, VsEventSink out) noexcept
{
    const auto oldNote = static_cast<uint8_t> (activeOutputNote);
    const auto oldChannel = activeChannel;
    const auto velocity = velocityFor (static_cast<double> (frame.rmsDb), static_cast<double> (gateThresholdDb),
                                       velocitySensitivityPercent);

    if (! legato)
        out.push (VsEvent::makeNoteOff (sampleOffset, oldChannel, oldNote));

    startNote (q, outputNote, velocity, frame.rmsDb, p, absoluteSample, sampleOffset, bend, out);

    if (legato)
        out.push (VsEvent::makeNoteOff (sampleOffset, oldChannel, oldNote));

    lastFrameVoiced = true;
}

void NoteSegmenter::endNote (int64_t absoluteSample, int sampleOffset, BendGenerator& bend, VsEventSink out) noexcept
{
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
        endNote (absoluteSample, sampleOffset, bend, out);

    reset();
}

} // namespace vocalsynth
