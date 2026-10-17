#include "vocalsynth/VoicePipeline.h"

#include "vocalsynth/Pitch.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace vocalsynth
{

namespace
{
/** Delay in ms common to both onset paths: decimator FIR group delay + decimation phase + conditioner
    group delay at f0 min. */
double frontEndDelayMs (double sampleRate, double f0MinHz) noexcept
{
    const auto factor = Decimator::factorFor (sampleRate);
    const auto firDelay = static_cast<double> ((Decimator::numTapsFor (factor) - 1) / 2);
    const auto phaseDelay = static_cast<double> (factor - 1);
    return 1000.0 * (firDelay + phaseDelay) / sampleRate + 1000.0 * InputConditioner::groupDelaySeconds (sampleRate, f0MinHz);
}
} // namespace

int VoicePipeline::fastHopSamplesFor (double analysisSampleRate) noexcept
{
    return std::max (1, Framer::hopSamplesFor (analysisSampleRate) / fastHopDivisor);
}

double VoicePipeline::computeConfirmedLatencyMs (double sampleRate, VoiceRange range) noexcept
{
    if (! (sampleRate > 0.0))
        return 0.0;

    const auto analysisRate = sampleRate / static_cast<double> (Decimator::factorFor (sampleRate));
    const auto f0Min = voiceRangeInfo (range).f0MinHz;
    const auto window = static_cast<double> (Framer::windowSamplesFor (analysisRate, f0Min));
    const auto hops = static_cast<double> (latencyBudgetHops * Framer::hopSamplesFor (analysisRate));

    return frontEndDelayMs (sampleRate, f0Min) + 1000.0 * (window + hops) / analysisRate;
}

double VoicePipeline::computeFastOnsetLatencyMs (double sampleRate, VoiceRange range, double f0Hz) noexcept
{
    if (! (sampleRate > 0.0))
        return 0.0;

    const auto analysisRate = sampleRate / static_cast<double> (Decimator::factorFor (sampleRate));
    const auto& info = voiceRangeInfo (range);
    const auto f = f0Hz > info.f0MinHz ? f0Hz : info.f0MinHz; // NaN -> f0 min
    const auto period = static_cast<int> (std::ceil (analysisRate / f)) + 1;
    const auto required = static_cast<double> (FastOnsetDetector::requiredSamplesFor (analysisRate, info.f0MinHz, info.f0MaxHz, period));
    const auto hops = static_cast<double> (fastLatencyBudgetHops * fastHopSamplesFor (analysisRate));

    return frontEndDelayMs (sampleRate, f) + 1000.0 * (required + hops) / analysisRate;
}

double VoicePipeline::computeAlgorithmicLatencyMs (double sampleRate, VoiceRange range) noexcept
{
    return std::min (computeFastOnsetLatencyMs (sampleRate, range), computeConfirmedLatencyMs (sampleRate, range));
}

VoicePipeline::VoicePipeline()
{
    // Not real-time (allocates); gives a usable object before the first prepare()
    prepare (48000.0, 512);
}

void VoicePipeline::prepare (double newSampleRate, int /*maxBlockSize*/)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;

    conditioner.prepare (sampleRate);
    decimator.prepare (sampleRate);

    const auto analysisRate = decimator.getOutputSampleRate();
    const auto maxWindow = Framer::windowSamplesFor (analysisRate, lowestF0MinHz);
    framer.prepare (analysisRate, maxWindow);
    yin.prepare (analysisRate, maxWindow);
    mpm.prepare (analysisRate, maxWindow);
    fastDetector.prepare (analysisRate, maxWindow);
    fastHopSamples = fastHopSamplesFor (analysisRate);

    const auto hopSeconds = static_cast<double> (framer.getHopSamples()) / analysisRate;
    postProcessor.prepare (1.0 / hopSeconds);
    segmenter.prepare (sampleRate);
    bend.prepare (sampleRate, hopSeconds);
    levelCoefficient = 1.0 - std::exp (-1000.0 / (onsetEnvelopeTimeConstantMs * sampleRate));

    frameCounter = 0;
    reset();
}

void VoicePipeline::reset() noexcept
{
    conditioner.reset();
    decimator.reset();
    framer.reset();
    postProcessor.reset();
    segmenter.reset();
    bend.reset();

    lastSettings = {};
    hasLastSettings = false;
    samplePosition = 0;
    lastFrame = {};
    levelEnvelope = 0.0;
    levelAbove = false;
    levelLastRise = 0.0;
    levelLastRiseSample = 0;
    applySettings (sanitise (VoiceSettings {}));
    publishSnapshot();
}

void VoicePipeline::applySettings (const VoiceSettings& s) noexcept
{
    const auto window = Framer::windowSamplesFor (decimator.getOutputSampleRate(), voiceRangeInfo (s.range).f0MinHz);
    const auto factor = static_cast<int64_t> (decimator.getFactor());
    const auto hop = static_cast<int64_t> (framer.getHopSamples());
    framer.setWindowSamples (window);
    segmenter.setSettings (s);
    segmenter.setOnsetTiming (hop * factor, (static_cast<int64_t> (window) + provisionalTimeoutHops * hop) * factor,
                              static_cast<int64_t> (window) * factor);
    bend.setMode (s.bendMode, s.bendRange);

    // Detection-delay model of the timing side output (never changes the events)
    const auto msToSamples = [this] (double ms) noexcept { return static_cast<int64_t> (std::llround (ms * sampleRate / 1000.0)); };
    const auto hopInput = hop * factor;
    timingRange = s.range;
    NoteSegmenter::DetectionDelayModel model;
    model.frameDelaySamples = static_cast<double> (decimator.getGroupDelayInputSamples())
                              + static_cast<double> (factor) * static_cast<double> (window - 1) / 2.0
                              + static_cast<double> ((PitchPostProcessor::defaultMedianLength / 2) * hopInput);
    model.confirmedOnsetDelaySamples = msToSamples (computeConfirmedLatencyMs (sampleRate, s.range));
    model.fastOnsetDelay = &VoicePipeline::fastOnsetDelayFor;
    model.fastOnsetDelayContext = this;
    model.onsetSearchSamples = model.confirmedOnsetDelaySamples + onsetSearchExtraHops * hopInput;
    segmenter.setDetectionDelayModel (model);

    levelOnPower = std::pow (10.0, static_cast<double> (s.gateThresholdDb - NoteSegmenter::gateHysteresisDb) / 10.0);
    levelOffPower = levelOnPower * std::pow (10.0, -levelReleaseHysteresisDb / 10.0);
}

int64_t VoicePipeline::fastOnsetDelayFor (const void* pipeline, double f0Hz) noexcept
{
    const auto& self = *static_cast<const VoicePipeline*> (pipeline);
    const auto ms = computeFastOnsetLatencyMs (self.sampleRate, self.timingRange, f0Hz);
    return static_cast<int64_t> (std::llround (ms * self.sampleRate / 1000.0));
}

void VoicePipeline::updateLevelEnvelope (float conditionedSample, int64_t absoluteSample) noexcept
{
    const auto power = static_cast<double> (conditionedSample) * static_cast<double> (conditionedSample);
    const auto previous = levelEnvelope;
    levelEnvelope += levelCoefficient * (power - levelEnvelope);

    if (levelEnvelope < 1.0e-30) // long silences without FTZ stay fast
        levelEnvelope = 0.0;

    if (levelEnvelope > previous)
    {
        levelLastRise = levelEnvelope;
        levelLastRiseSample = absoluteSample;
    }

    if (! levelAbove)
    {
        if (levelEnvelope >= levelOnPower)
        {
            levelAbove = true;
            segmenter.setLevelOnsetSample (absoluteSample);
        }

        return;
    }

    if (levelEnvelope < levelOffPower)
    {
        levelAbove = false;

        // Exponential decay since the newest rise: that is where the sound stopped (abrupt end) or a
        // little before the crossing (slow fade, the envelope follows the input)
        const auto tauSamples = onsetEnvelopeTimeConstantMs * sampleRate / 1000.0;
        const auto decay = levelLastRise > levelOffPower ? tauSamples * std::log (levelLastRise / levelOffPower) : 0.0;
        const auto estimate = absoluteSample - static_cast<int64_t> (std::llround (std::min (decay, 1.0e9)));
        segmenter.setLevelReleaseSample (std::max (levelLastRiseSample, estimate));
    }
}

void VoicePipeline::forwardWithTimings (VsEventSink out, VsEventTimingSink timing) noexcept
{
    const auto& t = segmenter.getLastEventTiming();
    const auto n = segmenterScratch.size();

    for (std::size_t i = 0; i < n; ++i)
    {
        const auto& e = segmenterScratch[i];

        if (! out.push (e))
            continue;

        VsEventTiming chosen;

        switch (e.type)
        {
            case VsEventType::noteOn: chosen = t.noteOn; break;
            case VsEventType::noteOff: chosen = t.noteOff; break;
            case VsEventType::pitchBend:
                if (i + 1 < n && segmenterScratch[i + 1].type == VsEventType::noteOn)
                    chosen = t.noteOn;   // note start bend
                else if (i > 0 && segmenterScratch[i - 1].type == VsEventType::noteOff)
                    chosen = t.noteOff;  // centre after a note-off
                else
                    chosen = t.update;
                break;
            case VsEventType::controlChange: break;
        }

        timing.push (chosen);
    }
}

PitchDetector& VoicePipeline::currentDetector() noexcept
{
    if (lastSettings.algorithm == PitchAlgorithm::mpm)
        return mpm;

    return yin;
}

void VoicePipeline::processToSink (const float* mono, int numSamples, const VoiceSettings& settings,
                                   VsEventSink out) noexcept
{
    processToSink (mono, numSamples, settings, out, VsEventTimingSink {});
}

void VoicePipeline::processToSink (const float* mono, int numSamples, const VoiceSettings& settings, VsEventSink out,
                                   VsEventTimingSink timing) noexcept
{
    if (numSamples <= 0)
        return;

    const auto s = sanitise (settings);
    const auto flushed = hasLastSettings && requiresFlush (lastSettings, s);

    if (flushed)
    {
        runSegmenter (out, timing, [&] (VsEventSink sink) noexcept { segmenter.flush (samplePosition, 0, bend, sink); });
        postProcessor.reset();
    }

    // Applied only on a change (cheap compare): with 1-sample host blocks this runs per sample
    if (! hasLastSettings || ! (s == lastSettings))
        applySettings (s);

    lastSettings = s;
    hasLastSettings = true;
    const auto framesBefore = frameCounter;

    const auto& range = voiceRangeInfo (s.range);
    auto& detector = currentDetector();

    for (int i = 0; i < numSamples; ++i)
    {
        const auto x = conditioner.processSample (mono != nullptr ? mono[i] : 0.0f);
        updateLevelEnvelope (x, samplePosition + i);
        float decimated = 0.0f;

        if (! decimator.pushSample (x, decimated))
            continue;

        const auto hopCompleted = framer.pushSample (decimated);
        const auto fastDue = framer.getNumSamplesPushed() % fastHopSamples == 0;

        if (! hopCompleted && ! fastDue)
            continue;

        const auto hopSample = samplePosition + i;

        if (hopCompleted)
        {
            const auto raw = detector.analyse (framer.getWindow(), range.f0MinHz, range.f0MaxHz);
            lastFrame = postProcessor.process (raw);
            const auto noteWasActive = segmenter.getActiveNote() >= 0;
            runSegmenter (out, timing, [&] (VsEventSink sink) noexcept
                          { segmenter.processFrame (lastFrame, hopSample, i, bend, sink); });
            ++frameCounter;

            // A released note ends the octave context: the next onset may start in any octave
            if (noteWasActive && segmenter.getActiveNote() < 0)
                postProcessor.forgetOctaveReference();

            if (frameObserver != nullptr)
                frameObserver (frameObserverContext,
                               { hopSample, raw, lastFrame, segmenter.getActiveNote(), segmenter.isFrameVoiced() });
        }

        // Provisional onsets: only while no note is on (after the full frame of this sample, if any).
        // Below the gate a frame cannot start a note, and while the fast path is blocked the segmenter
        // reads only the level: no search then (same result for the segmenter).
        if (fastDue && segmenter.getActiveNote() < 0)
        {
            const auto minRmsDb = segmenter.isFastPathBlocked() ? std::numeric_limits<float>::infinity()
                                                                : s.gateThresholdDb;
            const auto fastFrame = fastDetector.analyse (framer.getWindow(), range.f0MinHz, range.f0MaxHz, minRmsDb);
            runSegmenter (out, timing, [&] (VsEventSink sink) noexcept
                          { segmenter.processOnsetFrame (fastFrame, hopSample, i, bend, sink); });
        }
    }

    samplePosition += numSamples;

    if (flushed || frameCounter != framesBefore)
        publishSnapshot();
}

void VoicePipeline::flushToSink (int sampleOffset, VsEventSink out) noexcept
{
    flushToSink (sampleOffset, out, VsEventTimingSink {});
}

void VoicePipeline::flushToSink (int sampleOffset, VsEventSink out, VsEventTimingSink timing) noexcept
{
    const auto offset = std::max (0, sampleOffset);
    runSegmenter (out, timing, [&] (VsEventSink sink) noexcept { segmenter.flush (samplePosition + offset, offset, bend, sink); });
    postProcessor.forgetOctaveReference();
    publishSnapshot();
}

double VoicePipeline::getAlgorithmicLatencyMs() const noexcept
{
    return computeAlgorithmicLatencyMs (sampleRate, lastSettings.range);
}

double VoicePipeline::getFrameCentreDelayInputSamples() const noexcept
{
    const auto factor = static_cast<double> (decimator.getFactor());
    return static_cast<double> (decimator.getGroupDelayInputSamples())
           + factor * static_cast<double> (framer.getWindowSamples() - 1) / 2.0;
}

int VoicePipeline::maxEventsForSamples (int numSamples) const noexcept
{
    if (numSamples <= 0)
        return 0;

    const auto hop = std::max (1, getHopInputSamples());
    return maxEventsPerFlush + maxEventsPerHop * ((numSamples + hop - 1) / hop) + NoteSegmenter::maxEventsPerOnsetFrame;
}

void VoicePipeline::setFrameObserver (FrameObserver observer, void* context) noexcept
{
    frameObserver = observer;
    frameObserverContext = context;
}

void VoicePipeline::publishSnapshot() noexcept
{
    VoiceAnalysisSnapshot s;
    s.frequencyHz = lastFrame.frequencyHz;
    s.confidence = lastFrame.confidence;
    s.rmsDb = lastFrame.rmsDb;
    s.voiced = segmenter.isFrameVoiced();
    s.activeNote = segmenter.getActiveNote();
    s.quantisedNote = segmenter.getQuantisedNote();
    s.latencyMs = static_cast<float> (getAlgorithmicLatencyMs());
    s.frameCounter = frameCounter;

    if (lastFrame.frequencyHz > 0.0f)
    {
        s.nearestNote = nearestMidiNote (static_cast<double> (lastFrame.frequencyHz));
        s.centsOffset = static_cast<float> (centsFromNote (static_cast<double> (lastFrame.frequencyHz), s.nearestNote));
    }

    snapshot.publish (s);
}

} // namespace vocalsynth
