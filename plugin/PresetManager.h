#pragma once

#include "FactoryPresets.h"
#include "WavetableStore.h"
#include "WavetableWorker.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>

namespace vocalsynth
{

/** A factory or user preset as listed in the preset bar. */
struct PresetInfo
{
    juce::String name;
    PresetCategory category = PresetCategory::init;
    juce::String author;
    juce::StringArray tags;
    bool isFactory = true;
    int factoryIndex = -1; ///< FactoryPresets index (factory presets)
    juce::File file;       ///< user presets
};

/** Factory + user presets (docs/SYNTH-DESIGN.md 14). File format .vspreset (XML):

        <VocalSynthPreset version="1" name="Big Lead" category="Lead" author="..." tags="voice,bright">
          <PARAM id="oscAPosition" value="35.0"/>          (plain values, params::isPresetParameter IDs only)
          <TABLE osc="0" ref="factory:basic-shapes" name="Basic Shapes"/>
          <TABLE osc="1" ref="user:<hash>" name="..." frames="64" data="(base64 PCM16 LE)"/>
        </VocalSynthPreset>

    load(): every preset parameter is set (missing IDs -> default, unknown IDs ignored, values clamped) with
    beginChangeGesture / setValueNotifyingHost / endChangeGesture per parameter on the MESSAGE thread; the
    tables go through the WavetableWorker. Never touches synthOn / synthVolume / dryLevel or any voice, test or
    capture parameter. "Modified" = a preset parameter changed after the last load / save (APVTS listener).
    User presets live in getUserFolder() (<Documents>/VocalSynth/Presets, created on the first save; any
    Unicode path); names are sanitised for the file system (juce::File::createLegalFileName).
    Message thread only. Owner: agent D. */
class PresetManager : private juce::AudioProcessorValueTreeState::Listener
{
public:
    static constexpr int formatVersion = 1;
    static inline const juce::String fileExtension { ".vspreset" };
    static inline const juce::Identifier presetTreeType { "VocalSynthPreset" };

    PresetManager (juce::AudioProcessorValueTreeState& apvts, WavetableStore& store, WavetableWorker& worker);
    ~PresetManager() override;

    /** Factory presets (FactoryPresets order) followed by the user presets (sorted by name). */
    juce::Array<PresetInfo> getPresets() const;

    /** Rescans the user folder. */
    void refresh();

    bool load (const PresetInfo& preset);
    bool loadFactory (int factoryIndex);
    bool loadInit() { return loadFactory (0); }
    void loadNext();
    void loadPrevious();

    /** Writes <user folder>/<name>.vspreset (overwrites) and makes it current. False + errorMessage on failure. */
    bool saveUserPreset (const juce::String& name, PresetCategory category, juce::String& errorMessage);

    juce::String getCurrentName() const;
    bool isModified() const noexcept { return modified.load (std::memory_order_relaxed); }

    /** SynthState restore: name and modified flag of the project. */
    void setCurrent (const juce::String& name, bool isModifiedFlag);

    static juce::File getDefaultUserFolder();
    juce::File getUserFolder() const;
    void setUserFolder (const juce::File& folder); ///< tests / settings

private:
    void parameterChanged (const juce::String& parameterID, float newValue) override;

    juce::AudioProcessorValueTreeState& apvts;
    WavetableStore& store;
    WavetableWorker& worker;
    juce::File userFolder;
    juce::String currentName { "Init" };
    std::atomic<bool> modified { false };
    juce::Array<PresetInfo> userPresets;
};

} // namespace vocalsynth
