#include "vocalsynth/VoicePipeline.h"

#include "vocalsynth/Pitch.h"

#include <algorithm>
#include <cmath>

namespace vocalsynth
{

double VoicePipeline::computeAlgorithmicLatencyMs (double sampleRate, VoiceRange range) noexcept
{
    if (! (sampleRate > 0.0))
        return 0.0;

    const auto factor = Decimator::factorFor (sampleRate);
    const auto analysisRate = sampleRate / static_cast<double> (factor);
    const auto firDelay = static_cast<double> ((Decimator::numTapsFor (factor) - 1) / 2);
    const auto phaseDelay = static_cast<double> (factor - 1);
    const auto f0Min = voiceRangeInfo (range).f0MinHz;
    const auto window = static_cast<double> (Framer::windowSamplesFor (analysisRate, f0Min));
    const auto hops = static_cast<double> (latencyBudgetHops * Framer::hopSamplesFor (analysisRate));

    return 1000.0 * (firDelay + phaseDelay) / sampleRate
           + 1000.0 * InputConditioner::groupDelaySeconds (sampleRate, f0Min)
           + 1000.0 * (window + hops) / analysisRate;
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
    framer.setWindowSamples (Framer::windowSamplesFor (decimator.getOutputSampleRate(), voiceRangeInfo (s.range).f0MinHz));
    segmenter.setSettings (s);
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

    lastSettings = s;
    hasLastSettings = true;
    applySettings (s);
    const auto framesBefore = frameCounter;

    const auto& range = voiceRangeInfo (s.range);
    auto& detector = currentDetector();

    for (int i = 0; i < numSamples; ++i)
    {
        const auto x = conditioner.processSample (mono != nullptr ? mono[i] : 0.0f);
        float decimated = 0.0f;

        if (! decimator.pushSample (x, decimated) || ! framer.pushSample (decimated))
            continue;

        const auto hopSample = samplePosition + i;
        const auto raw = detector.analyse (framer.getWindow(), range.f0MinHz, range.f0MaxHz);
        lastFrame = postProcessor.process (raw);
        segmenter.processFrame (lastFrame, hopSample, i, bend, out);
        ++frameCounter;

        if (frameObserver != nullptr)
            frameObserver (frameObserverContext,
                           { hopSample, raw, lastFrame, segmenter.getActiveNote(), segmenter.isFrameVoiced() });
    }

    samplePosition += numSamples;

    if (flushed || frameCounter != framesBefore)
        publishSnapshot();
}

void VoicePipeline::flushToSink (int sampleOffset, VsEventSink out) noexcept
{
    segmenter.flush (samplePosition + std::max (0, sampleOffset), std::max (0, sampleOffset), bend, out);
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
    return maxEventsPerFlush + maxEventsPerHop * ((numSamples + hop - 1) / hop);
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
