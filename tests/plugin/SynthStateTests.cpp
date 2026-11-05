// SynthState: round trip with embedded user table, data once per hash, broken / missing children, stateVersion.
// Test plan: docs/SYNTH-DESIGN.md 21. Owner: agent D (Plugin data).

#include "SynthDataTestUtils.h"
#include "SynthState.h"
#include "WavetableWorker.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>
#include <random>

using vocalsynth::SynthState;
using vocalsynth::UserTableData;
using vocalsynth::WavetableReference;
using vocalsynth::WavetableStore;
using vocalsynth::WavetableWorker;
namespace testdata = vocalsynth::testdata;

namespace
{
const juce::Identifier tableType ("TABLE");

int countTables (const juce::ValueTree& tree)
{
    int n = 0;

    for (const auto& child : tree)
        if (child.hasType (tableType))
            ++n;

    return n;
}

/** Through XML text and back, as a project file does it (every property becomes a string). */
juce::ValueTree viaXml (const juce::ValueTree& tree)
{
    const auto xml = tree.createXml();
    REQUIRE (xml != nullptr);
    const auto parsed = juce::parseXML (xml->toString());
    REQUIRE (parsed != nullptr);
    return juce::ValueTree::fromXml (*parsed);
}

bool isDefaultFactory (const SynthState::Table& t)
{
    return t.reference.kind == WavetableReference::Kind::factory && t.reference.id == "basic-shapes";
}

/** The restore step of setStateInformation (docs/SYNTH-DESIGN.md 13.1, integration): one synchronous load per
    slot. Kept here so the state tests run the whole path without a processor. */
void restoreTables (const SynthState& state, WavetableWorker& worker)
{
    for (int s = 0; s < WavetableStore::numSlots; ++s)
    {
        const auto& t = state.tables[static_cast<std::size_t> (s)];
        juce::String error;

        if (t.reference.kind == WavetableReference::Kind::user)
            worker.loadEmbeddedNow (s, t.embedded, error);
        else
            worker.loadFactoryNow (s, t.reference.id, error);
    }
}

SynthState saveTables (const WavetableWorker& worker)
{
    SynthState state;

    for (int s = 0; s < WavetableStore::numSlots; ++s)
    {
        const auto info = worker.getRequestedReference (s);
        state.tables[static_cast<std::size_t> (s)] = { info.reference, info.userData };
    }

    return state;
}
} // namespace

TEST_CASE ("SynthState: defaults and layout", "[plugin][synth][state]")
{
    const SynthState state;
    CHECK (state.presetName == "Init");
    CHECK_FALSE (state.presetModified);
    CHECK (state.editorScalePercent == 100);
    CHECK (isDefaultFactory (state.tables[0]));
    CHECK (isDefaultFactory (state.tables[1]));

    const auto tree = state.toValueTree();
    CHECK (tree.hasType ("SYNTH"));
    CHECK (static_cast<int> (tree.getProperty ("version")) == SynthState::currentVersion);
    CHECK (tree.getProperty ("presetName").toString() == "Init");
    CHECK (static_cast<int> (tree.getProperty ("editorScale")) == 100);
    REQUIRE (countTables (tree) == 2);
    CHECK (tree.getChild (0).getProperty ("osc").toString() == "0");
    CHECK (tree.getChild (0).getProperty ("ref").toString() == "factory:basic-shapes");
    CHECK (tree.getChild (1).getProperty ("osc").toString() == "1");
    CHECK_FALSE (tree.getChild (1).hasProperty ("data"));

    const auto back = SynthState::fromValueTree (viaXml (tree));
    CHECK (back.presetName == "Init");
    CHECK (isDefaultFactory (back.tables[0]));
    CHECK (back.tables[0].reference.name == "Basic Shapes");
}

TEST_CASE ("SynthState: round trip with user tables, data stored once per hash", "[plugin][synth][state]")
{
    const auto data = testdata::makeUserData (5, 2, juce::String::fromUTF8 ("Gr\xc3\xbc\xc3\x9f" "e & <Table> \"1\".wav"));

    SynthState state;
    state.presetName = juce::String::fromUTF8 ("M\xc3\xb6in Pr\xc3\xa4set");
    state.presetModified = true;
    state.editorScalePercent = 150;

    SECTION ("the same table on both oscillators")
    {
        for (auto& t : state.tables)
            t = { WavetableReference::fromRefString ("user:" + data->hash, data->name), data };

        const auto tree = state.toValueTree();
        REQUIRE (countTables (tree) == 2);
        CHECK (tree.getChild (0).hasProperty ("data"));
        CHECK (static_cast<int> (tree.getChild (0).getProperty ("frames")) == 5);
        CHECK_FALSE (tree.getChild (1).hasProperty ("data")); // once per hash
        CHECK_FALSE (tree.getChild (1).hasProperty ("frames"));

        const auto back = SynthState::fromValueTree (viaXml (tree));
        CHECK (back.presetName == state.presetName);
        CHECK (back.presetModified);
        CHECK (back.editorScalePercent == 150);

        for (const auto& t : back.tables)
        {
            CHECK (t.reference == state.tables[0].reference);
            REQUIRE (t.embedded != nullptr);
            CHECK (t.embedded->pcm == data->pcm);
            CHECK (t.embedded->numFrames == 5);
            CHECK (t.embedded->hash == data->hash);
            CHECK (t.embedded->name == data->name);
        }
    }

    SECTION ("a factory and a user table; the data may come with the second TABLE")
    {
        state.tables[0] = { WavetableReference::fromRefString ("factory:fm-2op", "FM 2-Op"), nullptr };
        state.tables[1] = { WavetableReference::fromRefString ("user:" + data->hash, data->name), data };

        auto tree = state.toValueTree();
        CHECK_FALSE (tree.getChild (0).hasProperty ("data"));
        CHECK (tree.getChild (1).hasProperty ("data"));

        // Reorder: a later TABLE carries the data (hand-edited or other writer)
        auto refOnly = tree.getChild (1).createCopy();
        refOnly.removeProperty ("data", nullptr);
        refOnly.removeProperty ("frames", nullptr);
        refOnly.setProperty ("osc", 0, nullptr);
        tree.removeChild (0, nullptr);
        tree.addChild (refOnly, 0, nullptr);

        const auto back = SynthState::fromValueTree (viaXml (tree));
        REQUIRE (back.tables[0].embedded != nullptr);
        CHECK (back.tables[0].embedded->pcm == data->pcm);
        CHECK (back.tables[1].embedded->pcm == data->pcm);
    }
}

TEST_CASE ("SynthState: full path through the worker - save, restore in a new instance, save again", "[plugin][synth][state]")
{
    testdata::ScopedTempFolder temp;
    juce::ValueTree saved;
    juce::String userHash;

    {
        WavetableStore store;
        WavetableWorker worker (store);
        juce::String error;
        REQUIRE (worker.loadFactoryNow (0, "harmonic-sweep", error));
        const auto file = temp.folder.getChildFile (juce::String::fromUTF8 ("\xc3\x9c" "ber Tab.wav"));
        const auto wav = testdata::makeWav16 (testdata::morphFrames (6, 9));
        REQUIRE (file.replaceWithData (wav.getData(), wav.getSize()));
        worker.requestImport (1, file);
        REQUIRE (worker.waitUntilIdle (20000));
        userHash = store.getSlot (1).reference.id;
        REQUIRE (store.getSlot (1).reference.kind == WavetableReference::Kind::user);

        saved = viaXml (saveTables (worker).toValueTree());
    }

    // Nothing holds the user table any more: it must come back from the embedded data alone
    CHECK (WavetableStore::findShared ("user:" + userHash) == nullptr);

    WavetableStore store;
    WavetableWorker worker (store);
    restoreTables (SynthState::fromValueTree (saved), worker);

    CHECK (store.getSlot (0).reference.id == "harmonic-sweep");
    CHECK (store.getSlot (1).reference.toRefString() == "user:" + userHash);
    REQUIRE (store.acquire (1) != nullptr);
    CHECK (store.acquire (1)->getNumFrames() == 6);
    CHECK (worker.getStatus().errorSlot == -1);

    // getState right after setState: the same tree
    CHECK (viaXml (saveTables (worker).toValueTree()).isEquivalentTo (saved));
}

TEST_CASE ("SynthState: a user TABLE without data is found by hash or falls back to the default", "[plugin][synth][state]")
{
    const auto data = testdata::makeUserData (2, 21, "Held.wav");
    juce::ValueTree tree ("SYNTH");
    tree.appendChild (juce::ValueTree (tableType, { { "osc", 0 }, { "ref", "user:" + data->hash }, { "name", "Held.wav" } }), nullptr);
    tree.appendChild (juce::ValueTree (tableType, { { "osc", 1 }, { "ref", "user:00000000deadbeef" }, { "name", "Gone.wav" } }), nullptr);

    const auto state = SynthState::fromValueTree (tree);
    REQUIRE (state.tables[0].embedded != nullptr);
    CHECK (state.tables[0].embedded->pcm.empty());
    CHECK (state.tables[0].embedded->hash == data->hash);

    // Another instance holds the first table
    WavetableStore holderStore;
    WavetableWorker holder (holderStore);
    juce::String error;
    REQUIRE (holder.loadEmbeddedNow (0, data, error));

    WavetableStore store;
    WavetableWorker worker (store);
    restoreTables (state, worker);

    CHECK (store.acquire (0) == holderStore.acquire (0));
    REQUIRE (store.getSlot (0).userData != nullptr);
    CHECK (store.getSlot (0).userData->pcm == data->pcm); // the next save embeds it again
    CHECK (store.getSlot (1).reference.toRefString() == "factory:basic-shapes");
    CHECK (worker.getStatus().errorSlot == 1);
    CHECK (worker.getStatus().lastError.contains ("Gone.wav"));
}

TEST_CASE ("SynthState: missing, broken and extra content falls back safely", "[plugin][synth][state]")
{
    const auto data = testdata::makeUserData (1, 3);
    const auto good = juce::ValueTree (tableType, { { "osc", 1 }, { "ref", "factory:pwm" }, { "name", "PWM" } });

    // No / wrong tree
    for (const auto& tree : { juce::ValueTree(), juce::ValueTree ("CAPTURE"), juce::ValueTree ("SYNTH") })
    {
        const auto s = SynthState::fromValueTree (tree);
        CHECK (s.presetName == "Init");
        CHECK (isDefaultFactory (s.tables[0]));
        CHECK (isDefaultFactory (s.tables[1]));
    }

    juce::ValueTree tree ("SYNTH", { { "version", 7 }, { "presetName", "Later" }, { "presetModified", "maybe" },
                                     { "editorScale", 1000 }, { "futureThing", "x" } });
    tree.appendChild (juce::ValueTree ("UI", { { "page", 3 } }), nullptr);                         // later child
    tree.appendChild (juce::ValueTree (tableType, { { "osc", 5 }, { "ref", "factory:fold" } }), nullptr);   // bad osc
    tree.appendChild (juce::ValueTree (tableType, { { "osc", "abc" }, { "ref", "factory:fold" } }), nullptr);
    tree.appendChild (juce::ValueTree (tableType, { { "osc", 0 }, { "ref", "factory:no-such" } }), nullptr);
    tree.appendChild (good, nullptr);
    tree.appendChild (juce::ValueTree (tableType, { { "osc", 1 }, { "ref", "factory:organ" } }), nullptr); // duplicate

    auto s = SynthState::fromValueTree (tree);
    CHECK (s.presetName == "Later");
    CHECK_FALSE (s.presetModified);
    CHECK (s.editorScalePercent == 100);
    CHECK (isDefaultFactory (s.tables[0]));
    CHECK (s.tables[1].reference.id == "pwm"); // the first TABLE of an oscillator wins

    // Bad references
    for (const auto* ref : { "", "garbage", "user:", "user:xyz", "user:0123456789ABCDEFG", "factory:", "user:../../etc" })
    {
        CAPTURE (ref);
        juce::ValueTree t ("SYNTH");
        t.appendChild (juce::ValueTree (tableType, { { "osc", 0 }, { "ref", ref } }), nullptr);
        CHECK (isDefaultFactory (SynthState::fromValueTree (t).tables[0]));
    }

    // Upper-case hashes are accepted (normalised)
    {
        juce::ValueTree t ("SYNTH");
        t.appendChild (juce::ValueTree (tableType, { { "osc", 0 }, { "ref", "user:" + data->hash.toUpperCase() } }), nullptr);
        CHECK (SynthState::fromValueTree (t).tables[0].reference.id == data->hash);
    }

    // Broken data: reference kept (it may still be found by hash), data dropped
    const auto withData = [&] (const juce::var& frames, const juce::String& base64)
    {
        juce::ValueTree t ("SYNTH");
        t.appendChild (juce::ValueTree (tableType, { { "osc", 0 }, { "ref", "user:" + data->hash }, { "name", "x" },
                                                     { "frames", frames }, { "data", base64 } }), nullptr);
        return SynthState::fromValueTree (t).tables[0];
    };

    const auto encoded = SynthState::encodePcm16 (data->pcm);
    CHECK (withData (1, encoded).embedded->pcm == data->pcm);
    CHECK (withData (2, encoded).embedded->pcm.empty());       // frames do not match the data
    CHECK (withData (0, encoded).embedded->pcm.empty());
    CHECK (withData (-1, encoded).embedded->pcm.empty());
    CHECK (withData (257, encoded).embedded->pcm.empty());
    CHECK (withData ("1x", encoded).embedded->pcm.empty());
    CHECK (withData (1, encoded.substring (1)).embedded->pcm.empty());
    CHECK (withData (1, "!!!!" + encoded.substring (4)).embedded->pcm.empty());
    CHECK (withData (1, {}).embedded->pcm.empty());
    CHECK (withData (1, encoded).reference.id == data->hash);

    // Too long: rejected before decoding
    CHECK (withData (256, juce::String::repeatedString ("AAAA", 400000)).embedded->pcm.empty());
}

TEST_CASE ("SynthState: PCM16 base64", "[plugin][synth][state]")
{
    std::vector<int16_t> pcm { 0, 1, -1, 32767, -32768, 256, -256, 12345 };
    const auto text = SynthState::encodePcm16 (pcm);
    CHECK (SynthState::decodePcm16 (text) == pcm);

    // Little endian: 0x0001 -> 01 00
    CHECK (SynthState::encodePcm16 (std::vector<int16_t> { 1 }) == "AQA=");
    CHECK (SynthState::decodePcm16 ("AQA=") == std::vector<int16_t> { 1 });

    CHECK (SynthState::decodePcm16 ("").empty());
    CHECK (SynthState::decodePcm16 ("AQ==").empty());   // one byte: not PCM16
    CHECK (SynthState::decodePcm16 ("AQA").empty());    // not padded
    CHECK (SynthState::decodePcm16 ("A?A=").empty());
    CHECK (SynthState::encodePcm16 (std::vector<int16_t> {}).isEmpty());
}

TEST_CASE ("SynthState: stateVersion and removeFromState", "[plugin][synth][state]")
{
    juce::ValueTree root ("PARAMS");
    CHECK (SynthState::getStateVersion (root) == 0);
    root.setProperty (SynthState::stateVersionProperty, SynthState::currentStateVersion, nullptr);
    CHECK (SynthState::getStateVersion (root) == 4);
    CHECK (SynthState::getStateVersion (viaXml (root)) == 4);
    root.setProperty (SynthState::stateVersionProperty, "garbage", nullptr);
    CHECK (SynthState::getStateVersion (root) == 0);
    root.setProperty (SynthState::stateVersionProperty, -3, nullptr);
    CHECK (SynthState::getStateVersion (root) == 0);

    root.appendChild (juce::ValueTree ("PARAM", { { "id", "x" } }), nullptr);
    root.appendChild (SynthState().toValueTree(), nullptr);
    root.appendChild (juce::ValueTree ("CAPTURE"), nullptr);
    root.appendChild (SynthState().toValueTree(), nullptr);

    SynthState::removeFromState (root);
    CHECK (root.getNumChildren() == 2);
    CHECK_FALSE (root.getChildWithName (SynthState::treeType).isValid());
    CHECK (root.getChildWithName ("CAPTURE").isValid());
}

TEST_CASE ("SynthState: fuzzed trees and corrupted XML never crash", "[plugin][synth][state]")
{
    std::mt19937 rng (4242);
    const auto data = testdata::makeUserData (1, 8);
    SynthState state;
    state.tables[0] = { WavetableReference::fromRefString ("user:" + data->hash, "u.wav"), data };
    state.tables[1] = { WavetableReference::fromRefString ("factory:bell", "Bell"), nullptr };
    const auto validXml = state.toValueTree().createXml()->toString();

    const std::array<juce::var, 9> values { juce::var(), juce::var (0), juce::var (-1), juce::var (1.0e300),
                                            juce::var ("user:0123456789abcdef"), juce::var ("factory:pwm"),
                                            juce::var ("AQA="), juce::var (std::numeric_limits<double>::quiet_NaN()),
                                            juce::var (juce::String::repeatedString ("A", 1001)) };
    const std::array<const char*, 9> names { "osc", "ref", "name", "frames", "data", "version", "presetName",
                                             "presetModified", "editorScale" };

    for (int n = 0; n < 3000; ++n)
    {
        // Random tree
        juce::ValueTree tree (rng() % 8 == 0 ? "OTHER" : "SYNTH");

        for (int p = 0; p < 4; ++p)
            tree.setProperty (names[rng() % names.size()], values[rng() % values.size()], nullptr);

        for (int c = static_cast<int> (rng() % 5); c > 0; --c)
        {
            juce::ValueTree child (rng() % 4 == 0 ? "JUNK" : "TABLE");

            for (int p = static_cast<int> (rng() % 6); p > 0; --p)
                child.setProperty (names[rng() % names.size()], values[rng() % values.size()], nullptr);

            tree.appendChild (child, nullptr);
        }

        const auto s = SynthState::fromValueTree (tree);
        CHECK (s.editorScalePercent >= 75);
        CHECK (s.editorScalePercent <= 200);
        (void) s.toValueTree();

        // Corrupted text of a valid state
        auto text = validXml;

        for (int k = static_cast<int> (rng() % 8) + 1; k > 0; --k)
        {
            const auto pos = static_cast<int> (rng() % static_cast<unsigned> (text.length()));
            text = text.substring (0, pos) + juce::String::charToString (static_cast<juce::juce_wchar> (32 + rng() % 95))
                   + text.substring (pos + 1);
        }

        if (auto xml = juce::parseXML (text))
        {
            const auto parsed = SynthState::fromValueTree (juce::ValueTree::fromXml (*xml));

            for (const auto& t : parsed.tables)
                if (t.embedded != nullptr && ! t.embedded->pcm.empty())
                    CHECK (t.embedded->pcm.size() == static_cast<std::size_t> (t.embedded->numFrames) * 2048u);
        }

        // Random base64
        juce::String junk;

        for (int k = static_cast<int> (rng() % 40); k > 0; --k)
            junk += juce::String::charToString (static_cast<juce::juce_wchar> (32 + rng() % 95));

        (void) SynthState::decodePcm16 (junk);
    }
}
