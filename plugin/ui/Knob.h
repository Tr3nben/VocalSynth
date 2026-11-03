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
    has a control" check find knobs by it). Message thread. Owner: agent E. */
class Knob : public juce::Component
{
public:
    Knob (juce::AudioProcessorValueTreeState& apvts, const juce::String& parameterId, const juce::String& labelText);
    ~Knob() override;

    juce::Slider& getSlider() noexcept { return slider; }
    const juce::String& getParameterId() const noexcept { return parameterId; }

    /** Normalised (0..1) range of the modulation ring; empty range = no ring. */
    void setModulationRange (juce::Range<double> normalisedRange);

    void resized() override;

private:
    juce::String parameterId;
    juce::Slider slider;
    juce::Label label;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Knob)
};

} // namespace vocalsynth::ui
