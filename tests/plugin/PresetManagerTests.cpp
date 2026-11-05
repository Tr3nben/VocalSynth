// PresetManager: file round trip, missing / unknown IDs, never touches voice / capture / mix parameters, modified flag, user folder.
// Test plan: docs/SYNTH-DESIGN.md 21. Owner: agent D (Plugin data).

#include "Parameters.h"
#include "PluginProcessor.h"
#include "PresetManager.h"
#include "SynthDataTestUtils.h"
#include "SynthParameters.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <random>
#include <set>
#include <thread>

using vocalsynth::PresetCategory;
using vocalsynth::PresetInfo;
using vocalsynth::PresetManager;
using vocalsynth::Variant;
using vocalsynth::VocalSynthProcessor;
using vocalsynth::WavetableReference;
using vocalsynth::WavetableStore;
using vocalsynth::WavetableWorker;
namespace params = vocalsynth::params;
namespace testdata = vocalsynth::testdata;

namespace
{
/** A processor (for its APVTS and the non-synth parameters) with the plugin-data objects next to it, the way
    the integration wires them (docs/SYNTH-DESIGN.md 17), and a user folder with spaces and umlauts. */
struct Fixture
{
    testdata::ScopedTempFolder temp;
    VocalSynthProcessor processor { Variant::fx };
    WavetableStore store;
    WavetableWorker worker { store };
    PresetManager presets { processor.getParameters(), store, worker };
    juce::File folder = temp.folder.getChildFile (juce::String::fromUTF8 ("Pr\xc3\xa4sets und mehr"));

    Fixture()
    {
        presets.setUserFolder (folder);
        juce::String error;
        worker.loadFactoryNow (0, "basic-shapes", error);
        worker.loadFactoryNow (1, "basic-shapes", error);
    }

    juce::RangedAudioParameter& param (const juce::String& id)
    {
        auto* p = processor.getParameters().getParameter (id);
        REQUIRE (p != nullptr);
        return *p;
    }

    float plain (const juce::String& id)
    {
        auto& p = param (id);
        return p.convertFrom0to1 (p.getValue());
    }

    void setPlain (const juce::String& id, float value)
    {
        auto& p = param (id);
        p.setValueNotifyingHost (p.convertTo0to1 (value));
    }

    /** Every preset parameter to a random value of its range. */
    void randomisePresetParameters (std::mt19937& rng)
    {
        std::uniform_real_distribution<float> unit (0.0f, 1.0f);

        for (const auto& id : params::getSynthParameterIds())
            if (params::isPresetParameter (id))
                param (id).setValueNotifyingHost (unit (rng));
    }

    std::map<juce::String, float> allPlainValues()
    {
        std::map<juce::String, float> values;

        for (auto* p : processor.getParameters().processor.getParameters())
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (p))
                values[ranged->paramID] = ranged->convertFrom0to1 (ranged->getValue());

        return values;
    }

    PresetInfo userPreset (const juce::String& name)
    {
        for (const auto& p : presets.getPresets())
            if (! p.isFactory && p.name == name)
                return p;

        FAIL ("user preset not listed: " << name);
        return {};
    }
};

/** Records which parameters a preset load changes (through the host notification path). */
struct ChangeRecorder final : juce::AudioProcessorListener
{
    std::set<int> changed;

    void audioProcessorParameterChanged (juce::AudioProcessor*, int index, float) override { changed.insert (index); }
    void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override {}
};

void writeFile (const juce::File& file, const juce::String& text)
{
    REQUIRE (file.getParentDirectory().createDirectory().wasOk());
    REQUIRE (file.replaceWithText (text, false, false, "\n"));
}
} // namespace

TEST_CASE ("PresetManager: lists the factory presets, then the user presets", "[plugin][synth][presets]")
{
    Fixture f;
    const auto list = f.presets.getPresets();
    REQUIRE (list.size() == vocalsynth::FactoryPresets::getNumPresets());
    CHECK (list[0].name == "Init");
    CHECK (list[0].isFactory);
    CHECK (list[0].factoryIndex == 0);
    CHECK (list[0].category == PresetCategory::init);
    CHECK (f.presets.getCurrentName() == "Init");
    CHECK_FALSE (f.presets.isModified());

    CHECK (PresetManager::getDefaultUserFolder().getFileName() == "Presets");
    CHECK (PresetManager::getDefaultUserFolder().getParentDirectory().getFileName() == "VocalSynth");
    CHECK (f.presets.getUserFolder() == f.folder);
    CHECK_FALSE (f.folder.exists()); // created on the first save only
}

TEST_CASE ("PresetManager: Init sets every preset parameter to its default and the tables synchronously", "[plugin][synth][presets]")
{
    Fixture f;
    std::mt19937 rng (1);
    f.randomisePresetParameters (rng);
    juce::String error;
    REQUIRE (f.worker.loadFactoryNow (0, "pwm", error));
    REQUIRE (f.worker.loadFactoryNow (1, "fold", error));

    REQUIRE (f.presets.loadInit());

    for (const auto& id : params::getSynthParameterIds())
    {
        if (! params::isPresetParameter (id))
            continue;

        CAPTURE (id);
        auto& p = f.param (id);
        CHECK (p.getValue() == Catch::Approx (p.getDefaultValue()).margin (1.0e-6));
    }

    // Published when load() returns, no waiting
    CHECK (f.store.getSlot (0).reference.toRefString() == "factory:basic-shapes");
    CHECK (f.store.getSlot (1).reference.toRefString() == "factory:basic-shapes");
    CHECK (f.worker.getRequestedReference (0).reference.toRefString() == "factory:basic-shapes");
    CHECK (f.presets.getCurrentName() == "Init");
    CHECK_FALSE (f.presets.isModified());

    CHECK_FALSE (f.presets.loadFactory (-1));
    CHECK_FALSE (f.presets.loadFactory (vocalsynth::FactoryPresets::getNumPresets()));
}

TEST_CASE ("PresetManager: a preset never touches the mix, voice, test or capture parameters", "[plugin][synth][presets]")
{
    Fixture f;
    juce::String error;

    // Every parameter that is not a preset parameter gets a non-default value
    for (auto* p : f.processor.getParameters().processor.getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (p); ranged != nullptr && ! params::isPresetParameter (ranged->paramID))
            ranged->setValueNotifyingHost (ranged->getDefaultValue() < 0.5f ? 1.0f : 0.0f);

    std::mt19937 rng (2);
    f.randomisePresetParameters (rng);
    REQUIRE (f.presets.saveUserPreset ("Everything", PresetCategory::lead, error));

    const auto before = f.allPlainValues();
    ChangeRecorder recorder;
    f.processor.addListener (&recorder);

    REQUIRE (f.presets.loadInit());
    REQUIRE (f.presets.load (f.userPreset ("Everything")));

    f.processor.removeListener (&recorder);
    const auto after = f.allPlainValues();

    for (const auto& [id, value] : before)
    {
        if (params::isPresetParameter (id))
            continue;

        CAPTURE (id);
        CHECK (juce::exactlyEqual (after.at (id), value));
    }

    const auto& all = f.processor.getParameters().processor.getParameters();

    for (const auto index : recorder.changed)
    {
        auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (all[index]);
        REQUIRE (ranged != nullptr);
        CAPTURE (ranged->paramID);
        CHECK (params::isPresetParameter (ranged->paramID));
    }

    CHECK (recorder.changed.size() == static_cast<std::size_t> (params::numSynthParameters - 3));
}

TEST_CASE ("PresetManager: user preset file round trip with an embedded user table", "[plugin][synth][presets]")
{
    Fixture f;
    std::mt19937 rng (3);
    juce::String error;

    f.randomisePresetParameters (rng);
    const auto saved = f.allPlainValues();

    const auto data = testdata::makeUserData (4, 77, juce::String::fromUTF8 ("Sch\xc3\xb6ne Tabelle.wav"));
    REQUIRE (f.worker.loadFactoryNow (0, "formant-vowel", error));
    REQUIRE (f.worker.loadEmbeddedNow (1, data, error));

    const auto name = juce::String::fromUTF8 ("M\xc3\xa4in Lead: Test/1?");
    REQUIRE (f.presets.saveUserPreset (name, PresetCategory::vocal, error));
    CHECK (f.folder.isDirectory());
    CHECK (f.presets.getCurrentName() == name);
    CHECK_FALSE (f.presets.isModified());

    const auto file = f.presets.getUserPresetFile (name);
    CHECK (file.existsAsFile());
    CHECK (file.getFileName() == juce::String::fromUTF8 ("M\xc3\xa4in Lead Test1.vspreset"));
    CHECK (file.getParentDirectory() == f.folder);

    // The file: plain values of the 99 preset parameters, both tables, the user data embedded
    const auto xml = juce::XmlDocument::parse (file);
    REQUIRE (xml != nullptr);
    CHECK (xml->hasTagName ("VocalSynthPreset"));
    CHECK (xml->getIntAttribute ("version") == PresetManager::formatVersion);
    CHECK (xml->getStringAttribute ("name") == name);
    CHECK (xml->getStringAttribute ("category") == "Vocal");
    CHECK (xml->getNumChildElements() == params::numSynthParameters - 3 + 2);
    CHECK (xml->getChildByAttribute ("id", "synthOn") == nullptr);
    CHECK (xml->getChildByAttribute ("id", "synthVolume") == nullptr);
    CHECK (xml->getChildByAttribute ("id", "dryLevel") == nullptr);
    REQUIRE (xml->getChildByAttribute ("id", "filterCutoff") != nullptr);
    CHECK (static_cast<float> (xml->getChildByAttribute ("id", "filterCutoff")->getDoubleAttribute ("value"))
           == Catch::Approx (saved.at ("filterCutoff")).epsilon (1.0e-6));
    REQUIRE (xml->getChildByAttribute ("osc", "1") != nullptr);
    CHECK (xml->getChildByAttribute ("osc", "1")->getStringAttribute ("ref") == "user:" + data->hash);
    CHECK (xml->getChildByAttribute ("osc", "1")->hasAttribute ("data"));

    // Listed, with its category
    const auto info = f.userPreset (name);
    CHECK (info.category == PresetCategory::vocal);
    CHECK (info.file == file);
    CHECK_FALSE (info.isFactory);

    // Back to Init: nothing holds the user table any more ...
    REQUIRE (f.presets.loadInit());
    f.store.release ({}, {});
    f.store.release ({}, {});
    f.store.collectGarbage();
    CHECK (WavetableStore::findShared ("user:" + data->hash) == nullptr);

    // ... the preset brings everything back, the table from the file's data
    REQUIRE (f.presets.load (info));
    CHECK (f.presets.getCurrentName() == name);
    CHECK_FALSE (f.presets.isModified());

    for (const auto& [id, value] : saved)
    {
        if (! params::isPresetParameter (id))
            continue;

        CAPTURE (id);
        CHECK (f.plain (id) == Catch::Approx (value).epsilon (1.0e-5).margin (1.0e-5));
    }

    CHECK (f.store.getSlot (0).reference.id == "formant-vowel");
    CHECK (f.store.getSlot (1).reference.toRefString() == "user:" + data->hash);
    REQUIRE (f.store.getSlot (1).userData != nullptr);
    CHECK (f.store.getSlot (1).userData->pcm == data->pcm);
    CHECK (f.store.getSlot (1).reference.name == data->name);
    CHECK (f.worker.getStatus().errorSlot == -1);
}

TEST_CASE ("PresetManager: missing IDs get defaults, unknown IDs and bad values are ignored, values clamped", "[plugin][synth][presets]")
{
    Fixture f;
    std::mt19937 rng (4);
    f.randomisePresetParameters (rng);

    const auto file = f.folder.getChildFile ("Hand Made.vspreset");
    writeFile (file, R"(<?xml version="1.0" encoding="UTF-8"?>
<VocalSynthPreset version="9" name="Hand Made" category="Space Sounds" author="me" tags="a, b ,,c">
  <PARAM id="oscAPosition" value="35.0"/>
  <PARAM id="filterResonance" value="250"/>
  <PARAM id="glideTime" value="abc"/>
  <PARAM id="fooBar" value="1"/>
  <PARAM id="gateThreshold" value="-10"/>
  <PARAM id="synthVolume" value="3"/>
  <PARAM value="3"/>
  <TABLE osc="0" ref="factory:pwm" name="PWM"/>
  <TABLE osc="1" ref="user:0000000000000bad" name="Lost.wav"/>
  <FUTURE thing="1"/>
</VocalSynthPreset>)");

    const auto gateBefore = f.plain ("gateThreshold");
    const auto volumeBefore = f.plain ("synthVolume");
    f.presets.refresh();
    const auto info = f.userPreset ("Hand Made");
    CHECK (info.category == PresetCategory::fx); // unknown category
    CHECK (info.author == "me");
    CHECK (info.tags == juce::StringArray { "a", "b", "c" });

    REQUIRE (f.presets.load (info));
    CHECK (f.plain ("oscAPosition") == Catch::Approx (35.0f));
    CHECK (f.plain ("filterResonance") == Catch::Approx (100.0f));
    CHECK (f.plain ("glideTime") == Catch::Approx (0.0f));
    CHECK (juce::exactlyEqual (f.plain ("gateThreshold"), gateBefore));
    CHECK (juce::exactlyEqual (f.plain ("synthVolume"), volumeBefore));
    CHECK (f.param ("filterCutoff").getValue() == Catch::Approx (f.param ("filterCutoff").getDefaultValue()));
    CHECK (f.param ("mod8Amount").getValue() == Catch::Approx (f.param ("mod8Amount").getDefaultValue()));

    CHECK (f.store.getSlot (0).reference.id == "pwm");
    CHECK (f.store.getSlot (1).reference.toRefString() == "factory:basic-shapes"); // table missing -> default
    CHECK (f.worker.getStatus().errorSlot == 1);
}

TEST_CASE ("PresetManager: corrupt preset files are rejected without changing anything", "[plugin][synth][presets]")
{
    Fixture f;
    juce::String error;
    REQUIRE (f.worker.loadFactoryNow (0, "bell", error));
    f.setPlain ("oscAPosition", 12.0f);

    const auto junk = f.folder.getChildFile ("Junk.vspreset");
    const auto wrongRoot = f.folder.getChildFile ("Wrong.vspreset");
    writeFile (junk, "<<< not xml");
    writeFile (wrongRoot, "<PARAMS><PARAM id=\"oscAPosition\" value=\"90\"/></PARAMS>");
    f.presets.refresh();

    for (const auto& file : { junk, wrongRoot, f.folder.getChildFile ("Missing.vspreset") })
    {
        PresetInfo info;
        info.isFactory = false;
        info.file = file;
        info.name = file.getFileNameWithoutExtension();
        CHECK_FALSE (f.presets.load (info));
    }

    CHECK (f.plain ("oscAPosition") == Catch::Approx (12.0f));
    CHECK (f.store.getSlot (0).reference.id == "bell");
    CHECK (f.presets.getCurrentName() == "Init");

    // Listed by file name (no name attribute to read)
    const auto list = f.presets.getPresets();
    CHECK (list.size() == vocalsynth::FactoryPresets::getNumPresets() + 2);
}

TEST_CASE ("PresetManager: modified flag - snapshot compare, markModified, setCurrent", "[plugin][synth][presets]")
{
    Fixture f;
    REQUIRE (f.presets.loadInit());
    CHECK_FALSE (f.presets.isModified());

    // A preset parameter changes: modified; back to the value: not modified
    const auto old = f.plain ("filterCutoff");
    f.setPlain ("filterCutoff", 500.0f);
    CHECK (f.presets.isModified());
    f.setPlain ("filterCutoff", old);
    CHECK_FALSE (f.presets.isModified());

    // Mix and voice parameters do not count
    f.setPlain ("synthVolume", -20.0f);
    f.setPlain ("dryLevel", -30.0f);
    f.param ("synthOn").setValueNotifyingHost (1.0f);
    f.param ("gateThreshold").setValueNotifyingHost (0.9f);
    CHECK_FALSE (f.presets.isModified());

    // A table change from the UI
    f.presets.markModified();
    CHECK (f.presets.isModified());
    REQUIRE (f.presets.loadInit());
    CHECK_FALSE (f.presets.isModified());

    // A restored project: name and flag as saved, snapshot of the current values
    f.setPlain ("oscAPosition", 80.0f);
    f.presets.setCurrent ("Project Sound", true);
    CHECK (f.presets.getCurrentName() == "Project Sound");
    CHECK (f.presets.isModified());
    f.presets.setCurrent ("Project Sound", false);
    CHECK_FALSE (f.presets.isModified());
    f.setPlain ("oscAPosition", 81.0f);
    CHECK (f.presets.isModified());

    // Saving makes it clean again
    juce::String error;
    REQUIRE (f.presets.saveUserPreset ("Clean", PresetCategory::pad, error));
    CHECK_FALSE (f.presets.isModified());
    CHECK (f.presets.getCurrentName() == "Clean");
}

TEST_CASE ("PresetManager: name and modified flag from another thread while presets load", "[plugin][synth][presets]")
{
    Fixture f;
    juce::String error;
    REQUIRE (f.presets.saveUserPreset ("One", PresetCategory::lead, error));
    REQUIRE (f.presets.saveUserPreset ("Two", PresetCategory::bass, error));

    std::atomic<bool> running { true };
    std::atomic<int> reads { 0 };

    // getStateInformation / setStateInformation may run on a host thread
    std::thread host ([&]
    {
        int n = 0;

        while (running.load())
        {
            const auto name = f.presets.getCurrentName();
            const auto modified = f.presets.isModified();
            juce::ignoreUnused (name, modified);

            if (++n % 50 == 0)
                f.presets.setCurrent ("From Host", n % 100 == 0);

            reads.fetch_add (1);
        }
    });

    for (int i = 0; i < 60; ++i)
    {
        if (i % 3 == 0)
            f.presets.loadNext();
        else if (i % 3 == 1)
            f.presets.loadPrevious();
        else
            f.presets.markModified();
    }

    while (reads.load() < 100)
        std::this_thread::yield();

    running.store (false);
    host.join();
    CHECK (reads.load() >= 100);
}

TEST_CASE ("PresetManager: previous / next walk factory then user presets with wrap-around", "[plugin][synth][presets]")
{
    Fixture f;
    juce::String error;
    REQUIRE (f.presets.saveUserPreset ("B Preset", PresetCategory::lead, error));
    REQUIRE (f.presets.saveUserPreset ("A Preset", PresetCategory::lead, error));
    REQUIRE (f.presets.loadInit());

    const auto list = f.presets.getPresets();
    const auto numFactory = vocalsynth::FactoryPresets::getNumPresets();
    REQUIRE (list.size() == numFactory + 2);
    CHECK (list[numFactory].name == "A Preset"); // user presets sorted by name
    CHECK (list[numFactory + 1].name == "B Preset");

    juce::StringArray forward;

    for (int i = 0; i < list.size(); ++i)
    {
        f.presets.loadNext();
        forward.add (f.presets.getCurrentName());
    }

    CHECK (forward[forward.size() - 1] == "Init"); // wrapped around
    CHECK (forward[numFactory - 1] == "A Preset");
    CHECK (forward[numFactory] == "B Preset");

    f.presets.loadPrevious(); // Init -> last user preset
    CHECK (f.presets.getCurrentName() == "B Preset");
    f.presets.loadPrevious();
    CHECK (f.presets.getCurrentName() == "A Preset");

    // A restored project's name is found in the list
    f.presets.setCurrent ("A Preset", false);
    f.presets.loadNext();
    CHECK (f.presets.getCurrentName() == "B Preset");

    // An unknown name starts at the beginning
    f.presets.setCurrent ("Somewhere Else", false);
    f.presets.loadNext();
    CHECK (f.presets.getCurrentName() == "Init");
}

TEST_CASE ("PresetManager: Windows-safe file names", "[plugin][synth][presets]")
{
    Fixture f;
    const auto fileName = [&] (const juce::String& name) { return f.presets.getUserPresetFile (name).getFileName(); };

    CHECK (fileName ("Lead") == "Lead.vspreset");
    CHECK (fileName ("  Spaced  ") == "Spaced.vspreset");
    CHECK (fileName ("a/b\\c:d*e?f\"g<h>i|j") == "abcdefghij.vspreset");
    CHECK (fileName ("Ends with dots...") == "Ends with dots.vspreset");
    CHECK (fileName ("") == "Preset.vspreset");
    CHECK (fileName ("???") == "Preset.vspreset");
    CHECK (fileName ("CON") == "_CON.vspreset");
    CHECK (fileName ("lpt1") == "_lpt1.vspreset");
    CHECK (fileName ("Com3.old") == "_Com3.old.vspreset");
    CHECK (fileName ("Console") == "Console.vspreset");
    CHECK (fileName ("COM0") == "COM0.vspreset");
    CHECK (fileName (juce::String::fromUTF8 ("Gr\xc3\xbc\xc3\x9f" "e \xe2\x99\xab")) == juce::String::fromUTF8 ("Gr\xc3\xbc\xc3\x9f" "e \xe2\x99\xab.vspreset"));
    CHECK (fileName ("Tab\there") == "Tabhere.vspreset");
    CHECK (fileName (juce::String::repeatedString ("x", 300)).length() <= 128 + 9);
    CHECK (f.presets.getUserPresetFile ("x").getParentDirectory() == f.folder);

    juce::String error;
    CHECK_FALSE (f.presets.saveUserPreset ("   ", PresetCategory::lead, error));
    CHECK (error.isNotEmpty());
}

TEST_CASE ("PresetManager: rename and delete user presets", "[plugin][synth][presets]")
{
    Fixture f;
    juce::String error;
    REQUIRE (f.presets.saveUserPreset ("Old Name", PresetCategory::bass, error));
    REQUIRE (f.presets.saveUserPreset ("Other", PresetCategory::bass, error));
    REQUIRE (f.presets.load (f.userPreset ("Old Name")));

    const auto oldFile = f.userPreset ("Old Name").file;
    REQUIRE (f.presets.renameUserPreset (f.userPreset ("Old Name"), juce::String::fromUTF8 ("N\xc3\xa9w Name"), error));
    CHECK_FALSE (oldFile.exists());
    const auto renamed = f.userPreset (juce::String::fromUTF8 ("N\xc3\xa9w Name"));
    CHECK (renamed.file.existsAsFile());
    CHECK (renamed.category == PresetCategory::bass);
    CHECK (f.presets.getCurrentName() == juce::String::fromUTF8 ("N\xc3\xa9w Name"));

    // Clashes, factory presets, empty names
    CHECK_FALSE (f.presets.renameUserPreset (renamed, "Other", error));
    CHECK (error.contains ("exists"));
    CHECK_FALSE (f.presets.renameUserPreset (f.presets.getPresets()[0], "X", error));
    CHECK_FALSE (f.presets.renameUserPreset (renamed, "  ", error));

    REQUIRE (f.presets.deleteUserPreset (renamed, error));
    CHECK_FALSE (renamed.file.exists());
    CHECK (f.presets.getPresets().size() == vocalsynth::FactoryPresets::getNumPresets() + 1);
    CHECK_FALSE (f.presets.deleteUserPreset (renamed, error));
    CHECK_FALSE (f.presets.deleteUserPreset (f.presets.getPresets()[0], error));
}

TEST_CASE ("PresetManager: saving fails cleanly when the folder cannot be created", "[plugin][synth][presets]")
{
    Fixture f;
    const auto blocker = f.temp.folder.getChildFile ("not a folder");
    REQUIRE (blocker.replaceWithText ("file"));
    f.presets.setUserFolder (blocker.getChildFile ("Presets"));

    juce::String error;
    CHECK_FALSE (f.presets.saveUserPreset ("Nope", PresetCategory::lead, error));
    CHECK (error.isNotEmpty());
    CHECK (f.presets.getCurrentName() == "Init");
}
