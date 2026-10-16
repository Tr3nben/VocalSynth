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
    if (numSamples <= 0)
        return;

    const auto s = sanitise (settings);
    const auto flushed = hasLastSettings && requiresFlush (lastSettings, s);

    if (flushed)
    {
        segmenter.flush (samplePosition, 0, bend, out);
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
            segmenter.processFrame (lastFrame, hopSample, i, bend, out);
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
            segmenter.processOnsetFrame (fastDetector.analyse (framer.getWindow(), range.f0MinHz, range.f0MaxHz, minRmsDb),
                                         hopSample, i, bend, out);
        }
    }

    samplePosition += numSamples;

    if (flushed || frameCounter != framesBefore)
        publishSnapshot();
}

void VoicePipeline::flushToSink (int sampleOffset, VsEventSink out) noexcept
{
    segmenter.flush (samplePosition + std::max (0, sampleOffset), std::max (0, sampleOffset), bend, out);
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
