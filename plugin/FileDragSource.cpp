#include "FileDragSource.h"

#include "ui/Theme.h"

namespace vocalsynth
{

namespace
{
// Same colours as the editor (PluginEditor.cpp): the 0.4 theme tokens (references: no copy before the header's
// constants are initialised)
const juce::Colour& panelColour = ui::theme::control;
const juce::Colour& accentColour = ui::theme::accent;
const juce::Colour& textColour = ui::theme::text;
const juce::Colour& dimTextColour = ui::theme::textDim;
} // namespace

FileDragSource::FileDragSource (juce::String t, std::function<juce::File()> producer)
    : text (std::move (t)),
      produceFile (std::move (producer))
{
    setMouseCursor (juce::MouseCursor::DraggingHandCursor);
    setTooltip ("Drag into FL Studio (pattern / playlist)");
}

void FileDragSource::setAvailable (bool isNowAvailable, const juce::String& reason)
{
    if (available == isNowAvailable && unavailableText == reason)
        return;

    available = isNowAvailable;
    unavailableText = reason;

    if (available)
        unavailableText.clear();

    setMouseCursor (available ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
    repaint();
}

void FileDragSource::setIdleText (const juce::String& newText)
{
    if (idleText != newText)
    {
        idleText = newText;
        repaint();
    }
}

juce::String FileDragSource::getStatusText() const
{
    if (! available)
        return unavailableText;

    return statusText.isNotEmpty() ? statusText : idleText;
}

void FileDragSource::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat().reduced (1.0f);
    const auto alpha = available ? 1.0f : 0.45f;

    g.setColour ((isPressed && available ? panelColour.brighter (0.2f) : panelColour).withMultipliedAlpha (alpha));
    g.fillRoundedRectangle (bounds, 6.0f);

    g.setColour (accentColour.withMultipliedAlpha (alpha));
    g.drawRoundedRectangle (bounds, 6.0f, 1.5f);

    auto area = getLocalBounds().reduced (8, 2);

    g.setColour (textColour.withMultipliedAlpha (alpha));
    g.setFont (juce::FontOptions (15.0f, juce::Font::bold));
    g.drawText (text, area.removeFromTop (area.getHeight() / 2 + 4), juce::Justification::centredBottom);

    g.setColour (dimTextColour);
    g.setFont (juce::FontOptions (12.0f));
    g.drawFittedText (getStatusText(), area, juce::Justification::centredTop, 1, 0.8f);
}

void FileDragSource::mouseDown (const juce::MouseEvent&)
{
    dragStartedThisGesture = false;
    isPressed = true;
    repaint();
}

void FileDragSource::mouseDrag (const juce::MouseEvent& e)
{
    if (! available || dragStartedThisGesture || e.getDistanceFromDragStart() < dragThresholdPixels)
        return;

    // Only once per gesture: exporting does file I/O, and a second external drag must not start
    dragStartedThisGesture = true;

    const auto file = produceFile != nullptr ? produceFile() : juce::File();

    if (! file.existsAsFile())
    {
        statusText = "export failed";
        repaint();
        return;
    }

    statusText = file.getFileName();
    repaint();

    juce::Component::SafePointer<FileDragSource> safeThis (this);

    if (! juce::DragAndDropContainer::performExternalDragDropOfFiles ({ file.getFullPathName() }, false, this,
                                                                      [safeThis]
                                                                      {
                                                                          if (safeThis != nullptr)
                                                                          {
                                                                              safeThis->isPressed = false;
                                                                              safeThis->repaint();
                                                                          }
                                                                      }))
    {
        statusText = "drag failed: " + file.getFileName();
        repaint();
    }
}

void FileDragSource::mouseUp (const juce::MouseEvent&)
{
    isPressed = false;
    repaint();
}

} // namespace vocalsynth
