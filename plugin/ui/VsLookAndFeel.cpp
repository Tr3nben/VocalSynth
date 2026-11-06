#include "ui/VsLookAndFeel.h"

#include <cmath>

namespace vocalsynth::ui
{

namespace
{
bool flag (const juce::Component& c, const char* name)
{
    return static_cast<bool> (c.getProperties().getWithDefault (name, false));
}

/** Normalised modulation range of a slider ("modStart" / "modEnd"), empty if none. */
juce::Range<float> modulationRange (const juce::Slider& slider)
{
    const auto& props = slider.getProperties();

    if (! props.contains ("modStart") || ! props.contains ("modEnd"))
        return {};

    const auto a = juce::jlimit (0.0f, 1.0f, static_cast<float> (static_cast<double> (props["modStart"])));
    const auto b = juce::jlimit (0.0f, 1.0f, static_cast<float> (static_cast<double> (props["modEnd"])));
    return { juce::jmin (a, b), juce::jmax (a, b) };
}

juce::Path chevron (juce::Rectangle<float> area)
{
    juce::Path p;
    const auto c = area.getCentre();
    const auto w = juce::jmin (area.getWidth(), area.getHeight()) * 0.5f;
    p.startNewSubPath (c.x - w * 0.5f, c.y - w * 0.22f);
    p.lineTo (c.x, c.y + w * 0.28f);
    p.lineTo (c.x + w * 0.5f, c.y - w * 0.22f);
    return p;
}

juce::Colour enabledColour (juce::Colour c, const juce::Component& component)
{
    return component.isEnabled() ? c : c.withMultipliedAlpha (0.4f);
}
} // namespace

VsLookAndFeel::VsLookAndFeel()
{
    // The V4 colour scheme covers every widget the old pages use (scroll bars, list boxes, text editors, ...)
    setColourScheme ({ theme::background,  // windowBackground
                       theme::panel,       // widgetBackground
                       theme::panel,       // menuBackground
                       theme::controlOutline, // outline
                       theme::text,        // defaultText
                       theme::control,     // defaultFill
                       theme::textOnAccent, // highlightedText
                       theme::accent,      // highlightedFill
                       theme::text });     // menuText

    setColour (juce::ResizableWindow::backgroundColourId, theme::background);
    setColour (juce::DocumentWindow::backgroundColourId, theme::background);

    setColour (juce::Slider::rotarySliderFillColourId, theme::accent);
    setColour (juce::Slider::rotarySliderOutlineColourId, theme::knobTrack);
    setColour (juce::Slider::trackColourId, theme::accent);
    setColour (juce::Slider::backgroundColourId, theme::knobTrack);
    setColour (juce::Slider::thumbColourId, theme::text);
    setColour (juce::Slider::textBoxTextColourId, theme::text);
    setColour (juce::Slider::textBoxBackgroundColourId, theme::control);
    setColour (juce::Slider::textBoxOutlineColourId, theme::controlOutline);
    setColour (juce::Slider::textBoxHighlightColourId, theme::accentDim);

    setColour (juce::Label::textColourId, theme::text);
    setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Label::textWhenEditingColourId, theme::text);
    setColour (juce::Label::backgroundWhenEditingColourId, theme::control);
    setColour (juce::Label::outlineWhenEditingColourId, theme::accent);

    setColour (juce::TextButton::buttonColourId, theme::control);
    setColour (juce::TextButton::buttonOnColourId, theme::accent);
    setColour (juce::TextButton::textColourOffId, theme::text);
    setColour (juce::TextButton::textColourOnId, theme::textOnAccent);

    setColour (juce::ToggleButton::textColourId, theme::text);
    setColour (juce::ToggleButton::tickColourId, theme::accent);
    setColour (juce::ToggleButton::tickDisabledColourId, theme::textDim);

    setColour (juce::ComboBox::backgroundColourId, theme::control);
    setColour (juce::ComboBox::outlineColourId, theme::controlOutline);
    setColour (juce::ComboBox::textColourId, theme::text);
    setColour (juce::ComboBox::arrowColourId, theme::textDim);
    setColour (juce::ComboBox::focusedOutlineColourId, theme::accentDim);

    setColour (juce::PopupMenu::backgroundColourId, theme::panel);
    setColour (juce::PopupMenu::textColourId, theme::text);
    setColour (juce::PopupMenu::headerTextColourId, theme::textDim);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, theme::accent.withAlpha (0.22f));
    setColour (juce::PopupMenu::highlightedTextColourId, theme::text);

    setColour (juce::TextEditor::backgroundColourId, theme::control);
    setColour (juce::TextEditor::textColourId, theme::text);
    setColour (juce::TextEditor::outlineColourId, theme::controlOutline);
    setColour (juce::TextEditor::focusedOutlineColourId, theme::accent);
    setColour (juce::TextEditor::highlightColourId, theme::accentDim);
    setColour (juce::CaretComponent::caretColourId, theme::accent);

    setColour (juce::ListBox::backgroundColourId, theme::panel);
    setColour (juce::ListBox::outlineColourId, theme::panelOutline);
    setColour (juce::ScrollBar::thumbColourId, theme::controlOutline);

    setColour (juce::TooltipWindow::backgroundColourId, theme::panelHeader);
    setColour (juce::TooltipWindow::textColourId, theme::text);
    setColour (juce::TooltipWindow::outlineColourId, theme::controlOutline);

    setColour (juce::AlertWindow::backgroundColourId, theme::panel);
    setColour (juce::AlertWindow::textColourId, theme::text);
    setColour (juce::AlertWindow::outlineColourId, theme::controlOutline);

    setColour (juce::BubbleComponent::backgroundColourId, theme::panelHeader);
    setColour (juce::BubbleComponent::outlineColourId, theme::accentDim);

    setColour (juce::TabbedButtonBar::tabOutlineColourId, theme::panelOutline);
    setColour (juce::TabbedButtonBar::frontOutlineColourId, theme::accent);
    setColour (juce::TabbedButtonBar::tabTextColourId, theme::textDim);
    setColour (juce::TabbedButtonBar::frontTextColourId, theme::text);
    setColour (juce::TabbedComponent::backgroundColourId, theme::background);
    setColour (juce::TabbedComponent::outlineColourId, theme::panelOutline);
}

VsLookAndFeel::~VsLookAndFeel() = default;

//==============================================================================
void VsLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                                      float sliderPosProportional, float rotaryStartAngle, float rotaryEndAngle,
                                      juce::Slider& slider)
{
    const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat();
    const auto size = juce::jmin (bounds.getWidth(), bounds.getHeight());

    if (size < 8.0f)
        return;

    const auto centre = bounds.getCentre();
    const auto lineWidth = juce::jlimit (2.0f, 4.0f, size * 0.075f);
    const auto modWidth = juce::jmax (1.5f, lineWidth * 0.6f);
    const auto outerRadius = size * 0.5f - modWidth * 0.5f - 0.5f;          // modulation ring
    const auto arcRadius = outerRadius - modWidth * 0.5f - lineWidth * 0.5f - 1.5f; // track / value arc
    const auto bodyRadius = arcRadius - lineWidth * 0.5f - juce::jmax (2.0f, size * 0.06f);
    const auto pos = juce::jlimit (0.0f, 1.0f, sliderPosProportional);
    const auto angleOf = [&] (float proportion) { return rotaryStartAngle + proportion * (rotaryEndAngle - rotaryStartAngle); };
    const auto valueAngle = angleOf (pos);
    const bool enabled = slider.isEnabled();
    const bool hot = slider.isMouseOverOrDragging() && enabled;

    // Track
    juce::Path track;
    track.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, rotaryStartAngle, rotaryEndAngle, true);
    g.setColour (theme::knobTrack);
    g.strokePath (track, juce::PathStrokeType (lineWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // Value arc (from the centre for bipolar parameters)
    const auto from = flag (slider, "bipolar") ? 0.5f : 0.0f;

    if (std::abs (pos - from) > 0.002f)
    {
        juce::Path value;
        value.addCentredArc (centre.x, centre.y, arcRadius, arcRadius, 0.0f, angleOf (juce::jmin (from, pos)),
                             angleOf (juce::jmax (from, pos)), true);
        g.setColour (enabledColour (hot ? theme::accent.brighter (0.15f) : theme::accent, slider));
        g.strokePath (value, juce::PathStrokeType (lineWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // Modulation ring
    if (const auto mod = modulationRange (slider); mod.getLength() > 0.001f)
    {
        juce::Path ring;
        ring.addCentredArc (centre.x, centre.y, outerRadius, outerRadius, 0.0f, angleOf (mod.getStart()),
                            angleOf (mod.getEnd()), true);
        g.setColour (enabledColour (theme::modulation, slider));
        g.strokePath (ring, juce::PathStrokeType (modWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // Cap with a soft vertical gradient and a pointer
    if (bodyRadius > 3.0f)
    {
        const auto body = juce::Rectangle<float> (bodyRadius * 2.0f, bodyRadius * 2.0f).withCentre (centre);
        g.setGradientFill (juce::ColourGradient (hot ? theme::knobBody.brighter (0.08f) : theme::knobBody, body.getX(),
                                                 body.getY(), theme::knobBodyDark, body.getX(), body.getBottom(), false));
        g.fillEllipse (body);
        g.setColour (theme::controlOutline.withAlpha (0.8f));
        g.drawEllipse (body.reduced (0.5f), 1.0f);

        juce::Path pointer;
        const auto pw = juce::jmax (1.6f, size * 0.045f);
        pointer.addRoundedRectangle (-pw * 0.5f, -bodyRadius + bodyRadius * 0.18f, pw, bodyRadius * 0.55f, pw * 0.5f);
        g.setColour (enabledColour (theme::text, slider));
        g.fillPath (pointer, juce::AffineTransform::rotation (valueAngle).translated (centre));
    }
}

void VsLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
                                      float minSliderPos, float maxSliderPos, juce::Slider::SliderStyle style,
                                      juce::Slider& slider)
{
    const auto area = juce::Rectangle<int> (x, y, width, height).toFloat();

    if (style == juce::Slider::LinearBar && flag (slider, "vsBar"))
    {
        // Flat value bar (Knob::Style::bar): the Knob draws label and value on top
        const auto bar = slider.getLocalBounds().toFloat().reduced (0.5f);
        const auto corner = theme::controlCorner;
        g.setColour (slider.isMouseOverOrDragging() ? theme::controlHover : theme::control);
        g.fillRoundedRectangle (bar, corner);

        const auto proportion = juce::jlimit (0.0f, 1.0f, (sliderPos - bar.getX()) / juce::jmax (1.0f, bar.getWidth()));
        const auto from = flag (slider, "bipolar") ? 0.5f : 0.0f;
        const auto x0 = bar.getX() + bar.getWidth() * juce::jmin (from, proportion);
        const auto x1 = bar.getX() + bar.getWidth() * juce::jmax (from, proportion);
        const auto fillColour = slider.findColour (juce::Slider::trackColourId);

        g.saveState();
        juce::Path clip;
        clip.addRoundedRectangle (bar, corner);
        g.reduceClipRegion (clip);

        if (std::abs (proportion - from) > 0.002f)
        {
            // Light tint over the value range + a solid indicator line at the bottom (the text stays clean)
            const auto range = juce::Rectangle<float>::leftTopRightBottom (x0, bar.getY(), x1, bar.getBottom());
            g.setColour (enabledColour (fillColour.withAlpha (0.13f), slider));
            g.fillRect (range);
            g.setColour (enabledColour (fillColour, slider));
            g.fillRect (range.withTop (bar.getBottom() - 3.0f));
        }

        if (from > 0.0f)
        {
            g.setColour (theme::controlOutline);
            g.fillRect (juce::Rectangle<float> (bar.getX() + bar.getWidth() * from - 0.5f, bar.getBottom() - 5.0f, 1.0f, 5.0f));
        }

        if (const auto mod = modulationRange (slider); mod.getLength() > 0.001f)
        {
            g.setColour (enabledColour (theme::modulation, slider));
            g.fillRect (juce::Rectangle<float>::leftTopRightBottom (bar.getX() + bar.getWidth() * mod.getStart(),
                                                                    bar.getY(),
                                                                    bar.getX() + bar.getWidth() * mod.getEnd(),
                                                                    bar.getY() + 2.0f));
        }

        g.restoreState();
        g.setColour (theme::controlOutline);
        g.drawRoundedRectangle (bar, corner, 1.0f);
        return;
    }

    if (slider.isBar() || slider.isTwoValue() || slider.isThreeValue())
    {
        LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style, slider);
        return;
    }

    // Single-value linear slider (old pages): thin track, accent fill, round thumb
    const bool horizontal = slider.isHorizontal();
    const auto trackWidth = 4.0f;
    const auto start = horizontal ? juce::Point<float> (area.getX() + 4.0f, area.getCentreY())
                                  : juce::Point<float> (area.getCentreX(), area.getBottom() - 4.0f);
    const auto end = horizontal ? juce::Point<float> (area.getRight() - 4.0f, area.getCentreY())
                                : juce::Point<float> (area.getCentreX(), area.getY() + 4.0f);
    const auto thumb = horizontal ? juce::Point<float> (sliderPos, area.getCentreY())
                                  : juce::Point<float> (area.getCentreX(), sliderPos);

    juce::Path track;
    track.startNewSubPath (start);
    track.lineTo (end);
    g.setColour (theme::knobTrack);
    g.strokePath (track, { trackWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded });

    juce::Path value;
    value.startNewSubPath (start);
    value.lineTo (thumb);
    g.setColour (enabledColour (slider.findColour (juce::Slider::trackColourId), slider));
    g.strokePath (value, { trackWidth, juce::PathStrokeType::curved, juce::PathStrokeType::rounded });

    const auto thumbSize = 12.0f;
    g.setColour (enabledColour (theme::text, slider));
    g.fillEllipse (juce::Rectangle<float> (thumbSize, thumbSize).withCentre (thumb));
}

juce::Label* VsLookAndFeel::createSliderTextBox (juce::Slider& slider)
{
    auto* label = LookAndFeel_V4::createSliderTextBox (slider);
    label->setFont (theme::font (theme::fontNormal));
    label->setColour (juce::Label::outlineColourId, slider.findColour (juce::Slider::textBoxOutlineColourId));
    return label;
}

juce::Font VsLookAndFeel::getSliderPopupFont (juce::Slider&)
{
    return theme::font (theme::fontNormal, true);
}

int VsLookAndFeel::getSliderPopupPlacement (juce::Slider&)
{
    return juce::BubbleComponent::above;
}

void VsLookAndFeel::drawBubble (juce::Graphics& g, juce::BubbleComponent&, const juce::Point<float>&,
                                const juce::Rectangle<float>& body)
{
    g.setColour (theme::panelHeader);
    g.fillRoundedRectangle (body, theme::controlCorner);
    g.setColour (theme::accentDim);
    g.drawRoundedRectangle (body.reduced (0.5f), theme::controlCorner, 1.0f);
}

//==============================================================================
void VsLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour& backgroundColour,
                                          bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown)
{
    const auto bounds = button.getLocalBounds().toFloat().reduced (0.5f);
    const bool on = button.getToggleState();
    auto fill = on ? button.findColour (juce::TextButton::buttonOnColourId) : backgroundColour;

    if (! on && backgroundColour == theme::control && shouldDrawButtonAsHighlighted)
        fill = theme::controlHover;
    else if (shouldDrawButtonAsDown)
        fill = fill.brighter (0.1f);
    else if (shouldDrawButtonAsHighlighted)
        fill = fill.brighter (0.05f);

    g.setColour (enabledColour (fill, button));
    g.fillRoundedRectangle (bounds, theme::controlCorner);
    g.setColour (on ? fill.brighter (0.2f) : (shouldDrawButtonAsHighlighted ? theme::accentDim : theme::controlOutline));
    g.drawRoundedRectangle (bounds, theme::controlCorner, 1.0f);
}

juce::Font VsLookAndFeel::getTextButtonFont (juce::TextButton&, int buttonHeight)
{
    return theme::font (juce::jmin (theme::fontNormal, static_cast<float> (buttonHeight) * 0.6f));
}

void VsLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& button, bool, bool)
{
    auto area = button.getLocalBounds().reduced (6, 0);
    const auto arrow = button.getProperties().getWithDefault ("vsArrow", {}).toString();

    if (arrow.isNotEmpty())
    {
        // "<" / ">" as a drawn chevron (preset bar)
        const auto r = button.getLocalBounds().toFloat();
        const auto s = juce::jmin (r.getWidth(), r.getHeight()) * 0.42f;
        const auto c = r.getCentre();
        const auto dx = arrow == "left" ? s * 0.25f : -s * 0.25f;
        juce::Path p;
        p.startNewSubPath (c.x + dx, c.y - s * 0.5f);
        p.lineTo (c.x - dx, c.y);
        p.lineTo (c.x + dx, c.y + s * 0.5f);
        g.setColour (enabledColour (button.isOver() ? theme::text : theme::textDim, button));
        g.strokePath (p, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        return;
    }

    const bool dropdown = flag (button, "vsDropdown");
    const auto colour = button.findColour (button.getToggleState() ? juce::TextButton::textColourOnId
                                                                    : juce::TextButton::textColourOffId);

    if (dropdown)
    {
        const auto arrowArea = area.removeFromRight (14).toFloat();
        g.setColour (enabledColour (theme::textDim, button));
        g.strokePath (chevron (arrowArea), juce::PathStrokeType (1.5f, juce::PathStrokeType::curved,
                                                                 juce::PathStrokeType::rounded));
        area.removeFromRight (2);
    }

    g.setFont (getTextButtonFont (button, button.getHeight()));
    g.setColour (enabledColour (colour, button));
    g.drawFittedText (button.getButtonText(), area, dropdown ? juce::Justification::centredLeft : juce::Justification::centred,
                      1, 0.8f);
}

void VsLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& button, bool shouldDrawButtonAsHighlighted,
                                      bool shouldDrawButtonAsDown)
{
    const auto style = button.getProperties().getWithDefault ("vsStyle", {}).toString();
    const bool on = button.getToggleState();
    const auto bounds = button.getLocalBounds().toFloat();

    if (style == "power")
    {
        // Round power switch: accent ring + glyph when on
        const auto d = juce::jmin (bounds.getWidth(), bounds.getHeight()) - 2.0f;
        const auto r = juce::Rectangle<float> (d, d).withCentre (bounds.getCentre());
        g.setColour (on ? theme::accent.withAlpha (0.18f) : (shouldDrawButtonAsHighlighted ? theme::controlHover : theme::control));
        g.fillEllipse (r);
        g.setColour (on ? theme::accent : theme::controlOutline);
        g.drawEllipse (r.reduced (0.5f), 1.0f);

        juce::Path glyph;
        const auto c = r.getCentre();
        const auto gr = d * 0.24f;
        glyph.addCentredArc (c.x, c.y, gr, gr, 0.0f, juce::MathConstants<float>::pi * 0.22f,
                             juce::MathConstants<float>::pi * 1.78f, true);
        glyph.startNewSubPath (c.x, c.y - gr * 1.25f);
        glyph.lineTo (c.x, c.y - gr * 0.15f);
        g.setColour (enabledColour (on ? theme::accent : theme::textDim, button));
        g.strokePath (glyph, juce::PathStrokeType (juce::jmax (1.3f, d * 0.09f), juce::PathStrokeType::curved,
                                                   juce::PathStrokeType::rounded));
        return;
    }

    if (style == "chip")
    {
        const auto r = bounds.reduced (0.5f);
        auto fill = on ? theme::accent.withAlpha (0.2f) : theme::control;

        if (shouldDrawButtonAsHighlighted || shouldDrawButtonAsDown)
            fill = on ? theme::accent.withAlpha (0.28f) : theme::controlHover;

        g.setColour (fill);
        g.fillRoundedRectangle (r, theme::controlCorner);
        g.setColour (on ? theme::accent : theme::controlOutline);
        g.drawRoundedRectangle (r, theme::controlCorner, 1.0f);

        // LED dot + text
        auto content = button.getLocalBounds().reduced (7, 0);
        const auto led = content.removeFromLeft (8).toFloat();
        const auto ledDot = juce::Rectangle<float> (6.0f, 6.0f).withCentre (led.getCentre());
        g.setColour (on ? theme::accent : theme::knobTrack);
        g.fillEllipse (ledDot);
        content.removeFromLeft (5);
        g.setColour (enabledColour (on ? theme::text : theme::textDim, button));
        g.setFont (theme::font (theme::fontSmall, on));
        g.drawFittedText (button.getButtonText(), content, juce::Justification::centredLeft, 1, 0.8f);
        return;
    }

    // Default (old pages): a small switch followed by the text
    const auto h = juce::jmin (bounds.getHeight() - 4.0f, 16.0f);
    const auto sw = juce::Rectangle<float> (h * 1.75f, h).withPosition (2.0f, (bounds.getHeight() - h) * 0.5f);
    g.setColour (on ? theme::accent : (shouldDrawButtonAsHighlighted ? theme::controlHover : theme::knobTrack));
    g.fillRoundedRectangle (sw, h * 0.5f);
    const auto knobD = h - 4.0f;
    const auto knobX = on ? sw.getRight() - 2.0f - knobD : sw.getX() + 2.0f;
    g.setColour (enabledColour (on ? theme::textOnAccent : theme::textDim, button));
    g.fillEllipse (knobX, sw.getY() + 2.0f, knobD, knobD);

    g.setColour (enabledColour (button.findColour (juce::ToggleButton::textColourId), button));
    g.setFont (theme::font (juce::jmin (theme::fontNormal + 1.0f, bounds.getHeight() * 0.75f)));
    g.drawFittedText (button.getButtonText(),
                      button.getLocalBounds().withTrimmedLeft (juce::roundToInt (sw.getRight()) + 6).withTrimmedRight (2),
                      juce::Justification::centredLeft, 2, 0.8f);
}

//==============================================================================
void VsLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height, bool, int, int, int, int, juce::ComboBox& box)
{
    const auto bounds = juce::Rectangle<int> (0, 0, width, height).toFloat().reduced (0.5f);
    const bool hot = box.isMouseOver (true) && box.isEnabled();
    g.setColour (hot ? theme::controlHover : box.findColour (juce::ComboBox::backgroundColourId));
    g.fillRoundedRectangle (bounds, theme::controlCorner);
    g.setColour (box.hasKeyboardFocus (false) || hot ? theme::accentDim : box.findColour (juce::ComboBox::outlineColourId));
    g.drawRoundedRectangle (bounds, theme::controlCorner, 1.0f);

    const auto arrowArea = juce::Rectangle<float> (static_cast<float> (width) - 18.0f, 0.0f, 14.0f, static_cast<float> (height));
    g.setColour (enabledColour (box.findColour (juce::ComboBox::arrowColourId), box));
    g.strokePath (chevron (arrowArea), juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

juce::Font VsLookAndFeel::getComboBoxFont (juce::ComboBox& box)
{
    const auto size = flag (box, "vsCompact") ? theme::fontSmall + 0.5f : theme::fontNormal;
    return theme::font (juce::jmin (size, static_cast<float> (box.getHeight()) * 0.6f));
}

void VsLookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    label.setBounds (4, 1, box.getWidth() - 4 - 18, box.getHeight() - 2);
    label.setFont (getComboBoxFont (box));
    label.setMinimumHorizontalScale (0.75f);
}

//==============================================================================
void VsLookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int width, int height)
{
    g.fillAll (theme::panel);
    g.setColour (theme::controlOutline);
    g.drawRect (0, 0, width, height, 1);
}

void VsLookAndFeel::drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle<int>& area, bool isSeparator,
                                       bool isActive, bool isHighlighted, bool isTicked, bool hasSubMenu,
                                       const juce::String& text, const juce::String& shortcutKeyText,
                                       const juce::Drawable* icon, const juce::Colour* textColour)
{
    if (isSeparator)
    {
        g.setColour (theme::panelOutline);
        g.fillRect (area.reduced (8, 0).withHeight (1).withCentre (area.getCentre()));
        return;
    }

    auto r = area.reduced (2, 0);

    if (isHighlighted && isActive)
    {
        g.setColour (findColour (juce::PopupMenu::highlightedBackgroundColourId));
        g.fillRoundedRectangle (r.toFloat(), 3.0f);
    }

    auto colour = textColour != nullptr ? *textColour : theme::text;

    if (! isActive)
        colour = theme::textDim.withAlpha (0.6f);

    auto content = r.reduced (8, 0);
    const auto tickArea = content.removeFromLeft (14).toFloat();

    if (isTicked)
    {
        g.setColour (theme::accent);
        g.fillEllipse (juce::Rectangle<float> (6.0f, 6.0f).withCentre (tickArea.getCentre()));
    }
    else if (icon != nullptr)
    {
        icon->drawWithin (g, tickArea.reduced (1.0f), juce::RectanglePlacement::centred, 1.0f);
    }

    content.removeFromLeft (4);

    if (hasSubMenu)
    {
        const auto arrow = content.removeFromRight (12).toFloat();
        juce::Path p;
        p.startNewSubPath (arrow.getCentreX() - 2.0f, arrow.getCentreY() - 4.0f);
        p.lineTo (arrow.getCentreX() + 2.0f, arrow.getCentreY());
        p.lineTo (arrow.getCentreX() - 2.0f, arrow.getCentreY() + 4.0f);
        g.setColour (theme::textDim);
        g.strokePath (p, juce::PathStrokeType (1.5f));
    }

    g.setFont (getPopupMenuFont());
    g.setColour (colour);
    g.drawFittedText (text, content, juce::Justification::centredLeft, 1, 0.8f);

    if (shortcutKeyText.isNotEmpty())
    {
        g.setColour (theme::textDim);
        g.drawText (shortcutKeyText, content, juce::Justification::centredRight, true);
    }
}

void VsLookAndFeel::drawPopupMenuSectionHeader (juce::Graphics& g, const juce::Rectangle<int>& area,
                                                const juce::String& sectionName)
{
    g.setFont (theme::titleFont());
    g.setColour (theme::textDim);
    g.drawFittedText (sectionName.toUpperCase(), area.reduced (12, 0), juce::Justification::bottomLeft, 1);
}

juce::Font VsLookAndFeel::getPopupMenuFont()
{
    return theme::font (theme::fontNormal + 0.5f);
}

//==============================================================================
void VsLookAndFeel::drawTooltip (juce::Graphics& g, const juce::String& text, int width, int height)
{
    const auto bounds = juce::Rectangle<int> (width, height).toFloat();
    g.setColour (theme::panelHeader);
    g.fillRoundedRectangle (bounds, 4.0f);
    g.setColour (theme::controlOutline);
    g.drawRoundedRectangle (bounds.reduced (0.5f), 4.0f, 1.0f);

    juce::AttributedString s;
    s.setJustification (juce::Justification::centredLeft);
    s.append (text, theme::font (theme::fontNormal), theme::text);
    juce::TextLayout layout;
    layout.createLayoutWithBalancedLineLengths (s, static_cast<float> (width) - 16.0f);
    layout.draw (g, bounds.reduced (8.0f, 5.0f));
}

juce::Rectangle<int> VsLookAndFeel::getTooltipBounds (const juce::String& tipText, juce::Point<int> screenPos,
                                                      juce::Rectangle<int> parentArea)
{
    juce::AttributedString s;
    s.append (tipText, theme::font (theme::fontNormal), theme::text);
    juce::TextLayout layout;
    layout.createLayoutWithBalancedLineLengths (s, 320.0f);

    const auto w = static_cast<int> (layout.getWidth() + 18.0f);
    const auto h = static_cast<int> (layout.getHeight() + 12.0f);

    return juce::Rectangle<int> (screenPos.x > parentArea.getCentreX() ? screenPos.x - (w + 12) : screenPos.x + 24,
                                 screenPos.y > parentArea.getCentreY() ? screenPos.y - (h + 6) : screenPos.y + 6, w, h)
        .constrainedWithin (parentArea);
}

//==============================================================================
int VsLookAndFeel::getTabButtonBestWidth (juce::TabBarButton& button, int tabDepth)
{
    const auto textWidth = juce::GlyphArrangement::getStringWidthInt (theme::font (theme::fontNormal, true),
                                                                      button.getButtonText());
    return juce::jmax (tabDepth * 2, textWidth + 32);
}

void VsLookAndFeel::drawTabButton (juce::TabBarButton& button, juce::Graphics& g, bool isMouseOver, bool)
{
    const auto area = button.getActiveArea();
    const bool front = button.isFrontTab();

    if (front || isMouseOver)
    {
        g.setColour (front ? theme::panel : theme::controlHover.withAlpha (0.5f));
        g.fillRect (area);
    }

    if (front)
    {
        g.setColour (theme::accent);
        g.fillRect (area.withTop (area.getBottom() - 2));
    }

    g.setColour (front ? theme::text : theme::textDim);
    g.setFont (theme::font (theme::fontNormal, front));
    g.drawFittedText (button.getButtonText(), area.reduced (8, 0), juce::Justification::centred, 1);
}

void VsLookAndFeel::drawTabbedButtonBarBackground (juce::TabbedButtonBar& bar, juce::Graphics& g)
{
    g.setColour (theme::background);
    g.fillRect (bar.getLocalBounds());
    g.setColour (theme::panelOutline);
    g.fillRect (bar.getLocalBounds().removeFromBottom (1));
}

void VsLookAndFeel::drawCornerResizer (juce::Graphics& g, int w, int h, bool isMouseOver, bool isMouseDragging)
{
    g.setColour ((isMouseOver || isMouseDragging) ? theme::accent : theme::textDim.withAlpha (0.6f));
    const auto size = static_cast<float> (juce::jmin (w, h));

    for (float i = 0.3f; i < 1.0f; i += 0.3f)
        g.drawLine (static_cast<float> (w) - size * i, static_cast<float> (h), static_cast<float> (w),
                    static_cast<float> (h) - size * i, 1.5f);
}

} // namespace vocalsynth::ui
