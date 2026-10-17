// CaptureExporter and ExportFiles (docs/M3-DESIGN.md 12, 16): .mid readable by juce::MidiFile (PPQ, tempo /
// time-signature meta at tick 0, RPN bend range when bends are on, end-of-track rule, pairing), WAV
// readable by juce::WavAudioFormat (24-bit / float, length, samples), file names, temp + move with a
// blocked target (numbered sibling), persistWav (move, never overwrites), heartbeat + stale-folder
// cleanup (24 h, keeps the own folder), deleteInstanceFolder.

#include "CaptureExporter.h"
#include "CaptureTestUtils.h"
#include "CaptureWorker.h"
#include "ExportFiles.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <regex>

using namespace vocalsynth;
using vocalsynth::test::noteOffAt;
using vocalsynth::test::noteOnAt;
using vocalsynth::test::TimedEvent;

namespace
{
constexpr double rate = 48000.0;

/** A temporary folder, deleted at the end of the test. */
struct TempFolder
{
    juce::File folder = juce::File::createTempFile ("vs-export-test");
    TempFolder() { folder.createDirectory(); }
    ~TempFolder() { folder.deleteRecursively(); }
};

CapturedEvent capturedEvent (int64_t sample, VsEventType type, int note, int velocity = 100)
{
    CapturedEvent e;
    e.sample = sample;
    e.estimatedSample = sample;
    e.event.type = type;
    e.event.channel = 1;
    e.event.data1 = (uint8_t) note;
    e.event.data2 = (uint8_t) (type == VsEventType::noteOn ? velocity : 0);
    return e;
}

/** A free take with three notes (0.5 s each, 0.5 s apart) at 120 BPM. */
Take freeTake (int number = 3)
{
    Take t;
    t.number = number;
    t.mode = TakeMode::free;
    t.closed = true;
    t.sampleRate = rate;
    t.maxBlockSize = 512;
    t.startSample = 48000;
    t.endSample = 48000 + 3 * 48000;
    t.captureEndSample = t.endSample;
    t.freeBpm = 120.0;
    t.wallClockStartMs = juce::Time (2026, 9, 17, 14, 3, 22, 0, true).toMilliseconds(); // 2026-10-17 14:03:22

    for (int i = 0; i < 3; ++i)
    {
        const auto at = t.startSample + i * 48000;
        t.events.push_back (capturedEvent (at, VsEventType::noteOn, 60 + i));
        t.events.push_back (capturedEvent (at + 24000, VsEventType::noteOff, 60 + i));
    }

    t.noteCount = 3;
    return t;
}

/** A synced take made by the real capture path (tap -> worker): bar 3 at 120 BPM, True bends, range 12. */
std::shared_ptr<const Take> syncedTake (CaptureTap& tap, CaptureWorker& worker)
{
    tap.prepare (rate, 512);
    worker.setManualSteppingForTests (true);
    worker.start();

    vocalsynth::test::TapFeeder feeder (tap);
    feeder.worker = &worker;
    feeder.settings.captureAudio = false;
    feeder.settings.settings.bendMode = BendMode::trueBend;
    feeder.settings.settings.bendRangeSemitones = 12;
    const int64_t start = 48 * 512, stop = start + 8 * 24000; // block boundaries; 2 bars from ppq 8 (bar 3)
    feeder.transport = [=] (int64_t s) { return vocalsynth::test::playingTransport (s, rate, 120.0, start, 8.0, stop); };

    std::vector<TimedEvent> events;

    for (int beat = 0; beat < 6; ++beat)
    {
        const auto at = start + beat * 24000;
        events.push_back (noteOnAt (at, 60 + beat));

        TimedEvent bend;
        bend.sample = at + 4000;
        bend.event.type = VsEventType::pitchBend;
        bend.event.bend = 1000;
        events.push_back (bend);
        events.push_back (noteOffAt (at + 12000, 60 + beat));
    }

    feeder.runTo (stop + (int64_t) rate, 512, events);
    worker.runStepForTests();
    const auto takes = worker.getTakes();
    return takes.empty() ? nullptr : takes.front();
}
} // namespace

TEST_CASE ("ExportFiles: writeBytesAtomically writes the bytes", "[plugin][captureexporter]")
{
    const juce::TemporaryFile temp (".bin");
    const char bytes[] = { 'V', 'S', '3' };
    const auto file = ExportFiles::writeBytesAtomically (temp.getFile(), bytes, sizeof (bytes));
    REQUIRE (file == temp.getFile());
    CHECK (file.getSize() == 3);
    CHECK (ExportFiles::writeBytesAtomically (juce::File(), bytes, sizeof (bytes)) == juce::File());
}

TEST_CASE ("ExportFiles: a blocked target gets a numbered sibling; moveOrCopy never overwrites", "[plugin][captureexporter]")
{
    TempFolder dir;

    // A non-empty directory where the file should go: the temporary file cannot replace it (like a file FL
    // holds open on Windows)
    const auto target = dir.folder.getChildFile ("Take.mid");
    REQUIRE (target.getChildFile ("keep").create().wasOk());
    const char bytes[] = { 1, 2, 3, 4 };
    const auto written = ExportFiles::writeBytesAtomically (target, bytes, sizeof (bytes));
    REQUIRE (written.existsAsFile());
    CHECK (written != target);
    CHECK (written.getParentDirectory() == dir.folder);
    CHECK (written.getSize() == 4);

    const auto source = dir.folder.getChildFile ("a.wav");
    const auto existing = dir.folder.getChildFile ("sub").getChildFile ("a.wav");
    REQUIRE (source.replaceWithText ("new"));
    REQUIRE (existing.create().wasOk());
    REQUIRE (existing.replaceWithText ("old"));
    const auto moved = ExportFiles::moveOrCopy (source, existing);
    REQUIRE (moved.existsAsFile());
    CHECK (moved != existing);
    CHECK (existing.loadFileAsString() == "old");
    CHECK (moved.loadFileAsString() == "new");
    CHECK_FALSE (source.exists());
}

TEST_CASE ("CaptureExporter: folders", "[plugin][captureexporter]")
{
    CHECK (CaptureExporter::getCaptureRoot().isDirectory());
    CHECK (CaptureExporter::getCaptureRoot().getParentDirectory() == ExportFiles::getTempRoot());
    CHECK (CaptureExporter::getInstanceFolder ("abc").getParentDirectory() == CaptureExporter::getCaptureRoot());
    CHECK (CaptureExporter::getDefaultTakesFolder().getFileName() == "Takes");
    CHECK (CaptureExporter::getDefaultTakesFolder().getParentDirectory().getFileName() == "VocalSynth");

    const auto id = CaptureExporter::makeInstanceId();
    CHECK (id.length() == 16);
    CHECK (id.containsOnly ("0123456789abcdef"));
    CHECK (id != CaptureExporter::makeInstanceId());
}

TEST_CASE ("CaptureExporter: file names follow docs/M3-SPEC.md 8", "[plugin][captureexporter]")
{
    const auto take = freeTake (3);
    RenderOptions options;
    const auto rendered = TakeRenderer::render (take, options);

    // Free takes: no bar, never a quantize tag
    CHECK (CaptureExporter::makeBaseName (take, rendered) == "VocalSynth_2026-10-17_140322_Take03");
    CHECK (CaptureExporter::makeMidiFileName (take, rendered, options) == "VocalSynth_2026-10-17_140322_Take03_Clean.mid");
    options.quantize = QuantizeGrid::sixteenth;
    CHECK (CaptureExporter::makeMidiFileName (take, rendered, options) == "VocalSynth_2026-10-17_140322_Take03_Clean.mid");
    CHECK (CaptureExporter::makeWavFileName (take, rendered) == "VocalSynth_2026-10-17_140322_Take03_Voice.wav");

    // Synced: bar number (3 digits), version, quantize tag with the strength below 100 %
    auto synced = take;
    synced.mode = TakeMode::synced;
    synced.number = 12;
    RenderedTake r;
    r.originBar = 17;
    options = {};
    options.version = ExportVersion::raw;
    CHECK (CaptureExporter::makeMidiFileName (synced, r, options) == "VocalSynth_2026-10-17_140322_Take12_Bar017_Raw.mid");
    options.quantize = QuantizeGrid::sixteenth;
    CHECK (CaptureExporter::makeMidiFileName (synced, r, options).endsWith ("_Take12_Bar017_Raw_Q16.mid"));
    options.quantize = QuantizeGrid::eighthTriplet;
    options.quantizeStrengthPercent = 50.0;
    CHECK (CaptureExporter::makeMidiFileName (synced, r, options).endsWith ("_Bar017_Raw_Q8T-50.mid"));
    r.originBar = 1234;
    CHECK (CaptureExporter::makeWavFileName (synced, r).endsWith ("_Take12_Bar1234_Voice.wav"));

    // ASCII, no spaces
    const std::regex pattern ("^VocalSynth_\\d{4}-\\d{2}-\\d{2}_\\d{6}_Take\\d{2,}(_Bar\\d{3,})?_(Raw|Clean)(_Q[0-9T]+(-\\d+)?)?\\.mid$");
    CHECK (std::regex_match (CaptureExporter::makeMidiFileName (synced, r, options).toStdString(), pattern));
}

TEST_CASE ("CaptureExporter: a free take as .mid is readable by juce::MidiFile", "[plugin][captureexporter][file]")
{
    TempFolder dir;
    const auto take = freeTake();
    const auto file = CaptureExporter::writeMidi (take, RenderOptions {}, SmfFormat::type1, dir.folder);
    REQUIRE (file.existsAsFile());
    CHECK (file.getParentDirectory() == dir.folder);

    juce::MidiFile midi;
    REQUIRE (vocalsynth::test::readMidiFile (file, midi));
    CHECK (midi.getTimeFormat() == 960);
    REQUIRE (midi.getNumTracks() == 1);

    // Tick 0: track name, tempo 120, 4/4 (M1 order)
    const auto& track = *midi.getTrack (0);
    REQUIRE (track.getNumEvents() >= 3);
    CHECK (track.getEventPointer (0)->message.isTrackNameEvent());
    CHECK (track.getEventPointer (0)->message.getTextFromTextMetaEvent() == "VocalSynth Take 3 Clean");
    CHECK (track.getEventPointer (1)->message.isTempoMetaEvent());
    CHECK (track.getEventPointer (1)->message.getTempoSecondsPerQuarterNote() == Catch::Approx (0.5));
    CHECK (track.getEventPointer (2)->message.isTimeSignatureMetaEvent());

    for (int i = 0; i < 3; ++i)
        CHECK (track.getEventPointer (i)->message.getTimeStamp() == 0.0);

    // Notes: 0.5 s at 120 BPM = 960 ticks apart, 480 long; tick 0 = the first note-on (pre-roll 0)
    const auto notes = vocalsynth::test::notesOf (midi);
    REQUIRE (notes.size() == 3);

    for (std::size_t i = 0; i < 3; ++i)
    {
        CHECK (notes[i].note == 60 + (int) i);
        CHECK (notes[i].onTick == Catch::Approx (960.0 * (double) i * 2.0));
        CHECK (notes[i].offTick == Catch::Approx (notes[i].onTick + 960.0));
    }

    // No RPN without bends (bend mode Off), end-of-track right after the last event (free)
    for (int i = 0; i < track.getNumEvents(); ++i)
        CHECK_FALSE (track.getEventPointer (i)->message.isController());

    const auto* last = track.getEventPointer (track.getNumEvents() - 1);
    CHECK (last->message.isEndOfTrackMetaEvent());
    CHECK (last->message.getTimeStamp() == Catch::Approx (notes.back().offTick));

    // Byte-identical for an identical take
    const auto again = CaptureExporter::writeMidi (take, RenderOptions {}, SmfFormat::type1, dir.folder);
    juce::MemoryBlock a, b;
    file.loadFileAsData (a);
    again.loadFileAsData (b);
    CHECK (a == b);
}

TEST_CASE ("CaptureExporter: a synced take with bends writes RPN 0 and ends on a bar line", "[plugin][captureexporter][file]")
{
    TempFolder dir;
    const auto instance = CaptureExporter::getInstanceFolder ("test" + CaptureExporter::makeInstanceId().substring (4));
    CaptureTap tap;
    CaptureWorker worker (tap, instance);
    const auto take = syncedTake (tap, worker);
    REQUIRE (take != nullptr);
    REQUIRE (take->mode == TakeMode::synced);
    CHECK (take->settings.bendRangeSemitones == 12);

    RenderOptions options; // bends: As plugin = on (True)
    options.roundTripSamples = TakeRenderer::roundTripFor (*take, CaptureOffsetSettings { CaptureOffsetMode::off, 0.0, 0.0 });
    const auto file = CaptureExporter::writeMidi (*take, options, SmfFormat::type1, dir.folder);
    REQUIRE (file.existsAsFile());
    CAPTURE (file.getFileName());
    CHECK (file.getFileName().contains ("_Take01_Bar003_Clean"));

    juce::MidiFile midi;
    REQUIRE (vocalsynth::test::readMidiFile (file, midi));
    const auto& track = *midi.getTrack (0);

    // RPN 0 = 12 semitones at tick 0: CC 101 0, CC 100 0, CC 6 12, CC 38 0, then the null RPN
    std::vector<std::pair<int, int>> controllers;
    int bends = 0;

    for (int i = 0; i < track.getNumEvents(); ++i)
    {
        const auto& m = track.getEventPointer (i)->message;

        if (m.isController() && m.getTimeStamp() == 0.0)
            controllers.emplace_back (m.getControllerNumber(), m.getControllerValue());

        if (m.isPitchWheel())
            ++bends;
    }

    REQUIRE (controllers.size() >= 4);
    CHECK (controllers[0] == std::pair { 101, 0 });
    CHECK (controllers[1] == std::pair { 100, 0 });
    CHECK (controllers[2] == std::pair { 6, 12 });
    CHECK (controllers[3] == std::pair { 38, 0 });
    CHECK (bends >= 6);

    // Notes on the beats from tick 0 = bar 3, all paired; end-of-track at the end of bar 4 (2 bars)
    const auto notes = vocalsynth::test::notesOf (midi);
    REQUIRE (notes.size() == 6);

    for (std::size_t i = 0; i < notes.size(); ++i)
    {
        CHECK (notes[i].onTick == Catch::Approx (960.0 * (double) i));
        CHECK (notes[i].offTick == Catch::Approx (notes[i].onTick + 480.0));
    }

    const auto* last = track.getEventPointer (track.getNumEvents() - 1);
    CHECK (last->message.isEndOfTrackMetaEvent());
    CHECK (last->message.getTimeStamp() == Catch::Approx (2.0 * 4.0 * 960.0));

    // Bends off: no RPN, no pitch wheel
    options.bends = BendExport::off;
    const auto plain = CaptureExporter::writeMidi (*take, options, SmfFormat::type1, dir.folder);
    juce::MidiFile midiPlain;
    REQUIRE (vocalsynth::test::readMidiFile (plain, midiPlain));

    for (int i = 0; i < midiPlain.getTrack (0)->getNumEvents(); ++i)
    {
        const auto& m = midiPlain.getTrack (0)->getEventPointer (i)->message;
        CHECK_FALSE (m.isPitchWheel());
        CHECK_FALSE (m.isController());
    }

    worker.stop();
    CaptureExporter::deleteInstanceFolder (instance);
}

TEST_CASE ("CaptureExporter: WAV files are readable, sliced writing = one call", "[plugin][captureexporter][file]")
{
    TempFolder dir;
    const auto format = GENERATE (WavSampleFormat::pcm24, WavSampleFormat::float32);
    const auto numFrames = GENERATE ((uint64_t) 1, (uint64_t) 70001, (uint64_t) 200000); // odd counts: pad byte
    CAPTURE ((int) format, numFrames);

    const auto signal = [] (uint64_t i)
    { return 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * 440.0 * (double) i / rate); };
    const auto file = CaptureExporter::writeWav (dir.folder.getChildFile ("one.wav"), rate, format, numFrames,
                                                 [&signal] (uint64_t first, std::span<float> out)
                                                 {
                                                     for (std::size_t i = 0; i < out.size(); ++i)
                                                         out[i] = signal (first + i);
                                                 });
    REQUIRE (file.existsAsFile());

    double wavRate = 0.0;
    int bits = 0;
    const auto audio = vocalsynth::test::readWav (file, wavRate, bits);
    CHECK (wavRate == Catch::Approx (rate));
    CHECK (bits == (format == WavSampleFormat::pcm24 ? 24 : 32));
    REQUIRE (audio.getNumChannels() == 1);
    CHECK ((uint64_t) audio.getNumSamples() == numFrames);

    for (int i = 0; i < audio.getNumSamples(); i += 997)
        CHECK (audio.getSample (0, i) == Catch::Approx (signal ((uint64_t) i)).margin (format == WavSampleFormat::pcm24 ? 2.0e-7 : 0.0));

    // The same samples in irregular slices with WavFileWriter give the same bytes
    CaptureExporter::WavFileWriter writer;
    REQUIRE (writer.open (dir.folder.getChildFile ("sliced.wav"), rate, format, numFrames));
    std::vector<float> block;
    uint64_t pos = 0;

    for (std::size_t slice = 1; pos < numFrames; slice = slice * 3 + 1)
    {
        block.resize ((std::size_t) juce::jmin<uint64_t> (slice, numFrames - pos));

        for (std::size_t i = 0; i < block.size(); ++i)
            block[i] = signal (pos + i);

        REQUIRE (writer.write (block));
        pos += block.size();
    }

    CHECK_FALSE (writer.write (block)); // nothing beyond numFrames
    const auto sliced = writer.finish();
    REQUIRE (sliced.existsAsFile());
    juce::MemoryBlock a, b;
    file.loadFileAsData (a);
    sliced.loadFileAsData (b);
    CHECK (a == b);
}

TEST_CASE ("CaptureExporter: WavFileWriter abort and short writes leave no file", "[plugin][captureexporter][file]")
{
    TempFolder dir;
    const auto target = dir.folder.getChildFile ("take.wav");

    {
        CaptureExporter::WavFileWriter writer;
        REQUIRE (writer.open (target, rate, WavSampleFormat::pcm24, 1000));
        CHECK (writer.isOpen());
        const std::vector<float> half (500, 0.1f);
        REQUIRE (writer.write (half));
        CHECK (writer.finish() == juce::File()); // 500 of 1000 frames: not a valid file
        CHECK_FALSE (writer.isOpen());
    }

    {
        CaptureExporter::WavFileWriter writer;
        REQUIRE (writer.open (target, rate, WavSampleFormat::pcm24, 1000));
        writer.abort();
    }

    {
        CaptureExporter::WavFileWriter writer; // destroyed while open
        REQUIRE (writer.open (target, rate, WavSampleFormat::float32, 1000));
    }

    CHECK_FALSE (target.exists());
    CHECK (dir.folder.getNumberOfChildFiles (juce::File::findFilesAndDirectories) == 0);
}

TEST_CASE ("CaptureExporter: persistWav moves into the takes folder and never overwrites", "[plugin][captureexporter][file]")
{
    TempFolder dir;
    const auto cache = dir.folder.getChildFile ("cache");
    const auto takes = dir.folder.getChildFile ("Takes"); // created by persistWav
    REQUIRE (cache.createDirectory());

    const auto cached = cache.getChildFile ("VocalSynth_x_Take01_Voice.wav");
    REQUIRE (cached.replaceWithText ("first"));
    const auto persisted = CaptureExporter::persistWav (cached, takes);
    REQUIRE (persisted.existsAsFile());
    CHECK (persisted == takes.getChildFile (cached.getFileName()));
    CHECK_FALSE (cached.exists());

    // Same name again (another instance, a re-render): numbered sibling, the first file is kept
    REQUIRE (cached.replaceWithText ("second"));
    const auto second = CaptureExporter::persistWav (cached, takes);
    REQUIRE (second.existsAsFile());
    CHECK (second != persisted);
    CHECK (persisted.loadFileAsString() == "first");
    CHECK (second.loadFileAsString() == "second");

    CHECK (CaptureExporter::persistWav (cache.getChildFile ("missing.wav"), takes) == juce::File());
}

TEST_CASE ("CaptureExporter: heartbeat, stale-folder cleanup and instance folder deletion", "[plugin][captureexporter][file]")
{
    TempFolder dir;
    const auto root = dir.folder;

    const auto own = root.getChildFile ("own");
    const auto stale = root.getChildFile ("stale");
    const auto staleNoHeartbeat = root.getChildFile ("staleNoHeartbeat");
    const auto fresh = root.getChildFile ("fresh");

    for (const auto& f : { own, stale, staleNoHeartbeat, fresh })
        REQUIRE (f.createDirectory());

    const auto old = juce::Time::getCurrentTime() - juce::RelativeTime::hours (25.0);

    for (const auto& f : { own, stale })
    {
        CaptureExporter::touchHeartbeat (f);
        REQUIRE (f.getChildFile (CaptureExporter::heartbeatFileName).setLastModificationTime (old));
    }

    REQUIRE (staleNoHeartbeat.getChildFile ("x.mid").replaceWithText ("x"));
    REQUIRE (staleNoHeartbeat.setLastModificationTime (old));
    CaptureExporter::touchHeartbeat (fresh);
    CHECK (fresh.getChildFile (CaptureExporter::heartbeatFileName).existsAsFile());

    // touchHeartbeat never creates the folder
    CaptureExporter::touchHeartbeat (root.getChildFile ("never"));
    CHECK_FALSE (root.getChildFile ("never").exists());

    CHECK (CaptureExporter::deleteStaleInstanceFolders (root, "own") == 2);
    CHECK (own.isDirectory());
    CHECK (fresh.isDirectory());
    CHECK_FALSE (stale.exists());
    CHECK_FALSE (staleNoHeartbeat.exists());

    // deleteInstanceFolder only deletes below the capture root
    CaptureExporter::deleteInstanceFolder (fresh);
    CHECK (fresh.isDirectory());

    const auto instance = CaptureExporter::getInstanceFolder ("test" + CaptureExporter::makeInstanceId().substring (4));
    REQUIRE (instance.getChildFile ("a").getChildFile ("b.wav").create().wasOk());
    CaptureExporter::deleteInstanceFolder (instance);
    CHECK_FALSE (instance.exists());
}
