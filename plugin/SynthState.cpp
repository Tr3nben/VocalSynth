#include "SynthState.h"

namespace vocalsynth
{

// Scaffold (docs/SYNTH-DESIGN.md 13): writes the version and preset name only, reads defaults. Agent D
// implements the tables and the base64 data.

juce::ValueTree SynthState::toValueTree() const
{
    juce::ValueTree tree (treeType);
    tree.setProperty ("version", currentVersion, nullptr);
    tree.setProperty ("presetName", presetName, nullptr);
    tree.setProperty ("presetModified", presetModified, nullptr);
    tree.setProperty ("editorScale", editorScalePercent, nullptr);
    return tree;
}

SynthState SynthState::fromValueTree (const juce::ValueTree&)
{
    return {};
}

void SynthState::removeFromState (juce::ValueTree& root)
{
    for (auto child = root.getChildWithName (treeType); child.isValid(); child = root.getChildWithName (treeType))
        root.removeChild (child, nullptr);
}

int SynthState::getStateVersion (const juce::ValueTree& root)
{
    return static_cast<int> (root.getProperty (stateVersionProperty, 0));
}

juce::String SynthState::encodePcm16 (std::span<const int16_t>)
{
    return {};
}

std::vector<int16_t> SynthState::decodePcm16 (const juce::String&)
{
    return {};
}

} // namespace vocalsynth
