#pragma once

#include "WavetableStore.h"

#include <juce_data_structures/juce_data_structures.h>

#include <array>
#include <memory>
#include <span>
#include <vector>

namespace vocalsynth
{

/** Non-parameter synth state, saved as the child SYNTH of the APVTS state (docs/SYNTH-DESIGN.md 13):

        <SYNTH version="1" presetName="Init" presetModified="0" editorScale="100">
          <TABLE osc="0" ref="factory:basic-shapes" name="Basic Shapes"/>
          <TABLE osc="1" ref="user:0123456789abcdef" name="My Table.wav" frames="64" data="(base64 PCM16 LE)"/>
        </SYNTH>

    The data of a user table is stored once per hash (a second TABLE with the same ref carries no data).
    The root tree also gets stateVersion = currentStateVersion (4 from 0.4.0; missing = 0.3.x or older).
    Loading tolerates a missing, extra, or broken SYNTH child, missing / extra TABLEs, bad refs and bad
    data (each falls back to the default factory table); never throws. Plain value. Owner: agent D. */
struct SynthState
{
    static inline const juce::Identifier treeType { "SYNTH" };
    static constexpr int currentVersion = 1;

    static inline const juce::Identifier stateVersionProperty { "stateVersion" };
    static constexpr int currentStateVersion = 4;

    struct Table
    {
        WavetableReference reference;
        std::shared_ptr<const UserTableData> embedded; ///< user tables: the data to restore (may be null)
    };

    std::array<Table, WavetableStore::numSlots> tables;
    juce::String presetName = "Init";
    bool presetModified = false;
    int editorScalePercent = 100; ///< 75..200, last editor scale (moves to the UI child in 0.5)

    /** Child tree (version = currentVersion). */
    juce::ValueTree toValueTree() const;

    /** Reads toValueTree() output of this or an older version; invalid tree -> defaults. */
    static SynthState fromValueTree (const juce::ValueTree& tree);

    /** Removes every SYNTH child of a state root (before apvts.replaceState, like CAPTURE). */
    static void removeFromState (juce::ValueTree& root);

    /** stateVersion of a state root, 0 if missing (0.3.x or older). */
    static int getStateVersion (const juce::ValueTree& root);

    /** PCM16 little-endian <-> base64 (juce::Base64). decode returns empty on bad input. */
    static juce::String encodePcm16 (std::span<const int16_t> pcm);
    static std::vector<int16_t> decodePcm16 (const juce::String& base64);
};

} // namespace vocalsynth
