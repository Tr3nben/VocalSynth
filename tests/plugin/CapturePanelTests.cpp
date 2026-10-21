// CapturePanel, the "Capture" tab (docs/M3-DESIGN.md 14, 16): status texts (synced / free / off, overflow only
// when non-zero), take rows newest first, repaint only on change, drag handles available / preparing, buttons
// send the tap requests, Clear asks first, export options persist in CaptureState, layout. The editor page
// switch incl. the Capture page is in PluginEditorTests.cpp.

#include "CaptureExporter.h"
#include "CapturePanel.h"
#include "CaptureTestUtils.h"
#include "Parameters.h"
#include "PluginProcessor.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

using namespace vocalsynth;

namespace
{
constexpr double rate = 48000.0;

juce::String enDash()
{
    return juce::String (juce::CharPointer_UTF8 ("\xe2\x80\x93"));
}

void setPlain (VocalSynthProcessor& p, const juce::ParameterID& id, float value)
{
    auto* param = p.getParameters().getParameter (id.getParamID());
    REQUIRE (param != nullptr);
    param->setValueNotifyingHost (param->convertTo0to1 (value));
}

/** A processor in Test Pattern mode with a manually stepped worker, driven by a CaptureHost. */
struct Rig
{
    explicit Rig (Variant variant = Variant::fx)
        : p (variant)
    {
        p.getCaptureWorker().setManualSteppingForTests (true);
        setPlain (p, params::source, (float) static_cast<int> (params::Source::testPattern));
        setPlain (p, params::testPattern, (float) static_cast<int> (TestPattern::notes));

        auto s = p.getCaptureState();
        s.takeGapSeconds = TakeSegmenter::minTakeGapSeconds;
        s.takesFolder = takesFolder.getFullPathName();
        p.setCaptureState (s);

        host = std::make_unique<test::CaptureHost> (p, p.getCaptureWorker(), rate, 512);
        host->input = [] (int64_t pos)
        { return 0.2f * (float) std::sin (juce::MathConstants<double>::twoPi * 220.0 * (double) pos / rate); };
    }

    ~Rig()
    {
        host.reset();
        takesFolder.deleteRecursively();
    }

    /** Free take: the pattern plays for seconds, then stops; the gap (0.5 s) closes the take. */
    void freeTake (double seconds)
    {
        setPlain (p, params::testPattern, (float) static_cast<int> (TestPattern::notes));
        host->runTo (host->position + (int64_t) (seconds * rate), 512);
        setPlain (p, params::testPattern, (float) static_cast<int> (TestPattern::off));
        host->runTo (host->position + (int64_t) (1.5 * rate), 512);
        host->steps (1);
    }

    void stepWavs()
    {
        for (int i = 0; i < 100; ++i)
            p.getCaptureWorker().runStepForTests();
    }

    juce::File takesFolder = juce::File::createTempFile ("vs-panel-takes");
    VocalSynthProcessor p;
    std::unique_ptr<test::CaptureHost> host;
};

template <typename T>
T* child (juce::Component& panel, const juce::String& id)
{
    return dynamic_cast<T*> (panel.findChildWithID (id));
}
} // namespace

TEST_CASE ("CapturePanel status texts", "[plugin][capturepanel]")
{
    CaptureStatus s;
    CHECK (CapturePanel::formatStatus (s, false, 0) == "Capture off");
    CHECK (CapturePanel::formatStatus (s, true, 0) == "Capture on " + enDash() + " waiting for audio");

    s.started = true;
    s.state = TakeSegmenter::State::freeWaiting;
    CHECK (CapturePanel::formatStatus (s, true, 0) == "Free " + enDash() + " waiting for a note");
    CHECK (CapturePanel::formatStatus (s, false, 0) == "Capture off");

    s.state = TakeSegmenter::State::freeRecording;
    s.recordingTakeNumber = 4;
    CHECK (CapturePanel::formatStatus (s, true, 0) == "Free " + enDash() + " recording take 4");

    s.state = TakeSegmenter::State::synced;
    s.bpm = 128.0;
    s.recordingTakeNumber = 3;
    s.recordingBar = 17;
    CHECK (CapturePanel::formatStatus (s, true, 0) == "Synced 128.0 BPM 4/4 " + enDash() + " recording take 3 (bar 17)");
    CHECK (CapturePanel::formatStatus (s, true, 12).endsWith ("(bar 17)" + juce::String (juce::CharPointer_UTF8 (" \xc2\xb7 ")) + "12 notes"));
    CHECK_FALSE (CapturePanel::formatStatus (s, true, 0).contains ("overflow"));

    s.overflow.records = 3;
    CHECK (CapturePanel::formatStatus (s, true, 0).contains ("overflow: 3 records, 0 audio samples"));

    s.audioHistoryBytes = 3 * 1024 * 1024;
    s.takeBytes = 1024 * 1024 / 2;
    s.numTakes = 5;
    CHECK (CapturePanel::formatMemory (s).startsWith ("Audio history 3.0 MB"));
    CHECK (CapturePanel::formatMemory (s).endsWith ("takes 0.5 MB (5)"));

    CHECK (CapturePanel::grabButtonText (true, 8, 30) == "Grab last 8 bars");
    CHECK (CapturePanel::grabButtonText (true, 1, 30) == "Grab last 1 bar");
    CHECK (CapturePanel::grabButtonText (false, 8, 30) == "Grab last 30 s");
}

TEST_CASE ("CapturePanel row texts", "[plugin][capturepanel]")
{
    const auto dot = juce::String (juce::CharPointer_UTF8 (" \xc2\xb7 "));

    Take free;
    free.number = 4;
    free.mode = TakeMode::free;
    free.sampleRate = rate;
    free.startSample = 0;
    free.endSample = (int64_t) (7.5 * rate);
    free.wallClockStartMs = juce::Time (2026, 9, 17, 14, 3, 22, 0, true).toMilliseconds();
    RenderedTake r;
    r.noteCount = 5;
    CHECK (CapturePanel::describeTake (free, r) == "Take 4" + dot + "14:03:22" + dot + "7.5 s" + dot + "5 notes");

    Take synced = free;
    synced.number = 3;
    synced.mode = TakeMode::synced;
    synced.kind = TakeKind::grab;
    synced.incomplete = true;
    synced.audio = TakeAudio::truncated;
    r.originBar = 17;
    r.endTick = 4 * 4 * 960;
    r.noteCount = 1;
    CHECK (CapturePanel::describeTake (synced, r)
           == "Take 3" + dot + "Bar 17" + dot + "4 bars" + dot + "1 note" + dot + "Grab" + dot + "incomplete" + dot
                  + "audio truncated");
}

TEST_CASE ("CapturePanel lists takes newest first and repaints only on change", "[plugin][capturepanel]")
{
    Rig rig;
    CapturePanel panel (rig.p);
    panel.setSize (908, 350);
    panel.runTimerTickForTests();

    CHECK (panel.getNumTakeRows() == 0);
    CHECK (panel.getSelectedTakeNumber() == 0);
    CHECK_FALSE (panel.getMidiDragSource().isAvailable());
    CHECK (panel.getMidiDragSource().getStatusText() == "no take yet");
    CHECK (panel.getStatusText() == "Capture on " + enDash() + " waiting for audio"); // no block yet
    CHECK (panel.getHintText() == "Takes are not saved with the project " + enDash() + " drag or save what you want to keep.");
    CHECK (panel.getAutoOffsetText().startsWith ("Auto = 21.3 ms (2"));

    setPlain (rig.p, params::testPattern, (float) static_cast<int> (TestPattern::off));
    rig.host->runTo (512, 512);
    rig.host->steps (1);
    panel.runTimerTickForTests();
    CHECK (panel.getStatusText() == "Free " + enDash() + " waiting for a note");

    // No worker steps while singing: the step that publishes the take also starts its WAV (> 1 slice)
    rig.host->stepEverySamples = int64_t { 1 } << 40;
    rig.freeTake (1.0);
    rig.host->stepEverySamples = 2048;
    panel.runTimerTickForTests();
    REQUIRE (panel.getNumTakeRows() == 1);
    CHECK (panel.getSelectedTakeNumber() == 1); // a new take is selected
    CHECK (panel.getRowText (0).startsWith ("Take 1"));
    CHECK (panel.getMidiDragSource().isAvailable());
    CHECK_FALSE (panel.getWavDragSource().isAvailable());
    CHECK (panel.getWavDragSource().getStatusText() == "preparing...");

    // Idle ticks: nothing to repaint
    const auto repaints = panel.getContentRepaintCount();
    panel.runTimerTickForTests();
    panel.runTimerTickForTests();
    CHECK (panel.getContentRepaintCount() == repaints);

    // The WAV gets ready
    rig.stepWavs();
    panel.runTimerTickForTests();
    CHECK (panel.getWavDragSource().isAvailable());

    // A second take goes on top and is selected (the newest one was selected)
    rig.freeTake (1.0);
    panel.runTimerTickForTests();
    REQUIRE (panel.getNumTakeRows() == 2);
    CHECK (panel.getRowText (0).startsWith ("Take 2"));
    CHECK (panel.getRowText (1).startsWith ("Take 1"));
    CHECK (panel.getSelectedTakeNumber() == 2);
    CHECK (panel.getContentRepaintCount() > repaints);

    // An older selection stays when a new take arrives
    panel.selectTake (1);
    CHECK (panel.getSelectedTakeNumber() == 1);
    rig.freeTake (1.0);
    panel.runTimerTickForTests();
    CHECK (panel.getNumTakeRows() == 3);
    CHECK (panel.getSelectedTakeNumber() == 1);

    // Painting must not throw or assert
    const auto image = panel.createComponentSnapshot (panel.getLocalBounds());
    CHECK (image.getWidth() == 908);
}

TEST_CASE ("CapturePanel drag actions write the MIDI and persist the WAV", "[plugin][capturepanel][file]")
{
    Rig rig;
    CapturePanel panel (rig.p);
    rig.freeTake (1.5);
    rig.stepWavs();
    panel.runTimerTickForTests();
    REQUIRE (panel.getSelectedTakeNumber() == 1);

    const auto midi = panel.exportSelectedMidi();
    REQUIRE (midi.existsAsFile());
    CHECK (midi.getParentDirectory() == rig.p.getCaptureWorker().getInstanceFolder());
    CHECK (midi.getFileName().endsWith ("_Take01_Clean.mid")); // free take: no bar
    juce::MidiFile midiFile;
    REQUIRE (test::readMidiFile (midi, midiFile));
    CHECK (! test::notesOf (midiFile).empty());

    const auto wav = panel.exportSelectedWav();
    REQUIRE (wav.existsAsFile());
    CHECK (wav.getParentDirectory() == rig.takesFolder);
    CHECK (wav.getFileName().endsWith ("_Take01_Voice.wav"));
    CHECK (panel.exportSelectedWav() == wav); // the second drag reuses it
    panel.runTimerTickForTests();
    CHECK (panel.getWavDragSource().getStatusText() == "saved in Takes folder");

    // The options apply to the next drag: Raw, written as a new file
    auto* version = child<juce::ComboBox> (panel, "version");
    REQUIRE (version != nullptr);
    version->setSelectedItemIndex (0, juce::sendNotificationSync);
    CHECK (rig.p.getCaptureState().render.version == ExportVersion::raw);
    CHECK (panel.exportSelectedMidi().getFileName().endsWith ("_Take01_Raw.mid"));

    // Destroying the processor deletes the instance folder, the persisted WAV stays
    const auto instance = rig.p.getCaptureWorker().getInstanceFolder();
    CHECK (instance.isDirectory());
}

TEST_CASE ("CapturePanel buttons send the requests; Clear asks first", "[plugin][capturepanel]")
{
    Rig rig;
    CapturePanel panel (rig.p);
    panel.runTimerTickForTests();

    SECTION ("New take while the transport plays splits the take")
    {
        rig.host->playHead.playStartSample = rig.host->position;
        setPlain (rig.p, params::testPattern, (float) static_cast<int> (TestPattern::notes));
        rig.host->runTo (rig.host->position + (int64_t) (2.0 * rate), 512);
        panel.runTimerTickForTests();
        CHECK (panel.getStatusText().startsWith ("Synced 120.0 BPM 4/4 " + enDash() + " recording take 1 (bar"));

        panel.pressNewTake();
        rig.host->runTo (rig.host->position + (int64_t) (2.0 * rate), 512);
        rig.host->playHead.playStopSample = rig.host->position;
        rig.host->runTo (rig.host->position + (int64_t) (1.0 * rate), 512);
        rig.host->steps (2);
        panel.runTimerTickForTests();

        REQUIRE (panel.getNumTakeRows() == 2);
        const auto takes = rig.p.getCaptureWorker().getTakes();
        CHECK (takes.front()->kind == TakeKind::manual);
        CHECK (takes.front()->mode == TakeMode::synced);
    }

    SECTION ("Grab last 30 s in free mode")
    {
        auto* grab = child<juce::TextButton> (panel, "grab");
        REQUIRE (grab != nullptr);
        CHECK (grab->getButtonText() == "Grab last 30 s");

        // Notes without a pause long enough for a take to close (gap 30 s)
        auto s = rig.p.getCaptureState();
        s.takeGapSeconds = 30.0;
        rig.p.setCaptureState (s);
        rig.host->runTo (rig.host->position + (int64_t) (3.0 * rate), 512);
        panel.pressGrab();
        rig.host->runTo (rig.host->position + (int64_t) (1.0 * rate), 512);
        rig.host->steps (2);
        panel.runTimerTickForTests();

        REQUIRE (panel.getNumTakeRows() == 1);
        CHECK (rig.p.getCaptureWorker().getTakes().front()->kind == TakeKind::grab);
        CHECK (panel.getRowText (0).contains ("Grab"));
    }

    SECTION ("Clear: cancelled does nothing, confirmed empties the list")
    {
        rig.freeTake (1.0);
        panel.runTimerTickForTests();
        REQUIRE (panel.getNumTakeRows() == 1);

        int asked = 0;
        bool answer = false;
        panel.confirmClear = [&] (std::function<void (bool)> done)
        {
            ++asked;
            done (answer);
        };

        panel.pressClear();
        rig.host->runTo (rig.host->position + 4096, 512);
        rig.host->steps (1);
        panel.runTimerTickForTests();
        CHECK (asked == 1);
        CHECK (panel.getNumTakeRows() == 1);

        answer = true;
        panel.pressClear();
        rig.host->runTo (rig.host->position + 4096, 512);
        rig.host->steps (1);
        panel.runTimerTickForTests();
        CHECK (asked == 2);
        CHECK (panel.getNumTakeRows() == 0);
        CHECK (panel.getSelectedTakeNumber() == 0);
    }
}

TEST_CASE ("CapturePanel option controls persist in CaptureState and follow a state load", "[plugin][capturepanel][state]")
{
    VocalSynthProcessor p (Variant::fx);
    CapturePanel panel (p);

    auto* quantize = child<juce::ComboBox> (panel, "quantize");
    auto* strength = child<juce::Slider> (panel, "strength");
    auto* bends = child<juce::ComboBox> (panel, "bends");
    auto* grabBars = child<juce::ComboBox> (panel, "grabBars");
    auto* gap = child<juce::Slider> (panel, "gap");
    REQUIRE ((quantize != nullptr && strength != nullptr && bends != nullptr && grabBars != nullptr && gap != nullptr));

    CHECK_FALSE (strength->isEnabled()); // quantize off by default
    quantize->setSelectedItemIndex (4, juce::sendNotificationSync); // 1/16
    strength->setValue (50.0, juce::sendNotificationSync);
    bends->setSelectedItemIndex (2, juce::sendNotificationSync);    // On
    grabBars->setSelectedId (16, juce::sendNotificationSync);
    gap->setValue (5.0, juce::sendNotificationSync);
    CHECK (strength->isEnabled());

    const auto s = p.getCaptureState();
    CHECK (s.render.quantize == QuantizeGrid::sixteenth);
    CHECK (s.render.quantizeStrengthPercent == Catch::Approx (50.0));
    CHECK (s.render.bends == BendExport::on);
    CHECK (s.grabBars == 16);
    CHECK (s.takeGapSeconds == Catch::Approx (5.0));

    // Saved with the project; a load (e.g. an older project) updates the controls at the next tick
    juce::MemoryBlock saved;
    p.getStateInformation (saved);
    p.setCaptureState (CaptureState {});
    panel.runTimerTickForTests();
    CHECK (quantize->getSelectedItemIndex() == 0);
    p.setStateInformation (saved.getData(), (int) saved.getSize());
    panel.runTimerTickForTests();
    CHECK (quantize->getSelectedItemIndex() == 4);
    CHECK (grabBars->getSelectedId() == 16);

    // The parameter controls are attached (offset mode, capture on/off)
    auto* mode = child<juce::ComboBox> (panel, "offsetMode");
    REQUIRE (mode != nullptr);
    mode->setSelectedItemIndex (1, juce::sendNotificationSync);
    CHECK (p.getCaptureOffsetSettings().mode == CaptureOffsetMode::manual);
    auto* capture = child<juce::ToggleButton> (panel, "capture");
    REQUIRE (capture != nullptr);
    CHECK (capture->getToggleState());
}

TEST_CASE ("CapturePanel layout: everything inside, nothing overlaps", "[plugin][capturepanel]")
{
    const auto variant = GENERATE (Variant::fx, Variant::instrument);
    VocalSynthProcessor p (variant);
    CapturePanel panel (p);
    panel.setSize (908, 350); // the editor's page area

    std::vector<juce::Component*> visible;

    for (auto* c : panel.getChildren())
        if (c->isVisible())
        {
            CAPTURE (c->getComponentID(), c->getBounds().toString());
            CHECK (panel.getLocalBounds().contains (c->getBounds()));
            CHECK_FALSE (c->getBounds().isEmpty());
            visible.push_back (c);
        }

    CHECK (visible.size() >= 30);

    for (std::size_t i = 0; i < visible.size(); ++i)
        for (std::size_t j = i + 1; j < visible.size(); ++j)
        {
            CAPTURE (visible[i]->getComponentID(), visible[j]->getComponentID());
            CHECK_FALSE (visible[i]->getBounds().intersects (visible[j]->getBounds()));
        }
}
