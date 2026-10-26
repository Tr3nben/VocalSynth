// M3.1 take length and Record mode (docs/M3.1-DESIGN.md 2): parameter IDs, version hint 3, choices, defaults,
// layout order, old states load with the defaults. Implementation (docs/M3.1-DESIGN.md 4, 7): the processor
// sends mode and limit per block, the Record / Cancel button, worker status, Capture tab texts and controls,
// end to end: Record 8 bars at 120 BPM with an ASIO 2048 round trip; Record armed at bar 2 beat 3 with clicks
// after the phrase; background takes cut by Take Max Length 4 bars.

#include "CaptureExporter.h"
#include "CapturePanel.h"
#include "CaptureTestUtils.h"
#include "Parameters.h"
#include "PluginProcessor.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>

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

//==============================================================================
namespace
{
/** Mouse-click-like noise (0.3.0 feedback: clicks after singing kept takes open): a press and a release 80 ms
    later, each a 4 ms burst of decaying white noise (deterministic hash), every period samples in
    [from, to) (host time, heard roundTrip samples later). */
float clickNoise (int64_t position, int64_t from, int64_t to, int64_t period, int64_t roundTrip)
{
    const auto hostSample = position - roundTrip;

    if (hostSample < from || hostSample >= to)
        return 0.0f;

    const auto inPeriod = (hostSample - from) % period;
    const auto inClick = inPeriod < 3840 ? inPeriod : inPeriod - 3840;

    if (inClick < 0 || inClick >= 192)
        return 0.0f;

    auto h = static_cast<uint32_t> (hostSample) * 2654435761u;
    h ^= h >> 15;
    h *= 2246822519u;
    h ^= h >> 13;
    const auto white = static_cast<float> (h & 0xffffu) / 32768.0f - 1.0f;
    return 0.6f * white * std::exp (-static_cast<float> (inClick) / 48.0f);
}

int midiNoteOf (double hz)
{
    return static_cast<int> (std::lround (69.0 + 12.0 * std::log2 (hz / 440.0)));
}
} // namespace

TEST_CASE ("Record mode end to end: armed at bar 2 beat 3, 8 bars, clicks after the phrase add nothing",
           "[plugin][capture][recordmode][voice]")
{
    using namespace vocalsynth;
    constexpr int preparedBlock = 512;
    constexpr int64_t roundTrip = 2 * preparedBlock;
    constexpr int64_t armSample = bar + 2 * 24000; // bar 2, beat 3 (transport from sample 0 = bar 1)
    constexpr int64_t recordStart = 2 * bar;       // the next bar line: bar 3
    constexpr int64_t recordEnd = recordStart + 8 * bar; // bar 11
    constexpr int64_t clicksFrom = recordStart + 6 * bar + 30000; // after the phrase, over the end and beyond
    constexpr int64_t clicksTo = recordEnd + 3 * bar;

    VocalSynthProcessor p (Variant::fx);
    p.getCaptureWorker().setManualSteppingForTests (true);
    setPlain (p, params::voiceRange, (float) static_cast<int> (VoiceRange::tenor));
    setPlain (p, params::captureMode, 1.0f); // Record
    const auto takesFolder = juce::File::createTempFile ("vs-record-click-takes");
    auto state = p.getCaptureState();
    state.takesFolder = takesFolder.getFullPathName();
    p.setCaptureState (state);

    // Sung (host time): 12 half-bar notes in bars 3 .. 8, then only clicks
    constexpr double pitches[] = { 220.0, 246.94, 261.63 };
    std::vector<test::SungNote> notes;

    for (int k = 0; k < 12; ++k)
        notes.push_back ({ recordStart + 4000 + k * 48000, 40000, pitches[k % 3] });

    test::CaptureHost host (p, p.getCaptureWorker(), rate, preparedBlock);
    host.playHead.playStartSample = 0;
    host.input = [notes] (int64_t pos)
    {
        return test::sungPhrase (notes, pos, rate, roundTrip)
               + clickNoise (pos, clicksFrom, clicksTo, 19200, roundTrip);
    };

    host.runTo (armSample, preparedBlock);
    p.getCaptureTap().requestRecord (p.getRecordBars());
    host.runTo (armSample + 4 * preparedBlock, preparedBlock);
    host.steps (1);
    auto status = p.getCaptureWorker().getStatus();
    CHECK (status.record.phase == RecordStatus::Phase::countIn);
    CHECK (status.record.startBar == 3);

    host.runTo (recordStart + 4 * bar, preparedBlock);
    status = p.getCaptureWorker().getStatus();
    CHECK (status.record.phase == RecordStatus::Phase::recording);
    CHECK (status.record.startBar == 3);

    host.runTo (clicksTo + bar, preparedBlock);
    host.playHead.playStopSample = host.position;
    host.runTo (host.position + (int64_t) rate, preparedBlock);
    host.steps (4);

    CHECK (host.noteOns == 12); // the live MIDI: the sung notes, nothing from the clicks

    auto& worker = p.getCaptureWorker();
    const auto takes = worker.getTakes();
    REQUIRE (takes.size() == 1); // one Record take, no background take from the clicks
    const auto& take = *takes.front();
    CHECK (take.kind == TakeKind::record);
    CHECK (take.startSample == recordStart);
    CHECK (take.endSample == recordEnd);
    CHECK (take.anchorBar == 3);
    CHECK (take.maxBars == 8);
    CHECK (take.noteCount == 12);

    const auto folder = juce::File::createTempFile ("vs-record-click-mid");

    for (const auto version : { ExportVersion::clean, ExportVersion::raw })
    {
        CAPTURE (version == ExportVersion::clean ? "Clean" : "Raw");
        auto options = p.getRenderOptionsFor (take);
        options.version = version;
        const auto midi = CaptureExporter::writeMidi (take, options, SmfFormat::type1, folder);
        REQUIRE (midi.existsAsFile());
        CHECK (midi.getFileName().contains ("_Take01_Bar003_"));
        juce::MidiFile midiFile;
        REQUIRE (test::readMidiFile (midi, midiFile));
        const auto fileNotes = test::notesOf (midiFile);
        REQUIRE (fileNotes.size() == notes.size());

        for (std::size_t k = 0; k < notes.size(); ++k)
        {
            CAPTURE (k);
            const auto expectedOn = static_cast<double> (notes[k].startSample - recordStart) / static_cast<double> (bar) * 3840.0;
            CHECK (fileNotes[k].note == midiNoteOf (notes[k].frequencyHz));
            CHECK (fileNotes[k].onTick == Catch::Approx (expectedOn).margin (96.0)); // within 25 ms
            CHECK (fileNotes[k].offTick > fileNotes[k].onTick);
            CHECK (fileNotes[k].offTick <= Catch::Approx (static_cast<double> (clicksFrom - recordStart) / static_cast<double> (bar) * 3840.0));
        }

        const auto& track = *midiFile.getTrack (0);
        const auto* last = track.getEventPointer (track.getNumEvents() - 1);
        CHECK (last->message.isEndOfTrackMetaEvent());
        CHECK (last->message.getTimeStamp() == Catch::Approx (8.0 * 3840.0)); // bars 3 .. 11, exactly 8 bars
    }

    folder.deleteRecursively();
    takesFolder.deleteRecursively();
}

TEST_CASE ("Background end to end: Take Max Length 4 bars cuts 10 bars of singing into takes of at most 4 bars",
           "[plugin][capture][recordmode][voice]")
{
    using namespace vocalsynth;
    constexpr int preparedBlock = 512;
    constexpr int64_t roundTrip = 2 * preparedBlock;
    constexpr int64_t singStart = bar; // bar 2
    constexpr int numNotes = 20;       // 10 bars, two notes per bar

    VocalSynthProcessor p (Variant::fx);
    p.getCaptureWorker().setManualSteppingForTests (true);
    setPlain (p, params::voiceRange, (float) static_cast<int> (VoiceRange::tenor));
    setPlain (p, params::takeMaxBars, 1.0f); // index 1: 4 bars
    const auto takesFolder = juce::File::createTempFile ("vs-maxbars-takes");
    auto state = p.getCaptureState();
    state.takesFolder = takesFolder.getFullPathName();
    p.setCaptureState (state);

    constexpr double pitches[] = { 220.0, 246.94, 261.63, 196.0 };
    std::vector<test::SungNote> notes;

    for (int k = 0; k < numNotes; ++k)
        notes.push_back ({ singStart + 6000 + k * 48000, 36000, pitches[k % 4] });

    test::CaptureHost host (p, p.getCaptureWorker(), rate, preparedBlock);
    host.playHead.playStartSample = 0;
    host.input = [notes] (int64_t pos) { return test::sungPhrase (notes, pos, rate, roundTrip); };

    host.runTo (singStart + 10 * bar + 2 * bar, preparedBlock);
    host.playHead.playStopSample = host.position;
    host.runTo (host.position + (int64_t) rate, preparedBlock);
    host.steps (4);

    auto& worker = p.getCaptureWorker();
    auto takes = worker.getTakes();
    std::sort (takes.begin(), takes.end(), [] (const auto& a, const auto& b) { return a->startSample < b->startSample; });
    REQUIRE (takes.size() == 3); // bars 2 .. 5, 6 .. 9, 10 .. 11

    const auto folder = juce::File::createTempFile ("vs-maxbars-mid");
    std::size_t totalNotes = 0;

    for (std::size_t i = 0; i < takes.size(); ++i)
    {
        const auto& take = *takes[i];
        CAPTURE (i, take.startSample, take.endSample, take.anchorBar);
        CHECK (take.kind == TakeKind::automatic);
        CHECK (take.closed);
        CHECK (take.maxBars == 4);
        CHECK (take.anchorBar == 2 + 4 * static_cast<int64_t> (i));
        CHECK (take.startSample == (take.anchorBar - 1) * bar);
        CHECK (take.endSample - take.startSample <= 4 * bar);

        if (i + 1 < takes.size())
            CHECK (take.endSample == takes[i + 1]->startSample); // cut at the bar line, the next take there

        for (const auto version : { ExportVersion::clean, ExportVersion::raw })
        {
            auto options = p.getRenderOptionsFor (take);
            options.version = version;
            const auto rendered = TakeRenderer::render (take, options);
            CHECK (rendered.endTick <= 4 * 3840);
            CHECK (rendered.endTick % 3840 == 0);
        }

        const auto midi = CaptureExporter::writeMidi (take, p.getRenderOptionsFor (take), SmfFormat::type1, folder);
        REQUIRE (midi.existsAsFile());
        juce::MidiFile midiFile;
        REQUIRE (test::readMidiFile (midi, midiFile));
        const auto fileNotes = test::notesOf (midiFile);

        for (const auto& n : fileNotes)
        {
            CHECK (n.onTick >= 0.0);
            CHECK (n.offTick <= 4.0 * 3840.0);
        }

        totalNotes += fileNotes.size();
    }

    CHECK (totalNotes == static_cast<std::size_t> (numNotes)); // every sung note in exactly one take

    folder.deleteRecursively();
    takesFolder.deleteRecursively();
}

TEST_CASE ("Background end to end: noise notes and a note just over the cut do not shape the takes (Max 8 bars)",
           "[plugin][capture][recordmode][voice]")
{
    // 0.3.0 feedback: clicks and noise kept takes open. Short pitched bursts (notes < 80 ms) before, between
    // and after two phrases; the first phrase's last note is held 20 ms past the cut (bar 11).
    using namespace vocalsynth;
    constexpr int preparedBlock = 512;
    constexpr int64_t roundTrip = 2 * preparedBlock;
    constexpr int64_t cut = 10 * bar; // bar 11: 8 bars after bar 3

    VocalSynthProcessor p (Variant::fx);
    p.getCaptureWorker().setManualSteppingForTests (true);
    setPlain (p, params::voiceRange, (float) static_cast<int> (VoiceRange::tenor));
    setPlain (p, params::takeMaxBars, 2.0f); // index 2: 8 bars
    const auto takesFolder = juce::File::createTempFile ("vs-noise-takes");
    auto state = p.getCaptureState();
    state.takesFolder = takesFolder.getFullPathName();
    p.setCaptureState (state);

    constexpr double pitches[] = { 220.0, 246.94, 261.63, 196.0 };
    std::vector<test::SungNote> notes;
    notes.push_back ({ 20000, 2400, 329.63 });       // bar 1: a 50 ms burst
    notes.push_back ({ bar + 30000, 2400, 293.66 }); // bar 2

    for (int k = 0; k < 16; ++k) // phrase A: bars 3 .. 10, the last note ends 20 ms after the cut
    {
        const auto start = 2 * bar + 4000 + k * 48000;
        notes.push_back ({ start, k == 15 ? cut + 960 - start : 40000, pitches[k % 4] });
    }

    notes.push_back ({ cut + 50000, 2400, 329.63 }); // bar 11
    notes.push_back ({ cut + bar + 20000, 2400, 293.66 }); // bar 12

    for (int k = 0; k < 4; ++k) // phrase B: bars 13 .. 14
        notes.push_back ({ 12 * bar + 4000 + k * 48000, 40000, pitches[k % 4] });

    test::CaptureHost host (p, p.getCaptureWorker(), rate, preparedBlock);
    host.playHead.playStartSample = 0;
    host.input = [notes] (int64_t pos) { return test::sungPhrase (notes, pos, rate, roundTrip); };

    host.runTo (16 * bar, preparedBlock);
    host.playHead.playStopSample = host.position;
    host.runTo (host.position + (int64_t) rate, preparedBlock);
    host.steps (4);

    CAPTURE (host.noteOns);
    CHECK (host.noteOns >= 20); // the phrases; the bursts may add short notes (that is the point)

    auto takes = p.getCaptureWorker().getTakes();
    std::sort (takes.begin(), takes.end(), [] (const auto& a, const auto& b) { return a->startSample < b->startSample; });
    REQUIRE (takes.size() == 2); // no noise-only take, no take for the 20 ms rest

    const auto& a = *takes[0];
    const auto& b = *takes[1];
    CHECK (a.number == 1);
    CHECK (a.anchorBar == 3);
    CHECK (a.startSample == 2 * bar);
    CHECK (a.endSample == cut);
    CHECK (b.number == 2);
    CHECK (b.anchorBar == 13);
    CHECK (b.startSample == 12 * bar);

    for (const auto version : { ExportVersion::clean, ExportVersion::raw })
    {
        CAPTURE (version == ExportVersion::clean ? "Clean" : "Raw");
        auto options = p.getRenderOptionsFor (a);
        options.version = version;
        const auto ra = TakeRenderer::render (a, options);
        CHECK (ra.originBar == 3);
        CHECK (ra.endTick == 8 * 3840);
        CHECK (ra.noteCount == 16);

        options = p.getRenderOptionsFor (b);
        options.version = version;
        const auto rb = TakeRenderer::render (b, options);
        CHECK (rb.originBar == 13);
        CHECK (rb.endTick == 2 * 3840);
        CHECK (rb.noteCount == 4);
    }

    takesFolder.deleteRecursively();
}
