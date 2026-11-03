#pragma once

#include "PresetManager.h"
#include "WavetableStore.h"
#include "WavetableWorker.h"
#include "ui/Knob.h"
#include "ui/ChoiceChip.h"
#include "ui/PresetBar.h"
#include "ui/SectionPanel.h"
#include "ui/WavetableView.h"
#include "vocalsynth/ExpressionFrame.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <vector>

namespace vocalsynth::ui
{

/** Everything the synth page needs; the processor owns all of it (docs/SYNTH-DESIGN.md 17, 20.6). */
struct SynthPanelContext
{
    juce::AudioProcessorValueTreeState& apvts;
    PresetManager& presets;
    WavetableStore& store;
    WavetableWorker& worker;
    const ExpressionPublisher& expression;
    /** Text of the top row's DSP display (the processor's synth load, formatted by the editor); empty
        function = no display. Message thread. */
    std::function<juce::String()> dspLoadText {};
};

/** The "Synth" page of 0.4 (docs/SYNTH-DESIGN.md 20.6), content area 1100 x 670 design pixels at 100 %:
    preset bar + synth on / volume / dry; Osc A / Osc B (WavetableView, table menu: factory tables +
    "Import WAV..." (FileChooser, async) -> WavetableWorker, every osc parameter); Sub / Noise; Filter; Env 1-3;
    LFO 1-2; Mod matrix (8 rows: source, dest, amount); Voice (play mode, glide, amp velocity, the four
    expression meters at 30 Hz); FX (chorus, delay, reverb). Every synth parameter has exactly one control
    (Knob or ChoiceChip) whose componentID is the parameter ID. Works for both variants (same parameters).
    Message thread. Owner: agent E. */
class SynthPanel : public juce::Component, private juce::Timer
{
public:
    explicit SynthPanel (const SynthPanelContext& context);
    ~SynthPanel() override;

    /** Design size of the page content at 100 %. */
    static constexpr int designWidth = 1100;
    static constexpr int designHeight = 670;

    /** The control bound to a parameter (Knob or ChoiceChip), nullptr if none (tests). */
    juce::Component* findControlFor (const juce::String& parameterId);

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;

    SynthPanelContext context;
    PresetBar presetBar;
    std::vector<std::unique_ptr<juce::Component>> controls; ///< Knobs and ChoiceChips

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SynthPanel)
};

} // namespace vocalsynth::ui
