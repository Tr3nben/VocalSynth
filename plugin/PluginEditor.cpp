#include "PluginEditor.h"

#include "Parameters.h"

namespace vocalsynth
{

namespace
{
constexpr int meterRefreshHz = 30;
constexpr float meterDecayPerTick = 0.85f;
constexpr float meterFloorDb = -60.0f;
} // namespace

VocalSynthEditor::VocalSynthEditor (VocalSynthProcessor& p)
    : AudioProcessorEditor (p),
      vsProcessor (p),
      muteDryAttachment (p.getParameters(), params::muteDry.getParamID(), muteDryButton)
{
    // The instrument never outputs the dry voice, so the switch only matters for the FX
    muteDryButton.setVisible (p.getVariant() == Variant::fx);
    addAndMakeVisible (muteDryButton);

    setSize (420, 220);
    startTimerHz (meterRefreshHz);
}

VocalSynthEditor::~VocalSynthEditor()
{
    stopTimer();
}

void VocalSynthEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff15171c));

    auto area = getLocalBounds().reduced (20);

    g.setColour (juce::Colours::white);
    g.setFont (juce::FontOptions (24.0f, juce::Font::bold));
    g.drawText (vsProcessor.getName(), area.removeFromTop (32), juce::Justification::centredLeft);

    g.setColour (juce::Colours::grey);
    g.setFont (juce::FontOptions (14.0f));
    g.drawText ("Version " VOCALSYNTH_VERSION_STRING " - skeleton build (M0), no analysis yet",
                area.removeFromTop (22),
                juce::Justification::centredLeft);

    // Input level meter (dBFS, -60 .. 0)
    const auto label = vsProcessor.getVariant() == Variant::instrument ? "Sidechain input" : "Input";
    g.setColour (juce::Colours::lightgrey);
    g.drawText (label, meterBounds.withY (meterBounds.getY() - 20).withHeight (18),
                juce::Justification::centredLeft);

    g.setColour (juce::Colour (0xff2a2e36));
    g.fillRoundedRectangle (meterBounds.toFloat(), 4.0f);

    const auto db = juce::Decibels::gainToDecibels (displayedLevel, meterFloorDb);
    const auto proportion = juce::jmap (db, meterFloorDb, 0.0f, 0.0f, 1.0f);
    g.setColour (db > -3.0f ? juce::Colours::orangered : juce::Colour (0xff3ddc97));
    g.fillRoundedRectangle (meterBounds.toFloat().withWidth ((float) meterBounds.getWidth() * proportion),
                            4.0f);
}

void VocalSynthEditor::resized()
{
    auto area = getLocalBounds().reduced (20);
    area.removeFromTop (32 + 22 + 30);
    meterBounds = area.removeFromTop (16);
    area.removeFromTop (20);
    muteDryButton.setBounds (area.removeFromTop (28).withWidth (140));
}

void VocalSynthEditor::timerCallback()
{
    const auto peak = vsProcessor.takeInputPeak();
    displayedLevel = juce::jmax (peak, displayedLevel * meterDecayPerTick);
    repaint (meterBounds);
}

} // namespace vocalsynth
