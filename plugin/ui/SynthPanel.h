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
    /** Called on the message thread after the preset bar loaded a preset (the processor calls
        updateHostDisplay (programChanged) for CLAP, docs/SYNTH-DESIGN.md 14.4). Optional. (Added by agent E.) */
    std::function<void()> onPresetLoaded {};
};

/** The "Synth" page of 0.4 (docs/SYNTH-DESIGN.md 20.6), content area 1100 x 670 design pixels at 100 %:
    preset bar + synth on / volume / dry; Osc A / Osc B (WavetableView, table menu: factory tables +
    "Import WAV..." (FileChooser, async) -> WavetableWorker, every osc parameter); Sub / Noise; Filter; Env 1-3;
    LFO 1-2; Mod matrix (8 rows: source, dest, amount); Voice (play mode, glide, amp velocity, the four
    expression meters at 30 Hz); FX (chorus, delay, reverb). Every synth parameter has exactly one control
    (Knob or ChoiceChip) whose componentID is the parameter ID. Works for both variants (same parameters).
    Message thread. Owner: agent E.

    Details (agent E):
    - Layout: rows preset bar / OSC A, OSC B, SUB + NOISE, VOICE / FILTER, ENV 1-3 / LFO 1-2, MOD MATRIX /
      CHORUS, DELAY, REVERB, MASTER (synthOn, synthVolume, dryLevel). The page is always laid out at the design
      size on an inner content component; any other panel size scales it with a transform (aspect kept,
      centred), so the panel works both inside a scaled editor content (identity) and when the editor gives it
      scaled bounds.
    - The 30 Hz timer runs while the panel is visible (setVisible (false) stops it) and reads only the message-thread APIs: store generation / getSlot (when the generation
      changed), worker status / requested references, APVTS raw values (wavetable positions, modulation rings,
      dimming), ExpressionPublisher::read; the DSP text twice per second. Nothing writes a parameter except the
      attachments of the controls.
    - Table menu: factory tables, "Import WAV..." (FileChooser; a frame-size menu only when the file is ambiguous:
      no clm chunk, longer than one cycle and not a multiple of 2048 samples), the current user table. A change
      requests the table asynchronously (WavetableWorker::requestFactory / requestImport) and calls
      PresetManager::markModified(). */
class SynthPanel : public juce::Component, private juce::Timer
{
public:
    explicit SynthPanel (const SynthPanelContext& context);
    ~SynthPanel() override;

    /** Design size of the page content at 100 %. */
    static constexpr int designWidth = 1100;
    static constexpr int designHeight = 670;

    static constexpr int timerHz = 30;

    /** The control bound to a parameter (Knob or ChoiceChip), nullptr if none (tests). */
    juce::Component* findControlFor (const juce::String& parameterId);

    void paint (juce::Graphics& g) override;
    void resized() override;
    void visibilityChanged() override;

    //==============================================================================
    // For the tests (and used by the controls)

    /** The design-size component that holds the sections (scaled by a transform when the panel is not
        at the design size). */
    juce::Component& getContent() noexcept;

    PresetBar& getPresetBar() noexcept { return presetBar; }
    WavetableView& getWavetableView (int slot);
    juce::String getTableButtonText (int slot);
    juce::String getDspText() const;

    /** Table menu of an oscillator. Item IDs: factoryTableItemBase + FactoryTable index, importItem,
        currentUserTableItem (ticked, no action). */
    enum TableMenuIds
    {
        factoryTableItemBase = 1,
        importItem = 1000,
        currentUserTableItem = 1001
    };

    juce::PopupMenu createTableMenu (int slot);
    void handleTableMenuResult (int slot, int itemId);

    /** Requests a factory table for the slot (async) and marks the preset modified; no-op if it is the table
        already requested. */
    void chooseFactoryTable (int slot, const juce::String& factoryId);

    /** Requests an import (async) and marks the preset modified. */
    void importTable (int slot, const juce::File& file, int frameSizeOverride = 0);

    /** True if the WAV's frame size cannot be decided from the file: no clm chunk and either a multiple of 2048
        sample frames longer than 2048 (64 x 256, 16 x 1024 and a 4096 single cycle look like n x 2048), or more
        than WavetableBuilder::maxSingleCycleSamples sample frames and no multiple of 2048. The import then asks
        (createFrameSizeMenu). Reads only the chunk headers. */
    static bool needsFrameSizeChoice (const juce::File& wavFile);

    /** The frame-size question: 256 / 512 / 1024 / 2048 / 4096 samples (item ID = size), 2048 ticked as the
        default (what the import picks without a choice). */
    static juce::PopupMenu createFrameSizeMenu (const juce::String& fileName);

    /** Runs one timer tick now (tests). */
    void runTimerTickForTests() { timerCallback(); }

private:
    struct Parts;

    void timerCallback() override;
    void buildControls();
    void layoutContent();
    void showTableMenu (int slot);
    void launchImport (int slot);
    void updateTables();
    void updateModulationRings();
    void updateDimming();

    SynthPanelContext context;
    PresetBar presetBar;
    std::vector<std::unique_ptr<juce::Component>> controls; ///< Knobs and ChoiceChips
    std::unique_ptr<Parts> parts;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SynthPanel)
};

} // namespace vocalsynth::ui
