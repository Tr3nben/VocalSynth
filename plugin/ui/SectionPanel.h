#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace vocalsynth::ui
{

/** Titled, rounded frame around a group of controls (docs/SYNTH-DESIGN.md 20.3). Children are added by the
    owner; getContentBounds() is the area below the title inside the padding. Optional "on" switch in the
    title row (setHeaderComponent, e.g. a ChoiceChip for oscAOn). Message thread. Owner: agent E. */
class SectionPanel : public juce::Component
{
public:
    explicit SectionPanel (const juce::String& title);
    ~SectionPanel() override;

    const juce::String& getTitle() const noexcept { return title; }

    /** Shown right-aligned in the title row (not owned; nullptr = none). */
    void setHeaderComponent (juce::Component* component);

    juce::Rectangle<int> getContentBounds() const;

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    juce::String title;
    juce::Component* header = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SectionPanel)
};

} // namespace vocalsynth::ui
