#pragma once

#include "ui/Theme.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace vocalsynth::ui
{

/** The VocalSynth look (docs/SYNTH-DESIGN.md 20.2): vector-drawn, theme colours, no bitmaps (HiDPI-sharp at
    any scale). Set on the whole editor by the integration; the old pages keep their layout and only get
    these colours / shapes. Draws rotary sliders (arc track + value arc + pointer, modulation ring from the
    slider properties "modStart" / "modEnd" = doubles in normalised units 0..1, see Knob), linear sliders,
    buttons, toggle buttons, combo boxes, popup menus, tab buttons, labels. Owner: agent E.

    Component properties understood (set by the widgets of plugin/ui, optional elsewhere):
    - Slider "bipolar" (bool): the value arc / bar starts at the centre (pan, fine, amounts).
    - Slider "modStart" / "modEnd" (double 0..1): modulation ring (rotary) or band (bar); equal = none.
    - Slider "vsBar" (bool) on a LinearBar slider: the flat value bar of Knob::Style::bar (text drawn by Knob).
    - ToggleButton "vsStyle" = "power" (round power switch, section headers) or "chip" (pill, accent when on);
      anything else: a switch + text (old pages).
    - Button "vsDropdown" (bool): a chevron at the right (table menu, preset name).
    - ComboBox "vsCompact" (bool): a slightly smaller font (dense rows such as the mod matrix).
    - Button "vsArrow" = "left" / "right": a drawn chevron instead of the text (preset prev / next). */
class VsLookAndFeel : public juce::LookAndFeel_V4
{
public:
    VsLookAndFeel();
    ~VsLookAndFeel() override;

    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPosProportional,
                           float rotaryStartAngle, float rotaryEndAngle, juce::Slider& slider) override;

    void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPos, float minSliderPos,
                           float maxSliderPos, juce::Slider::SliderStyle style, juce::Slider& slider) override;
    juce::Label* createSliderTextBox (juce::Slider& slider) override;
    juce::Font getSliderPopupFont (juce::Slider& slider) override;
    int getSliderPopupPlacement (juce::Slider& slider) override;
    void drawBubble (juce::Graphics& g, juce::BubbleComponent& bubble, const juce::Point<float>& tip,
                     const juce::Rectangle<float>& body) override;

    void drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour& backgroundColour,
                               bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;
    void drawButtonText (juce::Graphics& g, juce::TextButton& button, bool shouldDrawButtonAsHighlighted,
                         bool shouldDrawButtonAsDown) override;
    juce::Font getTextButtonFont (juce::TextButton& button, int buttonHeight) override;
    void drawToggleButton (juce::Graphics& g, juce::ToggleButton& button, bool shouldDrawButtonAsHighlighted,
                           bool shouldDrawButtonAsDown) override;

    void drawComboBox (juce::Graphics& g, int width, int height, bool isButtonDown, int buttonX, int buttonY,
                       int buttonW, int buttonH, juce::ComboBox& box) override;
    juce::Font getComboBoxFont (juce::ComboBox& box) override;
    void positionComboBoxText (juce::ComboBox& box, juce::Label& label) override;

    void drawPopupMenuBackground (juce::Graphics& g, int width, int height) override;
    void drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle<int>& area, bool isSeparator, bool isActive,
                            bool isHighlighted, bool isTicked, bool hasSubMenu, const juce::String& text,
                            const juce::String& shortcutKeyText, const juce::Drawable* icon,
                            const juce::Colour* textColour) override;
    void drawPopupMenuSectionHeader (juce::Graphics& g, const juce::Rectangle<int>& area,
                                     const juce::String& sectionName) override;
    juce::Font getPopupMenuFont() override;

    void drawTooltip (juce::Graphics& g, const juce::String& text, int width, int height) override;
    juce::Rectangle<int> getTooltipBounds (const juce::String& tipText, juce::Point<int> screenPos,
                                           juce::Rectangle<int> parentArea) override;

    int getTabButtonBestWidth (juce::TabBarButton& button, int tabDepth) override;
    void drawTabButton (juce::TabBarButton& button, juce::Graphics& g, bool isMouseOver, bool isMouseDown) override;
    void drawTabbedButtonBarBackground (juce::TabbedButtonBar& bar, juce::Graphics& g) override;

    void drawCornerResizer (juce::Graphics& g, int w, int h, bool isMouseOver, bool isMouseDragging) override;

    /** The rotary track angles every Knob uses (7 o'clock to 5 o'clock). */
    static constexpr float rotaryStart = juce::MathConstants<float>::pi * 1.25f;
    static constexpr float rotaryEnd = juce::MathConstants<float>::pi * 2.75f;

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VsLookAndFeel)
};

} // namespace vocalsynth::ui
