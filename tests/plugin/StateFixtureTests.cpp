// State fixtures of earlier releases (docs/SYNTH-DESIGN.md 13.3, 21): tests/data/state-0.3.1-{fx,instrument}.bin
// are the getStateInformation() blobs of the 0.3.1 plugin (captured from the v0.3.1 code by the 0.4 design
// agent, with non-default voice, test, capture parameters and capture settings; the .xml next to each is the
// same state as text). Loading one must keep every saved value, give every synth parameter its default and
// leave the synth OFF, even when the instance was switched on before (new instances, decision E4).
// Owner from the implementation phase on: agent D (integration extends it with the SYNTH child).

#include "CaptureSettings.h"
#include "Parameters.h"
#include "PluginProcessor.h"
#include "SynthParameters.h"
#include "SynthState.h"
#include "WavetableWorker.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

using vocalsynth::Variant;
using vocalsynth::VocalSynthProcessor;

namespace
{
juce::File fixture (const juce::String& name)
{
    // UTF-8: the checkout path may contain umlauts (Windows)
    return juce::File (juce::String (juce::CharPointer_UTF8 (VOCALSYNTH_TEST_DATA_DIR))).getChildFile (name);
}

juce::RangedAudioParameter& getParameter (VocalSynthProcessor& p, const juce::String& id)
{
    auto* param = p.getParameters().getParameter (id);
    REQUIRE (param != nullptr);
    return *param;
}

float getPlain (VocalSynthProcessor& p, const juce::String& id)
{
    auto& param = getParameter (p, id);
    return param.convertFrom0to1 (param.getValue());
}

int countChildren (const juce::ValueTree& tree, const juce::Identifier& type)
{
    int n = 0;

    for (const auto& child : tree)
        if (child.hasType (type))
            ++n;

    return n;
}
} // namespace

TEST_CASE ("0.3.1 state fixtures load with every old value unchanged and the synth off", "[plugin][state][fixtures]")
{
    using namespace vocalsynth;

    const auto [variant, name] = GENERATE (std::pair { Variant::fx, juce::String ("state-0.3.1-fx") },
                                           std::pair { Variant::instrument, juce::String ("state-0.3.1-instrument") });
    CAPTURE (name);

    juce::MemoryBlock blob;
    REQUIRE (fixture (name + ".bin").loadFileAsData (blob));
    const auto xml = juce::AudioProcessor::getXmlFromBinary (blob.getData(), static_cast<int> (blob.getSize()));
    REQUIRE (xml != nullptr);
    const auto saved = juce::ValueTree::fromXml (*xml);
    REQUIRE (saved.hasType ("PARAMS"));
    CHECK (SynthState::getStateVersion (saved) == 0);           // 0.3.x states have no stateVersion
    CHECK (countChildren (saved, "PARAM") == 28);
    CHECK (! saved.getChildWithName (SynthState::treeType).isValid());

    // The readable copy is the same state
    const auto text = juce::parseXML (fixture (name + ".xml"));
    REQUIRE (text != nullptr);
    CHECK (juce::ValueTree::fromXml (*text).isEquivalentTo (saved));

    VocalSynthProcessor p (variant);

    // A 0.4 instance is created with the synth on (decision E4, applied by the processor constructor);
    // set it here explicitly so the test holds before and after that change.
    auto& synthOnParam = getParameter (p, params::synthOn.getParamID());
    synthOnParam.setValueNotifyingHost (1.0f);
    getParameter (p, params::filterCutoff.getParamID()).setValueNotifyingHost (0.25f);

    p.setStateInformation (blob.getData(), static_cast<int> (blob.getSize()));

    for (const auto& child : saved)
    {
        if (! child.hasType ("PARAM"))
            continue;

        const auto id = child.getProperty ("id").toString();
        CAPTURE (id);
        CHECK (getPlain (p, id) == Catch::Approx (static_cast<float> (static_cast<double> (child.getProperty ("value")))));
    }

    CHECK (synthOnParam.getValue() == 0.0f);

    // Every other synth parameter at its default (synthOn's default is on = a new instance, integration 0.4;
    // an old project gets it off from migrateState, checked above)
    for (const auto& id : params::getSynthParameterIds())
    {
        if (id == params::synthOn.getParamID())
            continue;

        CAPTURE (id);
        auto& param = getParameter (p, id);
        CHECK (param.getValue() == Catch::Approx (param.getDefaultValue()));
    }

    CHECK (p.getCaptureState() == CaptureState::fromValueTree (saved.getChildWithName (CaptureState::treeType)));

    // Saving again keeps exactly one CAPTURE child and at most one SYNTH child
    juce::MemoryBlock again;
    p.getStateInformation (again);
    const auto againXml = juce::AudioProcessor::getXmlFromBinary (again.getData(), static_cast<int> (again.getSize()));
    REQUIRE (againXml != nullptr);
    const auto againTree = juce::ValueTree::fromXml (*againXml);
    CHECK (countChildren (againTree, CaptureState::treeType) == 1);
    CHECK (countChildren (againTree, SynthState::treeType) <= 1);
    CHECK (countChildren (againTree, "PARAM") == 28 + params::numSynthParameters);
}

TEST_CASE ("0.3.1 state fixtures give the default synth state and default tables", "[plugin][state][fixtures]")
{
    using namespace vocalsynth;

    const auto name = GENERATE (juce::String ("state-0.3.1-fx"), juce::String ("state-0.3.1-instrument"));
    CAPTURE (name);

    juce::MemoryBlock blob;
    REQUIRE (fixture (name + ".bin").loadFileAsData (blob));
    const auto xml = juce::AudioProcessor::getXmlFromBinary (blob.getData(), static_cast<int> (blob.getSize()));
    REQUIRE (xml != nullptr);
    auto state = juce::ValueTree::fromXml (*xml);

    // The restore path of setStateInformation (docs/SYNTH-DESIGN.md 13.1) on a state without SYNTH
    const auto synthState = SynthState::fromValueTree (state.getChildWithName (SynthState::treeType));
    CHECK (synthState.presetName == "Init");
    CHECK_FALSE (synthState.presetModified);
    CHECK (synthState.editorScalePercent == 100);

    WavetableStore store;
    WavetableWorker worker (store);

    for (int s = 0; s < WavetableStore::numSlots; ++s)
    {
        const auto& table = synthState.tables[static_cast<std::size_t> (s)];
        CHECK (table.reference.toRefString() == "factory:basic-shapes");
        CHECK (table.embedded == nullptr);

        juce::String error;
        CHECK (worker.loadFactoryNow (s, table.reference.id, error));
        CHECK (store.getSlot (s).reference.toRefString() == "factory:basic-shapes");
        CHECK (store.acquire (s) != nullptr);
    }

    // Saving adds exactly one SYNTH child and stateVersion 4; nothing else of the old state changes
    const auto before = state.createCopy();
    SynthState::removeFromState (state);
    CHECK (state.isEquivalentTo (before));

    SynthState saved;

    for (int s = 0; s < WavetableStore::numSlots; ++s)
    {
        const auto info = worker.getRequestedReference (s);
        saved.tables[static_cast<std::size_t> (s)] = { info.reference, info.userData };
    }

    state.appendChild (saved.toValueTree(), nullptr);
    state.setProperty (SynthState::stateVersionProperty, SynthState::currentStateVersion, nullptr);
    CHECK (SynthState::getStateVersion (state) == 4);
    CHECK (countChildren (state, SynthState::treeType) == 1);
    CHECK (countChildren (state, "PARAM") == countChildren (before, "PARAM"));
    CHECK (countChildren (state, CaptureState::treeType) == 1);
    CHECK (SynthState::fromValueTree (state.getChildWithName (SynthState::treeType)).tables[1].reference.toRefString()
           == "factory:basic-shapes");
}
