#include "CaptureSettings.h"

namespace vocalsynth
{

// TODO(C): implement (docs/M3-DESIGN.md 13). Stubs: only the version is written, defaults are read.

juce::ValueTree CaptureState::toValueTree() const
{
    juce::ValueTree tree (treeType);
    tree.setProperty ("version", currentVersion, nullptr);
    return tree;
}

CaptureState CaptureState::fromValueTree (const juce::ValueTree&)
{
    return {};
}

} // namespace vocalsynth
