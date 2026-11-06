#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace vocalsynth::ui
{

/** Compact control for a choice or bool parameter (docs/SYNTH-DESIGN.md 20.3): shows the current choice
    as a rounded chip; click opens a popup menu of the choices (bool: toggles). componentID = the parameter
    ID. Message thread. Owner: agent E.

    Details (agent E): choices are a ComboBox (ComboBoxAttachment; popup of the choices, mouse wheel steps)
    with an optional caption above it; bools are a ToggleButton (ButtonAttachment) drawn as a pill with an LED
    (ToggleStyle::chip, text = the given text or the parameter name) or as a round power switch
    (ToggleStyle::power, for section headers). Tooltips: setTooltip (default = the parameter name). */
class ChoiceChip : public juce::Component
{
public:
    enum class ToggleStyle
    {
        chip,
        power
    };

    ChoiceChip (juce::AudioProcessorValueTreeState& apvts, const juce::String& parameterId);

    /** text: the toggle's text (bool) or the caption above the chip (choice). */
    ChoiceChip (juce::AudioProcessorValueTreeState& apvts, const juce::String& parameterId, const juce::String& text,
                ToggleStyle toggleStyle = ToggleStyle::chip);
    ~ChoiceChip() override;

    const juce::String& getParameterId() const noexcept { return parameterId; }

    /** The ComboBox used for choices (null for bools) and the button used for bools (null for choices). */
    juce::ComboBox* getComboBox() noexcept { return combo.get(); }
    juce::ToggleButton* getToggle() noexcept { return toggle.get(); }

    /** Caption above a choice chip (empty = none; takes theme::chipCaptionHeight of the height). */
    void setCaption (const juce::String& caption);
    const juce::String& getCaption() const noexcept { return caption; }

    void setTooltip (const juce::String& text);

    /** The text the chip shows now (the current choice, or the toggle text). */
    juce::String getDisplayText() const;

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    juce::String parameterId;
    juce::String caption;
    std::unique_ptr<juce::ComboBox> combo;
    std::unique_ptr<juce::ToggleButton> toggle;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> comboAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> buttonAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChoiceChip)
};

} // namespace vocalsynth::ui
