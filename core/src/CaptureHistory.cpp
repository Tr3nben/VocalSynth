#include "vocalsynth/CaptureHistory.h"

#include <algorithm>

namespace vocalsynth
{

// TODO(A): implement (docs/M3-DESIGN.md 5.1). Stubs: stores nothing.

void CaptureHistory::prepare (double audioHistoryMinutes)
{
    setAudioHistoryMinutes (audioHistoryMinutes);
    clear();
}

void CaptureHistory::setAudioHistoryMinutes (double minutes)
{
    audioMinutes = std::clamp (minutes, minAudioMinutes, maxAudioMinutes);
}

void CaptureHistory::clear()
{
    ringStart = 0;
    ringCount = 0;
    settingsBeforeRing = {};
    timeline.reset (sampleRate);
    audioRange = {};
    missingAudioSamples = 0;
    endSample = 0;
}

void CaptureHistory::addRecord (const CaptureRecord&)
{
}

void CaptureHistory::addAudio (int64_t, std::span<const float>)
{
}

void CaptureHistory::setAudioOff (int64_t)
{
}

CaptureHistory::AudioRange CaptureHistory::getAudioRange() const noexcept
{
    return audioRange;
}

std::size_t CaptureHistory::readAudio (int64_t, std::span<float> destination) const noexcept
{
    std::fill (destination.begin(), destination.end(), 0.0f);
    return 0;
}

int64_t CaptureHistory::getOldestEventSample() const noexcept
{
    return endSample;
}

int64_t CaptureHistory::getEndSample() const noexcept
{
    return endSample;
}

std::vector<CaptureRecord> CaptureHistory::copyRecords (int64_t, int64_t) const
{
    return {};
}

CaptureSettingsInfo CaptureHistory::getSettingsAt (int64_t) const noexcept
{
    return settingsBeforeRing;
}

std::size_t CaptureHistory::getAudioCapacitySamples() const noexcept
{
    return 0;
}

std::size_t CaptureHistory::getAllocatedAudioBytes() const noexcept
{
    return 0;
}

} // namespace vocalsynth
