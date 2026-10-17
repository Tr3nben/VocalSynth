#include "Parameters.h"
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "TunerDisplayModel.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cmath>
#include <functional>
#include <vector>

using namespace vocalsynth;
using vocalsynth::VoiceRange;

// Editor smoke tests: build, layout and paint headless (software renderer, no window)

namespace
{
juce::StringArray collectLabelTexts (juce::Component& root)
{
    juce::StringArray labelTexts;
    std::function<void (juce::Component&)> collect = [&] (juce::Component& c)
    {
        if (auto* label = dynamic_cast<juce::Label*> (&c))
            labelTexts.add (label->getText());

        for (auto* child : c.getChildren())
            collect (*child);
    };
    collect (root);
    return labelTexts;
}

/** Every visible child lies inside the editor, and visible siblings do not overlap. */
void checkLayout (juce::Component& editor)
{
    std::vector<juce::Component*> visible;

    for (auto* child : editor.getChildren())
        if (child->isVisible() && dynamic_cast<juce::TooltipWindow*> (child) == nullptr)
        {
            CHECK (editor.getLocalBounds().contains (child->getBounds()));
            CHECK_FALSE (child->getBounds().isEmpty());
            visible.push_back (child);
        }

    for (std::size_t i = 0; i < visible.size(); ++i)
        for (std::size_t j = i + 1; j < visible.size(); ++j)
        {
            CAPTURE (visible[i]->getComponentID(), visible[j]->getComponentID(), visible[i]->getBounds().toString(),
                     visible[j]->getBounds().toString());
            CHECK_FALSE (visible[i]->getBounds().intersects (visible[j]->getBounds()));
        }
}
} // namespace

TEST_CASE ("Editor builds for both variants and shows all three pages", "[editor]")
{
    const auto variant = GENERATE (Variant::fx, Variant::instrument);
    CAPTURE (variant == Variant::fx ? "fx" : "instrument");

    VocalSynthProcessor processor (variant);
    processor.prepareToPlay (48000.0, 512);

    // Some blocks first, so the constructor shows a filled histogram, counters and a test note
    if (auto* source = processor.getParameters().getParameter (params::source.getParamID()))
        source->setValueNotifyingHost (source->convertTo0to1 ((float) params::Source::testPattern));

    if (auto* pattern = processor.getParameters().getParameter (params::testPattern.getParamID()))
        pattern->setValueNotifyingHost (pattern->convertTo0to1 ((float) TestPattern::notes));

    juce::AudioBuffer<float> buffer (juce::jmax (processor.getTotalNumInputChannels(),
                                                 processor.getTotalNumOutputChannels()),
                                     512);
    juce::MidiBuffer midi;

    for (const auto blockSize : { 512, 512, 512, 64, 7, 1 })
    {
        buffer.clear();
        juce::AudioBuffer<float> view (buffer.getArrayOfWritePointers(), buffer.getNumChannels(), blockSize);
        processor.processBlock (view, midi);
    }

    std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());
    REQUIRE (editor != nullptr);
    CHECK (dynamic_cast<juce::DragAndDropContainer*> (editor.get()) != nullptr);
    CHECK (editor->getWidth() == VocalSynthEditor::defaultWidth);
    CHECK (editor->getHeight() == VocalSynthEditor::defaultHeight);

    auto* vsEditor = dynamic_cast<VocalSynthEditor*> (editor.get());
    REQUIRE (vsEditor != nullptr);
    CHECK (vsEditor->getPage() == VocalSynthEditor::Page::voice); // the main panel comes first

    for (const auto page : { VocalSynthEditor::Page::voice, VocalSynthEditor::Page::testAndRouting,
                             VocalSynthEditor::Page::capture })
    {
        const auto isVoice = page == VocalSynthEditor::Page::voice;
        CAPTURE ((int) page);
        vsEditor->showPage (page);
        CHECK (vsEditor->getPage() == page);
        checkLayout (*editor);

        // Painting must not throw or assert (software renderer, no window needed)
        const auto image = editor->createComponentSnapshot (editor->getLocalBounds());
        CHECK (image.getWidth() == VocalSynthEditor::defaultWidth);

        // For a visual check of the layout: VOCALSYNTH_EDITOR_SNAPSHOT_DIR=<dir> writes a PNG per variant and page
        if (const auto dir = juce::SystemStats::getEnvironmentVariable ("VOCALSYNTH_EDITOR_SNAPSHOT_DIR", {});
            dir.isNotEmpty())
        {
            const auto name = juce::String (variant == Variant::fx ? "editor-fx" : "editor-instrument")
                              + (isVoice ? "-voice.png" : page == VocalSynthEditor::Page::capture ? "-capture.png" : "-test.png");
            const auto file = juce::File (dir).getChildFile (name);
            file.deleteFile();
            juce::FileOutputStream out (file);
            juce::PNGImageFormat().writeImageToStream (image, out);
        }
    }

    editor.reset();
    processor.releaseResources();
}

TEST_CASE ("Editor: the Capture page shows the capture panel only", "[editor][capture]")
{
    const auto variant = GENERATE (Variant::fx, Variant::instrument);
    VocalSynthProcessor processor (variant);
    processor.prepareToPlay (48000.0, 512);

    std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());
    auto* vsEditor = dynamic_cast<VocalSynthEditor*> (editor.get());
    REQUIRE (vsEditor != nullptr);

    auto& panel = vsEditor->getCapturePanel();
    CHECK_FALSE (panel.isVisible());
    vsEditor->showPage (VocalSynthEditor::Page::capture);
    CHECK (panel.isVisible());
    CHECK (editor->getLocalBounds().contains (panel.getBounds()));
    CHECK (panel.getHeight() >= 330);
    CHECK (panel.getStatusText().isNotEmpty());

    // The Voice and Test & Routing controls are hidden on the Capture page
    for (auto* c : editor->getChildren())
        if (c != &panel && c->isVisible() && dynamic_cast<juce::TextButton*> (c) == nullptr
            && dynamic_cast<juce::TooltipWindow*> (c) == nullptr)
            FAIL_CHECK ("visible on the Capture page: " << c->getComponentID() << " " << c->getBounds().toString());

    vsEditor->showPage (VocalSynthEditor::Page::voice);
    CHECK_FALSE (panel.isVisible());

    // Version text (header): "Version 0.3.0 - M3 capture" (en dash)
    CHECK (vsEditor->getVersionText().startsWith ("Version 0.3.0 " + juce::String (juce::CharPointer_UTF8 ("\xe2\x80\x93"))
                                                  + " M3 capture"));
    CHECK (juce::String (VOCALSYNTH_VERSION_STRING) == "0.3.0");

    editor.reset();
    processor.releaseResources();
}

TEST_CASE ("Editor shows the plugin format", "[editor]")
{
    CHECK (VocalSynthEditor::getFormatName (juce::AudioProcessor::wrapperType_VST3) == "VST3");
    CHECK (VocalSynthEditor::getFormatName (juce::AudioProcessor::wrapperType_Undefined) == "CLAP");
    CHECK (VocalSynthEditor::getFormatName (juce::AudioProcessor::wrapperType_Standalone) == "Standalone");
}

TEST_CASE ("Editor control labels match the names used in FL-ROUTING.md and LIESMICH.txt", "[editor]")
{
    VocalSynthProcessor processor (Variant::fx);
    std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());
    REQUIRE (editor != nullptr);

    const auto labelTexts = collectLabelTexts (*editor);

    // The docs (FL-ROUTING.md, M2-TESTING.md, LIESMICH.txt) refer to these controls by name
    for (const auto* expected : { "Test pattern", "MIDI channel", "Test tempo (BPM)", "CC number", "Source",
                                  "Voice Range", "Algorithm", "Key", "Scale", "Bend Mode", "Bend Range", "Stickiness",
                                  "Gate", "Min Note", "Velocity Sens.", "Octave" })
    {
        CAPTURE (expected);
        CHECK (labelTexts.contains (expected));
    }

    // Toggle buttons and page buttons
    juce::StringArray buttonTexts;
    for (auto* child : editor->getChildren())
        if (auto* button = dynamic_cast<juce::Button*> (child))
            buttonTexts.add (button->getButtonText());

    for (const auto* expected : { "Legato", "Monitor Tone", "Virtual MIDI Out", "Mute Dry", "Restart pattern", "Voice",
                                  "Test & Routing" })
    {
        CAPTURE (expected);
        CHECK (buttonTexts.contains (expected));
    }
}

TEST_CASE ("Every M2 parameter has a control on the Voice page", "[editor]")
{
    VocalSynthProcessor processor (Variant::fx);
    std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());
    auto* vsEditor = dynamic_cast<VocalSynthEditor*> (editor.get());
    REQUIRE (vsEditor != nullptr);
    vsEditor->showPage (VocalSynthEditor::Page::voice);

    int comboBoxes = 0, sliders = 0, toggles = 0;
    for (auto* child : editor->getChildren())
    {
        if (! child->isVisible())
            continue;

        comboBoxes += dynamic_cast<juce::ComboBox*> (child) != nullptr ? 1 : 0;
        sliders += dynamic_cast<juce::Slider*> (child) != nullptr ? 1 : 0;
        toggles += dynamic_cast<juce::ToggleButton*> (child) != nullptr ? 1 : 0;
    }

    CHECK (comboBoxes == 7); // Source, Voice Range, Algorithm, Key, Scale, Bend Mode, Bend Range
    CHECK (sliders == 5);    // Stickiness, Gate, Min Note, Velocity Sens., Octave
    CHECK (toggles == 1);    // Legato

    // A control changes its parameter (the attachments are connected)
    juce::ComboBox* rangeBox = nullptr;
    for (auto* child : editor->getChildren())
        if (auto* box = dynamic_cast<juce::ComboBox*> (child); box != nullptr && box->getNumItems() == 7)
            rangeBox = box;

    REQUIRE (rangeBox != nullptr);
    CHECK (rangeBox->getSelectedItemIndex() == 2); // Tenor
    rangeBox->setSelectedItemIndex (0, juce::sendNotificationSync);
    CHECK (processor.getVoiceSettings().range == VoiceRange::bass);
}

TEST_CASE ("Editor shows the latency and follows the analysis", "[editor]")
{
    VocalSynthProcessor processor (Variant::fx);
    processor.prepareToPlay (44100.0, 512);
    std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());
    REQUIRE (editor != nullptr);

    const auto latency = VocalSynthEditor::formatLatency (processor.getVoiceLatencyMs());
    bool found = false;
    for (const auto& text : collectLabelTexts (*editor))
        found = found || (text.contains ("Analysis") && text.contains (latency) && text.contains ("Tenor"));

    CHECK (found);
    CHECK (VocalSynthEditor::formatLatency (24.21) == "24.2 ms");

    // Process a little silence, then repaint the voice page with the new snapshot
    juce::AudioBuffer<float> buffer (2, 512);
    juce::MidiBuffer midi;
    for (int i = 0; i < 20; ++i)
    {
        buffer.clear();
        processor.processBlock (buffer, midi);
    }

    const auto image = editor->createComponentSnapshot (editor->getLocalBounds());
    CHECK (image.getWidth() == VocalSynthEditor::defaultWidth);
}

TEST_CASE ("Note names follow FL Studio (C5 = 60)", "[editor]")
{
    CHECK (VocalSynthEditor::getNoteName (60) == "C5");
    CHECK (VocalSynthEditor::getNoteName (69) == "A5"); // 440 Hz
    CHECK (VocalSynthEditor::getNoteName (61) == "C#5");
    CHECK (VocalSynthEditor::getNoteName (0) == "C0");
    CHECK (VocalSynthEditor::getNoteName (127) == "G10");
    CHECK (VocalSynthEditor::getNoteName (-1) == "-");
    CHECK (VocalSynthEditor::getNoteName (128) == "-");
}

//==============================================================================
// v0.2.1: calm tuner, latency box, DSP load

namespace
{
constexpr double tickMs = 1000.0 / 30.0; // the editor's timer rate

float hzOfNote (double note)
{
    return (float) (440.0 * std::exp2 ((note - 69.0) / 12.0));
}

/** A voiced frame at a fractional MIDI note (nearest note computed like the analysis does). */
TunerDisplayModel::Input pitchInput (double note, int sungNote = -1)
{
    TunerDisplayModel::Input in;
    in.frequencyHz = hzOfNote (note);
    in.nearestNote = (int) std::lround (note);
    in.sungNote = sungNote;
    in.confidence = 0.95f;
    in.voiced = true;
    return in;
}

/** Runs the model for durationMs in steps of stepMs with the same input. */
const TunerDisplayModel::State& run (TunerDisplayModel& model, const TunerDisplayModel::Input& in, double durationMs,
                                     double stepMs = tickMs)
{
    for (double t = 0.0; t < durationMs - 1.0e-9; t += stepMs)
        model.update (in, juce::jmin (stepMs, durationMs - t));

    return model.getState();
}
} // namespace

TEST_CASE ("Tuner: the sent note is shown at once and sticks", "[editor][tuner]")
{
    TunerDisplayModel model;

    // Sung 0.6 semitones above C5 (nearest C#5), but VocalSynth holds C5 (stickiness)
    const auto& s = model.update (pitchInput (60.6, 60), tickMs);
    CHECK (s.note == 60);
    CHECK (s.noteIsSegmented);
    CHECK (s.noteAlpha == 1.0f);
    CHECK (s.needleCents == Catch::Approx (TunerDisplayModel::maxNeedleCents)); // clamped, snapped on appearance
    CHECK (s.displayHz == Catch::Approx (hzOfNote (60.6)).epsilon (1.0e-4));
}

TEST_CASE ("Tuner: without a sent note, the nearest note needs to be stable", "[editor][tuner]")
{
    TunerDisplayModel model;

    // A glide that flips between two notes every tick is never named
    for (int i = 0; i < 30; ++i)
        model.update (pitchInput (i % 2 == 0 ? 60.4 : 60.6), tickMs);

    CHECK (model.getState().note == -1);

    // A steady note is named after noteStableMs, not before
    model.reset();
    model.update (pitchInput (62.1), tickMs); // first sighting
    run (model, pitchInput (62.1), TunerDisplayModel::noteStableMs - 10.0, 10.0);
    CHECK (model.getState().note == -1);
    run (model, pitchInput (62.1), 20.0, 10.0);
    CHECK (model.getState().note == 62);
    CHECK_FALSE (model.getState().noteIsSegmented);

    // Once a note is shown, a sung note that still matches it continues without waiting
    model.update (pitchInput (62.0, 62), tickMs);
    model.update (pitchInput (62.0), tickMs); // note-off, still singing D5
    CHECK (model.getState().note == 62);
}

TEST_CASE ("Tuner: the name is held after the voice stops, then fades to a dash", "[editor][tuner]")
{
    TunerDisplayModel model;
    run (model, pitchInput (57.0, 57), 300.0);
    REQUIRE (model.getState().note == 57);

    const TunerDisplayModel::Input silence;
    run (model, silence, TunerDisplayModel::noteHoldMs - 20.0, 10.0);
    CHECK (model.getState().note == 57);
    CHECK (model.getState().noteAlpha == 1.0f);

    run (model, silence, 20.0 + TunerDisplayModel::noteFadeMs / 2.0, 10.0);
    CHECK (model.getState().note == 57);
    CHECK (model.getState().noteAlpha > 0.0f);
    CHECK (model.getState().noteAlpha < 1.0f);

    run (model, silence, TunerDisplayModel::noteFadeMs, 10.0);
    CHECK (model.getState().note == -1);
    CHECK (model.getState().displayHz == 0.0f);

    // Coming back after the fade: the needle starts at its target (no sweep from the old position)
    const auto& s = model.update (pitchInput (64.3, 64), tickMs);
    CHECK (s.note == 64);
    CHECK (s.needleCents == Catch::Approx (30.0f).margin (0.01));
}

TEST_CASE ("Tuner: the needle is smoothed, independent of the frame rate", "[editor][tuner]")
{
    // Step from -30 to +30 cents on a held A4: one time constant later the needle is 63 % of the way
    for (const auto stepMs : { 5.0, 16.0, tickMs, 50.0 })
    {
        CAPTURE (stepMs);
        TunerDisplayModel model;
        model.update (pitchInput (57.0 - 0.3, 57), tickMs);
        REQUIRE (model.getState().needleCents == Catch::Approx (-30.0f).margin (0.01));

        run (model, pitchInput (57.3, 57), 120.0, stepMs);
        const auto expected = -30.0 + 60.0 * (1.0 - std::exp (-120.0 / TunerDisplayModel::needleTimeConstantMs));
        CHECK (model.getState().needleCents == Catch::Approx (expected).margin (0.05));
    }

    // Vibrato +-50 cents at 5.5 Hz shows as roughly +-12 cents
    TunerDisplayModel model;
    float minCents = 100.0f, maxCents = -100.0f;
    for (int i = 0; i < 30 * 4; ++i)
    {
        const auto t = (double) i * tickMs / 1000.0;
        const auto& s = model.update (pitchInput (57.0 + 0.5 * std::sin (juce::MathConstants<double>::twoPi * 5.5 * t), 57),
                                      tickMs);
        if (i >= 30) // after the start-up
        {
            minCents = juce::jmin (minCents, s.needleCents);
            maxCents = juce::jmax (maxCents, s.needleCents);
        }
    }
    CHECK (maxCents < 20.0f);
    CHECK (minCents > -20.0f);
}

TEST_CASE ("Tuner: a note change glides the needle instead of jumping", "[editor][tuner]")
{
    TunerDisplayModel model;
    run (model, pitchInput (60.45, 60), 500.0); // C5 held, needle at +45
    REQUIRE (model.getState().needleCents == Catch::Approx (45.0f).margin (0.5));

    // The sent note changes to C#5; sung pitch -40 cents from it
    const auto before = model.getState().needleCents;
    const auto& s = model.update (pitchInput (60.6, 61), tickMs);
    CHECK (s.note == 61);
    const auto maxStep = 85.0f * TunerDisplayModel::smoothingCoefficient (tickMs, TunerDisplayModel::needleTimeConstantMs);
    CHECK (std::abs (s.needleCents - before) <= maxStep + 0.01f);
}

TEST_CASE ("Tuner: Hz and cents texts change at most five times per second", "[editor][tuner]")
{
    TunerDisplayModel model;
    int hzChanges = 0;
    float lastHz = -1.0f;

    for (int i = 0; i < 30 * 3; ++i) // 3 s of vibrato
    {
        const auto t = (double) i * tickMs / 1000.0;
        const auto& s = model.update (pitchInput (57.0 + 0.3 * std::sin (juce::MathConstants<double>::twoPi * 5.5 * t), 57),
                                      tickMs);
        if (! juce::exactlyEqual (s.displayHz, lastHz))
            ++hzChanges;
        lastHz = s.displayHz;
    }

    CHECK (hzChanges <= 3 * 5 + 1);
    CHECK (hzChanges >= 3 * 4);

    CHECK (VocalSynthEditor::formatTunerHz (261.63f) == "262 Hz");
    CHECK (VocalSynthEditor::formatTunerHz (110.0f) == "110 Hz");
    CHECK (VocalSynthEditor::formatTunerHz (1046.5f) == "1.05 kHz");
    CHECK (VocalSynthEditor::formatTunerHz (0.0f) == "- Hz");
}

TEST_CASE ("Tuner: Hz and cents texts follow a note change at once", "[editor][tuner]")
{
    // v0.2.1 review: the Hz text came from the lagged smoothed pitch, so A3 -> A4 read "A4, 260 Hz"
    // for 200 ms and reached 432 Hz only after 400 ms
    TunerDisplayModel model;
    run (model, pitchInput (57.0, 57), 500.0); // A3 (FL: A4), 220 Hz
    REQUIRE (model.getState().displayHz == Catch::Approx (220.0f).margin (0.5f));

    const auto& s = model.update (pitchInput (69.1, 69), tickMs); // A4 (FL: A5), 10 cents sharp
    CHECK (s.note == 69);
    CHECK (s.displayHz == Catch::Approx (hzOfNote (69.1)).epsilon (1.0e-4));
    CHECK (s.displayCents == 10);
    CHECK (std::abs (s.needleCents) < 10.0f + 1.0f); // the needle still glides (from 0 towards +10)

    // Within the note the texts are stepped again
    const auto shownHz = s.displayHz;
    model.update (pitchInput (69.4, 69), tickMs);
    CHECK (juce::exactlyEqual (model.getState().displayHz, shownHz));
}

TEST_CASE ("Tuner: voiced LED attacks at once and holds on release", "[editor][tuner]")
{
    TunerDisplayModel model;
    auto in = pitchInput (57.0, 57);
    CHECK (model.update (in, tickMs).ledOn);

    in.voiced = false;
    in.confidence = 0.2f;
    run (model, in, TunerDisplayModel::ledHoldMs - 20.0, 10.0);
    CHECK (model.getState().ledOn);
    run (model, in, 30.0, 10.0);
    CHECK_FALSE (model.getState().ledOn);

    // Confidence: fast up, slower down
    TunerDisplayModel bar;
    auto loud = pitchInput (57.0, 57);
    run (bar, loud, 50.0, 10.0);
    CHECK (bar.getState().confidence > 0.9f);
    loud.confidence = 0.0f;
    run (bar, loud, 50.0, 10.0);
    CHECK (bar.getState().confidence > 0.6f);

    // Inactive (Test Pattern): everything off at once
    TunerDisplayModel::Input inactive = pitchInput (57.0, 57);
    inactive.active = false;
    const auto& off = bar.update (inactive, tickMs);
    CHECK (off.note == -1);
    CHECK (off.noteAlpha == 0.0f);
    CHECK (off.displayHz == 0.0f);
    CHECK_FALSE (off.ledOn);
    CHECK (off.confidence == 0.0f);
}

TEST_CASE ("Latency box: buffer, total and the large-buffer warning", "[editor][latency]")
{
    const auto big = VocalSynthEditor::describeLatency (24.2, 2048, 44100.0);
    CHECK (big.bufferText == "Buffer 2048 smp = 46.4 ms");
    CHECK (big.totalText.contains ("117 ms")); // 24.2 + 2 x 46.4
    CHECK (big.totalText.contains ("+46 ms"));  // FL MIDI port: about one more buffer
    CHECK (big.largeBuffer);

    const auto small = VocalSynthEditor::describeLatency (24.2, 256, 48000.0);
    CHECK (small.bufferText == "Buffer 256 smp = 5.3 ms");
    CHECK (small.totalText.contains ("35 ms"));
    CHECK_FALSE (small.largeBuffer);
    CHECK_FALSE (VocalSynthEditor::describeLatency (24.2, VocalSynthEditor::largeBufferSamples, 48000.0).largeBuffer);
    CHECK (VocalSynthEditor::describeLatency (24.2, VocalSynthEditor::largeBufferSamples + 1, 48000.0).largeBuffer);

    const auto none = VocalSynthEditor::describeLatency (24.2, 0, 0.0);
    CHECK (none.bufferText.contains ("no audio yet"));
    CHECK_FALSE (none.largeBuffer);
    CHECK (VocalSynthEditor::largeBufferHint().contains ("128 or 256 samples"));
}

TEST_CASE ("Editor shows the buffer warning with a large host buffer", "[editor][latency]")
{
    const auto variant = GENERATE (Variant::fx, Variant::instrument);
    const auto blockSize = GENERATE (2048, 256);
    CAPTURE (variant == Variant::fx ? "fx" : "instrument", blockSize);

    VocalSynthProcessor processor (variant);
    processor.prepareToPlay (44100.0, blockSize);

    juce::AudioBuffer<float> buffer (juce::jmax (processor.getTotalNumInputChannels(),
                                                 processor.getTotalNumOutputChannels()),
                                     blockSize);
    juce::MidiBuffer midi;
    for (int i = 0; i < 4; ++i)
    {
        buffer.clear();
        processor.processBlock (buffer, midi);
    }

    std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());
    REQUIRE (editor != nullptr);
    checkLayout (*editor);

    const auto texts = collectLabelTexts (*editor);
    CHECK (texts.contains (VocalSynthEditor::largeBufferHint()) == (blockSize > VocalSynthEditor::largeBufferSamples));

    bool bufferShown = false, dspShown = false;
    for (const auto& text : texts)
    {
        bufferShown = bufferShown || text.contains ("Buffer " + juce::String (blockSize) + " smp");
        dspShown = dspShown || (text.startsWith ("VocalSynth DSP: avg") && text.contains ("% of the block time"));
    }

    CHECK (bufferShown);
    CHECK (dspShown);

    const auto image = editor->createComponentSnapshot (editor->getLocalBounds());
    CHECK (image.getWidth() == VocalSynthEditor::defaultWidth);

    if (const auto dir = juce::SystemStats::getEnvironmentVariable ("VOCALSYNTH_EDITOR_SNAPSHOT_DIR", {}); dir.isNotEmpty())
    {
        const auto file = juce::File (dir).getChildFile (juce::String (variant == Variant::fx ? "editor-fx" : "editor-instrument")
                                                         + "-voice-buffer" + juce::String (blockSize) + ".png");
        file.deleteFile();
        juce::FileOutputStream out (file);
        juce::PNGImageFormat().writeImageToStream (image, out);
    }
}

TEST_CASE ("DSP load text", "[editor]")
{
    CHECK (VocalSynthEditor::formatDspLoad ({}) == "VocalSynth DSP: - (no audio yet)");

    DspLoadMeter::Reading r;
    r.averageProportion = 0.021f;
    r.peakProportion = 0.053f;
    r.valid = true;
    CHECK (VocalSynthEditor::formatDspLoad (r) == "VocalSynth DSP: avg 2.1 % / peak 5.3 % of the block time");
}

TEST_CASE ("Editor tuner: an Octave change does not flash the note one octave off", "[editor][tuner]")
{
    VocalSynthProcessor processor (Variant::fx);
    processor.prepareToPlay (48000.0, 2048);
    juce::AudioBuffer<float> buffer (2, 2048);
    juce::MidiBuffer midi;
    int64_t pos = 0;

    const auto processSung = [&] (int blocks)
    {
        for (int b = 0; b < blocks; ++b, pos += 2048)
        {
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 2048; ++i)
                    buffer.setSample (ch, i, 0.3f * (float) std::sin (juce::MathConstants<double>::twoPi * 220.0
                                                                      * (double) (pos + i) / 48000.0));
            midi.clear();
            processor.processBlock (buffer, midi);
        }
    };

    processSung (10); // A3, MIDI 57
    REQUIRE (processor.getVoiceSnapshot().activeNote == 57);

    std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());
    auto* vsEditor = dynamic_cast<VocalSynthEditor*> (editor.get());
    REQUIRE (vsEditor != nullptr);
    vsEditor->runTimerTickForTests();
    REQUIRE (vsEditor->getTunerState().note == 57);

    // Octave +1: before the next block the snapshot still holds the old note; the tuner keeps A3
    auto* octave = processor.getParameters().getParameter (params::octaveShift.getParamID());
    REQUIRE (octave != nullptr);
    octave->setValueNotifyingHost (octave->convertTo0to1 (1.0f));
    vsEditor->runTimerTickForTests();
    CHECK (vsEditor->getTunerState().note == 57);

    processSung (2);
    REQUIRE (processor.getVoiceSnapshot().activeNote == 69);
    CHECK (processor.getVoiceSnapshot().quantisedNote == 57);
    vsEditor->runTimerTickForTests();
    CHECK (vsEditor->getTunerState().note == 57);
}

TEST_CASE ("Editor Voice page meter repaints when only the peak bar moves", "[editor]")
{
    // v0.2.1 review: the meter key was taken after the peak update, so a moving peak bar alone
    // (Source = Test Pattern, or a steady RMS) never repainted
    VocalSynthProcessor processor (Variant::fx);
    processor.prepareToPlay (48000.0, 512);

    if (auto* source = processor.getParameters().getParameter (params::source.getParamID()))
        source->setValueNotifyingHost (source->convertTo0to1 ((float) params::Source::testPattern));

    std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());
    auto* vsEditor = dynamic_cast<VocalSynthEditor*> (editor.get());
    REQUIRE (vsEditor != nullptr);
    vsEditor->runTimerTickForTests();
    const auto before = vsEditor->getVoiceMeterRepaintCount();

    juce::AudioBuffer<float> buffer (2, 512);
    juce::MidiBuffer midi;
    buffer.clear();
    buffer.setSample (0, 100, 0.5f); // a peak at -6 dBFS
    buffer.setSample (1, 100, 0.5f);
    processor.processBlock (buffer, midi);
    vsEditor->runTimerTickForTests();
    CHECK (vsEditor->getVoiceMeterRepaintCount() == before + 1);

    // The peak bar decays over the next ticks: still repainted
    buffer.clear();
    processor.processBlock (buffer, midi);
    vsEditor->runTimerTickForTests();
    CHECK (vsEditor->getVoiceMeterRepaintCount() == before + 2);
}

TEST_CASE ("Editor tuner names the note being sent", "[editor][tuner]")
{
    VocalSynthProcessor processor (Variant::fx);
    processor.prepareToPlay (48000.0, 256);

    // 0.4 s of a sung A3 (220 Hz = FL "A4", MIDI 57), still sounding when the editor opens
    juce::AudioBuffer<float> buffer (2, 256);
    juce::MidiBuffer midi;
    for (int pos = 0; pos < 48000 * 4 / 10; pos += 256)
    {
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < 256; ++i)
                buffer.setSample (ch, i, 0.3f * (float) std::sin (juce::MathConstants<double>::twoPi * 220.0 * (pos + i) / 48000.0));
        midi.clear();
        processor.processBlock (buffer, midi);
    }

    REQUIRE (processor.getVoiceSnapshot().activeNote == 57);

    std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());
    auto* vsEditor = dynamic_cast<VocalSynthEditor*> (editor.get());
    REQUIRE (vsEditor != nullptr);

    const auto& tuner = vsEditor->getTunerState();
    CHECK (tuner.note == 57);
    CHECK (tuner.noteIsSegmented);
    CHECK (tuner.displayHz == Catch::Approx (220.0f).margin (2.0f));
    CHECK (std::abs (tuner.needleCents) < 10.0f);
    CHECK (tuner.ledOn);

    const auto image = editor->createComponentSnapshot (editor->getLocalBounds());
    CHECK (image.getWidth() == VocalSynthEditor::defaultWidth);

    if (const auto dir = juce::SystemStats::getEnvironmentVariable ("VOCALSYNTH_EDITOR_SNAPSHOT_DIR", {}); dir.isNotEmpty())
    {
        const auto file = juce::File (dir).getChildFile ("editor-fx-voice-note.png");
        file.deleteFile();
        juce::FileOutputStream out (file);
        juce::PNGImageFormat().writeImageToStream (image, out);
    }
}
