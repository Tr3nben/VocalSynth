#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace vocalsynth::ui
{

/** Rotary knob bound to one parameter (docs/SYNTH-DESIGN.md 20.3): label above, value text below
    (parameter text + label/unit), double-click = default, drag with shift = fine, mouse wheel. Optional
    modulation ring (setModulationRange: the normalised range the mod matrix currently sweeps, drawn by
    VsLookAndFeel in theme::modulation). componentID = the parameter ID (tests and the "every synth parameter
    has a control" check find knobs by it). Message thread. Owner: agent E.

    Details (agent E): the slider is attached with a SliderAttachment (every change is a host gesture; no
    direct parameter writes). Fine drag and fine wheel with shift OR ctrl / cmd (1/10 speed, also when the
    modifier changes during a drag: no jump). While dragging a bubble shows the value with its unit; a
    double-click on the value text lets the user type a value. Style::bar is a flat horizontal bar (label left,
    value right, drawn by the Knob) for tight rows (mod amounts, master levels); bipolar parameters (range
    symmetric around 0) fill from the centre and show a sign. */
class Knob : public juce::Component
{
public:
    enum class Style
    {
        rotary,
        bar
    };

    Knob (juce::AudioProcessorValueTreeState& apvts, const juce::String& parameterId, const juce::String& labelText);
    Knob (juce::AudioProcessorValueTreeState& apvts, const juce::String& parameterId, const juce::String& labelText,
          Style style);
    ~Knob() override;

    juce::Slider& getSlider() noexcept { return slider; }
    const juce::String& getParameterId() const noexcept { return parameterId; }
    Style getStyle() const noexcept { return style; }

    /** Normalised (0..1) range of the modulation ring; empty range = no ring. */
    void setModulationRange (juce::Range<double> normalisedRange);
    juce::Range<double> getModulationRange() const noexcept { return modulationRange; }

    /** Tooltip of the knob (slider and value text). */
    void setTooltip (const juce::String& text);

    /** The value text as shown ("-6.0 dB", "+12 st", "1250 Hz"). */
    juce::String getValueText() const;
    const juce::String& getLabelText() const noexcept { return labelText; }

    /** Fine-drag factor (shift / ctrl / cmd). */
    static constexpr double fineFactor = 0.1;

    void resized() override;
    void parentHierarchyChanged() override;
    void paintOverChildren (juce::Graphics& g) override;

    /** Slider with fine drag and fine wheel (public for the tests). */
    class DragSlider : public juce::Slider
    {
    public:
        DragSlider();
        void mouseDown (const juce::MouseEvent& e) override;
        void mouseDrag (const juce::MouseEvent& e) override;
        void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;

        static bool isFine (const juce::ModifierKeys& mods) noexcept;

    private:
        juce::Point<float> lastPosition, virtualPosition;
    };

private:
    void updateValueText();

    juce::String parameterId;
    juce::String labelText;
    Style style = Style::rotary;
    bool bipolar = false;
    juce::RangedAudioParameter* parameter = nullptr;
    DragSlider slider;
    juce::Label label;
    juce::Label valueLabel;
    juce::Range<double> modulationRange;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Knob)
};

} // namespace vocalsynth::ui
