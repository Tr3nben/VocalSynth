#include "FileDragSource.h"

namespace vocalsynth
{

// TODO(C): move the M1 implementation here (VocalSynthEditor::FileDragSource in PluginEditor.cpp).

FileDragSource::FileDragSource (juce::String t, std::function<juce::File()> producer)
    : text (std::move (t)),
      produceFile (std::move (producer))
{
}

void FileDragSource::setAvailable (bool isNowAvailable, const juce::String& reason)
{
    available = isNowAvailable;
    statusText = reason;
    repaint();
}

void FileDragSource::paint (juce::Graphics&)
{
}

void FileDragSource::mouseDown (const juce::MouseEvent&)
{
    dragStartedThisGesture = false;
    isPressed = true;
}

void FileDragSource::mouseDrag (const juce::MouseEvent&)
{
}

void FileDragSource::mouseUp (const juce::MouseEvent&)
{
    isPressed = false;
}

} // namespace vocalsynth
