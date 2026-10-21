// M3.1 take length and Record mode (docs/M3.1-DESIGN.md 2): parameter IDs, version hint 3, choices, defaults,
// layout order, old states load with the defaults. Implementation (docs/M3.1-DESIGN.md 4, 7): the processor
// sends mode and limit per block, the Record / Cancel button, worker status, Capture tab texts and controls,
// end to end: Record 8 bars at 120 BPM with an ASIO 2048 round trip.

#include "CaptureExporter.h"
#include "CapturePanel.h"
#include "CaptureTestUtils.h"
#include "Parameters.h"
#include "PluginProcessor.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

using vocalsynth::Variant;
using vocalsynth::VocalSynthProcessor;

namespace
{
juce::AudioParameterChoice& getChoice (VocalSynthProcessor& p, const juce::ParameterID& id)
{
    auto* choice = dynamic_cast<juce::AudioParameterChoice*> (p.getParameters().getParameter (id.getParamID()));
    REQUIRE (choice != nullptr);
    return *choice;
}

void checkChoices (const juce::AudioParameterChoice& choice, std::initializer_list<const char*> expected)
{
    REQUIRE (choice.choices.size() == static_cast<int> (expected.size()));
    int i = 0;

    for (const auto* name : expected)
        CHECK (choice.choices[i++] == juce::String (name));
}
} // namespace

TEST_CASE ("M3.1 parameters exist with the design IDs, choices, defaults and version hint 3",
           "[plugin][parameters][capture][recordmode]")
{
    using namespace vocalsynth::params;
    VocalSynthProcessor p (GENERATE (Variant::fx, Variant::instrument));

    for (const auto* id : { &captureMode, &takeMaxBars, &recordBars })
        CHECK (id->getVersionHint() == 3); // after the released hints 1 (M1 / M2) and 2 (M3)

    auto& mode = getChoice (p, captureMode);
    checkChoices (mode, { "Background", "Record" });
    CHECK (mode.getIndex() == static_cast<int> (vocalsynth::CaptureMode::background));
    CHECK (mode.getName (100) == "Capture Mode");

    auto& maxBars = getChoice (p, takeMaxBars);
    checkChoices (maxBars, { "Off", "4 bars", "8 bars", "16 bars", "32 bars" });
    CHECK (vocalsynth::takeMaxBarsForIndex (maxBars.getIndex()) == 16);
    CHECK (maxBars.getName (100) == "Take Max Length");

    auto& bars = getChoice (p, recordBars);
    checkChoices (bars, { "1 bar", "2 bars", "4 bars", "8 bars", "16 bars", "32 bars" });
    CHECK (vocalsynth::recordBarsForIndex (bars.getIndex()) == 8);
    CHECK (bars.getName (100) == "Record Length");
}

TEST_CASE ("M3.1 parameters are appended after the M3 parameters", "[plugin][parameters][capture][recordmode]")
{
    VocalSynthProcessor p (Variant::fx);
    juce::StringArray ids;

    for (auto* parameter : p.getParameters().processor.getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (parameter))
            ids.add (ranged->paramID);

    CHECK (ids.indexOf ("captureFineOffsetMs") == 24);
    CHECK (ids.indexOf ("captureMode") == 25);
    CHECK (ids.indexOf ("takeMaxBars") == 26);
    CHECK (ids.indexOf ("recordBars") == 27);
}

TEST_CASE ("A 0.3.0 state without the M3.1 parameters loads with their defaults", "[plugin][parameters][capture][recordmode]")
{
    using namespace vocalsynth::params;
    juce::MemoryBlock saved;

    {
        VocalSynthProcessor p (Variant::fx);
        getChoice (p, captureMode).setValueNotifyingHost (1.0f); // Record
        getChoice (p, takeMaxBars).setValueNotifyingHost (0.0f); // Off
        getChoice (p, recordBars).setValueNotifyingHost (0.0f);  // 1 bar

        // Simulate the 0.3.0 state: drop the three new PARAM children
        auto state = p.getParameters().copyState();

        for (const auto* id : { &captureMode, &takeMaxBars, &recordBars })
            state.removeChild (state.getChildWithProperty ("id", id->getParamID()), nullptr);

        if (const auto xml = state.createXml())
            juce::AudioProcessor::copyXmlToBinary (*xml, saved);
    }

    VocalSynthProcessor restored (Variant::fx);
    getChoice (restored, captureMode).setValueNotifyingHost (1.0f);
    restored.setStateInformation (saved.getData(), static_cast<int> (saved.getSize()));

    CHECK (getChoice (restored, captureMode).getIndex() == 0);
    CHECK (getChoice (restored, takeMaxBars).getIndex() == vocalsynth::defaultTakeMaxBarsIndex);
    CHECK (getChoice (restored, recordBars).getIndex() == vocalsynth::defaultRecordBarsIndex);
}

//==============================================================================
namespace
{
constexpr double rate = 48000.0;
constexpr int64_t bar = 96000; // 120 BPM, 4/4

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

template <typename T>
T* child (juce::Component& panel, const juce::String& id)
{
    return dynamic_cast<T*> (panel.findChildWithID (id));
}
} // namespace

TEST_CASE ("Record mode: the processor sends mode and take limit with the block settings", "[plugin][capture][recordmode]")
{
    using namespace vocalsynth;
    VocalSynthProcessor p (Variant::fx);
    p.getCaptureWorker().setManualSteppingForTests (true);
    test::CaptureHost host (p, p.getCaptureWorker(), rate, 512);

    host.runTo (4096, 512);
    host.steps (1);
    auto status = p.getCaptureWorker().getStatus();
    CHECK (status.mode == CaptureMode::background);
    CHECK (status.takeMaxBars == 16);
    CHECK (p.getTakeMaxBars() == 16);
    CHECK (p.getRecordBars() == 8);

    setPlain (p, params::takeMaxBars, 0.0f);
    setPlain (p, params::captureMode, 1.0f);
    setPlain (p, params::recordBars, 5.0f);
    CHECK (p.getCaptureMode() == CaptureMode::record);
    CHECK (p.getTakeMaxBars() == 0);
    CHECK (p.getRecordBars() == 32);
    host.runTo (8192, 512);
    host.steps (1);
    status = p.getCaptureWorker().getStatus();
    CHECK (status.mode == CaptureMode::record);
    CHECK (status.takeMaxBars == 0);
    CHECK (status.record.phase == RecordStatus::Phase::idle);
}

TEST_CASE ("Record mode: Capture tab status texts", "[plugin][capturepanel][recordmode]")
{
    using namespace vocalsynth;
    CaptureStatus s;
    s.started = true;
    s.state = TakeSegmenter::State::synced;
    s.bpm = 128.0;
    s.recordingTakeNumber = 3;
    s.recordingBar = 17;
    s.takeMaxBars = 16;

    // Background with a limit: waiting for the first note, then bar k/N
    CHECK (CapturePanel::formatStatus (s, true, 0) == "Synced 128.0 BPM 4/4 " + enDash() + " take 3 waiting for a note");
    s.openTakeBars = 16;
    s.openTakeStartBar = 16;
    s.openTakeBar = 2;
    CHECK (CapturePanel::formatStatus (s, true, 0) == "Synced 128.0 BPM 4/4 " + enDash() + " take 3, bar 2/16 (bar 17)");

    // Off: as 0.3.0
    CaptureStatus off = s;
    off.takeMaxBars = 0;
    off.openTakeBars = 0;
    off.openTakeBar = 0;
    off.openTakeStartBar = 0;
    CHECK (CapturePanel::formatStatus (off, true, 0) == "Synced 128.0 BPM 4/4 " + enDash() + " recording take 3 (bar 17)");

    CaptureStatus free;
    free.started = true;
    free.state = TakeSegmenter::State::freeRecording;
    free.recordingTakeNumber = 4;
    free.openTakeBars = 16;
    CHECK (CapturePanel::formatStatus (free, true, 0) == "Free " + enDash() + " recording take 4 (max 16 bars)");

    // Record mode
    CaptureStatus r;
    r.started = true;
    r.state = TakeSegmenter::State::syncedIdle;
    r.mode = CaptureMode::record;
    CHECK (CapturePanel::formatStatus (r, true, 0, 8) == "Record mode " + enDash() + " press Record (8 bars)");
    CHECK (CapturePanel::formatStatus (r, true, 0, 1) == "Record mode " + enDash() + " press Record (1 bar)");
    r.record.phase = RecordStatus::Phase::waitingForPlay;
    r.record.bars = 8;
    CHECK (CapturePanel::formatStatus (r, true, 0) == "Armed " + enDash() + " starts when FL plays (8 bars)");
    r.record.phase = RecordStatus::Phase::countIn;
    r.record.startBar = 17;
    r.record.beatsToStart = 2.4;
    CHECK (CapturePanel::formatStatus (r, true, 0) == "Starts at bar 17 " + enDash() + " 3 beats");
    r.record.beatsToStart = 0.3;
    CHECK (CapturePanel::formatStatus (r, true, 0) == "Starts at bar 17 " + enDash() + " 1 beat");
    r.record.phase = RecordStatus::Phase::recording;
    r.record.barInTake = 3;
    r.record.takeNumber = 5;
    r.recordingTakeNumber = 5;
    CHECK (CapturePanel::formatStatus (r, true, 0) == "Recording bar 3/8 (take 5)");
    CHECK (CapturePanel::formatStatus (r, false, 0) == "Capture off");

    // Row badge of a Record take
    Take take;
    take.number = 5;
    take.mode = TakeMode::synced;
    take.kind = TakeKind::record;
    RenderedTake rendered;
    rendered.originBar = 17;
    rendered.endTick = 8 * 3840;
    rendered.noteCount = 12;
    CHECK (CapturePanel::describeTake (take, rendered).contains ("Bar 17"));
    CHECK (CapturePanel::describeTake (take, rendered).contains ("8 bars"));
    CHECK (CapturePanel::describeTake (take, rendered).contains (juce::String (juce::CharPointer_UTF8 (" \xc2\xb7 Rec"))));
}

TEST_CASE ("Record mode: Capture tab controls and the Record / Cancel button", "[plugin][capturepanel][recordmode]")
{
    using namespace vocalsynth;
    VocalSynthProcessor p (Variant::fx);
    p.getCaptureWorker().setManualSteppingForTests (true);
    setPlain (p, params::source, (float) static_cast<int> (params::Source::testPattern));
    setPlain (p, params::testPattern, (float) static_cast<int> (TestPattern::notes));
    test::CaptureHost host (p, p.getCaptureWorker(), rate, 512);
    CapturePanel panel (p);
    panel.setSize (908, 350);
    host.runTo (4096, 512);
    host.steps (1);
    panel.runTimerTickForTests();

    auto* mode = child<juce::ComboBox> (panel, "captureMode");
    auto* maxLength = child<juce::ComboBox> (panel, "takeMaxBars");
    auto* length = child<juce::ComboBox> (panel, "recordBars");
    auto* record = child<juce::TextButton> (panel, "record");
    auto* newTake = child<juce::TextButton> (panel, "newTake");
    REQUIRE (mode != nullptr);
    REQUIRE (maxLength != nullptr);
    REQUIRE (length != nullptr);
    REQUIRE (record != nullptr);
    REQUIRE (newTake != nullptr);

    // Background: Max length (attached to the parameter), no Record button
    CHECK (mode->getSelectedItemIndex() == 0);
    CHECK (maxLength->isVisible());
    CHECK (maxLength->getText() == "16 bars");
    CHECK_FALSE (record->isVisible());
    CHECK_FALSE (length->isVisible());
    CHECK (newTake->isEnabled());
    panel.pressRecord(); // ignored in Background mode
    host.runTo (8192, 512);
    host.steps (1);
    CHECK (p.getCaptureWorker().getStatus().record.phase == RecordStatus::Phase::idle);

    maxLength->setSelectedItemIndex (1, juce::sendNotificationSync); // the attachment writes the parameter
    CHECK (p.getTakeMaxBars() == 4);

    // Record mode via the combo
    mode->setSelectedItemIndex (1, juce::sendNotificationSync);
    CHECK (p.getCaptureMode() == CaptureMode::record);
    host.runTo (12288, 512);
    host.steps (1);
    panel.runTimerTickForTests();
    CHECK_FALSE (maxLength->isVisible());
    CHECK (length->isVisible());
    CHECK (length->getText() == "8 bars");
    CHECK (record->isVisible());
    CHECK (panel.getRecordButtonText() == "Record");
    CHECK_FALSE (newTake->isEnabled());
    CHECK (panel.getStatusText() == "Record mode " + enDash() + " press Record (8 bars)");

    // Layout in Record mode: inside, nothing overlaps
    std::vector<juce::Component*> visible;

    for (auto* c : panel.getChildren())
        if (c->isVisible())
        {
            CAPTURE (c->getComponentID(), c->getBounds().toString());
            CHECK (panel.getLocalBounds().contains (c->getBounds()));
            CHECK_FALSE (c->getBounds().isEmpty());
            visible.push_back (c);
        }

    for (std::size_t i = 0; i < visible.size(); ++i)
        for (std::size_t j = i + 1; j < visible.size(); ++j)
        {
            CAPTURE (visible[i]->getComponentID(), visible[j]->getComponentID());
            CHECK_FALSE (visible[i]->getBounds().intersects (visible[j]->getBounds()));
        }

    // Stopped: armed, waits for Play; Cancel disarms
    panel.pressRecord();
    CHECK (panel.getRecordButtonText() == "Cancel");
    host.runTo (16384, 512);
    host.steps (1);
    panel.runTimerTickForTests();
    CHECK (panel.getStatusText() == "Armed " + enDash() + " starts when FL plays (8 bars)");
    CHECK (panel.getRecordButtonText() == "Cancel");
    panel.pressRecord();
    CHECK (panel.getRecordButtonText() == "Record");
    host.runTo (20480, 512);
    host.steps (1);
    panel.runTimerTickForTests();
    CHECK (panel.getStatusText() == "Record mode " + enDash() + " press Record (8 bars)");
    CHECK (panel.getRecordButtonText() == "Record");

    // Playing: count-in, then recording, then by itself back to idle; one Rec take of 2 bars
    length->setSelectedItemIndex (1, juce::sendNotificationSync); // 2 bars
    host.playHead.playStartSample = host.position;
    host.runTo (host.position + 24000, 512); // beat 2 of bar 1
    panel.pressRecord();
    host.runTo (host.position + 512, 512);
    host.steps (1);
    panel.runTimerTickForTests();
    CHECK (panel.getStatusText() == "Starts at bar 2 " + enDash() + " 3 beats");
    host.runTo (host.playHead.playStartSample + bar + 24000, 512);
    host.steps (1);
    panel.runTimerTickForTests();
    CHECK (panel.getStatusText().startsWith ("Recording bar 1/2 (take "));
    CHECK (panel.getRecordButtonText() == "Cancel");
    host.runTo (host.playHead.playStartSample + 4 * bar, 512);
    host.steps (2);
    panel.runTimerTickForTests();
    CHECK (panel.getStatusText() == "Record mode " + enDash() + " press Record (2 bars)");
    CHECK (panel.getRecordButtonText() == "Record");
    // Newest first: the Rec take; below it the free background take the switch to Record mode closed
    REQUIRE (panel.getNumTakeRows() == 2);
    CHECK_FALSE (panel.getRowText (1).contains ("Rec"));
    CHECK (panel.getRowText (0).contains ("Bar 2"));
    CHECK (panel.getRowText (0).contains ("2 bars"));
    CHECK (panel.getRowText (0).contains ("Rec"));
}

TEST_CASE ("Record mode end to end: Record 8 bars at 120 BPM with an ASIO 2048 round trip", "[plugin][capture][recordmode][voice]")
{
    using namespace vocalsynth;
    constexpr int preparedBlock = 2048;
    constexpr int64_t roundTrip = 2 * preparedBlock;  // what the sound card adds (Auto = 2 x the prepared block)
    constexpr int64_t transportStart = 61440;          // bar 5 (ppq 16)
    constexpr int64_t recordStart = transportStart + bar;  // the Record button is pressed in bar 5: bar 6
    constexpr int64_t recordEnd = recordStart + 8 * bar;

    VocalSynthProcessor p (Variant::fx);
    p.getCaptureWorker().setManualSteppingForTests (true);
    setPlain (p, params::voiceRange, (float) static_cast<int> (VoiceRange::tenor));
    setPlain (p, params::captureMode, 1.0f);
    const auto takesFolder = juce::File::createTempFile ("vs-record-takes");
    auto state = p.getCaptureState();
    state.takesFolder = takesFolder.getFullPathName();
    p.setCaptureState (state);

    // Sung (host time): over the start, one note per bar, one held over the end
    std::vector<test::SungNote> notes;
    notes.push_back ({ recordStart - 12000, 36000, 220.0 });

    for (int k = 1; k < 8; ++k)
        notes.push_back ({ recordStart + k * bar, 16800, k % 2 == 0 ? 246.94 : 220.0 });

    notes.push_back ({ recordEnd - 24000, 48000, 196.0 });

    test::CaptureHost host (p, p.getCaptureWorker(), rate, preparedBlock);
    host.playHead.playStartSample = transportStart;
    host.playHead.playStartPpq = 16.0;
    host.input = [notes] (int64_t pos) { return test::sungPhrase (notes, pos, rate, roundTrip); };

    host.runTo (transportStart + 30720, preparedBlock); // bar 5, beat 2.3
    p.getCaptureTap().requestRecord (p.getRecordBars());
    host.runTo (recordStart + 3 * bar, preparedBlock);
    auto status = p.getCaptureWorker().getStatus();
    CHECK (status.record.phase == RecordStatus::Phase::recording);
    CHECK (status.record.startBar == 6);
    CHECK (status.record.barInTake == 3);
    host.runTo (recordEnd + 2 * bar, preparedBlock);
    host.playHead.playStopSample = host.position;
    host.runTo (host.position + (int64_t) rate, preparedBlock);
    host.steps (4);

    auto& worker = p.getCaptureWorker();
    const auto takes = worker.getTakes();
    REQUIRE (takes.size() == 1); // no background take in Record mode
    const auto& take = *takes.front();
    CHECK (take.kind == TakeKind::record);
    CHECK (take.startSample == recordStart);
    CHECK (take.endSample == recordEnd);
    CHECK (take.anchorBar == 6);
    CHECK (take.maxBars == 8);

    const auto folder = juce::File::createTempFile ("vs-record-mid");
    const auto midi = CaptureExporter::writeMidi (take, p.getRenderOptionsFor (take), SmfFormat::type1, folder);
    REQUIRE (midi.existsAsFile());
    CHECK (midi.getFileName().contains ("_Take01_Bar006_Clean.mid"));
    juce::MidiFile midiFile;
    REQUIRE (test::readMidiFile (midi, midiFile));
    const auto fileNotes = test::notesOf (midiFile);
    REQUIRE (fileNotes.size() == 9);
    CHECK (fileNotes.front().onTick == Catch::Approx (0.0));         // sung before bar 6: starts at bar 6
    CHECK (fileNotes.back().offTick == Catch::Approx (8.0 * 3840.0)); // held over the end: clipped, with a note-off

    for (int k = 1; k < 8; ++k)
        CHECK (fileNotes[(std::size_t) k].onTick == Catch::Approx (3840.0 * k).margin (48.0)); // on the bar lines

    const auto& track = *midiFile.getTrack (0);
    const auto* last = track.getEventPointer (track.getNumEvents() - 1);
    CHECK (last->message.isEndOfTrackMetaEvent());
    CHECK (last->message.getTimeStamp() == Catch::Approx (8.0 * 3840.0)); // exactly 8 bars

    // Raw has the same length; the WAV is exactly 8 bars from tick 0
    auto rawOptions = p.getRenderOptionsFor (take);
    rawOptions.version = ExportVersion::raw;
    CHECK (TakeRenderer::render (take, rawOptions).endTick == 8 * 3840);

    for (int i = 0; i < 20 && worker.getWavState (take.number).kind == TakeWavState::Kind::preparing; ++i)
        worker.runStepForTests();

    const auto wav = worker.getWavState (take.number);
    REQUIRE (wav.kind == TakeWavState::Kind::ready);
    CHECK (wav.file.getFileName().endsWith ("_Take01_Bar006_Voice.wav"));
    double wavRate = 0.0;
    int bits = 0;
    const auto audio = test::readWav (wav.file, wavRate, bits);
    CHECK (audio.getNumSamples() == 8 * bar);

    folder.deleteRecursively();
    takesFolder.deleteRecursively();
}
