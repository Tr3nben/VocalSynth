#pragma once

#include "FactoryPresets.h"
#include "WavetableStore.h"
#include "WavetableWorker.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <mutex>
#include <vector>

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
    capture parameter. "Modified" = a preset parameter's plain value differs from the snapshot taken at the
    last load / save / setCurrent (compared against the APVTS raw atomics when isModified() is called; NO
    APVTS listener: a listener runs on the audio thread during host automation, and JUCE's ListenerList may
    allocate there).
    User presets live in getUserFolder() (<Documents>/VocalSynth/Presets, created on the first save; any
    Unicode path); names are sanitised for the file system (juce::File::createLegalFileName).
    Tables: loaded SYNCHRONOUSLY (WavetableWorker::loadFactoryNow / loadEmbeddedNow) so the preset is
    complete when load() returns. Threads: message thread, except getCurrentName(), isModified() and
    setCurrent(), which getStateInformation / setStateInformation may call from any non-audio thread
    (internal mutex). Owner: agent D. */
class PresetManager
{
public:
    static constexpr int formatVersion = 1;
    static inline const juce::String fileExtension { ".vspreset" };
    static inline const juce::Identifier presetTreeType { "VocalSynthPreset" };

    PresetManager (juce::AudioProcessorValueTreeState& apvts, WavetableStore& store, WavetableWorker& worker);
    ~PresetManager();

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

    /** The file saveUserPreset (name) writes: a Windows-safe file name (juce::File::createLegalFileName, no
        trailing dots / spaces, no reserved device name such as CON or LPT1) + fileExtension. The UI asks
        before overwriting when it exists. Message thread. (Added by agent D for rename / delete.) */
    juce::File getUserPresetFile (const juce::String& name) const;

    /** Renames a user preset: its name attribute and its file. False + errorMessage for a factory preset, an
        empty name, a missing file or an existing other file with the new name. Message thread. */
    bool renameUserPreset (const PresetInfo& preset, const juce::String& newName, juce::String& errorMessage);

    /** Deletes a user preset file. False + errorMessage for a factory preset or when the file cannot be
        deleted. The current name stays (the parameters are unchanged). Message thread. */
    bool deleteUserPreset (const PresetInfo& preset, juce::String& errorMessage);

    juce::String getCurrentName() const;
    /** True if a preset parameter differs from the snapshot (see class comment) or setCurrent() said so. */
    bool isModified() const;

    /** A change that is not a parameter (the UI picked or imported a table) makes the preset modified. */
    void markModified();

    /** SynthState restore (after apvts.replaceState): name and modified flag of the project; takes the
        snapshot of the current values. */
    void setCurrent (const juce::String& name, bool isModifiedFlag);

    static juce::File getDefaultUserFolder();
    juce::File getUserFolder() const;
    void setUserFolder (const juce::File& folder); ///< tests / settings

private:
    /** Parameters + tables of a preset to apply (factory data or a parsed file). */
    struct PresetData;

    bool apply (const PresetData& data);
    void takeSnapshotLocked();
    std::vector<float> readPresetValues() const;
    int findCurrentIndex (const juce::Array<PresetInfo>& presets) const;
    void scanIfNeeded() const;

    juce::AudioProcessorValueTreeState& apvts;
    WavetableStore& store;
    WavetableWorker& worker;
    juce::File userFolder;

    /** The params::isPresetParameter parameters in layout order (constructor; immutable afterwards). */
    std::vector<juce::RangedAudioParameter*> presetParameters;
    std::vector<std::atomic<float>*> presetRaw;

    mutable std::mutex mutex;          ///< guards currentName, modifiedFlag, snapshot (never the audio thread)
    juce::String currentName { "Init" };
    bool modifiedFlag = false;         ///< "modified" as restored from a project (setCurrent)
    std::vector<float> snapshot;       ///< plain values of the preset parameters at the last load / save
    bool currentIsFactory = true;      ///< identity of the current preset for prev / next (with the name)
    int currentFactoryIndex = 0;
    juce::File currentFile;

    // Message thread only
    mutable juce::Array<PresetInfo> userPresets;
    mutable bool scanned = false;      ///< the user folder is read on the first getPresets(), not in the constructor
};

} // namespace vocalsynth
