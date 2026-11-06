#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace vocalsynth::ui
{

/** Titled, rounded frame around a group of controls (docs/SYNTH-DESIGN.md 20.3). Children are added by the
    owner; getContentBounds() is the area below the title inside the padding. Optional "on" switch in the
    title row (setHeaderComponent, e.g. a ChoiceChip for oscAOn). Message thread. Owner: agent E.

    Details (agent E): the title sits in a slightly lighter band (titleBandHeight); setPowerComponent puts a
    component (the section's on switch) left of the title, setHeaderComponent one right-aligned (width
    setHeaderWidth, default 80). setActive (false) dims the title (the owner dims the controls). */
class SectionPanel : public juce::Component
{
public:
    explicit SectionPanel (const juce::String& title);
    ~SectionPanel() override;

    const juce::String& getTitle() const noexcept { return title; }

    /** Dim second part of the title ("AMP" in "ENV 1  AMP"). */
    void setSubtitle (const juce::String& subtitle);

    /** Shown right-aligned in the title row (not owned; nullptr = none). */
    void setHeaderComponent (juce::Component* component);
    void setHeaderWidth (int width);

    /** Shown left of the title, square (not owned; nullptr = none). */
    void setPowerComponent (juce::Component* component);

    void setActive (bool shouldBeActive);
    bool isActive() const noexcept { return active; }

    juce::Rectangle<int> getContentBounds() const;

    /** Height of the title band in design pixels. */
    static constexpr int titleBandHeight = 24;

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    juce::Rectangle<int> getTitleRow() const;

    juce::String title;
    juce::String subtitle;
    juce::Component* header = nullptr;
    juce::Component* power = nullptr;
    int headerWidth = 80;
    bool active = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SectionPanel)
};

} // namespace vocalsynth::ui
