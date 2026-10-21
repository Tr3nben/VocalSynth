// M3 capture parameters and non-parameter state (docs/M3-DESIGN.md 13): IDs, ranges, defaults, version
// hint 2, the layout, CaptureState round trip (value tree and get/setStateInformation, one CAPTURE child
// after repeated save/load, never takes), old states load with defaults, state calls from another thread.

#include "CaptureSettings.h"
#include "Parameters.h"
#include "PluginProcessor.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <atomic>
#include <thread>

using vocalsynth::Variant;
using vocalsynth::VocalSynthProcessor;

namespace
{
juce::RangedAudioParameter& getParameter (VocalSynthProcessor& p, const juce::ParameterID& id)
{
    auto* param = p.getParameters().getParameter (id.getParamID());
    REQUIRE (param != nullptr);
    return *param;
}
} // namespace

TEST_CASE ("M3 capture parameters exist with the design IDs, ranges and defaults", "[plugin][parameters][capture]")
{
    using namespace vocalsynth::params;
    VocalSynthProcessor p (GENERATE (Variant::fx, Variant::instrument));

    for (const auto* id : { &captureEnabled, &captureAudio, &captureOffsetMode, &captureOffsetMs, &captureFineOffsetMs })
        CHECK (id->getVersionHint() == 2); // after the released M1 / M2 parameters (hint 1)

    auto* enabled = dynamic_cast<juce::AudioParameterBool*> (&getParameter (p, captureEnabled));
    REQUIRE (enabled != nullptr);
    CHECK (enabled->get());

    auto* audio = dynamic_cast<juce::AudioParameterBool*> (&getParameter (p, captureAudio));
    REQUIRE (audio != nullptr);
    CHECK (audio->get());

    auto* mode = dynamic_cast<juce::AudioParameterChoice*> (&getParameter (p, captureOffsetMode));
    REQUIRE (mode != nullptr);
    REQUIRE (mode->choices.size() == 3);
    CHECK (mode->choices[0] == "Auto");
    CHECK (mode->choices[1] == "Manual");
    CHECK (mode->choices[2] == "Off");
    CHECK (mode->getIndex() == 0);

    auto* offset = dynamic_cast<juce::AudioParameterFloat*> (&getParameter (p, captureOffsetMs));
    REQUIRE (offset != nullptr);
    CHECK (offset->range.start == Catch::Approx (0.0f));
    CHECK (offset->range.end == Catch::Approx (300.0f));
    CHECK (offset->get() == Catch::Approx (0.0f));
    CHECK (offset->getLabel() == "ms");

    auto* fine = dynamic_cast<juce::AudioParameterFloat*> (&getParameter (p, captureFineOffsetMs));
    REQUIRE (fine != nullptr);
    CHECK (fine->range.start == Catch::Approx (-100.0f));
    CHECK (fine->range.end == Catch::Approx (100.0f));
    CHECK (fine->get() == Catch::Approx (0.0f));
}

TEST_CASE ("CaptureState defaults follow the decisions", "[plugin][capture][state]")
{
    const vocalsynth::CaptureState state;
    CHECK (state.render.version == vocalsynth::ExportVersion::clean);
    CHECK (state.render.quantize == vocalsynth::QuantizeGrid::off);
    CHECK (state.render.bends == vocalsynth::BendExport::asPlugin);
    CHECK (state.takeGapSeconds == Catch::Approx (3.0));
    CHECK (state.audioHistoryMinutes == Catch::Approx (10.0));
    CHECK (state.grabBars == 8);
    CHECK (state.grabSeconds == 30);
    CHECK (state.toValueTree().hasType (vocalsynth::CaptureState::treeType));
}

TEST_CASE ("The parameter layout has the M1, M2 and M3 parameters with unique IDs", "[plugin][parameters][capture]")
{
    VocalSynthProcessor p (Variant::fx);
    const auto& parameters = p.getParameters().processor.getParameters();
    juce::StringArray ids;

    for (auto* parameter : parameters)
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (parameter))
            ids.add (ranged->paramID);

    // 7 M1 + 13 M2 + 5 M3 + 3 M3.1 (RecordModeTests.cpp), appended in that order (IDs are permanent)
    CHECK (ids.size() == 28);
    CHECK (parameters.size() == 28);
    auto unique = ids;
    unique.removeDuplicates (false);
    CHECK (unique.size() == ids.size());
    CHECK (ids.indexOf ("captureEnabled") == 20);
    CHECK (ids.indexOf ("captureFineOffsetMs") == 24);
    CHECK (ids.indexOf ("legato") == 19);
}

namespace
{
vocalsynth::CaptureState nonDefaultState()
{
    vocalsynth::CaptureState s;
    s.render.version = vocalsynth::ExportVersion::raw;
    s.render.quantize = vocalsynth::QuantizeGrid::sixteenthTriplet;
    s.render.quantizeStrengthPercent = 50.0;
    s.render.quantizeEnds = true;
    s.render.bends = vocalsynth::BendExport::off;
    s.render.controlChanges = false;
    s.render.cleanMinNoteMs = 120.0;
    s.render.freePreRollMs = 250.0;
    s.smfFormat = vocalsynth::SmfFormat::type0;
    s.wavFormat = vocalsynth::WavSampleFormat::float32;
    s.grabBars = 16;
    s.grabSeconds = 60;
    s.takeGapSeconds = 5.5;
    s.audioHistoryMinutes = 20.0;
    s.takesFolder = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("VS Takes").getFullPathName();
    return s;
}

void setPlain (VocalSynthProcessor& p, const juce::ParameterID& id, float value)
{
    auto& param = getParameter (p, id);
    param.setValueNotifyingHost (param.convertTo0to1 (value));
}

float getPlain (VocalSynthProcessor& p, const juce::ParameterID& id)
{
    auto& param = getParameter (p, id);
    return param.convertFrom0to1 (param.getValue());
}
} // namespace

TEST_CASE ("CaptureState round-trips through its value tree; invalid values are clamped", "[plugin][capture][state]")
{
    const auto s = nonDefaultState();
    const auto tree = s.toValueTree();
    CHECK (tree.hasType (vocalsynth::CaptureState::treeType));
    CHECK ((int) tree.getProperty ("version") == vocalsynth::CaptureState::currentVersion);
    CHECK (vocalsynth::CaptureState::fromValueTree (tree) == s);
    CHECK_FALSE (vocalsynth::CaptureState::fromValueTree (tree) == vocalsynth::CaptureState {});

    // Old projects (no child) and foreign trees: the defaults
    CHECK (vocalsynth::CaptureState::fromValueTree ({}) == vocalsynth::CaptureState {});
    CHECK (vocalsynth::CaptureState::fromValueTree (juce::ValueTree ("OTHER")) == vocalsynth::CaptureState {});

    // Missing properties: defaults; out of range: clamped; grab lengths snap to the nearest choice
    juce::ValueTree odd (vocalsynth::CaptureState::treeType);
    odd.setProperty ("version", 1, nullptr);
    odd.setProperty ("quantize", 99, nullptr);
    odd.setProperty ("quantizeStrength", 250.0, nullptr);
    odd.setProperty ("grabBars", 5, nullptr);
    odd.setProperty ("grabSeconds", 1000, nullptr);
    odd.setProperty ("takeGapSeconds", 0.01, nullptr);
    odd.setProperty ("audioHistoryMinutes", 600.0, nullptr);
    odd.setProperty ("exportVersion", -3, nullptr);
    const auto r = vocalsynth::CaptureState::fromValueTree (odd);
    CHECK (r.render.quantize == vocalsynth::QuantizeGrid::thirtySecond);
    CHECK (r.render.quantizeStrengthPercent == Catch::Approx (100.0));
    CHECK (r.render.version == vocalsynth::ExportVersion::raw);
    CHECK (r.grabBars == 4);
    CHECK (r.grabSeconds == 120);
    CHECK (r.takeGapSeconds == Catch::Approx (vocalsynth::TakeSegmenter::minTakeGapSeconds));
    CHECK (r.audioHistoryMinutes == Catch::Approx (vocalsynth::CaptureHistory::maxAudioMinutes));
    CHECK (r.render.bends == vocalsynth::BendExport::asPlugin); // missing
    CHECK (r.render.controlChanges);
}

TEST_CASE ("State round trip: capture parameters and settings, one CAPTURE child, no takes", "[plugin][capture][state]")
{
    using namespace vocalsynth::params;
    const auto variant = GENERATE (Variant::fx, Variant::instrument);

    juce::MemoryBlock state;
    {
        VocalSynthProcessor p (variant);
        setPlain (p, captureEnabled, 0.0f);
        setPlain (p, captureAudio, 0.0f);
        setPlain (p, captureOffsetMode, 1.0f);
        setPlain (p, captureOffsetMs, 123.4f);
        setPlain (p, captureFineOffsetMs, -12.5f);
        p.setCaptureState (nonDefaultState());
        p.getStateInformation (state);
    }

    VocalSynthProcessor restored (variant);
    restored.setStateInformation (state.getData(), (int) state.getSize());
    CHECK (getPlain (restored, captureEnabled) == 0.0f);
    CHECK (getPlain (restored, captureAudio) == 0.0f);
    CHECK (getPlain (restored, captureOffsetMode) == 1.0f);
    CHECK (getPlain (restored, captureOffsetMs) == Catch::Approx (123.4f).margin (1.0e-3));
    CHECK (getPlain (restored, captureFineOffsetMs) == Catch::Approx (-12.5f).margin (1.0e-3));
    CHECK (restored.getCaptureState() == nonDefaultState());

    const auto offset = restored.getCaptureOffsetSettings();
    CHECK (offset.mode == vocalsynth::CaptureOffsetMode::manual);
    CHECK (offset.offsetMs == Catch::Approx (123.4).margin (1.0e-3));

    // The worker got the non-parameter options
    const auto options = restored.getCaptureWorker().getOptions();
    CHECK (options.takeGapSeconds == Catch::Approx (5.5));
    CHECK (options.audioHistoryMinutes == Catch::Approx (20.0));
    CHECK (options.wavFormat == vocalsynth::WavSampleFormat::float32);
    CHECK (restored.getTakesFolder().getFileName() == "VS Takes");

    // Repeated save / load keeps exactly one CAPTURE child; the state holds parameters and settings only
    juce::MemoryBlock again = state;

    for (int i = 0; i < 4; ++i)
    {
        restored.setStateInformation (again.getData(), (int) again.getSize());
        again.reset();
        restored.getStateInformation (again);
    }

    const auto xml = juce::AudioProcessor::getXmlFromBinary (again.getData(), (int) again.getSize());
    REQUIRE (xml != nullptr);
    CHECK (xml->getNumChildElements() == 29); // 28 PARAM + 1 CAPTURE
    int captureChildren = 0;

    for (auto* child : xml->getChildIterator())
    {
        if (child->hasTagName ("CAPTURE"))
            ++captureChildren;
        else
            CHECK (child->hasTagName ("PARAM"));
    }

    CHECK (captureChildren == 1);
    CHECK (again.getSize() < 8192); // never take data
}

TEST_CASE ("A state without capture settings loads the capture defaults", "[plugin][capture][state]")
{
    using namespace vocalsynth::params;

    // An M2 state (0.2.x): PARAMS without the M3 parameters and without CAPTURE
    juce::MemoryBlock m2State;
    {
        VocalSynthProcessor p (Variant::fx);
        auto tree = p.getParameters().copyState();

        for (const auto* id : { &captureEnabled, &captureAudio, &captureOffsetMode, &captureOffsetMs, &captureFineOffsetMs })
            tree.removeChild (tree.getChildWithProperty ("id", id->getParamID()), nullptr);

        auto xml = tree.createXml();
        REQUIRE (xml != nullptr);
        juce::AudioProcessor::copyXmlToBinary (*xml, m2State);
    }

    VocalSynthProcessor p (Variant::fx);
    setPlain (p, captureEnabled, 0.0f);
    setPlain (p, captureOffsetMode, 2.0f);
    p.setCaptureState (nonDefaultState());

    p.setStateInformation (m2State.getData(), (int) m2State.getSize());
    CHECK (getPlain (p, captureEnabled) == 1.0f);
    CHECK (getPlain (p, captureAudio) == 1.0f);
    CHECK (getPlain (p, captureOffsetMode) == 0.0f);
    CHECK (getPlain (p, captureOffsetMs) == 0.0f);
    CHECK (getPlain (p, captureFineOffsetMs) == 0.0f);
    CHECK (p.getCaptureState() == vocalsynth::CaptureState {});
}

TEST_CASE ("State calls from a background thread while the UI changes the capture settings", "[plugin][capture][state]")
{
    VocalSynthProcessor p (Variant::fx);
    p.prepareToPlay (48000.0, 512);
    std::atomic<bool> done { false };

    std::thread host ([&]
                      {
                          juce::MemoryBlock block;

                          for (int i = 0; i < 200; ++i)
                          {
                              block.reset();
                              p.getStateInformation (block);
                              p.setStateInformation (block.getData(), (int) block.getSize());
                          }

                          done = true;
                      });

    for (int i = 0; ! done; ++i)
    {
        auto s = p.getCaptureState();
        s.grabBars = vocalsynth::TakeSegmenter::grabBarChoices[(std::size_t) i % 6];
        p.setCaptureState (s);
    }

    host.join();
    juce::MemoryBlock block;
    p.getStateInformation (block);
    const auto xml = juce::AudioProcessor::getXmlFromBinary (block.getData(), (int) block.getSize());
    REQUIRE (xml != nullptr);
    CHECK (xml->getChildByName ("CAPTURE") != nullptr);
    p.releaseResources();
}
