#include "vocalsynth/MonitorSynth.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace vocalsynth
{

void MonitorSynth::prepare (double newSampleRate) noexcept
{
    sampleRate = (newSampleRate > 0.0 && std::isfinite (newSampleRate)) ? newSampleRate : 48000.0;

    attackStep = 1.0 / (attackSeconds * sampleRate);
    releaseStep = 1.0 / (releaseSeconds * sampleRate);
    gainStep = 1.0 / (attackSeconds * sampleRate);

    reset();
}

void MonitorSynth::reset() noexcept
{
    phase = 0.0;
    envelope = 0.0;
    gateOn = false;
    currentNote = -1;
    bendSemitones = 0.0;
    outputGain = outputEnabled ? 1.0 : 0.0;
    phaseIncrement = 0.0;
}

void MonitorSynth::releaseAll() noexcept
{
    gateOn = false;
    bendSemitones = 0.0;
    updatePhaseIncrement();

    // Nothing sounds: like reset(), jump the output gain to the switch state, so a note-on right
    // after this does not start with a gain ramp from a stale state
    if (envelope <= 0.0)
        outputGain = outputEnabled ? 1.0 : 0.0;
}

void MonitorSynth::updatePhaseIncrement() noexcept
{
    if (currentNote < 0)
    {
        phaseIncrement = 0.0;
        return;
    }

    const auto semitonesFromA4 = static_cast<double> (currentNote) - 69.0 + bendSemitones;
    const auto frequency = 440.0 * std::exp2 (semitonesFromA4 / 12.0);

    // Keep below Nyquist (only relevant for very high notes at very low sample rates).
    phaseIncrement = std::min (frequency / sampleRate, 0.49);
}

void MonitorSynth::applyEvent (const VsEvent& e) noexcept
{
    switch (e.type)
    {
        case VsEventType::noteOn:
            if (e.data2 == 0) // MIDI convention: note-on with velocity 0 is a note-off
            {
                if (gateOn && static_cast<int> (e.data1) == currentNote)
                    gateOn = false;
                break;
            }

            // Starting from silence: begin at phase 0 (sin(0) = 0, no click). A note-on while the
            // voice still sounds only retunes it; the envelope continues from its current level.
            if (envelope <= 0.0)
                phase = 0.0;

            currentNote = std::clamp (static_cast<int> (e.data1), 0, 127);
            gateOn = true;
            updatePhaseIncrement();
            break;

        case VsEventType::noteOff:
            // Only the sounding note can be released (a stale note-off must not cut a newer note).
            if (gateOn && static_cast<int> (e.data1) == currentNote)
                gateOn = false;
            break;

        case VsEventType::pitchBend:
        {
            const auto bend = std::clamp (static_cast<int> (e.bend), -8192, 8191);
            bendSemitones = bendRangeSemitones * static_cast<double> (bend) / 8192.0;
            updatePhaseIncrement();
            break;
        }

        case VsEventType::controlChange:
        default:
            break;
    }
}

void MonitorSynth::renderSegment (float* const* channels, int numChannels, int start, int end) noexcept
{
    const auto gainTarget = outputEnabled ? 1.0 : 0.0;
    const bool canWrite = channels != nullptr && numChannels > 0;

    for (int i = start; i < end; ++i)
    {
        // Envelope (linear; snaps exactly to 0/1, so no denormals).
        if (gateOn)
            envelope = std::min (1.0, envelope + attackStep);
        else if (envelope > 0.0)
            envelope = std::max (0.0, envelope - releaseStep);

        const bool voiceSilent = envelope <= 0.0;

        // Output gain ramp. While the voice is silent the ramp is inaudible, so it jumps to the
        // target; this keeps the result identical for any block split.
        if (voiceSilent)
            outputGain = gainTarget;
        else if (outputGain < gainTarget)
            outputGain = std::min (gainTarget, outputGain + gainStep);
        else if (outputGain > gainTarget)
            outputGain = std::max (gainTarget, outputGain - gainStep);

        if (voiceSilent)
        {
            phase = 0.0;
            continue;
        }

        const auto amplitude = envelope * outputGain * static_cast<double> (peakGain);

        if (amplitude > 0.0 && canWrite)
        {
            const auto sample = static_cast<float> (amplitude * std::sin (2.0 * std::numbers::pi * phase));

            for (int ch = 0; ch < numChannels; ++ch)
                if (auto* data = channels[ch])
                    data[i] += sample;
        }

        phase += phaseIncrement;

        if (phase >= 1.0)
            phase -= 1.0;
    }
}

void MonitorSynth::processEvents (std::span<const VsEvent> events, float* const* channels, int numChannels,
                                  int numSamples) noexcept
{
    if (numSamples <= 0)
    {
        // Nothing to render, but the events still change the state (no sample time passes).
        for (const auto& e : events)
            applyEvent (e);
        return;
    }

    // Fast path: idle voice, no events -> exactly nothing to add.
    if (events.empty() && ! isActive())
    {
        outputGain = outputEnabled ? 1.0 : 0.0;
        phase = 0.0;
        return;
    }

    int position = 0;

    for (const auto& e : events)
    {
        const auto offset = std::clamp (e.sampleOffset, 0, numSamples - 1);

        if (offset > position)
        {
            renderSegment (channels, numChannels, position, offset);
            position = offset;
        }

        applyEvent (e);
    }

    renderSegment (channels, numChannels, position, numSamples);
}

} // namespace vocalsynth
