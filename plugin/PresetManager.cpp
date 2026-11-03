#include "PresetManager.h"

#include "SynthParameters.h"

namespace vocalsynth
{

// Scaffold (docs/SYNTH-DESIGN.md 14): lists the factory presets, loads and saves nothing. Agent D implements.

PresetManager::PresetManager (juce::AudioProcessorValueTreeState& state, WavetableStore& s, WavetableWorker& w)
    : apvts (state),
      store (s),
      worker (w),
      userFolder (getDefaultUserFolder())
{
}

PresetManager::~PresetManager() = default;

juce::Array<PresetInfo> PresetManager::getPresets() const
{
    juce::Array<PresetInfo> result;

    for (int i = 0; i < FactoryPresets::getNumPresets(); ++i)
    {
        const auto& p = FactoryPresets::getPreset (i);
        PresetInfo info;
        info.name = juce::String (p.name.data(), p.name.size());
        info.category = p.category;
        info.author = "VocalSynth";
        info.isFactory = true;
        info.factoryIndex = i;
        result.add (info);
    }

    result.addArray (userPresets);
    return result;
}

void PresetManager::refresh() {}

bool PresetManager::load (const PresetInfo&)
{
    return false;
}

bool PresetManager::loadFactory (int)
{
    // Scaffold: the slot info is read so the members are in use; agent D sets parameters and tables here
    juce::ignoreUnused (apvts.state, store.getSlot (0), worker.getStatus());
    return false;
}

void PresetManager::loadNext() {}

void PresetManager::loadPrevious() {}

bool PresetManager::saveUserPreset (const juce::String&, PresetCategory, juce::String& errorMessage)
{
    errorMessage = "Not implemented yet";
    return false;
}

juce::String PresetManager::getCurrentName() const
{
    const std::scoped_lock lock (mutex);
    return currentName;
}

bool PresetManager::isModified() const
{
    const std::scoped_lock lock (mutex);
    return modifiedFlag;
}

void PresetManager::markModified()
{
    const std::scoped_lock lock (mutex);
    modifiedFlag = true;
}

void PresetManager::setCurrent (const juce::String& name, bool isModifiedFlag)
{
    const std::scoped_lock lock (mutex);
    currentName = name;
    modifiedFlag = isModifiedFlag;
}

juce::File PresetManager::getDefaultUserFolder()
{
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
        .getChildFile ("VocalSynth")
        .getChildFile ("Presets");
}

juce::File PresetManager::getUserFolder() const
{
    return userFolder;
}

void PresetManager::setUserFolder (const juce::File& folder)
{
    userFolder = folder;
}

} // namespace vocalsynth
