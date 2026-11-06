#include "ui/Knob.h"

#include "ui/Theme.h"
#include "ui/VsLookAndFeel.h"

#include <cmath>

namespace vocalsynth::ui
{

//==============================================================================
Knob::DragSlider::DragSlider()
{
    // Always absolute drag (ctrl / cmd would otherwise swap to JUCE's velocity mode); fine drag is ours
    setVelocityModeParameters (1.0, 1, 0.0, false);
    setRepaintsOnMouseActivity (true);
    setScrollWheelEnabled (true);
}

bool Knob::DragSlider::isFine (const juce::ModifierKeys& mods) noexcept
{
    return mods.isShiftDown() || mods.isCommandDown() || mods.isCtrlDown();
}

void Knob::DragSlider::mouseDown (const juce::MouseEvent& e)
{
    lastPosition = virtualPosition = e.position;
    Slider::mouseDown (e);
}

void Knob::DragSlider::mouseDrag (const juce::MouseEvent& e)
{
    // A virtual mouse position that moves at 1/10 speed while a fine modifier is held: JUCE measures the
    // drag from the mouse-down position, so changing the modifier mid-drag continues smoothly.
    const auto delta = e.position - lastPosition;
    lastPosition = e.position;
    virtualPosition += delta * static_cast<float> (isFine (e.mods) ? fineFactor : 1.0);
    Slider::mouseDrag (e.withNewPosition (virtualPosition));
}

void Knob::DragSlider::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (! isFine (e.mods))
    {
        Slider::mouseWheelMove (e, wheel);
        return;
    }

    auto fine = wheel;
    fine.deltaX *= static_cast<float> (fineFactor);
    fine.deltaY *= static_cast<float> (fineFactor);
    Slider::mouseWheelMove (e.withNewPosition (e.position), fine);
}

//==============================================================================
Knob::Knob (juce::AudioProcessorValueTreeState& apvts, const juce::String& id, const juce::String& text)
    : Knob (apvts, id, text, Style::rotary)
{
}

Knob::Knob (juce::AudioProcessorValueTreeState& apvts, const juce::String& id, const juce::String& text, Style s)
    : parameterId (id),
      labelText (text),
      style (s),
      parameter (apvts.getParameter (id))
{
    setComponentID (id);
    slider.setComponentID (id);
    slider.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);

    if (style == Style::rotary)
    {
        slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setRotaryParameters (VsLookAndFeel::rotaryStart, VsLookAndFeel::rotaryEnd, true);
        slider.setMouseDragSensitivity (200);
    }
    else
    {
        slider.setSliderStyle (juce::Slider::LinearBar);
        slider.setSliderSnapsToMousePosition (false);
        slider.setMouseDragSensitivity (220);
        slider.getProperties().set ("vsBar", true);
    }

    label.setText (text, juce::dontSendNotification);
    label.setJustificationType (juce::Justification::centred);
    label.setFont (theme::font (theme::fontSmall));
    label.setColour (juce::Label::textColourId, theme::textDim);
    label.setInterceptsMouseClicks (false, false);
    label.setMinimumHorizontalScale (0.7f);
    label.setBorderSize ({});

    valueLabel.setJustificationType (juce::Justification::centred);
    valueLabel.setFont (theme::font (theme::fontSmall));
    valueLabel.setColour (juce::Label::textColourId, theme::text);
    valueLabel.setMinimumHorizontalScale (0.7f);
    valueLabel.setBorderSize ({});
    valueLabel.setEditable (false, true, false); // double-click: type a value
    valueLabel.onTextChange = [this]
    {
        const auto typed = valueLabel.getText().trim();

        if (typed.isNotEmpty())
            slider.setValue (slider.getValueFromText (typed), juce::sendNotificationSync);

        updateValueText();
    };

    if (style == Style::rotary)
    {
        addAndMakeVisible (label);
        addAndMakeVisible (valueLabel);
    }

    addAndMakeVisible (slider);

    if (parameter != nullptr)
    {
        const auto range = parameter->getNormalisableRange();
        bipolar = range.start < 0.0f && range.end > 0.0f
                  && std::abs (range.start + range.end) <= 0.001f * (range.end - range.start);
        slider.getProperties().set ("bipolar", bipolar);

        attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (apvts, id, slider);

        // Unit and sign: "-6.0 dB", "+12 st", "50 %" (the parameter texts carry no unit, the label does)
        const auto unit = parameter->getLabel();
        slider.setTextValueSuffix (unit.isNotEmpty() ? " " + unit : juce::String());

        if (bipolar)
        {
            auto plainText = slider.textFromValueFunction;
            slider.textFromValueFunction = [plainText] (double v)
            {
                auto t = plainText != nullptr ? plainText (v) : juce::String (v);
                return (v > 0.0 && ! t.startsWithChar ('+')) ? "+" + t : t;
            };
        }

        const auto name = parameter->getName (64);
        slider.setTooltip (name);
        valueLabel.setTooltip (name);
    }

    slider.onValueChange = [this] { updateValueText(); };
    updateValueText();
}

Knob::~Knob() = default;

void Knob::setModulationRange (juce::Range<double> normalisedRange)
{
    if (normalisedRange == modulationRange)
        return;

    modulationRange = normalisedRange;
    slider.getProperties().set ("modStart", normalisedRange.getStart());
    slider.getProperties().set ("modEnd", normalisedRange.getEnd());
    slider.repaint();
}

void Knob::setTooltip (const juce::String& text)
{
    slider.setTooltip (text);
    valueLabel.setTooltip (text);
}

juce::String Knob::getValueText() const
{
    return const_cast<DragSlider&> (slider).getTextFromValue (slider.getValue());
}

void Knob::updateValueText()
{
    const auto text = getValueText();

    if (style == Style::rotary)
        valueLabel.setText (text, juce::dontSendNotification);
    else
        repaint();
}

void Knob::resized()
{
    auto bounds = getLocalBounds();

    if (style == Style::bar)
    {
        slider.setBounds (bounds);
        return;
    }

    // label (top) / rotary (square, centred) / value (bottom)
    label.setBounds (bounds.removeFromTop (theme::knobLabelHeight - 1));
    valueLabel.setBounds (bounds.removeFromBottom (theme::knobValueHeight - 1));
    const auto side = juce::jmin (bounds.getWidth(), bounds.getHeight());
    slider.setBounds (bounds.withSizeKeepingCentre (side, side));
}

void Knob::parentHierarchyChanged()
{
    // Value bubble while dragging, shown in the top-level component (the editor): never a desktop window
    auto* top = getTopLevelComponent();
    slider.setPopupDisplayEnabled (top != this, false, top != this ? top : nullptr);
}

void Knob::paintOverChildren (juce::Graphics& g)
{
    if (style != Style::bar)
        return;

    auto area = getLocalBounds().reduced (7, 0);
    g.setFont (theme::font (theme::fontSmall));
    g.setColour (isEnabled() ? theme::textDim : theme::textDim.withAlpha (0.5f));

    if (labelText.isNotEmpty())
        g.drawFittedText (labelText, area, juce::Justification::centredLeft, 1, 0.8f);

    g.setColour (isEnabled() ? theme::text : theme::text.withAlpha (0.5f));
    g.drawFittedText (getValueText(), area, labelText.isNotEmpty() ? juce::Justification::centredRight
                                                                  : juce::Justification::centred,
                      1, 0.8f);
}

} // namespace vocalsynth::ui
