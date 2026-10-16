#include "CapturePanel.h"

#include "PluginProcessor.h"

namespace vocalsynth
{

// TODO(C): implement (docs/M3-DESIGN.md 14). Stub: an empty component, the timer is not started.

CapturePanel::CapturePanel (VocalSynthProcessor& p)
    : vsProcessor (p)
{
}

CapturePanel::~CapturePanel()
{
    stopTimer();
}

void CapturePanel::paint (juce::Graphics&)
{
}

void CapturePanel::resized()
{
}

juce::String CapturePanel::getStatusText() const
{
    return "Capture off";
}

void CapturePanel::timerCallback()
{
    (void) vsProcessor;
    (void) shownVersion;
}

} // namespace vocalsynth
